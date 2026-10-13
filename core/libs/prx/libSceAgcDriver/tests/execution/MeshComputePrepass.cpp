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

struct Vertex {
    std::array<float, 4> position;
    std::array<float, 4> color;
};

constexpr std::uint32_t Triangles = 40;
constexpr std::uint32_t RecordDwords = 8;
constexpr MeshConfiguration SmallSubgroup{4u, 4u, 12u, 12u, 4u, 64u, 1024u, 0u, 4u, true};
constexpr MeshConfiguration WideSubgroup{4u, 32u, 96u, 96u, 32u, 128u, 2048u, 0u, 4u, true};

alignas(256) std::array<Vertex, 3 * Triangles> Ordered{};
alignas(256) std::array<Vertex, 3 * Triangles> Scrambled{};
alignas(256) std::array<std::uint16_t, 3 * Triangles> Indices{};

Vertex TriangleVertex(std::uint32_t triangle, std::uint32_t corner) {
    const auto base = static_cast<float>(triangle);
    return {{base + 0.25f * static_cast<float>(corner), -base, 0.5f, 1.0f}, {static_cast<float>(corner), base / 64.0f, 1.0f - base / 64.0f, 1.0f}};
}

std::array<std::uint32_t, 4> VertexBufferDescriptor(const void* vertices, std::uint32_t count) {
    const auto address = reinterpret_cast<std::uintptr_t>(vertices);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (32u << 16u), count, 0x01016facu};
}

struct Case {
    const char* name;
    MeshConfiguration mesh;
    std::uint32_t waveSize;
    bool indexed;
    std::uint32_t instances;
};

bool SameVertex(std::span<const std::uint32_t> record, const Vertex& expected) {
    for (std::uint32_t i = 0; i < 4u; ++i) {
        if (record[i] != std::bit_cast<std::uint32_t>(expected.position[i]) || record[4u + i] != std::bit_cast<std::uint32_t>(expected.color[i])) return false;
    }
    return true;
}

void Run(AgcDriver::VulkanDevice& device, const Case& setup) {
    const auto& code = setup.waveSize == 32u ? NggGeometryProgram::Wave32Code : NggGeometryProgram::Code;
    AgcDriver::Pm4::DrawParameters draw{setup.indexed ? reinterpret_cast<std::uintptr_t>(Indices.data()) : 0u, static_cast<std::uint32_t>(Indices.size()), setup.indexed ? 2u : 0u, setup.instances, 0, setup.indexed};
    std::vector<std::uint32_t> userData(12, 0u);
    const auto index = AgcDriver::Graphics::MeshIndexBufferDescriptor(draw.indexed ? draw : AgcDriver::Pm4::DrawParameters{reinterpret_cast<std::uintptr_t>(Indices.data()), static_cast<std::uint32_t>(Indices.size()), 2, 1, 0, true});
    std::copy(index.begin(), index.end(), userData.begin() + ShaderRecompiler::MeshIndexBufferUserWord);
    const auto vertices = VertexBufferDescriptor(setup.indexed ? Scrambled.data() : Ordered.data(), static_cast<std::uint32_t>(Ordered.size()));
    std::copy(vertices.begin(), vertices.end(), userData.begin() + 8);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(std::span(code))}}};
    const ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Mesh, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {setup.waveSize, 0, userData, std::nullopt, std::nullopt, ShaderRecompiler::ShaderVertexStageInfo{}, memory},
        device.Target(),
        {0, 0, 0, ShaderRecompiler::MeshDrawPushOffsetBytes},
        ShaderRecompiler::GraphicsCompileContext{0, {}, setup.mesh, std::nullopt, {draw.indexAddress, draw.indexCount, draw.indexSize, draw.instanceCount}}
    };
    const auto result = ShaderRecompiler::Recompile(request);
    const auto groups = (Triangles + setup.mesh.primitivesPerGroup - 1u) / setup.mesh.primitivesPerGroup;
    const auto output = device.DispatchMeshPrepass(result, setup.mesh, RecordDwords, draw, groups);
    const auto& mesh = setup.mesh;
    for (std::uint32_t instance = 0; instance < setup.instances; ++instance) {
        for (std::uint32_t group = 0; group < groups; ++group) {
            const auto slot = instance * groups + group;
            for (std::uint32_t primitive = 0; primitive < mesh.maxPrimitives; ++primitive) {
                const auto triangle = group * mesh.primitivesPerGroup + primitive;
                const auto* corners = output.indices.data() + (static_cast<std::size_t>(slot) * mesh.maxPrimitives + primitive) * 3u;
                const auto where = std::string(setup.name) + ": instance " + std::to_string(instance) + " triangle " + std::to_string(triangle);
                if (triangle >= Triangles) {
                    Require(corners[0] == corners[1] && corners[1] == corners[2], where + " past the draw is not degenerate");
                    continue;
                }
                for (std::uint32_t corner = 0; corner < 3u; ++corner) {
                    const auto vertex = corners[corner];
                    Require(vertex >= slot * mesh.maxVertices && vertex < (slot + 1u) * mesh.maxVertices, where + " corner " + std::to_string(corner) + " indexes vertex " + std::to_string(vertex) + " outside its group");
                    const auto record = std::span(output.records).subspan(static_cast<std::size_t>(vertex) * RecordDwords, RecordDwords);
                    Require(SameVertex(record, TriangleVertex(triangle, corner)), where + " corner " + std::to_string(corner) + " record differs from the input vertex (x bits 0x" + std::to_string(record[0]) + ")");
                }
            }
        }
    }
    const auto summary = ShaderRecompiler::SummarizeMeshPrepass(output.records, output.indices, static_cast<std::uint64_t>(groups) * setup.instances, mesh.maxVertices, mesh.maxPrimitives, RecordDwords);
    Require(summary.liveTriangles == static_cast<std::uint64_t>(Triangles) * setup.instances && summary.unwrittenTriangles == 0u && summary.indicesOutsideSlot == 0u && summary.cornersOnUnwrittenRecords == 0u && summary.nonFinitePositions == 0u && summary.positionsBehindEye == 0u,
            std::string(setup.name) + ": the pre-pass summary disagrees: " + summary.Describe());
    std::printf("PASS %s (%u groups): %s\n", setup.name, groups, summary.Describe().c_str());
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
        auto wave32 = SmallSubgroup;
        wave32.threadsPerGroup = 32u;
        for (const auto& setup : {Case{"non-indexed one-wave groups", SmallSubgroup, 64u, false, 1u}, Case{"indexed one-wave groups", SmallSubgroup, 64u, true, 1u}, Case{"non-indexed two-wave groups", WideSubgroup, 64u, false, 1u},
                                  Case{"indexed wave32 groups", wave32, 32u, true, 1u}, Case{"two instances", SmallSubgroup, 64u, false, 2u}}) {
            Run(device, setup);
        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
