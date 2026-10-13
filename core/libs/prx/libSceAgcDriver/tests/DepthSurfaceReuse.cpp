#include "GraphicsTests.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string_view>

namespace {

using namespace AgcDriver::Graphics;

constexpr std::uint32_t FormatR8Uint = 5;
constexpr std::uint32_t FormatR16Unorm = 7;
constexpr std::uint32_t FormatR16Uint = 11;
constexpr std::uint32_t FormatR32Uint = 20;
constexpr std::uint32_t FormatR32Float = 22;
constexpr std::uint32_t FormatR11G11B10Float = 36;
constexpr std::uint32_t FormatR8G8B8A8Unorm = 56;
constexpr std::uint32_t TileDepth64KB = 0x18;
constexpr std::uint32_t TileRenderTarget64KB = 0x1b;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint32_t Type2DArray = 13;

std::uintptr_t nextHandle = 0x1000;

template<typename THandle>
THandle newHandle() {
    return reinterpret_cast<THandle>(nextHandle += 0x10);
}

VKAPI_ATTR VkResult VKAPI_CALL createImage(VkDevice, const VkImageCreateInfo*, const VkAllocationCallbacks*, VkImage* image) {
    *image = newHandle<VkImage>();
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL imageMemoryRequirements(VkDevice, VkImage, VkMemoryRequirements* requirements) {
    *requirements = {65536, 65536, 1};
}

VKAPI_ATTR VkResult VKAPI_CALL allocateMemory(VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*, VkDeviceMemory* memory) {
    *memory = newHandle<VkDeviceMemory>();
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL bindImageMemory(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize) {
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL createImageView(VkDevice, const VkImageViewCreateInfo*, const VkAllocationCallbacks*, VkImageView* view) {
    *view = newHandle<VkImageView>();
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL allocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer* commands) {
    *commands = newHandle<VkCommandBuffer>();
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL createFence(VkDevice, const VkFenceCreateInfo*, const VkAllocationCallbacks*, VkFence* fence) {
    *fence = newHandle<VkFence>();
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL beginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo*) {
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL endCommandBuffer(VkCommandBuffer) {
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL queueSubmit(VkQueue, std::uint32_t, const VkSubmitInfo*, VkFence) {
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL waitForFences(VkDevice, std::uint32_t, const VkFence*, VkBool32, std::uint64_t) {
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL getFenceStatus(VkDevice, VkFence) {
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL freeCommandBuffers(VkDevice, VkCommandPool, std::uint32_t, const VkCommandBuffer*) {}
VKAPI_ATTR void VKAPI_CALL destroyFence(VkDevice, VkFence, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL destroyImageView(VkDevice, VkImageView, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL destroyImage(VkDevice, VkImage, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL freeMemory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL cmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, std::uint32_t, const VkMemoryBarrier*, std::uint32_t, const VkBufferMemoryBarrier*, std::uint32_t, const VkImageMemoryBarrier*) {}
float lastClearDepth = -1.0f;
std::uint32_t clears = 0;

VKAPI_ATTR void VKAPI_CALL cmdClearDepthStencilImage(VkCommandBuffer, VkImage, VkImageLayout, const VkClearDepthStencilValue* value, std::uint32_t, const VkImageSubresourceRange*) {
    lastClearDepth = value->depth;
    ++clears;
}

VKAPI_ATTR void VKAPI_CALL formatProperties(VkPhysicalDevice, VkFormat, VkFormatProperties* properties) {
    *properties = {};
    properties->optimalTilingFeatures = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
}

PFN_vkVoidFunction VKAPI_CALL deviceProc(VkDevice, const char* name) {
    static const std::map<std::string_view, PFN_vkVoidFunction> table{
        {"vkCreateImage", reinterpret_cast<PFN_vkVoidFunction>(createImage)},
        {"vkGetImageMemoryRequirements", reinterpret_cast<PFN_vkVoidFunction>(imageMemoryRequirements)},
        {"vkAllocateMemory", reinterpret_cast<PFN_vkVoidFunction>(allocateMemory)},
        {"vkBindImageMemory", reinterpret_cast<PFN_vkVoidFunction>(bindImageMemory)},
        {"vkCreateImageView", reinterpret_cast<PFN_vkVoidFunction>(createImageView)},
        {"vkAllocateCommandBuffers", reinterpret_cast<PFN_vkVoidFunction>(allocateCommandBuffers)},
        {"vkCreateFence", reinterpret_cast<PFN_vkVoidFunction>(createFence)},
        {"vkBeginCommandBuffer", reinterpret_cast<PFN_vkVoidFunction>(beginCommandBuffer)},
        {"vkEndCommandBuffer", reinterpret_cast<PFN_vkVoidFunction>(endCommandBuffer)},
        {"vkQueueSubmit", reinterpret_cast<PFN_vkVoidFunction>(queueSubmit)},
        {"vkWaitForFences", reinterpret_cast<PFN_vkVoidFunction>(waitForFences)},
        {"vkGetFenceStatus", reinterpret_cast<PFN_vkVoidFunction>(getFenceStatus)},
        {"vkFreeCommandBuffers", reinterpret_cast<PFN_vkVoidFunction>(freeCommandBuffers)},
        {"vkDestroyFence", reinterpret_cast<PFN_vkVoidFunction>(destroyFence)},
        {"vkDestroyImageView", reinterpret_cast<PFN_vkVoidFunction>(destroyImageView)},
        {"vkDestroyImage", reinterpret_cast<PFN_vkVoidFunction>(destroyImage)},
        {"vkFreeMemory", reinterpret_cast<PFN_vkVoidFunction>(freeMemory)},
        {"vkCmdPipelineBarrier", reinterpret_cast<PFN_vkVoidFunction>(cmdPipelineBarrier)},
        {"vkCmdClearDepthStencilImage", reinterpret_cast<PFN_vkVoidFunction>(cmdClearDepthStencilImage)}
    };
    const auto it = table.find(name);
    return it == table.end() ? nullptr : it->second;
}

struct View {
    std::uint64_t address;
    std::uint32_t format;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t tileMode = TileDepth64KB;
    std::uint32_t type = Type2D;
    std::uint32_t lastArray = 0;
};

std::array<std::uint32_t, 8> tsharp(const View& view) {
    std::array<std::uint32_t, 8> words{};
    const auto base = view.address >> 8u;
    const auto widthMinus1 = view.width - 1u;
    const auto heightMinus1 = view.height - 1u;
    words[0] = static_cast<std::uint32_t>(base);
    words[1] = static_cast<std::uint32_t>((base >> 32u) & 0xffu) | ((view.format & 0x1ffu) << 20u) | ((widthMinus1 & 0x3u) << 30u);
    words[2] = ((widthMinus1 >> 2u) & 0xfffu) | ((heightMinus1 & 0x3fffu) << 14u);
    words[3] = 4u | (5u << 3u) | (6u << 6u) | (7u << 9u) | ((view.tileMode & 0x1fu) << 20u) | ((view.type & 0xfu) << 28u);
    words[4] = view.lastArray & 0x1fffu;
    return words;
}

std::shared_ptr<Texture> lookup(const Context& context, const View& view) {
    const auto words = tsharp(view);
    return DepthSurfaceTexture(context, words, DecodeTextureResource(words), VkComponentMapping{});
}

}

void RunDepthSurfaceReuseTests() {
    static int deviceTag = 0;
    Context context{};
    context.device = reinterpret_cast<VkDevice>(&deviceTag);
    context.deviceProc = deviceProc;
    context.formatProperties = formatProperties;
    context.memory.memoryTypeCount = 1;
    context.memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    context.limits.maxFramebufferWidth = 16384;
    context.limits.maxFramebufferHeight = 16384;
    constexpr std::uint64_t depth32 = 0x40000000;
    constexpr std::uint64_t depth16 = 0x50000000;
    constexpr std::uint64_t depthStencil = 0x60000000;
    constexpr std::uint64_t stencil = 0x60800000;
    DepthSurfaceView(context, {depth32, 0, {384, 384}, VK_FORMAT_D32_SFLOAT, 1.0f, 0});
    DepthSurfaceView(context, {depth16, 0, {256, 256}, VK_FORMAT_D16_UNORM, 1.0f, 0});
    DepthSurfaceView(context, {depthStencil, stencil, {320, 180}, VK_FORMAT_D32_SFLOAT_S8_UINT, 1.0f, 0});

    Require(lookup(context, {depth32, FormatR32Float, 384, 384}) != nullptr, "an R32F view of a D32 surface's own extent must sample its depth plane");
    Require(lookup(context, {depth16, FormatR16Unorm, 256, 256}) != nullptr, "an R16 view of a D16 surface's own extent must sample its depth plane");
    Require(lookup(context, {depth32, FormatR32Uint, 384, 384}) != nullptr, "a 32-bit raw depth bits view of a D32 surface must sample its depth plane");
    Require(lookup(context, {depth16, FormatR16Uint, 256, 256}) != nullptr, "a 16-bit raw depth bits view of a D16 surface must sample its depth plane");

    Require(lookup(context, {depth32, FormatR32Float, 192, 192}) == nullptr, "a view of another extent over a depth surface must be read as reused memory");
    Require(lookup(context, {depth32, FormatR11G11B10Float, 384, 384}) == nullptr, "an R11G11B10 view of a D32 surface's own extent must be read as reused memory");
    Require(lookup(context, {depth32, FormatR32Float, 512, 512, TileDepth64KB, Type2DArray, 5}) == nullptr, "a 2D array view of another extent over a depth surface must be read as reused memory");
    Require(lookup(context, {depth32, FormatR32Uint, 384, 384, TileRenderTarget64KB}) == nullptr, "an R32 uint view without a depth layout must be read as reused memory");
    Require(lookup(context, {depth16, FormatR32Float, 256, 256}) == nullptr, "an R32F view of a D16 surface's own extent must be read as reused memory");

    Require(lookup(context, {stencil, FormatR8Uint, 320, 180}) != nullptr, "an 8-bit view of a stencil plane's own extent must sample the stencil plane");
    Require(lookup(context, {stencil, FormatR8G8B8A8Unorm, 320, 180, TileRenderTarget64KB}) == nullptr, "an RGBA8 view of a stencil plane's own extent must be read as reused memory");
    Require(lookup(context, {stencil, FormatR32Float, 320, 180}) == nullptr, "an R32F view of a stencil plane's own extent must be read as reused memory");

    Require(DepthSurfaceAt(depth32) && DepthSurfaceAt(depth16) && DepthSurfaceAt(depthStencil) && DepthSurfaceAt(stencil), "every bound depth and stencil plane must be known as a depth surface");
    static int otherDeviceTag = 0;
    auto otherDevice = context;
    otherDevice.device = reinterpret_cast<VkDevice>(&otherDeviceTag);
    RetireDepthSurfacesAt(otherDevice, depth32);
    Require(DepthSurfaceAt(depth32), "a color target of another device must leave a depth surface alone");
    RetireDepthSurfacesAt(context, depth32);
    Require(!DepthSurfaceAt(depth32), "a color target bound over a depth surface must retire it");
    Require(lookup(context, {depth32, FormatR32Float, 384, 384}) == nullptr, "a retired depth surface must not serve samples");
    Require(DepthSurfaceAt(depth16) && DepthSurfaceAt(depthStencil) && DepthSurfaceAt(stencil), "retiring one depth surface must leave the others");
    RetireDepthSurfacesAt(context, stencil);
    Require(!DepthSurfaceAt(depthStencil) && !DepthSurfaceAt(stencil), "a color target bound over a stencil plane must retire its depth/stencil surface");
    Require(DepthSurfaceAt(depth16), "retiring a depth/stencil surface must leave the other surfaces");
    const auto first = DepthSurfaceView(context, {depth32, 0, {384, 384}, VK_FORMAT_D32_SFLOAT, 1.0f, 0});
    Require(DepthSurfaceAt(depth32) && first != VK_NULL_HANDLE, "a depth target bound again after its retirement gets a fresh surface");
    Require(DepthSurfaceView(context, {depth32, 0, {384, 384}, VK_FORMAT_D32_SFLOAT, 1.0f, 0}) == first, "the fresh surface serves the next binding of the same target");
    Require(lookup(context, {depth32, FormatR32Float, 384, 384}) != nullptr, "the fresh surface serves samples again");

    static constexpr std::array<std::uint32_t, 12> storeKernel{0xd7460000u, 0x0401060cu, 0xf4201a84u, 0xfa000000u, 0xbf8cc07fu, 0x7e06026au, 0xd7460001u, 0x0405060du, 0x7e04020eu, 0xf0200128u, 0x00000300u, 0xbf810000u};
    const auto storeUserData = [](std::uint64_t image, std::uint32_t width, std::uint32_t height, std::uint64_t value) {
        std::array<std::uint32_t, 12> words{};
        const auto descriptor = tsharp({image, FormatR32Float, width, height, TileDepth64KB, Type2DArray});
        std::copy(descriptor.begin(), descriptor.end(), words.begin());
        words[8] = static_cast<std::uint32_t>(value);
        words[9] = static_cast<std::uint32_t>(value >> 32u);
        words[10] = 16u;
        return words;
    };
    const std::array<std::uint32_t, 3> threads{8u, 8u, 1u};
    const auto covering = storeUserData(depth32, 384, 384, 0x7000);
    const auto match = MatchDepthStoreClear(storeKernel, covering, threads, {48u, 48u, 1u});
    Require(match && match->image == depth32 && match->width == 384 && match->height == 384 && match->value == 0x7000, "a constant image_store over a whole 2D surface must be recognised as a clear");
    Require(!MatchDepthStoreClear(storeKernel, covering, threads, {47u, 48u, 1u}), "a constant image_store that misses a column must not be a clear");
    Require(!MatchDepthStoreClear(storeKernel, covering, {8u, 4u, 1u}, {48u, 96u, 1u}), "another thread shape must not be recognised");
    static constexpr std::array<std::uint32_t, 15> storeVectorKernel{0xd7460004u, 0x0401060cu, 0xf4280404u, 0xfa000000u, 0xd7460005u, 0x0405060du, 0x7e0c020eu, 0xbf8cc07fu, 0x7e000210u, 0x7e020211u, 0x7e040212u, 0x7e060213u, 0xf0200f28u, 0x00000004u, 0xbf810000u};
    const auto vectorMatch = MatchDepthStoreClear(storeVectorKernel, covering, threads, {48u, 48u, 1u});
    Require(vectorMatch && vectorMatch->image == depth32 && vectorMatch->value == 0x7000, "a four-component constant image_store over a whole 2D surface must be recognised as a clear of its first component");
    auto patched = storeKernel;
    patched[5] = 0x7e060203u;
    Require(!MatchDepthStoreClear(patched, covering, threads, {48u, 48u, 1u}), "a store of another value must not be recognised");
    Require(!MatchDepthStoreClear(storeKernel, storeUserData(depth32, 384, 384, 0), threads, {48u, 48u, 1u}), "a null value buffer must not be recognised");
    const auto before = clears;
    Require(ClearDepthSurface(context, depth32, 384, 384, 0.25f) && clears == before + 1 && lastClearDepth == 0.25f, "a recognised clear of a depth surface must clear its depth plane with the stored value");
    Require(!ClearDepthSurface(context, depth32, 192, 384, 0.5f) && clears == before + 1, "a clear narrower than the depth surface must be refused");
    Require(!ClearDepthSurface(context, 0x70000000, 384, 384, 0.5f), "a clear of memory without a depth surface must be refused");

    ClearDepthSurfaces(context.device);
}
