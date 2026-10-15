#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BufferPool.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <type_traits>

namespace AgcDriver::Graphics {

namespace {

VkDeviceSize ClassAllocationBytes(const VkMemoryRequirements& requirements, std::size_t capacity) {
    const auto alignment = std::max<VkDeviceSize>(requirements.alignment, 1);
    return std::max<VkDeviceSize>(requirements.size, (static_cast<VkDeviceSize>(capacity) + alignment - 1) / alignment * alignment);
}

VkBuffer BindExactBuffer(const Context& context, std::size_t size, VkBufferUsageFlags usage, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize availableBytes) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    Check(context.Function<PFN_vkCreateBuffer>("vkCreateBuffer")(context.device, &info, nullptr, &buffer), "vkCreateBuffer pooled");
    try {
        VkMemoryRequirements requirements{};
        context.Function<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(context.device, buffer, &requirements);
        Require(requirements.size <= availableBytes, "pooled buffer memory is smaller than the requested buffer");
        Require(requirements.alignment == 0 || offset % requirements.alignment == 0, "pooled buffer offset breaks the requested buffer's alignment");
        Check(context.Function<PFN_vkBindBufferMemory>("vkBindBufferMemory")(context.device, buffer, memory, offset), "vkBindBufferMemory pooled");
    } catch (...) {
        context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, buffer, nullptr);
        throw;
    }
    return buffer;
}

}

Buffer::Buffer(const Context& context, std::size_t size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties) : context(context), size(size), capacity(BufferPool::Capacity(size, properties)), usage(BufferPool::Usage(usage, properties)), properties(properties) {
    Require(size != 0, "zero-sized GPU buffer");
    const bool addressable = (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0;
    Require(!addressable || context.bufferDeviceAddress, "buffer device address is not enabled");
    cache = GetBufferPool(context);
    if (const auto allocation = cache->Take(size, this->usage, properties)) {
        buffer = allocation->buffer;
        memory = allocation->memory;
        mapping = allocation->mapping;
        deviceAddress = allocation->address;
        allocationBytes = allocation->allocationBytes;
        capacity = allocation->bytes;
        offset = allocation->offset;
        slab = allocation->slab;
        alternates = allocation->alternates;
        if (allocation->bufferBytes != size) {
            const auto dropped = KeepAlternate(alternates, {buffer, deviceAddress, allocation->bufferBytes});
            if (dropped.buffer != VK_NULL_HANDLE) context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, dropped.buffer, nullptr);
            buffer = VK_NULL_HANDLE;
            deviceAddress = 0;
            try {
                buffer = BindExactBuffer(context, size, this->usage, memory, offset, allocationBytes);
                initializeAddress(usage);
            } catch (...) {
                release();
                throw;
            }
        }
        ready = true;
        return;
    }
    try {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = this->usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Check(context.Function<PFN_vkCreateBuffer>("vkCreateBuffer")(context.device, &info, nullptr, &buffer), "vkCreateBuffer");
        VkMemoryRequirements requirements{};
        context.Function<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(context.device, buffer, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        const VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, nullptr, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
        if (addressable) allocation.pNext = &flags;
        allocation.allocationSize = ClassAllocationBytes(requirements, capacity);
        allocationBytes = allocation.allocationSize;
        // The CPU reads most of these buffers back (write-back, diffs), which is very slow from
        // write-combined memory, so the default host properties prefer cached host memory.
        constexpr VkMemoryPropertyFlags hostDefault = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if (properties == hostDefault) {
            try {
                allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, hostDefault | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
            } catch (const std::runtime_error&) {
                allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, hostDefault);
            }
        } else {
            allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, properties);
        }
        if ((properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 && BufferPool::SlabEligible(capacity, requirements.alignment, requirements.size, context.limits.nonCoherentAtomSize)) {
            const auto slot = cache->TakeSlot(context, allocation.memoryTypeIndex, capacity, addressable);
            memory = slot.memory;
            offset = slot.offset;
            slab = true;
            allocationBytes = capacity;
            Check(context.Function<PFN_vkBindBufferMemory>("vkBindBufferMemory")(context.device, buffer, memory, offset), "vkBindBufferMemory slab");
            initializeAddress(usage);
            mapping = slot.mapping;
            ready = true;
            return;
        }
        memory = cache->AllocateMemory(allocation, "vkAllocateMemory buffer");
        Check(context.Function<PFN_vkBindBufferMemory>("vkBindBufferMemory")(context.device, buffer, memory, 0), "vkBindBufferMemory");
        initializeAddress(usage);
        if ((properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) Check(context.Function<PFN_vkMapMemory>("vkMapMemory")(context.device, memory, 0, VK_WHOLE_SIZE, 0, &mapping), "vkMapMemory");
        ready = true;
    } catch (...) {
        release();
        throw;
    }
}

Buffer::~Buffer() {
    release();
}

void Buffer::release() noexcept {
    if (ready && cache) {
        cache->Put({buffer, memory, mapping, deviceAddress, allocationBytes, capacity, usage, properties, offset, slab, size, alternates});
        return;
    }
    for (const auto& alternate : alternates) {
        if (alternate.buffer != VK_NULL_HANDLE) context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, alternate.buffer, nullptr);
    }
    if (slab) {
        if (buffer) context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, buffer, nullptr);
        cache->PutSlot(memory, offset);
        return;
    }
    if (mapping) context.Function<PFN_vkUnmapMemory>("vkUnmapMemory")(context.device, memory);
    if (buffer) context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, buffer, nullptr);
    if (memory) context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, memory, nullptr);
}

