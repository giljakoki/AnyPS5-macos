#include "Optimization/LaneLocalLowering.hpp"
#include "Optimization/ConstantFolder.hpp"
#include "Optimization/DeadCodeEliminator.hpp"
#include "Optimization/WaveUniformity.hpp"
#include "IntermediateRepresentation/IrBuilder.hpp"
#include <algorithm>
#include <bit>
#include <cstddef>
#include <map>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ShaderRecompiler {
namespace {

constexpr std::uint32_t ImplicationDepthLimit = 64;

bool IsImmediateU32(const IrValue* value, std::uint32_t constant) {
    value = value->Resolve();
    return value->HasImmediate() && value->Type() == IrType::U32 && value->ImmediateU32() == constant;
}

bool IsBoolImmediate(const IrValue* value, bool expected) {
    value = value->Resolve();
    return value->HasImmediate() && value->Type() == IrType::U1 && value->ImmediateBool() == expected;
}

IrValue* OtherArgument(const IrValue* value, std::uint32_t constant) {
    if (value->ArgumentCount() != 2u) return nullptr;
    if (IsImmediateU32(value->Argument(1), constant)) return value->Argument(0)->Resolve();
    if (IsImmediateU32(value->Argument(0), constant)) return value->Argument(1)->Resolve();
    return nullptr;
}

bool IsLaneId(const IrValue* value) {
    return value != nullptr && value->Resolve()->Opcode() == IrOpcode::LaneId;
}

bool IsSelect(IrOpcode opcode) {
    return opcode == IrOpcode::SelectU1 || opcode == IrOpcode::SelectU32 || opcode == IrOpcode::SelectF32;
}

using Lanes = std::vector<std::uint32_t>;

struct PairKey {
    const IrValue* low;
    const IrValue* high;
    bool operator==(const PairKey& other) const = default;
};

struct PairKeyHash {
    std::size_t operator()(const PairKey& key) const {
        return std::hash<const IrValue*>{}(key.low) * 31u ^ std::hash<const IrValue*>{}(key.high);
    }
};

class Implications {
public:
    Implications(const IrValue* target, std::size_t& work) : target(target->Resolve()), work(work) {}

    bool Implies(const IrValue* predicate, std::uint32_t depth = 0) {
        predicate = predicate->Resolve();
        if (predicate == target) return true;
        if (predicate->Opcode() == IrOpcode::LogicalNot && target->Opcode() == IrOpcode::LogicalNot && predicate->Argument(0)->Resolve() == target->Argument(0)->Resolve()) return true;
        if (target->HasImmediate()) return target->Type() == IrType::U1 && target->ImmediateBool();
        if (predicate->HasImmediate()) return predicate->Type() == IrType::U1 && !predicate->ImmediateBool();
        if (const auto known = results.find(predicate); known != results.end()) return known->second != Proof::NotImplied;
        if (depth > ImplicationDepthLimit || work == 0) return false;
        --work;
        results.emplace(predicate, Proof::Assumed);
        const auto mark = log.size();
        log.push_back(predicate);
        bool result = false;
        switch (predicate->Opcode()) {
        case IrOpcode::LogicalAnd:
            result = Implies(predicate->Argument(0), depth + 1u) || Implies(predicate->Argument(1), depth + 1u);
            break;
        case IrOpcode::LogicalOr:
            result = Implies(predicate->Argument(0), depth + 1u) && Implies(predicate->Argument(1), depth + 1u);
            break;
        case IrOpcode::SelectU1:
            result = Implies(predicate->Argument(1), depth + 1u) && Implies(predicate->Argument(2), depth + 1u);
            break;
        case IrOpcode::Phi:
            result = predicate->ArgumentCount() != 0u;
            for (std::size_t index = 0; result && index < predicate->ArgumentCount(); ++index) {
                result = Implies(predicate->Argument(index), depth + 1u);
            }
            break;
        default:
            break;
        }
        if (!result && target->Opcode() == IrOpcode::LogicalAnd) {
            Implications left(target->Argument(0), work);
            Implications right(target->Argument(1), work);
            result = left.Implies(predicate, depth + 1u) && right.Implies(predicate, depth + 1u);
        }
        if (!result && target->Opcode() == IrOpcode::LogicalOr) {
            Implications left(target->Argument(0), work);
            Implications right(target->Argument(1), work);
            result = left.Implies(predicate, depth + 1u) || right.Implies(predicate, depth + 1u);
        }
        if (!result) {
            for (std::size_t index = mark; index < log.size(); ++index) results.erase(log[index]);
            log.resize(mark);
            results[predicate] = Proof::NotImplied;
            return false;
        }
        results[predicate] = Proof::Implied;
        return true;
    }

    const IrValue* Target() const {
        return target;
    }

private:
    enum class Proof : std::uint8_t { Assumed, Implied, NotImplied };

    const IrValue* target;
    std::size_t& work;
    std::unordered_map<const IrValue*, Proof> results;
    std::vector<const IrValue*> log;
};

class Lowering {
public:
    Lowering(IrProgram& program, std::uint32_t lanes) : program(program), lanes(lanes), builder(program), stage(program.Resources().stage) {}

