#include "Recompiler.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "SpirvBackend/SpirvAnalysis.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace ShaderRecompiler {

namespace {

constexpr std::uint32_t InputPrimitiveSize(std::uint32_t primitive) {
    return primitive == 1u ? 1u : primitive == 2u ? 2u : 3u;
}

constexpr std::uint32_t InputPrimitiveStep(std::uint32_t primitive) {
    return primitive == 5u || primitive == 6u ? 1u : InputPrimitiveSize(primitive);
}

}

std::uint32_t MeshProgramLayout::ParameterCount() const {
    return static_cast<std::uint32_t>(std::popcount(parameterMask));
}

bool MeshProgramLayout::VertexStageCandidate() const {
    return !sharedMemory && barriers == 0u && !subgroupOperations && allocations == 0u;
}

std::uint32_t MeshProgramLayout::GroupsFor(std::uint32_t indexCount) const {
    const auto size = InputPrimitiveSize(configuration.inputPrimitive);
    if (indexCount < size || configuration.primitivesPerGroup == 0u) return 0u;
    const auto primitives = (indexCount - size) / InputPrimitiveStep(configuration.inputPrimitive) + 1u;
    return (primitives - 1u) / configuration.primitivesPerGroup + 1u;
}

MeshProgramLayout DescribeMeshProgram(const RecompileRequest& request) {
    if (request.shader.stage != ShaderStage::Mesh || !request.graphics.has_value() || !request.graphics->mesh.has_value()) {
        throw std::runtime_error("mesh program layout: the request is not a mesh-stage program with a mesh configuration");
    }
    MeshProgramLayout layout;
    layout.configuration = *request.graphics->mesh;
    layout.waveSize = request.context.waveSize;
    if (layout.waveSize != 32u && layout.waveSize != 64u) {
        throw std::runtime_error("mesh program layout: unsupported wave size " + std::to_string(layout.waveSize));
    }
    layout.wavesPerGroup = (layout.configuration.threadsPerGroup + layout.waveSize - 1u) / layout.waveSize;

    const auto program = PrepareResourceProgram(request);
    for (const auto& exported : program.Metadata().exportInfo) {
        switch (exported.kind) {
        case ExportTargetKind::Position:
            if (exported.index >= layout.positionComponents.size()) throw std::runtime_error("mesh program layout: position export " + std::to_string(exported.index) + " is out of range");
            layout.positionComponents[exported.index] |= exported.en;
            break;
        case ExportTargetKind::Parameter:
            if (exported.index >= 32u) throw std::runtime_error("mesh program layout: parameter export " + std::to_string(exported.index) + " is out of range");
            layout.parameterMask |= 1u << exported.index;
            break;
        case ExportTargetKind::Primitive:
            layout.primitiveExport = true;
            break;
        case ExportTargetKind::Null:
            break;
        default:
            throw std::runtime_error("mesh program layout: the program exports to a pixel target");
        }
    }
    const auto control = PositionExportControl(request);
    for (std::uint32_t index = 1; index < layout.positionComponents.size(); index++) {
        for (std::uint32_t component = 0; component < 4u; component++) {
            if ((layout.positionComponents[index] & (1u << component)) == 0u) continue;
            const auto output = DecodePositionExportComponent(control, index, component);
            layout.layer |= output.layer;
            layout.viewportIndex |= output.viewport;
            layout.pointSize |= output.pointSize;
            if (output.clipDistance != std::numeric_limits<std::uint32_t>::max() || output.cullDistance != std::numeric_limits<std::uint32_t>::max()) layout.clipCullDistances++;
        }
    }
    for (const auto& block : program.Blocks()) {
        for (const IrValue* inst : block->Instructions()) {
            if (inst->Opcode() == IrOpcode::Barrier) layout.barriers++;
            if (inst->Opcode() == IrOpcode::MeshAllocate) layout.allocations++;
            if (SharedAccessOf(inst->Opcode()) != SharedAccess::None) layout.sharedMemory = true;
        }
    }
    const auto requirements = AnalyzeProgramRequirements(program);
    layout.subgroupOperations = requirements.subgroupBallot || requirements.subgroupShuffle || requirements.subgroupLocalInvocationId;

    const auto scalars = (layout.layer ? 1u : 0u) + (layout.viewportIndex ? 1u : 0u) + (layout.pointSize ? 1u : 0u) + layout.clipCullDistances;
    layout.recordDwords = (4u + 4u * layout.ParameterCount() + scalars + 3u) & ~3u;
    layout.recordsPerGroup = layout.configuration.maxVertices;
    layout.vertexBytesPerGroup = static_cast<std::uint64_t>(layout.recordsPerGroup) * layout.recordDwords * 4u;
    layout.indexBytesPerGroup = static_cast<std::uint64_t>(layout.configuration.maxPrimitives) * 3u * 4u;
    return layout;
}


