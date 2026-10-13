#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include <array>
#include <chrono>
#include <cstdio>
#include <exception>
#include <vector>
#include <cstdlib>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>

namespace AgcDriver::DriverDetail {

namespace {

struct RecentWork {
    const char* kind = nullptr;
    std::uint32_t queue = 0;
    std::uint64_t serial = 0;
    std::size_t offset = 0;
    std::uint64_t first = 0;
    std::uint64_t pixel = 0;
    std::uint64_t compute = 0;
    std::uint64_t color = 0;
    std::uint64_t index = 0;
    std::array<std::uint32_t, 6> words{};
    std::chrono::steady_clock::time_point at;
};

std::size_t RecentWorkCapacity() {
    static const std::size_t capacity = [] {
        const char* value = std::getenv("APS5_TRACE_RECENT_WORK");
        return value != nullptr ? static_cast<std::size_t>(std::strtoull(value, nullptr, 10)) : std::size_t{0};
    }();
    return capacity;
}

std::mutex& RecentWorkMutex() {
    static auto* mutex = new std::mutex();
    return *mutex;
}

std::vector<RecentWork>& RecentWorkRing() {
    static auto* ring = new std::vector<RecentWork>();
    return *ring;
}

std::uint64_t& RecentWorkCount() {
    static std::uint64_t count = 0;
    return count;
}

std::uint64_t ProgramAt(const Registers& registers, std::uint32_t low) {
    const auto lo = registers.find(low);
    const auto hi = registers.find(low + 1u);
    if (lo == registers.end()) return 0;
    return (static_cast<std::uint64_t>(lo->second) << 8u) | (hi == registers.end() ? 0u : static_cast<std::uint64_t>(hi->second & 0xffu) << 40u);
}

std::terminate_handler& PreviousTerminate() {
    static std::terminate_handler previous = nullptr;
    return previous;
}

}

bool SerializeWork() {
    static const bool serialize = [] {
        const char* value = std::getenv("APS5_SERIALIZE_WORK");
        return value != nullptr && std::string_view(value) == "1";
    }();
    return serialize;
}

bool RecentWorkTraced() {
    return RecentWorkCapacity() != 0;
}

void NoteRecentWork(const char* kind, std::uint32_t queue, std::uint64_t serial, std::size_t offset, const QueueState& state, std::span<const std::uint32_t> packet) {
    const auto capacity = RecentWorkCapacity();
    if (capacity == 0) return;
    RecentWork work{kind, queue, serial, offset};
    work.first = ProgramAt(state.shader, 0xc8);
    work.pixel = ProgramAt(state.shader, 0x08);
    work.compute = ProgramAt(state.shader, 0x20c);
    const auto colorLow = state.context.find(0x318);
    const auto colorHigh = state.context.find(0x390);
    work.color = colorLow == state.context.end() ? 0 : (static_cast<std::uint64_t>(colorLow->second) << 8u) | (colorHigh == state.context.end() ? 0u : static_cast<std::uint64_t>(colorHigh->second) << 40u);
    work.index = state.indexBase;
    for (std::size_t i = 0; i < work.words.size() && i < packet.size(); ++i) work.words[i] = packet[i];
    work.at = std::chrono::steady_clock::now();
    std::lock_guard lock(RecentWorkMutex());
    auto& ring = RecentWorkRing();
    if (ring.size() < capacity) ring.push_back(work);
    else ring[RecentWorkCount() % capacity] = work;
    ++RecentWorkCount();
}

void DumpRecentWork(const char* why) {
    const auto capacity = RecentWorkCapacity();
    if (capacity == 0) return;
    std::unique_lock lock(RecentWorkMutex(), std::try_to_lock);
    if (!lock.owns_lock()) return;
    const auto& ring = RecentWorkRing();
    const auto count = RecentWorkCount();
    const auto now = std::chrono::steady_clock::now();
    std::fprintf(stderr, "[recent-work] %s: the last %zu of %llu draws and dispatches, oldest first\n", why, ring.size(), static_cast<unsigned long long>(count));
    for (std::size_t n = 0; n < ring.size(); ++n) {
        const auto& work = ring[ring.size() < capacity ? n : (count + n) % capacity];
        std::fprintf(stderr, "[recent-work] -%.1f ms queue 0x%x serial %llu dword %zu %s first 0x%llx pixel 0x%llx compute 0x%llx color 0x%llx index 0x%llx words %08x %08x %08x %08x %08x %08x\n",
                     std::chrono::duration<double, std::milli>(now - work.at).count(), work.queue, static_cast<unsigned long long>(work.serial), work.offset, work.kind, static_cast<unsigned long long>(work.first), static_cast<unsigned long long>(work.pixel),
                     static_cast<unsigned long long>(work.compute), static_cast<unsigned long long>(work.color), static_cast<unsigned long long>(work.index), work.words[0], work.words[1], work.words[2], work.words[3], work.words[4], work.words[5]);
    }
}

void InstallRecentWorkDump() {
    if (RecentWorkCapacity() == 0 || PreviousTerminate() != nullptr) return;
    PreviousTerminate() = std::set_terminate([] {
        DumpRecentWork("terminating");
        if (const auto previous = PreviousTerminate()) previous();
        std::abort();
    });
}

bool SkipFailedWork() {
    static const bool skip = [] {
        const char* value = std::getenv("APS5_SKIP_FAILED_DRAWS");
        return value != nullptr && std::string_view(value) == "1";
    }();
    return skip;
}

void NoteSkippedWork(const char* reason, const char* kind) {
    static std::mutex mutex;
    static std::map<std::string, std::uint64_t> skipped;
    static std::uint64_t total = 0;
    std::string key = std::string(kind) + ": " + reason;
    if (const auto request = key.find("\nRecompileRequest:"); request != std::string::npos) key.resize(request);
    std::lock_guard lock(mutex);
    ++total;
    const auto count = ++skipped[key];
    if (count == 1u) {
        std::fprintf(stderr, "[draw] skipped a %s the driver cannot run (APS5_SKIP_FAILED_DRAWS, %llu skipped so far): %s\n", kind, static_cast<unsigned long long>(total), reason);
    } else if ((count & (count - 1u)) == 0u) {
        std::fprintf(stderr, "[draw] skipped %llu packets for %.300s\n", static_cast<unsigned long long>(count), key.c_str());
    }
}

double TraceMs() {
    static const auto origin = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - origin).count();
}

void require(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(std::string("AGC driver: ") + reason);
    }
}

std::uint32_t readRegister(const Registers& registers, std::uint32_t offset) {
    const auto it = registers.find(offset);
    require(it != registers.end(), "required shader register has not been written");
    return it->second;
}

std::uint32_t readUserData(const Registers& shader, std::uint32_t offset) {
    const auto it = shader.find(offset);
    return it == shader.end() ? 0u : it->second;
}

}
