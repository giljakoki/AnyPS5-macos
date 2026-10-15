#include "Optimization/HostInterpolationChecker.hpp"
#include "Recompiler.hpp"
#include "IntermediateRepresentation/IrBlock.hpp"
#include "IntermediateRepresentation/IrMetadata/StageIO.hpp"
#include "Optimization/DeadCodeEliminator.hpp"
#include "Optimization/MaskedSelectEliminator.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <optional>
#include <string_view>
#include <cstdio>
#include <string>
#include <utility>
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

struct LaneWords {
    const IrValue* low = nullptr;
    const IrValue* high = nullptr;
    bool operator==(const LaneWords&) const = default;
};

bool isImmediateValue(const IrValue* value, std::uint32_t immediate) {
    value = value->Resolve();
    return value->HasImmediate() && value->ImmediateU32() == immediate;
}

std::optional<LaneWords> ownBitWords(const IrValue* value) {
    value = value->Resolve();
    if (value->Opcode() != IrOpcode::INotEqual32 || !isImmediateValue(value->Argument(1), 0u)) return std::nullopt;
    const auto* bit = value->Argument(0)->Resolve();
    if (bit->Opcode() != IrOpcode::BitwiseAnd32 || !isImmediateValue(bit->Argument(1), 1u)) return std::nullopt;
    const auto* shift = bit->Argument(0)->Resolve();
    if (shift->Opcode() != IrOpcode::ShiftRightLogical32) return std::nullopt;
    const auto* laneBits = shift->Argument(1)->Resolve();
    if (laneBits->Opcode() != IrOpcode::BitwiseAnd32 || laneBits->Argument(0)->Resolve()->Opcode() != IrOpcode::LaneId || !isImmediateValue(laneBits->Argument(1), 31u)) return std::nullopt;
    const auto* half = shift->Argument(0)->Resolve();
    if (half->Opcode() != IrOpcode::SelectU32) return std::nullopt;
    const auto* low = half->Argument(0)->Resolve();
    if (low->Opcode() != IrOpcode::ULessThan32 || low->Argument(0)->Resolve()->Opcode() != IrOpcode::LaneId || !isImmediateValue(low->Argument(1), 32u)) return std::nullopt;
    return LaneWords{half->Argument(1)->Resolve(), half->Argument(2)->Resolve()};
}

bool implies(const IrValue* condition, const IrValue* implied, std::uint32_t depth = 0) {
    condition = condition->Resolve();
    implied = implied->Resolve();
    if (condition == implied) return true;
    if (depth > 8 || condition->Opcode() != IrOpcode::LogicalAnd) return false;
    return implies(condition->Argument(0), implied, depth + 1) || implies(condition->Argument(1), implied, depth + 1);
}

const IrValue* ballotOf(const IrValue* word, std::uint32_t half) {
    word = word->Resolve();
    if (word->Opcode() != IrOpcode::CompositeExtractU32x4 || !isImmediateValue(word->Argument(1), half)) return nullptr;
    const auto* ballot = word->Argument(0)->Resolve();
    return ballot->Opcode() == IrOpcode::Ballot ? ballot : nullptr;
}

bool alwaysTrue(const IrValue* value) {
    value = value->Resolve();
    return value->Type() == IrType::U1 && value->HasImmediate() && value->ImmediateU32() != 0u;
}

bool wordWithin(const IrValue* exec, const IrValue* condition, std::uint32_t half, std::uint32_t depth = 0) {
    exec = exec->Resolve();
    condition = condition->Resolve();
    if (exec == condition) return true;
    if (depth > 12) return false;
    if (exec->Opcode() == IrOpcode::BitwiseAnd32 && (wordWithin(exec->Argument(0), condition, half, depth + 1) || wordWithin(exec->Argument(1), condition, half, depth + 1))) return true;
    if (condition->Opcode() == IrOpcode::BitwiseAnd32) return wordWithin(exec, condition->Argument(0), half, depth + 1) && wordWithin(exec, condition->Argument(1), half, depth + 1);
    if (condition->Opcode() == IrOpcode::CompositeExtractU64 && isImmediateValue(condition->Argument(1), half)) {
        const auto* wqm = condition->Argument(0)->Resolve();
        if (wqm->Opcode() == IrOpcode::WqmU64) {
            const auto* pair = wqm->Argument(0)->Resolve();
            if (pair->Opcode() == IrOpcode::CompositeConstructU64 && wordWithin(exec, pair->Argument(half), half, depth + 1)) return true;
        }
    }
    const auto* conditionBallot = ballotOf(condition, half);
    if (conditionBallot != nullptr && alwaysTrue(conditionBallot->Argument(0))) return true;
    const auto* execBallot = ballotOf(exec, half);
    if (execBallot == nullptr || conditionBallot == nullptr) return false;
    return implies(execBallot->Argument(0), conditionBallot->Argument(0));
}

