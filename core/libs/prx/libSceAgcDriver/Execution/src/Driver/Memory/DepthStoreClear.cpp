#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <mutex>

namespace AgcDriver::DriverDetail {

bool Driver::clearDepthByStore(const QueueState& queue, std::span<const std::uint32_t> packet, std::span<const std::uint32_t> code, const std::vector<std::uint32_t>& userData, const ShaderRecompiler::ShaderComputeStageInfo& compute, const std::shared_ptr<VulkanDevice>& localDevice) {
    std::array<std::uint32_t, 3> groups{packet[1], packet[2], packet[3]};
    if ((packet[4] & 0x20u) != 0) {
        for (std::uint32_t axis = 0; axis < 3; ++axis) {
            const auto threads = std::max(readRegister(queue.shader, 0x207 + axis) & 0xffffu, 1u);
            groups[axis] = (groups[axis] + threads - 1) / threads;
        }
    }
    const auto clear = Graphics::MatchDepthStoreClear(code, userData, compute.numThreads, groups);
    if (!clear || !Graphics::DepthSurfaceAt(clear->image)) return false;
    GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Fill);
    std::lock_guard lock(GuestMemory::GpuMutex());
    if (!GuestMemory::Accessible(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(clear->value)), 4)) return false;
    std::uint32_t bits = 0;
    GuestMemory::Read(clear->value, std::as_writable_bytes(std::span(&bits, 1)), 4);
    return localDevice->ClearDepthSurface(clear->image, clear->width, clear->height, std::bit_cast<float>(bits));
}

}
