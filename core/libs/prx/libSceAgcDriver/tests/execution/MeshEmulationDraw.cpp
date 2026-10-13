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
constexpr std::uint32_t CoveredTriangle = 22;
constexpr std::uint32_t CulledAlphaBits = 0x3f800800u;
constexpr std::array<std::uint8_t, 4> Background{16, 24, 40, 255};
constexpr std::array<std::uint8_t, 4> Underlay{200, 30, 30, 255};
constexpr std::array<std::uint8_t, 4> Overlay{20, 40, 220, 255};
constexpr MeshConfiguration SmallSubgroup{4u, 4u, 12u, 12u, 4u, 64u, 1024u, 0u, 4u};

alignas(256) constexpr std::array<std::uint32_t, 10> VertexCode{
    0xe0382000, 0x80000005, 0xe0382010, 0x80000405, 0xbf8c3f70, 0xf80008cf, 0x03020100, 0xf800020f, 0x07060504, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 7> PixelCode{
    0xc8020002, 0xc8060102, 0xc80a0202, 0xc80e0302, 0xf800180f, 0x03020100, 0xbf810000,
};

struct Vertex {
    std::array<float, 4> position;
    std::array<float, 4> color;
};

alignas(256) std::array<std::byte, Width * Height * 4> Pixels{};
alignas(256) std::array<Vertex, 3 * Triangles> Ordered{};
alignas(256) std::array<Vertex, 6> FullScreen{};
alignas(256) std::array<Vertex, 3> Cover{};

std::array<float, 4> Rgba(const std::array<std::uint8_t, 4>& color) {
    return {color[0] / 255.0f, color[1] / 255.0f, color[2] / 255.0f, color[3] / 255.0f};
}

std::array<std::uint8_t, 4> TriangleColor(std::uint32_t triangle) {
    return {static_cast<std::uint8_t>(40u + 29u * triangle), static_cast<std::uint8_t>(200u + 53u * triangle), static_cast<std::uint8_t>(triangle % 2u == 0u ? 60u : 200u), 255};
}

std::array<float, 2> CellCorner(std::uint32_t triangle, float across, float up) {
    const float cellWidth = 2.0f / Columns;
    const float cellHeight = 2.0f / Rows;
    return {-1.0f + cellWidth * (static_cast<float>(triangle % Columns) + across), -1.0f + cellHeight * (static_cast<float>(triangle / Columns) + up)};
}

std::size_t CellPixel(std::uint32_t triangle) {
    const auto column = triangle % Columns;
    const auto row = triangle / Columns;
    const auto x = (column * Width + Width / 2u) / Columns;
    const auto y = Height - 1u - (row * Height + (Height * 4u) / 10u) / Rows;
    return (static_cast<std::size_t>(y) * Width + x) * 4u;
}

std::size_t GapPixel(std::uint32_t triangle) {
    const auto column = triangle % Columns;
    const auto row = triangle / Columns;
    const auto x = column * Width / Columns + 1u;
    const auto y = Height - 1u - (row * Height / Rows + 1u);
    return (static_cast<std::size_t>(y) * Width + x) * 4u;
}

bool PixelIs(std::size_t offset, const std::array<std::uint8_t, 4>& color) {
    for (std::size_t c = 0; c < 4; ++c) {
        if (std::to_integer<std::uint8_t>(Pixels[offset + c]) != color[c]) return false;
    }
    return true;
}

std::string PixelText(std::size_t offset) {
    char text[64];
    std::snprintf(text, sizeof(text), "(%u, %u, %u, %u)", std::to_integer<unsigned>(Pixels[offset]), std::to_integer<unsigned>(Pixels[offset + 1]), std::to_integer<unsigned>(Pixels[offset + 2]), std::to_integer<unsigned>(Pixels[offset + 3]));
    return text;
}

std::array<std::uint32_t, 4> VertexBufferDescriptor(const void* vertices, std::uint32_t count) {
    const auto address = reinterpret_cast<std::uintptr_t>(vertices);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (32u << 16u), count, 0x01016facu};
}

ShaderRecompiler::RecompileResult CompilePixel(const ShaderRecompiler::SpirvTarget& target, ShaderRecompiler::BindingLayout layout) {
    ShaderRecompiler::ShaderPixelStageInfo pixel{};
    pixel.interpolatorCount = 1;
    pixel.interpolatorSettings[0] = 0x400u;
    pixel.targetOutputMode[0] = 9;
    pixel.targetExportMapping.fill(0xe4u);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(PixelCode.data()), std::as_bytes(std::span(PixelCode))}}};
    const ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(PixelCode.data()), PixelCode, 0, {}},
        {64, 0, {}, std::nullopt, pixel, std::nullopt, memory},
        target,
        layout,
        std::nullopt
    };
    return ShaderRecompiler::Recompile(request);
}

AgcDriver::Graphics::State TargetState() {
    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, 64u, 64u, std::nullopt, std::nullopt};
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
    return state;
}

void DrawVertices(AgcDriver::VulkanDevice& device, const Vertex* vertices, std::uint32_t count) {
    const auto target = device.Target();
    std::vector<std::uint32_t> userData(4, 0u);
    const auto buffer = VertexBufferDescriptor(vertices, count);
    std::copy(buffer.begin(), buffer.end(), userData.begin());
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(VertexCode.data()), std::as_bytes(std::span(VertexCode))}}};
    const ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(VertexCode.data()), VertexCode, 0, {}},
        {64u, 0, userData, std::nullopt, std::nullopt, ShaderRecompiler::ShaderVertexStageInfo{}, memory},
        target,
        {0, 0, 0, 64}
    };
    const auto vertex = ShaderRecompiler::Recompile(request);
    const auto push = static_cast<std::uint32_t>(vertex.pushConstants.size());
    const auto pixel = CompilePixel(target, PixelPushLayout(push, target));
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{{ShaderStage::Vertex, &vertex, 0}, {ShaderStage::Fragment, &pixel, PixelPushOffset(push, target)}}};
    device.Draw(TargetState(), {0, count, 0, 1, 0, false}, shaders);
}

