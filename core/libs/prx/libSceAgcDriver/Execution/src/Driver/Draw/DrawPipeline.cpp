#include <pthread.h>
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PresentTrace.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <sys/sysctl.h>
#else
#include <unistd.h>
#endif

namespace AgcDriver::DriverDetail {

namespace {

constexpr std::array<const char*, static_cast<std::size_t>(DrawPipeline::DrainReason::Count)> DrainNames{"packet", "flush", "labels", "capture", "indirect", "submission", "inputs"};
constexpr std::array<const char*, static_cast<std::size_t>(DrawPipeline::Kind::Count)> KindNames{"draw", "fill", "label", "dma", "completion", "submit"};

std::atomic<bool> orderedSuspended{false};

// The process's memory charged to it by the OS (macOS: phys_footprint, which counts Metal and
// IOAccelerator memory as well as the heap; Windows: private bytes; elsewhere: resident pages).
std::uint64_t processFootprint() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) return 0;
    return counters.PrivateUsage;
#elif defined(__APPLE__)
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) return 0;
    return info.phys_footprint;
#else
    long pages = 0, resident = 0;
    if (FILE* statm = std::fopen("/proc/self/statm", "r")) {
        if (std::fscanf(statm, "%ld %ld", &pages, &resident) != 2) resident = 0;
        std::fclose(statm);
    }
    return static_cast<std::uint64_t>(resident) * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
#endif
}

// Bytes the host heap has handed out (macOS: every malloc zone); 0 where it is not known.
std::uint64_t heapInUse() {
#if defined(__APPLE__)
    malloc_statistics_t stats{};
    malloc_zone_statistics(nullptr, &stats);
    return stats.size_in_use;
#else
    return 0;
#endif
}

std::uint64_t physicalMemory() {
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    return GlobalMemoryStatusEx(&status) ? status.ullTotalPhys : 0;
#elif defined(__APPLE__)
    std::uint64_t bytes = 0;
    std::size_t size = sizeof(bytes);
    return sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) == 0 ? bytes : 0;
#else
    return static_cast<std::uint64_t>(sysconf(_SC_PHYS_PAGES)) * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
#endif
}

// APS5_PIPELINE_FOOTPRINT_LIMIT_MB: the footprint above which ordered items stop (0 never stops
// them); three quarters of physical memory by default.
std::uint64_t footprintLimit() {
    static const std::uint64_t limit = [] {
        if (const char* text = std::getenv("APS5_PIPELINE_FOOTPRINT_LIMIT_MB")) return static_cast<std::uint64_t>(std::strtoull(text, nullptr, 10)) << 20u;
        return physicalMemory() / 4 * 3;
    }();
    return limit;
}

std::uint64_t elapsedNs(std::chrono::steady_clock::time_point since) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - since).count());
}

// APS5_PIPELINE_SPIN_US: how long the idle committer polls for the next item before it sleeps (50 us).
std::chrono::microseconds spinBeforeSleep() {
    static const auto spin = [] {
        const char* text = std::getenv("APS5_PIPELINE_SPIN_US");
        return std::chrono::microseconds(text != nullptr ? std::strtoull(text, nullptr, 10) : 50);
    }();
    return spin;
}

void cpuRelax() {
#if defined(__aarch64__)
    __builtin_arm_isb(15);
#elif defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
#endif
}

// The worker and the committer hold the mutex for a few hundred nanoseconds at a time, but a
// contended std::mutex waits in the kernel (__psynch_mutexwait on macOS): a short spin takes most
// collisions without the round trip.
std::unique_lock<std::mutex> acquire(std::mutex& mutex) {
    for (int spin = 0; spin < 128; ++spin) {
        if (mutex.try_lock()) return std::unique_lock<std::mutex>(mutex, std::adopt_lock);
        cpuRelax();
    }
    return std::unique_lock<std::mutex>(mutex);
}

}

DrawPipeline& DrawPipeline::Queue0() {
    static DrawPipeline* pipeline = new DrawPipeline();
    return *pipeline;
}

