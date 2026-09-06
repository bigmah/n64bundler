// SPDX-License-Identifier: GPL-3.0-or-later
// The reference console, with breakpoints.
//
// Same headless core as refrun, plus mupen64plus's debugger: -e <addr> stops
// when the game executes an address and prints who called it with what, -w
// <addr> stops when the game writes one. That is the one question a static
// recompilation cannot answer about itself -- not "what does my run do" but
// "what does a console do here instead".
#define M64P_CORE_PROTOTYPES 1
#include <mupen64plus/m64p_types.h>
#include <mupen64plus/m64p_frontend.h>
#include <mupen64plus/m64p_config.h>
#include <mupen64plus/m64p_debugger.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_BP 32
static unsigned exec_bp[MAX_BP]; static int exec_count;
static unsigned write_bp[MAX_BP]; static unsigned write_len[MAX_BP]; static int write_count;
static int hits_left = 40;
static int want_stack = 0;
// A ring of the most recent stub addresses, so a breakpoint somewhere else can
// print the calls that led to it. Recording is a store; printing is rare.
#define RING 65536
static unsigned ring[RING]; static unsigned ring_at = 0;
static unsigned trace_lo = 0, trace_hi = 0;
// Which addresses in a window the game actually executed. One bit each, which
// is what makes "did it ever get here" answerable for a whole subsystem at
// once rather than one breakpoint at a time -- the question a diff against the
// other console's own list of functions wants.
static unsigned census_lo = 0, census_hi = 0;
static unsigned char *census = NULL;
static const char *census_path = NULL;
static int ring_want = 0;
static int want_regs = 0;
static unsigned mem_at = 0; static int mem_words = 0;
static const char *dump_path = NULL;
static int skip_hits = 0;
static double run_seconds = 30.0;


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
static const char *gfx_path(void) {
    // The core's own stub video plugin never finishes a display list, so a game
    // that waits on the display processor stops a frame or two in. That is fine
    // for a question about boot and useless for a question about play, so a real
    // one can be named here and is attached when it is.
    return getenv("M64P_GFX");
}
static const char *rsp_path(void) {
    return env_or("M64P_RSP", "/opt/homebrew/lib/mupen64plus/mupen64plus-rsp-hle.dylib");
}
static const char *data_path(void) {
    return env_or("M64P_DATA", "/opt/homebrew/share/mupen64plus");
}

static void debug_cb(void *ctx, int level, const char *message) {
    (void)ctx;
    if (level <= 2) fprintf(stderr, "[core %d] %s\n", level, message);
}
static void state_cb(void *ctx, m64p_core_param p, int v) { (void)ctx; (void)p; (void)v; }

static const char *reg_name[32] = {
    "zero","at","v0","v1","a0","a1","a2","a3","t0","t1","t2","t3","t4","t5","t6","t7",
    "s0","s1","s2","s3","s4","s5","s6","s7","t8","t9","k0","k1","gp","sp","fp","ra"};

static void dbg_init(void) {
    for (int i = 0; i < exec_count; i++) {
        m64p_breakpoint bp;
        memset(&bp, 0, sizeof(bp));
        bp.address = exec_bp[i];
        bp.endaddr = exec_bp[i];
        bp.flags = M64P_BKP_FLAG_ENABLED | M64P_BKP_FLAG_EXEC;
        DebugBreakpointCommand(M64P_BKP_CMD_ADD_STRUCT, 0, &bp);
        fprintf(stderr, "[bp] exec 0x%08X\n", exec_bp[i]);
    }
    if (trace_hi != 0) {
        m64p_breakpoint bp;
        memset(&bp, 0, sizeof(bp));
        bp.address = trace_lo; bp.endaddr = trace_hi - 1;
        bp.flags = M64P_BKP_FLAG_ENABLED | M64P_BKP_FLAG_EXEC;
        DebugBreakpointCommand(M64P_BKP_CMD_ADD_STRUCT, 0, &bp);
        fprintf(stderr, "[bp] ring over 0x%08X..0x%08X\n", trace_lo, trace_hi - 1);
    }
    if (census != NULL) {
        m64p_breakpoint bp;
        memset(&bp, 0, sizeof(bp));
        bp.address = census_lo; bp.endaddr = census_hi - 1;
        bp.flags = M64P_BKP_FLAG_ENABLED | M64P_BKP_FLAG_EXEC;
        DebugBreakpointCommand(M64P_BKP_CMD_ADD_STRUCT, 0, &bp);
        fprintf(stderr, "[bp] census over 0x%08X..0x%08X\n", census_lo, census_hi - 1);
    }
    for (int i = 0; i < write_count; i++) {
        m64p_breakpoint bp;
        memset(&bp, 0, sizeof(bp));
        bp.address = write_bp[i];
        bp.endaddr = write_bp[i] + write_len[i] - 1;
        bp.flags = M64P_BKP_FLAG_ENABLED | M64P_BKP_FLAG_WRITE;
        DebugBreakpointCommand(M64P_BKP_CMD_ADD_STRUCT, 0, &bp);
        fprintf(stderr, "[bp] write 0x%08X..0x%08X\n", bp.address, bp.endaddr);
    }
    DebugSetRunState(M64P_DBG_RUNSTATE_RUNNING);
    DebugStep();
}