void DrawMesh(AgcDriver::VulkanDevice& device, std::uint32_t instances) {
    const auto target = device.Target();
    const auto& code = NggGeometryProgram::CullingCode;
    auto mesh = SmallSubgroup;
    mesh.compute = true;
    const AgcDriver::Pm4::DrawParameters draw{0, static_cast<std::uint32_t>(Ordered.size()), 0, instances, 0, false};
    std::vector<std::uint32_t> userData(12, 0u);
    const auto vertices = VertexBufferDescriptor(Ordered.data(), static_cast<std::uint32_t>(Ordered.size()));
    std::copy(vertices.begin(), vertices.end(), userData.begin() + 8);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(std::span(code))}}};
    const ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Mesh, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {64u, 0, userData, std::nullopt, std::nullopt, ShaderRecompiler::ShaderVertexStageInfo{}, memory},
        target,
        {0, 0, 0, ShaderRecompiler::MeshDrawPushOffsetBytes},
        ShaderRecompiler::GraphicsCompileContext{0, {}, mesh, std::nullopt, {draw.indexAddress, draw.indexCount, draw.indexSize, draw.instanceCount}}
    };
    const auto prepass = ShaderRecompiler::Recompile(request);
    const auto push = static_cast<std::uint32_t>(prepass.pushConstants.size());
    const auto pixel = CompilePixel(target, {0, 0, push, ShaderRecompiler::MeshDrawPushOffsetBytes - push});
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{{ShaderStage::Mesh, &prepass, 0}, {ShaderStage::Fragment, &pixel, push}}};
    auto state = TargetState();
    state.stages = {AgcDriver::Graphics::ShaderPath::Geometry, 0x20u, 64u, 64u, SmallSubgroup, std::nullopt};
    device.DrawMeshEmulated(state, draw, shaders);
}

void Check(const char* what) {
    for (std::uint32_t triangle = 0; triangle < Triangles; ++triangle) {
        const auto offset = CellPixel(triangle);
        const auto expected = triangle == CoveredTriangle ? Overlay : triangle == CulledTriangle ? Underlay : TriangleColor(triangle);
        Require(PixelIs(offset, expected), std::string(what) + ": triangle " + std::to_string(triangle) + " pixel " + PixelText(offset));
        Require(PixelIs(GapPixel(triangle), Underlay), std::string(what) + ": the gap of cell " + std::to_string(triangle) + " lost the first draw, pixel " + PixelText(GapPixel(triangle)));
    }
    std::printf("PASS %s\n", what);
}

}

int main() {
    try {
        setenv("ANYPS5_NO_SHADER_CACHE", "1", 1);
        setenv("APS5_MESH_EMULATION", "1", 1);
        for (std::uint32_t triangle = 0; triangle < Triangles; ++triangle) {
            const std::array<std::array<float, 2>, 3> corners{{CellCorner(triangle, 0.1f, 0.1f), CellCorner(triangle, 0.9f, 0.1f), CellCorner(triangle, 0.5f, 0.9f)}};
            for (std::uint32_t corner = 0; corner < 3u; ++corner) {
                auto color = Rgba(corner == 0u ? TriangleColor(triangle) : std::array<std::uint8_t, 4>{255, 255, 255, 255});
                if (triangle == CulledTriangle && corner == 0u) color[3] = std::bit_cast<float>(CulledAlphaBits);
                Ordered[3 * triangle + corner] = {{corners[corner][0], corners[corner][1], 0.5f, 1.0f}, color};
            }
        }
        const std::array<std::array<float, 2>, 6> screen{{{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}}};
        for (std::size_t i = 0; i < screen.size(); ++i) FullScreen[i] = {{screen[i][0], screen[i][1], 0.5f, 1.0f}, Rgba(Underlay)};
        const std::array<std::array<float, 2>, 3> cover{{CellCorner(CoveredTriangle, 0.3f, 0.2f), CellCorner(CoveredTriangle, 0.7f, 0.2f), CellCorner(CoveredTriangle, 0.5f, 0.7f)}};
        for (std::size_t i = 0; i < cover.size(); ++i) Cover[i] = {{cover[i][0], cover[i][1], 0.5f, 1.0f}, Rgba(Overlay)};

        const auto testDevice = OpenVulkanTestDevice();
        if (!testDevice) return VulkanTestSkipped;
        auto& device = *testDevice;
        if (!device.MeshEmulation()) {
            std::puts("skipped, the device has mesh shaders");
            return VulkanTestSkipped;
        }
        for (const auto instances : {1u, 2u}) {
            for (std::size_t i = 0; i < Pixels.size(); i += 4) {
                for (std::size_t c = 0; c < 4; ++c) Pixels[i + c] = std::byte{Background[c]};
            }
            DrawVertices(device, FullScreen.data(), static_cast<std::uint32_t>(FullScreen.size()));
            DrawMesh(device, instances);
            DrawVertices(device, Cover.data(), static_cast<std::uint32_t>(Cover.size()));
            device.WaitIdle();
            Check(instances == 1u ? "vertex, emulated mesh and vertex draws in one frame" : "the same frame with two mesh instances");
        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