std::size_t DrawPipeline::Depth() {
    static const std::size_t depth = [] {
        const char* text = std::getenv("APS5_PIPELINED_DRAWS");
        if (std::getenv("APS5_LOCKED_DRAW_PREPARE") != nullptr || std::getenv("APS5_DRAW_DRAIN") != nullptr || std::getenv("APS5_DRAIN_ALL") != nullptr || std::getenv("APS5_NO_WORDWISE_CAPTURE") != nullptr) return std::size_t{0};
        if (text == nullptr) return std::size_t{64};
        const auto value = std::strtoull(text, nullptr, 10);
        return value == 0 ? std::size_t{0} : value == 1 ? std::size_t{64} : static_cast<std::size_t>(std::min<unsigned long long>(value, 256));
    }();
    return depth;
}

bool DrawPipeline::Ordered(const char* drainVariable) {
    static const bool orderedItems = std::getenv("APS5_PIPELINE_ORDERED_ITEMS") != nullptr;
    return orderedItems && std::getenv(drainVariable) == nullptr;
}

bool DrawPipeline::OrderedSuspended() {
    return orderedSuspended.load(std::memory_order_relaxed);
}

void DrawPipeline::checkFootprint(std::chrono::steady_clock::time_point now) {
    lastFootprintCheck = now;
    if (orderedSuspended.load(std::memory_order_relaxed) || std::getenv("APS5_PIPELINE_ORDERED_ITEMS") == nullptr) return;
    const auto limit = footprintLimit();
    if (limit == 0) return;
    const auto footprint = processFootprint();
    if (footprint < limit) return;
    orderedSuspended.store(true, std::memory_order_relaxed);
    std::fprintf(stderr, "[draw] pipelined: the process footprint reached %.0f MiB (heap %.0f MiB), over APS5_PIPELINE_FOOTPRINT_LIMIT_MB=%.0f; labels, fills and submission ends drain the pipeline from now on\n", static_cast<double>(footprint) / 1048576.0, static_cast<double>(heapInUse()) / 1048576.0, static_cast<double>(limit) / 1048576.0);
}

bool& DrawPipeline::Active() {
    static thread_local bool active = false;
    return active;
}

std::atomic<std::uint64_t>& DrawPipeline::EpochToken() {
    static std::atomic<std::uint64_t> token{1};
    return token;
}

void DrawPipeline::FollowEpoch(std::uint64_t token) {
    static thread_local std::uint64_t followed = 0;
    if (followed == token) return;
    followed = token;
    GuestMemory::BumpCollectEpoch();
}

void DrawPipeline::rethrowFailure() {
    if (failure == nullptr) return;
    auto error = std::exchange(failure, nullptr);
    std::rethrow_exception(error);
}

void DrawPipeline::Enqueue(Commit commit, std::vector<Range> writes, std::uint64_t labelAddress, std::vector<std::byte> labelBytes, Kind kind) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    auto lock = acquire(mutex);
    rethrowFailure();
    if (items.size() >= Depth()) {
        // The committer takes the GPU lock for every item: waiting for room while holding it never ends.
        if (GuestMemory::GpuMutex().HeldByThisThread()) throw std::runtime_error("draw pipeline enqueue into a full queue under the GPU lock");
        const auto start = std::chrono::steady_clock::now();
        ++idleWaiters;
        idle.wait(lock, [&] { return items.size() < Depth(); });
        --idleWaiters;
        fullWaitNs += elapsedNs(start);
        rethrowFailure();
    }
    depthSum += items.size();
    ++enqueued;
    for (const auto& [begin, limit] : writes) {
        if (begin < writesBegin.load(std::memory_order_relaxed)) writesBegin.store(begin, std::memory_order_release);
        if (limit > writesEnd.load(std::memory_order_relaxed)) writesEnd.store(limit, std::memory_order_release);
    }
    items.push_back({std::move(commit), std::move(writes), labelAddress, std::move(labelBytes), kind, EpochToken().load(std::memory_order_relaxed)});
    outstanding.fetch_add(1, std::memory_order_release);
    if (!started) {
        // A 64 MiB stack: a std::thread gets 512 KiB on macOS, too little for a draw commit.
        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        pthread_attr_setstacksize(&attributes, 64u << 20u);
        pthread_t committer;
        const int created = pthread_create(&committer, &attributes, [](void* pipeline) -> void* {
            Graphics::MarkVkCommitterThread();
            static_cast<DrawPipeline*>(pipeline)->run();
            return nullptr;
        }, this);
        pthread_attr_destroy(&attributes);
        if (created != 0) throw std::runtime_error("draw pipeline: cannot start the committer thread");
        pthread_detach(committer);
        started = true;
    }
    // Notified after the unlock: woken under it, the committer would only wait again for the mutex.
    const bool wakeCommitter = committerWaiting;
    if (profile || enqueued % 256 == 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now - lastFootprintCheck >= std::chrono::seconds(1)) checkFootprint(now);
        if (profile && now - lastReport >= std::chrono::seconds(10)) report(now);
    }
    lock.unlock();
    if (wakeCommitter) wake.notify_one();
}

