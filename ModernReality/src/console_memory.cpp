// SPDX-License-Identifier: GPL-3.0-or-later
//
// See include/modernreality/console_memory.h for what this is for.

#include "modernreality/console_memory.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#endif

namespace n64b {
namespace {

int backing_fd = -1;
off_t backing_offset = 0;
size_t backing_bytes = 0;

/// A shared object, opened, sized and unlinked straight away.
///
/// The name is gone before anything else could have opened it, the descriptor
/// keeps the object alive, and what is left is anonymous. Sized before being
/// unlinked because a POSIX shared object may only be sized once and only
/// before it is mapped, which is the order every platform agrees on.
///
/// The name is only ever seen by these two calls and still has to be unique:
/// two consoles started at once would otherwise collide, and O_EXCL would turn
/// that into a failure to start rather than a game quietly sharing another
/// game's memory.
int make_shared_object(size_t bytes, std::string &error) {
    char name[32];
    std::snprintf(name, sizeof name, "/n64b-%d", int(::getpid()));
    int fd = ::shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        error = std::string("shm_open: ") + std::strerror(errno);
        return -1;
    }
    if (::ftruncate(fd, off_t(bytes)) != 0) {
        error = std::string("ftruncate: ") + std::strerror(errno);
        ::close(fd);
        ::shm_unlink(name);
        return -1;
    }
    ::shm_unlink(name);
    return fd;
}

/// An object in memory that nothing else can name.
///
/// Linux has a call for exactly this, and it is worth preferring where it
/// exists: a shared object lives in `/dev/shm`, which a container is routinely
/// given only 64MB of, and an anonymous file in memory is subject to no such
/// limit. Everywhere else -- macOS, and a Linux whose headers predate the call
/// -- a shared object that is unlinked immediately is the same thing.
int make_backing_file(size_t bytes, std::string &error) {
#if defined(__linux__) && defined(SYS_memfd_create)
    const int fd = int(::syscall(SYS_memfd_create, "n64b-console", 1u /* MFD_CLOEXEC */));
    if (fd >= 0) {
        if (::ftruncate(fd, off_t(bytes)) == 0) {
            return fd;
        }
        error = std::string("ftruncate: ") + std::strerror(errno);
        ::close(fd);
        return -1;
    }
    // An old kernel under new headers. Fall through to the portable way.
#endif
    return make_shared_object(bytes, error);
}

} // namespace

bool back_console_memory(uint8_t *rdram, size_t bytes, int fd, off_t offset, std::string &error) {
    if (rdram == nullptr || bytes == 0) {
        error = "there is no console memory to back";
        return false;
    }
    if (backing_fd >= 0) {
        error = "the console's memory already has a backing object";
        return false;
    }

    bool ours = false;
    if (fd < 0) {
        fd = make_backing_file(bytes, error);
        if (fd < 0) {
            return false;
        }
        ours = true;
    }

    // The runtime has already put the first megabyte of the cartridge in these
    // pages by the time this runs, so they are carried out, replaced, and
    // carried back. Replacing them first would start the game with a megabyte
    // of zeroes where its entrypoint is.
    std::vector<uint8_t> carried(bytes);
    std::memcpy(carried.data(), rdram, bytes);

    void *mapped =
        ::mmap(rdram, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, offset);
    if (mapped != rdram) {
        error = std::string("mmap: ") + std::strerror(errno);
        // Put back what was taken away, so that a console whose memory could
        // not be shared is still a console that runs.
        void *restored = ::mmap(rdram, bytes, PROT_READ | PROT_WRITE,
                                MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
        if (restored == rdram) {
            std::memcpy(rdram, carried.data(), bytes);
        }
        if (ours) {
            ::close(fd);
        }
        return false;
    }
    std::memcpy(rdram, carried.data(), bytes);

    backing_fd = fd;
    backing_offset = offset;
    backing_bytes = bytes;
    return true;
}

bool console_memory_backed() { return backing_fd >= 0; }

bool alias_console_memory(void *target, size_t physical, size_t bytes) {
    if (backing_fd < 0) {
        return false;
    }
    // A view that ran off the end of the object would be a view of nothing: the
    // pages past it read as zero and write nowhere, which is the failure that
    // looks like the game's own memory quietly not working.
    if (bytes > backing_bytes || physical > backing_bytes - bytes) {
        return false;
    }
    void *mapped = ::mmap(target, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
                          backing_fd, backing_offset + off_t(physical));
    return mapped == target;
}

void unalias_console_memory(void *target, size_t bytes) {
    (void)::mmap(target, bytes, PROT_NONE, MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
}

} // namespace n64b
