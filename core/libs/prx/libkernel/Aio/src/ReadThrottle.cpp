#include "prx/libkernel/Aio/include/ReadThrottle.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>

namespace ReadThrottle {

namespace {

std::uint64_t ParseUnsigned(const char* name, const char* text) {
    char* end = nullptr;
    const auto value = std::strtoull(text, &end, 10);
    if (*text == '\0' || end == nullptr || *end != '\0') throw std::runtime_error(std::string(name) + " must be a non-negative integer");
    return value;
}

struct FlipPacing {
    std::mutex mutex;
    std::condition_variable flipped;
    std::uint64_t flips = 0;
    std::int64_t credit = 0;
    std::uint64_t heldReads = 0;
    std::uint64_t stalls = 0;
    double heldMillis = 0.0;
    double reportedAt = 0.0;
    unsigned stallMillis = FlipStallMillis;
    PacingTotals totals;
};

FlipPacing& Pacing() {
    static FlipPacing& pacing = *new FlipPacing;
    return pacing;
}

std::chrono::steady_clock::time_point Origin() {
    static const auto origin = std::chrono::steady_clock::now();
    return origin;
}

}

Settings Current() {
    static const Settings settings = [] {
        Settings result;
        if (const char* value = std::getenv("APS5_LOADER_THROTTLE_US")) result.microsPerMiB = ParseUnsigned("APS5_LOADER_THROTTLE_US", value);
        result.windowSeconds = 20.0;
        if (const char* value = std::getenv("APS5_LOADER_THROTTLE_SECONDS")) result.windowSeconds = static_cast<double>(ParseUnsigned("APS5_LOADER_THROTTLE_SECONDS", value));
        if (const char* value = std::getenv("APS5_LOADER_MIB_PER_FLIP")) result.bytesPerFlip = ParseUnsigned("APS5_LOADER_MIB_PER_FLIP", value) * 1024u * 1024u;
        static_cast<void>(Origin());
        return result;
    }();
    return settings;
}

std::uint64_t Flips() {
    auto& pacing = Pacing();
    std::lock_guard lock(pacing.mutex);
    return pacing.flips;
}

PacingTotals Totals() {
    auto& pacing = Pacing();
    std::lock_guard lock(pacing.mutex);
    auto totals = pacing.totals;
    totals.credit = pacing.credit;
    return totals;
}

void SetFlipStallMillis(unsigned millis) {
    auto& pacing = Pacing();
    std::lock_guard lock(pacing.mutex);
    pacing.stallMillis = millis;
}

namespace {

void PaceToFlips(const Settings& settings, std::uint64_t bytes) {
    auto& pacing = Pacing();
    std::unique_lock lock(pacing.mutex);
    if (pacing.flips == 0) return;
    const auto start = std::chrono::steady_clock::now();
    bool held = false;
    bool stalled = false;
    while (!FlipAdmits(settings.bytesPerFlip, pacing.credit)) {
        held = true;
        const auto flips = pacing.flips;
        ++pacing.totals.waiting;
        const bool flipped = pacing.flipped.wait_for(lock, std::chrono::milliseconds(pacing.stallMillis), [&] { return pacing.flips != flips; });
        --pacing.totals.waiting;
        if (!flipped) {
            pacing.credit = FlipCapacity(settings.bytesPerFlip);
            ++pacing.stalls;
            ++pacing.totals.stalls;
            stalled = true;
            break;
        }
    }
    if (!stalled) pacing.credit -= static_cast<std::int64_t>(bytes);
    const auto now = std::chrono::steady_clock::now();
    if (held) {
        ++pacing.totals.heldReads;
        if (!stalled) ++pacing.totals.releasedByFlip;
        ++pacing.heldReads;
        pacing.heldMillis += std::chrono::duration<double, std::milli>(now - start).count();
    }
    const double elapsed = std::chrono::duration<double>(now - Origin()).count();
    if (pacing.heldReads != 0 && elapsed - pacing.reportedAt >= 5.0) {
        std::fprintf(stderr, "[throttle] %.2f s: flip pacing held %llu reads for %.1f ms (%llu flips, %llu stalls without a flip)\n", elapsed, static_cast<unsigned long long>(pacing.heldReads), pacing.heldMillis,
                     static_cast<unsigned long long>(pacing.flips), static_cast<unsigned long long>(pacing.stalls));
        pacing.reportedAt = elapsed;
        pacing.heldReads = 0;
        pacing.heldMillis = 0.0;
        pacing.stalls = 0;
    }
}

}

void After(std::uint64_t bytes) {
    const auto settings = Current();
    if (settings.bytesPerFlip != 0) PaceToFlips(settings, bytes);
    if (settings.microsPerMiB == 0) return;
    thread_local double carry = 0.0;
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - Origin()).count();
    const auto micros = DelayMicros(settings, bytes, elapsed, carry);
    if (micros != 0) std::this_thread::sleep_for(std::chrono::microseconds(micros));
    struct Report {
        std::mutex mutex;
        std::uint64_t bytes = 0;
        std::uint64_t micros = 0;
        std::uint64_t reads = 0;
        double start = 0.0;
    };
    static Report& report = *new Report;
    std::lock_guard lock(report.mutex);
    report.bytes += bytes;
    report.micros += micros;
    ++report.reads;
    if (elapsed - report.start >= 0.25 && elapsed < settings.windowSeconds + 1.0) {
        std::fprintf(stderr, "[throttle] %.2f s: %llu reads, %.1f MiB, %.1f ms slept%s\n", elapsed, static_cast<unsigned long long>(report.reads), static_cast<double>(report.bytes) / (1024.0 * 1024.0), static_cast<double>(report.micros) / 1000.0, elapsed >= settings.windowSeconds ? " (window over)" : "");
        report.start = elapsed;
        report.bytes = 0;
        report.micros = 0;
        report.reads = 0;
    }
}

}

extern "C" void LoaderFlipCompleted_nid_no_patch() {
    const auto bytesPerFlip = ReadThrottle::Current().bytesPerFlip;
    if (bytesPerFlip == 0) return;
    auto& pacing = ReadThrottle::Pacing();
    {
        std::lock_guard lock(pacing.mutex);
        if (++pacing.flips == 1)
            std::fprintf(stderr, "[throttle] flip pacing on: %.1f MiB per completed flip, at most %llu flips of reads ahead, a held read goes on after %u ms without a flip\n",
                         static_cast<double>(bytesPerFlip) / (1024.0 * 1024.0), static_cast<unsigned long long>(ReadThrottle::FlipHeadroom), pacing.stallMillis);
        pacing.credit = ReadThrottle::FlipRefill(pacing.credit, bytesPerFlip);
    }
    pacing.flipped.notify_all();
}
