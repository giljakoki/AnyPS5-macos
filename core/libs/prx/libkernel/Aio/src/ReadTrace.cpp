#include "prx/libkernel/Aio/include/ReadTrace.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace ReadTrace {
namespace {

struct Counter {
    std::uint64_t calls = 0;
    std::uint64_t bytes = 0;
    std::uint64_t nanos = 0;
    std::uint64_t longestNanos = 0;
};

struct FileCounter {
    std::uint64_t calls = 0;
    std::uint64_t bytes = 0;
    std::uint64_t nanos = 0;
    std::uint64_t largest = 0;
};

constexpr std::size_t KindCount = 4;
constexpr std::size_t TopFiles = 6;
constexpr auto ReportInterval = std::chrono::seconds(10);

std::mutex g_mutex;
std::array<Counter, KindCount> g_counters;
std::map<std::string, FileCounter> g_files;
std::uint64_t g_largestBytes = 0;
std::string g_largestFile;
std::chrono::steady_clock::time_point g_lastReport;
std::chrono::steady_clock::time_point g_start;
bool g_started = false;

const char* KindName(Kind kind) {
    switch (kind) {
    case Kind::Aio: return "aio";
    case Kind::Pread: return "pread";
    case Kind::Read: return "read";
    case Kind::AioWait: return "aio-wait";
    }
    return "?";
}

std::string DescriptorName(int descriptor) {
#ifdef _WIN32
    return "fd " + std::to_string(descriptor);
#else
    char path[4096];
#ifdef __APPLE__
    if (::fcntl(descriptor, F_GETPATH, path) == 0) return path;
#else
    const auto link = "/proc/self/fd/" + std::to_string(descriptor);
    const auto length = ::readlink(link.c_str(), path, sizeof(path) - 1);
    if (length > 0) {
        path[length] = '\0';
        return path;
    }
#endif
    return "fd " + std::to_string(descriptor);
#endif
}

std::string ShortName(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

void Report(std::chrono::steady_clock::time_point now) {
    const double seconds = std::chrono::duration<double>(now - g_start).count();
    std::string line = "[reads] " + std::to_string(static_cast<long long>(seconds)) + " s:";
    char text[256];
    for (std::size_t index = 0; index < KindCount; ++index) {
        const auto& counter = g_counters[index];
        if (counter.calls == 0) continue;
        std::snprintf(text, sizeof(text), " %s %llu calls %.1f MiB avg %.3f ms max %.1f ms total %.2f s;", KindName(static_cast<Kind>(index)), static_cast<unsigned long long>(counter.calls), static_cast<double>(counter.bytes) / 1048576.0, static_cast<double>(counter.nanos) / 1e6 / static_cast<double>(counter.calls), static_cast<double>(counter.longestNanos) / 1e6, static_cast<double>(counter.nanos) / 1e9);
        line += text;
    }
    if (g_largestBytes != 0) {
        std::snprintf(text, sizeof(text), " largest %.2f MiB from %s;", static_cast<double>(g_largestBytes) / 1048576.0, ShortName(g_largestFile).c_str());
        line += text;
    }
    std::vector<std::pair<std::string, FileCounter>> files(g_files.begin(), g_files.end());
    std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) { return left.second.bytes > right.second.bytes; });
    for (std::size_t index = 0; index < files.size() && index < TopFiles; ++index) {
        const auto& file = files[index].second;
        std::snprintf(text, sizeof(text), " %s %llu x %.1f MiB (avg %.1f KiB, largest %.1f KiB, %.2f s)", ShortName(files[index].first).c_str(), static_cast<unsigned long long>(file.calls), static_cast<double>(file.bytes) / 1048576.0, static_cast<double>(file.bytes) / 1024.0 / static_cast<double>(file.calls), static_cast<double>(file.largest) / 1024.0, static_cast<double>(file.nanos) / 1e9);
        line += text;
        if (index + 1 < files.size() && index + 1 < TopFiles) line += ",";
    }
    std::fprintf(stderr, "%s\n", line.c_str());
    g_counters.fill(Counter{});
    g_files.clear();
    g_largestBytes = 0;
    g_largestFile.clear();
}

}

void Start(std::chrono::steady_clock::time_point now) {
    if (g_started) return;
    g_started = true;
    g_start = now;
    g_lastReport = now;
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(ReportInterval);
            const auto tick = std::chrono::steady_clock::now();
            std::lock_guard lock(g_mutex);
            if (tick - g_lastReport >= ReportInterval) {
                g_lastReport = tick;
                Report(tick);
            }
        }
    }).detach();
}

bool Enabled() {
    static const bool enabled = [] {
        const bool value = std::getenv("APS5_TRACE_READS") != nullptr;
        if (value) {
            std::lock_guard lock(g_mutex);
            Start(std::chrono::steady_clock::now());
        }
        return value;
    }();
    return enabled;
}

void Record(Kind kind, int descriptor, std::uint64_t bytes, std::uint64_t nanos) {
    if (!Enabled()) return;
    const auto now = std::chrono::steady_clock::now();
    const auto name = kind == Kind::AioWait ? std::string() : DescriptorName(descriptor);
    std::lock_guard lock(g_mutex);
    Start(now);
    auto& counter = g_counters[static_cast<std::size_t>(kind)];
    ++counter.calls;
    counter.bytes += bytes;
    counter.nanos += nanos;
    counter.longestNanos = std::max(counter.longestNanos, nanos);
    if (kind != Kind::AioWait) {
        auto& file = g_files[name];
        ++file.calls;
        file.bytes += bytes;
        file.nanos += nanos;
        file.largest = std::max(file.largest, bytes);
        if (bytes > g_largestBytes) {
            g_largestBytes = bytes;
            g_largestFile = name;
        }
    }
    if (now - g_lastReport >= ReportInterval) {
        g_lastReport = now;
        Report(now);
    }
}

}