    std::vector<SubgroupUse> Run();

private:
    void indexBlocks();
    void cleanup();

    std::optional<Lanes> laneValues(const IrValue* value, std::unordered_map<const IrValue*, std::optional<Lanes>>& table, std::uint32_t depth);
    void foldLaneIndexValues();

    struct ThreadBit {
        IrValue* low;
        IrValue* high;
    };
    std::optional<ThreadBit> matchThreadBit(const IrValue* value) const;
    IrValue* laneBit(IrValue* low, IrValue* high);
    IrValue* computeLaneBit(IrValue* low, IrValue* high);
    IrValue& emitAfter(IrValue* low, IrValue* high, IrOpcode opcode, IrType type, std::initializer_list<IrValue*> arguments);
    IrValue* later(IrValue* first, IrValue* second) const;
    void rewriteThreadBits();

    void rewriteUniformLaneReads();

    struct Construct {
        std::size_t header = 0;
        std::size_t merge = 0;
        std::vector<std::size_t> arms;
        std::unordered_set<std::size_t> inside;
    };
    std::optional<std::vector<std::size_t>> armBlocks(std::size_t entry, std::size_t merge, std::size_t header, std::string& reason) const;
    bool transparent(const Construct& construct, Implications& implies, std::string& reason) const;
    std::optional<IrValue*> peel(IrValue* value, const Construct& construct, Implications& implies, std::unordered_map<const IrValue*, IrValue*>& memo, std::unordered_set<const IrValue*>& peeling) const;
    bool sameBase(IrValue* first, IrValue* second, Implications& implies) const;
    bool mergePhisTransparent(const Construct& construct, Implications& implies, std::string& reason) const;
    void lowerWaveMaskBranches();

    std::vector<SubgroupUse> describe() const;

