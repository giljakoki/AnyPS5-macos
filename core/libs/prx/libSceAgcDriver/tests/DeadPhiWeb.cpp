#include "IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/DeadCodeEliminator.hpp"
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <initializer_list>

using namespace ShaderRecompiler;

namespace {

struct Builder {
    IrProgram program;
    IrBlock* block = nullptr;

    Builder() {
        block = &newBlock();
        program.SetEntryBlock(*block);
    }

    IrBlock& newBlock() {
        auto& created = program.CreateBlock();
        program.BlockOrder().push_back(&created);
        return created;
    }

    IrValue& constant(std::uint32_t value) {
        auto& created = program.CreateValue(IrOpcode::Void, IrType::U32);
        created.SetImmediateU32(value);
        return created;
    }

    IrValue& emit(IrOpcode opcode, IrType type, std::initializer_list<IrValue*> arguments) {
        auto& created = program.CreateValue(opcode, type);
        for (auto* argument : arguments) created.AddArgument(argument);
        block->AppendInstruction(&created);
        return created;
    }

    IrValue& lane() {
        return emit(IrOpcode::LaneId, IrType::U32, {});
    }

    IrValue& add(IrValue& value, std::uint32_t immediate) {
        return emit(IrOpcode::IAdd32, IrType::U32, {&value, &constant(immediate)});
    }

    IrValue& phi(IrBlock& predecessor, IrValue& incoming) {
        auto& created = program.CreateValue(IrOpcode::Phi, IrType::U32);
        block->AppendInstruction(&created);
        created.AddPhiOperand(&predecessor, &incoming);
        return created;
    }

    void keep(IrValue& value) {
        (void)emit(IrOpcode::ReferenceU32, IrType::Void, {&value});
    }

    void eliminate() {
        DeadCodeEliminator{}.Eliminate(program);
    }
};

struct Web {
    Builder b;
    IrBlock* entry = nullptr;
    IrBlock* header = nullptr;
    IrBlock* body = nullptr;
    IrValue* input = nullptr;
    IrValue* carried = nullptr;
    IrValue* latch = nullptr;

    Web() {
        entry = b.block;
        input = &b.lane();
        header = &b.newBlock();
        b.block = header;
        carried = &b.phi(*entry, *input);
        body = &b.newBlock();
        b.block = body;
        latch = &b.phi(*header, *carried);
        carried->AddPhiOperand(body, latch);
    }
};

bool removed(const IrValue& value) {
    return value.Parent() == nullptr;
}

bool expect(const char* name, bool condition) {
    if (!condition) std::fprintf(stderr, "%s\n", name);
    return condition;
}

bool run(const char* name, const std::function<bool()>& test) {
    try {
        return expect(name, test());
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s: %s\n", name, error.what());
        return false;
    }
}

}

int main() {
    bool passed = true;

    passed &= run("a loop-carried value read only by its own phis must die with its source", [] {
        Web w;
        w.b.eliminate();
        return removed(*w.carried) && removed(*w.latch) && removed(*w.input) && w.header->Instructions().empty() && w.body->Instructions().empty();
    });

    passed &= run("a loop-carried value read after the loop must stay", [] {
        Web w;
        auto& exit = w.b.newBlock();
        w.b.block = &exit;
        w.b.keep(w.b.phi(*w.header, *w.carried));
        w.b.eliminate();
        return !removed(*w.carried) && !removed(*w.latch) && !removed(*w.input);
    });

    passed &= run("a loop-carried value read in the loop body must stay", [] {
        Web w;
        w.b.block = w.body;
        w.b.keep(w.b.add(*w.latch, 1u));
        w.b.eliminate();
        return !removed(*w.carried) && !removed(*w.latch) && !removed(*w.input);
    });

    passed &= run("a dead phi web beside a live one must die alone", [] {
        Web w;
        w.b.block = w.header;
        auto& other = w.b.phi(*w.entry, w.b.constant(3u));
        w.b.block = w.body;
        auto& otherLatch = w.b.phi(*w.header, other);
        other.AddPhiOperand(w.body, &otherLatch);
        w.b.keep(w.b.add(otherLatch, 1u));
        w.b.eliminate();
        return removed(*w.carried) && removed(*w.latch) && removed(*w.input) && !removed(other) && !removed(otherLatch);
    });

    return passed ? 0 : 1;
}
