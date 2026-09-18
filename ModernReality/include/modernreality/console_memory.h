/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The console's memory, and the second views of it the hardware implies.
 *
 * librecomp holds the eight megabytes as one flat array indexed by
 * `address - 0x80000000`, which is right for the window a game normally uses
 * and wrong for two things the console also does.
 *
 * KSEG1 is not a second eight megabytes; it is the same eight megabytes read
 * past the cache. A game that writes a structure through one window and has
 * the RCP read it through the other needs both windows to be the same pages.
 *
 * A TLB entry is the same shape of problem: two addresses, one page.
 *
 * Both are aliases, and an alias of anonymous memory is not something POSIX
 * offers -- `mmap` can only put a view of a *file* somewhere. So the console's
 * memory is given one: an object in memory that the eight megabytes are
 * themselves a view of, after which any number of further views can be made of
 * the same pages with plain `mmap`.
 *
 * This is what a gym already does for a different reason -- the caller in the
 * other process reads Mario out of the same pages -- and it is the same
 * mechanism, so a gym hands its own descriptor in rather than getting a second
 * one. See `include/modernreality/gym.h`.
 *
 * Nothing here is platform-specific. `mach_vm_remap` did this on macOS and has
 * no equivalent on Linux; a shared mapping of a descriptor is the same alias on
 * both, and being the same code on both is why the hard part is exercised
 * wherever this is built.
 */

#ifndef MODERNREALITY_CONSOLE_MEMORY_H
#define MODERNREALITY_CONSOLE_MEMORY_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <string>

namespace n64b {

/// Give the console's memory something to be a view of.
///
/// `rdram` is where librecomp committed it and `bytes` is how much of it is
/// really there -- the eight megabytes, not the four gigabytes reserved around
/// them. The contents are carried across, so this can run after the runtime has
/// already put the first megabyte of the cartridge in place.
///
/// `fd` is a descriptor to use instead of making one, for a caller that already
/// has the memory in an object it shares with somebody; `offset` is where the
/// eight megabytes sit inside it. Pass -1 and 0 for one of our own, which is
/// unlinked immediately and so is never visible to anything else.
///
/// Called once, and before anything below.
bool back_console_memory(uint8_t *rdram, size_t bytes, int fd, off_t offset, std::string &error);

/// Whether the above happened. Every alias below fails without it.
bool console_memory_backed();

/// A second view of the console's memory at `target`.
///
/// `physical` is how far into the eight megabytes the view starts and `bytes`
/// is how much of it to show. Both must be multiples of the host's page size,
/// which for `physical` is what the caller has to work out -- a console page
/// can be smaller than a host page.
bool alias_console_memory(void *target, size_t physical, size_t bytes);

/// Put `target` back the way librecomp reserved it: present, so the next view
/// can overwrite it, and inaccessible, so a read of an unmapped address faults
/// where it happens rather than reading someone else's data.
void unalias_console_memory(void *target, size_t bytes);

} // namespace n64b

#endif /* MODERNREALITY_CONSOLE_MEMORY_H */
