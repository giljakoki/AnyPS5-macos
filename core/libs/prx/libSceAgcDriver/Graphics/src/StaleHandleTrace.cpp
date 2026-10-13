#include "prx/libSceAgcDriver/Graphics/include/StaleHandleTrace.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#ifndef _WIN32
#include <dlfcn.h>
#include <execinfo.h>
#endif

namespace AgcDriver::Graphics {

namespace {

template <typename Handle>
std::uint64_t key(Handle handle) {
    return reinterpret_cast<std::uint64_t>(handle);
}

struct Freed {
    std::chrono::steady_clock::time_point at;
    std::array<void*, 8> frames{};
    int depth = 0;
};

struct CommandRefs {
    std::unordered_map<std::uint64_t, std::uint64_t> memories;
    std::unordered_set<std::uint64_t> buffers;
    std::uint64_t fence = 0;
    bool submitted = false;
    std::chrono::steady_clock::time_point begun;
};

struct AddressRange {
    std::uint64_t end = 0;
    std::uint64_t buffer = 0;
    std::uint64_t memory = 0;
    Freed freed;
};

struct Tracker {
    std::mutex mutex;
    std::unordered_map<std::uint64_t, std::uint64_t> bufferSize;
    std::unordered_map<std::uint64_t, std::uint64_t> memorySize;
    std::unordered_map<std::uint64_t, std::pair<const std::byte*, std::uint64_t>> memoryMapping;
    std::unordered_map<std::uint64_t, std::uint64_t> mappingOffset;
    std::unordered_map<std::uint64_t, std::uint64_t> bufferOffset;
    std::unordered_map<std::uint64_t, std::chrono::steady_clock::time_point> bufferBound;
    std::unordered_map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> bufferAddress;
    std::map<std::uint64_t, AddressRange> freedRanges;
    std::array<std::pair<std::uint64_t, AddressRange>, 64> freedRing{};
    std::uint64_t freedRingCount = 0;
    std::atomic<bool> dumped{false};
    std::unordered_map<std::uint64_t, std::uint64_t> bufferMemory;
    std::unordered_map<std::uint64_t, std::uint64_t> imageMemory;
    std::unordered_map<std::uint64_t, std::uint64_t> viewImage;
    std::unordered_map<std::uint64_t, std::uint64_t> bufferViewBuffer;
    std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> framebufferViews;
    std::unordered_map<std::uint64_t, std::unordered_map<std::uint64_t, std::pair<StaleObject, std::uint64_t>>> setRefs;
    std::unordered_map<std::uint64_t, Freed> freedMemory;
    std::unordered_set<std::uint64_t> deadObjects;
    std::unordered_map<std::uint64_t, CommandRefs> commands;
    std::atomic<std::uint64_t> reports{0};
};

Tracker& Track() {
    static Tracker& tracker = *new Tracker();
    return tracker;
}

void capture(std::array<void*, 8>& frames, int& depth) {
#ifndef _WIN32
    void* raw[9];
    const int count = backtrace(raw, 9);
    depth = 0;
    for (int i = 1; i < count && depth < 8; ++i) frames[static_cast<std::size_t>(depth++)] = raw[i];
#else
    depth = 0;
#endif
}

void printFrames(const std::array<void*, 8>& frames, int depth) {
    for (int i = 0; i < depth; ++i) {
#ifndef _WIN32
        Dl_info info{};
        if (dladdr(frames[static_cast<std::size_t>(i)], &info) != 0 && info.dli_fname != nullptr) {
            const char* slash = std::strrchr(info.dli_fname, '/');
            std::fprintf(stderr, " %s+0x%llx", slash != nullptr ? slash + 1 : info.dli_fname, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(frames[static_cast<std::size_t>(i)]) - reinterpret_cast<std::uintptr_t>(info.dli_fbase)));
            continue;
        }
#endif
        std::fprintf(stderr, " %p", frames[static_cast<std::size_t>(i)]);
    }
}

const char* objectName(StaleObject kind) {
    switch (kind) {
    case StaleObject::Memory: return "memory";
    case StaleObject::Buffer: return "buffer";
    case StaleObject::Image: return "image";
    case StaleObject::ImageView: return "image view";
    case StaleObject::BufferView: return "buffer view";
    }
    return "?";
}

bool reportAllowed() {
    return Track().reports.fetch_add(1, std::memory_order_relaxed) < 64;
}

std::uint64_t memoryOf(Tracker& tracker, StaleObject kind, std::uint64_t handle) {
    switch (kind) {
    case StaleObject::Memory: return handle;
    case StaleObject::Buffer: {
        const auto found = tracker.bufferMemory.find(handle);
        return found != tracker.bufferMemory.end() ? found->second : 0;
    }
    case StaleObject::Image: {
        const auto found = tracker.imageMemory.find(handle);
        return found != tracker.imageMemory.end() ? found->second : 0;
    }
    case StaleObject::ImageView: {
        const auto found = tracker.viewImage.find(handle);
        return found != tracker.viewImage.end() ? memoryOf(tracker, StaleObject::Image, found->second) : 0;
    }
    case StaleObject::BufferView: {
        const auto found = tracker.bufferViewBuffer.find(handle);
        return found != tracker.bufferViewBuffer.end() ? memoryOf(tracker, StaleObject::Buffer, found->second) : 0;
    }
    }
    return 0;
}

void reference(Tracker& tracker, VkCommandBuffer commands, const char* command, StaleObject kind, std::uint64_t handle) {
    if (handle == 0) return;
    if (tracker.deadObjects.contains(handle) && reportAllowed()) {
        std::array<void*, 8> frames{};
        int depth = 0;
        capture(frames, depth);
        std::fprintf(stderr, "[stale] %s records destroyed %s 0x%llx into command buffer %p from", command, objectName(kind), static_cast<unsigned long long>(handle), static_cast<void*>(commands));
        printFrames(frames, depth);
        std::fprintf(stderr, "\n");
    }
    const auto memory = memoryOf(tracker, kind, handle);
    if (memory == 0) return;
    if (const auto freed = tracker.freedMemory.find(memory); freed != tracker.freedMemory.end() && reportAllowed()) {
        std::array<void*, 8> frames{};
        int depth = 0;
        capture(frames, depth);
        std::fprintf(stderr, "[stale] %s records %s 0x%llx whose memory 0x%llx was freed %.1f ms earlier into command buffer %p; recorded from", command, objectName(kind), static_cast<unsigned long long>(handle), static_cast<unsigned long long>(memory), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - freed->second.at).count(), static_cast<void*>(commands));
        printFrames(frames, depth);
        std::fprintf(stderr, "; freed from");
        printFrames(freed->second.frames, freed->second.depth);
        std::fprintf(stderr, "\n");
    }
    auto& refs = tracker.commands[key(commands)];
    refs.memories.emplace(memory, handle);
    if (kind == StaleObject::Buffer) refs.buffers.insert(handle);
}

