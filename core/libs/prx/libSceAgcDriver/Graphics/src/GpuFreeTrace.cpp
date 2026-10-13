#include "prx/libSceAgcDriver/Graphics/include/GpuFreeTrace.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <string_view>
#include <type_traits>
#ifndef _WIN32
#include <dlfcn.h>
#include <execinfo.h>
#endif

namespace AgcDriver::Graphics {

namespace {

enum class FreeKind : std::uint8_t { Memory, Buffer, Image, ImageView, BufferView };

const char* kindName(FreeKind kind) {
    switch (kind) {
    case FreeKind::Memory: return "vkFreeMemory";
    case FreeKind::Buffer: return "vkDestroyBuffer";
    case FreeKind::Image: return "vkDestroyImage";
    case FreeKind::ImageView: return "vkDestroyImageView";
    case FreeKind::BufferView: return "vkDestroyBufferView";
    }
    return "?";
}

constexpr std::size_t Frames = 6;

struct FreeNote {
    FreeKind kind = FreeKind::Memory;
    std::uint64_t handle = 0;
    std::chrono::steady_clock::time_point at;
    std::array<void*, Frames> frames{};
    int depth = 0;
};

struct Deferred {
    FreeKind kind;
    VkDevice device;
    std::uint64_t handle;
    std::chrono::steady_clock::time_point due;
    const Recorder* recorder = nullptr;
    std::uint64_t serial = 0;
};

struct FreeState {
    PFN_vkGetDeviceProcAddr real = nullptr;
    PFN_vkFreeMemory freeMemory = nullptr;
    PFN_vkDestroyBuffer destroyBuffer = nullptr;
    PFN_vkDestroyImage destroyImage = nullptr;
    PFN_vkDestroyImageView destroyImageView = nullptr;
    PFN_vkDestroyBufferView destroyBufferView = nullptr;
    PFN_vkDestroyDevice destroyDevice = nullptr;
    std::mutex mutex;
    std::deque<Deferred> deferred;
    std::array<FreeNote, 256> ring{};
    std::uint64_t count = 0;
    std::uint64_t deferredTotal = 0;
    std::atomic<bool> reported{false};
};

FreeState& State() {
    static FreeState& state = *new FreeState();
    return state;
}

bool Traced() {
    static const bool traced = std::getenv("APS5_TRACE_GPU_FREES") != nullptr;
    return traced;
}

constexpr std::string_view BatchPrefix = "batches:";

std::chrono::milliseconds DeferBy() {
    static const std::chrono::milliseconds value{[] {
        const char* text = std::getenv("APS5_DEFER_GPU_FREES");
        if (text == nullptr || std::string_view(text).starts_with(BatchPrefix)) return 0ull;
        return static_cast<unsigned long long>(std::strtoull(text, nullptr, 10));
    }()};
    return value;
}

int DeferBatches() {
    static const int value = [] {
        const char* text = std::getenv("APS5_DEFER_GPU_FREES");
        if (text == nullptr || !std::string_view(text).starts_with(BatchPrefix)) return -1;
        return static_cast<int>(std::strtol(text + BatchPrefix.size(), nullptr, 10));
    }();
    return value;
}

bool Deferring() {
    return DeferBy().count() != 0 || DeferBatches() >= 0;
}

bool Due(const Deferred& entry, std::chrono::steady_clock::time_point now) {
    if (DeferBatches() < 0) return entry.due <= now;
    const auto* recorder = Recorder::Active();
    return entry.recorder == nullptr || recorder != entry.recorder || recorder->BatchFinished(entry.serial);
}

void release(FreeState& state, const Deferred& entry) {
    switch (entry.kind) {
    case FreeKind::Memory: state.freeMemory(entry.device, reinterpret_cast<VkDeviceMemory>(entry.handle), nullptr); break;
    case FreeKind::Buffer: state.destroyBuffer(entry.device, reinterpret_cast<VkBuffer>(entry.handle), nullptr); break;
    case FreeKind::Image: state.destroyImage(entry.device, reinterpret_cast<VkImage>(entry.handle), nullptr); break;
    case FreeKind::ImageView: state.destroyImageView(entry.device, reinterpret_cast<VkImageView>(entry.handle), nullptr); break;
    case FreeKind::BufferView: state.destroyBufferView(entry.device, reinterpret_cast<VkBufferView>(entry.handle), nullptr); break;
    }
}

void handle(FreeKind kind, VkDevice device, std::uint64_t object) {
    if (object == 0) return;
    auto& state = State();
    const auto now = std::chrono::steady_clock::now();
    FreeNote note;
    if (Traced()) {
        note.kind = kind;
        note.handle = object;
        note.at = now;
#ifndef _WIN32
        void* frames[Frames + 2];
        const int depth = backtrace(frames, static_cast<int>(Frames + 2));
        for (int i = 2; i < depth; ++i) note.frames[static_cast<std::size_t>(i - 2)] = frames[i];
        note.depth = depth > 2 ? depth - 2 : 0;
#endif
    }
    if (!Deferring()) {
        if (Traced()) {
            std::lock_guard lock(state.mutex);
            state.ring[state.count % state.ring.size()] = note;
            ++state.count;
        }
        release(state, {kind, device, object, now});
        return;
    }
    std::lock_guard lock(state.mutex);
    if (Traced()) {
        state.ring[state.count % state.ring.size()] = note;
        ++state.count;
    }
    Deferred entry{kind, device, object, now + DeferBy()};
    if (DeferBatches() >= 0) {
        if (const auto* recorder = Recorder::Active(); recorder != nullptr) {
            entry.recorder = recorder;
            entry.serial = recorder->NewestSubmitted() + 1u + static_cast<std::uint64_t>(DeferBatches());
        }
    }
    state.deferred.push_back(entry);
    ++state.deferredTotal;
    while (!state.deferred.empty() && Due(state.deferred.front(), now)) {
        release(state, state.deferred.front());
        state.deferred.pop_front();
    }
}

VKAPI_ATTR void VKAPI_CALL guardedFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks*) {
    handle(FreeKind::Memory, device, reinterpret_cast<std::uint64_t>(memory));
}

VKAPI_ATTR void VKAPI_CALL guardedDestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks*) {
    handle(FreeKind::Buffer, device, reinterpret_cast<std::uint64_t>(buffer));
}

