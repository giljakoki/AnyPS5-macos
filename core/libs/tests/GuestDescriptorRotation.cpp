#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libkernel/File/include/FileFlags.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <fcntl.h>
#include <unistd.h>

extern "C" {
int APS5_VABI sceKernelOpen(const char*, int, std::uint16_t);
int APS5_VABI sceKernelClose(int);
std::int64_t APS5_VABI sceKernelPread(int, void*, std::size_t, std::int64_t);
}

static void Check(bool value, int line) {
    if (!value) {
        std::fprintf(stderr, "Descriptor rotation check failed at line %d\n", line);
        std::abort();
    }
}
#define Require(value) Check((value), __LINE__)

static constexpr int ErrorEbadf = static_cast<int>(0x80020009u);

static void WriteFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream(path, std::ios::binary) << text;
}

int main() {
    const auto directory = std::filesystem::path("anyps5-descriptor-rotation-" + std::to_string(::getpid()));
    std::filesystem::create_directories(directory);
    const auto first = directory / "first.bin";
    const auto second = directory / "second.bin";
    WriteFile(first, "FIRSTFILE");
    WriteFile(second, "SECONDONE");

    // A title that caches reads by descriptor number must not see the number of
    // a file it just closed handed to the next file it opens.
    const int a = sceKernelOpen(first.string().c_str(), SCE_KERNEL_O_RDONLY, 0);
    Require(a >= 0);
    Require(sceKernelClose(a) == 0);
    const int b = sceKernelOpen(second.string().c_str(), SCE_KERNEL_O_RDONLY, 0);
    Require(b >= 0);
    Require(b != a);
    char bytes[9] = {};
    Require(sceKernelPread(b, bytes, sizeof(bytes), 0) == static_cast<std::int64_t>(sizeof(bytes)));
    Require(std::string(bytes, sizeof(bytes)) == "SECONDONE");
    Require(sceKernelClose(a) == ErrorEbadf);
    Require(sceKernelClose(b) == 0);
    Require(sceKernelClose(b) == ErrorEbadf);

    // The moved descriptor keeps close-on-exec off like the original open, and
    // cycling through many opens neither leaks descriptors nor leaves the
    // guest FD_SETSIZE range; numbers repeat only after a full cycle.
    std::set<int> seen;
    int repeats = 0;
    int previous = -1;
    for (int index = 0; index < 4096; ++index) {
        const int fd = sceKernelOpen(index % 2 ? first.string().c_str() : second.string().c_str(), SCE_KERNEL_O_RDONLY, 0);
        Require(fd >= 0 && fd < 1024);
        Require(fd != previous);
        Require((::fcntl(fd, F_GETFD) & FD_CLOEXEC) == 0);
        if (!seen.insert(fd).second) ++repeats;
        previous = fd;
        Require(sceKernelClose(fd) == 0);
    }
    Require(seen.size() > 16);
    Require(repeats > 0);

    // Descriptors held open stay valid and distinct while others rotate past.
    const int held = sceKernelOpen(first.string().c_str(), SCE_KERNEL_O_RDONLY, 0);
    Require(held >= 0);
    for (int index = 0; index < 2048; ++index) {
        const int fd = sceKernelOpen(second.string().c_str(), SCE_KERNEL_O_RDONLY, 0);
        Require(fd >= 0 && fd != held);
        Require(sceKernelClose(fd) == 0);
    }
    Require(sceKernelPread(held, bytes, sizeof(bytes), 0) == static_cast<std::int64_t>(sizeof(bytes)));
    Require(std::string(bytes, sizeof(bytes)) == "FIRSTFILE");
    Require(sceKernelClose(held) == 0);

    std::filesystem::remove_all(directory);
    std::puts("descriptor rotation tests passed");
    return 0;
}
