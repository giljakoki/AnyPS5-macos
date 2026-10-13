#include "prx/libSceAgcDriver/Graphics/include/GpuFreeTrace.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RecorderProgress.hpp"
#include "prx/libSceAgcDriver/Graphics/include/StaleHandleTrace.hpp"
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
#include <unordered_map>
#include <utility>
#include <vector>
#ifndef _WIN32
#include <dlfcn.h>
#include <execinfo.h>
#endif

namespace AgcDriver::Graphics {

namespace {

enum class FreeKind : std::uint8_t { Memory, Buffer, Image, ImageView, BufferView, QueryPool };

const char* kindName(FreeKind kind) {
    switch (kind) {
    case FreeKind::Memory: return "vkFreeMemory";
    case FreeKind::Buffer: return "vkDestroyBuffer";
    case FreeKind::Image: return "vkDestroyImage";
    case FreeKind::ImageView: return "vkDestroyImageView";
    case FreeKind::BufferView: return "vkDestroyBufferView";
    case FreeKind::QueryPool: return "vkDestroyQueryPool";
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
    PFN_vkDestroyQueryPool destroyQueryPool = nullptr;
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

std::atomic<void (*)(const char*)>& RetirementReport() {
    static std::atomic<void (*)(const char*)>& report = *new std::atomic<void (*)(const char*)>(nullptr);
    return report;
}

bool Due(const Deferred& entry, std::chrono::steady_clock::time_point now) {
    if (DeferBatches() < 0) return entry.due <= now;
    const auto* recorder = ActiveRecorderProgress();
    return entry.recorder == nullptr || recorder != entry.recorder || BatchFinished(*recorder, entry.serial);
}

// The free gate. MoltenVK makes EVERY live GPU-addressable buffer (memory allocated with
// VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, host imports included) resident in each command buffer
// whose pipelines dereference device addresses, whether or not the command buffer names it; freeing
// any such memory while one of those command buffers is in flight loses the device with
// kIOGPUCommandBufferCallbackErrorInvalidResource (scratchpad probe 'bdaunrelateddev' and
// 'bdaunrelatedimport': 64 unrelated 64 KiB BDA buffers or host imports freed while a BDA dispatch
// runs; plain memory is harmless). The driver's own lifetimes only cover the command buffers that
// use an object, so memory (and query pools, the other object Metal checks) is freed only once every
// queue submission made before the free completed: each vkQueueSubmit gets a trailing batch that
// signals a per-queue timeline semaphore, and a free waits for the values assigned so far. The value
// is assigned under the gate's mutex before the real submit and a free is decided (and made) under
// the same mutex, so a submit either sees the memory freed already or counts for the free.
// On by default on macOS; APS5_FREE_GATE=0 disables it, APS5_FREE_GATE=1 enables it elsewhere.
bool GateEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_FREE_GATE");
        if (value != nullptr) return std::string_view(value) != "0";
#ifdef __APPLE__
        return true;
#else
        return false;
#endif
    }();
    return enabled;
}

struct GateQueue {
    VkDevice device = VK_NULL_HANDLE;
    VkSemaphore timeline = VK_NULL_HANDLE;
    std::uint64_t assigned = 0;
    std::uint64_t completed = 0;
    bool tried = false;
};

struct GatedFree {
    FreeKind kind;
    VkDevice device;
    std::uint64_t handle;
    std::vector<std::pair<VkQueue, std::uint64_t>> waits;
};

// The driver entry points the gate calls for one device, resolved at its first lookup.
struct GateDevice {
    PFN_vkQueueSubmit queueSubmit = nullptr;
    PFN_vkGetDeviceQueue getDeviceQueue = nullptr;
    PFN_vkCreateSemaphore createSemaphore = nullptr;
    PFN_vkDestroySemaphore destroySemaphore = nullptr;
    PFN_vkGetSemaphoreCounterValue counterValue = nullptr;
    PFN_vkDeviceWaitIdle deviceWaitIdle = nullptr;
};

struct GateState {
    std::mutex mutex;
    std::unordered_map<VkDevice, GateDevice> devices;
    std::unordered_map<VkQueue, GateQueue> queues;
    std::deque<GatedFree> pending;
    VkDevice lastDevice = VK_NULL_HANDLE;
    std::uint64_t gated = 0;
    std::uint64_t held = 0;
    std::size_t mostPending = 0;
    std::uint64_t ungatedSubmits = 0;
};

GateState& Gate() {
    static GateState& gate = *new GateState();
    return gate;
}

void releaseNow(FreeState& state, FreeKind kind, VkDevice device, std::uint64_t handle) {
    switch (kind) {
    case FreeKind::Memory: state.freeMemory(device, reinterpret_cast<VkDeviceMemory>(handle), nullptr); break;
    case FreeKind::Buffer: state.destroyBuffer(device, reinterpret_cast<VkBuffer>(handle), nullptr); break;
    case FreeKind::Image: state.destroyImage(device, reinterpret_cast<VkImage>(handle), nullptr); break;
    case FreeKind::ImageView: state.destroyImageView(device, reinterpret_cast<VkImageView>(handle), nullptr); break;
    case FreeKind::BufferView: state.destroyBufferView(device, reinterpret_cast<VkBufferView>(handle), nullptr); break;
    case FreeKind::QueryPool: state.destroyQueryPool(device, reinterpret_cast<VkQueryPool>(handle), nullptr); break;
    }
}

// Under the gate's mutex: the queue's entry, its timeline semaphore made at its first submit (a
// device without timeline semaphores, or a failed creation, leaves the queue ungated).
GateQueue& gateQueue(GateState& gate, VkQueue queue) {
    auto& entry = gate.queues[queue];
    if (entry.device == VK_NULL_HANDLE) entry.device = gate.lastDevice;
    const auto found = gate.devices.find(entry.device);
    if (!entry.tried && found != gate.devices.end() && found->second.createSemaphore != nullptr && found->second.counterValue != nullptr) {
        entry.tried = true;
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        type.initialValue = 0;
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
        VkSemaphore made = VK_NULL_HANDLE;
        if (found->second.createSemaphore(entry.device, &info, nullptr, &made) == VK_SUCCESS) entry.timeline = made;
        else std::fprintf(stderr, "[gpu-free] free gate: no timeline semaphore for queue %p; its submits are not gated\n", static_cast<void*>(queue));
    }
    return entry;
}

// Under the gate's mutex: reads the completed values of queues with outstanding submits.
void refreshCompleted(GateState& gate, VkDevice device) {
    for (auto& [queue, entry] : gate.queues) {
        if (entry.timeline == VK_NULL_HANDLE || entry.completed >= entry.assigned) continue;
        if (device != VK_NULL_HANDLE && entry.device != device) continue;
        const auto found = gate.devices.find(entry.device);
        if (found == gate.devices.end()) continue;
        std::uint64_t value = 0;
        if (found->second.counterValue(entry.device, entry.timeline, &value) == VK_SUCCESS && value != UINT64_MAX) entry.completed = std::max(entry.completed, value);
    }
}

bool satisfied(const GateState& gate, const GatedFree& entry) {
    for (const auto& [queue, value] : entry.waits) {
        const auto found = gate.queues.find(queue);
        if (found != gate.queues.end() && found->second.completed < value) return false;
    }
    return true;
}

// Under the gate's mutex: makes the held frees whose submissions completed, oldest first.
void drainGate(GateState& gate, FreeState& state) {
    if (gate.pending.empty()) return;
    refreshCompleted(gate, VK_NULL_HANDLE);
    for (auto it = gate.pending.begin(); it != gate.pending.end();) {
        if (!satisfied(gate, *it)) {
            ++it;
            continue;
        }
        releaseNow(state, it->kind, it->device, it->handle);
        it = gate.pending.erase(it);
    }
}

void gateFree(FreeState& state, FreeKind kind, VkDevice device, std::uint64_t handle) {
    auto& gate = Gate();
    std::lock_guard lock(gate.mutex);
    ++gate.gated;
    refreshCompleted(gate, device);
    GatedFree entry{kind, device, handle, {}};
    for (const auto& [queue, queued] : gate.queues) {
        if (queued.timeline != VK_NULL_HANDLE && queued.device == device && queued.completed < queued.assigned) entry.waits.emplace_back(queue, queued.assigned);
    }
    if (entry.waits.empty()) {
        releaseNow(state, kind, device, handle);
    } else {
        ++gate.held;
        gate.pending.push_back(std::move(entry));
        gate.mostPending = std::max(gate.mostPending, gate.pending.size());
    }
    drainGate(gate, state);
}

// Makes every held free of `device` (vkDestroyDevice: the device is idled first).
void flushGate(FreeState& state, VkDevice device) {
    auto& gate = Gate();
    std::lock_guard lock(gate.mutex);
    const auto functions = gate.devices[device];
    gate.devices.erase(device);
    if (gate.lastDevice == device) gate.lastDevice = VK_NULL_HANDLE;
    bool idled = false;
    for (auto it = gate.pending.begin(); it != gate.pending.end();) {
        if (it->device != device) {
            ++it;
            continue;
        }
        if (!idled && functions.deviceWaitIdle != nullptr) functions.deviceWaitIdle(device);
        idled = true;
        releaseNow(state, it->kind, it->device, it->handle);
        it = gate.pending.erase(it);
    }
    for (auto it = gate.queues.begin(); it != gate.queues.end();) {
        if (it->second.device != device) {
            ++it;
            continue;
        }
        if (it->second.timeline != VK_NULL_HANDLE && functions.destroySemaphore != nullptr) {
            if (!idled && functions.deviceWaitIdle != nullptr) functions.deviceWaitIdle(device);
            idled = true;
            functions.destroySemaphore(device, it->second.timeline, nullptr);
        }
        it = gate.queues.erase(it);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL gatedQueueSubmit(VkQueue queue, std::uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    auto& gate = Gate();
    VkSemaphore timeline = VK_NULL_HANDLE;
    std::uint64_t value = 0;
    PFN_vkQueueSubmit queueSubmit = nullptr;
    {
        std::lock_guard lock(gate.mutex);
        auto& entry = gateQueue(gate, queue);
        if (const auto found = gate.devices.find(entry.device); found != gate.devices.end()) queueSubmit = found->second.queueSubmit;
        if (queueSubmit == nullptr) {
            // A queue of a device whose vkQueueSubmit was never resolved through the gate: cannot happen
            // (this wrapper is only handed out by that resolution), kept as a defined failure.
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        timeline = entry.timeline;
        if (timeline != VK_NULL_HANDLE) value = ++entry.assigned;
        else ++gate.ungatedSubmits;
        drainGate(gate, State());
    }
    if (timeline == VK_NULL_HANDLE) return queueSubmit(queue, count, submits, fence);
    // A trailing batch with only the signal: a signal operation's first scope includes every queue
    // operation earlier in submission order, so the value means all of this call's batches completed.
    VkTimelineSemaphoreSubmitInfo values{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    values.signalSemaphoreValueCount = 1;
    values.pSignalSemaphoreValues = &value;
    VkSubmitInfo signal{VK_STRUCTURE_TYPE_SUBMIT_INFO, &values};
    signal.signalSemaphoreCount = 1;
    signal.pSignalSemaphores = &timeline;
    constexpr std::uint32_t Inline = 8;
    if (count < Inline) {
        VkSubmitInfo batches[Inline];
        for (std::uint32_t i = 0; i < count; ++i) batches[i] = submits[i];
        batches[count] = signal;
        return queueSubmit(queue, count + 1, batches, fence);
    }
    std::vector<VkSubmitInfo> batches(submits, submits + count);
    batches.push_back(signal);
    return queueSubmit(queue, count + 1, batches.data(), fence);
}

VKAPI_ATTR void VKAPI_CALL gatedGetDeviceQueue(VkDevice device, std::uint32_t family, std::uint32_t index, VkQueue* queue) {
    auto& gate = Gate();
    PFN_vkGetDeviceQueue getDeviceQueue = nullptr;
    {
        std::lock_guard lock(gate.mutex);
        getDeviceQueue = gate.devices[device].getDeviceQueue;
    }
    if (getDeviceQueue == nullptr) return;
    getDeviceQueue(device, family, index, queue);
    if (queue == nullptr || *queue == VK_NULL_HANDLE) return;
    std::lock_guard lock(gate.mutex);
    gate.queues[*queue].device = device;
}

void release(FreeState& state, const Deferred& entry) {
    if (GateEnabled() && (entry.kind == FreeKind::Memory || entry.kind == FreeKind::QueryPool)) {
        gateFree(state, entry.kind, entry.device, entry.handle);
        return;
    }
    switch (entry.kind) {
    case FreeKind::Memory: state.freeMemory(entry.device, reinterpret_cast<VkDeviceMemory>(entry.handle), nullptr); break;
    case FreeKind::Buffer: state.destroyBuffer(entry.device, reinterpret_cast<VkBuffer>(entry.handle), nullptr); break;
    case FreeKind::Image: state.destroyImage(entry.device, reinterpret_cast<VkImage>(entry.handle), nullptr); break;
    case FreeKind::ImageView: state.destroyImageView(entry.device, reinterpret_cast<VkImageView>(entry.handle), nullptr); break;
    case FreeKind::BufferView: state.destroyBufferView(entry.device, reinterpret_cast<VkBufferView>(entry.handle), nullptr); break;
    case FreeKind::QueryPool: state.destroyQueryPool(entry.device, reinterpret_cast<VkQueryPool>(entry.handle), nullptr); break;
    }
}

void handle(FreeKind kind, VkDevice device, std::uint64_t object) {
    if (object == 0) return;
    if (kind != FreeKind::QueryPool) StaleHandleRelease(static_cast<StaleObject>(kind), object);
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
        if (const auto* recorder = ActiveRecorderProgress(); recorder != nullptr) {
            entry.recorder = recorder;
            entry.serial = NewestSubmittedBatch(*recorder) + 1u + static_cast<std::uint64_t>(DeferBatches());
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

VKAPI_ATTR void VKAPI_CALL guardedDestroyQueryPool(VkDevice device, VkQueryPool pool, const VkAllocationCallbacks*) {
    handle(FreeKind::QueryPool, device, reinterpret_cast<std::uint64_t>(pool));
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
    if (GateEnabled()) flushGate(state, device);
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
    if (GateEnabled()) {
        auto& gate = Gate();
        // The gate's own calls go straight to the driver, resolved per device at its first lookup.
        std::unique_lock lock(gate.mutex);
        gate.lastDevice = device;
        auto [entry, added] = gate.devices.try_emplace(device);
        auto& functions = entry->second;
        if (added) {
            functions.createSemaphore = reinterpret_cast<PFN_vkCreateSemaphore>(state.real(device, "vkCreateSemaphore"));
            functions.destroySemaphore = reinterpret_cast<PFN_vkDestroySemaphore>(state.real(device, "vkDestroySemaphore"));
            functions.counterValue = reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(state.real(device, "vkGetSemaphoreCounterValue"));
            if (functions.counterValue == nullptr) functions.counterValue = reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(state.real(device, "vkGetSemaphoreCounterValueKHR"));
            functions.deviceWaitIdle = reinterpret_cast<PFN_vkDeviceWaitIdle>(state.real(device, "vkDeviceWaitIdle"));
        }
        // The stale tracer's wrappers (when on) sit between the gate and the driver.
        const auto inner = [&] {
            const auto wrapped = StaleHandleWrapper(name, real);
            return wrapped != nullptr ? wrapped : real;
        };
        if (std::strcmp(name, "vkQueueSubmit") == 0) {
            functions.queueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(inner());
            return reinterpret_cast<PFN_vkVoidFunction>(&gatedQueueSubmit);
        }
        if (std::strcmp(name, "vkGetDeviceQueue") == 0) {
            functions.getDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(real);
            return reinterpret_cast<PFN_vkVoidFunction>(&gatedGetDeviceQueue);
        }
        lock.unlock();
        if (std::strcmp(name, "vkDestroyQueryPool") == 0) {
            state.destroyQueryPool = reinterpret_cast<PFN_vkDestroyQueryPool>(inner());
            return reinterpret_cast<PFN_vkVoidFunction>(&guardedDestroyQueryPool);
        }
    }
    if (const auto wrapped = StaleHandleWrapper(name, real)) return wrapped;
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
    if (!Traced() && !Deferring() && !StaleHandlesTraced() && !GateEnabled()) return real;
    if (GateEnabled()) std::fprintf(stderr, "[gpu-free] free gate: Vulkan memory and query pools are freed once every queue submission made before the free completed (APS5_FREE_GATE=0 disables)\n");
    if (StaleHandlesTraced()) std::fprintf(stderr, "[stale] APS5_TRACE_STALE_HANDLES: commands recording freed memory or destroyed objects, and memory freed while a command buffer uses it, are reported\n");
    auto& state = State();
    state.real = real;
    if (Traced()) std::fprintf(stderr, "[gpu-free] APS5_TRACE_GPU_FREES: Vulkan frees are recorded and the last 256 printed when the device is lost\n");
    if (DeferBatches() >= 0) std::fprintf(stderr, "[gpu-free] APS5_DEFER_GPU_FREES: Vulkan memory, buffers, images and views are freed once the batch open when the driver releases them, and %d more, completed\n", DeferBatches());
    if (DeferBy().count() != 0) std::fprintf(stderr, "[gpu-free] APS5_DEFER_GPU_FREES: Vulkan memory, buffers, images and views are freed %lld ms after the driver releases them\n", static_cast<long long>(DeferBy().count()));
    return &guardedDeviceProc;
}

void InstallImportRetirementReport(void (*report)(const char* why)) {
    RetirementReport().store(report, std::memory_order_release);
}

void ReportImportRetirementsIfLinked(const char* why) {
    if (const auto report = RetirementReport().load(std::memory_order_acquire); report != nullptr) report(why);
}

bool FreeGateEnabled() {
    return GateEnabled();
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
    if (GateEnabled()) {
        auto& gate = Gate();
        std::unique_lock gateLock(gate.mutex, std::try_to_lock);
        if (gateLock.owns_lock()) std::fprintf(stderr, "[gpu-free] %s: free gate %llu frees, %llu held, %zu still held (most %zu), %llu ungated submits\n", why, static_cast<unsigned long long>(gate.gated), static_cast<unsigned long long>(gate.held), gate.pending.size(), gate.mostPending, static_cast<unsigned long long>(gate.ungatedSubmits));
    }
    for (std::uint64_t n = count - shown; n < count; ++n) {
        const auto& note = ring[n % ring.size()];
        std::fprintf(stderr, "[gpu-free]   -%.1f ms %s 0x%llx from", std::chrono::duration<double, std::milli>(now - note.at).count(), kindName(note.kind), static_cast<unsigned long long>(note.handle));
        for (int i = 0; i < note.depth; ++i) printFrame(note.frames[static_cast<std::size_t>(i)]);
        std::fprintf(stderr, "\n");
    }
}

}
