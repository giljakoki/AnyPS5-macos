#include "IntermediateRepresentation/IrMetadata/StageIO.hpp"
#include "IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/HostInterpolationChecker.hpp"
#include "Optimization/ShaderStageInputInfo.hpp"
#include "Recompiler.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

using namespace ShaderRecompiler;

namespace {

struct Builder {
    IrProgram program;
    IrBlock* block = nullptr;

    Builder() {
        block = &program.CreateBlock();
        program.BlockOrder().push_back(block);
        program.SetEntryBlock(*block);
    }

    IrValue& constant(std::uint32_t value, IrType type = IrType::U32) {
        auto& created = program.CreateValue(IrOpcode::Void, type);
        created.SetImmediateU32(value);
        return created;
    }

    IrValue& emit(IrOpcode opcode, IrType type, std::initializer_list<IrValue*> arguments) {
        auto& created = program.CreateValue(opcode, type);
        for (auto* argument : arguments) created.AddArgument(argument);
        block->AppendInstruction(&created);
        return created;
    }

    IrValue& barycentric(std::uint32_t component) {
        return emit(IrOpcode::GetBuiltin, IrType::U32, {&constant(static_cast<std::uint32_t>(StageInputKind::BaryCoordSmooth)), &constant(component)});
    }

    IrValue& laneCondition() {
        return emit(IrOpcode::IEqual32, IrType::U1, {&emit(IrOpcode::LaneId, IrType::U32, {}), &constant(3u)});
    }

    IrValue& sample(IrValue& coordinate) {
        std::initializer_list<IrValue*> zeros{&constant(0u), &constant(0u), &constant(0u), &constant(0u), &constant(0u), &constant(0u), &constant(0u), &constant(0u), &constant(0u), &constant(0u), &constant(0u)};
        auto& address = program.CreateValue(IrOpcode::MakeImageAddress, IrType::ImageAddress);
        address.AddArgument(&coordinate);
        address.AddArgument(&constant(0u));
        for (auto* zero : zeros) address.AddArgument(zero);
        block->AppendInstruction(&address);
        auto& image = program.CreateValue(IrOpcode::Void, IrType::ImageResource);
        auto& sampler = program.CreateValue(IrOpcode::Void, IrType::SamplerResource);
        return emit(IrOpcode::ImageSampleRaw, IrType::U32x4, {&image, &sampler, &address});
    }

    IrValue& ownBit(IrValue& low, IrValue& high) {
        auto& lane = emit(IrOpcode::LaneId, IrType::U32, {});
        auto& lower = emit(IrOpcode::ULessThan32, IrType::U1, {&lane, &constant(32u)});
        auto& word = emit(IrOpcode::SelectU32, IrType::U32, {&lower, &low, &high});
        auto& shift = emit(IrOpcode::ShiftRightLogical32, IrType::U32, {&word, &emit(IrOpcode::BitwiseAnd32, IrType::U32, {&lane, &constant(31u)})});
        return emit(IrOpcode::INotEqual32, IrType::U1, {&emit(IrOpcode::BitwiseAnd32, IrType::U32, {&shift, &constant(1u)}), &constant(0u)});
    }

    std::pair<IrValue*, IrValue*> ballotWords(IrValue& predicate) {
        auto& ballot = emit(IrOpcode::Ballot, IrType::U32x4, {&predicate});
        return {&emit(IrOpcode::CompositeExtractU32x4, IrType::U32, {&ballot, &constant(0u)}), &emit(IrOpcode::CompositeExtractU32x4, IrType::U32, {&ballot, &constant(1u)})};
    }

    std::pair<IrValue*, IrValue*> wholeQuad(std::pair<IrValue*, IrValue*> words) {
        auto& pair = emit(IrOpcode::CompositeConstructU64, IrType::U64, {words.first, words.second});
        auto& wqm = emit(IrOpcode::WqmU64, IrType::U64, {&pair});
        return {&emit(IrOpcode::CompositeExtractU64, IrType::U32, {&wqm, &constant(0u)}), &emit(IrOpcode::CompositeExtractU64, IrType::U32, {&wqm, &constant(1u)})};
    }

    void exportColor(IrValue& color) {
        emit(IrOpcode::SetAttribute, IrType::Void, {&color, &constant(1u, IrType::U1)});
    }

    void exportWord(IrValue& word, IrValue* exec = nullptr) {
        auto& color = emit(IrOpcode::CompositeConstructU32x4, IrType::U32x4, {&word, &constant(0u), &constant(0u), &constant(0u)});
        emit(IrOpcode::SetAttribute, IrType::Void, {&color, exec != nullptr ? exec : &constant(1u, IrType::U1)});
    }