void DrawPipeline::report(std::chrono::steady_clock::time_point now) {
    lastReport = now;
    std::string drainText;
    for (std::size_t i = 0; i < drains.size(); ++i) {
        char item[64];
        std::snprintf(item, sizeof(item), " %s %llu (%.1f ms)", DrainNames[i], static_cast<unsigned long long>(drains[i]), static_cast<double>(drainWaitNs[i]) / 1e6);
        drainText += item;
    }
    drainText += "; packet drains by opcode:";
    for (int shown = 0; shown < 5; ++shown) {
        const auto top = std::max_element(drainOpcodes.begin(), drainOpcodes.begin() + 0x100);
        if (*top == 0) break;
        char item[32];
        std::snprintf(item, sizeof(item), " 0x%02x x%llu", static_cast<unsigned>(top - drainOpcodes.begin()), static_cast<unsigned long long>(*top));
        drainText += item;
        *top = 0;
    }
    drainOpcodes = {};
    const auto perCommit = commits == 0 ? 0.0 : static_cast<double>(commitNs) / 1e3 / static_cast<double>(commits);
    const auto perDraw = enqueued == 0 ? 0.0 : static_cast<double>(fullWaitNs) / 1e3 / static_cast<double>(enqueued);
    const auto queued = enqueued == 0 ? 0.0 : static_cast<double>(depthSum) / static_cast<double>(enqueued);
    std::string kindText;
    for (std::size_t i = 0; i < kindCommits.size(); ++i) {
        if (kindCommits[i] == 0) continue;
        char item[64];
        std::snprintf(item, sizeof(item), " %s %llu/%.1f us", KindNames[i], static_cast<unsigned long long>(kindCommits[i]), static_cast<double>(kindNs[i]) / 1e3 / static_cast<double>(kindCommits[i]));
        kindText += item;
    }
    std::fprintf(stderr, "[draw] pipelined (10 s, depth %zu): %llu commits at %.1f us each on the committer, %llu failed; by kind (count/us each):%s; %llu enqueued behind %.2f on average, waiting %.1f us per draw for room; drains:%s; footprint %.0f MiB, heap %.0f MiB%s\n", Depth(), static_cast<unsigned long long>(commits), perCommit, static_cast<unsigned long long>(commitErrors), kindText.c_str(), static_cast<unsigned long long>(enqueued), queued, perDraw, drainText.c_str(), static_cast<double>(processFootprint()) / 1048576.0, static_cast<double>(heapInUse()) / 1048576.0, OrderedSuspended() ? " (ordered items suspended)" : "");
    commits = commitNs = commitErrors = enqueued = fullWaitNs = depthSum = 0;
    kindCommits = {};
    kindNs = {};
    drains = {};
    drainWaitNs = {};
}

