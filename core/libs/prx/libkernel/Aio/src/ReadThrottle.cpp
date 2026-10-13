#include "prx/libkernel/Aio/include/ReadThrottle.hpp"
#include <chrono>
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
        static_cast<void>(Origin());
        return result;
    }();
    return settings;
}

void After(std::uint64_t bytes) {
    const auto settings = Current();
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
