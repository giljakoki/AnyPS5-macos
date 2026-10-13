#include "prx/libkernel/System/include/RaiseTrace.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#ifndef _WIN32
#include <csignal>
#include <pthread.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/thread_info.h>
#endif

namespace RaiseTrace {

namespace {

struct Slot {
    std::atomic<std::uintptr_t> key{0};
    std::atomic<std::uint64_t> sentNanos{0};
    std::atomic<std::uint32_t> sent{0};
    std::atomic<std::uint32_t> entered{0};
    std::atomic<std::uint32_t> returned{0};
    std::atomic<std::uint32_t> reported{0};
    unsigned port = 0;
    char name[40]{};
};

constexpr std::size_t SlotCount = 512;
std::array<Slot, SlotCount> slots;
static_assert(std::atomic<std::uintptr_t>::is_always_lock_free && std::atomic<std::uint64_t>::is_always_lock_free);

std::uint64_t Now() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<std::uint64_t>(ts.tv_nsec);
}

const std::uint64_t startNanos = Now();

double Seconds(std::uint64_t nanos) {
    return static_cast<double>(nanos - startNanos) / 1e9;
}

Slot* Find(Pthread thread) {
    const auto key = reinterpret_cast<std::uintptr_t>(thread);
    for (auto& slot : slots)
        if (slot.key.load(std::memory_order_acquire) == key) return &slot;
    return nullptr;
}

Slot* Claim(Pthread thread) {
    if (Slot* found = Find(thread)) return found;
    const auto key = reinterpret_cast<std::uintptr_t>(thread);
    for (auto& slot : slots) {
        std::uintptr_t expected = 0;
        if (slot.key.compare_exchange_strong(expected, key, std::memory_order_acq_rel)) return &slot;
    }
    return nullptr;
}

#ifdef __APPLE__
std::string Describe(unsigned port) {
    thread_basic_info_data_t basic{};
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    if (thread_info(port, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&basic), &count) != KERN_SUCCESS) return "gone";
    thread_identifier_info_data_t identifier{};
    count = THREAD_IDENTIFIER_INFO_COUNT;
    std::uint64_t id = 0;
    if (thread_info(port, THREAD_IDENTIFIER_INFO, reinterpret_cast<thread_info_t>(&identifier), &count) == KERN_SUCCESS) id = identifier.thread_id;
    char line[160];
    std::snprintf(line, sizeof(line), "tid %llu run_state %d suspend %d flags 0x%x cpu %.1f%% sleep %d s", static_cast<unsigned long long>(id), basic.run_state, basic.suspend_count, basic.flags,
                  basic.cpu_usage * 100.0 / TH_USAGE_SCALE, basic.sleep_time);
    return line;
}

struct HostThread {
    unsigned port;
    std::uint64_t id;
    double cpu;
    int runState;
    int suspend;
    char name[64];
};

int ReadThreads(HostThread* out, int capacity) {
    thread_act_array_t list = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &list, &count) != KERN_SUCCESS) return 0;
    int written = 0;
    for (mach_msg_type_number_t i = 0; i < count; ++i) {
        if (written < capacity) {
            HostThread& t = out[written];
            t = HostThread{list[i], 0, 0, 0, 0, {}};
            thread_extended_info_data_t extended{};
            mach_msg_type_number_t n = THREAD_EXTENDED_INFO_COUNT;
            if (thread_info(list[i], THREAD_EXTENDED_INFO, reinterpret_cast<thread_info_t>(&extended), &n) == KERN_SUCCESS) {
                t.cpu = extended.pth_cpu_usage * 100.0 / TH_USAGE_SCALE;
                t.runState = extended.pth_run_state;
                std::snprintf(t.name, sizeof(t.name), "%s", extended.pth_name);
            }
            thread_basic_info_data_t basic{};
            n = THREAD_BASIC_INFO_COUNT;
            if (thread_info(list[i], THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&basic), &n) == KERN_SUCCESS) t.suspend = basic.suspend_count;
            thread_identifier_info_data_t identifier{};
            n = THREAD_IDENTIFIER_INFO_COUNT;
            if (thread_info(list[i], THREAD_IDENTIFIER_INFO, reinterpret_cast<thread_info_t>(&identifier), &n) == KERN_SUCCESS) t.id = identifier.thread_id;
            ++written;
        }
        mach_port_deallocate(mach_task_self(), list[i]);
    }
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(list), count * sizeof(thread_act_t));
    return written;
}

