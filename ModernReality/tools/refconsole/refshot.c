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
// How many frames the *game* has finished, as against how many the video
// interface has scanned out.
//
// A vertical interrupt happens sixty times a second whatever the game is
// doing; a game that cannot build a frame in a sixtieth of a second simply
// shows the previous one again. So the interesting rate -- the one that says
// whether a runtime is keeping up, and the one `n64b-run --developer` reports
// as display lists a second -- is how often the game hands the video interface
// a different framebuffer to scan out. That is VI_ORIGIN changing, and the
// debugger hands the registers over.
static uint32_t last_origin;
static uint32_t last_status;

/// Frames the game has drawn so far, which is what a scripted press is counted
/// in. `M64CMD_ADVANCE_FRAME` advances one of these: mupen64plus counts a frame
/// in `new_frame()`, which its RSP calls once per graphics task, so this is one
/// display list -- exactly what `n64b-run` counts for `N64B_SCREENSHOT_AFTER`
/// and `N64B_INPUT`. Reads of the controller are not that clock: a game drawing
/// twenty frames a second reads the pad on each of sixty video interrupts.
static volatile unsigned long current_frame;

/// The video interface's own registers, which say what is being scanned out
/// and how.
///
/// VI_STATUS carries the pixel format, the anti-aliasing mode and whether the
/// gamma is on, and is worth reporting because it is directly comparable: a
/// runtime that models the video interface right prints the same word here as
/// the console does.
///
/// VI_ORIGIN is worth *not* drawing a conclusion from. It alternates between
/// the game's two framebuffers on every retrace whatever the frame rate is,
/// because libultra keeps two `OSViContext`s and `__osViSwapContext` exchanges
/// them each time -- so counting how often it changes measures that exchange
/// and not the game. The game's frame rate is the frame count above, which
/// mupen64plus advances once per graphics task.
static void sample_vi(void) {
    const uint32_t *vi = DebugMemGetPointer(M64P_DBG_PTR_VI_REG);
    if (vi == NULL) return;
    last_origin = vi[1];
    last_status = vi[0];
    if (getenv("REFSHOT_TRACE_VI") != NULL) {
        static unsigned long n;
        fprintf(stderr, "[vi] %lu origin 0x%08X status 0x%08X\n", n++, last_origin, last_status);
    }
}

static void advance_one_frame(void) {
    pthread_mutex_lock(&state_lock);
    emu_state = M64EMU_RUNNING;
    pthread_mutex_unlock(&state_lock);
    CoreDoCommand(M64CMD_ADVANCE_FRAME, 0, NULL);
    wait_for_state(M64EMU_PAUSED);
    current_frame++;
    sample_vi();
}

// The pad the game sees, driven by N64B_INPUT in exactly the spelling
// n64b-run's own scripted input uses, so one script drives both consoles.
//
//   <frame>:<buttons>  with buttons joined by `+`, and an empty list a release.
//
// Frames are the frames the game has drawn -- one display list -- which is what
// the runtime counts too, and what a screenshot here is numbered by. Both
// consoles have to be counted in the same thing or a script means two different
// moments.
struct scripted_frame {
    unsigned long at;
    unsigned short buttons;
    int x, y;
};
static struct scripted_frame *script;
static int script_count;
static unsigned long pad_reads;

/// The bit a button name sets in mupen64plus's own `BUTTONS`, or -1.
///
/// Written through the union's named fields rather than as numbers, because
/// the numbers are not the console's. `BUTTONS.Value` is a bitfield the
/// compiler packs from the bottom -- R_DPAD is bit 0 and A_BUTTON is bit 7 --
/// where the N64's own controller halfword has A at bit 15 and Start at 12.
/// Written out by hand in the console's order, as they were, every button in
/// this file was a different button: `start` pressed the R trigger, `a` and
/// `b` set the two reserved bits and did nothing at all, and the reference
/// console sat in its attract mode through every script this tool played it,
/// which read as a console that ignores the pad.
static int mask_for(const char *name) {
    BUTTONS b;
    b.Value = 0;
    if      (strcmp(name, "a") == 0)     b.A_BUTTON = 1;
    else if (strcmp(name, "b") == 0)     b.B_BUTTON = 1;
    else if (strcmp(name, "z") == 0)     b.Z_TRIG = 1;
    else if (strcmp(name, "start") == 0) b.START_BUTTON = 1;
    else if (strcmp(name, "l") == 0)     b.L_TRIG = 1;
    else if (strcmp(name, "r") == 0)     b.R_TRIG = 1;
    else if (strcmp(name, "du") == 0)    b.U_DPAD = 1;
    else if (strcmp(name, "dd") == 0)    b.D_DPAD = 1;
    else if (strcmp(name, "dl") == 0)    b.L_DPAD = 1;
    else if (strcmp(name, "dr") == 0)    b.R_DPAD = 1;
    else if (strcmp(name, "cu") == 0)    b.U_CBUTTON = 1;
    else if (strcmp(name, "cd") == 0)    b.D_CBUTTON = 1;
    else if (strcmp(name, "cl") == 0)    b.L_CBUTTON = 1;
    else if (strcmp(name, "cr") == 0)    b.R_CBUTTON = 1;
    else return -1;
    return (int)b.Value;
}

static void parse_script(const char *spec) {
    if (spec == NULL || *spec == '\0') return;
    int commas = 1;
    for (const char *p = spec; *p; p++) if (*p == ',') commas++;
    script = calloc(commas, sizeof(*script));
    char *copy = strdup(spec);
    // strtok_r, and two save pointers, because the inner walk over a frame's
    // buttons runs inside the outer walk over the frames. One strtok has one
    // piece of state, so the first `a+b` would end the outer loop as well --
    // which is a script of one entry, silently, and two consoles doing
    // different things.
    char *entries = NULL, *names = NULL;
    for (char *entry = strtok_r(copy, ",", &entries); entry != NULL;
         entry = strtok_r(NULL, ",", &entries)) {
        char *colon = strchr(entry, ':');
        if (colon == NULL) continue;
        *colon = '\0';
        struct scripted_frame frame = { strtoul(entry, NULL, 10), 0, 0, 0 };
        for (char *name = strtok_r(colon + 1, "+", &names); name != NULL;
             name = strtok_r(NULL, "+", &names)) {
            int known = 0;
            const int bit = mask_for(name);
            if (bit >= 0) { frame.buttons |= (unsigned short)bit; known = 1; }
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
    pad_reads++;
    unsigned long now = current_frame;
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
        // Both counts, because they are different clocks and a script written
        // against one cannot be read against the other. `frame` is a vertical
        // interrupt, sixty a second on both consoles; `pad` is a read of
        // controller one, which happens once per frame the *game* runs. Their
        // ratio is the game's own frame rate, and it is the number to compare
        // against `n64b-run`'s display lists per second.
        fprintf(stderr, "[shot] frame %lu, pad reads %lu, vi origin 0x%08X status 0x%08X\n",
                frame, pad_reads, last_origin, last_status);
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
