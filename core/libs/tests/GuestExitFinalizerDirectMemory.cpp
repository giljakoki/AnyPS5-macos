#include "SceTypes.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

extern "C" {
int APS5_VABI sceKernelAllocateDirectMemory(std::int64_t searchStart, std::int64_t searchEnd, std::size_t length, std::size_t alignment, int memoryType, std::int64_t* physicalAddress);
int APS5_VABI sceKernelMapDirectMemory(void** address, std::size_t length, int protection, int flags, std::int64_t directMemoryStart, std::size_t alignment);
int APS5_VABI sceKernelReleaseDirectMemory(std::int64_t start, std::size_t length);
void ExitFinalizerOrderFixtureSet(void (*callback)());
}

namespace {

constexpr std::size_t Bytes = 0x200000;
constexpr int ReadWrite = 0x3;
std::int64_t physical = 0;

void Fail(const char* message) {
    std::fprintf(stderr, "exit finalizer direct memory: %s\n", message);
    std::fflush(stderr);
    _exit(1);
}

void ReleaseAtExit() {
    if (sceKernelReleaseDirectMemory(physical, Bytes) != 0) Fail("releasing direct memory from a finalizer failed");
    std::puts("exit finalizer direct memory: released after static destruction");
    std::fflush(stdout);
}

}

int main() {
    if (sceKernelAllocateDirectMemory(0, 0x400000000LL, Bytes, Bytes, 0, &physical) != 0) Fail("allocating direct memory failed");
    void* address = nullptr;
    if (sceKernelMapDirectMemory(&address, Bytes, ReadWrite, 0, physical, Bytes) != 0 || address == nullptr) Fail("mapping direct memory failed");
    static_cast<volatile char*>(address)[0] = 1;
    ExitFinalizerOrderFixtureSet(ReleaseAtExit);
    std::exit(0);
}
