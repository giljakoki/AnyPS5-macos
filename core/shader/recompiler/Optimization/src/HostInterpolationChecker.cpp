#include "Optimization/HostInterpolationChecker.hpp"
#include "IntermediateRepresentation/IrBlock.hpp"
#include "IntermediateRepresentation/IrMetadata/StageIO.hpp"
#include "Optimization/DeadCodeEliminator.hpp"
#include "Optimization/MaskedSelectEliminator.hpp"
#include <array>
#include <cstdio>
#include <string>
#include <vector>

namespace ShaderRecompiler {
namespace {

bool isCopy(const IrValue& value) {
    return value.IsIdentity() || value.Opcode() == IrOpcode::BitCastF32U32 || value.Opcode() == IrOpcode::BitCastU32F32;
}

const IrValue* lookThrough(const IrValue* value) {
    value = value->Resolve();
    while (value->Opcode() == IrOpcode::BitCastF32U32 || value->Opcode() == IrOpcode::BitCastU32F32) value = value->Argument(0)->Resolve();
    return value;
}

const IrValue* partialOf(const IrValue& interpolation) {
    const auto* exec = interpolation.Argument(4)->Resolve();
    const auto* value = lookThrough(interpolation.Argument(3));
    while (value->Opcode() == IrOpcode::SelectU32) {
        if (value->Argument(0)->Resolve() != exec) return value;
        value = lookThrough(value->Argument(1));
    }
    return value;
}

const IrValue* barycentricOperand(const IrValue* value, const IrValue* exec) {
    value = lookThrough(value);
    while (value->Opcode() == IrOpcode::SelectU32 && value->Argument(0)->Resolve() == exec) value = lookThrough(value->Argument(1));
    return value;
}

bool isBarycentric(const IrValue& value) {
    if (value.Opcode() != IrOpcode::GetBuiltin) return false;
    const auto kind = static_cast<StageInputKind>(value.Argument(0)->Resolve()->ImmediateU32());
    return kind == StageInputKind::BaryCoordSmooth || kind == StageInputKind::BaryCoordNoPerspective;
}

bool isBarycentric(const IrValue& value, StageInputKind kind, std::uint32_t component) {
    return isBarycentric(value) && static_cast<StageInputKind>(value.Argument(0)->Resolve()->ImmediateU32()) == kind && value.Argument(1)->Resolve()->ImmediateU32() == component;
}

std::string hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%x", value);
    return text;
}

std::string describe(const IrProgram& program, const IrValue& inst) {
    const auto parameter = [&] { return " attr" + std::to_string(inst.Argument(0)->Resolve()->ImmediateU32()) + "." + "xyzw"[inst.Argument(1)->Resolve()->ImmediateU32() & 3u]; };
    std::string text;
    if (inst.Opcode() == IrOpcode::InterpolateHostP1) {
        text = "v_interp_p1_f32" + parameter();
    } else if (inst.Opcode() == IrOpcode::InterpolateHostP2) {
        text = "v_interp_p2_f32" + parameter();
    } else if (inst.Opcode() == IrOpcode::GetInterpolationParameter) {
        const auto mode = inst.Argument(2)->Resolve()->ImmediateU32();
        text = std::string("v_interp_mov_f32 ") + (mode == 0u ? "p10" : mode == 1u ? "p20" : "p0") + parameter();
    } else if (isBarycentric(inst)) {
        const bool smooth = static_cast<StageInputKind>(inst.Argument(0)->Resolve()->ImmediateU32()) == StageInputKind::BaryCoordSmooth;
        text = std::string(smooth ? "perspective " : "linear ") + (inst.Argument(1)->Resolve()->ImmediateU32() == 0u ? "I" : "J") + " VGPR";
    } else {
        text = std::string(IrOpcodeName(inst.Opcode()));
    }
    const auto& blocks = program.Blocks();
    const auto& info = program.Metadata().blockInfo;
    for (std::size_t i = 0; i < blocks.size() && i < info.size(); ++i) {
        if (blocks[i].get() == inst.Parent()) return text + " (block pc " + hex(info[i].startPc) + ".." + hex(info[i].endPc) + ")";
    }
    return text;
}

void listReaders(const IrProgram& program, const IrValue& value, std::string& text) {
    for (const auto& use : value.OperandUses()) {
        if (isCopy(*use.user)) {
            listReaders(program, *use.user, text);
        } else {
            text += (text.empty() ? "" : ", ") + describe(program, *use.user) + " operand " + std::to_string(use.operand);
        }
    }
}

std::string readers(const IrProgram& program, const IrValue& value) {
    std::string text;
    listReaders(program, value, text);
    return text;
}

bool inputIsFlat(const IrProgram& program, const ShaderPixelInputInfo& pixel, std::uint32_t input) {
    const auto& metadata = program.Metadata();
    return pixel.InputIsFlat(input, metadata.pixelVertexZeroReads, metadata.pixelOtherReads);
}

template<typename TAccept>
bool usesAll(const IrValue& value, const TAccept& accept) {
    for (const auto& use : value.OperandUses()) {
        if (isCopy(*use.user)) {
            if (!usesAll(*use.user, accept)) return false;
        } else if (!accept(*use.user, use.operand)) {
            return false;
        }
    }
    return true;
}

bool sameCondition(const IrValue& left, const IrValue& right) {
    return left.Argument(0)->Resolve() == right.Argument(0)->Resolve();
}

bool partialSelect(const IrValue& select) {
    return usesAll(select, [&](const IrValue& user, std::size_t operand) {
        return (user.Opcode() == IrOpcode::InterpolateHostP2 && operand == 3u) || (user.Opcode() == IrOpcode::SelectU32 && operand == 2u && sameCondition(user, select));
    });
}

bool sameParameter(const IrValue& left, const IrValue& right) {
    return left.Argument(0)->Resolve()->ImmediateU32() == right.Argument(0)->Resolve()->ImmediateU32() && left.Argument(1)->Resolve()->ImmediateU32() == right.Argument(1)->Resolve()->ImmediateU32();
}

bool exact(const IrValue& inst, const IrProgram& program, const ShaderPixelInputInfo& pixel, std::vector<IrValue*>& selects, std::string& reason) {
    if (inst.Opcode() == IrOpcode::InterpolateHostP1) {
        const bool paired = usesAll(inst, [&](const IrValue& user, std::size_t operand) {
            if (user.Opcode() == IrOpcode::InterpolateHostP2 && operand == 3u) return true;
            if (user.Opcode() != IrOpcode::SelectU32 || operand != 1u || !partialSelect(user)) return false;
            selects.push_back(const_cast<IrValue*>(&user));
            return true;
        });
        if (!paired) reason = describe(program, inst) + " result is read by " + readers(program, inst);
        return paired;
    }
    if (inst.Opcode() == IrOpcode::GetInterpolationParameter) {
        const auto input = inst.Argument(0)->Resolve()->ImmediateU32();
        const auto mode = inst.Argument(2)->Resolve()->ImmediateU32();
        if (pixel.InputIsDefault(input) || (mode == 2u && inputIsFlat(program, pixel, input))) return true;
        const bool otherReads = input < 32u && (program.Metadata().pixelOtherReads & (1u << input)) != 0u;
        const auto read = mode == 2u ? " reads P0 of input " + std::to_string(input) + ", which is not flat" + (otherReads ? " and is also interpolated or read as P10/P20" : "") : " reads a difference of two vertex values of input " + std::to_string(input) + ", which host interpolation does not provide";
        reason = describe(program, inst) + read + " (SPI_PS_INPUT_CNTL_" + std::to_string(input) + "=" + hex(input < 32u ? pixel.interpolatorSettings[input] : 0u) + ")";
        return false;
    }
    if (inst.Opcode() != IrOpcode::InterpolateHostP2) return true;
    const auto* partial = partialOf(inst);
    if (partial->Opcode() != IrOpcode::InterpolateHostP1 || !sameParameter(inst, *partial)) {
        reason = describe(program, inst) + " continues " + describe(program, *partial) + ", not its own v_interp_p1_f32 under the same EXEC";
        return false;
    }
    const auto& metadata = program.Metadata();
    const auto kind = pixel.InputIsLinear(inst.Argument(0)->Resolve()->ImmediateU32(), metadata.pixelLinearInputs, metadata.pixelPerspectiveInputs) ? StageInputKind::BaryCoordNoPerspective : StageInputKind::BaryCoordSmooth;
    const auto* exec = inst.Argument(4)->Resolve();
    const auto* i = barycentricOperand(partial->Argument(2), exec);
    const auto* j = barycentricOperand(inst.Argument(2), exec);
    if (isBarycentric(*i, kind, 0u) && isBarycentric(*j, kind, 1u)) return true;
    reason = describe(program, inst) + " interpolates with I = " + describe(program, *i) + " and J = " + describe(program, *j) + ", not the unchanged " + (kind == StageInputKind::BaryCoordSmooth ? "perspective" : "linear") + " I/J VGPRs";
    return false;
}

bool sharesSlots(const IrProgram& program, const ShaderPixelInputInfo& pixel, std::string& reason) {
    constexpr std::uint32_t unassigned = 3u;
    std::array<std::uint32_t, 32> interpolation {};
    std::array<std::uint32_t, 32> owner {};
    interpolation.fill(unassigned);
    const auto& metadata = program.Metadata();
    for (const auto& block : program.Blocks()) {
        for (const IrValue* inst : block->Instructions()) {
            if (inst->Opcode() != IrOpcode::GetAttribute && inst->Opcode() != IrOpcode::GetInterpolationParameter) continue;
            const auto input = inst->Argument(0)->Resolve()->ImmediateU32();
            if (input >= pixel.inputNum || pixel.InputIsDefault(input)) continue;
            const auto mode = inputIsFlat(program, pixel, input) ? 2u : pixel.InputIsLinear(input, metadata.pixelLinearInputs, metadata.pixelPerspectiveInputs) ? 1u : 0u;
            auto& slot = interpolation[pixel.InputSlot(input)];
            if (slot != unassigned && slot != mode) {
                static constexpr const char* modes[] = {"perspective", "linear", "flat"};
                reason = "pixel inputs " + std::to_string(owner[pixel.InputSlot(input)]) + " (" + modes[slot] + ") and " + std::to_string(input) + " (" + modes[mode] + ") read the same vertex parameter " + std::to_string(pixel.InputSlot(input));
                return true;
            }
            slot = mode;
            owner[pixel.InputSlot(input)] = input;
        }
    }
    return false;
}

}