bool coversLanes(const LaneWords& condition, const LaneWords& exec) {
    return wordWithin(exec.low, condition.low, 0u) && wordWithin(exec.high, condition.high, 1u);
}

const IrValue* incomingFor(const IrValue& phi, const IrBlock* block) {
    for (std::size_t index = 0; index < phi.PhiBlockCount(); ++index) {
        if (phi.PhiBlock(index) == block) return phi.Argument(index)->Resolve();
    }
    return nullptr;
}

bool execWithinCondition(const IrValue* condition, const IrValue* exec) {
    const auto execWords = ownBitWords(exec);
    if (!execWords) return false;
    condition = condition->Resolve();
    if (const auto words = ownBitWords(condition)) return coversLanes(*words, *execWords);
    if (!condition->IsPhi() || !execWords->low->IsPhi() || !execWords->high->IsPhi() || condition->Parent() != execWords->low->Parent() || condition->Parent() != execWords->high->Parent()) return false;
    for (std::size_t index = 0; index < condition->PhiBlockCount(); ++index) {
        const auto* block = condition->PhiBlock(index);
        const auto* low = incomingFor(*execWords->low, block);
        const auto* high = incomingFor(*execWords->high, block);
        const auto words = ownBitWords(condition->Argument(index));
        if (low == nullptr || high == nullptr || !words || !coversLanes(*words, LaneWords{low, high})) return false;
    }
    return true;
}

bool carriesBarycentric(const IrValue* value, std::vector<const IrValue*>& visited) {
    value = lookThrough(value);
    if (isBarycentric(*value)) return true;
    if (!value->IsPhi() || std::find(visited.begin(), visited.end(), value) != visited.end()) return false;
    visited.push_back(value);
    for (std::size_t index = 0; index < value->ArgumentCount(); ++index) {
        if (value->Argument(index) != nullptr && carriesBarycentric(value->Argument(index), visited)) return true;
    }
    return false;
}

bool takeExportedSelects(IrProgram& program) {
    std::vector<IrValue*> proven;
    for (const auto& block : program.Blocks()) {
        for (IrValue* inst : block->Instructions()) {
            if (inst->Opcode() != IrOpcode::SelectU32) continue;
            std::vector<const IrValue*> visited;
            if (!carriesBarycentric(inst->Argument(2), visited)) continue;
            const bool exported = usesAll(*inst, [&](const IrValue& user, std::size_t) {
                if (user.Opcode() != IrOpcode::CompositeConstructU32x4) return false;
                return usesAll(user, [&](const IrValue& store, std::size_t operand) {
                    return store.Opcode() == IrOpcode::SetAttribute && operand == 0u && execWithinCondition(inst->Argument(0), store.Argument(1));
                });
            });
            if (exported && inst->HasUses()) proven.push_back(inst);
        }
    }
    for (auto* select : proven) select->ReplaceAllUsesWith(select->Argument(1));
    return !proven.empty();
}

bool laneLocalValue(const IrValue& value) {
    if (IrOpcodeHasSideEffects(value.Opcode()) || value.Type() == IrType::Void) return false;
    switch (value.Opcode()) {
        case IrOpcode::Ballot:
        case IrOpcode::ReadLane:
        case IrOpcode::ReadFirstLane:
        case IrOpcode::WriteLane:
        case IrOpcode::DppMoveU32:
        case IrOpcode::DppUpdateU32:
        case IrOpcode::Permlane16U32:
        case IrOpcode::SwizzleU32:
        case IrOpcode::PermuteU32:
        case IrOpcode::BpermuteU32:
        case IrOpcode::MakeImageAddress:
            return false;
        default:
            break;
    }
    return BufferAccessOf(value.Opcode()) == BufferAccess::None && SharedAccessOf(value.Opcode()) == SharedAccess::None && AddressOpcodeInfoOf(value.Opcode()).access == AddressAccess::None && ImageOpcodeInfoOf(value.Opcode()).access == ImageAccess::None;
}

