#include "Recompiler.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "SpirvBackend/SpirvAnalysis.hpp"
#include <bit>
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

}
