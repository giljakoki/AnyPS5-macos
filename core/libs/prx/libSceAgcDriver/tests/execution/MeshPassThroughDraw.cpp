#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "../NggGeometryProgram.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <bit>
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
using ShaderRecompiler::MeshConfiguration;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Width = 192;
constexpr std::uint32_t Height = 128;
constexpr std::uint32_t Columns = 8;
constexpr std::uint32_t Rows = 5;
constexpr std::uint32_t Triangles = Columns * Rows;
constexpr std::uint32_t CulledTriangle = 13;
constexpr std::uint32_t CulledAlphaBits = 0x3f800800u;
constexpr std::array<std::uint8_t, 4> Background{16, 24, 40, 255};
constexpr std::array<std::uint8_t, 4> OtherCorner{255, 255, 255, 255};
constexpr MeshConfiguration SmallSubgroup{4u, 4u, 12u, 12u, 4u, 64u, 1024u, 0u, 4u, true};
constexpr MeshConfiguration WideSubgroup{4u, 32u, 96u, 96u, 32u, 128u, 2048u, 0u, 4u, true};

alignas(256) constexpr std::array<std::uint32_t, 7> PixelCode{
    0xc8020002, 0xc8060102, 0xc80a0202, 0xc80e0302, 0xf800180f, 0x03020100, 0xbf810000,
};

struct Vertex {
    std::array<float, 4> position;
    std::array<float, 4> color;
};

alignas(256) std::array<std::byte, Width * Height * 4> Pixels{};
alignas(256) std::array<Vertex, 3 * Triangles> Ordered{};
alignas(256) std::array<Vertex, 3 * Triangles> Scrambled{};
alignas(256) std::array<std::uint16_t, 3 * Triangles> Indices{};

std::array<std::uint8_t, 4> TriangleColor(std::uint32_t triangle) {
    return {static_cast<std::uint8_t>(40u + 29u * triangle), static_cast<std::uint8_t>(200u + 53u * triangle), static_cast<std::uint8_t>(triangle % 2u == 0u ? 60u : 200u), 255};
}

Vertex TriangleVertex(std::uint32_t triangle, std::uint32_t corner) {
    const float cellWidth = 2.0f / Columns;
    const float cellHeight = 2.0f / Rows;
    const float x = -1.0f + cellWidth * static_cast<float>(triangle % Columns);
    const float y = -1.0f + cellHeight * static_cast<float>(triangle / Columns);
    const auto color = corner == 0u ? TriangleColor(triangle) : OtherCorner;
    std::array<float, 4> rgba{color[0] / 255.0f, color[1] / 255.0f, color[2] / 255.0f, 1.0f};
    if (triangle == CulledTriangle && corner == 0u) rgba[3] = std::bit_cast<float>(CulledAlphaBits);
    const std::array<std::array<float, 2>, 3> corners{{{0.1f, 0.1f}, {0.9f, 0.1f}, {0.5f, 0.9f}}};
    return {{x + corners[corner][0] * cellWidth, y + corners[corner][1] * cellHeight, 0.5f, 1.0f}, rgba};
}

std::size_t CellPixel(std::uint32_t triangle) {
    const auto column = triangle % Columns;
    const auto row = triangle / Columns;
    const auto x = (column * Width + Width / 2u) / Columns;
    const auto y = Height - 1u - (row * Height + (Height * 4u) / 10u) / Rows;
    return (static_cast<std::size_t>(y) * Width + x) * 4u;
}

std::string PixelText(std::size_t offset) {
    char text[64];
    std::snprintf(text, sizeof(text), "(%u, %u, %u, %u)", std::to_integer<unsigned>(Pixels[offset]), std::to_integer<unsigned>(Pixels[offset + 1]), std::to_integer<unsigned>(Pixels[offset + 2]), std::to_integer<unsigned>(Pixels[offset + 3]));
    return text;
}

bool PixelIs(std::size_t offset, const std::array<std::uint8_t, 4>& color) {
    for (std::size_t c = 0; c < 4; ++c) {
        if (std::to_integer<std::uint8_t>(Pixels[offset + c]) != color[c]) return false;
    }
    return true;
}

std::array<std::uint32_t, 4> VertexBufferDescriptor(const void* vertices, std::uint32_t count) {
    const auto address = reinterpret_cast<std::uintptr_t>(vertices);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (32u << 16u), count, 0x01016facu};
}

struct Case {
    const char* name;
    MeshConfiguration mesh;
    bool indexed;
    std::uint32_t instances;
};