void referenceSet(Tracker& tracker, VkCommandBuffer commands, const char* command, VkDescriptorSet set) {
    const auto found = tracker.setRefs.find(key(set));
    if (found == tracker.setRefs.end()) return;
    for (const auto& [slot, ref] : found->second) reference(tracker, commands, command, ref.first, ref.second);
}

void completeFence(Tracker& tracker, std::uint64_t fence) {
    if (fence == 0) return;
    for (auto& [commands, refs] : tracker.commands) {
        if (refs.submitted && refs.fence == fence) {
            refs.memories.clear();
            refs.buffers.clear();
            refs.submitted = false;
        }
    }
}

#define STALE_REAL(name) static PFN_##name real_##name = nullptr

STALE_REAL(vkAllocateMemory);
STALE_REAL(vkBindBufferMemory);
STALE_REAL(vkBindImageMemory);
STALE_REAL(vkCreateBuffer);
STALE_REAL(vkCreateImage);
STALE_REAL(vkCreateImageView);
STALE_REAL(vkCreateFramebuffer);
STALE_REAL(vkDestroyFramebuffer);
STALE_REAL(vkAllocateDescriptorSets);
STALE_REAL(vkUpdateDescriptorSets);
STALE_REAL(vkBeginCommandBuffer);
STALE_REAL(vkResetCommandBuffer);
STALE_REAL(vkFreeCommandBuffers);
STALE_REAL(vkQueueSubmit);
STALE_REAL(vkWaitForFences);
STALE_REAL(vkGetFenceStatus);
STALE_REAL(vkResetFences);
STALE_REAL(vkQueueWaitIdle);
STALE_REAL(vkDeviceWaitIdle);
STALE_REAL(vkCmdBindDescriptorSets);
STALE_REAL(vkCmdBindVertexBuffers);
STALE_REAL(vkCmdBindIndexBuffer);
STALE_REAL(vkCmdCopyBuffer);
STALE_REAL(vkCmdCopyBufferToImage);
STALE_REAL(vkCmdCopyImageToBuffer);
STALE_REAL(vkCmdCopyImage);
STALE_REAL(vkCmdBlitImage);
STALE_REAL(vkCmdFillBuffer);
STALE_REAL(vkCmdUpdateBuffer);
STALE_REAL(vkCmdClearColorImage);
STALE_REAL(vkCmdClearDepthStencilImage);
STALE_REAL(vkCmdPipelineBarrier);
STALE_REAL(vkCmdDispatchIndirect);
STALE_REAL(vkCmdDrawIndirect);
STALE_REAL(vkCmdDrawIndexedIndirect);
STALE_REAL(vkCmdDrawIndirectCountKHR);
STALE_REAL(vkCmdDrawIndexedIndirectCountKHR);
STALE_REAL(vkCmdDrawMeshTasksIndirectEXT);
STALE_REAL(vkCmdBeginRenderPass);
STALE_REAL(vkCmdBeginRenderingKHR);
STALE_REAL(vkCmdCopyQueryPoolResults);
STALE_REAL(vkGetBufferDeviceAddressKHR);
STALE_REAL(vkCmdPushConstants);
STALE_REAL(vkMapMemory);
STALE_REAL(vkUnmapMemory);

