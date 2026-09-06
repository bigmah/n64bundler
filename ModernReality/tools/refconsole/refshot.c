// SPDX-License-Identifier: GPL-3.0-or-later
// A mupen64plus frontend that exists only to answer "what does the console
// draw here". refrun answers what a game computes; this answers what it looks
// like, which is the question a renderer bug asks.
//
// The core runs the cartridge with a real video plugin and takes a screenshot
// at each of the moments named on the command line. A pad script may be given
// too, because a game past its title screen has to be driven there.
#define M64P_CORE_PROTOTYPES 1
#include <mupen64plus/m64p_types.h>
#include <mupen64plus/m64p_frontend.h>
#include <mupen64plus/m64p_config.h>
#include <mupen64plus/m64p_plugin.h>
#include <mupen64plus/m64p_debugger.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>

static double *at_seconds;
static int at_count;

static const char *env_or(const char *name, const char *fallback) {
    const char *value = getenv(name);
    return (value != NULL && *value != '\0') ? value : fallback;
}
static const char *core_path(void) {
    return env_or("M64P_CORE", "/opt/homebrew/lib/libmupen64plus.dylib");
}
static const char *rsp_path(void) {
    return env_or("M64P_RSP", "/opt/homebrew/lib/mupen64plus/mupen64plus-rsp-hle.dylib");
}
static const char *gfx_path(void) {
    return env_or("M64P_GFX", "/opt/homebrew/lib/mupen64plus/mupen64plus-video-glide64mk2.dylib");
}
static const char *data_path(void) {
    return env_or("M64P_DATA", "/opt/homebrew/share/mupen64plus");
}

static void debug_cb(void *ctx, int level, const char *message) {
    (void)ctx;
    if (level <= 2) fprintf(stderr, "[core %d] %s\n", level, message);
}

// The emulator's own state, which is how a frame advance is waited for: the
// core resumes, runs exactly one frame and pauses again, and the only report
// of that is this callback.
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t state_changed = PTHREAD_COND_INITIALIZER;
static int emu_state = 0;

static void state_cb(void *ctx, m64p_core_param param, int value) {
    (void)ctx;
    if (param != M64CORE_EMU_STATE) return;
    pthread_mutex_lock(&state_lock);
    emu_state = value;
    pthread_cond_broadcast(&state_changed);
    pthread_mutex_unlock(&state_lock);
}

static void wait_for_state(int wanted) {
    pthread_mutex_lock(&state_lock);
    while (emu_state != wanted) pthread_cond_wait(&state_changed, &state_lock);
    pthread_mutex_unlock(&state_lock);
}

/// The console's eight megabytes, for the frames a comparison wants both a
/// picture and a state for. Needs a core built with its debugger, which is the
/// same core refrun and refdbg need.
static const char *ram_prefix;

static void write_memory_image(unsigned long frame) {
    if (ram_prefix == NULL) return;
    void *rdram = DebugMemGetPointer(M64P_DBG_PTR_RDRAM);
    if (rdram == NULL) { fprintf(stderr, "no rdram pointer; is this core built with DEBUGGER=1?\n"); return; }
    char path[512];
    snprintf(path, sizeof(path), "%s.%lu.bin", ram_prefix, frame);
    FILE *f = fopen(path, "wb");
    if (f == NULL) { perror("fopen"); return; }
    fwrite(rdram, 1, 0x800000, f);
    fclose(f);
    fprintf(stderr, "wrote %s\n", path);
}

/// One vertical interrupt, and not a fraction more.
static void advance_one_frame(void) {
    pthread_mutex_lock(&state_lock);
    emu_state = M64EMU_RUNNING;
    pthread_mutex_unlock(&state_lock);
    CoreDoCommand(M64CMD_ADVANCE_FRAME, 0, NULL);
    wait_for_state(M64EMU_PAUSED);
}

// The pad the game sees, driven by N64B_INPUT in exactly the spelling
// n64b-run's own scripted input uses, so one script drives both consoles.
//
//   <frame>:<buttons>  with buttons joined by `+`, and an empty list a release.
//
// Frames are counted in reads of controller one, which is what the runtime
// counts too.
struct scripted_frame {
    unsigned long at;
    unsigned short buttons;
    int x, y;
};
static struct scripted_frame *script;
static int script_count;
static unsigned long pad_reads;

static const struct { const char *name; unsigned short bit; } kButtons[] = {
    {"a", 0x8000}, {"b", 0x4000}, {"z", 0x2000}, {"start", 0x1000},
    {"du", 0x0800}, {"dd", 0x0400}, {"dl", 0x0200}, {"dr", 0x0100},
    {"l", 0x0020}, {"r", 0x0010},
    {"cu", 0x0008}, {"cd", 0x0004}, {"cl", 0x0002}, {"cr", 0x0001},
};