static void dbg_update(unsigned int pc) {
    if (census != NULL && pc >= census_lo && pc < census_hi) {
        census[(pc - census_lo) >> 5] |= (unsigned char)(1u << (((pc - census_lo) >> 2) & 7));
        DebugSetRunState(M64P_DBG_RUNSTATE_RUNNING);
        return;
    }
    if (trace_hi != 0 && pc >= trace_lo && pc < trace_hi) {
        ring[ring_at++ % RING] = pc;
        DebugSetRunState(M64P_DBG_RUNSTATE_RUNNING);
        return;
    }
    if (skip_hits > 0) { skip_hits--; DebugSetRunState(M64P_DBG_RUNSTATE_RUNNING); return; }
    if (hits_left > 0) {
        hits_left--;
        long long *r = (long long *)DebugGetCPUDataPtr(M64P_CPU_REG_REG);
        unsigned flags = 0, accessed = 0;
        DebugBreakpointTriggeredBy(&flags, &accessed);
        fprintf(stderr, "[hit] pc=%08X flags=%X at=%08X ra=%08X a0=%08X a1=%08X a2=%08X a3=%08X "
                        "v0=%08X sp=%08X\n",
                pc, flags, accessed, (unsigned)r[31], (unsigned)r[4], (unsigned)r[5],
                (unsigned)r[6], (unsigned)r[7], (unsigned)r[2], (unsigned)r[29]);
        (void)reg_name;
        if (dump_path != NULL) {
            void *rdram = DebugMemGetPointer(M64P_DBG_PTR_RDRAM);
            FILE *f = fopen(dump_path, "wb");
            if (f != NULL) { fwrite(rdram, 1, 0x800000, f); fclose(f);
                             fprintf(stderr, "  wrote %s\n", dump_path); }
            dump_path = NULL;
        }
        if (want_regs) {
            for (int j = 0; j < 32; j++) {
                fprintf(stderr, "  %s=%08X%s", reg_name[j], (unsigned)r[j], (j % 6 == 5) ? "\n" : "");
            }
            fprintf(stderr, "\n");
        }
        if (mem_words > 0) {
            fprintf(stderr, "  mem %08X:", mem_at);
            for (int j = 0; j < mem_words; j++) {
                fprintf(stderr, " %08X", DebugMemRead32(mem_at + j * 4));
            }
            fprintf(stderr, "\n");
        }
        if (want_stack) {
            // Return addresses left on the stack, which is the only call chain
            // a breakpoint can recover: MIPS keeps no frame pointer, so what
            // is here is every $ra some frame saved, newest first.
            unsigned sp = (unsigned)r[29];
            fprintf(stderr, "  stack:");
            for (int j = 0; j < want_stack; j++) {
                unsigned v = DebugMemRead32(sp + j * 4);
                if ((v >= 0x80012030u && v < 0x80126800u) || (v >= 0x80190000u && v < 0x80400000u)) {
                    fprintf(stderr, " %08X", v);
                }
            }
            fprintf(stderr, "\n");
        }
        if (ring_want > 0) {
            unsigned have = ring_at < (unsigned)ring_want ? ring_at : (unsigned)ring_want;
            fprintf(stderr, "  ring (%u total, last %u):\n", ring_at, have);
            for (unsigned j = ring_at - have; j < ring_at; j++) {
                fprintf(stderr, "   ring %08X\n", ring[j % RING]);
            }
        }
        fflush(stderr);
    }
    DebugSetRunState(M64P_DBG_RUNSTATE_RUNNING);
}

static void dbg_vi(void) {}

static void *timer_thread(void *arg) {
    (void)arg;
    usleep((useconds_t)(run_seconds * 1000000.0));
    fprintf(stderr, "[time] stopping after %.1fs\n", run_seconds);
    DebugSetRunState(M64P_DBG_RUNSTATE_RUNNING);
    CoreDoCommand(M64CMD_STOP, 0, NULL);
    return NULL;
}