VkBuffer Buffer::Handle() const {
    return buffer;
}

std::span<std::byte> Buffer::Bytes() {
    Require(mapping != nullptr, "device-local buffer has no host mapping");
    return {static_cast<std::byte*>(mapping), size};
}

void Buffer::Invalidate() {
    Require(mapping != nullptr, "cannot invalidate an unmapped GPU buffer");
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = memory;
    range.offset = offset;
    range.size = slab ? static_cast<VkDeviceSize>(capacity) : VK_WHOLE_SIZE;
    Check(context.Function<PFN_vkInvalidateMappedMemoryRanges>("vkInvalidateMappedMemoryRanges")(context.device, 1, &range), "vkInvalidateMappedMemoryRanges");
}

DeviceBuffer::DeviceBuffer(const Context& context, std::size_t size, VkBufferUsageFlags usage) : context(context), size(size), capacity(BufferPool::Capacity(size, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)), usage(BufferPool::Usage(usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
    Require(size != 0, "zero-sized device buffer");
    cache = GetBufferPool(context);
    if (const auto allocation = cache->Take(size, this->usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
        buffer = allocation->buffer;
        memory = allocation->memory;
        allocationBytes = allocation->allocationBytes;
        capacity = allocation->bytes;
        alternates = allocation->alternates;
        if (allocation->bufferBytes == size) return;
        const auto dropped = KeepAlternate(alternates, {buffer, 0, allocation->bufferBytes});
        if (dropped.buffer != VK_NULL_HANDLE) context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, dropped.buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        try {
            buffer = BindExactBuffer(context, size, this->usage, memory, 0, allocationBytes);
        } catch (...) {
            release();
            throw;
        }
        return;
    }
    try {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = this->usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Check(context.Function<PFN_vkCreateBuffer>("vkCreateBuffer")(context.device, &info, nullptr, &buffer), "vkCreateBuffer device");
        VkMemoryRequirements requirements{};
        context.Function<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(context.device, buffer, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = ClassAllocationBytes(requirements, capacity);
        allocationBytes = allocation.allocationSize;
        allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        memory = cache->AllocateMemory(allocation, "vkAllocateMemory device buffer");
        Check(context.Function<PFN_vkBindBufferMemory>("vkBindBufferMemory")(context.device, buffer, memory, 0), "vkBindBufferMemory device");
    } catch (...) {
        release();
        throw;
    }
}

DeviceBuffer::~DeviceBuffer() {
    release();
}

void DeviceBuffer::release() noexcept {
    if (buffer && memory && cache) {
        cache->Put({buffer, memory, nullptr, 0, allocationBytes, capacity, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, false, size, alternates});
        return;
    }
    for (const auto& alternate : alternates) {
        if (alternate.buffer != VK_NULL_HANDLE) context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, alternate.buffer, nullptr);
    }
    if (buffer) context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, buffer, nullptr);
    if (memory) context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, memory, nullptr);
}

