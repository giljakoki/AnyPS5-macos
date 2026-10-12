#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::Graphics::StorageTexture;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 8;
constexpr std::uint32_t Side = 256;
constexpr std::uint32_t Format32UInt = 20;
constexpr std::uint32_t TileR64KBX = 0x1b;
constexpr std::uint32_t Type2D = 9;
constexpr std::size_t BlockBytes = 0x60000;
constexpr std::uint32_t Untouched = 0xdeadbeefu;

alignas(256) constexpr std::array<std::uint32_t, 7> StoreCode{
    0x7e040300, 0x7e060280, 0x7e0202ff, 0x12345678u, 0xf0200108, 0x00020102, 0xbf810000,
};

std::uint64_t AddressOf(const void* data) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
}

std::array<std::uint32_t, 8> TextureDescriptor(const std::uint32_t* texels) {
    const auto address = AddressOf(texels);
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (Format32UInt << 20u) | (((Side - 1u) & 3u) << 30u),
        ((Side - 1u) >> 2u) | ((Side - 1u) << 14u),
        0xfacu | (TileR64KBX << 20u) | (Type2D << 28u),
        0u,
        0u,
        0u,
        0u,
    };
}

std::uint64_t GuestBytes(const std::uint32_t* texels) {
    const auto words = TextureDescriptor(texels);
    return AgcDriver::Graphics::DescribeSurface(AgcDriver::Graphics::DecodeTextureResource(words)).guestBytes;
}

void Store(AgcDriver::VulkanDevice& device, const std::uint32_t* texels) {
    std::vector<std::uint32_t> userData(16, 0u);
    const auto texture = TextureDescriptor(texels);
    std::copy(texture.begin(), texture.end(), userData.begin() + 8);
    const std::span<const std::uint32_t> code(StoreCode);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        std::vector<std::uint32_t> storage((BlockBytes + 0x10000u) / 4u);
        auto* texels = reinterpret_cast<std::uint32_t*>((reinterpret_cast<std::uintptr_t>(storage.data()) + 0xffffu) & ~std::uintptr_t{0xffffu});
        std::fill(texels, texels + BlockBytes / 4u, Untouched);
        {
            GuestAllocations::Mutation mutation;
            mutation.Add(texels, BlockBytes, true, true, true);
        }
        const auto surface = AddressOf(texels);
        const auto bytes = GuestBytes(texels);
        Require(bytes <= BlockBytes, "the surface must fit the block");

        Store(*device, texels);
        bool pending = false;
        const auto written = StorageTexture::FindPresentable(surface, bytes, pending);
        Require(written != nullptr && pending && written == StorageTexture::FindPending(surface, bytes), "the store's pending image does not serve the present");

        device->StoreIdleResults(true);
        device->WaitIdle();
        Require(StorageTexture::FindPending(surface, bytes) == nullptr, "the idle flush left the image pending");
        const auto flushed = StorageTexture::FindLive(surface, bytes);
        Require(flushed == written, flushed == nullptr ? std::string("the flushed image is not live") : "another image is live after the flush");
        std::shared_ptr<StorageTexture> live;
        {
            std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
            live = StorageTexture::FindPresentable(surface, bytes, pending);
        }
        Require(live == written && !pending, "the flushed image no longer serves the present");

        const std::uint32_t patched = Untouched ^ 0x00ff00ffu;
        AgcDriver::GuestMemory::Write(surface, std::as_bytes(std::span<const std::uint32_t>(&patched, 1)), 4);
        std::shared_ptr<StorageTexture> refreshed;
        {
            std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
            refreshed = StorageTexture::FindPresentable(surface, bytes, pending);
        }
        device->WaitIdle();
        Require(refreshed == written && !pending, "a CPU write after the flush loses the live image");

        {
            std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
            AgcDriver::Graphics::FlushCachedTextures(device->Device());
        }
        device->WaitIdle();
        {
            GuestAllocations::Mutation mutation;
            mutation.Remove(texels);
        }
        std::puts("presentable after idle flush tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