void DumpThreads(const char* reason) {
    static HostThread threads[1024];
    const int count = ReadThreads(threads, 1024);
    std::fprintf(stderr, "[raise] %.3f s thread dump (%s): %d host threads\n", Seconds(Now()), reason, count);
    for (int i = 0; i < count; ++i)
        std::fprintf(stderr, "[raise]   tid %llu '%s' run_state %d suspend %d cpu %.1f%%\n", static_cast<unsigned long long>(threads[i].id), threads[i].name, threads[i].runState, threads[i].suspend, threads[i].cpu);
}

void DumpRaisedState() {
    for (auto& slot : slots) {
        if (slot.key.load() == 0 || slot.sent.load() <= slot.entered.load()) continue;
        std::fprintf(stderr, "[raise]   reading the x86 state of '%s' (%s)\n", slot.name, Describe(slot.port).c_str());
        x86_thread_state64_t state{};
        mach_msg_type_number_t count = x86_THREAD_STATE64_COUNT;
        const kern_return_t result = thread_get_state(slot.port, x86_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state), &count);
        if (result != KERN_SUCCESS) {
            std::fprintf(stderr, "[raise]   thread_get_state failed: %d\n", result);
            continue;
        }
        std::fprintf(stderr, "[raise]   rip 0x%llx rsp 0x%llx rbp 0x%llx rax 0x%llx\n", static_cast<unsigned long long>(state.__rip), static_cast<unsigned long long>(state.__rsp),
                     static_cast<unsigned long long>(state.__rbp), static_cast<unsigned long long>(state.__rax));
    }
}
#endif

#ifndef _WIN32
std::atomic<std::uint64_t> probesDelivered{0};

void ProbeHandler(int) {
    probesDelivered.fetch_add(1, std::memory_order_relaxed);
}

