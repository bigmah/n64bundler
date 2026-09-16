/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Driving a game from another process.
 *
 * `n64b-run --gym <name>` runs a game the way a script wants one rather than
 * the way a player does: it advances exactly when it is told to, as fast as the
 * machine can carry it, and stops dead between frames with the console quiet --
 * every thread parked, every device answered -- so that whoever is reading the
 * game reads a settled one. That is what makes a recompiled cartridge usable as
 * an environment for something that learns to play it, and it is the only way
 * to run one faster than the sixty frames a second a television asks for.
 *
 * Two things pass between the two processes.
 *
 * The console's memory is shared, at `N64B_GYM_RDRAM_OFFSET` into the named
 * shared object, so the caller reads the game's own variables -- where Mario
 * is, how fast he is going -- out of the same eight megabytes the recompiled
 * code is running on, with no copying and no protocol for it. It is only safe
 * to read between frames, which is exactly when the caller is awake.
 *
 * So is the picture, for a game started with `--picture`: after every command,
 * the frame the video interface will show next, as 8-bit RGBA at
 * `N64B_GYM_PICTURE_OFFSET`. That is what the game looks like without knowing
 * anything about the game, which is the point of it.
 *
 * Everything else is this block, plus one byte each way over the socket on
 * `N64B_GYM_SOCKET_FD`: the caller fills in a command and writes a byte, the
 * game carries it out and writes a byte back. The byte is a doorbell rather
 * than a message -- what was asked and what happened are both in here.
 */

#ifndef MODERNREALITY_GYM_H
#define MODERNREALITY_GYM_H

#include <stdint.h>

#define N64B_GYM_MAGIC 0x4E36474Du /* "N6GM" */
/* 2 added the picture. */
#define N64B_GYM_ABI 2u

/* The descriptor the host expects its end of the control socket on. A spawned
 * game gets this from whoever spawned it; there is nothing to negotiate. */
#define N64B_GYM_SOCKET_FD 3

/* The shared object: a page of control block, the console's memory, and room
 * for the picture. The offsets are whole numbers of pages on every machine this
 * runs on, because the game's memory is mapped over the runtime's own
 * allocation at that offset and a mapping has to start on a page. */
#define N64B_GYM_RDRAM_OFFSET 0x10000u
#define N64B_GYM_RDRAM_BYTES (8u * 1024u * 1024u)

/* The picture: rows top to bottom, four bytes a pixel, red first and alpha
 * always 255, `picture_width` by `picture_height` of it in use. The room is
 * for the largest frame a console scans out, 640 by 480; most games draw 320
 * by 240. */
#define N64B_GYM_PICTURE_OFFSET (N64B_GYM_RDRAM_OFFSET + N64B_GYM_RDRAM_BYTES)
#define N64B_GYM_PICTURE_MAX_WIDTH 640u
#define N64B_GYM_PICTURE_MAX_HEIGHT 480u
#define N64B_GYM_PICTURE_BYTES (N64B_GYM_PICTURE_MAX_WIDTH * N64B_GYM_PICTURE_MAX_HEIGHT * 4u)

#define N64B_GYM_SHM_BYTES (N64B_GYM_PICTURE_OFFSET + N64B_GYM_PICTURE_BYTES)

/* Where the console's memory starts, as the game addresses it. A caller reading
 * the game's variables has an address out of a decompilation or an analysis and
 * needs the difference between that and an offset into the mapping. */
#define N64B_GYM_RDRAM_BASE 0x80000000u

enum n64b_gym_command {
    N64B_GYM_NOTHING = 0,
    /* Advance `count` frames of the game -- display lists it hands over, which
     * is the same clock `N64B_SCREENSHOT_AFTER` counts in. Each one takes as
     * many retraces as the game takes to draw it, which is two for a game that
     * runs at thirty frames a second and more while it is loading. */
    N64B_GYM_STEP = 1,
    /* Write everything needed to come back to this exact moment to `path`. */
    N64B_GYM_SAVE_STATE = 2,
    /* Put back a state written by the above. */
    N64B_GYM_LOAD_STATE = 3,
    /* End the game and the process. */
    N64B_GYM_QUIT = 4,
};

enum n64b_gym_status {
    N64B_GYM_OK = 0,
    /* The game did not finish what a retrace gave it to do, or drew no frame
     * however many retraces it was given. A game waiting for something that is
     * not coming looks like this. */
    N64B_GYM_STALLED = 1,
    /* The command could not be carried out; `message` says why. */
    N64B_GYM_REFUSED = 2,
};

/* Which frames of a step a headless game with a picture draws. A frame not
 * drawn is still a frame of the game -- its display list is handed over and
 * counted -- and nothing is rendered for it.
 *
 * Drawing only the last frame is nearly twice as fast at two frames a step,
 * and drawing nothing is for getting somewhere, such as replaying inputs
 * after a load, with no picture wanted until it ends. Both are wrong for a
 * game that builds a frame out of the one before it, so they are the
 * caller's to ask for. A step that draws nothing has no picture. */
enum n64b_gym_draw {
    N64B_GYM_DRAW_EVERY_FRAME = 0,
    N64B_GYM_DRAW_LAST_FRAME = 1,
    N64B_GYM_DRAW_NOTHING = 2,
};

/* One controller. The buttons are libultra's own bits -- A is 0x8000, B 0x4000,
 * Z 0x2000, Start 0x1000 -- and the stick is the fraction of full deflection
 * the host's own controller callback reports, so -1 to 1 in each axis. */
struct n64b_gym_pad {
    uint16_t buttons;
    uint16_t padding;
    float stick_x;
    float stick_y;
};

struct n64b_gym_block {
    uint32_t magic;
    uint32_t abi;
    uint32_t rdram_offset;
    uint32_t rdram_bytes;

    /* What the caller wants. */
    uint32_t command;
    uint32_t count;
    struct n64b_gym_pad pad[4];
    char path[512];
    /* With `--headless --picture`, which of the frames a step advances are
     * drawn: see `n64b_gym_draw`. */
    uint32_t draw;

    /* What happened. */
    uint32_t status;
    /* Retraces the last command spent, which is how a caller sees a game that
     * is taking longer over a frame than it usually does. */
    uint32_t retraces_taken;
    /* Frames the game has drawn, and retraces it has been given, since it
     * booted. Neither is reset by loading a state: they count this process's
     * work rather than the game's own clock. */
    uint64_t frames;
    uint64_t retraces;
    char message[256];

    /* The picture, with `--picture`: how much of the room it fills, and the
     * frame count it was taken at. A width of zero is no picture, which is
     * what a game without `--picture` always says, what one says while its
     * video interface is blanked or has not been set up, and what one says
     * after a load or a step that drew nothing, until it draws a frame: a
     * state holds framebuffers in whatever condition the game that saved it
     * left them. */
    uint32_t picture_width;
    uint32_t picture_height;
    uint64_t picture_frame;
};

#endif