    bool lower() {
        ShaderPixelInputInfo pixel{};
        std::string reason;
        return HostInterpolationChecker{}.Lower(program, pixel, reason);
    }
};

bool run(const char* name, const std::function<bool()>& test) {
    bool passed = false;
    try {
        passed = test();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s: %s\n", name, error.what());
    }
    std::printf("%s %s\n", passed ? "PASS" : "FAIL", name);
    return passed;
}

}

int main(int argc, char** argv) {
    const bool enabled = argc == 2 && std::string_view(argv[1]) == "enabled";
    if (enabled != BarycentricMaskedZero()) {
        std::fprintf(stderr, "APS5_BARYCENTRIC_MASKED_ZERO does not match the requested mode\n");
        return 1;
    }
    bool passed = true;

    passed &= run(enabled ? "a barycentric kept in deselected lanes and used as a sample coordinate is zeroed" : "without the flag a barycentric kept for a sample coordinate is refused", [&] {
        Builder b;
        auto& i = b.barycentric(0u);
        auto& kept = b.emit(IrOpcode::SelectU32, IrType::U32, {&b.laneCondition(), &b.constant(5u), &i});
        b.exportColor(b.sample(kept));
        return b.lower() == enabled && i.HasUses() != enabled;
    });

    passed &= run("a barycentric kept in deselected lanes and then exported is refused", [&] {
        Builder b;
        auto& i = b.barycentric(0u);
        auto& kept = b.emit(IrOpcode::SelectU32, IrType::U32, {&b.laneCondition(), &b.constant(5u), &i});
        b.exportWord(kept);
        return !b.lower();
    });

    passed &= run("a barycentric taken by a select is refused", [&] {
        Builder b;
        auto& i = b.barycentric(0u);
        auto& taken = b.emit(IrOpcode::SelectU32, IrType::U32, {&b.laneCondition(), &i, &b.constant(5u)});
        b.exportColor(b.sample(taken));
        return !b.lower();
    });

    passed &= run(enabled ? "a compare of a kept barycentric guarded by the keep condition may reach a ballot" : "without the flag a guarded ballot of a kept barycentric is refused", [&] {
        Builder b;
        auto& i = b.barycentric(0u);
        auto& condition = b.laneCondition();
        auto& kept = b.emit(IrOpcode::SelectU32, IrType::U32, {&condition, &b.constant(5u), &i});
        auto& compare = b.emit(IrOpcode::IEqual32, IrType::U1, {&kept, &b.constant(5u)});
        auto& guarded = b.emit(IrOpcode::LogicalAnd, IrType::U1, {&condition, &compare});
        auto& ballot = b.emit(IrOpcode::Ballot, IrType::U32x4, {&guarded});
        b.exportColor(ballot);
        b.exportColor(b.sample(kept));
        return b.lower() == enabled;
    });

    passed &= run("an unguarded ballot of a kept barycentric is refused", [&] {
        Builder b;
        auto& i = b.barycentric(0u);
        auto& kept = b.emit(IrOpcode::SelectU32, IrType::U32, {&b.laneCondition(), &b.constant(5u), &i});
        auto& compare = b.emit(IrOpcode::IEqual32, IrType::U1, {&kept, &b.constant(5u)});
        b.exportColor(b.emit(IrOpcode::Ballot, IrType::U32x4, {&compare}));
        return !b.lower();
    });

    passed &= run("a barycentric kept where the export mask is a subset of the select's whole-quad mask is proven unobserved", [&] {
        Builder b;
        auto& i = b.barycentric(0u);
        const auto exported = b.ballotWords(b.laneCondition());
        const auto quad = b.wholeQuad(exported);
        auto& kept = b.emit(IrOpcode::SelectU32, IrType::U32, {&b.ownBit(*quad.first, *quad.second), &b.constant(5u), &i});
        b.exportWord(kept, &b.ownBit(*exported.first, *exported.second));
        return b.lower() && !i.HasUses();
    });

    passed &= run("a barycentric kept where the export mask is wider than the select's mask is refused", [&] {
        Builder b;
        auto& i = b.barycentric(0u);
        const auto exported = b.ballotWords(b.laneCondition());
        const auto quad = b.wholeQuad(exported);
        auto& kept = b.emit(IrOpcode::SelectU32, IrType::U32, {&b.ownBit(*exported.first, *exported.second), &b.constant(5u), &i});
        b.exportWord(kept, &b.ownBit(*quad.first, *quad.second));
        return !b.lower();
    });

    return passed ? 0 : 1;
}
