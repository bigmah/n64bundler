// SPDX-License-Identifier: GPL-3.0-or-later
// A mupen64plus frontend that answers "how fast does the game actually run".
//
// refshot answers what the console draws and refdbg answers what it computes.
// Neither answers *when*, and when is its own class of bug: a runtime that
// draws every frame correctly and draws them too often is a game played at the
// wrong speed, which is the one thing a frame-numbered comparison cannot see.
// Two pictures numbered 1300 are the same moment of the game however many
// seconds each console took to get there.
//
// So this one runs the console flat out, with a real video plugin and the
// core's own speed limiter, and reports two rates once a second:
//
//   - **frames**, which the game drew. `M64CMD_SET_FRAME_CALLBACK` is called
//     from mupen64plus's `new_frame()`, which its RSP calls once per graphics
//     task, so this counts display lists -- the same clock `n64b-run
//     --developer` reports as "display lists so far".
//   - **pad reads**, which the game made. Banjo-Tooie polls the controller
//     once per vertical interrupt, so this should sit near sixty whatever the
//     game is doing, and is how you tell a console running at console speed
//     from one the limiter has let loose. A frame rate means nothing without
//     it: half the frames in half the seconds is the same number.
//
// Frames per second is the number a runtime has to match. A game that paces
// itself against the display processor runs at whatever rate the processor can
// feed it, and a runtime whose processor is instant runs the game faster than
// the game was ever meant to go -- everything in it moving too quickly, with
// every frame of it drawn correctly.
#define M64P_CORE_PROTOTYPES 1
#include <mupen64plus/m64p_types.h>
#include <mupen64plus/m64p_frontend.h>
#include <mupen64plus/m64p_config.h>
#include <mupen64plus/m64p_plugin.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <time.h>

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
static void state_cb(void *ctx, m64p_core_param param, int value) {
    (void)ctx; (void)param; (void)value;
}

/// Frames the game has drawn, and reads it has made of the pad. Both are
/// written from the core's thread and read from the reporting one, which is a
/// race that does not matter: a count that is one behind for a microsecond
/// still says twenty against thirty.
static volatile unsigned long current_frame;
static volatile unsigned long pad_reads;

static void frame_reached(unsigned int index) {
    current_frame = index;
}

// The pad, driven by N64B_INPUT in the spelling n64b-run and refshot both use,
// so one script drives every console this project measures.
struct scripted_frame {
    unsigned long at;
    unsigned short buttons;
    int x, y;
};
static struct scripted_frame *script;
static int script_count;

/// The bit a button name sets in mupen64plus's own `BUTTONS`, or -1. Written
/// through the union's named fields because the numbers are not the console's:
/// `BUTTONS.Value` is a bitfield packed from the bottom, and the N64's own
/// controller halfword is packed from the top.
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
    // Two save pointers, because the walk over a frame's buttons runs inside
    // the walk over the frames and one strtok has one piece of state.
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
            if (!known) fprintf(stderr, "refrate: no button called \"%s\"\n", name);
        }
        script[script_count++] = frame;
    }
    free(copy);
    fprintf(stderr, "refrate: %d scripted inputs\n", script_count);
}

// The input plugin is this file rather than a shared library: the core takes
// plugin entry points through dlsym on a handle, and RTLD_DEFAULT is a handle
// whose symbols are ours.
EXPORT m64p_error CALL PluginGetVersion(m64p_plugin_type *type, int *version, int *api,
                                        const char **name, int *caps) {
    if (type) *type = M64PLUGIN_INPUT;
    if (version) *version = 0x010000;
    if (api) *api = 0x020100;
    if (name) *name = "refrate scripted pad";
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

static double run_seconds = 120.0;

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/// One line a second for as long as was asked for, and then stop the core.
static void *report_thread(void *arg) {
    (void)arg;
    CoreDoCommand(M64CMD_SET_FRAME_CALLBACK, 0, (void *)frame_reached);
    const double started = now_seconds();
    unsigned long last_frame = 0, last_pad = 0;
    double next = started + 1.0;
    while (now_seconds() - started < run_seconds) {
        struct timespec nap = { 0, 20 * 1000 * 1000 };
        nanosleep(&nap, NULL);
        const double t = now_seconds();
        if (t < next) continue;
        next += 1.0;
        const unsigned long frame = current_frame, pad = pad_reads;
        printf("%6.1fs  %6lu frames (%2lu this second)  %6lu pad reads (%3lu this second)\n",
               t - started, frame, frame - last_frame, pad, pad - last_pad);
        fflush(stdout);
        last_frame = frame;
        last_pad = pad;
    }
    CoreDoCommand(M64CMD_STOP, 0, NULL);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
                "usage: refrate <rom> [seconds]\n"
                "  Reports the game's own frame rate, once a second, with the\n"
                "  console running at console speed. N64B_INPUT drives the pad.\n");
        return 2;
    }
    const char *rom_path = argv[1];
    if (argc > 2) run_seconds = atof(argv[2]);
    parse_script(getenv("N64B_INPUT"));

    if (CoreStartup(0x020102, NULL, data_path(), NULL, debug_cb, NULL, state_cb) != M64ERR_SUCCESS) {
        fprintf(stderr, "CoreStartup failed\n");
        return 1;
    }

    m64p_handle section;
    if (ConfigOpenSection("Core", &section) == M64ERR_SUCCESS) {
        // The cached interpreter by default, so the machine is the same
        // everywhere. `REFRATE_R4300` picks another: 0 pure interpreter, 2 the
        // dynamic recompiler. The emulated machine's timing is not supposed to
        // depend on which -- the core charges the same cycles per instruction
        // either way -- so running two of them and getting one answer is how
        // you tell a frame rate that belongs to the console from one that
        // belongs to a frontend that could not keep up.
        int emumode = 1;
        const char *pick = getenv("REFRATE_R4300");
        if (pick != NULL && *pick != '\0') {
            emumode = atoi(pick);
        }
        ConfigSetParameter(section, "R4300Emulator", M64TYPE_INT, &emumode);
        int no_extra = 0;  // Banjo-Tooie needs the Expansion Pak
        ConfigSetParameter(section, "DisableExtraMem", M64TYPE_BOOL, &no_extra);
        int no_osd = 0;
        ConfigSetParameter(section, "OnScreenDisplay", M64TYPE_BOOL, &no_osd);
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
    pthread_create(&t, NULL, report_thread, NULL);
    m64p_error err = CoreDoCommand(M64CMD_EXECUTE, 0, NULL);
    fprintf(stderr, "execute returned %d\n", err);
    pthread_join(t, NULL);
    CoreDoCommand(M64CMD_ROM_CLOSE, 0, NULL);
    CoreShutdown();
    return 0;
}
