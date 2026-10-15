#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libkernel/File/include/FileFlags.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <fcntl.h>
#include <unistd.h>

extern "C" {
int APS5_VABI sceKernelOpen(const char*, int, std::uint16_t);
int APS5_VABI sceKernelClose(int);
std::int64_t APS5_VABI sceKernelRead(int, void*, std::size_t);
std::int64_t APS5_VABI sceKernelLseek(int, std::int64_t, int);
int APS5_VABI close_nid_postfix(int);
}

static void Check(bool value, int line) {
    if (!value) {
        std::fprintf(stderr, "Guest descriptor table check failed at line %d\n", line);
        std::abort();
    }
}
#define Require(value) Check((value), __LINE__)

static constexpr int ErrorEbadf = static_cast<int>(0x80020009u);

int main() {
    const auto directory = std::filesystem::path("anyps5-descriptor-table-" + std::to_string(::getpid()));
    std::filesystem::create_directories(directory);
    const auto file = directory / "file.bin";
    std::ofstream(file, std::ios::binary) << "DATA";

    // A host-owned descriptor (driver file, direct-memory backing) is not the
    // guest's: closing or using its number must fail and leave it open.
    const int host = ::open(file.c_str(), O_RDONLY);
    Require(host >= 3);
    char byte;
    Require(sceKernelClose(host) == ErrorEbadf);
    Require(sceKernelRead(host, &byte, 1) == ErrorEbadf);
    Require(sceKernelLseek(host, 0, 0) == ErrorEbadf);
    Require(close_nid_postfix(host) != 0);
    Require(::fcntl(host, F_GETFD) >= 0);

    // Guest double close returns EBADF and must not close a host descriptor
    // that was handed the freed number in the meantime.
    const int guest = sceKernelOpen(file.c_str(), SCE_KERNEL_O_RDONLY, 0);
    Require(guest >= 0);
    Require(sceKernelRead(guest, &byte, 1) == 1);
    Require(sceKernelClose(guest) == 0);
    Require(sceKernelClose(guest) == ErrorEbadf);
    const int reused = ::dup2(host, guest);  // host takes the number the guest released
    Require(reused == guest);
    Require(sceKernelClose(guest) == ErrorEbadf);
    Require(::fcntl(guest, F_GETFD) >= 0);
    Require(::fcntl(host, F_GETFD) >= 0);

    // Guest descriptors stay clear of the low numbers host allocation uses.
    const int high = sceKernelOpen(file.c_str(), SCE_KERNEL_O_RDONLY, 0);
    Require(high >= 512);
    Require(sceKernelClose(high) == 0);

    ::close(guest);
    ::close(host);
    std::filesystem::remove_all(directory);
    return 0;
}
