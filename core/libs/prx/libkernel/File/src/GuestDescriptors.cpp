#include "prx/libkernel/File/include/GuestDescriptors.hpp"

#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <mutex>
#include <set>

namespace File {

#ifdef _WIN32

bool GuestDescriptorTableEnabled() { return false; }
int GuestDescriptorFloor() { return 0; }
void AdoptGuestDescriptor(int) {}
bool ReleaseGuestDescriptor(int) { return true; }
bool GuestDescriptorRejected(int) { return false; }

#else

namespace {
std::mutex& Lock() { static auto& lock = *new std::mutex; return lock; }
std::set<int>& Owned() { static auto& owned = *new std::set<int>; return owned; }
// Standard streams are inherited by the guest rather than opened by it.
constexpr int StandardStreams = 3;

// Keep 0-2 occupied so no host allocation (a shm backing, MoltenVK file) can take one of those numbers when the
// process was started with a standard stream closed.
const bool StandardStreamsOccupied = [] {
    for (int fd = 0; fd < StandardStreams; ++fd) {
        if (::fcntl(fd, F_GETFD) != -1) continue;
        const int null = ::open("/dev/null", fd == 0 ? O_RDONLY : O_WRONLY);
        if (null >= 0 && null != fd) {
            ::dup2(null, fd);
            ::close(null);
        }
    }
    return true;
}();
}

bool GuestDescriptorTableEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_NO_GUEST_FD_TABLE");
        return value == nullptr || *value == '\0' || *value == '0';
    }();
    return enabled;
}

int GuestDescriptorFloor() { return GuestDescriptorTableEnabled() ? 512 : 0; }

void AdoptGuestDescriptor(int fd) {
    if (!GuestDescriptorTableEnabled() || fd < StandardStreams) return;
    std::lock_guard guard(Lock());
    Owned().insert(fd);
}

// The host's standard streams must never be closed by the guest: a title closing descriptor 0 (Outer Wilds
// calls sceKernelClose(0)) would free that number for the next host allocation — a direct memory backing —
// and a later guest close of 0 then destroys the backing. Closing 0-2 is refused like any descriptor the
// guest does not own; reads and writes on them stay allowed.
bool ReleaseGuestDescriptor(int fd) {
    if (!GuestDescriptorTableEnabled()) return true;
    if (fd >= 0 && fd < StandardStreams) return false;
    std::lock_guard guard(Lock());
    return Owned().erase(fd) != 0;
}

bool GuestDescriptorRejected(int fd) {
    if (!GuestDescriptorTableEnabled() || fd < StandardStreams) return false;
    std::lock_guard guard(Lock());
    return Owned().count(fd) == 0;
}

#endif

}
