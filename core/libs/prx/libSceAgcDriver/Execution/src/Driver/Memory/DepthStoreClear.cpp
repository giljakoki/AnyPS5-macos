#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <tuple>

namespace AgcDriver::DriverDetail {

namespace {

void reportDepthStoreRefusal(std::uint64_t program, std::uint64_t image, const std::string& reason) {
    static std::mutex mutex;
    static std::set<std::tuple<std::uint64_t, std::uint64_t, std::string>> reported;
    std::lock_guard lock(mutex);
    if (!reported.emplace(program, image, reason).second) return;
    std::fprintf(stderr, "[depth-clear] compute program 0x%llx stores a constant into image 0x%llx but does not run as a depth clear: %s\n", static_cast<unsigned long long>(program), static_cast<unsigned long long>(image), reason.c_str());
}

}

bool Driver::clearDepthByStore(const QueueState& queue, std::span<const std::uint32_t> packet, std::span<const std::uint32_t> code, const std::vector<std::uint32_t>& userData, const ShaderRecompiler::ShaderComputeStageInfo& compute, const std::shared_ptr<VulkanDevice>& localDevice, std::uint64_t program, const std::array<std::uint32_t, 3>& partialThreads, bool indirect) {
    if (!Graphics::DepthStoreKernel(code)) return false;
    const auto imageAddress = userData.size() >= 8 ? Graphics::DecodeTextureResource(std::span(userData).first(8)).baseAddress : 0;
    if (!Graphics::DepthSurfaceAt(imageAddress)) return false;
    if (indirect) {
        reportDepthStoreRefusal(program, imageAddress, "an indirect dispatch, whose group counts are only in GPU memory");
        return false;
    }
    std::array<std::uint32_t, 3> groups{packet[1], packet[2], packet[3]};
    if ((packet[4] & 0x20u) != 0) {
        for (std::uint32_t axis = 0; axis < 3; ++axis) {
            const auto threads = std::max(readRegister(queue.shader, 0x207 + axis) & 0xffffu, 1u);
            groups[axis] = (groups[axis] + threads - 1) / threads;
        }
    }
    const char* refusal = nullptr;
    const auto clear = Graphics::MatchDepthStoreClear(code, userData, compute.numThreads, groups, partialThreads, &refusal);
    if (!clear) {
        reportDepthStoreRefusal(program, imageAddress, refusal != nullptr ? refusal : "the store does not match");
        return false;
    }
    GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Fill);
    std::lock_guard lock(GuestMemory::GpuMutex());
    if (!GuestMemory::Accessible(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(clear->value)), 4)) {
        reportDepthStoreRefusal(program, clear->image, "its value is not readable");
        return false;
    }
    std::uint32_t bits = 0;
    GuestMemory::Read(clear->value, std::as_writable_bytes(std::span(&bits, 1)), 4);
    std::string surfaceRefusal;
    if (localDevice->ClearDepthSurface(clear->image, clear->width, clear->height, std::bit_cast<float>(bits), &surfaceRefusal)) return true;
    constexpr std::uint32_t FormatR8Uint = 5;
    if (surfaceRefusal.find("stencil plane") != std::string::npos) {
        if (clear->format != FormatR8Uint) {
            reportDepthStoreRefusal(program, clear->image, "it stores into the stencil plane through format " + std::to_string(clear->format) + ", not R8_UINT");
            return false;
        }
        surfaceRefusal.clear();
        if (localDevice->ClearStencilSurface(clear->image, clear->width, clear->height, static_cast<std::uint8_t>(bits & 0xffu), &surfaceRefusal)) return true;
    }
    reportDepthStoreRefusal(program, clear->image, surfaceRefusal.empty() ? "no depth plane starts at the image" : surfaceRefusal);
    return false;
}

}