VKAPI_ATTR void VKAPI_CALL guardedDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks*) {
    handle(FreeKind::Image, device, reinterpret_cast<std::uint64_t>(image));
}

VKAPI_ATTR void VKAPI_CALL guardedDestroyImageView(VkDevice device, VkImageView view, const VkAllocationCallbacks*) {
    handle(FreeKind::ImageView, device, reinterpret_cast<std::uint64_t>(view));
}

VKAPI_ATTR void VKAPI_CALL guardedDestroyBufferView(VkDevice device, VkBufferView view, const VkAllocationCallbacks*) {
    handle(FreeKind::BufferView, device, reinterpret_cast<std::uint64_t>(view));
}

VKAPI_ATTR void VKAPI_CALL guardedDestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator) {
    auto& state = State();
    {
        std::lock_guard lock(state.mutex);
        std::deque<Deferred> kept;
        for (const auto& entry : state.deferred) {
            if (entry.device == device) release(state, entry);
            else kept.push_back(entry);
        }
        state.deferred.swap(kept);
    }
    state.destroyDevice(device, allocator);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL guardedDeviceProc(VkDevice device, const char* name) {
    auto& state = State();
    const auto real = state.real(device, name);
    if (real == nullptr || name == nullptr) return real;
    const auto swap = [&](auto& slot, auto guarded) {
        slot = reinterpret_cast<std::remove_reference_t<decltype(slot)>>(real);
        return reinterpret_cast<PFN_vkVoidFunction>(guarded);
    };
    if (std::strcmp(name, "vkFreeMemory") == 0) return swap(state.freeMemory, &guardedFreeMemory);
    if (std::strcmp(name, "vkDestroyBuffer") == 0) return swap(state.destroyBuffer, &guardedDestroyBuffer);
    if (std::strcmp(name, "vkDestroyImage") == 0) return swap(state.destroyImage, &guardedDestroyImage);
    if (std::strcmp(name, "vkDestroyImageView") == 0) return swap(state.destroyImageView, &guardedDestroyImageView);
    if (std::strcmp(name, "vkDestroyBufferView") == 0) return swap(state.destroyBufferView, &guardedDestroyBufferView);
    if (std::strcmp(name, "vkDestroyDevice") == 0) return swap(state.destroyDevice, &guardedDestroyDevice);
    return real;
}

void printFrame(void* frame) {
#ifndef _WIN32
    Dl_info info{};
    if (dladdr(frame, &info) != 0 && info.dli_fname != nullptr) {
        const char* slash = std::strrchr(info.dli_fname, '/');
        std::fprintf(stderr, " %s+0x%llx", slash != nullptr ? slash + 1 : info.dli_fname, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(frame) - reinterpret_cast<std::uintptr_t>(info.dli_fbase)));
        return;
    }
#endif
    std::fprintf(stderr, " %p", frame);
}

}