VKAPI_ATTR VkResult VKAPI_CALL staleMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void** data) {
    const auto result = real_vkMapMemory(device, memory, offset, size, flags, data);
    if (result == VK_SUCCESS) {
        auto& tracker = Track();
        std::lock_guard lock(tracker.mutex);
        const auto total = tracker.memorySize.contains(key(memory)) ? tracker.memorySize[key(memory)] : 0u;
        const auto bytes = size == VK_WHOLE_SIZE ? (total > offset ? total - offset : 0u) : size;
        tracker.memoryMapping[key(memory)] = {static_cast<const std::byte*>(*data), bytes};
        tracker.mappingOffset[key(memory)] = offset;
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL staleUnmapMemory(VkDevice device, VkDeviceMemory memory) {
    {
        auto& tracker = Track();
        std::lock_guard lock(tracker.mutex);
        tracker.memoryMapping.erase(key(memory));
    }
    real_vkUnmapMemory(device, memory);
}

const AddressRange* freedRangeAt(Tracker& tracker, std::uint64_t value, std::uint64_t& start) {
    auto found = tracker.freedRanges.upper_bound(value);
    if (found == tracker.freedRanges.begin()) return nullptr;
    --found;
    if (value >= found->second.end) return nullptr;
    start = found->first;
    return &found->second;
}

void scanAddresses(Tracker& tracker, VkCommandBuffer commands, const char* command, const void* data, std::size_t bytes, std::size_t step = 4) {
    if (tracker.freedRanges.empty() || data == nullptr) return;
    const auto* words = static_cast<const std::byte*>(data);
    for (std::size_t offset = 0; offset + 8 <= bytes; offset += step) {
        std::uint64_t value = 0;
        std::memcpy(&value, words + offset, sizeof(value));
        if (value == 0) continue;
        std::uint64_t start = 0;
        const auto* range = freedRangeAt(tracker, value, start);
        if (range == nullptr || !reportAllowed()) continue;
        std::array<void*, 8> frames{};
        int depth = 0;
        capture(frames, depth);
        std::fprintf(stderr, "[stale] %s passes device address 0x%llx (byte %zu) inside [0x%llx, 0x%llx) of buffer 0x%llx, whose memory 0x%llx was freed %.1f ms earlier, to command buffer %p; recorded from", command, static_cast<unsigned long long>(value), offset, static_cast<unsigned long long>(start), static_cast<unsigned long long>(range->end), static_cast<unsigned long long>(range->buffer), static_cast<unsigned long long>(range->memory), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - range->freed.at).count(), static_cast<void*>(commands));
        printFrames(frames, depth);
        std::fprintf(stderr, "; freed from");
        printFrames(range->freed.frames, range->freed.depth);
        std::fprintf(stderr, "\n");
    }
}

VKAPI_ATTR VkDeviceAddress VKAPI_CALL staleGetBufferDeviceAddress(VkDevice device, const VkBufferDeviceAddressInfo* info) {
    const auto address = real_vkGetBufferDeviceAddressKHR(device, info);
    if (address != 0) {
        auto& tracker = Track();
        std::lock_guard lock(tracker.mutex);
        const auto size = tracker.bufferSize.contains(key(info->buffer)) ? tracker.bufferSize[key(info->buffer)] : 1u;
        tracker.bufferAddress[key(info->buffer)] = {address, address + size};
        for (auto it = tracker.freedRanges.lower_bound(address); it != tracker.freedRanges.end() && it->first < address + size;) it = tracker.freedRanges.erase(it);
        if (auto it = tracker.freedRanges.lower_bound(address); it != tracker.freedRanges.begin() && std::prev(it)->second.end > address) tracker.freedRanges.erase(std::prev(it));
    }
    return address;
}

VKAPI_ATTR void VKAPI_CALL staleCmdPushConstants(VkCommandBuffer commands, VkPipelineLayout layout, VkShaderStageFlags stages, std::uint32_t offset, std::uint32_t size, const void* values) {
    {
        auto& tracker = Track();
        std::lock_guard lock(tracker.mutex);
        scanAddresses(tracker, commands, "vkCmdPushConstants", values, size);
    }
    real_vkCmdPushConstants(commands, layout, stages, offset, size, values);
}

VKAPI_ATTR VkResult VKAPI_CALL staleAllocateMemory(VkDevice device, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks* allocator, VkDeviceMemory* memory) {
    const auto result = real_vkAllocateMemory(device, info, allocator, memory);
    if (result == VK_SUCCESS) {
        auto& tracker = Track();
        std::lock_guard lock(tracker.mutex);
        tracker.freedMemory.erase(key(*memory));
        tracker.memorySize[key(*memory)] = info->allocationSize;
        tracker.memoryMapping.erase(key(*memory));
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL staleBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset) {
    {
        auto& tracker = Track();
        std::lock_guard lock(tracker.mutex);
        tracker.bufferMemory[key(buffer)] = key(memory);
        tracker.bufferOffset[key(buffer)] = offset;
        tracker.bufferBound[key(buffer)] = std::chrono::steady_clock::now();
    }
    return real_vkBindBufferMemory(device, buffer, memory, offset);
}

VKAPI_ATTR VkResult VKAPI_CALL staleBindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize offset) {
    {
        auto& tracker = Track();
        std::lock_guard lock(tracker.mutex);
        tracker.imageMemory[key(image)] = key(memory);
    }
    return real_vkBindImageMemory(device, image, memory, offset);
}

VKAPI_ATTR VkResult VKAPI_CALL staleCreateBuffer(VkDevice device, const VkBufferCreateInfo* info, const VkAllocationCallbacks* allocator, VkBuffer* buffer) {
    const auto result = real_vkCreateBuffer(device, info, allocator, buffer);
    if (result == VK_SUCCESS) {
        auto& tracker = Track();
        std::lock_guard lock(tracker.mutex);
        tracker.deadObjects.erase(key(*buffer));
        tracker.bufferMemory.erase(key(*buffer));
        tracker.bufferAddress.erase(key(*buffer));
        tracker.bufferSize[key(*buffer)] = info->size;
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL staleCreateImage(VkDevice device, const VkImageCreateInfo* info, const VkAllocationCallbacks* allocator, VkImage* image) {
    const auto result = real_vkCreateImage(device, info, allocator, image);
    if (result == VK_SUCCESS) {
        auto& tracker = Track();
        std::lock_guard lock(tracker.mutex);
        tracker.deadObjects.erase(key(*image));
        tracker.imageMemory.erase(key(*image));
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL staleCreateImageView(VkDevice device, const VkImageViewCreateInfo* info, const VkAllocationCallbacks* allocator, VkImageView* view) {
    const auto result = real_vkCreateImageView(device, info, allocator, view);
    if (result == VK_SUCCESS) {
        auto& tracker = Track();
        std::lock_guard lock(tracker.mutex);
        tracker.deadObjects.erase(key(*view));
        tracker.viewImage[key(*view)] = key(info->image);
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL staleCreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo* info, const VkAllocationCallbacks* allocator, VkFramebuffer* framebuffer) {
    const auto result = real_vkCreateFramebuffer(device, info, allocator, framebuffer);
    if (result == VK_SUCCESS) {
        auto& tracker = Track();
        std::lock_guard lock(tracker.mutex);
        auto& views = tracker.framebufferViews[key(*framebuffer)];
        views.clear();
        for (std::uint32_t i = 0; i < info->attachmentCount; ++i) views.push_back(key(info->pAttachments[i]));
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL staleDestroyFramebuffer(VkDevice device, VkFramebuffer framebuffer, const VkAllocationCallbacks* allocator) {
    {
        auto& tracker = Track();
        std::lock_guard lock(tracker.mutex);
        tracker.framebufferViews.erase(key(framebuffer));
    }
    real_vkDestroyFramebuffer(device, framebuffer, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL staleAllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo* info, VkDescriptorSet* sets) {
    const auto result = real_vkAllocateDescriptorSets(device, info, sets);
    if (result == VK_SUCCESS) {
        auto& tracker = Track();
        std::lock_guard lock(tracker.mutex);
        for (std::uint32_t i = 0; i < info->descriptorSetCount; ++i) tracker.setRefs.erase(key(sets[i]));
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL staleUpdateDescriptorSets(VkDevice device, std::uint32_t writeCount, const VkWriteDescriptorSet* writes, std::uint32_t copyCount, const VkCopyDescriptorSet* copies) {
    {
        auto& tracker = Track();
        std::lock_guard lock(tracker.mutex);
        for (std::uint32_t w = 0; w < writeCount; ++w) {
            const auto& write = writes[w];
            auto& refs = tracker.setRefs[key(write.dstSet)];
            for (std::uint32_t i = 0; i < write.descriptorCount; ++i) {
                const auto slot = (static_cast<std::uint64_t>(write.dstBinding) << 32u) | (write.dstArrayElement + i);
                std::pair<StaleObject, std::uint64_t> ref{StaleObject::Memory, 0};
                if (write.pBufferInfo != nullptr && (write.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || write.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || write.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC || write.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)) ref = {StaleObject::Buffer, key(write.pBufferInfo[i].buffer)};
                else if (write.pImageInfo != nullptr && write.descriptorType != VK_DESCRIPTOR_TYPE_SAMPLER) ref = {StaleObject::ImageView, key(write.pImageInfo[i].imageView)};
                else if (write.pTexelBufferView != nullptr) ref = {StaleObject::BufferView, key(write.pTexelBufferView[i])};
                if (ref.second != 0) refs[slot] = ref;
                else refs.erase(slot);
            }
        }
        for (std::uint32_t c = 0; c < copyCount; ++c) {
            const auto& copy = copies[c];
            const auto source = tracker.setRefs.find(key(copy.srcSet));
            for (std::uint32_t i = 0; i < copy.descriptorCount; ++i) {
                const auto from = (static_cast<std::uint64_t>(copy.srcBinding) << 32u) | (copy.srcArrayElement + i);
                const auto to = (static_cast<std::uint64_t>(copy.dstBinding) << 32u) | (copy.dstArrayElement + i);
                if (source == tracker.setRefs.end()) continue;
                const auto found = source->second.find(from);
                if (found != source->second.end()) tracker.setRefs[key(copy.dstSet)][to] = found->second;
            }
        }
    }
    real_vkUpdateDescriptorSets(device, writeCount, writes, copyCount, copies);
}

VKAPI_ATTR VkResult VKAPI_CALL staleBeginCommandBuffer(VkCommandBuffer commands, const VkCommandBufferBeginInfo* info) {
    {
        auto& tracker = Track();
        std::lock_guard lock(tracker.mutex);
        auto& refs = tracker.commands[key(commands)];
        refs.memories.clear();
        refs.buffers.clear();
        refs.submitted = false;
        refs.fence = 0;
        refs.begun = std::chrono::steady_clock::now();
    }
    return real_vkBeginCommandBuffer(commands, info);
}

VKAPI_ATTR VkResult VKAPI_CALL staleResetCommandBuffer(VkCommandBuffer commands, VkCommandBufferResetFlags flags) {
    {
        auto& tracker = Track();
        std::lock_guard lock(tracker.mutex);
        tracker.commands.erase(key(commands));
    }
    return real_vkResetCommandBuffer(commands, flags);
}

VKAPI_ATTR void VKAPI_CALL staleFreeCommandBuffers(VkDevice device, VkCommandPool pool, std::uint32_t count, const VkCommandBuffer* buffers) {
    {
        auto& tracker = Track();
        std::lock_guard lock(tracker.mutex);
        for (std::uint32_t i = 0; i < count; ++i) tracker.commands.erase(key(buffers[i]));
    }
    real_vkFreeCommandBuffers(device, pool, count, buffers);
}

VKAPI_ATTR VkResult VKAPI_CALL staleQueueSubmit(VkQueue queue, std::uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    {
        auto& tracker = Track();
        std::lock_guard lock(tracker.mutex);
        for (std::uint32_t s = 0; s < count; ++s) {
            for (std::uint32_t c = 0; c < submits[s].commandBufferCount; ++c) {
                auto& refs = tracker.commands[key(submits[s].pCommandBuffers[c])];
                refs.submitted = true;
                refs.fence = key(fence);
                if (!tracker.freedRanges.empty()) {
                    for (const auto buffer : refs.buffers) {
                        const auto memory = tracker.bufferMemory.find(buffer);
                        if (memory == tracker.bufferMemory.end()) continue;
                        const auto mapping = tracker.memoryMapping.find(memory->second);
                        if (mapping == tracker.memoryMapping.end()) continue;
                        const auto mapOffset = tracker.mappingOffset[memory->second];
                        const auto offset = tracker.bufferOffset[buffer];
                        const auto size = tracker.bufferSize.contains(buffer) ? tracker.bufferSize[buffer] : 0u;
                        if (offset < mapOffset || offset - mapOffset >= mapping->second.second || size == 0) continue;
                        const auto bytes = std::min<std::uint64_t>({size, mapping->second.second - (offset - mapOffset), 4u << 20u});
                        char label[160];
                        const auto bound = tracker.bufferBound.find(buffer);
                        std::snprintf(label, sizeof(label), "vkQueueSubmit (mapped contents of used buffer 0x%llx, %llu bytes, bound to memory %.1f ms earlier)", static_cast<unsigned long long>(buffer), static_cast<unsigned long long>(size), bound != tracker.bufferBound.end() ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - bound->second).count() : -1.0);
                        scanAddresses(tracker, submits[s].pCommandBuffers[c], label, mapping->second.first + (offset - mapOffset), static_cast<std::size_t>(bytes), 8);
                    }
                }
                for (const auto& [memory, handle] : refs.memories) {
                    const auto freed = tracker.freedMemory.find(memory);
                    if (freed == tracker.freedMemory.end() || !reportAllowed()) continue;
                    std::fprintf(stderr, "[stale] command buffer %p is submitted although memory 0x%llx it uses (through 0x%llx) was freed %.1f ms earlier; freed from", static_cast<void*>(submits[s].pCommandBuffers[c]), static_cast<unsigned long long>(memory), static_cast<unsigned long long>(handle), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - freed->second.at).count());
                    printFrames(freed->second.frames, freed->second.depth);
                    std::fprintf(stderr, "\n");
                }
            }
        }
    }
    return real_vkQueueSubmit(queue, count, submits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL staleWaitForFences(VkDevice device, std::uint32_t count, const VkFence* fences, VkBool32 waitAll, std::uint64_t timeout) {
    const auto result = real_vkWaitForFences(device, count, fences, waitAll, timeout);
    if (result == VK_SUCCESS && (waitAll == VK_TRUE || count == 1)) {
        auto& tracker = Track();
        std::lock_guard lock(tracker.mutex);
        for (std::uint32_t i = 0; i < count; ++i) completeFence(tracker, key(fences[i]));
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL staleGetFenceStatus(VkDevice device, VkFence fence) {
    const auto result = real_vkGetFenceStatus(device, fence);
    if (result == VK_SUCCESS) {
        auto& tracker = Track();
        std::lock_guard lock(tracker.mutex);
        completeFence(tracker, key(fence));
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL staleResetFences(VkDevice device, std::uint32_t count, const VkFence* fences) {
    {
        auto& tracker = Track();
        std::lock_guard lock(tracker.mutex);
        for (std::uint32_t i = 0; i < count; ++i) completeFence(tracker, key(fences[i]));
    }
    return real_vkResetFences(device, count, fences);
}

void completeAll() {
    auto& tracker = Track();
    std::lock_guard lock(tracker.mutex);
    for (auto& [commands, refs] : tracker.commands) {
        if (!refs.submitted) continue;
        refs.memories.clear();
        refs.buffers.clear();
        refs.submitted = false;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL staleQueueWaitIdle(VkQueue queue) {
    const auto result = real_vkQueueWaitIdle(queue);
    if (result == VK_SUCCESS) completeAll();
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL staleDeviceWaitIdle(VkDevice device) {
    const auto result = real_vkDeviceWaitIdle(device);
    if (result == VK_SUCCESS) completeAll();
    return result;
}

#define STALE_LOCK auto& tracker = Track(); std::lock_guard lock(tracker.mutex)

VKAPI_ATTR void VKAPI_CALL staleCmdBindDescriptorSets(VkCommandBuffer commands, VkPipelineBindPoint bindPoint, VkPipelineLayout layout, std::uint32_t first, std::uint32_t count, const VkDescriptorSet* sets, std::uint32_t dynamicCount, const std::uint32_t* dynamicOffsets) {
    {
        STALE_LOCK;
        for (std::uint32_t i = 0; i < count; ++i) referenceSet(tracker, commands, "vkCmdBindDescriptorSets", sets[i]);
    }
    real_vkCmdBindDescriptorSets(commands, bindPoint, layout, first, count, sets, dynamicCount, dynamicOffsets);
}

VKAPI_ATTR void VKAPI_CALL staleCmdBindVertexBuffers(VkCommandBuffer commands, std::uint32_t first, std::uint32_t count, const VkBuffer* buffers, const VkDeviceSize* offsets) {
    {
        STALE_LOCK;
        for (std::uint32_t i = 0; i < count; ++i) reference(tracker, commands, "vkCmdBindVertexBuffers", StaleObject::Buffer, key(buffers[i]));
    }
    real_vkCmdBindVertexBuffers(commands, first, count, buffers, offsets);
}

VKAPI_ATTR void VKAPI_CALL staleCmdBindIndexBuffer(VkCommandBuffer commands, VkBuffer buffer, VkDeviceSize offset, VkIndexType type) {
    {
        STALE_LOCK;
        reference(tracker, commands, "vkCmdBindIndexBuffer", StaleObject::Buffer, key(buffer));
    }
    real_vkCmdBindIndexBuffer(commands, buffer, offset, type);
}

VKAPI_ATTR void VKAPI_CALL staleCmdCopyBuffer(VkCommandBuffer commands, VkBuffer source, VkBuffer destination, std::uint32_t count, const VkBufferCopy* regions) {
    {
        STALE_LOCK;
        reference(tracker, commands, "vkCmdCopyBuffer", StaleObject::Buffer, key(source));
        reference(tracker, commands, "vkCmdCopyBuffer", StaleObject::Buffer, key(destination));
    }
    real_vkCmdCopyBuffer(commands, source, destination, count, regions);
}

VKAPI_ATTR void VKAPI_CALL staleCmdCopyBufferToImage(VkCommandBuffer commands, VkBuffer source, VkImage destination, VkImageLayout layout, std::uint32_t count, const VkBufferImageCopy* regions) {
    {
        STALE_LOCK;
        reference(tracker, commands, "vkCmdCopyBufferToImage", StaleObject::Buffer, key(source));
        reference(tracker, commands, "vkCmdCopyBufferToImage", StaleObject::Image, key(destination));
    }
    real_vkCmdCopyBufferToImage(commands, source, destination, layout, count, regions);
}

VKAPI_ATTR void VKAPI_CALL staleCmdCopyImageToBuffer(VkCommandBuffer commands, VkImage source, VkImageLayout layout, VkBuffer destination, std::uint32_t count, const VkBufferImageCopy* regions) {
    {
        STALE_LOCK;
        reference(tracker, commands, "vkCmdCopyImageToBuffer", StaleObject::Image, key(source));
        reference(tracker, commands, "vkCmdCopyImageToBuffer", StaleObject::Buffer, key(destination));
    }
    real_vkCmdCopyImageToBuffer(commands, source, layout, destination, count, regions);
}

VKAPI_ATTR void VKAPI_CALL staleCmdCopyImage(VkCommandBuffer commands, VkImage source, VkImageLayout sourceLayout, VkImage destination, VkImageLayout destinationLayout, std::uint32_t count, const VkImageCopy* regions) {
    {
        STALE_LOCK;
        reference(tracker, commands, "vkCmdCopyImage", StaleObject::Image, key(source));
        reference(tracker, commands, "vkCmdCopyImage", StaleObject::Image, key(destination));
    }
    real_vkCmdCopyImage(commands, source, sourceLayout, destination, destinationLayout, count, regions);
}

VKAPI_ATTR void VKAPI_CALL staleCmdBlitImage(VkCommandBuffer commands, VkImage source, VkImageLayout sourceLayout, VkImage destination, VkImageLayout destinationLayout, std::uint32_t count, const VkImageBlit* regions, VkFilter filter) {
    {
        STALE_LOCK;
        reference(tracker, commands, "vkCmdBlitImage", StaleObject::Image, key(source));
        reference(tracker, commands, "vkCmdBlitImage", StaleObject::Image, key(destination));
    }
    real_vkCmdBlitImage(commands, source, sourceLayout, destination, destinationLayout, count, regions, filter);
}

VKAPI_ATTR void VKAPI_CALL staleCmdFillBuffer(VkCommandBuffer commands, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, std::uint32_t data) {
    {
        STALE_LOCK;
        reference(tracker, commands, "vkCmdFillBuffer", StaleObject::Buffer, key(buffer));
    }
    real_vkCmdFillBuffer(commands, buffer, offset, size, data);
}

VKAPI_ATTR void VKAPI_CALL staleCmdUpdateBuffer(VkCommandBuffer commands, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, const void* data) {
    {
        STALE_LOCK;
        reference(tracker, commands, "vkCmdUpdateBuffer", StaleObject::Buffer, key(buffer));
        scanAddresses(tracker, commands, "vkCmdUpdateBuffer", data, static_cast<std::size_t>(size));
    }
    real_vkCmdUpdateBuffer(commands, buffer, offset, size, data);
}

VKAPI_ATTR void VKAPI_CALL staleCmdClearColorImage(VkCommandBuffer commands, VkImage image, VkImageLayout layout, const VkClearColorValue* color, std::uint32_t count, const VkImageSubresourceRange* ranges) {
    {
        STALE_LOCK;
        reference(tracker, commands, "vkCmdClearColorImage", StaleObject::Image, key(image));
    }
    real_vkCmdClearColorImage(commands, image, layout, color, count, ranges);
}

VKAPI_ATTR void VKAPI_CALL staleCmdClearDepthStencilImage(VkCommandBuffer commands, VkImage image, VkImageLayout layout, const VkClearDepthStencilValue* value, std::uint32_t count, const VkImageSubresourceRange* ranges) {
    {
        STALE_LOCK;
        reference(tracker, commands, "vkCmdClearDepthStencilImage", StaleObject::Image, key(image));
    }
    real_vkCmdClearDepthStencilImage(commands, image, layout, value, count, ranges);
}

VKAPI_ATTR void VKAPI_CALL staleCmdPipelineBarrier(VkCommandBuffer commands, VkPipelineStageFlags sourceStage, VkPipelineStageFlags destinationStage, VkDependencyFlags flags, std::uint32_t memoryCount, const VkMemoryBarrier* memoryBarriers, std::uint32_t bufferCount, const VkBufferMemoryBarrier* bufferBarriers, std::uint32_t imageCount, const VkImageMemoryBarrier* imageBarriers) {
    {
        STALE_LOCK;
        for (std::uint32_t i = 0; i < bufferCount; ++i) reference(tracker, commands, "vkCmdPipelineBarrier", StaleObject::Buffer, key(bufferBarriers[i].buffer));
        for (std::uint32_t i = 0; i < imageCount; ++i) reference(tracker, commands, "vkCmdPipelineBarrier", StaleObject::Image, key(imageBarriers[i].image));
    }
    real_vkCmdPipelineBarrier(commands, sourceStage, destinationStage, flags, memoryCount, memoryBarriers, bufferCount, bufferBarriers, imageCount, imageBarriers);
}

VKAPI_ATTR void VKAPI_CALL staleCmdDispatchIndirect(VkCommandBuffer commands, VkBuffer buffer, VkDeviceSize offset) {
    {
        STALE_LOCK;
        reference(tracker, commands, "vkCmdDispatchIndirect", StaleObject::Buffer, key(buffer));
    }
    real_vkCmdDispatchIndirect(commands, buffer, offset);
}

VKAPI_ATTR void VKAPI_CALL staleCmdDrawIndirect(VkCommandBuffer commands, VkBuffer buffer, VkDeviceSize offset, std::uint32_t count, std::uint32_t stride) {
    {
        STALE_LOCK;
        reference(tracker, commands, "vkCmdDrawIndirect", StaleObject::Buffer, key(buffer));
    }
    real_vkCmdDrawIndirect(commands, buffer, offset, count, stride);
}

VKAPI_ATTR void VKAPI_CALL staleCmdDrawIndexedIndirect(VkCommandBuffer commands, VkBuffer buffer, VkDeviceSize offset, std::uint32_t count, std::uint32_t stride) {
    {
        STALE_LOCK;
        reference(tracker, commands, "vkCmdDrawIndexedIndirect", StaleObject::Buffer, key(buffer));
    }
    real_vkCmdDrawIndexedIndirect(commands, buffer, offset, count, stride);
}

VKAPI_ATTR void VKAPI_CALL staleCmdDrawIndirectCount(VkCommandBuffer commands, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countOffset, std::uint32_t maxCount, std::uint32_t stride) {
    {
        STALE_LOCK;
        reference(tracker, commands, "vkCmdDrawIndirectCountKHR", StaleObject::Buffer, key(buffer));
        reference(tracker, commands, "vkCmdDrawIndirectCountKHR", StaleObject::Buffer, key(countBuffer));
    }
    real_vkCmdDrawIndirectCountKHR(commands, buffer, offset, countBuffer, countOffset, maxCount, stride);
}

VKAPI_ATTR void VKAPI_CALL staleCmdDrawIndexedIndirectCount(VkCommandBuffer commands, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countOffset, std::uint32_t maxCount, std::uint32_t stride) {
    {
        STALE_LOCK;
        reference(tracker, commands, "vkCmdDrawIndexedIndirectCountKHR", StaleObject::Buffer, key(buffer));
        reference(tracker, commands, "vkCmdDrawIndexedIndirectCountKHR", StaleObject::Buffer, key(countBuffer));
    }
    real_vkCmdDrawIndexedIndirectCountKHR(commands, buffer, offset, countBuffer, countOffset, maxCount, stride);
}

VKAPI_ATTR void VKAPI_CALL staleCmdDrawMeshTasksIndirect(VkCommandBuffer commands, VkBuffer buffer, VkDeviceSize offset, std::uint32_t count, std::uint32_t stride) {
    {
        STALE_LOCK;
        reference(tracker, commands, "vkCmdDrawMeshTasksIndirectEXT", StaleObject::Buffer, key(buffer));
    }
    real_vkCmdDrawMeshTasksIndirectEXT(commands, buffer, offset, count, stride);
}

VKAPI_ATTR void VKAPI_CALL staleCmdBeginRenderPass(VkCommandBuffer commands, const VkRenderPassBeginInfo* info, VkSubpassContents contents) {
    {
        STALE_LOCK;
        const auto found = tracker.framebufferViews.find(key(info->framebuffer));
        if (found != tracker.framebufferViews.end()) {
            for (const auto view : found->second) reference(tracker, commands, "vkCmdBeginRenderPass", StaleObject::ImageView, view);
        }
    }
    real_vkCmdBeginRenderPass(commands, info, contents);
}

VKAPI_ATTR void VKAPI_CALL staleCmdBeginRendering(VkCommandBuffer commands, const VkRenderingInfo* info) {
    {
        STALE_LOCK;
        for (std::uint32_t i = 0; i < info->colorAttachmentCount; ++i) reference(tracker, commands, "vkCmdBeginRenderingKHR", StaleObject::ImageView, key(info->pColorAttachments[i].imageView));
        if (info->pDepthAttachment != nullptr) reference(tracker, commands, "vkCmdBeginRenderingKHR", StaleObject::ImageView, key(info->pDepthAttachment->imageView));
        if (info->pStencilAttachment != nullptr) reference(tracker, commands, "vkCmdBeginRenderingKHR", StaleObject::ImageView, key(info->pStencilAttachment->imageView));
    }
    real_vkCmdBeginRenderingKHR(commands, info);
}

VKAPI_ATTR void VKAPI_CALL staleCmdCopyQueryPoolResults(VkCommandBuffer commands, VkQueryPool pool, std::uint32_t first, std::uint32_t count, VkBuffer destination, VkDeviceSize offset, VkDeviceSize stride, VkQueryResultFlags flags) {
    {
        STALE_LOCK;
        reference(tracker, commands, "vkCmdCopyQueryPoolResults", StaleObject::Buffer, key(destination));
    }
    real_vkCmdCopyQueryPoolResults(commands, pool, first, count, destination, offset, stride, flags);
}

}

bool StaleHandlesTraced() {
    static const bool traced = std::getenv("APS5_TRACE_STALE_HANDLES") != nullptr;
    return traced;
}

std::uint64_t StaleHandleReports() {
    return Track().reports.load(std::memory_order_relaxed);
}

void StaleHandleRelease(StaleObject kind, std::uint64_t handle) {
    if (!StaleHandlesTraced() || handle == 0) return;
    auto& tracker = Track();
    std::lock_guard lock(tracker.mutex);
    if (kind != StaleObject::Memory) {
        tracker.deadObjects.insert(handle);
        return;
    }
    Freed freed;
    freed.at = std::chrono::steady_clock::now();
    capture(freed.frames, freed.depth);
    for (const auto& [commands, refs] : tracker.commands) {
        const auto found = refs.memories.find(handle);
        if (found == refs.memories.end() || !reportAllowed()) continue;
        std::fprintf(stderr, "[stale] memory 0x%llx is freed while command buffer %p (%s, begun %.1f ms earlier) still uses it through 0x%llx; freed from", static_cast<unsigned long long>(handle), reinterpret_cast<void*>(commands), refs.submitted ? "submitted" : "recording", std::chrono::duration<double, std::milli>(freed.at - refs.begun).count(), static_cast<unsigned long long>(found->second));
        printFrames(freed.frames, freed.depth);
        std::fprintf(stderr, "\n");
    }
    tracker.freedMemory[handle] = freed;
    tracker.memoryMapping.erase(handle);
    for (const auto& [buffer, memory] : tracker.bufferMemory) {
        if (memory != handle) continue;
        const auto address = tracker.bufferAddress.find(buffer);
        if (address == tracker.bufferAddress.end()) continue;
        AddressRange range{address->second.second, buffer, handle, freed};
        tracker.freedRanges[address->second.first] = range;
        tracker.freedRing[tracker.freedRingCount++ % tracker.freedRing.size()] = {address->second.first, range};
    }
}

double StaleAddressFreedMs(std::uint64_t address) {
    if (!StaleHandlesTraced() || address == 0) return -1.0;
    auto& tracker = Track();
    std::lock_guard lock(tracker.mutex);
    std::uint64_t start = 0;
    const auto* range = freedRangeAt(tracker, address, start);
    return range != nullptr ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - range->freed.at).count() : -1.0;
}

void StaleHandleDump(const char* why) {
    if (!StaleHandlesTraced()) return;
    auto& tracker = Track();
    if (tracker.dumped.exchange(true)) return;
    std::unique_lock lock(tracker.mutex, std::defer_lock);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (!lock.try_lock() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    const auto now = std::chrono::steady_clock::now();
    const auto shown = std::min<std::uint64_t>(tracker.freedRingCount, tracker.freedRing.size());
    std::fprintf(stderr, "[stale] %s: %llu frees of memory behind a queried device address, the last %llu oldest first\n", why, static_cast<unsigned long long>(tracker.freedRingCount), static_cast<unsigned long long>(shown));
    for (std::uint64_t n = tracker.freedRingCount - shown; n < tracker.freedRingCount; ++n) {
        const auto& [start, range] = tracker.freedRing[n % tracker.freedRing.size()];
        std::fprintf(stderr, "[stale]   -%.1f ms [0x%llx, 0x%llx) buffer 0x%llx memory 0x%llx freed from", std::chrono::duration<double, std::milli>(now - range.freed.at).count(), static_cast<unsigned long long>(start), static_cast<unsigned long long>(range.end), static_cast<unsigned long long>(range.buffer), static_cast<unsigned long long>(range.memory));
        printFrames(range.freed.frames, range.freed.depth);
        std::fprintf(stderr, "\n");
    }
}

PFN_vkVoidFunction StaleHandleWrapper(const char* name, PFN_vkVoidFunction real) {
    if (!StaleHandlesTraced() || name == nullptr || real == nullptr) return nullptr;
    const std::string_view wanted(name);
#define STALE_WRAP(vkName, wrapper) \
    if (wanted == #vkName) { \
        real_##vkName = reinterpret_cast<PFN_##vkName>(real); \
        return reinterpret_cast<PFN_vkVoidFunction>(&wrapper); \
    }
    STALE_WRAP(vkAllocateMemory, staleAllocateMemory)
    STALE_WRAP(vkBindBufferMemory, staleBindBufferMemory)
    STALE_WRAP(vkBindImageMemory, staleBindImageMemory)
    STALE_WRAP(vkCreateBuffer, staleCreateBuffer)
    STALE_WRAP(vkCreateImage, staleCreateImage)
    STALE_WRAP(vkCreateImageView, staleCreateImageView)
    STALE_WRAP(vkCreateFramebuffer, staleCreateFramebuffer)
    STALE_WRAP(vkDestroyFramebuffer, staleDestroyFramebuffer)
    STALE_WRAP(vkAllocateDescriptorSets, staleAllocateDescriptorSets)
    STALE_WRAP(vkUpdateDescriptorSets, staleUpdateDescriptorSets)
    STALE_WRAP(vkBeginCommandBuffer, staleBeginCommandBuffer)
    STALE_WRAP(vkResetCommandBuffer, staleResetCommandBuffer)
    STALE_WRAP(vkFreeCommandBuffers, staleFreeCommandBuffers)
    STALE_WRAP(vkQueueSubmit, staleQueueSubmit)
    STALE_WRAP(vkWaitForFences, staleWaitForFences)
    STALE_WRAP(vkGetFenceStatus, staleGetFenceStatus)
    STALE_WRAP(vkResetFences, staleResetFences)
    STALE_WRAP(vkQueueWaitIdle, staleQueueWaitIdle)
    STALE_WRAP(vkDeviceWaitIdle, staleDeviceWaitIdle)
    STALE_WRAP(vkCmdBindDescriptorSets, staleCmdBindDescriptorSets)
    STALE_WRAP(vkCmdBindVertexBuffers, staleCmdBindVertexBuffers)
    STALE_WRAP(vkCmdBindIndexBuffer, staleCmdBindIndexBuffer)
    STALE_WRAP(vkCmdCopyBuffer, staleCmdCopyBuffer)
    STALE_WRAP(vkCmdCopyBufferToImage, staleCmdCopyBufferToImage)
    STALE_WRAP(vkCmdCopyImageToBuffer, staleCmdCopyImageToBuffer)
    STALE_WRAP(vkCmdCopyImage, staleCmdCopyImage)
    STALE_WRAP(vkCmdBlitImage, staleCmdBlitImage)
    STALE_WRAP(vkCmdFillBuffer, staleCmdFillBuffer)
    STALE_WRAP(vkCmdUpdateBuffer, staleCmdUpdateBuffer)
    STALE_WRAP(vkCmdClearColorImage, staleCmdClearColorImage)
    STALE_WRAP(vkCmdClearDepthStencilImage, staleCmdClearDepthStencilImage)
    STALE_WRAP(vkCmdPipelineBarrier, staleCmdPipelineBarrier)
    STALE_WRAP(vkCmdDispatchIndirect, staleCmdDispatchIndirect)
    STALE_WRAP(vkCmdDrawIndirect, staleCmdDrawIndirect)
    STALE_WRAP(vkCmdDrawIndexedIndirect, staleCmdDrawIndexedIndirect)
    STALE_WRAP(vkCmdDrawIndirectCountKHR, staleCmdDrawIndirectCount)
    STALE_WRAP(vkCmdDrawIndexedIndirectCountKHR, staleCmdDrawIndexedIndirectCount)
    STALE_WRAP(vkCmdDrawMeshTasksIndirectEXT, staleCmdDrawMeshTasksIndirect)
    STALE_WRAP(vkCmdBeginRenderPass, staleCmdBeginRenderPass)
    STALE_WRAP(vkCmdBeginRenderingKHR, staleCmdBeginRendering)
    STALE_WRAP(vkCmdCopyQueryPoolResults, staleCmdCopyQueryPoolResults)
    STALE_WRAP(vkGetBufferDeviceAddressKHR, staleGetBufferDeviceAddress)
    STALE_WRAP(vkCmdPushConstants, staleCmdPushConstants)
    STALE_WRAP(vkMapMemory, staleMapMemory)
    STALE_WRAP(vkUnmapMemory, staleUnmapMemory)
#undef STALE_WRAP
    return nullptr;
}

}
