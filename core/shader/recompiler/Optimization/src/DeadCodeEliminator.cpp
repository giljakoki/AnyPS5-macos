#include "Optimization/DeadCodeEliminator.hpp"
#include <unordered_set>
#include <vector>

namespace ShaderRecompiler {
namespace {

bool removePhiWebs(std::span<IrBlock* const> blocks) {
    std::vector<IrValue*> phis;
    std::vector<IrValue*> pending;
    std::unordered_set<const IrValue*> live;
    for (IrBlock* block : blocks) {
        for (IrValue* inst : block->Instructions()) {
            if (!inst->IsPhi()) continue;
            phis.push_back(inst);
            for (const IrValue* user : inst->Uses()) {
                if (user->IsPhi()) continue;
                if (live.insert(inst).second) pending.push_back(inst);
                break;
            }
        }
    }
    while (!pending.empty()) {
        IrValue* phi = pending.back();
        pending.pop_back();
        for (IrValue* argument : phi->Arguments()) {
            IrValue* incoming = argument == nullptr ? nullptr : argument->Resolve();
            if (incoming != nullptr && incoming->IsPhi() && live.insert(incoming).second) pending.push_back(incoming);
        }
    }
    std::vector<IrValue*> dead;
    for (IrValue* phi : phis) {
        if (!live.contains(phi) && phi->HasUses()) dead.push_back(phi);
    }
    for (IrValue* phi : dead) {
        for (std::size_t index = 0; index < phi->ArgumentCount(); ++index) phi->ReplaceArgument(index, nullptr);
    }
    for (IrValue* phi : dead) {
        auto& instructions = phi->Parent()->Instructions();
        for (auto it = instructions.begin(); it != instructions.end(); ++it) {
            if (*it != phi) continue;
            instructions.erase(it);
            break;
        }
        phi->Invalidate();
        phi->SetParent(nullptr);
    }
    return !dead.empty();
}

}

void DeadCodeEliminator::RemoveIdentities(std::span<IrBlock* const> blocks) const {
    for (IrBlock* block : blocks) {
        auto& instructions = block->Instructions();
        for (auto it = instructions.begin(); it != instructions.end();) {
            IrValue* inst = *it;
            if (inst->Opcode() != IrOpcode::Identity) {
                ++it;
                continue;
            }
            IrValue* replacement = inst->Argument(0);
            inst->ReplaceUsesWith(replacement, false);
            it = instructions.erase(it);
            inst->SetParent(nullptr);
        }
    }
}

void DeadCodeEliminator::RemoveIdentities(IrProgram& program) const {
    RemoveIdentities(program.BlockOrder());
}

void DeadCodeEliminator::Eliminate(std::span<IrBlock* const> blocks) const {
    bool changed;
    do {
        changed = false;
        for (auto blockIt = blocks.rbegin(); blockIt != blocks.rend(); ++blockIt) {
            auto& instructions = (*blockIt)->Instructions();
            auto it = instructions.end();
            while (it != instructions.begin()) {
                --it;
                IrValue* inst = *it;
                if (inst->HasUses() || inst->MayHaveSideEffects()) {
                    continue;
                }
                inst->Invalidate();
                it = instructions.erase(it);
                inst->SetParent(nullptr);
                changed = true;
            }
        }
        if (!changed) changed = removePhiWebs(blocks);
    } while (changed);
}

void DeadCodeEliminator::Eliminate(IrProgram& program) const {
    Eliminate(program.BlockOrder());
}

}
