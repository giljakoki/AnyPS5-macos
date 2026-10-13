#include "SceTypes.hpp"
#include "prx/libc/include/general/AsmMacros.hpp"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <thread>

extern "C" {
int APS5_VABI sceKernelInstallExceptionHandler(int signum, void* handler);
int APS5_VABI sceKernelRaiseException(Pthread thread, int signum);
int APS5_VABI scePthreadCreate(Pthread* thread, const PthreadAttr* attr, PthreadEntry entry, void* arg, const char* name);
int APS5_VABI scePthreadJoin(Pthread thread, void** retval);
int APS5_VABI scePthreadMutexInit(PthreadMutex* mutex, const PthreadMutexattr* attr, const char* name);
int APS5_VABI scePthreadMutexLock(PthreadMutex* mutex);
int APS5_VABI scePthreadMutexUnlock(PthreadMutex* mutex);
int APS5_VABI scePthreadCondInit(PthreadCond* cond, const PthreadCondattr* attr, const char* name);
int APS5_VABI scePthreadCondWait(PthreadCond* cond, PthreadMutex* mutex);
int APS5_VABI scePthreadCondTimedwait(PthreadCond* cond, PthreadMutex* mutex, KernelUseconds usec);
int APS5_VABI scePthreadCondBroadcast(PthreadCond* cond);
int APS5_VABI sceKernelCreateSema(KernelSema* sem, const char* name, uint32_t attr, int init, int max, void* opt);
int APS5_VABI sceKernelSignalSema(KernelSema sem, int count);
int APS5_VABI sceKernelWaitSema(KernelSema sem, int need, KernelUseconds* time);
int APS5_VABI sceKernelCreateEventFlag(KernelEventFlag* ef, const char* name, uint32_t attr, uint64_t init, const void* param);
int APS5_VABI sceKernelSetEventFlag(KernelEventFlag ef, uint64_t pattern);
int APS5_VABI sceKernelClearEventFlag(KernelEventFlag ef, uint64_t pattern);
int APS5_VABI sceKernelWaitEventFlag(KernelEventFlag ef, uint64_t pattern, uint32_t mode, uint64_t* result, KernelUseconds* timeout);
}

namespace {

constexpr int RaisedSignal = 30;
constexpr int Rounds = 300;
constexpr std::uint32_t WaitAnd = 0x01;
constexpr std::uint32_t WaitOr = 0x02;
constexpr std::uint32_t MultipleWaiters = 0x20;
constexpr std::size_t ContextRsp = 0xf8;

KernelSema suspended = nullptr;
KernelSema resumed = nullptr;
KernelEventFlag resume = nullptr;
std::atomic<bool> stopping{false};
std::atomic<int> started{0};
std::atomic<int> handled{0};

void Require(bool value, const char* message) {
    if (value) return;
    std::fprintf(stderr, "%s\n", message);
    std::abort();
}

void APS5_VABI StopWorldHandler(int signum, void* context) {
    Require(signum == RaisedSignal, "unexpected signal number");
    std::uint64_t rsp = 0;
    std::memcpy(&rsp, static_cast<const unsigned char*>(context) + ContextRsp, sizeof(rsp));
    const auto here = reinterpret_cast<std::uintptr_t>(&rsp);
    Require(rsp > here && rsp - here < (std::uint64_t{1} << 20), "the handler context does not hold the interrupted stack pointer");
    handled.fetch_add(1);
    Require(sceKernelSignalSema(suspended, 1) == 0, "acknowledging the suspend failed");
    std::uint64_t pattern = 0;
    Require(sceKernelWaitEventFlag(resume, 1, WaitAnd, &pattern, nullptr) == 0, "waiting for the resume failed");
    Require(sceKernelSignalSema(resumed, 1) == 0, "acknowledging the resume failed");
}

void BlockHostSignals() {
    sigset_t all;
    sigfillset(&all);
    sigdelset(&all, SIGBUS);
    sigdelset(&all, SIGSEGV);
    Require(pthread_sigmask(SIG_BLOCK, &all, nullptr) == 0, "blocking the host signals failed");
}

void* APS5_VABI EventFlagWaiter(void*) {
    BlockHostSignals();
    KernelEventFlag flag = nullptr;
    Require(sceKernelCreateEventFlag(&flag, "idle", MultipleWaiters, 0, nullptr) == 0, "creating an event flag failed");
    started.fetch_add(1);
    std::uint64_t pattern = 0;
    while (!stopping.load()) {
        KernelUseconds timeout = 200000;
        sceKernelWaitEventFlag(flag, 1, WaitOr, &pattern, &timeout);
    }
    return nullptr;
}

void* APS5_VABI SemaphoreWaiter(void*) {
    BlockHostSignals();
    KernelSema sema = nullptr;
    Require(sceKernelCreateSema(&sema, "idle", 0, 0, 1, nullptr) == 0, "creating a semaphore failed");
    started.fetch_add(1);
    while (!stopping.load()) {
        KernelUseconds timeout = 200000;
        sceKernelWaitSema(sema, 1, &timeout);
    }
    return nullptr;
}

void* APS5_VABI ConditionWaiter(void*) {
    BlockHostSignals();
    PthreadMutex mutex = nullptr;
    PthreadCond cond = nullptr;
    Require(scePthreadMutexInit(&mutex, nullptr, "idle") == 0 && scePthreadCondInit(&cond, nullptr, "idle") == 0, "creating a condition variable failed");
    started.fetch_add(1);
    Require(scePthreadMutexLock(&mutex) == 0, "locking the mutex failed");
    while (!stopping.load()) scePthreadCondTimedwait(&cond, &mutex, 200000);
    Require(scePthreadMutexUnlock(&mutex) == 0, "unlocking the mutex failed");
    return nullptr;
}

void* APS5_VABI ShortWaiter(void*) {
    KernelSema sema = nullptr;
    Require(sceKernelCreateSema(&sema, "short", 0, 0, 1, nullptr) == 0, "creating a semaphore failed");
    started.fetch_add(1);
    volatile std::uint64_t work = 0;
    while (!stopping.load()) {
        for (int index = 0; index < 2000; ++index) work = work + 1;
        KernelUseconds timeout = 100;
        sceKernelWaitSema(sema, 1, &timeout);
    }
    return nullptr;
}

void* APS5_VABI Runner(void*) {
    started.fetch_add(1);
    volatile std::uint64_t work = 0;
    while (!stopping.load()) work = work + 1;
    return nullptr;
}

}

