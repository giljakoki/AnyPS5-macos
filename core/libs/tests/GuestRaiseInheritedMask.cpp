#include "SceTypes.hpp"
#include "prx/libc/include/general/AsmMacros.hpp"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <pthread.h>
#include <thread>

extern "C" {
int APS5_VABI sceKernelInstallExceptionHandler(int signum, void* handler);
int APS5_VABI sceKernelRaiseException(Pthread thread, int signum);
int APS5_VABI scePthreadCreate(Pthread* thread, const PthreadAttr* attr, PthreadEntry entry, void* arg, const char* name);
int APS5_VABI scePthreadJoin(Pthread thread, void** retval);
int APS5_VABI sceKernelCreateSema(KernelSema* sem, const char* name, uint32_t attr, int init, int max, void* opt);
int APS5_VABI sceKernelSignalSema(KernelSema sem, int count);
int APS5_VABI sceKernelWaitSema(KernelSema sem, int need, KernelUseconds* time);
int APS5_VABI sceKernelDeleteSema(KernelSema sem);
int APS5_VABI sceKernelCreateEventFlag(KernelEventFlag* ef, const char* name, uint32_t attr, uint64_t init, const void* param);
int APS5_VABI sceKernelDeleteEventFlag(KernelEventFlag ef);
int APS5_VABI sceKernelSetEventFlag(KernelEventFlag ef, uint64_t pattern);
int APS5_VABI sceKernelClearEventFlag(KernelEventFlag ef, uint64_t pattern);
int APS5_VABI sceKernelWaitEventFlag(KernelEventFlag ef, uint64_t pattern, uint32_t mode, uint64_t* result, KernelUseconds* timeout);
}

namespace {

constexpr int RaisedSignal = 30;
constexpr int Workers = 4;
constexpr int Rounds = 20;
constexpr std::uint32_t WaitAnd = 0x01;
constexpr std::uint32_t WaitOr = 0x02;
constexpr std::uint32_t MultipleWaiters = 0x20;

KernelSema suspended = nullptr;
KernelSema resumed = nullptr;
KernelEventFlag resume = nullptr;
KernelEventFlag idle = nullptr;
std::atomic<int> started{0};
std::atomic<bool> stopping{false};

void Require(bool value, const char* message) {
    if (value) return;
    std::fprintf(stderr, "%s\n", message);
    std::abort();
}

void APS5_VABI StopWorldHandler(int signum, void*) {
    Require(signum == RaisedSignal, "unexpected signal number");
    Require(sceKernelSignalSema(suspended, 1) == 0, "acknowledging the suspend failed");
    std::uint64_t pattern = 0;
    Require(sceKernelWaitEventFlag(resume, 1, WaitAnd, &pattern, nullptr) == 0, "waiting for the resume failed");
    Require(sceKernelSignalSema(resumed, 1) == 0, "acknowledging the resume failed");
}

void* APS5_VABI Idle(void*) {
    started.fetch_add(1);
    while (!stopping.load()) {
        std::uint64_t pattern = 0;
        sceKernelWaitEventFlag(idle, 1, WaitOr, &pattern, nullptr);
    }
    return nullptr;
}

void* APS5_VABI Spawner(void* arg) {
    auto* threads = static_cast<Pthread*>(arg);
    for (int index = 0; index < Workers; ++index) Require(scePthreadCreate(&threads[index], nullptr, Idle, nullptr, "idle") == 0, "creating a worker failed");
    return nullptr;
}

}

int main() {
    Require(sceKernelCreateSema(&suspended, "SuspendSemaphore", 0, 0, 256, nullptr) == 0, "creating the suspend semaphore failed");
    Require(sceKernelCreateSema(&resumed, "ResumeSemaphore", 0, 0, 256, nullptr) == 0, "creating the resume semaphore failed");
    Require(sceKernelCreateEventFlag(&resume, "resumeEvent", MultipleWaiters | 0x01, 0, nullptr) == 0, "creating the resume event failed");
    Require(sceKernelCreateEventFlag(&idle, "idle", MultipleWaiters, 0, nullptr) == 0, "creating the idle event failed");
    Require(sceKernelInstallExceptionHandler(RaisedSignal, reinterpret_cast<void*>(&StopWorldHandler)) == 0, "installing the handler failed");

    sigset_t all;
    sigset_t previous;
    sigfillset(&all);
    Require(pthread_sigmask(SIG_BLOCK, &all, &previous) == 0, "blocking the host signals failed");
    Pthread spawner = nullptr;
    Pthread threads[Workers]{};
    Require(scePthreadCreate(&spawner, nullptr, Spawner, threads, "spawner") == 0, "creating the spawner failed");
    Require(pthread_sigmask(SIG_SETMASK, &previous, nullptr) == 0, "restoring the host signals failed");
    Require(scePthreadJoin(spawner, nullptr) == 0, "joining the spawner failed");
    while (started.load() != Workers) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    for (int round = 0; round < Rounds; ++round) {
        int raised = 0;
        for (Pthread thread : threads)
            if (sceKernelRaiseException(thread, RaisedSignal) == 0) ++raised;
        Require(raised == Workers, "raising on a waiting thread failed");
        KernelUseconds timeout = 2000000;
        Require(sceKernelWaitSema(suspended, raised, &timeout) == 0, "a thread created with the host signals blocked never ran the exception handler");
        Require(sceKernelSetEventFlag(resume, 1) == 0, "resuming failed");
        timeout = 2000000;
        Require(sceKernelWaitSema(resumed, raised, &timeout) == 0, "a suspended thread never resumed");
        Require(sceKernelClearEventFlag(resume, 0) == 0, "clearing the resume event failed");
    }

    stopping.store(true);
    Require(sceKernelSetEventFlag(idle, 1) == 0, "waking the workers failed");
    for (Pthread thread : threads) Require(scePthreadJoin(thread, nullptr) == 0, "joining a worker failed");
    Require(sceKernelDeleteEventFlag(idle) == 0 && sceKernelDeleteEventFlag(resume) == 0, "deleting the event flags failed");
    Require(sceKernelDeleteSema(suspended) == 0 && sceKernelDeleteSema(resumed) == 0, "deleting the semaphores failed");
    std::puts("raise inherited mask tests passed");
    return 0;
}