PFN_vkGetDeviceProcAddr GuardDeviceProc(PFN_vkGetDeviceProcAddr real) {
    if (!Traced() && !Deferring()) return real;
    auto& state = State();
    state.real = real;
    if (Traced()) std::fprintf(stderr, "[gpu-free] APS5_TRACE_GPU_FREES: Vulkan frees are recorded and the last 256 printed when the device is lost\n");
    if (DeferBatches() >= 0) std::fprintf(stderr, "[gpu-free] APS5_DEFER_GPU_FREES: Vulkan memory, buffers, images and views are freed once the batch open when the driver releases them, and %d more, completed\n", DeferBatches());
    if (DeferBy().count() != 0) std::fprintf(stderr, "[gpu-free] APS5_DEFER_GPU_FREES: Vulkan memory, buffers, images and views are freed %lld ms after the driver releases them\n", static_cast<long long>(DeferBy().count()));
    return &guardedDeviceProc;
}

void ReportGpuFrees(const char* why) {
    if (!Traced()) return;
    auto& state = State();
    if (state.reported.exchange(true)) return;
    std::unique_lock lock(state.mutex, std::defer_lock);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (!lock.try_lock()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            std::fprintf(stderr, "[gpu-free] %s: the free record stayed locked for 500 ms; printing it unlocked\n", why);
            break;
        }
        std::this_thread::yield();
    }
    const auto ring = state.ring;
    const auto count = state.count;
    const auto deferredTotal = state.deferredTotal;
    if (lock.owns_lock()) lock.unlock();
    const auto now = std::chrono::steady_clock::now();
    const auto shown = std::min<std::uint64_t>(count, ring.size());
    std::fprintf(stderr, "[gpu-free] %s: %llu Vulkan frees and destroys (%llu deferred), the last %llu oldest first, with the callers' module offsets\n", why, static_cast<unsigned long long>(count), static_cast<unsigned long long>(deferredTotal), static_cast<unsigned long long>(shown));
    for (std::uint64_t n = count - shown; n < count; ++n) {
        const auto& note = ring[n % ring.size()];
        std::fprintf(stderr, "[gpu-free]   -%.1f ms %s 0x%llx from", std::chrono::duration<double, std::milli>(now - note.at).count(), kindName(note.kind), static_cast<unsigned long long>(note.handle));
        for (int i = 0; i < note.depth; ++i) printFrame(note.frames[static_cast<std::size_t>(i)]);
        std::fprintf(stderr, "\n");
    }
}

}
