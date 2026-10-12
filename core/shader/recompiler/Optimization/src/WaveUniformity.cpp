#include "Optimization/WaveUniformity.hpp"
#include <unordered_map>

namespace ShaderRecompiler {

namespace {

bool UniformSource(const IrValue& value) {
    switch (value.Opcode()) {
    case IrOpcode::Ballot:
    case IrOpcode::ReadFirstLane:
    case IrOpcode::ReadLane:
    case IrOpcode::GetUserData:
    case IrOpcode::GetShaderBase:
    case IrOpcode::ReadConst:
    case IrOpcode::ReadConstBuffer:
    case IrOpcode::MeshDrawParameter:
    case IrOpcode::MeshArgument:
    case IrOpcode::GetSrtResource:
    case IrOpcode::GetBufferResource:
    case IrOpcode::GetAddressResource:
    case IrOpcode::GetScratchResource:
    case IrOpcode::GetImageResource:
    case IrOpcode::GetSamplerResource:
        return true;
    default:
        return false;
    }
}

bool LaneSource(const IrProgram& program, const IrValue& value) {
    switch (value.Opcode()) {
    case IrOpcode::LaneId:
    case IrOpcode::GetAttribute:
    case IrOpcode::GetInterpolationParameter:
    case IrOpcode::GetInterpolationParameterF16:
    case IrOpcode::GetTessellationAttribute:
    case IrOpcode::DppMoveU32:
    case IrOpcode::DppUpdateU32:
    case IrOpcode::Permlane16U32:
    case IrOpcode::PermuteU32:
    case IrOpcode::BpermuteU32:
    case IrOpcode::SwizzleU32:
    case IrOpcode::WriteLane:
    case IrOpcode::DataAppend:
    case IrOpcode::DataConsume:
    case IrOpcode::LoadVectorWindow:
        return true;
    case IrOpcode::GetBuiltin: {
        const auto* kind = value.Argument(0)->Resolve();
        return kind == nullptr || !kind->HasImmediate() || static_cast<StageInputKind>(kind->ImmediateU32()) != StageInputKind::WorkgroupId;
    }
    default:
        break;
    }
    const auto address = AddressOpcodeInfoOf(value.Opcode()).access;
    if (address == AddressAccess::Read) {
        const auto index = value.Flags<MemoryFlags>().index;
        const auto& memory = program.Resources().memoryInfo;
        if (index < memory.size() && memory[index].kind == ResourceKind::ScalarAddress) return false;
    }
    return address != AddressAccess::None || BufferAccessOf(value.Opcode()) != BufferAccess::None || SharedAccessOf(value.Opcode()) != SharedAccess::None || ImageOpcodeInfoOf(value.Opcode()).access != ImageAccess::None;
}

}

bool IsWaveMaskBranch(BranchCondition condition) {
    return condition == BranchCondition::ExecZero || condition == BranchCondition::ExecNonZero || condition == BranchCondition::VccZero || condition == BranchCondition::VccNonZero;
}

bool IsWaveMaskBranch(const BlockInfo& info) {
    return !info.laneLocalBranch && IsWaveMaskBranch(info.terminator.condition);
}

std::unordered_set<const IrValue*> WaveUniformValues(const IrProgram& program) {
    std::unordered_map<const IrValue*, bool> varying;
    const auto isVarying = [&](const IrValue* value) {
        const auto* resolved = value->Resolve();
        if (resolved == nullptr || resolved->HasImmediate()) return false;
        const auto found = varying.find(resolved);
        return found != varying.end() && found->second;
    };
    bool changed = true;
    while (changed) {
        changed = false;
        for (const IrBlock* block : program.BlockOrder()) {
            for (const IrValue* inst : block->Instructions()) {
                bool lanes = !UniformSource(*inst) && LaneSource(program, *inst);
                for (std::size_t index = 0; !lanes && !UniformSource(*inst) && index < inst->ArgumentCount(); index++) {
                    if (inst->Argument(index) != nullptr) lanes = isVarying(inst->Argument(index));
                }
                auto& entry = varying[inst];
                if (lanes && !entry) {
                    entry = true;
                    changed = true;
                }
            }
        }
    }
    std::unordered_set<const IrValue*> uniform;
    for (const auto& [value, lanes] : varying) {
        if (!lanes) uniform.insert(value);
    }
    return uniform;
}

}