int main(int argc, char **argv) {
    const char *rom_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
            exec_bp[exec_count++] = (unsigned)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            char *colon = NULL;
            write_bp[write_count] = (unsigned)strtoul(argv[++i], &colon, 0);
            write_len[write_count] = (colon != NULL && *colon == ':') ? (unsigned)strtoul(colon + 1, NULL, 0) : 4;
            write_count++;
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            run_seconds = atof(argv[++i]);
        } else if (strcmp(argv[i], "-D") == 0 && i + 1 < argc) {
            dump_path = argv[++i];
        } else if (strcmp(argv[i], "-g") == 0) {
            want_regs = 1;
        } else if (strcmp(argv[i], "-m") == 0 && i + 2 < argc) {
            mem_at = (unsigned)strtoul(argv[++i], NULL, 0);
            mem_words = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-r") == 0 && i + 2 < argc) {
            trace_lo = (unsigned)strtoul(argv[++i], NULL, 0);
            trace_hi = (unsigned)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "-c") == 0 && i + 3 < argc) {
            census_lo = (unsigned)strtoul(argv[++i], NULL, 0);
            census_hi = (unsigned)strtoul(argv[++i], NULL, 0);
            census_path = argv[++i];
            census = calloc((census_hi - census_lo) / 32 + 1, 1);
        } else if (strcmp(argv[i], "-R") == 0 && i + 1 < argc) {
            ring_want = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            want_stack = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-k") == 0 && i + 1 < argc) {
            skip_hits = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            hits_left = atoi(argv[++i]);
        } else {
            rom_path = argv[i];
        }
    }
    if (rom_path == NULL) { fprintf(stderr, "usage: refdbg <rom> [-e addr] [-w addr[:len]] [-t secs] [-n hits]\n"); return 2; }

    if (CoreStartup(0x020102, NULL, data_path(), NULL, debug_cb, NULL, state_cb)
        != M64ERR_SUCCESS) { fprintf(stderr, "CoreStartup failed\n"); return 1; }

    m64p_handle section;
    if (ConfigOpenSection("Core", &section) == M64ERR_SUCCESS) {
        int emumode = 0; int off = 0;
        ConfigSetParameter(section, "R4300Emulator", M64TYPE_INT, &emumode);
        ConfigSetParameter(section, "DisableExtraMem", M64TYPE_BOOL, &off);
        ConfigSetParameter(section, "OnScreenDisplay", M64TYPE_BOOL, &off);
        int on = 1;
        ConfigSetParameter(section, "EnableDebugger", M64TYPE_BOOL, &on);
    }

    void *rsp = dlopen(rsp_path(), RTLD_NOW);
    m64p_error (*rsp_startup)(m64p_dynlib_handle, void *, void (*)(void *, int, const char *)) =
        dlsym(rsp, "PluginStartup");
    void *core = dlopen(core_path(), RTLD_NOW);
    rsp_startup(core, NULL, debug_cb);

    void *gfx = NULL;
    if (gfx_path() != NULL) {
        gfx = dlopen(gfx_path(), RTLD_NOW);
        if (gfx == NULL) { fprintf(stderr, "gfx: %s\n", dlerror()); return 1; }
        m64p_error (*gfx_startup)(m64p_dynlib_handle, void *, void (*)(void *, int, const char *)) =
            dlsym(gfx, "PluginStartup");
        if (gfx_startup(core, NULL, debug_cb) != M64ERR_SUCCESS) {
            fprintf(stderr, "gfx startup failed\n"); return 1;
        }
    }

    FILE *f = fopen(rom_path, "rb");
    if (f == NULL) { perror("rom"); return 1; }
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    void *rom = malloc(size);
    if (fread(rom, 1, size, f) != (size_t)size) return 1;
    fclose(f);
    if (CoreDoCommand(M64CMD_ROM_OPEN, (int)size, rom) != M64ERR_SUCCESS) return 1;
    free(rom);
    if (gfx != NULL) CoreAttachPlugin(M64PLUGIN_GFX, gfx);
    CoreAttachPlugin(M64PLUGIN_RSP, rsp);

    if (DebugSetCallbacks(dbg_init, dbg_update, dbg_vi) != M64ERR_SUCCESS) {
        fprintf(stderr, "this core has no debugger\n"); return 1;
    }

    pthread_t t; pthread_create(&t, NULL, timer_thread, NULL);
    CoreDoCommand(M64CMD_EXECUTE, 0, NULL);
    pthread_join(t, NULL);
    CoreDoCommand(M64CMD_ROM_CLOSE, 0, NULL);
    if (census != NULL && census_path != NULL) {
        FILE *f = fopen(census_path, "w");
        if (f != NULL) {
            unsigned long counted = 0;
            for (unsigned a = census_lo; a < census_hi; a += 4) {
                if (census[(a - census_lo) >> 5] & (1u << (((a - census_lo) >> 2) & 7))) {
                    fprintf(f, "%08X\n", a);
                    counted++;
                }
            }
            fclose(f);
            fprintf(stderr, "[census] %lu of %u addresses executed, written to %s\n",
                    counted, (census_hi - census_lo) / 4, census_path);
        }
    }
    CoreShutdown();
    return 0;
}
