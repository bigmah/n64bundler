// SPDX-License-Identifier: GPL-3.0-or-later
// A headless mupen64plus frontend that exists only to answer "what does the
// console do here". No video, no audio, no input: the core runs the game with
// its dummy plugins and this dumps RDRAM at whatever moments are asked for.
#define M64P_CORE_PROTOTYPES 1
#include <mupen64plus/m64p_types.h>
#include <mupen64plus/m64p_frontend.h>
#include <mupen64plus/m64p_config.h>
#include <mupen64plus/m64p_debugger.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>

static const char *out_prefix;
static double *at_seconds;
static int at_count;


// Where the three things this needs live. Set them in the environment rather
// than hard-coding a Homebrew prefix: the core has to be one built with
// DEBUGGER=1, which no package ships.
static const char *env_or(const char *name, const char *fallback) {
    const char *value = getenv(name);
    return (value != NULL && *value != '\0') ? value : fallback;
}
static const char *core_path(void) {
    return env_or("M64P_CORE", "./libmupen64plus.dylib");
}
static const char *rsp_path(void) {
    return env_or("M64P_RSP", "/opt/homebrew/lib/mupen64plus/mupen64plus-rsp-hle.dylib");
}
static const char *data_path(void) {
    return env_or("M64P_DATA", "/opt/homebrew/share/mupen64plus");
}

static void debug_cb(void *ctx, int level, const char *message) {
    (void)ctx;
    if (level <= 3) fprintf(stderr, "[core %d] %s\n", level, message);
}

static void state_cb(void *ctx, m64p_core_param param, int value) {
    (void)ctx;
    if (param == M64CORE_EMU_STATE) fprintf(stderr, "[state] emu %d\n", value);
}

static void dump(const char *path) {
    void *rdram = DebugMemGetPointer(M64P_DBG_PTR_RDRAM);
    if (rdram == NULL) { fprintf(stderr, "no rdram pointer\n"); return; }
    FILE *f = fopen(path, "wb");
    if (f == NULL) { perror("fopen"); return; }
    fwrite(rdram, 1, 0x800000, f);
    fclose(f);
    fprintf(stderr, "wrote %s\n", path);
}

static void *timer_thread(void *arg) {
    (void)arg;
    double waited = 0.0;
    for (int i = 0; i < at_count; i++) {
        double delta = at_seconds[i] - waited;
        if (delta > 0.0) usleep((useconds_t)(delta * 1000000.0));
        waited = at_seconds[i];
        char path[512];
        snprintf(path, sizeof(path), "%s.%.0fs.bin", out_prefix, at_seconds[i]);
        fprintf(stderr, "[dump] at %.1fs\n", at_seconds[i]);
        dump(path);
    }
    CoreDoCommand(M64CMD_STOP, 0, NULL);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: refrun <rom> <out-prefix> <seconds>...\n"); return 2; }
    const char *rom_path = argv[1];
    out_prefix = argv[2];
    at_count = argc - 3;
    at_seconds = calloc(at_count, sizeof(double));
    for (int i = 0; i < at_count; i++) at_seconds[i] = atof(argv[3 + i]);

    if (CoreStartup(0x020102, NULL, data_path(), NULL, debug_cb,
                    NULL, state_cb) != M64ERR_SUCCESS) {
        fprintf(stderr, "CoreStartup failed\n"); return 1;
    }

    m64p_handle section;
    if (ConfigOpenSection("Core", &section) == M64ERR_SUCCESS) {
        int emumode = 1; // cached interpreter: slower than the dynarec, and it
                         // is the same machine on every host.
        ConfigSetParameter(section, "R4300Emulator", M64TYPE_INT, &emumode);
        int no_extra = 0; // Banjo-Tooie needs the Expansion Pak.
        ConfigSetParameter(section, "DisableExtraMem", M64TYPE_BOOL, &no_extra);
        int no_osd = 0; // the on-screen display talks to a GL context nothing made
        ConfigSetParameter(section, "OnScreenDisplay", M64TYPE_BOOL, &no_osd);
    }

    FILE *f = fopen(rom_path, "rb");
    if (f == NULL) { perror("rom"); return 1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *rom = malloc(size);
    if (fread(rom, 1, size, f) != (size_t)size) { fprintf(stderr, "short read\n"); return 1; }
    fclose(f);

    // The RSP is the one plugin that has to be real. Video, audio and input
    // can stay the core's own stubs -- nothing here looks at a picture -- but
    // a game whose display and audio lists are never accepted never gets its
    // task-done interrupt, and waits for it forever.
    void *rsp = dlopen(rsp_path(), RTLD_NOW);
    if (rsp == NULL) { fprintf(stderr, "rsp: %s\n", dlerror()); return 1; }
    m64p_error (*rsp_startup)(m64p_dynlib_handle, void *, void (*)(void *, int, const char *)) =
        dlsym(rsp, "PluginStartup");
    void *core = dlopen(core_path(), RTLD_NOW);
    if (rsp_startup(core, NULL, debug_cb) != M64ERR_SUCCESS) {
        fprintf(stderr, "rsp startup failed\n"); return 1;
    }

    if (CoreDoCommand(M64CMD_ROM_OPEN, (int)size, rom) != M64ERR_SUCCESS) {
        fprintf(stderr, "ROM_OPEN failed\n"); return 1;
    }
    free(rom);

    if (CoreAttachPlugin(M64PLUGIN_RSP, rsp) != M64ERR_SUCCESS) {
        fprintf(stderr, "rsp attach failed\n"); return 1;
    }

    pthread_t t;
    pthread_create(&t, NULL, timer_thread, NULL);
    m64p_error err = CoreDoCommand(M64CMD_EXECUTE, 0, NULL);
    fprintf(stderr, "execute returned %d\n", err);
    pthread_join(t, NULL);
    CoreDoCommand(M64CMD_ROM_CLOSE, 0, NULL);
    CoreShutdown();
    return 0;
}