static void parse_script(const char *spec) {
    if (spec == NULL || *spec == '\0') return;
    int commas = 1;
    for (const char *p = spec; *p; p++) if (*p == ',') commas++;
    script = calloc(commas, sizeof(*script));
    char *copy = strdup(spec);
    for (char *entry = strtok(copy, ","); entry != NULL; entry = strtok(NULL, ",")) {
        char *colon = strchr(entry, ':');
        if (colon == NULL) continue;
        *colon = '\0';
        struct scripted_frame frame = { strtoul(entry, NULL, 10), 0, 0, 0 };
        for (char *name = strtok(colon + 1, "+"); name != NULL; name = strtok(NULL, "+")) {
            int known = 0;
            for (size_t i = 0; i < sizeof(kButtons) / sizeof(kButtons[0]); i++) {
                if (strcmp(name, kButtons[i].name) == 0) { frame.buttons |= kButtons[i].bit; known = 1; }
            }
            if (strcmp(name, "up") == 0)    { frame.y =  80; known = 1; }
            if (strcmp(name, "down") == 0)  { frame.y = -80; known = 1; }
            if (strcmp(name, "left") == 0)  { frame.x = -80; known = 1; }
            if (strcmp(name, "right") == 0) { frame.x =  80; known = 1; }
            if (!known) fprintf(stderr, "refshot: no button called \"%s\"\n", name);
        }
        script[script_count++] = frame;
    }
    free(copy);
    fprintf(stderr, "refshot: %d scripted inputs\n", script_count);
}

// The input plugin, which is this file rather than a shared library: the core
// takes plugin entry points through dlsym on a handle, and RTLD_DEFAULT is a
// handle whose symbols are ours.
EXPORT m64p_error CALL PluginGetVersion(m64p_plugin_type *type, int *version, int *api,
                                        const char **name, int *caps) {
    if (type) *type = M64PLUGIN_INPUT;
    if (version) *version = 0x010000;
    if (api) *api = 0x020100;
    if (name) *name = "refshot scripted pad";
    if (caps) *caps = 0;
    return M64ERR_SUCCESS;
}
EXPORT m64p_error CALL PluginStartup(m64p_dynlib_handle core, void *ctx,
                                     void (*log)(void *, int, const char *)) {
    (void)core; (void)ctx; (void)log;
    return M64ERR_SUCCESS;
}
EXPORT m64p_error CALL PluginShutdown(void) { return M64ERR_SUCCESS; }
EXPORT void CALL InitiateControllers(CONTROL_INFO info) {
    info.Controls[0].Present = 1;
    info.Controls[0].Plugin = PLUGIN_NONE;
    for (int i = 1; i < 4; i++) info.Controls[i].Present = 0;
}
EXPORT void CALL GetKeys(int controller, BUTTONS *keys) {
    keys->Value = 0;
    if (controller != 0) return;
    unsigned long now = pad_reads++;
    const struct scripted_frame *current = NULL;
    for (int i = 0; i < script_count; i++) {
        if (script[i].at > now) break;
        current = &script[i];
    }
    if (current == NULL) return;
    keys->Value = current->buttons;
    keys->X_AXIS = current->x;
    keys->Y_AXIS = current->y;
}
EXPORT void CALL ControllerCommand(int c, unsigned char *cmd) { (void)c; (void)cmd; }
EXPORT void CALL ReadController(int c, unsigned char *cmd) { (void)c; (void)cmd; }
EXPORT void CALL RomOpen(void) {}
EXPORT void CALL RomClosed(void) {}
EXPORT void CALL SDL_KeyDown(int mod, int sym) { (void)mod; (void)sym; }
EXPORT void CALL SDL_KeyUp(int mod, int sym) { (void)mod; (void)sym; }
EXPORT void CALL RenderCallback(void) {}