VkBuffer DeviceBuffer::Handle() const {
    return buffer;
}

std::size_t DeviceBuffer::Size() const {
    return size;
}

void CopyBuffer(const Context& context, VkCommandBuffer commands, VkBuffer source, VkDeviceSize sourceOffset, VkBuffer destination, VkDeviceSize destinationOffset, VkDeviceSize bytes) {
    const VkBufferCopy region{sourceOffset, destinationOffset, bytes};
    context.Resolved(&DeviceFunctions::cmdCopyBuffer, "vkCmdCopyBuffer")(commands, source, destination, 1, &region);
}

void RecordMemoryBarrier(const Context& context, VkCommandBuffer commands, VkPipelineStageFlags sourceStage, VkPipelineStageFlags destinationStage, VkAccessFlags sourceAccess, VkAccessFlags destinationAccess) {
    const VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, sourceAccess, destinationAccess};
    context.Resolved(&DeviceFunctions::cmdPipelineBarrier, "vkCmdPipelineBarrier")(commands, sourceStage, destinationStage, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

namespace {

// The timed device entry points (VkCallsProfiled): one trampoline per table member forwards to the
// resolved function and adds its duration to the calling thread class's counters.
#define APS5_VK_TIMED_CALLS(X) \
    X(cmdPipelineBarrier, PFN_vkCmdPipelineBarrier, "PipelineBarrier") \
    X(cmdCopyBuffer, PFN_vkCmdCopyBuffer, "CopyBuffer") \
    X(cmdUpdateBuffer, PFN_vkCmdUpdateBuffer, "UpdateBuffer") \
    X(cmdFillBuffer, PFN_vkCmdFillBuffer, "FillBuffer") \
    X(cmdBindPipeline, PFN_vkCmdBindPipeline, "BindPipeline") \
    X(cmdBindDescriptorSets, PFN_vkCmdBindDescriptorSets, "BindDescriptorSets") \
    X(cmdPushConstants, PFN_vkCmdPushConstants, "PushConstants") \
    X(cmdDispatch, PFN_vkCmdDispatch, "Dispatch") \
    X(cmdDispatchIndirect, PFN_vkCmdDispatchIndirect, "DispatchIndirect") \
    X(cmdBeginRenderPass, PFN_vkCmdBeginRenderPass, "BeginRenderPass") \
    X(cmdEndRenderPass, PFN_vkCmdEndRenderPass, "EndRenderPass") \
    X(cmdBeginRendering, PFN_vkCmdBeginRenderingKHR, "BeginRendering") \
    X(cmdEndRendering, PFN_vkCmdEndRenderingKHR, "EndRendering") \
    X(cmdSetViewport, PFN_vkCmdSetViewport, "SetViewport") \
    X(cmdSetScissor, PFN_vkCmdSetScissor, "SetScissor") \
    X(cmdSetDepthBounds, PFN_vkCmdSetDepthBounds, "SetDepthBounds") \
    X(cmdSetDepthBias, PFN_vkCmdSetDepthBias, "SetDepthBias") \
    X(cmdSetCullMode, PFN_vkCmdSetCullModeEXT, "SetCullMode") \
    X(cmdSetFrontFace, PFN_vkCmdSetFrontFaceEXT, "SetFrontFace") \
    X(cmdSetDepthTestEnable, PFN_vkCmdSetDepthTestEnableEXT, "SetDepthTestEnable") \
    X(cmdSetDepthWriteEnable, PFN_vkCmdSetDepthWriteEnableEXT, "SetDepthWriteEnable") \
    X(cmdSetDepthCompareOp, PFN_vkCmdSetDepthCompareOpEXT, "SetDepthCompareOp") \
    X(cmdSetDepthBoundsTestEnable, PFN_vkCmdSetDepthBoundsTestEnableEXT, "SetDepthBoundsTestEnable") \
    X(cmdSetStencilTestEnable, PFN_vkCmdSetStencilTestEnableEXT, "SetStencilTestEnable") \
    X(cmdSetStencilOp, PFN_vkCmdSetStencilOpEXT, "SetStencilOp") \
    X(cmdSetStencilCompareMask, PFN_vkCmdSetStencilCompareMask, "SetStencilCompareMask") \
    X(cmdSetStencilWriteMask, PFN_vkCmdSetStencilWriteMask, "SetStencilWriteMask") \
    X(cmdSetStencilReference, PFN_vkCmdSetStencilReference, "SetStencilReference") \
    X(cmdBindVertexBuffers, PFN_vkCmdBindVertexBuffers, "BindVertexBuffers") \
    X(cmdBindIndexBuffer, PFN_vkCmdBindIndexBuffer, "BindIndexBuffer") \
    X(cmdDraw, PFN_vkCmdDraw, "Draw") \
    X(cmdDrawIndexed, PFN_vkCmdDrawIndexed, "DrawIndexed") \
    X(cmdDrawIndirect, PFN_vkCmdDrawIndirect, "DrawIndirect") \
    X(cmdDrawIndexedIndirect, PFN_vkCmdDrawIndexedIndirect, "DrawIndexedIndirect") \
    X(cmdCopyBufferToImage, PFN_vkCmdCopyBufferToImage, "CopyBufferToImage") \
    X(cmdCopyImageToBuffer, PFN_vkCmdCopyImageToBuffer, "CopyImageToBuffer") \
    X(cmdClearColorImage, PFN_vkCmdClearColorImage, "ClearColorImage") \
    X(updateDescriptorSets, PFN_vkUpdateDescriptorSets, "UpdateDescriptorSets") \
    X(allocateDescriptorSets, PFN_vkAllocateDescriptorSets, "AllocateDescriptorSets")

enum VkTimedCall : std::size_t {
#define APS5_VK_ENUM(member, type, name) VkTimed_##member,
    APS5_VK_TIMED_CALLS(APS5_VK_ENUM)
#undef APS5_VK_ENUM
    VkTimedQueueSubmit,
    VkTimedCount
};

constexpr std::array<const char*, VkTimedCount> VkTimedNames{
#define APS5_VK_NAME(member, type, name) name,
    APS5_VK_TIMED_CALLS(APS5_VK_NAME)
#undef APS5_VK_NAME
    "QueueSubmit(submit thread)"};

struct VkCallCounters {
    std::array<std::atomic<std::uint64_t>, VkTimedCount> calls{};
    std::array<std::atomic<std::uint64_t>, VkTimedCount> nanoseconds{};
};

struct VkCallProfile {
    // [0] the committer, [1] every other thread.
    std::array<VkCallCounters, 2> threads;
    std::atomic<std::int64_t> windowStart{0};
    std::atomic<bool> printing{false};
};

VkCallProfile& vkCallProfile() {
    static auto* const profile = new VkCallProfile();
    return *profile;
}

thread_local bool vkCommitterThread = false;

std::int64_t vkNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void reportVkCalls(std::int64_t now) {
    auto& profile = vkCallProfile();
    auto start = profile.windowStart.load(std::memory_order_relaxed);
    if (start == 0) {
        profile.windowStart.compare_exchange_strong(start, now, std::memory_order_relaxed);
        return;
    }
    if (now - start < 10'000'000'000ll || profile.printing.exchange(true, std::memory_order_acquire)) return;
    if (!profile.windowStart.compare_exchange_strong(start, now, std::memory_order_relaxed)) {
        profile.printing.store(false, std::memory_order_release);
        return;
    }
    const double seconds = static_cast<double>(now - start) / 1e9;
    for (std::size_t thread = 0; thread < profile.threads.size(); ++thread) {
        auto& counters = profile.threads[thread];
        std::string line = thread == 0 ? "[vkcmd] committer" : "[vkcmd] other threads";
        char part[160];
        std::snprintf(part, sizeof(part), " (%.0f s, calls/us each/ms total):", seconds);
        line += part;
        double totalMs = 0;
        std::uint64_t totalCalls = 0;
        for (std::size_t index = 0; index < VkTimedCount; ++index) {
            const auto calls = counters.calls[index].exchange(0, std::memory_order_relaxed);
            const auto ns = counters.nanoseconds[index].exchange(0, std::memory_order_relaxed);
            if (calls == 0) continue;
            totalMs += static_cast<double>(ns) / 1e6;
            totalCalls += calls;
            std::snprintf(part, sizeof(part), " %s %llu/%.2f/%.1f", VkTimedNames[index], static_cast<unsigned long long>(calls), static_cast<double>(ns) / 1e3 / static_cast<double>(calls), static_cast<double>(ns) / 1e6);
            line += part;
        }
        std::snprintf(part, sizeof(part), "; total %llu calls %.1f ms\n", static_cast<unsigned long long>(totalCalls), totalMs);
        line += part;
        std::fputs(line.c_str(), stderr);
    }
    std::fflush(stderr);
    profile.printing.store(false, std::memory_order_release);
}

void noteVkCall(std::size_t index, std::int64_t start) {
    const auto now = vkNowNs();
    auto& counters = vkCallProfile().threads[vkCommitterThread ? 0 : 1];
    counters.calls[index].fetch_add(1, std::memory_order_relaxed);
    counters.nanoseconds[index].fetch_add(static_cast<std::uint64_t>(now - start), std::memory_order_relaxed);
    reportVkCalls(now);
}

template<std::size_t Index, typename TFunction>
struct TimedVkCall;

template<std::size_t Index, typename TResult, typename... TArgs>
struct TimedVkCall<Index, TResult(VKAPI_PTR*)(TArgs...)> {
    static inline TResult(VKAPI_PTR* real)(TArgs...) = nullptr;
    static TResult VKAPI_CALL Call(TArgs... args) {
        const auto start = vkNowNs();
        if constexpr (std::is_void_v<TResult>) {
            real(args...);
            noteVkCall(Index, start);
        } else {
            auto result = real(args...);
            noteVkCall(Index, start);
            return result;
        }
    }
};

void wrapTimedVkCalls(DeviceFunctions& functions) {
#define APS5_VK_WRAP(member, type, name) \
    if (functions.member != nullptr) { \
        TimedVkCall<VkTimed_##member, type>::real = functions.member; \
        functions.member = &TimedVkCall<VkTimed_##member, type>::Call; \
    }
    APS5_VK_TIMED_CALLS(APS5_VK_WRAP)
#undef APS5_VK_WRAP
}

}

bool VkCallsProfiled() {
    static const bool enabled = std::getenv("APS5_PROFILE_VKCMD") != nullptr || std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return enabled;
}

void MarkVkCommitterThread() {
    vkCommitterThread = true;
}

void NoteQueueSubmitTime(double us) {
    if (!VkCallsProfiled()) return;
    const auto now = vkNowNs();
    noteVkCall(VkTimedQueueSubmit, now - static_cast<std::int64_t>(us * 1000.0));
}

void FillDeviceFunctions(const Context& context, DeviceFunctions& functions) {
    functions.cmdPipelineBarrier = context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    functions.cmdCopyBuffer = context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");
    functions.cmdUpdateBuffer = context.Function<PFN_vkCmdUpdateBuffer>("vkCmdUpdateBuffer");
    functions.cmdFillBuffer = context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer");
    functions.cmdBindPipeline = context.Function<PFN_vkCmdBindPipeline>("vkCmdBindPipeline");
    functions.cmdBindDescriptorSets = context.Function<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets");
    functions.cmdPushConstants = context.Function<PFN_vkCmdPushConstants>("vkCmdPushConstants");
    functions.cmdDispatch = context.Function<PFN_vkCmdDispatch>("vkCmdDispatch");
    functions.cmdDispatchIndirect = context.Function<PFN_vkCmdDispatchIndirect>("vkCmdDispatchIndirect");
    functions.cmdBeginRenderPass = context.Function<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass");
    functions.cmdEndRenderPass = context.Function<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass");
    functions.cmdSetViewport = context.Function<PFN_vkCmdSetViewport>("vkCmdSetViewport");
    functions.cmdSetScissor = context.Function<PFN_vkCmdSetScissor>("vkCmdSetScissor");
    functions.cmdSetDepthBounds = context.Function<PFN_vkCmdSetDepthBounds>("vkCmdSetDepthBounds");
    functions.cmdSetDepthBias = context.Function<PFN_vkCmdSetDepthBias>("vkCmdSetDepthBias");
    if (context.graphicsPipelineLibrary) {
        functions.cmdBeginRendering = context.Function<PFN_vkCmdBeginRenderingKHR>("vkCmdBeginRenderingKHR");
        functions.cmdEndRendering = context.Function<PFN_vkCmdEndRenderingKHR>("vkCmdEndRenderingKHR");
        functions.cmdSetCullMode = context.Function<PFN_vkCmdSetCullModeEXT>("vkCmdSetCullModeEXT");
        functions.cmdSetFrontFace = context.Function<PFN_vkCmdSetFrontFaceEXT>("vkCmdSetFrontFaceEXT");
        functions.cmdSetDepthTestEnable = context.Function<PFN_vkCmdSetDepthTestEnableEXT>("vkCmdSetDepthTestEnableEXT");
        functions.cmdSetDepthWriteEnable = context.Function<PFN_vkCmdSetDepthWriteEnableEXT>("vkCmdSetDepthWriteEnableEXT");
        functions.cmdSetDepthCompareOp = context.Function<PFN_vkCmdSetDepthCompareOpEXT>("vkCmdSetDepthCompareOpEXT");
        functions.cmdSetDepthBoundsTestEnable = context.Function<PFN_vkCmdSetDepthBoundsTestEnableEXT>("vkCmdSetDepthBoundsTestEnableEXT");
        functions.cmdSetStencilTestEnable = context.Function<PFN_vkCmdSetStencilTestEnableEXT>("vkCmdSetStencilTestEnableEXT");
        functions.cmdSetStencilOp = context.Function<PFN_vkCmdSetStencilOpEXT>("vkCmdSetStencilOpEXT");
        functions.cmdSetStencilCompareMask = context.Function<PFN_vkCmdSetStencilCompareMask>("vkCmdSetStencilCompareMask");
        functions.cmdSetStencilWriteMask = context.Function<PFN_vkCmdSetStencilWriteMask>("vkCmdSetStencilWriteMask");
        functions.cmdSetStencilReference = context.Function<PFN_vkCmdSetStencilReference>("vkCmdSetStencilReference");
    }
    functions.cmdBindVertexBuffers = context.Function<PFN_vkCmdBindVertexBuffers>("vkCmdBindVertexBuffers");
    functions.cmdBindIndexBuffer = context.Function<PFN_vkCmdBindIndexBuffer>("vkCmdBindIndexBuffer");
    functions.cmdDraw = context.Function<PFN_vkCmdDraw>("vkCmdDraw");
    functions.cmdDrawIndexed = context.Function<PFN_vkCmdDrawIndexed>("vkCmdDrawIndexed");
    functions.cmdDrawIndirect = context.Function<PFN_vkCmdDrawIndirect>("vkCmdDrawIndirect");
    functions.cmdDrawIndexedIndirect = context.Function<PFN_vkCmdDrawIndexedIndirect>("vkCmdDrawIndexedIndirect");
    functions.cmdCopyBufferToImage = context.Function<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage");
    functions.cmdCopyImageToBuffer = context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer");
    functions.cmdClearColorImage = context.Function<PFN_vkCmdClearColorImage>("vkCmdClearColorImage");
    functions.updateDescriptorSets = context.Function<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets");
    functions.allocateDescriptorSets = context.Function<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets");
    functions.getFenceStatus = context.Function<PFN_vkGetFenceStatus>("vkGetFenceStatus");
    // The timers are installed once per process: the trampolines keep one real entry point each
    // (every device of the process resolves the same MoltenVK functions).
    if (VkCallsProfiled()) wrapTimedVkCalls(functions);
}

RenderTarget::RenderTarget(const Context& context, const ColorTarget& target, bool blending) : context(context) {
    VkFormatProperties properties{};
    context.formatProperties(context.physical, target.format, &properties);
    const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | (blending ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT : 0u);
    Require((properties.optimalTilingFeatures & required) == required, "render-target format does not support required operations");
    constexpr auto usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImageFormatProperties supported{};
    Check(context.imageFormatProperties(context.physical, target.format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0, &supported), "vkGetPhysicalDeviceImageFormatProperties");
    Require(target.extent.width <= supported.maxExtent.width && target.extent.height <= supported.maxExtent.height && (supported.sampleCounts & VK_SAMPLE_COUNT_1_BIT) != 0 && target.bytes <= supported.maxResourceSize, "render target exceeds device image limits");
    Require(target.extent.width <= context.limits.maxFramebufferWidth && target.extent.height <= context.limits.maxFramebufferHeight, "render target exceeds framebuffer limits");
    try {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = target.format;
        info.extent = {target.extent.width, target.extent.height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        Check(context.Function<PFN_vkCreateImage>("vkCreateImage")(context.device, &info, nullptr, &image), "vkCreateImage");
        VkMemoryRequirements requirements{};
        context.Function<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(context.device, image, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")(context.device, &allocation, nullptr, &memory), "vkAllocateMemory render target");
        Check(context.Function<PFN_vkBindImageMemory>("vkBindImageMemory")(context.device, image, memory, 0), "vkBindImageMemory");
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = target.format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        Check(context.Function<PFN_vkCreateImageView>("vkCreateImageView")(context.device, &viewInfo, nullptr, &view), "vkCreateImageView");
    } catch (...) {
        release();
        throw;
    }
}

RenderTarget::~RenderTarget() {
    release();
}

void RenderTarget::release() noexcept {
    if (view) context.Function<PFN_vkDestroyImageView>("vkDestroyImageView")(context.device, view, nullptr);
    if (image) context.Function<PFN_vkDestroyImage>("vkDestroyImage")(context.device, image, nullptr);
    if (memory) context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, memory, nullptr);
}

VkImage RenderTarget::Image() const {
    return image;
}

VkImageView RenderTarget::View() const {
    return view;
}

CommandBatch::CommandBatch(const Context& context) : context(context) {
    // Records into the device's one command pool and submits to its queue: device-lock work only.
    GuestMemory::AssertGpuLockHeld("CommandBatch");
    try {
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = context.pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        Check(context.Function<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(context.device, &allocation, &commands), "vkAllocateCommandBuffers");
        VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        Check(context.Function<PFN_vkCreateFence>("vkCreateFence")(context.device, &info, nullptr, &fence), "vkCreateFence");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Check(context.Function<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(commands, &begin), "vkBeginCommandBuffer");
        // Work recorded so far goes first in queue order, so this batch sees its results.
        if (auto* recorder = Recorder::Active(); recorder != nullptr && recorder->Recording()) recorder->Submit();
    } catch (...) {
        release();
        throw;
    }
}

CommandBatch::~CommandBatch() {
    release();
}

void CommandBatch::release() noexcept {
    if (pending) {
        auto result = context.Function<PFN_vkGetFenceStatus>("vkGetFenceStatus")(context.device, fence);
        if (result == VK_NOT_READY) {
            const auto access = QueueSubmitter::Access(context.queue);
            result = context.Function<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(context.queue);
        }
        if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST) std::terminate();
    }
    if (commands) context.Function<PFN_vkFreeCommandBuffers>("vkFreeCommandBuffers")(context.device, context.pool, 1, &commands);
    if (fence) context.Function<PFN_vkDestroyFence>("vkDestroyFence")(context.device, fence, nullptr);
}

VkCommandBuffer CommandBatch::Handle() const {
    return commands;
}

void CommandBatch::SubmitAndWait() {
    Submit();
    Wait();
}

void CommandBatch::Submit() {
    PerformanceTimer timing("Graphics.Submit");
    Require(!submitted, "command batch has already been submitted");
    Check(context.Function<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands), "vkEndCommandBuffer");
    VkSubmitInfo submission{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submission.commandBufferCount = 1;
    submission.pCommandBuffers = &commands;
    timing.Mark("command_end");
    {
        const auto access = QueueSubmitter::Access(context.queue);
        Check(context.Function<PFN_vkQueueSubmit>("vkQueueSubmit")(context.queue, 1, &submission, fence), "vkQueueSubmit graphics");
    }
    timing.Mark("queue_submit");
    pending = true;
    submitted = true;
}

void CommandBatch::Wait() {
    Require(submitted, "command batch has not been submitted");
    if (!pending) return;
    PerformanceTimer timing("Graphics.Wait");
    const auto result = context.Function<PFN_vkWaitForFences>("vkWaitForFences")(context.device, 1, &fence, VK_TRUE, 5'000'000'000ULL);
    timing.Mark("fence_wait");
    if (result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST) pending = false;
    Check(result, "vkWaitForFences graphics");
    // Recorded batches preceded this one, so their completions (write-backs) can run now.
    if (auto* recorder = Recorder::Active()) recorder->Reap();
}

}
