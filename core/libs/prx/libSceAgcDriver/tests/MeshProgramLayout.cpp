#include "BdaAbi.hpp"
#include "NggGeometryProgram.hpp"
#include "Recompiler.hpp"
#include "ControlFlow/RequestSerializer.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "spirv/unified1/spirv.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <optional>
#include <stdexcept>
#include <span>
#include <string_view>
#include <vector>

using namespace ShaderRecompiler;

namespace {

constexpr MeshConfiguration SmallSubgroup{4u, 4u, 12u, 12u, 4u, 64u, 1024u, 0u, 4u};
constexpr MeshConfiguration StripSubgroup{6u, 10u, 12u, 30u, 10u, 64u, 1024u, 0u, 4u};

SpirvTarget MeshlessTarget() {
    static constexpr std::array<std::uint32_t, 3> capabilities{
        spv::CapabilityInt64, spv::CapabilityPhysicalStorageBufferAddresses, spv::CapabilityStorageBuffer8BitAccess,
    };
    static constexpr std::array<std::string_view, 3> extensions{
        "SPV_KHR_8bit_storage", "SPV_KHR_float_controls", "SPV_KHR_physical_storage_buffer",
    };
    SpirvTarget target{0x00401000u, 0x00010300u, 32, BdaAbi::Version, capabilities, extensions, false, {1024, 1024, 64}, 1024, 32768, {}, {}};
    target.subgroupStages = 0x32u;
    return target;
}

constexpr std::uint32_t LayerExportControl = (1u << 21u) | (1u << 18u);

alignas(256) constexpr auto LayerCode = [] {
    auto code = NggGeometryProgram::Code;
    code[101] = 0xf80000d4u;
    return code;
}();

template<typename TBody>
auto WithRequest(std::span<const std::uint32_t> code, std::uint32_t waveSize, const std::optional<MeshConfiguration>& mesh, ShaderStage stage, std::uint32_t control, TBody body) {
    const std::vector<std::uint32_t> userData(12, 0u);
    const std::array<MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const RecompileRequest request{
        {stage, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {waveSize, 0, userData, std::nullopt, std::nullopt, ShaderVertexStageInfo{}, memory},
        MeshlessTarget(),
        {0, 0, 0, MeshDrawPushOffsetBytes},
        GraphicsCompileContext{0, {}, mesh, std::nullopt, {0, 0, 0, 1}, control},
        false
    };
    return body(request);
}

MeshProgramLayout Describe(std::uint32_t waveSize, const std::optional<MeshConfiguration>& mesh, ShaderStage stage = ShaderStage::Mesh) {
    const auto& code = waveSize == 32u ? NggGeometryProgram::Wave32Code : NggGeometryProgram::Code;
    return WithRequest(code, waveSize, mesh, stage, 0u, [](const RecompileRequest& request) { return DescribeMeshProgram(request); });
}

MeshProgramLayout DescribeLayer(std::uint32_t control, bool compute) {
    auto mesh = SmallSubgroup;
    mesh.compute = compute;
    return WithRequest(LayerCode, 64u, mesh, ShaderStage::Mesh, control, [](const RecompileRequest& request) { return DescribeMeshProgram(request); });
}

bool Run(const char* name, const std::function<bool()>& test) {
    bool passed = false;
    try {
        passed = test();
    } catch (const std::exception& error) {
        std::printf("FAIL %s: %s\n", name, error.what());
        return false;
    }
    std::printf("%s %s\n", passed ? "PASS" : "FAIL", name);
    return passed;
}

bool SharedShape(const MeshProgramLayout& layout) {
    return layout.positionComponents == std::array<std::uint32_t, 4>{0xfu, 0u, 0u, 0u} && layout.parameterMask == 1u && layout.ParameterCount() == 1u && layout.primitiveExport && !layout.layer && !layout.viewportIndex && !layout.pointSize &&
           layout.clipCullDistances == 0u && layout.allocations == 1u && layout.barriers >= 1u && layout.sharedMemory && !layout.VertexStageCandidate() && layout.recordDwords == 8u;
}

}

int main() {
    bool passed = true;

    passed &= Run("a wave64 NGG geometry program reports its exports, LDS, barriers and allocation", [] {
        const auto layout = Describe(64u, SmallSubgroup);
        return SharedShape(layout) && layout.waveSize == 64u && layout.wavesPerGroup == 1u && layout.recordsPerGroup == 12u && layout.vertexBytesPerGroup == 12u * 32u && layout.indexBytesPerGroup == 4u * 12u;
    });

    passed &= Run("a wave32 NGG geometry program counts two waves per group", [] {
        const auto layout = Describe(32u, SmallSubgroup);
        return SharedShape(layout) && layout.waveSize == 32u && layout.wavesPerGroup == 2u;
    });

    passed &= Run("triangle-list groups follow the index count", [] {
        const auto layout = Describe(64u, SmallSubgroup);
        return layout.GroupsFor(0u) == 0u && layout.GroupsFor(2u) == 0u && layout.GroupsFor(3u) == 1u && layout.GroupsFor(12u) == 1u && layout.GroupsFor(14u) == 1u && layout.GroupsFor(15u) == 2u && layout.GroupsFor(120u) == 10u;
    });

    passed &= Run("triangle-strip groups advance one index per primitive", [] {
        const auto layout = Describe(64u, StripSubgroup);
        return layout.recordsPerGroup == 30u && layout.indexBytesPerGroup == 10u * 12u && layout.GroupsFor(12u) == 1u && layout.GroupsFor(13u) == 2u && layout.GroupsFor(22u) == 2u && layout.GroupsFor(23u) == 3u;
    });

    passed &= Run("the pre-pass summary counts live, degenerate and unwritten triangles and the view they reach", [] {
        constexpr std::uint32_t maxVertices = 4u;
        constexpr std::uint32_t maxPrimitives = 3u;
        constexpr std::uint32_t recordDwords = 8u;
        std::vector<std::uint32_t> records(2u * maxVertices * recordDwords, MeshPrepassUnwritten);
        std::vector<std::uint32_t> indices(2u * maxPrimitives * 3u, MeshPrepassUnwritten);
        const auto place = [&](std::uint32_t record, float x, float y, float z, float w) {
            const std::array<float, 4> position{x, y, z, w};
            std::memcpy(records.data() + record * recordDwords, position.data(), sizeof(position));
        };
        place(0u, -0.5f, -0.5f, 0.25f, 1.0f);
        place(1u, 0.5f, -0.5f, 0.25f, 1.0f);
        place(2u, 0.0f, 1.0f, 0.5f, 2.0f);
        place(4u, 4.0f, 4.0f, 0.5f, 1.0f);
        place(5u, 6.0f, 4.0f, 0.5f, 1.0f);
        place(6u, 1.0f, 1.0f, 1.0f, -1.0f);
        const std::array<std::uint32_t, 18> triangles{0u, 1u, 2u, 2u, 2u, 2u, MeshPrepassUnwritten, MeshPrepassUnwritten, MeshPrepassUnwritten, 4u, 5u, 6u, 4u, 5u, 7u, 4u, 5u, 9u};
        std::copy(triangles.begin(), triangles.end(), indices.begin());
        const auto summary = SummarizeMeshPrepass(records, indices, 2u, maxVertices, maxPrimitives, recordDwords);
        return summary.triangles == 6u && summary.liveTriangles == 4u && summary.degenerateTriangles == 1u && summary.unwrittenTriangles == 1u && summary.indicesOutsideSlot == 1u && summary.cornersOnUnwrittenRecords == 1u &&
               summary.referencedRecords == 6u && summary.positionsBehindEye == 1u && summary.nonFinitePositions == 0u && summary.trianglesInView == 1u && summary.emptySlots == 0u && summary.ndcSeen && summary.ndcMin[0] == -0.5f && summary.ndcMax[0] == 6.0f &&
               summary.ndcMax[2] == 0.5f && summary.Describe().find("4 live") != std::string::npos;
    });

    passed &= Run("the pre-pass summary refuses buffers smaller than their slots", [] {
        const std::vector<std::uint32_t> records(8u, 0u);
        const std::vector<std::uint32_t> indices(3u, 0u);
        try {
            (void)SummarizeMeshPrepass(records, indices, 2u, 1u, 1u, 8u);
        } catch (const std::runtime_error&) {
            return true;
        }
        return false;
    });

    passed &= Run("a request without a mesh configuration throws", [] {
        try {
            (void)Describe(64u, std::nullopt);
        } catch (const std::runtime_error&) {
            return true;
        }
        return false;
    });

    passed &= Run("a request outside the mesh stage throws", [] {
        try {
            (void)Describe(64u, SmallSubgroup, ShaderStage::Vertex);
        } catch (const std::runtime_error&) {
            return true;
        }
        return false;
    });

    passed &= Run("PA_CL_VS_OUT_CNTL names position 1 z the layer of a compute-emulated program", [] {
        const auto layout = DescribeLayer(LayerExportControl, true);
        return layout.layer && layout.positionComponents[1] == 4u && layout.parameterMask == 0u && layout.recordDwords == 8u;
    });

    passed &= Run("without PA_CL_VS_OUT_CNTL position 1 z means nothing", [] {
        const auto layout = DescribeLayer(0u, true);
        return !layout.layer && layout.positionComponents[1] == 4u && layout.recordDwords == 4u;
    });

    passed &= Run("native stages keep ignoring layer and viewport exports", [] {
        auto mesh = SmallSubgroup;
        return WithRequest(LayerCode, 64u, mesh, ShaderStage::Mesh, LayerExportControl, [](const RecompileRequest& request) { return PositionExportControl(request) == (1u << 21u); }) &&
               WithRequest(LayerCode, 64u, std::optional<MeshConfiguration>{}, ShaderStage::Vertex, LayerExportControl | 1u, [](const RecompileRequest& request) { return PositionExportControl(request) == ((1u << 21u) | 1u); }) &&
               !DescribeLayer(LayerExportControl, false).layer;
    });

    passed &= Run("the request format keeps PA_CL_VS_OUT_CNTL and the mesh compute flag", [] {
        auto mesh = SmallSubgroup;
        mesh.compute = true;
        return WithRequest(LayerCode, 64u, mesh, ShaderStage::Mesh, LayerExportControl, [](const RecompileRequest& request) {
            const auto restored = RequestSerializer{}.Deserialize(RequestSerializer{}.Serialize(request));
            return restored.request.graphics.has_value() && restored.request.graphics->positionExportControl == LayerExportControl && restored.request.graphics->mesh.has_value() && restored.request.graphics->mesh->compute;
        });
    });

    passed &= Run("the cache key separates PA_CL_VS_OUT_CNTL and the mesh compute flag", [] {
        auto mesh = SmallSubgroup;
        const auto key = [&](std::uint32_t control) {
            return WithRequest(LayerCode, 64u, mesh, ShaderStage::Mesh, control, [](const RecompileRequest& request) {
                std::vector<std::uint64_t> words;
                BuildPreparedShaderKey(request, words);
                return words;
            });
        };
        const auto native = key(LayerExportControl);
        const auto plain = key(0u);
        mesh.compute = true;
        const auto emulated = key(LayerExportControl);
        const auto emulatedPlain = key(0u);
        return native != plain && native != emulated && emulated != emulatedPlain;
    });

    passed &= Run("the pass-through vertex shader exports the program's parameters and carries the mesh draw words", [] {
        auto layout = DescribeLayer(LayerExportControl, true);
        layout.parameterMask = 0x5u;
        layout.recordDwords = 16u;
        const auto shader = BuildMeshPassThroughVertexShader(layout, MeshlessTarget(), false);
        return !shader.spirv.empty() && shader.spirv[0] == 0x07230203u && shader.parameterExports == std::vector<std::uint32_t>{0u, 2u} && shader.pushConstants.size() == MeshDrawPushOffsetBytes + MeshDrawPushBytes && shader.bdaAbiVersion == BdaAbi::Version && shader.bindings.size() == 1u && shader.bindings[0].role == DescriptorRole::FaultBuffer;
    });

    passed &= Run("the pass-through vertex shader drops the layer unless asked and the device can write it", [] {
        const auto layout = DescribeLayer(LayerExportControl, true);
        const auto dropped = BuildMeshPassThroughVertexShader(layout, MeshlessTarget(), false);
        try {
            (void)BuildMeshPassThroughVertexShader(layout, MeshlessTarget(), true);
        } catch (const std::runtime_error&) {
            return !dropped.spirv.empty();
        }
        return false;
    });

    passed &= Run("the pass-through vertex shader rejects a record smaller than its exports", [] {
        auto layout = Describe(64u, SmallSubgroup);
        layout.recordDwords = 4u;
        try {
            (void)BuildMeshPassThroughVertexShader(layout, MeshlessTarget(), false);
        } catch (const std::runtime_error&) {
            return true;
        }
        return false;
    });

    return passed ? 0 : 1;
}