// Screenshots at exact frame counts, so that a picture from here and a picture
// from n64b-run are the same moment of the same game rather than two moments
// that happened to take the same number of seconds. Wall-clock cannot do this:
// one console runs a cached interpreter and the other runs compiled code.
static void *frame_thread(void *arg) {
    (void)arg;
    wait_for_state(M64EMU_RUNNING);
    CoreDoCommand(M64CMD_PAUSE, 0, NULL);
    wait_for_state(M64EMU_PAUSED);

    unsigned long frame = 0;
    for (int i = 0; i < at_count; i++) {
        const unsigned long wanted = (unsigned long)at_seconds[i];
        while (frame < wanted) { advance_one_frame(); frame++; }
        fprintf(stderr, "[shot] frame %lu\n", frame);
        CoreDoCommand(M64CMD_TAKE_NEXT_SCREENSHOT, 0, NULL);
        write_memory_image(frame);
        // The core writes the file when the frame it is on is finished, so
        // one more frame has to go by before the next request.
        advance_one_frame(); frame++;
    }
    CoreDoCommand(M64CMD_STOP, 0, NULL);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
                "usage: refshot <rom> <screenshot-dir> <frame>...\n"
                "  N64B_INPUT drives the pad, in n64b-run's own spelling.\n");
        return 2;
    }
    const char *rom_path = argv[1];
    const char *shot_dir = argv[2];
    at_count = argc - 3;
    at_seconds = calloc(at_count > 0 ? at_count : 1, sizeof(double));
    for (int i = 0; i < at_count; i++) at_seconds[i] = atof(argv[3 + i]);
    parse_script(getenv("N64B_INPUT"));
    ram_prefix = getenv("REFSHOT_RAM");

    if (CoreStartup(0x020102, NULL, data_path(), NULL, debug_cb, NULL, state_cb) != M64ERR_SUCCESS) {
        fprintf(stderr, "CoreStartup failed\n");
        return 1;
    }

    m64p_handle section;
    if (ConfigOpenSection("Core", &section) == M64ERR_SUCCESS) {
        int emumode = 1;   // the cached interpreter, so the machine is the same everywhere
        ConfigSetParameter(section, "R4300Emulator", M64TYPE_INT, &emumode);
        int no_extra = 0;  // Banjo-Tooie needs the Expansion Pak
        ConfigSetParameter(section, "DisableExtraMem", M64TYPE_BOOL, &no_extra);
        int no_osd = 0;
        ConfigSetParameter(section, "OnScreenDisplay", M64TYPE_BOOL, &no_osd);
        ConfigSetParameter(section, "ScreenshotPath", M64TYPE_STRING, (void *)shot_dir);
    }
    if (ConfigOpenSection("Video-General", &section) == M64ERR_SUCCESS) {
        int w = 640, h = 480, off = 0;
        ConfigSetParameter(section, "ScreenWidth", M64TYPE_INT, &w);
        ConfigSetParameter(section, "ScreenHeight", M64TYPE_INT, &h);
        ConfigSetParameter(section, "Fullscreen", M64TYPE_BOOL, &off);
    }

    FILE *f = fopen(rom_path, "rb");
    if (f == NULL) { perror("rom"); return 1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *rom = malloc(size);
    if (fread(rom, 1, size, f) != (size_t)size) { fprintf(stderr, "short read\n"); return 1; }
    fclose(f);

    void *core = dlopen(core_path(), RTLD_NOW);
    void *rsp = dlopen(rsp_path(), RTLD_NOW);
    void *gfx = dlopen(gfx_path(), RTLD_NOW);
    if (rsp == NULL || gfx == NULL) { fprintf(stderr, "plugin: %s\n", dlerror()); return 1; }
    m64p_error (*plugin_startup)(m64p_dynlib_handle, void *, void (*)(void *, int, const char *));
    plugin_startup = dlsym(rsp, "PluginStartup");
    if (plugin_startup(core, NULL, debug_cb) != M64ERR_SUCCESS) {
        fprintf(stderr, "rsp startup failed\n"); return 1;
    }
    plugin_startup = dlsym(gfx, "PluginStartup");
    if (plugin_startup(core, NULL, debug_cb) != M64ERR_SUCCESS) {
        fprintf(stderr, "gfx startup failed\n"); return 1;
    }

    if (CoreDoCommand(M64CMD_ROM_OPEN, (int)size, rom) != M64ERR_SUCCESS) {
        fprintf(stderr, "ROM_OPEN failed\n"); return 1;
    }
    free(rom);

    if (CoreAttachPlugin(M64PLUGIN_GFX, gfx) != M64ERR_SUCCESS) {
        fprintf(stderr, "gfx attach failed\n"); return 1;
    }
    if (CoreAttachPlugin(M64PLUGIN_INPUT, RTLD_DEFAULT) != M64ERR_SUCCESS) {
        fprintf(stderr, "input attach failed\n"); return 1;
    }
    if (CoreAttachPlugin(M64PLUGIN_RSP, rsp) != M64ERR_SUCCESS) {
        fprintf(stderr, "rsp attach failed\n"); return 1;
    }

    pthread_t t;
    pthread_create(&t, NULL, frame_thread, NULL);
    m64p_error err = CoreDoCommand(M64CMD_EXECUTE, 0, NULL);
    fprintf(stderr, "execute returned %d\n", err);
    pthread_join(t, NULL);
    CoreDoCommand(M64CMD_ROM_CLOSE, 0, NULL);
    CoreShutdown();
    return 0;
}
