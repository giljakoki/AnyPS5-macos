#include "SceTypes.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

extern "C" {
int APS5_VABI sceAudioOutOpen(int, int, int, std::uint32_t, std::uint32_t, std::uint32_t);
int APS5_VABI sceAudioOutOutput(int, const void*);
int APS5_VABI scePthreadCreate(Pthread* thread, const PthreadAttr* attr, PthreadEntry entry, void* arg, const char* name);
int APS5_VABI sceKernelCreateSema(KernelSema* sem, const char* name, uint32_t attr, int init, int max, void* opt);
int APS5_VABI sceKernelSignalSema(KernelSema sem, int count);
int APS5_VABI sceKernelWaitSema(KernelSema sem, int need, KernelUseconds* time);
int APS5_VABI scePthreadMutexInit(PthreadMutex* mutex, const PthreadMutexattr* attr, const char* name);
int APS5_VABI scePthreadMutexLock(PthreadMutex* mutex);
int APS5_VABI scePthreadMutexUnlock(PthreadMutex* mutex);
int APS5_VABI scePthreadCondInit(PthreadCond* cond, const PthreadCondattr* attr, const char* name);
int APS5_VABI scePthreadCondTimedwait(PthreadCond* cond, PthreadMutex* mutex, KernelUseconds usec);
}

namespace {

constexpr int User = 0x10000000;
constexpr int PortTypeMain = 0;
constexpr std::uint32_t FormatS16Mono = 0;
constexpr std::uint32_t Frames = 256;
constexpr std::uint32_t Frequency = 48000;
constexpr int Waiters = 4;

std::atomic<int> running{0};
KernelSema sema = nullptr;

void Require(bool value, const char* message) {
    if (value) return;
    std::fprintf(stderr, "%s\n", message);
    std::abort();
}

void* APS5_VABI Waiter(void*) {
    PthreadMutex mutex = nullptr;
    PthreadCond cond = nullptr;
    Require(scePthreadMutexInit(&mutex, nullptr, "exit") == 0 && scePthreadCondInit(&cond, nullptr, "exit") == 0, "creating a condition variable failed");
    running.fetch_add(1);
    for (;;) {
        KernelUseconds timeout = 10;
        sceKernelWaitSema(sema, 1, &timeout);
        sceKernelSignalSema(sema, 1);
        scePthreadMutexLock(&mutex);
        scePthreadCondTimedwait(&cond, &mutex, 10);
        scePthreadMutexUnlock(&mutex);
    }
}

void* APS5_VABI Speaker(void* arg) {
    const int handle = *static_cast<int*>(arg);
    std::vector<std::int16_t> block(Frames, 0);
    running.fetch_add(1);
    for (;;) sceAudioOutOutput(handle, block.data());
}

}

int main() {
    setenv("APS5_TRACE_WAITS", "1", 1);
    Require(sceKernelCreateSema(&sema, "exit", 0, 0, 4, nullptr) == 0, "creating the semaphore failed");
    int handle = sceAudioOutOpen(User, PortTypeMain, 0, Frames, Frequency, FormatS16Mono);
    Require(handle > 0, "the port must open");
    Pthread thread = nullptr;
    for (int index = 0; index < Waiters; ++index) Require(scePthreadCreate(&thread, nullptr, Waiter, nullptr, "waiter") == 0, "creating a waiter failed");
    Require(scePthreadCreate(&thread, nullptr, Speaker, &handle, "speaker") == 0, "creating the speaker failed");
    while (running.load() != Waiters + 1) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::puts("audio out exit while running tests passed");
    std::fflush(stdout);
    std::exit(0);
}
