#include "BdaAbi.hpp"
#include "NggGeometryProgram.hpp"
#include "Recompiler.hpp"
#include "spirv/unified1/spirv.hpp"
#include <array>
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

MeshProgramLayout Describe(std::uint32_t waveSize, const std::optional<MeshConfiguration>& mesh, ShaderStage stage = ShaderStage::Mesh) {
    const auto& code = waveSize == 32u ? NggGeometryProgram::Wave32Code : NggGeometryProgram::Code;
    const std::vector<std::uint32_t> userData(12, 0u);
    const std::array<MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(std::span(code))}}};
    const RecompileRequest request{
        {stage, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {waveSize, 0, userData, std::nullopt, std::nullopt, ShaderVertexStageInfo{}, memory},
        MeshlessTarget(),
        {0, 0, 0, MeshDrawPushOffsetBytes},
        GraphicsCompileContext{0, {}, mesh, std::nullopt, {0, 0, 0, 1}},
        false
    };
    return DescribeMeshProgram(request);
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
           layout.clipCullDistances == 0u && layout.allocations == 1u && layout.barriers >= 1u && layout.sharedMemory && !layout.VertexStageCandidate() && layout.recordDwords == 8u && !layout.perCornerRecords;
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

    return passed ? 0 : 1;
}
