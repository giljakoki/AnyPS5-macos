#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <unistd.h>

extern "C" {
int APS5_VABI sceKernelOpen(const char* path, int flags, std::uint16_t mode);
std::int64_t APS5_VABI sceKernelWrite(int d, const void* buf, std::size_t nbytes);
int APS5_VABI sceKernelClose(int d);
void ExitFinalizerOrderFixtureSet(void (*callback)());
}

namespace {

constexpr int WriteCreateTruncate = 0x1 | 0x200 | 0x400;
constexpr const char* Name = "/exit_finalizer_open.bin";

void Fail(const char* message) {
    std::fprintf(stderr, "exit finalizer open: %s\n", message);
    std::fflush(stderr);
    _exit(1);
}

void WriteFile() {
    const int file = sceKernelOpen(Name, WriteCreateTruncate, 0666);
    if (file < 0) Fail("opening the file for writing failed");
    const char byte = 1;
    if (sceKernelWrite(file, &byte, 1) != 1) Fail("writing the file failed");
    if (sceKernelClose(file) != 0) Fail("closing the file failed");
}

void OpenAtExit() {
    const auto resolved = ResolvePath_nid_no_patch(Name);
    if (resolved.filename() != "exit_finalizer_open.bin") Fail("a path resolved after static destruction lost its name");
    WriteFile();
    RecordWrittenPath_nid_no_patch(resolved);
    std::error_code error;
    std::filesystem::remove(resolved, error);
    std::puts("exit finalizer open: resolved and opened after static destruction");
    std::fflush(stdout);
}

}

int main() {
    WriteFile();
    RecordWrittenPath_nid_no_patch(ResolvePath_nid_no_patch(Name));
    ExitFinalizerOrderFixtureSet(OpenAtExit);
    std::exit(0);
}