int main() {
    Require(sceKernelCreateSema(&suspended, "SuspendSemaphore", 0, 0, 256, nullptr) == 0, "creating the suspend semaphore failed");
    Require(sceKernelCreateSema(&resumed, "ResumeSemaphore", 0, 0, 256, nullptr) == 0, "creating the resume semaphore failed");
    Require(sceKernelCreateEventFlag(&resume, "resumeEvent", MultipleWaiters | 0x01, 0, nullptr) == 0, "creating the resume event failed");
    Require(sceKernelInstallExceptionHandler(RaisedSignal, reinterpret_cast<void*>(&StopWorldHandler)) == 0, "installing the handler failed");

    PthreadEntry entries[] = {EventFlagWaiter, SemaphoreWaiter, ConditionWaiter, EventFlagWaiter, SemaphoreWaiter, ConditionWaiter, ShortWaiter, ShortWaiter, Runner};
    constexpr int Count = sizeof(entries) / sizeof(entries[0]);
    Pthread threads[Count]{};
    for (int index = 0; index < Count; ++index) Require(scePthreadCreate(&threads[index], nullptr, entries[index], nullptr, "worker") == 0, "creating a worker failed");
    while (started.load() != Count) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    for (int round = 0; round < Rounds; ++round) {
        for (Pthread thread : threads) Require(sceKernelRaiseException(thread, RaisedSignal) == 0, "raising on a worker failed");
        KernelUseconds timeout = 5000000;
        Require(sceKernelWaitSema(suspended, Count, &timeout) == 0, "a worker never ran the exception handler");
        Require(sceKernelSetEventFlag(resume, 1) == 0, "resuming failed");
        timeout = 5000000;
        Require(sceKernelWaitSema(resumed, Count, &timeout) == 0, "a suspended worker never resumed");
        Require(sceKernelClearEventFlag(resume, 0) == 0, "clearing the resume event failed");
    }
    Require(handled.load() == Rounds * Count, "a raise ran the handler more than once");

    stopping.store(true);
    for (Pthread thread : threads) Require(scePthreadJoin(thread, nullptr) == 0, "joining a worker failed");
    std::puts("raise waiting thread tests passed");
    return 0;
}
