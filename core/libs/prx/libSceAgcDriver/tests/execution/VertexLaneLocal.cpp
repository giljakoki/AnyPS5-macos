#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Vertices = 189;
constexpr std::uint32_t Width = 64;
constexpr std::uint32_t Height = 64;
alignas(256) std::array<std::uint32_t, Vertices * 4> Output{};
alignas(256) std::array<std::byte, Width * Height * 4> Pixels{};

alignas(256) constexpr std::array<std::uint32_t, 34> Wave32Code{
    0x7e140305u, 0x7e0c0281u, 0x7d880affu, 0x00000064u, 0x02000c80u, 0xd70f6a01u, 0x02020ad0u, 0x7e0e0280u,
    0x50020e80u, 0x7e0402c1u, 0x360e0a81u, 0x7d8a0e80u, 0xbe803c6au, 0xbf880002u, 0xd5690002u, 0x02010705u,
    0x887e007eu, 0xd7650003u, 0x020100c1u, 0xd7660003u, 0x020206c1u, 0x9384ff03u, 0x00080008u, 0x7d880604u,
    0x02060c80u, 0xe0782000u, 0x8002000au, 0x7e000280u, 0x7e020280u, 0x7e040280u, 0x7e0602f2u, 0xf80008cfu,
    0x03020100u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 34> Wave64Code{
    0x7e140305u, 0x7e0c0281u, 0x7d880affu, 0x00000064u, 0x02000c80u, 0xd70f6a01u, 0x02020ad0u, 0x7e0e0280u,
    0x50020e80u, 0x7e0402c1u, 0x360e0a81u, 0x7d8a0e80u, 0xbe80246au, 0xbf880002u, 0xd5690002u, 0x02010705u,
    0x88fe007eu, 0xd7650003u, 0x020100c1u, 0xd7660003u, 0x020206c1u, 0x9384ff03u, 0x00080008u, 0x7d880604u,
    0x02060c80u, 0xe0782000u, 0x8002000au, 0x7e000280u, 0x7e020280u, 0x7e040280u, 0x7e0602f2u, 0xf80008cfu,
    0x03020100u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 4> PixelCode{0x7e0e02f2u, 0xf800180fu, 0x07070707u, 0xbf810000u};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t stride, std::uint32_t count) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (stride << 16u), count, 0x01016facu};
}

void Draw(AgcDriver::VulkanDevice& device, std::span<const std::uint32_t> code, std::uint32_t waveSize) {
    Output.fill(0xdeadbeefu);
    Pixels.fill(std::byte{0});
    std::vector<std::uint32_t> userData(4, 0u);
    const auto output = BufferDescriptor(Output.data(), 16u, Vertices);
    std::copy(output.begin(), output.end(), userData.begin());
    const std::array<ShaderRecompiler::MemoryRegion, 1> vertexMemory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    ShaderRecompiler::RecompileRequest vertex{
        {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {waveSize, 8, userData, std::nullopt, std::nullopt, ShaderRecompiler::ShaderVertexStageInfo{}, vertexMemory},
        device.Target(),
        {0, 0, 0, 64}
    };
    vertex.useCache = false;
    const auto vertexResult = ShaderRecompiler::Recompile(vertex);
    const auto vertexPush = static_cast<std::uint32_t>(vertexResult.pushConstants.size());

    ShaderRecompiler::ShaderPixelStageInfo pixel{};
    pixel.inputAddr = ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionX);
    pixel.posX = true;
    pixel.targetOutputMode[0] = 9;
    pixel.targetExportMapping.fill(0xe4u);
    const std::vector<std::uint32_t> pixelUserData(4, 0u);
    const std::array<ShaderRecompiler::MemoryRegion, 1> pixelMemory{{{reinterpret_cast<std::uintptr_t>(PixelCode.data()), std::as_bytes(std::span(PixelCode))}}};
    ShaderRecompiler::RecompileRequest fragment{
        {ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(PixelCode.data()), PixelCode, 0, {}},
        {64, 0, pixelUserData, std::nullopt, pixel, std::nullopt, pixelMemory},
        device.Target(),
        PixelPushLayout(vertexPush, device.Target())
    };
    fragment.useCache = false;
    const auto pixelResult = ShaderRecompiler::Recompile(fragment);
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{
        {ShaderStage::Vertex, &vertexResult, 0},
        {ShaderStage::Fragment, &pixelResult, PixelPushOffset(vertexPush, device.Target())}
    }};

    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, waveSize, 64u, std::nullopt, std::nullopt};
    state.color = {reinterpret_cast<std::uintptr_t>(Pixels.data()), {Width, Height}, VK_FORMAT_R8G8B8A8_UNORM, Pixels.size(), 0xe4u};
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.renderExtent = {Width, Height};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(Height), static_cast<float>(Width), -static_cast<float>(Height), 0, 1};
    state.negativeOneToOne = false;
    state.scissor = {{0, 0}, {Width, Height}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15;
    state.blends = {state.blend};
    state.blendConstants = {};
    const AgcDriver::Pm4::DrawParameters draw{0, Vertices, 0, 1, 0, false};
    device.Draw(state, draw, shaders);
    device.WaitIdle();
}

void Check(std::uint32_t waveSize) {
    for (std::uint32_t vertex = 0; vertex < Vertices; ++vertex) {
        const auto what = "vertex lane-local: wave" + std::to_string(waveSize) + " vertex " + std::to_string(vertex);
        const auto* result = &Output[vertex * 4u];
        const std::uint32_t below = vertex < 100u ? 1u : 0u;
        const std::uint32_t carry = vertex >= 16u ? 1u : 0u;
        const std::uint32_t odd = (vertex & 1u) != 0u ? vertex * 3u : 0xffffffffu;
        Require(result[0] == below, what + " selected " + std::to_string(result[0]) + " through v_cmp and v_cndmask, expected " + std::to_string(below));
        Require(result[1] == carry, what + " carried " + std::to_string(result[1]) + " through VCC, expected " + std::to_string(carry));
        Require(result[2] == odd, what + " wrote " + std::to_string(result[2]) + " in the s_cbranch_execz region, expected " + std::to_string(odd));
        Require(result[3] == 1u, what + " compared v_mbcnt with the s3 count as " + std::to_string(result[3]) + ", expected 1");
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if (device->Target().subgroupSize < 32u) {
            std::printf("vertex lane-local draws skipped, subgroup size %u cannot hold a wave32\n", device->Target().subgroupSize);
            return 0;
        }
        const bool vertexSubgroups = (device->SubgroupStages() & VK_SHADER_STAGE_VERTEX_BIT) != 0u;
        std::printf("vertex shaders %s subgroup operations (stages 0x%x)\n", vertexSubgroups ? "have" : "lack", static_cast<unsigned>(device->SubgroupStages()));
        Draw(*device, Wave32Code, 32u);
        Check(32u);
        Draw(*device, Wave64Code, 64u);
        Check(64u);
        std::puts("vertex lane-local tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