    IrProgram& program;
    std::uint32_t lanes;
    IrBuilder builder;
    IrShaderStage stage;
    std::unordered_map<const IrBlock*, std::size_t> blockIndex;
    std::unordered_map<std::uint32_t, std::size_t> blockById;
    std::unordered_map<PairKey, IrValue*, PairKeyHash> bits;
    std::vector<PairKey> bitLog;
    std::unordered_set<const IrValue*> uniform;
    std::map<std::size_t, std::string> branchReasons;
};

void Lowering::indexBlocks() {
    blockIndex.clear();
    blockById.clear();
    const auto& order = program.BlockOrder();
    const auto& info = program.Metadata().blockInfo;
    if (order.size() != info.size()) throw std::runtime_error("lane-local lowering: block metadata is inconsistent");
    for (std::size_t index = 0; index < order.size(); ++index) {
        blockIndex.emplace(order[index], index);
        blockById.emplace(info[index].id, index);
    }
}

void Lowering::cleanup() {
    constexpr ConstantFolder folder;
    constexpr DeadCodeEliminator eliminator;
    folder.Fold(program);
    ResolveControlFlowIdentities(program);
    eliminator.RemoveIdentities(program);
    eliminator.Eliminate(program);
}

std::optional<Lanes> Lowering::laneValues(const IrValue* value, std::unordered_map<const IrValue*, std::optional<Lanes>>& table, std::uint32_t depth) {
    value = value->Resolve();
    if (value->HasImmediate()) {
        if (value->Type() == IrType::U32) return Lanes(lanes, value->ImmediateU32());
        if (value->Type() == IrType::U1) return Lanes(lanes, value->ImmediateBool() ? 1u : 0u);
        return std::nullopt;
    }
    if (const auto known = table.find(value); known != table.end()) return known->second;
    if (depth > 64u) return std::nullopt;
    const auto result = [&]() -> std::optional<Lanes> {
        if (value->Type() != IrType::U32 && value->Type() != IrType::U1) return std::nullopt;
        const auto argument = [&](std::size_t index) -> std::optional<Lanes> {
            return index < value->ArgumentCount() ? laneValues(value->Argument(index), table, depth + 1u) : std::nullopt;
        };
        const auto binary = [&](auto function) -> std::optional<Lanes> {
            const auto a = argument(0), b = argument(1);
            if (!a || !b) return std::nullopt;
            Lanes result(lanes);
            for (std::uint32_t lane = 0; lane < lanes; ++lane) {
                const auto folded = function((*a)[lane], (*b)[lane]);
                if (!folded) return std::nullopt;
                result[lane] = *folded;
            }
            return result;
        };
        const auto unary = [&](auto function) -> std::optional<Lanes> {
            const auto a = argument(0);
            if (!a) return std::nullopt;
            Lanes result(lanes);
            for (std::uint32_t lane = 0; lane < lanes; ++lane) result[lane] = function((*a)[lane]);
            return result;
        };
        using Fold = std::optional<std::uint32_t>;
        const auto signedOf = [](std::uint32_t word) { return std::bit_cast<std::int32_t>(word); };
        switch (value->Opcode()) {
        case IrOpcode::LaneId: {
            Lanes result(lanes);
            for (std::uint32_t lane = 0; lane < lanes; ++lane) result[lane] = lane;
            return result;
        }
        case IrOpcode::IAdd32: return binary([](std::uint32_t a, std::uint32_t b) -> Fold { return a + b; });
        case IrOpcode::ISub32: return binary([](std::uint32_t a, std::uint32_t b) -> Fold { return a - b; });
        case IrOpcode::IMul32: return binary([](std::uint32_t a, std::uint32_t b) -> Fold { return a * b; });
        case IrOpcode::BitwiseAnd32: return binary([](std::uint32_t a, std::uint32_t b) -> Fold { return a & b; });
        case IrOpcode::BitwiseOr32: return binary([](std::uint32_t a, std::uint32_t b) -> Fold { return a | b; });
        case IrOpcode::BitwiseXor32: return binary([](std::uint32_t a, std::uint32_t b) -> Fold { return a ^ b; });
        case IrOpcode::ShiftLeftLogical32: return binary([](std::uint32_t a, std::uint32_t b) -> Fold { if (b >= 32u) return std::nullopt; return a << b; });
        case IrOpcode::ShiftRightLogical32: return binary([](std::uint32_t a, std::uint32_t b) -> Fold { if (b >= 32u) return std::nullopt; return a >> b; });
        case IrOpcode::ShiftRightArithmetic32: return binary([&](std::uint32_t a, std::uint32_t b) -> Fold { if (b >= 32u) return std::nullopt; return static_cast<std::uint32_t>(signedOf(a) >> b); });
        case IrOpcode::UMin32: return binary([](std::uint32_t a, std::uint32_t b) -> Fold { return std::min(a, b); });
        case IrOpcode::UMax32: return binary([](std::uint32_t a, std::uint32_t b) -> Fold { return std::max(a, b); });
        case IrOpcode::SMin32: return binary([&](std::uint32_t a, std::uint32_t b) -> Fold { return signedOf(a) < signedOf(b) ? a : b; });
        case IrOpcode::SMax32: return binary([&](std::uint32_t a, std::uint32_t b) -> Fold { return signedOf(a) > signedOf(b) ? a : b; });
        case IrOpcode::ULessThan32: return binary([](std::uint32_t a, std::uint32_t b) -> Fold { return a < b ? 1u : 0u; });
        case IrOpcode::ULessThanEqual32: return binary([](std::uint32_t a, std::uint32_t b) -> Fold { return a <= b ? 1u : 0u; });
        case IrOpcode::UGreaterThan32: return binary([](std::uint32_t a, std::uint32_t b) -> Fold { return a > b ? 1u : 0u; });
        case IrOpcode::UGreaterThanEqual32: return binary([](std::uint32_t a, std::uint32_t b) -> Fold { return a >= b ? 1u : 0u; });
        case IrOpcode::SLessThan32: return binary([&](std::uint32_t a, std::uint32_t b) -> Fold { return signedOf(a) < signedOf(b) ? 1u : 0u; });
        case IrOpcode::SLessThanEqual32: return binary([&](std::uint32_t a, std::uint32_t b) -> Fold { return signedOf(a) <= signedOf(b) ? 1u : 0u; });
        case IrOpcode::SGreaterThan32: return binary([&](std::uint32_t a, std::uint32_t b) -> Fold { return signedOf(a) > signedOf(b) ? 1u : 0u; });
        case IrOpcode::SGreaterThanEqual32: return binary([&](std::uint32_t a, std::uint32_t b) -> Fold { return signedOf(a) >= signedOf(b) ? 1u : 0u; });
        case IrOpcode::IEqual32: return binary([](std::uint32_t a, std::uint32_t b) -> Fold { return a == b ? 1u : 0u; });
        case IrOpcode::INotEqual32: return binary([](std::uint32_t a, std::uint32_t b) -> Fold { return a != b ? 1u : 0u; });
        case IrOpcode::LogicalAnd: return binary([](std::uint32_t a, std::uint32_t b) -> Fold { return (a != 0u && b != 0u) ? 1u : 0u; });
        case IrOpcode::LogicalOr: return binary([](std::uint32_t a, std::uint32_t b) -> Fold { return (a != 0u || b != 0u) ? 1u : 0u; });
        case IrOpcode::LogicalXor: return binary([](std::uint32_t a, std::uint32_t b) -> Fold { return ((a != 0u) != (b != 0u)) ? 1u : 0u; });
        case IrOpcode::LogicalNot: return unary([](std::uint32_t a) { return a != 0u ? 0u : 1u; });
        case IrOpcode::BitwiseNot32: return unary([](std::uint32_t a) { return ~a; });
        case IrOpcode::BitCount32: return unary([](std::uint32_t a) { return static_cast<std::uint32_t>(std::popcount(a)); });
        case IrOpcode::BitReverse32: return unary([](std::uint32_t a) {
            std::uint32_t reversed = 0;
            for (std::uint32_t bit = 0; bit < 32u; ++bit) reversed |= ((a >> bit) & 1u) << (31u - bit);
            return reversed;
        });
        case IrOpcode::FindILsb32: return unary([](std::uint32_t a) { return a == 0u ? 0xffffffffu : static_cast<std::uint32_t>(std::countr_zero(a)); });
        case IrOpcode::FindUMsb32: return unary([](std::uint32_t a) { return a == 0u ? 0xffffffffu : 31u - static_cast<std::uint32_t>(std::countl_zero(a)); });
        case IrOpcode::SelectU32:
        case IrOpcode::SelectU1: {
            const auto condition = argument(0), a = argument(1), b = argument(2);
            if (!condition || !a || !b) return std::nullopt;
            Lanes result(lanes);
            for (std::uint32_t lane = 0; lane < lanes; ++lane) result[lane] = (*condition)[lane] != 0u ? (*a)[lane] : (*b)[lane];
            return result;
        }
        case IrOpcode::BitFieldUExtract:
        case IrOpcode::BitFieldSExtract: {
            const auto source = argument(0), offset = argument(1), count = argument(2);
            if (!source || !offset || !count) return std::nullopt;
            Lanes result(lanes);
            for (std::uint32_t lane = 0; lane < lanes; ++lane) {
                const auto o = (*offset)[lane], c = (*count)[lane];
                if (o > 32u || c > 32u - o) return std::nullopt;
                if (c == 0u) {
                    result[lane] = 0u;
                    continue;
                }
                const std::uint32_t field = c == 32u ? (*source)[lane] : ((*source)[lane] >> o) & ((1u << c) - 1u);
                if (value->Opcode() == IrOpcode::BitFieldSExtract && c < 32u && (field >> (c - 1u)) != 0u) result[lane] = field | ~((1u << c) - 1u);
                else result[lane] = field;
            }
            return result;
        }
        default:
            return std::nullopt;
        }
    }();
    table[value] = result;
    return result;
}

void Lowering::foldLaneIndexValues() {
    std::unordered_map<const IrValue*, std::optional<Lanes>> table;
    for (IrBlock* block : program.BlockOrder()) {
        for (IrValue* inst : block->Instructions()) {
            if (inst->IsIdentity() || inst->HasImmediate() || inst->Opcode() == IrOpcode::LaneId) continue;
            if (inst->Type() != IrType::U32 && inst->Type() != IrType::U1) continue;
            const auto values = laneValues(inst, table, 0u);
            if (!values || values->empty()) continue;
            if (!std::all_of(values->begin(), values->end(), [&](std::uint32_t word) { return word == values->front(); })) continue;
            IrValue& constant = inst->Type() == IrType::U1 ? builder.ConstantBool(values->front() != 0u) : builder.Constant(values->front());
            inst->ReplaceUsesWith(&constant, true);
        }
    }
}

std::optional<Lowering::ThreadBit> Lowering::matchThreadBit(const IrValue* value) const {
    value = value->Resolve();
    if (value->Opcode() != IrOpcode::INotEqual32) return std::nullopt;
    const IrValue* masked = OtherArgument(value, 0u);
    if (masked == nullptr || masked->Opcode() != IrOpcode::BitwiseAnd32) return std::nullopt;
    const IrValue* shifted = OtherArgument(masked, 1u);
    if (shifted == nullptr || shifted->Opcode() != IrOpcode::ShiftRightLogical32) return std::nullopt;
    const IrValue* amount = shifted->Argument(1)->Resolve();
    if (amount->Opcode() != IrOpcode::BitwiseAnd32 || !IsLaneId(OtherArgument(amount, 31u))) return std::nullopt;
    IrValue* word = shifted->Argument(0)->Resolve();
    if (word->Opcode() == IrOpcode::SelectU32) {
        const IrValue* half = word->Argument(0)->Resolve();
        if (half->Opcode() == IrOpcode::ULessThan32 && IsLaneId(half->Argument(0)) && IsImmediateU32(half->Argument(1), 32u)) {
            return ThreadBit{word->Argument(1)->Resolve(), word->Argument(2)->Resolve()};
        }
    }
    return ThreadBit{word, nullptr};
}

IrValue* Lowering::later(IrValue* first, IrValue* second) const {
    if (second == nullptr || second->Parent() == nullptr) return first;
    if (first == nullptr || first->Parent() == nullptr) return second;
    if (first->Parent() != second->Parent()) return blockIndex.at(first->Parent()) >= blockIndex.at(second->Parent()) ? first : second;
    for (const IrValue* inst : first->Parent()->Instructions()) {
        if (inst == first) return second;
        if (inst == second) return first;
    }
    return first;
}

IrValue& Lowering::emitAfter(IrValue* low, IrValue* high, IrOpcode opcode, IrType type, std::initializer_list<IrValue*> arguments) {
    IrValue& created = program.CreateValue(opcode, type);
    for (IrValue* argument : arguments) created.AddArgument(argument);
    IrValue* position = later(low, high);
    for (IrValue* argument : arguments) position = later(position, argument);
    if (position == nullptr || position->Parent() == nullptr) throw std::runtime_error("lane-local lowering: a lane bit has no insertion point");
    IrBlock* block = position->Parent();
    auto& instructions = block->Instructions();
    auto it = std::find(instructions.begin(), instructions.end(), position);
    if (it == instructions.end()) throw std::runtime_error("lane-local lowering: insertion point is not in its block");
    ++it;
    while (it != instructions.end() && (*it)->IsPhi()) ++it;
    if (it == instructions.end()) block->AppendInstruction(&created);
    else block->InsertInstructionBefore(*it, &created);
    return created;
}

IrValue* Lowering::laneBit(IrValue* low, IrValue* high) {
    low = low->Resolve();
    if (high != nullptr) high = high->Resolve();
    if (lanes <= 32u) high = nullptr;
    const PairKey key{low, high};
    if (const auto found = bits.find(key); found != bits.end()) return found->second;
    IrValue* result = computeLaneBit(low, high);
    bits[key] = result;
    bitLog.push_back(key);
    return result;
}

IrValue* Lowering::computeLaneBit(IrValue* low, IrValue* high) {
    const bool pair = high != nullptr;
    if (low->HasImmediate() && (!pair || high->HasImmediate())) {
        if (low->Type() != IrType::U32 || (pair && high->Type() != IrType::U32)) return nullptr;
        std::optional<bool> common;
        for (std::uint32_t lane = 0; lane < lanes; ++lane) {
            const bool bit = lane < 32u ? ((low->ImmediateU32() >> lane) & 1u) != 0u : ((high->ImmediateU32() >> (lane - 32u)) & 1u) != 0u;
            if (!common) common = bit;
            else if (*common != bit) return nullptr;
        }
        return &builder.ConstantBool(common.value_or(false));
    }
    if (low->HasImmediate() || (pair && high->HasImmediate())) return nullptr;
    if (low->Opcode() == IrOpcode::CompositeExtractU32x4) {
        if (!IsImmediateU32(low->Argument(1), 0u)) return nullptr;
        IrValue* ballot = low->Argument(0)->Resolve();
        if (ballot->Opcode() != IrOpcode::Ballot) return nullptr;
        if (pair && (high->Opcode() != IrOpcode::CompositeExtractU32x4 || high->Argument(0)->Resolve() != ballot || !IsImmediateU32(high->Argument(1), 1u))) return nullptr;
        return ballot->Argument(0)->Resolve();
    }
    if (pair && low->Opcode() != high->Opcode()) return nullptr;
    const auto side = [&](std::size_t index) { return pair ? high->Argument(index) : nullptr; };
    switch (low->Opcode()) {
    case IrOpcode::BitwiseAnd32:
    case IrOpcode::BitwiseOr32:
    case IrOpcode::BitwiseXor32: {
        const auto logical = low->Opcode() == IrOpcode::BitwiseAnd32 ? IrOpcode::LogicalAnd : low->Opcode() == IrOpcode::BitwiseOr32 ? IrOpcode::LogicalOr : IrOpcode::LogicalXor;
        IrValue* a = laneBit(low->Argument(0), side(0));
        IrValue* b = laneBit(low->Argument(1), side(1));
        if (pair && (a == nullptr || b == nullptr)) {
            a = laneBit(low->Argument(0), side(1));
            b = laneBit(low->Argument(1), side(0));
        }
        if (a == nullptr || b == nullptr) return nullptr;
        return &emitAfter(low, high, logical, IrType::U1, {a, b});
    }
    case IrOpcode::BitwiseNot32: {
        IrValue* a = laneBit(low->Argument(0), side(0));
        if (a == nullptr) return nullptr;
        return &emitAfter(low, high, IrOpcode::LogicalNot, IrType::U1, {a});
    }
    case IrOpcode::SelectU32: {
        IrValue* condition = low->Argument(0)->Resolve();
        if (pair && high->Argument(0)->Resolve() != condition) return nullptr;
        IrValue* a = laneBit(low->Argument(1), side(1));
        IrValue* b = laneBit(low->Argument(2), side(2));
        if (a == nullptr || b == nullptr) return nullptr;
        return &emitAfter(low, high, IrOpcode::SelectU1, IrType::U1, {condition, a, b});
    }
    case IrOpcode::Phi: {
        if (low->ArgumentCount() == 0u || low->ArgumentCount() != low->PhiBlockCount()) return nullptr;
        if (pair && (high->Parent() != low->Parent() || high->ArgumentCount() != low->ArgumentCount() || high->PhiBlockCount() != low->PhiBlockCount())) return nullptr;
        for (std::size_t index = 0; pair && index < low->PhiBlockCount(); ++index) {
            if (low->PhiBlock(index) != high->PhiBlock(index)) return nullptr;
        }
        IrValue& phi = program.CreateValue(IrOpcode::Phi, IrType::U1);
        low->Parent()->InsertInstructionBefore(low, &phi);
        const PairKey key{low, high};
        const auto mark = bitLog.size();
        bits[key] = &phi;
        bitLog.push_back(key);
        std::vector<IrValue*> incoming;
        for (std::size_t index = 0; index < low->ArgumentCount(); ++index) {
            IrValue* bit = laneBit(low->Argument(index), side(index));
            if (bit == nullptr) {
                for (std::size_t entry = mark; entry < bitLog.size(); ++entry) bits.erase(bitLog[entry]);
                bitLog.resize(mark);
                return nullptr;
            }
            incoming.push_back(bit);
        }
        for (std::size_t index = 0; index < incoming.size(); ++index) phi.AddPhiOperand(low->PhiBlock(index), incoming[index]);
        return &phi;
    }
    default:
        return nullptr;
    }
}

void Lowering::rewriteThreadBits() {
    std::vector<IrValue*> candidates;
    for (IrBlock* block : program.BlockOrder()) {
        for (IrValue* inst : block->Instructions()) {
            if (inst->Opcode() == IrOpcode::INotEqual32 && matchThreadBit(inst)) candidates.push_back(inst);
        }
    }
    for (IrValue* inst : candidates) {
        const auto shape = matchThreadBit(inst);
        if (!shape) continue;
        IrValue* bit = laneBit(shape->low, shape->high);
        if (bit != nullptr) inst->ReplaceUsesWith(bit, true);
    }
}

void Lowering::rewriteUniformLaneReads() {
    const auto uniformValues = WaveUniformValues(program);
    for (IrBlock* block : program.BlockOrder()) {
        for (IrValue* inst : block->Instructions()) {
            if (inst->Opcode() != IrOpcode::ReadFirstLane && inst->Opcode() != IrOpcode::ReadLane) continue;
            IrValue* source = inst->Argument(0)->Resolve();
            if (!source->HasImmediate() && !uniformValues.contains(source)) continue;
            if (inst->Opcode() == IrOpcode::ReadLane) {
                const IrValue* lane = inst->Argument(1)->Resolve();
                if (lane->HasImmediate() && lane->ImmediateU32() >= lanes) continue;
            }
            inst->ReplaceUsesWith(source, true);
        }
    }
}

std::optional<std::vector<std::size_t>> Lowering::armBlocks(std::size_t entry, std::size_t merge, std::size_t header, std::string& reason) const {
    std::vector<std::size_t> result;
    if (entry == merge) return result;
    const auto& order = program.BlockOrder();
    std::unordered_set<std::size_t> seen{entry};
    std::vector<std::size_t> pending{entry};
    while (!pending.empty()) {
        const auto index = pending.back();
        pending.pop_back();
        if (index == header || index <= header || index >= merge) {
            reason = index == header || index <= header ? "controls a loop" : "whose region leaves the construct";
            return std::nullopt;
        }
        result.push_back(index);
        for (const IrBlock* successor : order[index]->Successors()) {
            const auto next = blockIndex.at(successor);
            if (next == merge) continue;
            if (seen.insert(next).second) pending.push_back(next);
        }
    }
    return result;
}

bool Lowering::transparent(const Construct& construct, Implications& implies, std::string& reason) const {
    const auto& order = program.BlockOrder();
    const auto& info = program.Metadata().blockInfo;
    for (const auto index : construct.arms) {
        if (info[index].terminator.kind == TerminatorKind::Return) {
            reason = "whose region ends the program";
            return false;
        }
        if (info[index].terminator.kind != TerminatorKind::Branch && info[index].terminator.kind != TerminatorKind::ConditionalBranch) {
            reason = "whose region leaves the construct";
            return false;
        }
        for (const IrValue* inst : order[index]->Instructions()) {
            const auto opcode = inst->Opcode();
            const IrValue* guard = nullptr;
            switch (opcode) {
            case IrOpcode::Reference:
            case IrOpcode::ReferenceU32:
            case IrOpcode::ControlNop:
            case IrOpcode::Waitcnt:
            case IrOpcode::Sendmsg:
            case IrOpcode::TtraceData:
            case IrOpcode::InstPrefetch:
                continue;
            case IrOpcode::Barrier:
                if (stage == IrShaderStage::Vertex) continue;
                break;
            case IrOpcode::SetAttribute:
                guard = inst->Argument(1);
                break;
            default:
                if (!IrOpcodeHasSideEffects(opcode)) continue;
                {
                    const auto buffer = BufferAccessOf(opcode);
                    const auto shared = SharedAccessOf(opcode);
                    const auto address = AddressOpcodeInfoOf(opcode).access;
                    const auto image = ImageOpcodeInfoOf(opcode).access;
                    const bool masked = buffer == BufferAccess::Write || buffer == BufferAccess::Atomic || shared == SharedAccess::Write || shared == SharedAccess::Atomic || address == AddressAccess::Write || address == AddressAccess::Atomic || image == ImageAccess::Write || image == ImageAccess::Atomic;
                    if (masked && inst->ArgumentCount() != 0u) guard = inst->Argument(inst->ArgumentCount() - 1u);
                }
                break;
            }
            if (guard == nullptr || !implies.Implies(guard)) {
                reason = "whose region has an effect not masked by EXEC (" + std::string(IrOpcodeName(opcode)) + ")";
                return false;
            }
        }
    }
    return true;
}

bool Lowering::sameBase(IrValue* first, IrValue* second, Implications& implies) const {
    if (first == second) return true;
    return first->Type() == IrType::U1 && second->Type() == IrType::U1 && implies.Implies(first) && implies.Implies(second);
}

std::optional<IrValue*> Lowering::peel(IrValue* value, const Construct& construct, Implications& implies, std::unordered_map<const IrValue*, IrValue*>& memo, std::unordered_set<const IrValue*>& peeling) const {
    value = value->Resolve();
    if (value->HasImmediate() || value->Parent() == nullptr || !construct.inside.contains(blockIndex.at(value->Parent()))) return value;
    if (const auto known = memo.find(value); known != memo.end()) return known->second;
    if (peeling.contains(value)) return std::optional<IrValue*>(nullptr);
    if (value->Type() == IrType::U1 && implies.Implies(value)) return value;
    const auto argument = [&](std::size_t index) { return peel(value->Argument(index), construct, implies, memo, peeling); };
    const auto negated = [&](const IrValue* predicate) {
        predicate = predicate->Resolve();
        return predicate->Opcode() == IrOpcode::LogicalNot && implies.Implies(predicate->Argument(0));
    };
    std::optional<IrValue*> result;
    switch (value->Opcode()) {
    case IrOpcode::SelectU1:
    case IrOpcode::SelectU32:
    case IrOpcode::SelectF32:
        if (implies.Implies(value->Argument(0))) result = argument(2);
        else if (negated(value->Argument(0))) result = argument(1);
        break;
    case IrOpcode::DppUpdateU32:
        if (value->ArgumentCount() == 3u && implies.Implies(value->Argument(2))) result = argument(1);
        break;
    case IrOpcode::LogicalOr:
        if (implies.Implies(value->Argument(1))) result = argument(0);
        else if (implies.Implies(value->Argument(0))) result = argument(1);
        break;
    case IrOpcode::LogicalAnd:
        if (negated(value->Argument(1))) result = argument(0);
        else if (negated(value->Argument(0))) result = argument(1);
        break;
    case IrOpcode::Phi: {
        peeling.insert(value);
        IrValue* base = nullptr;
        bool failed = value->ArgumentCount() == 0u;
        for (std::size_t index = 0; !failed && index < value->ArgumentCount(); ++index) {
            const auto incoming = argument(index);
            if (!incoming) {
                failed = true;
                break;
            }
            if (*incoming == nullptr) continue;
            if (base == nullptr) base = *incoming;
            else if (!sameBase(base, *incoming, implies)) failed = true;
        }
        peeling.erase(value);
        if (!failed && base != nullptr) result = base;
        break;
    }
    default:
        break;
    }
    if (result && *result != nullptr) memo.emplace(value, *result);
    return result;
}

bool Lowering::mergePhisTransparent(const Construct& construct, Implications& implies, std::string& reason) const {
    const auto& order = program.BlockOrder();
    std::unordered_map<const IrValue*, IrValue*> memo;
    std::unordered_set<const IrValue*> peeling;
    for (const IrValue* phi : order[construct.merge]->Instructions()) {
        if (!phi->IsPhi()) continue;
        IrValue* base = nullptr;
        for (std::size_t index = 0; index < phi->PhiBlockCount() && index < phi->ArgumentCount(); ++index) {
            const auto predecessor = blockIndex.at(phi->PhiBlock(index));
            if (predecessor != construct.header && !construct.inside.contains(predecessor)) continue;
            const auto peeled = peel(phi->Argument(index), construct, implies, memo, peeling);
            if (!peeled || *peeled == nullptr) {
                reason = "whose region leaves a value that the skipped path does not have";
                return false;
            }
            if (base == nullptr) base = *peeled;
            else if (!sameBase(base, *peeled, implies)) {
                reason = "whose region leaves a value that the skipped path does not have";
                return false;
            }
        }
        if (base != nullptr && base->Parent() != nullptr && construct.inside.contains(blockIndex.at(base->Parent()))) {
            reason = "whose region leaves a value that the skipped path does not have";
            return false;
        }
    }
    return true;
}

void Lowering::lowerWaveMaskBranches() {
    uniform = WaveUniformValues(program);
    auto& info = program.Metadata().blockInfo;
    for (std::size_t header = info.size(); header-- > 0;) {
        auto& block = info[header];
        if (block.terminator.kind != TerminatorKind::ConditionalBranch || !IsWaveMaskBranch(block) || block.condition == nullptr) continue;
        IrValue* condition = block.condition->Resolve();
        if (condition->HasImmediate() || uniform.contains(condition)) {
            block.laneLocalBranch = true;
            continue;
        }
        const auto trueIndex = blockById.find(block.terminator.trueBlock);
        const auto falseIndex = blockById.find(block.terminator.falseBlock);
        if (trueIndex == blockById.end() || falseIndex == blockById.end()) continue;
        if (trueIndex->second == falseIndex->second) {
            block.laneLocalBranch = true;
            continue;
        }
        const auto mergeIndex = blockById.find(block.terminator.mergeBlock);
        if (block.terminator.loopHeader || mergeIndex == blockById.end()) {
            branchReasons[header] = "controls a loop";
            continue;
        }
        Construct construct;
        construct.header = header;
        construct.merge = mergeIndex->second;
        std::string reason;
        bool ok = true;
        for (const auto entry : {trueIndex->second, falseIndex->second}) {
            const auto arm = armBlocks(entry, construct.merge, header, reason);
            if (!arm) {
                ok = false;
                break;
            }
            for (const auto index : *arm) {
                if (!construct.inside.insert(index).second) {
                    reason = "whose region leaves the construct";
                    ok = false;
                }
                construct.arms.push_back(index);
            }
        }
        if (!ok) {
            branchReasons[header] = reason;
            continue;
        }
        const bool zero = block.terminator.condition == BranchCondition::ExecZero || block.terminator.condition == BranchCondition::VccZero;
        IrValue* active = condition;
        if (zero) {
            if (condition->Opcode() == IrOpcode::LogicalNot) active = condition->Argument(0)->Resolve();
            else {
                IrValue* existing = nullptr;
                for (const IrUse& use : condition->OperandUses()) {
                    if (use.user->Opcode() == IrOpcode::LogicalNot && !use.user->IsIdentity() && use.user->Parent() != nullptr) existing = use.user;
                }
                if (existing == nullptr) {
                    IrValue& inverted = program.CreateValue(IrOpcode::LogicalNot, IrType::U1);
                    inverted.AddArgument(condition);
                    program.BlockOrder()[header]->AppendInstruction(&inverted);
                    existing = &inverted;
                }
                active = existing;
            }
        }
        std::vector<IrValue*> guards{active};
        if (active->Opcode() == IrOpcode::LogicalAnd) {
            IrValue* left = active->Argument(0)->Resolve();
            IrValue* right = active->Argument(1)->Resolve();
            if (left->HasImmediate() || uniform.contains(left)) guards.push_back(right);
            if (right->HasImmediate() || uniform.contains(right)) guards.push_back(left);
        }
        bool lowered = false;
        reason = "whose condition is not a mask over EXEC";
        for (IrValue* guard : guards) {
            std::size_t work = 65536;
            Implications implies(guard, work);
            if (transparent(construct, implies, reason) && mergePhisTransparent(construct, implies, reason)) {
                lowered = true;
                break;
            }
        }
        if (lowered) block.laneLocalBranch = true;
        else branchReasons[header] = reason;
    }
}

std::vector<SubgroupUse> Describe(const IrProgram& program, const std::map<std::size_t, std::string>& branchReasons) {
    std::vector<SubgroupUse> uses;
    const auto& order = program.BlockOrder();
    const auto& info = program.Metadata().blockInfo;
    std::unordered_map<const IrBlock*, std::size_t> indices;
    for (std::size_t index = 0; index < order.size(); ++index) indices.emplace(order[index], index);
    const auto add = [&](std::string text, std::size_t block) {
        SubgroupUse use{std::move(text), info[block].startPc, info[block].endPc};
        if (std::none_of(uses.begin(), uses.end(), [&](const SubgroupUse& other) { return other.description == use.description && other.startPc == use.startPc && other.endPc == use.endPc; })) uses.push_back(std::move(use));
    };
    for (std::size_t index = 0; index < order.size(); ++index) {
        for (const IrValue* inst : order[index]->Instructions()) {
            if (inst->IsIdentity()) continue;
            const auto shared = SharedAccessOf(inst->Opcode());
            if (shared == SharedAccess::Append || shared == SharedAccess::Consume) {
                add("ds_append/ds_consume", index);
                continue;
            }
            switch (inst->Opcode()) {
            case IrOpcode::Ballot: if (inst->HasUses()) add("a ballot read as a value", index); break;
            case IrOpcode::LaneId: if (inst->HasUses()) add("the lane index", index); break;
            case IrOpcode::ReadFirstLane:
            case IrOpcode::ReadLane: add("v_readfirstlane/v_readlane of a lane-varying value", index); break;
            case IrOpcode::WriteLane: add("v_writelane", index); break;
            case IrOpcode::DppMoveU32:
            case IrOpcode::DppUpdateU32: add("DPP", index); break;
            case IrOpcode::Permlane16U32: add("v_permlane16/x16", index); break;
            case IrOpcode::SwizzleU32:
            case IrOpcode::PermuteU32:
            case IrOpcode::BpermuteU32: add("ds_swizzle/ds_permute/ds_bpermute", index); break;
            default: break;
            }
        }
        if (info[index].terminator.kind == TerminatorKind::ConditionalBranch && IsWaveMaskBranch(info[index])) {
            const auto reason = branchReasons.find(index);
            add("a wave-mask branch " + (reason == branchReasons.end() ? std::string("that is not lane-local") : reason->second), index);
        }
    }
    return uses;
}

std::vector<SubgroupUse> Lowering::describe() const {
    return Describe(program, branchReasons);
}

std::vector<SubgroupUse> Lowering::Run() {
    indexBlocks();
    foldLaneIndexValues();
    cleanup();
    rewriteThreadBits();
    cleanup();
    rewriteUniformLaneReads();
    cleanup();
    indexBlocks();
    lowerWaveMaskBranches();
    cleanup();
    return describe();
}

}

std::vector<SubgroupUse> DescribeSubgroupUses(const IrProgram& program) {
    return Describe(program, {});
}

std::vector<SubgroupUse> LaneLocalLowering::Lower(IrProgram& program, std::uint32_t lanes) const {
    if (lanes == 0u || lanes > 64u) throw std::runtime_error("lane-local lowering: invalid lane count");
    return Lowering(program, lanes).Run();
}

}