bool wordWithinPredicate(const IrValue* word, const IrValue* predicate, std::uint32_t half, std::uint32_t depth = 0) {
    word = word->Resolve();
    if (depth > 12) return false;
    if (word->Opcode() == IrOpcode::BitwiseAnd32) return wordWithinPredicate(word->Argument(0), predicate, half, depth + 1) || wordWithinPredicate(word->Argument(1), predicate, half, depth + 1);
    const auto* ballot = ballotOf(word, half);
    return ballot != nullptr && implies(ballot->Argument(0), predicate);
}

bool guards(const IrValue* guard, const IrValue* keep) {
    if (implies(guard, keep)) return true;
    const auto guardWords = ownBitWords(guard);
    const auto keepWords = ownBitWords(keep);
    if (guardWords && !keepWords) return wordWithinPredicate(guardWords->low, keep, 0u) && wordWithinPredicate(guardWords->high, keep, 1u);
    return guardWords && keepWords && coversLanes(*keepWords, *guardWords);
}

bool keepsOnlyIdleLanes(const IrValue* keep) {
    const auto words = ownBitWords(keep);
    if (!words) return false;
    const auto* low = ballotOf(words->low, 0u);
    const auto* high = ballotOf(words->high, 1u);
    return low != nullptr && high != nullptr && low->Parent() == high->Parent() && alwaysTrue(low->Argument(0)) && alwaysTrue(high->Argument(0));
}

bool maskedOnly(const IrValue& value, const IrValue* keep, std::vector<const IrValue*>& visited, bool otherLanes = false) {
    for (const auto& use : value.OperandUses()) {
        const auto& user = *use.user;
        if (std::find(visited.begin(), visited.end(), &user) != visited.end()) continue;
        if (isCopy(user) || user.IsPhi()) {
            visited.push_back(&user);
            if (!maskedOnly(user, keep, visited, otherLanes)) return false;
            continue;
        }
        if (user.Opcode() == IrOpcode::SelectU32 && use.operand == 2u) {
            visited.push_back(&user);
            if (!maskedOnly(user, keep != nullptr ? keep : user.Argument(0)->Resolve(), visited, otherLanes)) return false;
            continue;
        }
        if (keep == nullptr) return false;
        if (!otherLanes) {
            if (user.Opcode() == IrOpcode::LogicalAnd && guards(user.Argument(use.operand == 0u ? 1u : 0u), keep)) continue;
            if (user.Opcode() == IrOpcode::SelectU32 && use.operand == 1u && user.Argument(2)->Resolve() != &value && guards(user.Argument(0), keep)) continue;
            if (user.Opcode() == IrOpcode::SetAttribute && use.operand == 0u && guards(user.Argument(1), keep)) continue;
        }
        if (otherLanes && user.Opcode() == IrOpcode::SetAttribute && use.operand == 0u && keepsOnlyIdleLanes(keep)) continue;
        if (user.Opcode() == IrOpcode::MakeImageAddress) {
            const bool sampled = usesAll(user, [](const IrValue& sample, std::size_t) { return ImageOpcodeInfoOf(sample.Opcode()).access == ImageAccess::Read; });
            if (sampled) continue;
        }
        if (user.Opcode() == IrOpcode::DppMoveU32 || user.Opcode() == IrOpcode::DppUpdateU32) {
            visited.push_back(&user);
            if (!maskedOnly(user, keep, visited, true)) return false;
            continue;
        }
        if (laneLocalValue(user)) {
            visited.push_back(&user);
            if (!maskedOnly(user, keep, visited, otherLanes)) return false;
            continue;
        }
        return false;
    }
    return true;
}

bool zeroMaskedBarycentrics(IrProgram& program) {
    std::vector<IrValue*> zeroed;
    for (const auto& block : program.Blocks()) {
        for (IrValue* inst : block->Instructions()) {
            std::vector<const IrValue*> visited;
            if (isBarycentric(*inst) && inst->HasUses() && maskedOnly(*inst, nullptr, visited)) zeroed.push_back(inst);
        }
    }
    for (auto* barycentric : zeroed) {
        auto& zero = program.CreateValue(IrOpcode::Void, barycentric->Type());
        zero.SetImmediateU32(0u);
        barycentric->ReplaceAllUsesWith(&zero);
    }
    return !zeroed.empty();
}

