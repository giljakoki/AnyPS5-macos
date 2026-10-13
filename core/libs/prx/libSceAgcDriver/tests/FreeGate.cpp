// The free gate (Graphics/src/GpuFreeTrace.cpp): Vulkan memory and query pools are freed only once
// every queue submission made before the free completed. Part one drives the gate through fake
// device functions; part two (a Vulkan device with buffer device addresses and timeline semaphores)
// frees 64 unrelated GPU-addressable buffers while a dispatch that dereferences a device address
// runs, which loses a MoltenVK device without the gate (kIOGPUCommandBufferCallbackErrorInvalidResource).
#include "prx/libSceAgcDriver/Graphics/include/GpuFreeTrace.hpp"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#ifndef _WIN32
#include <dlfcn.h>
#endif

using namespace AgcDriver::Graphics;

namespace {

int failures = 0;

void Expect(bool condition, const std::string& what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++failures;
}

template <typename T> T handleOf(std::uintptr_t value) { return reinterpret_cast<T>(value); }

struct Fake {
    int frees = 0;
    int poolsDestroyed = 0;
    int idles = 0;
    int semaphoresDestroyed = 0;
    std::uint64_t counter = 0;
    std::uint32_t lastCount = 0;
    std::uint64_t lastSignal = 0;
    bool lastSignalsTimeline = false;
} fake;

const auto FakeQueue = handleOf<VkQueue>(0x4000);
const auto FakeTimeline = handleOf<VkSemaphore>(0x7700);

VKAPI_ATTR void VKAPI_CALL fakeFreeMemory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*) { ++fake.frees; }
VKAPI_ATTR void VKAPI_CALL fakeDestroyQueryPool(VkDevice, VkQueryPool, const VkAllocationCallbacks*) { ++fake.poolsDestroyed; }
VKAPI_ATTR void VKAPI_CALL fakeDestroyDevice(VkDevice, const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL fakeDeviceWaitIdle(VkDevice) {
    ++fake.idles;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL fakeGetDeviceQueue(VkDevice, std::uint32_t, std::uint32_t, VkQueue* queue) { *queue = FakeQueue; }
VKAPI_ATTR VkResult VKAPI_CALL fakeCreateSemaphore(VkDevice, const VkSemaphoreCreateInfo* info, const VkAllocationCallbacks*, VkSemaphore* semaphore) {
    const auto* type = static_cast<const VkSemaphoreTypeCreateInfo*>(info->pNext);
    if (type == nullptr || type->sType != VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO || type->semaphoreType != VK_SEMAPHORE_TYPE_TIMELINE) return VK_ERROR_FEATURE_NOT_PRESENT;
    *semaphore = FakeTimeline;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL fakeDestroySemaphore(VkDevice, VkSemaphore, const VkAllocationCallbacks*) { ++fake.semaphoresDestroyed; }
VKAPI_ATTR VkResult VKAPI_CALL fakeCounterValue(VkDevice, VkSemaphore, std::uint64_t* value) {
    *value = fake.counter;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL fakeQueueSubmit(VkQueue, std::uint32_t count, const VkSubmitInfo* submits, VkFence) {
    fake.lastCount = count;
    fake.lastSignalsTimeline = false;
    fake.lastSignal = 0;
    if (count == 0) return VK_SUCCESS;
    const auto& last = submits[count - 1];
    const auto* values = static_cast<const VkTimelineSemaphoreSubmitInfo*>(last.pNext);
    fake.lastSignalsTimeline = last.commandBufferCount == 0 && last.signalSemaphoreCount == 1 && last.pSignalSemaphores[0] == FakeTimeline && values != nullptr && values->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO && values->signalSemaphoreValueCount == 1;
    fake.lastSignal = fake.lastSignalsTimeline ? values->pSignalSemaphoreValues[0] : 0;
    return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fakeDeviceProc(VkDevice, const char* name) {
    const std::string_view wanted(name);
#define FAKE(vkName, function) \
    if (wanted == #vkName) return reinterpret_cast<PFN_vkVoidFunction>(&function);
    FAKE(vkFreeMemory, fakeFreeMemory)
    FAKE(vkDestroyQueryPool, fakeDestroyQueryPool)
    FAKE(vkDestroyDevice, fakeDestroyDevice)
    FAKE(vkDeviceWaitIdle, fakeDeviceWaitIdle)
    FAKE(vkGetDeviceQueue, fakeGetDeviceQueue)
    FAKE(vkCreateSemaphore, fakeCreateSemaphore)
    FAKE(vkDestroySemaphore, fakeDestroySemaphore)
    FAKE(vkGetSemaphoreCounterValue, fakeCounterValue)
    FAKE(vkQueueSubmit, fakeQueueSubmit)
#undef FAKE
    return nullptr;
}

void gateSemantics() {
    const auto guarded = GuardDeviceProc(&fakeDeviceProc);
    Expect(guarded != &fakeDeviceProc, "the free gate did not wrap the device resolver");
    const auto device = handleOf<VkDevice>(0x3000);
    const auto resolve = [&](const char* name) { return guarded(device, name); };
    const auto freeMemory = reinterpret_cast<PFN_vkFreeMemory>(resolve("vkFreeMemory"));
    const auto destroyPool = reinterpret_cast<PFN_vkDestroyQueryPool>(resolve("vkDestroyQueryPool"));
    const auto submit = reinterpret_cast<PFN_vkQueueSubmit>(resolve("vkQueueSubmit"));
    const auto getQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(resolve("vkGetDeviceQueue"));
    const auto destroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(resolve("vkDestroyDevice"));
    VkQueue queue = VK_NULL_HANDLE;
    getQueue(device, 0, 0, &queue);
    Expect(queue == FakeQueue, "vkGetDeviceQueue did not reach the driver");
    const auto memory = [](std::uintptr_t n) { return handleOf<VkDeviceMemory>(n); };

    freeMemory(device, memory(0x10), nullptr);
    Expect(fake.frees == 1, "a free with no submission outstanding was held");

    const VkSubmitInfo work{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    Expect(submit(queue, 1, &work, VK_NULL_HANDLE) == VK_SUCCESS, "the gated submit failed");
    Expect(fake.lastCount == 2 && fake.lastSignalsTimeline && fake.lastSignal == 1, "a submit did not get a trailing batch signalling timeline value 1");

    freeMemory(device, memory(0x20), nullptr);
    Expect(fake.frees == 1, "memory was freed while a submission made before the free was outstanding");
    destroyPool(device, handleOf<VkQueryPool>(0x90), nullptr);
    Expect(fake.poolsDestroyed == 0, "a query pool was destroyed while a submission made before it was outstanding");

    fake.counter = 1;
    freeMemory(device, memory(0x30), nullptr);
    Expect(fake.frees == 3 && fake.poolsDestroyed == 1, "held frees were not made once their submissions completed");

    Expect(submit(queue, 0, nullptr, VK_NULL_HANDLE) == VK_SUCCESS && fake.lastCount == 1 && fake.lastSignal == 2, "an empty submit was not gated with value 2");
    Expect(submit(queue, 1, &work, VK_NULL_HANDLE) == VK_SUCCESS && fake.lastSignal == 3, "the next submit did not signal value 3");
    freeMemory(device, memory(0x40), nullptr);
    fake.counter = 2;
    Expect(submit(queue, 1, &work, VK_NULL_HANDLE) == VK_SUCCESS && fake.lastSignal == 4, "the next submit did not signal value 4");
    Expect(fake.frees == 3, "a free was made before every submission made before it completed (2 of 3)");
    fake.counter = 3;
    freeMemory(device, memory(0x50), nullptr);
    Expect(fake.frees == 4, "a held free was not made once value 3 completed, or a later free was not held for value 4");

    destroyDevice(device, nullptr);
    Expect(fake.frees == 5, "destroying the device did not make the held frees");
    Expect(fake.idles >= 1, "the device was not idled before its held frees were made");
    Expect(fake.semaphoresDestroyed == 1, "the queue's timeline semaphore was not destroyed with the device");
}

#ifndef _WIN32
const std::uint32_t BdaShader[] = {
    0x07230203, 0x00010500, 0x00070000, 0x0000001b, 0x00000000, 0x00020011, 0x00000001, 0x00020011,
    0x000014e3, 0x00020011, 0x0000000b, 0x0009000a, 0x5f565053, 0x5f52484b, 0x73796870, 0x6c616369,
    0x6f74735f, 0x65676172, 0x6675625f, 0x00726566, 0x0003000e, 0x000014e4, 0x00000001, 0x0006000f,
    0x00000005, 0x00000001, 0x6e69616d, 0x00000000, 0x00000002, 0x00060010, 0x00000001, 0x00000011,
    0x00000040, 0x00000001, 0x00000001, 0x00030047, 0x00000003, 0x00000002, 0x00050048, 0x00000003,
    0x00000000, 0x00000023, 0x00000000, 0x00030047, 0x00000004, 0x00000002, 0x00050048, 0x00000004,
    0x00000000, 0x00000023, 0x00000000, 0x00020013, 0x00000005, 0x00030021, 0x00000006, 0x00000005,
    0x00040015, 0x00000007, 0x00000020, 0x00000000, 0x00040015, 0x00000008, 0x00000040, 0x00000000,
    0x0003001e, 0x00000004, 0x00000007, 0x00040020, 0x00000009, 0x000014e5, 0x00000004, 0x0003001e,
    0x00000003, 0x00000008, 0x00040020, 0x0000000a, 0x00000009, 0x00000003, 0x00040020, 0x0000000b,
    0x00000009, 0x00000008, 0x00040020, 0x0000000c, 0x000014e5, 0x00000007, 0x0004003b, 0x0000000a,
    0x00000002, 0x00000009, 0x00040015, 0x0000000d, 0x00000020, 0x00000001, 0x0004002b, 0x0000000d,
    0x0000000e, 0x00000000, 0x0005002b, 0x00000008, 0x0000000f, 0x00000000, 0x00000000, 0x00020014,
    0x00000010, 0x00050036, 0x00000005, 0x00000001, 0x00000000, 0x00000006, 0x000200f8, 0x00000011,
    0x00050041, 0x0000000b, 0x00000012, 0x00000002, 0x0000000e, 0x0004003d, 0x00000008, 0x00000013,
    0x00000012, 0x000500ab, 0x00000010, 0x00000014, 0x00000013, 0x0000000f, 0x000300f7, 0x00000015,
    0x00000000, 0x000400fa, 0x00000014, 0x00000016, 0x00000015, 0x000200f8, 0x00000016, 0x00040078,
    0x00000009, 0x00000017, 0x00000013, 0x00050041, 0x0000000c, 0x00000018, 0x00000017, 0x0000000e,
    0x0006003d, 0x00000007, 0x00000019, 0x00000018, 0x00000002, 0x00000004, 0x00050080, 0x00000007,
    0x0000001a, 0x00000019, 0x00000019, 0x0005003e, 0x00000018, 0x0000001a, 0x00000002, 0x00000004,
    0x000200f9, 0x00000015, 0x000200f8, 0x00000015, 0x000100fd, 0x00010038
};

#define LOAD(name) auto name = reinterpret_cast<PFN_##name>(gdpa(device, #name))

// 0 passed, 77 skipped (no usable device), 1 failed.
int unrelatedAddressableFrees() {
    void* loader = dlopen("libvulkan.1.dylib", RTLD_NOW);
    if (loader == nullptr) loader = dlopen("libvulkan.so.1", RTLD_NOW);
    if (loader == nullptr) return 77;
    const auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader, "vkGetInstanceProcAddr"));
    if (gipa == nullptr) return 77;
    const auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_2;
    const char* instanceExtensions[] = {"VK_KHR_portability_enumeration"};
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
#ifdef __APPLE__
    instanceInfo.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    instanceInfo.enabledExtensionCount = 1;
    instanceInfo.ppEnabledExtensionNames = instanceExtensions;
#endif
    VkInstance instance = VK_NULL_HANDLE;
    if (createInstance == nullptr || createInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS) return 77;
    const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"));
    std::uint32_t count = 1;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    if (enumerate(instance, &count, &physical) < 0 || count == 0) return 77;
    VkPhysicalDeviceVulkan12Features supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &supported};
    reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(gipa(instance, "vkGetPhysicalDeviceFeatures2"))(physical, &features);
    if (!supported.bufferDeviceAddress || !supported.timelineSemaphore || !features.features.shaderInt64) return 77;
    const float priority = 1;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan12Features enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    enabled.bufferDeviceAddress = VK_TRUE;
    enabled.timelineSemaphore = VK_TRUE;
    VkPhysicalDeviceFeatures2 enable{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &enabled};
    enable.features.shaderInt64 = VK_TRUE;
    const char* deviceExtensions[] = {"VK_KHR_portability_subset"};
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &enable};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
#ifdef __APPLE__
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = deviceExtensions;
#endif
    VkDevice device = VK_NULL_HANDLE;
    if (reinterpret_cast<PFN_vkCreateDevice>(gipa(instance, "vkCreateDevice"))(physical, &deviceInfo, nullptr, &device) != VK_SUCCESS) return 77;
    const auto gdpa = GuardDeviceProc(reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(instance, "vkGetDeviceProcAddr")));
    LOAD(vkGetDeviceQueue); LOAD(vkCreateCommandPool); LOAD(vkAllocateCommandBuffers); LOAD(vkBeginCommandBuffer); LOAD(vkEndCommandBuffer);
    LOAD(vkCmdBindPipeline); LOAD(vkCmdDispatch); LOAD(vkCmdPushConstants); LOAD(vkQueueSubmit); LOAD(vkCreateFence); LOAD(vkWaitForFences); LOAD(vkResetFences);
    LOAD(vkCreateBuffer); LOAD(vkDestroyBuffer); LOAD(vkAllocateMemory); LOAD(vkFreeMemory); LOAD(vkBindBufferMemory); LOAD(vkGetBufferMemoryRequirements);
    LOAD(vkCreateShaderModule); LOAD(vkCreatePipelineLayout); LOAD(vkCreateComputePipelines); LOAD(vkGetBufferDeviceAddress); LOAD(vkDestroyDevice);
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, 0, 0, &queue);
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(device, &poolInfo, nullptr, &pool);
    VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = sizeof(BdaShader);
    moduleInfo.pCode = BdaShader;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &moduleInfo, nullptr, &module) != VK_SUCCESS) return 77;
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout);
    VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
    pipelineInfo.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS) return 77;
    struct Addressable {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };
    const auto makeAddressable = [&] {
        Addressable made;
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = 65536;
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        vkCreateBuffer(device, &bufferInfo, nullptr, &made.buffer);
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, made.buffer, &requirements);
        VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = static_cast<std::uint32_t>(__builtin_ctz(requirements.memoryTypeBits));
        vkAllocateMemory(device, &allocation, nullptr, &made.memory);
        vkBindBufferMemory(device, made.buffer, made.memory, 0);
        return made;
    };
    const auto target = makeAddressable();
    VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    addressInfo.buffer = target.buffer;
    const std::uint64_t address = vkGetBufferDeviceAddress(device, &addressInfo);
    VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandInfo.commandPool = pool;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = 1;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device, &commandInfo, &commands);
    const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(commands, &begin);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdPushConstants(commands, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, &address);
    for (int i = 0; i < 3000; ++i) vkCmdDispatch(commands, 4096, 1, 1);
    vkEndCommandBuffer(commands);
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(device, &fenceInfo, nullptr, &fence);
    int result = 0;
    for (int round = 0; round < 4 && result == 0; ++round) {
        std::vector<Addressable> unrelated;
        for (int i = 0; i < 64; ++i) unrelated.push_back(makeAddressable());
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands;
        VkResult status = vkQueueSubmit(queue, 1, &submit, fence);
        for (const auto& gone : unrelated) {
            vkDestroyBuffer(device, gone.buffer, nullptr);
            vkFreeMemory(device, gone.memory, nullptr);
        }
        if (status == VK_SUCCESS) status = vkWaitForFences(device, 1, &fence, VK_TRUE, 20'000'000'000ull);
        if (status != VK_SUCCESS) {
            std::fprintf(stderr, "FAIL: round %d: a dispatch dereferencing a device address failed (%d) after unrelated addressable memory was freed during it\n", round, static_cast<int>(status));
            result = 1;
        }
        vkResetFences(device, 1, &fence);
    }
    if (result == 0) vkDestroyDevice(device, nullptr);
    return result;
}
#endif

}

int main() {
    // APS5_FREE_GATE=0 runs only the device part, which then shows the loss the gate prevents.
    if (FreeGateEnabled()) {
        gateSemantics();
        if (failures != 0) return 1;
        std::printf("free gate semantics passed\n");
    }
#ifndef _WIN32
    const int device = unrelatedAddressableFrees();
    if (device == 77) std::printf("no Vulkan device with buffer device addresses and timeline semaphores; device part skipped\n");
    else if (device != 0) return 1;
    else std::printf("free gate device test passed\n");
#endif
    return 0;
}