void Run(AgcDriver::VulkanDevice& device, const Case& setup) {
    for (std::size_t i = 0; i < Pixels.size(); i += 4) {
        for (std::size_t c = 0; c < 4; ++c) Pixels[i + c] = std::byte{Background[c]};
    }
    const auto& code = NggGeometryProgram::CullingCode;
    const AgcDriver::Pm4::DrawParameters indexedDraw{reinterpret_cast<std::uintptr_t>(Indices.data()), static_cast<std::uint32_t>(Indices.size()), 2u, setup.instances, 0, true};
    const AgcDriver::Pm4::DrawParameters draw = setup.indexed ? indexedDraw : AgcDriver::Pm4::DrawParameters{0u, static_cast<std::uint32_t>(Indices.size()), 0u, setup.instances, 0, false};
    std::vector<std::uint32_t> userData(12, 0u);
    const auto index = AgcDriver::Graphics::MeshIndexBufferDescriptor(indexedDraw);
    std::copy(index.begin(), index.end(), userData.begin() + ShaderRecompiler::MeshIndexBufferUserWord);
    const auto vertices = VertexBufferDescriptor(setup.indexed ? Scrambled.data() : Ordered.data(), static_cast<std::uint32_t>(Ordered.size()));
    std::copy(vertices.begin(), vertices.end(), userData.begin() + 8);
    const auto target = device.Target();
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(std::span(code))}}};
    const ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Mesh, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {64u, 0, userData, std::nullopt, std::nullopt, ShaderRecompiler::ShaderVertexStageInfo{}, memory},
        target,
        {0, 0, 0, ShaderRecompiler::MeshDrawPushOffsetBytes},
        ShaderRecompiler::GraphicsCompileContext{0, {}, setup.mesh, std::nullopt, {draw.indexAddress, draw.indexCount, draw.indexSize, draw.instanceCount}}
    };
    const auto prepass = ShaderRecompiler::Recompile(request);

    constexpr std::uint32_t PassThroughPushBytes = ShaderRecompiler::MeshDrawPushOffsetBytes + ShaderRecompiler::MeshDrawPushBytes;
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
        {0, 0, PassThroughPushBytes, PassThroughPushBytes},
        std::nullopt
    };
    const auto pixelResult = ShaderRecompiler::Recompile(fragment);

    AgcDriver::VulkanDevice::MeshEmulationDraw emulated{};
    auto& state = emulated.state;
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, 64u, 64u, std::nullopt, std::nullopt};
    state.color = {reinterpret_cast<std::uintptr_t>(Pixels.data()), {Width, Height}, VK_FORMAT_R8G8B8A8_UNORM, Pixels.size(), 0xe4u};
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.renderExtent = {Width, Height};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.primitiveRestart = false;
    state.viewport = {0, static_cast<float>(Height), static_cast<float>(Width), -static_cast<float>(Height), 0, 1};
    state.negativeOneToOne = false;
    state.scissor = {{0, 0}, {Width, Height}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15;
    state.blends = {state.blend};
    state.blendConstants = {};
    emulated.layout.configuration = setup.mesh;
    emulated.layout.parameterMask = 1u;
    emulated.layout.recordDwords = 8u;
    emulated.fragment = &pixelResult;
    const auto groups = (Triangles + setup.mesh.primitivesPerGroup - 1u) / setup.mesh.primitivesPerGroup;
    const auto output = device.DispatchMeshPrepass(prepass, setup.mesh, 8u, draw, groups, {}, &emulated);

    for (std::uint32_t instance = 0; instance < setup.instances; ++instance) {
        for (std::uint32_t triangle = 0; triangle < Triangles; ++triangle) {
            const auto slot = instance * groups + triangle / setup.mesh.primitivesPerGroup;
            const auto* corners = output.indices.data() + (static_cast<std::size_t>(slot) * setup.mesh.maxPrimitives + triangle % setup.mesh.primitivesPerGroup) * 3u;
            const bool degenerate = corners[0] == corners[1] && corners[1] == corners[2];
            Require(degenerate == (triangle == CulledTriangle), std::string(setup.name) + ": instance " + std::to_string(instance) + " triangle " + std::to_string(triangle) + (degenerate ? " was culled" : " was not culled"));
        }
    }
    for (std::uint32_t triangle = 0; triangle < Triangles; ++triangle) {
        const auto offset = CellPixel(triangle);
        const auto expected = triangle == CulledTriangle ? Background : TriangleColor(triangle);
        Require(PixelIs(offset, expected), std::string(setup.name) + ": triangle " + std::to_string(triangle) + (triangle == CulledTriangle ? " was drawn although culled" : " was not drawn in its first corner's color") + ", pixel " + PixelText(offset));
    }
    Require(PixelIs(0, Background) && PixelIs(Pixels.size() - 4u, Background), std::string(setup.name) + ": the corners changed");
    std::printf("PASS %s\n", setup.name);
}

}

int main() {
    try {
        setenv("ANYPS5_NO_SHADER_CACHE", "1", 1);
        for (std::uint32_t triangle = 0; triangle < Triangles; ++triangle) {
            for (std::uint32_t corner = 0; corner < 3u; ++corner) {
                Ordered[3 * triangle + corner] = TriangleVertex(triangle, corner);
                const auto slot = 3 * (Triangles - 1 - triangle) + (corner + 1) % 3;
                Scrambled[slot] = TriangleVertex(triangle, corner);
                Indices[3 * triangle + corner] = static_cast<std::uint16_t>(slot);
            }
        }
        const auto testDevice = OpenVulkanTestDevice();
        if (!testDevice) return VulkanTestSkipped;
        auto& device = *testDevice;
        if (device.DeviceName().starts_with("llvmpipe")) {
            std::puts("skipped, llvmpipe");
            return VulkanTestSkipped;
        }
        for (const auto& setup : {Case{"non-indexed one-wave groups", SmallSubgroup, false, 1u}, Case{"indexed two-wave groups", WideSubgroup, true, 1u}, Case{"two instances", SmallSubgroup, false, 2u}}) {
            Run(device, setup);
        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