bool unhookUnobservedPhiReads(IrProgram& program) {
    std::vector<std::pair<IrValue*, std::size_t>> unobserved;
    for (const auto& block : program.Blocks()) {
        for (IrValue* inst : block->Instructions()) {
            if (!inst->IsPhi() || (inst->Type() != IrType::U32 && inst->Type() != IrType::F32)) continue;
            for (std::size_t index = 0; index < inst->ArgumentCount(); ++index) {
                const IrValue* incoming = inst->Argument(index);
                if (incoming == nullptr || !isBarycentric(*lookThrough(incoming))) continue;
                if (PhiIncomingUnobserved(program, *inst, index)) unobserved.emplace_back(inst, index);
            }
        }
    }
    for (const auto& [phi, index] : unobserved) {
        auto& zero = program.CreateValue(IrOpcode::Void, phi->Type());
        zero.SetImmediateU32(0u);
        phi->ReplaceArgument(index, &zero);
    }
    return !unobserved.empty();
}

// A raw I/J VGPR the shader overwrites under EXEC in an if/else that a wave-mask s_cbranch_execz
// may skip: v = phi(skipped: v, ran: select(c, x, v)). On the skipped edge c is false in every
// lane, so the phi equals select(c, phi(skipped: any, ran: x), v), which the complementary-select
// rewrite below can then see through.
bool carriesBarycentricDeep(const IrValue* value, std::vector<const IrValue*>& visited, std::uint32_t depth = 0) {
    value = lookThrough(value);
    if (isBarycentric(*value)) return true;
    if (depth > 16 || std::find(visited.begin(), visited.end(), value) != visited.end()) return false;
    visited.push_back(value);
    if (value->Opcode() == IrOpcode::SelectU32) return carriesBarycentricDeep(value->Argument(1), visited, depth + 1) || carriesBarycentricDeep(value->Argument(2), visited, depth + 1);
    if (!value->IsPhi()) return false;
    for (std::size_t index = 0; index < value->ArgumentCount(); ++index) {
        if (value->Argument(index) != nullptr && carriesBarycentricDeep(value->Argument(index), visited, depth + 1)) return true;
    }
    return false;
}

const IrValue* negated(const IrValue* value) {
    value = value->Resolve();
    return value->Opcode() == IrOpcode::LogicalNot ? value->Argument(0)->Resolve() : nullptr;
}

const BlockInfo* infoOf(const IrProgram& program, const IrBlock* block) {
    const auto& blocks = program.Blocks();
    const auto& info = program.Metadata().blockInfo;
    for (std::size_t i = 0; i < blocks.size() && i < info.size(); ++i) {
        if (blocks[i].get() == block) return &info[i];
    }
    return nullptr;
}

// Whether `condition` is false in every lane that reaches `to` straight from `from`'s wave-mask branch.
bool falseInEveryLane(const IrProgram& program, const IrBlock* from, const IrBlock* to, const IrValue* condition) {
    const auto* source = infoOf(program, from);
    const auto* target = infoOf(program, to);
    if (source == nullptr || target == nullptr || source->condition == nullptr || source->laneLocalBranch) return false;
    const auto& term = source->terminator;
    if (term.kind != TerminatorKind::ConditionalBranch || term.trueBlock == term.falseBlock) return false;
    condition = condition->Resolve();
    switch (term.condition) {
        case BranchCondition::ExecZero:
        case BranchCondition::VccZero:
            return term.trueBlock == target->id && negated(source->condition) == condition;
        case BranchCondition::ExecNonZero:
        case BranchCondition::VccNonZero:
            return term.falseBlock == target->id && source->condition->Resolve() == condition;
        default:
            return false;
    }
}

