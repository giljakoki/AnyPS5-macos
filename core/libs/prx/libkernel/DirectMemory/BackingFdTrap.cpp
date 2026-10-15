#include "prx/libkernel/DirectMemory/BackingFdTrap.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <cstdint>
#include <string>

// Private but stable libsystem_kernel API (xnu bsd/sys/guarded.h); the SDK does not ship the header.
typedef std::uint64_t guardid_t;
extern "C" int change_fdguard_np(int fd, const guardid_t* guard, unsigned guardflags, const guardid_t* nguard, unsigned nguardflags, int* fdflagsp);
extern "C" int guarded_close_np(int fd, const guardid_t* guard);
#endif

namespace DirectMemoryTrap {

bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_TRAP_BACKING_FD");
        return value != nullptr && *value != '\0' && *value != '0';
    }();
    return enabled;
}

#ifdef __APPLE__
namespace {

constexpr unsigned GuardClose = 1u << 0;
constexpr unsigned GuardDup = 1u << 1;
constexpr guardid_t GuardId = 0x4150533544424b46ull;  // "APS5DBKF"

}  // namespace

bool GuardBackingFd(int fd) {
    if (!Enabled()) return true;
    int flags = FD_CLOEXEC;
    if (change_fdguard_np(fd, nullptr, 0, &GuardId, GuardClose | GuardDup, &flags) != 0) {
        std::fprintf(stderr, "[fdtrap] guarding backing fd %d failed: %s\n", fd, std::strerror(errno));
        return false;
    }
    std::fprintf(stderr, "[fdtrap] guarded direct memory backing fd %d\n", fd);
    return true;
}

int CreateBackingFile(unsigned long long bytes) {
    const char* directory = std::getenv("TMPDIR");
    std::string path = std::string(directory && *directory ? directory : "/tmp") + "/aps5-dm-XXXXXX";
    const int fd = ::mkstemp(path.data());
    if (fd < 0) return -1;
    ::unlink(path.c_str());
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0 || !GuardBackingFd(fd)) {
        const int error = errno;
        ::close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

void CloseBackingFd(int fd) {
    if (Enabled() && guarded_close_np(fd, &GuardId) == 0) return;
    ::close(fd);
}

void CheckBackingFd(int fd, const void* backing, unsigned long long bytes) {
    if (!Enabled()) return;
    if (::fcntl(fd, F_GETFD) >= 0) return;
    const int error = errno;
    std::fprintf(stderr, "[fdtrap] backing %p (0x%llx bytes) fd %d is no longer open (%s) yet no guarded close fired: "
        "the fd was never valid or the backing object is stale (lifetime bug)\n", backing, bytes, fd, std::strerror(error));
    std::fflush(stderr);
    std::abort();
}

#else

bool GuardBackingFd(int) { return true; }
int CreateBackingFile(unsigned long long) { errno = ENOTSUP; return -1; }
#ifdef _WIN32
void CloseBackingFd(int) {}
#else
void CloseBackingFd(int fd) { ::close(fd); }
#endif
void CheckBackingFd(int, const void*, unsigned long long) {}

#endif

}  // namespace DirectMemoryTrap
