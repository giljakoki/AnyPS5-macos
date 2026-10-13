#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "../NggGeometryProgram.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Width = 64;
constexpr std::uint32_t Height = 32;
constexpr ShaderRecompiler::MeshConfiguration SmallSubgroup{4u, 4u, 12u, 12u, 4u, 64u, 1024u, 0u, 4u};

alignas(256) constexpr std::array<std::uint32_t, 7> PixelCode{
    0xc8020002, 0xc8060102, 0xc80a0202, 0xc80e0302, 0xf800180f, 0x03020100, 0xbf810000,
};

alignas(256) std::array<std::byte, Width * Height * 4> Pixels{};
alignas(256) std::array<std::array<float, 8>, 12> Vertices{};

void DrawMesh(AgcDriver::VulkanDevice& device) {
    const auto target = device.Target();
    const auto& code = NggGeometryProgram::Code;
    const AgcDriver::Pm4::DrawParameters draw{0, static_cast<std::uint32_t>(Vertices.size()), 0, 1, 0, false};
    std::vector<std::uint32_t> userData(12, 0u);
    const auto address = reinterpret_cast<std::uintptr_t>(Vertices.data());
    const std::array<std::uint32_t, 4> vertexBuffer{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (32u << 16u), static_cast<std::uint32_t>(Vertices.size()), 0x01016facu};
    std::copy(vertexBuffer.begin(), vertexBuffer.end(), userData.begin() + 8);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(std::span(code))}}};
    const ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Mesh, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {64u, 0, userData, std::nullopt, std::nullopt, ShaderRecompiler::ShaderVertexStageInfo{}, memory},
        target,
        {0, 0, 0, ShaderRecompiler::MeshDrawPushOffsetBytes},
        ShaderRecompiler::GraphicsCompileContext{0, {}, SmallSubgroup, std::nullopt, {draw.indexAddress, draw.indexCount, draw.indexSize, draw.instanceCount}}
    };
    const auto mesh = ShaderRecompiler::Recompile(request);
    const auto push = static_cast<std::uint32_t>(mesh.pushConstants.size());
    ShaderRecompiler::ShaderPixelStageInfo pixel{};
    pixel.interpolatorCount = 1;
    pixel.interpolatorSettings[0] = 0x400u;
    pixel.targetOutputMode[0] = 9;
    pixel.targetExportMapping.fill(0xe4u);
    const std::array<ShaderRecompiler::MemoryRegion, 1> pixelMemory{{{reinterpret_cast<std::uintptr_t>(PixelCode.data()), std::as_bytes(std::span(PixelCode))}}};
    const ShaderRecompiler::RecompileRequest fragment{
        {ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(PixelCode.data()), PixelCode, 0, {}},
        {64, 0, {}, std::nullopt, pixel, std::nullopt, pixelMemory},
        target,
        {0, 0, push, ShaderRecompiler::MeshDrawPushOffsetBytes - push},
        std::nullopt
    };
    const auto pixelResult = ShaderRecompiler::Recompile(fragment);
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{{ShaderStage::Mesh, &mesh, 0}, {ShaderStage::Fragment, &pixelResult, push}}};
    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Geometry, 0x20u, 64u, 64u, SmallSubgroup, std::nullopt};
    state.color = {reinterpret_cast<std::uintptr_t>(Pixels.data()), {Width, Height}, VK_FORMAT_R8G8B8A8_UNORM, Pixels.size(), 0xe4u};
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.renderExtent = {Width, Height};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(Height), static_cast<float>(Width), -static_cast<float>(Height), 0, 1};
    state.scissor = {{0, 0}, {Width, Height}};
    state.blend.colorWriteMask = 15;
    state.blends = {state.blend};
    device.Draw(state, draw, shaders);
}

}

int main() {
    try {
        setenv("ANYPS5_NO_SHADER_CACHE", "1", 1);
        unsetenv("APS5_MESH_EMULATION");
        const auto testDevice = OpenVulkanTestDevice();
        if (!testDevice) return VulkanTestSkipped;
        auto& device = *testDevice;
        if (device.Target().mesh.has_value()) {
            std::puts("skipped, the device has mesh shaders");
            return VulkanTestSkipped;
        }
        Require(!device.MeshEmulation(), "mesh emulation is on without APS5_MESH_EMULATION");
        try {
            DrawMesh(device);
        } catch (const std::exception& error) {
            const std::string message = error.what();
            Require(message.find("does not support mesh shaders") != std::string::npos || message.find("needs subgroup operations") != std::string::npos, "a mesh draw without APS5_MESH_EMULATION failed differently: " + message);
            std::puts("PASS a mesh draw without APS5_MESH_EMULATION still throws");
            return 0;
        }
        Require(false, "a mesh draw without APS5_MESH_EMULATION was drawn");
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