void DrawPipeline::run() {
    for (;;) {
        Commit* commit = nullptr;
        Kind kind = Kind::Draw;
        std::uint64_t epoch = 0;
        if (const auto spin = spinBeforeSleep(); spin.count() != 0 && outstanding.load(std::memory_order_acquire) == 0) {
            const auto until = std::chrono::steady_clock::now() + spin;
            for (std::uint32_t polls = 1; outstanding.load(std::memory_order_acquire) == 0; ++polls) {
                cpuRelax();
                if ((polls & 63u) == 0 && std::chrono::steady_clock::now() >= until) break;
            }
        }
        {
            auto lock = acquire(mutex);
            committerWaiting = true;
            wake.wait(lock, [&] { return !items.empty(); });
            committerWaiting = false;
            commit = &items.front().commit;
            kind = items.front().kind;
            epoch = items.front().epoch;
        }
        // Every item, not only draws, runs in the collect epoch the worker was in when it queued it.
        FollowEpoch(epoch);
        const auto start = std::chrono::steady_clock::now();
        std::exception_ptr fatal;
        bool failed = false;
        Graphics::PresentTrace::Committing() = true;
        try {
            (*commit)();
        } catch (const std::exception& error) {
            failed = true;
            fatal = std::current_exception();
            static std::atomic<std::uint64_t> reported{0};
            if (reported.fetch_add(1, std::memory_order_relaxed) < 20) std::fprintf(stderr, "[draw] pipelined commit failed: %.200s\n", error.what());
        } catch (...) {
            fatal = std::current_exception();
        }
        Graphics::PresentTrace::Committing() = false;
        const auto spent = elapsedNs(start);
        auto lock = acquire(mutex);
        items.pop_front();
        if (items.empty()) {
            writesBegin.store(std::numeric_limits<std::uint64_t>::max(), std::memory_order_release);
            writesEnd.store(0, std::memory_order_release);
        }
        ++commits;
        commitNs += spent;
        ++kindCommits[static_cast<std::size_t>(kind)];
        kindNs[static_cast<std::size_t>(kind)] += spent;
        if (failed) ++commitErrors;
        if (fatal != nullptr && failure == nullptr) failure = fatal;
        outstanding.fetch_sub(1, std::memory_order_release);
        const bool wakeIdle = idleWaiters != 0;
        lock.unlock();
        if (wakeIdle) idle.notify_all();
    }
}

void DrawPipeline::Drain(DrainReason reason, std::uint32_t opcode) {
    if (!Busy()) return;
    if (GuestMemory::GpuMutex().HeldByThisThread()) throw std::runtime_error("draw pipeline drain under the GPU lock (reason " + std::to_string(static_cast<unsigned>(reason)) + ", opcode " + std::to_string(opcode) + ")");
    const auto start = std::chrono::steady_clock::now();
    std::unique_lock lock(mutex);
    ++idleWaiters;
    idle.wait(lock, [&] { return items.empty(); });
    --idleWaiters;
    const auto index = static_cast<std::size_t>(reason);
    ++drains[index];
    ++drainOpcodes[std::min<std::uint32_t>(opcode, 0x100)];
    drainWaitNs[index] += elapsedNs(start);
    rethrowFailure();
}

std::optional<std::uint64_t> DrawPipeline::PendingLabel(std::uint64_t address, std::size_t bytes) {
    if (bytes == 0 || bytes > 8) return std::nullopt;
    const auto end = address + bytes;
    std::lock_guard lock(mutex);
    for (auto item = items.rbegin(); item != items.rend(); ++item) {
        const bool overlaps = std::any_of(item->writes.begin(), item->writes.end(), [&](const Range& range) { return address < range.second && range.first < end; });
        if (!overlaps) continue;
        if (item->labelBytes.empty() || address < item->labelAddress || end > item->labelAddress + item->labelBytes.size()) return std::nullopt;
        std::uint64_t value = 0;
        std::memcpy(&value, item->labelBytes.data() + (address - item->labelAddress), bytes);
        return value;
    }
    return std::nullopt;
}

bool DrawPipeline::Overlaps(std::uint64_t address, std::size_t bytes) {
    const auto end = address + bytes;
    if (address >= writesEnd.load(std::memory_order_acquire) || end <= writesBegin.load(std::memory_order_acquire)) return false;
    const auto lock = acquire(mutex);
    for (const auto& item : items) {
        for (const auto& [begin, limit] : item.writes) {
            if (address < limit && begin < end) return true;
        }
    }
    return false;
}

}