void Watch() {
    pthread_setname_np("aps5.raise-watch");
    struct sigaction action{};
    action.sa_handler = ProbeHandler;
    action.sa_flags = SA_RESTART;
    sigemptyset(&action.sa_mask);
    sigaction(SIGUSR2, &action, nullptr);
    std::atomic<bool> probeReady{false};
    pthread_t probe{};
    std::thread([&probe, &probeReady] {
        pthread_setname_np("aps5.raise-probe");
        sigset_t unblocked;
        sigemptyset(&unblocked);
        sigaddset(&unblocked, SIGUSR2);
        pthread_sigmask(SIG_UNBLOCK, &unblocked, nullptr);
        probe = pthread_self();
        probeReady.store(true);
        for (;;) pause();
    }).detach();
    while (!probeReady.load()) usleep(1000);
    std::uint64_t probesSent = 0;
    std::uint64_t lastDelivered = 0;
    std::uint64_t lastDeliveryNanos = Now();
    bool probeStalled = false;
    bool serverBusy = false;
    bool dumped = false;
    unsigned tick = 0;
    for (;;) {
        usleep(100000);
        ++tick;
        const std::uint64_t now = Now();
        const std::uint64_t delivered = probesDelivered.load();
        if (delivered != lastDelivered) {
            if (probeStalled) std::fprintf(stderr, "[raise] %.3f s host signal probe delivered again after %.3f s (%llu sent, %llu delivered)\n", Seconds(now), static_cast<double>(now - lastDeliveryNanos) / 1e9,
                                           static_cast<unsigned long long>(probesSent), static_cast<unsigned long long>(delivered));
            probeStalled = false;
            lastDelivered = delivered;
            lastDeliveryNanos = now;
        } else if (!probeStalled && probesSent > delivered && now - lastDeliveryNanos > 500000000ULL) {
            probeStalled = true;
            std::fprintf(stderr, "[raise] %.3f s host signal probe undelivered for %.3f s (%llu sent, %llu delivered)\n", Seconds(now), static_cast<double>(now - lastDeliveryNanos) / 1e9,
                         static_cast<unsigned long long>(probesSent), static_cast<unsigned long long>(delivered));
        }
        pthread_kill(probe, SIGUSR2);
        ++probesSent;
#ifdef __APPLE__
        if (tick % 5 == 0) {
            static HostThread threads[1024];
            const int count = ReadThreads(threads, 1024);
            double serverCpu = -1;
            for (int i = 0; i < count; ++i)
                if (std::strstr(threads[i].name, "exceptionserver") != nullptr) serverCpu = threads[i].cpu;
            const bool busy = serverCpu >= 50.0;
            if (busy != serverBusy) {
                std::fprintf(stderr, "[raise] %.3f s rosetta exception server %s (cpu %.1f%%)\n", Seconds(now), busy ? "busy" : "idle again", serverCpu);
                serverBusy = busy;
                if (busy) {
                    for (int i = 0; i < count; ++i)
                        if (threads[i].cpu >= 5.0 || threads[i].runState == TH_STATE_RUNNING || threads[i].runState == TH_STATE_UNINTERRUPTIBLE)
                            std::fprintf(stderr, "[raise]   active tid %llu '%s' run_state %d suspend %d cpu %.1f%%\n", static_cast<unsigned long long>(threads[i].id), threads[i].name, threads[i].runState, threads[i].suspend, threads[i].cpu);
                }
            }
        }
        bool missed = false;
        for (auto& slot : slots) {
            if (slot.key.load() == 0) continue;
            const auto sent = slot.sent.load();
            const auto sentNanos = slot.sentNanos.load();
            if (!Unhandled(sent, slot.entered.load(), slot.reported.load(), sentNanos, now)) continue;
            slot.reported.store(sent);
            missed = true;
            std::fprintf(stderr, "[raise] %.3f s no handler on '%s' %.3f s after signal %u (entered %u, returned %u): %s\n", Seconds(now), slot.name, static_cast<double>(now - sentNanos) / 1e9, sent,
                         slot.entered.load(), slot.returned.load(), Describe(slot.port).c_str());
        }
        if (missed && !dumped) {
            dumped = true;
            std::fprintf(stderr, "[raise] host signal probe: %llu sent, %llu delivered, last delivery %.3f s ago\n", static_cast<unsigned long long>(probesSent), static_cast<unsigned long long>(probesDelivered.load()),
                         static_cast<double>(now - lastDeliveryNanos) / 1e9);
            DumpThreads("first raise without a handler");
            DumpRaisedState();
        }
#endif
    }
}
#endif

}

bool Enabled() {
    static const bool enabled = std::getenv("APS5_TRACE_RAISE") != nullptr;
    return enabled;
}

void Start() {
#ifndef _WIN32
    if (!Enabled()) return;
    static const bool started = [] {
        std::thread(Watch).detach();
        return true;
    }();
    (void)started;
#endif
}

bool Unhandled(std::uint32_t sent, std::uint32_t entered, std::uint32_t reported, std::uint64_t sentNanos, std::uint64_t nowNanos) {
    if (sent <= entered || reported == sent || sentNanos > nowNanos) return false;
    return nowNanos - sentNanos >= UnhandledGraceNanos;
}

void Raising(Pthread thread) {
    if (!Enabled()) return;
    Claim(thread);
}

void Sent(Pthread thread, const char* name, const char* how) {
    const bool sent = how != nullptr;
    if (!Enabled()) return;
    Slot* slot = Claim(thread);
    std::string state = "untracked";
    if (slot != nullptr) {
#ifdef __APPLE__
        slot->port = thread->hostMachThread;
        state = Describe(slot->port);
#endif
        std::snprintf(slot->name, sizeof(slot->name), "%s", name);
        if (sent) {
            slot->sentNanos.store(Now());
            slot->sent.fetch_add(1);
        }
    }
    std::fprintf(stderr, "[raise] %.3f s exception for guest thread %p '%s' %s; %s\n", Seconds(Now()), static_cast<void*>(thread), name, sent ? how : "not raised, the thread has finished", state.c_str());
}

void Entered(Pthread self) {
    if (Slot* slot = Find(self)) slot->entered.fetch_add(1);
}

void Returned(Pthread self) {
    if (Slot* slot = Find(self)) slot->returned.fetch_add(1);
}

}