bool HostInterpolationChecker::Lower(IrProgram& program, const ShaderPixelInputInfo& pixel, std::string& reason) const {
    std::vector<IrValue*> selects;
    for (const auto& block : program.Blocks()) {
        for (const IrValue* inst : block->Instructions()) {
            if (!exact(*inst, program, pixel, selects, reason)) return false;
        }
    }
    if (sharesSlots(program, pixel, reason)) return false;
    for (auto* select : selects) select->ReplaceAllUsesWith(select->Argument(2));
    for (const auto& block : program.Blocks()) {
        auto& instructions = block->Instructions();
        for (auto it = instructions.begin(); it != instructions.end();) {
            IrValue* inst = *it;
            if (inst->Opcode() != IrOpcode::InterpolateHostP2) {
                ++it;
                continue;
            }
            inst->Invalidate();
            inst->SetParent(nullptr);
            it = instructions.erase(it);
        }
    }
    DeadCodeEliminator{}.Eliminate(program);
    if (MaskedSelectEliminator{}.Eliminate(program).removedSelects != 0u) DeadCodeEliminator{}.Eliminate(program);
    for (const auto& block : program.Blocks()) {
        for (const IrValue* inst : block->Instructions()) {
            if (isBarycentric(*inst) || inst->Opcode() == IrOpcode::InterpolateHostP1) {
                reason = describe(program, *inst) + " is read by " + readers(program, *inst);
                return false;
            }
        }
    }
    return true;
}

}