MeshPrepassSummary SummarizeMeshPrepass(std::span<const std::uint32_t> records, std::span<const std::uint32_t> indices, std::uint64_t slots, std::uint32_t maxVertices, std::uint32_t maxPrimitives, std::uint32_t recordDwords) {
    if (recordDwords < 4u || maxVertices == 0u || maxPrimitives == 0u) throw std::runtime_error("mesh pre-pass summary: invalid slot shape");
    if (records.size() < slots * maxVertices * recordDwords || indices.size() < slots * maxPrimitives * 3u) throw std::runtime_error("mesh pre-pass summary: the buffers are smaller than their slots");
    MeshPrepassSummary summary;
    summary.slots = slots;
    std::vector<std::uint8_t> seen(slots * maxVertices, 0u);
    const auto position = [&](std::uint64_t record) {
        std::array<float, 4> value{};
        std::memcpy(value.data(), records.data() + record * recordDwords, sizeof(value));
        return value;
    };
    const auto written = [&](std::uint64_t record) {
        const auto* words = records.data() + record * recordDwords;
        return !(words[0] == MeshPrepassUnwritten && words[1] == MeshPrepassUnwritten && words[2] == MeshPrepassUnwritten && words[3] == MeshPrepassUnwritten);
    };
    for (std::uint64_t slot = 0; slot < slots; ++slot) {
        const auto base = slot * maxVertices;
        std::uint64_t live = 0;
        for (std::uint32_t primitive = 0; primitive < maxPrimitives; ++primitive) {
            const auto* corner = indices.data() + (slot * maxPrimitives + primitive) * 3u;
            ++summary.triangles;
            if (corner[0] == MeshPrepassUnwritten || corner[1] == MeshPrepassUnwritten || corner[2] == MeshPrepassUnwritten) {
                ++summary.unwrittenTriangles;
                continue;
            }
            if (corner[0] == corner[1] || corner[1] == corner[2] || corner[0] == corner[2]) {
                ++summary.degenerateTriangles;
                continue;
            }
            ++summary.liveTriangles;
            ++live;
            bool inside = true;
            bool inView = false;
            for (std::uint32_t index = 0; index < 3u; ++index) {
                if (corner[index] < base || corner[index] >= base + maxVertices) {
                    inside = false;
                    ++summary.indicesOutsideSlot;
                }
            }
            if (!inside) continue;
            for (std::uint32_t index = 0; index < 3u; ++index) {
                const auto record = static_cast<std::uint64_t>(corner[index]);
                if (!written(record)) {
                    ++summary.cornersOnUnwrittenRecords;
                    continue;
                }
                const auto value = position(record);
                const bool first = seen[record] == 0u;
                seen[record] = 1u;
                if (first) ++summary.referencedRecords;
                if (!std::isfinite(value[0]) || !std::isfinite(value[1]) || !std::isfinite(value[2]) || !std::isfinite(value[3])) {
                    if (first) ++summary.nonFinitePositions;
                    continue;
                }
                if (value[3] <= 0.0f) {
                    if (first) ++summary.positionsBehindEye;
                    continue;
                }
                const std::array<float, 3> ndc{value[0] / value[3], value[1] / value[3], value[2] / value[3]};
                if (std::fabs(ndc[0]) <= 1.0f && std::fabs(ndc[1]) <= 1.0f) inView = true;
                for (std::uint32_t axis = 0; axis < 3u; ++axis) {
                    summary.ndcMin[axis] = summary.ndcSeen ? std::min(summary.ndcMin[axis], ndc[axis]) : ndc[axis];
                    summary.ndcMax[axis] = summary.ndcSeen ? std::max(summary.ndcMax[axis], ndc[axis]) : ndc[axis];
                }
                summary.ndcSeen = true;
            }
            if (inView) ++summary.trianglesInView;
        }
        if (live == 0u) ++summary.emptySlots;
    }
    return summary;
}

std::string MeshPrepassSummary::Describe() const {
    char text[640];
    std::snprintf(text, sizeof(text),
                  "%llu slots (%llu without a live triangle), %llu triangles: %llu live, %llu degenerate, %llu never written; %llu indices outside their slot, %llu corners on unwritten records; %llu records referenced: %llu non-finite, %llu with w <= 0; %llu live triangles reach the view",
                  static_cast<unsigned long long>(slots), static_cast<unsigned long long>(emptySlots), static_cast<unsigned long long>(triangles), static_cast<unsigned long long>(liveTriangles), static_cast<unsigned long long>(degenerateTriangles),
                  static_cast<unsigned long long>(unwrittenTriangles), static_cast<unsigned long long>(indicesOutsideSlot), static_cast<unsigned long long>(cornersOnUnwrittenRecords), static_cast<unsigned long long>(referencedRecords),
                  static_cast<unsigned long long>(nonFinitePositions), static_cast<unsigned long long>(positionsBehindEye), static_cast<unsigned long long>(trianglesInView));
    std::string result = text;
    if (ndcSeen) {
        std::snprintf(text, sizeof(text), "; NDC x [%g, %g] y [%g, %g] z [%g, %g]", ndcMin[0], ndcMax[0], ndcMin[1], ndcMax[1], ndcMin[2], ndcMax[2]);
        result += text;
    }
    return result;
}

}