bool splitSkippedSelectPhis(IrProgram& program) {
    bool changed = false;
    for (const auto& block : program.Blocks()) {
        std::vector<IrValue*> phis;
        IrValue* firstOther = nullptr;
        for (IrValue* inst : block->Instructions()) {
            if (!inst->IsPhi()) {
                firstOther = inst;
                break;
            }
            phis.push_back(inst);
        }
        for (IrValue* phi : phis) {
            if (phi->Type() != IrType::U32 || phi->ArgumentCount() != 2u || phi->PhiBlockCount() != 2u || !phi->HasUses()) continue;
            for (std::size_t skipped = 0; skipped < 2u; ++skipped) {
                IrValue* kept = phi->Argument(skipped)->Resolve();
                IrValue* select = phi->Argument(1u - skipped)->Resolve();
                IrBlock* from = phi->PhiBlock(skipped);
                IrBlock* ran = phi->PhiBlock(1u - skipped);
                if (select->Opcode() != IrOpcode::SelectU32 || select->Argument(2)->Resolve() != kept) continue;
                if (ran->Predecessors().size() != 1u || ran->Predecessors().front() != from) continue;
                if (!falseInEveryLane(program, from, block.get(), select->Argument(0))) continue;
                std::vector<const IrValue*> visited;
                if (!carriesBarycentricDeep(kept, visited)) continue;
                auto& zero = program.CreateValue(IrOpcode::Void, IrType::U32);
                zero.SetImmediateU32(0u);
                auto& taken = program.CreateValue(IrOpcode::Phi, IrType::U32);
                taken.AddPhiOperand(from, &zero);
                taken.AddPhiOperand(ran, select->Argument(1));
                block->InsertInstructionBefore(phi, &taken);
                auto& merged = program.CreateValue(IrOpcode::SelectU32, IrType::U32);
                merged.AddArgument(select->Argument(0));
                merged.AddArgument(&taken);
                merged.AddArgument(kept);
                phi->ReplaceAllUsesWith(&merged);
                if (firstOther != nullptr) {
                    block->InsertInstructionBefore(firstOther, &merged);
                } else {
                    block->AppendInstruction(&merged);
                }
                changed = true;
                break;
            }
        }
    }
    return changed;
}

// LogicalAnd(own bit of ballot(true), x) is x for every lane that executed the ballot.
const IrValue* withoutActiveLane(const IrValue* value) {
    value = value->Resolve();
    while (value->Opcode() == IrOpcode::LogicalAnd) {
        if (keepsOnlyIdleLanes(value->Argument(0))) {
            value = value->Argument(1)->Resolve();
        } else if (keepsOnlyIdleLanes(value->Argument(1))) {
            value = value->Argument(0)->Resolve();
        } else {
            break;
        }
    }
    return value;
}

bool complementary(const IrValue* left, const IrValue* right) {
    left = withoutActiveLane(left);
    right = withoutActiveLane(right);
    return negated(left) == right || negated(right) == left;
}

// select(p, u, select(!p, v, w)) is select(p, u, v); select(p, select(!p, v, w), u) is select(p, w, u).
bool foldComplementarySelects(IrProgram& program) {
    bool changed = false;
    for (const auto& block : program.Blocks()) {
        for (IrValue* inst : block->Instructions()) {
            if (inst->Opcode() != IrOpcode::SelectU32) continue;
            for (std::size_t operand = 1; operand <= 2u; ++operand) {
                const IrValue* inner = inst->Argument(operand)->Resolve();
                if (inner->Opcode() != IrOpcode::SelectU32 || !complementary(inst->Argument(0), inner->Argument(0))) continue;
                std::vector<const IrValue*> visited;
                if (!carriesBarycentricDeep(inner, visited)) continue;
                inst->ReplaceArgument(operand, inner->Argument(operand == 2u ? 1u : 2u));
                changed = true;
            }
        }
    }
    return changed;
}

bool resolveSkippedBarycentricSelects(IrProgram& program) {
    bool changed = false;
    for (int round = 0; round < 8; ++round) {
        const bool split = splitSkippedSelectPhis(program);
        const bool folded = foldComplementarySelects(program);
        if (!split && !folded) break;
        changed = true;
    }
    return changed;
}

}

bool BarycentricMaskedZero() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_BARYCENTRIC_MASKED_ZERO");
        return value != nullptr && std::string_view(value) == "1";
    }();
    return enabled;
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
    if (unhookUnobservedPhiReads(program)) DeadCodeEliminator{}.Eliminate(program);
    if (takeExportedSelects(program)) DeadCodeEliminator{}.Eliminate(program);
    if (resolveSkippedBarycentricSelects(program)) DeadCodeEliminator{}.Eliminate(program);
    if (BarycentricMaskedZero() && zeroMaskedBarycentrics(program)) DeadCodeEliminator{}.Eliminate(program);
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
