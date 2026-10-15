#include "prx/libkernel/File/include/GuestDescriptors.hpp"

#include <cstdlib>
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

bool ReleaseGuestDescriptor(int fd) {
    if (!GuestDescriptorTableEnabled() || fd >= 0 && fd < StandardStreams) return true;
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
