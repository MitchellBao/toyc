#include "pass_manager.h"

#include "analysis/cfg.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace toyc::passes {
namespace {

// Textbook loop-invariant code motion.
//
// For each natural loop that has a single dedicated preheader, hoist
// loop-invariant instructions into the preheader. Only pure, non-faulting
// instructions are hoisted (Const/Copy/Unary/Binary, and loads proven
// invariant), so executing them once in the preheader is safe even for a
// zero-trip loop -- hence no "dominates every exit" check is needed. Invariants
// in the loop *header* (the condition block) are hoisted too, e.g. the `n` load
// in `while (i < n)` that would otherwise be re-loaded every iteration. LICM
// only moves loads, never folds them to constants, so a dynamic loop bound stays
// dynamic and loop-sum still leaves such loops alone.

bool isWhileHeader(const ir::BasicBlock& block)
{
    return block.label.find(".while.cond") == 0;
}

bool functionContainsCall(const ir::Function& function)
{
    for (const ir::BasicBlock& block : function.blocks) {
        for (const ir::Instruction& inst : block.instructions) {
            if (inst.kind == ir::InstructionKind::Call) {
                return true;
            }
        }
    }
    return false;
}

bool definesValue(const ir::Instruction& inst)
{
    return inst.dst.id >= 0
        && inst.kind != ir::InstructionKind::StoreGlobal
        && inst.kind != ir::InstructionKind::StoreLocal;
}

bool isPureHoistable(const ir::Instruction& inst)
{
    if (inst.hasSideEffect || !definesValue(inst)) {
        return false;
    }
    if (inst.kind == ir::InstructionKind::Binary
        && (inst.binaryOp == ir::BinaryOpcode::Div || inst.binaryOp == ir::BinaryOpcode::Mod)) {
        return inst.operands.size() == 2 && inst.operands[1].isImmediate && inst.operands[1].immediate != 0;
    }
    switch (inst.kind) {
    case ir::InstructionKind::Const:
    case ir::InstructionKind::Copy:
    case ir::InstructionKind::Unary:
    case ir::InstructionKind::Binary:
    case ir::InstructionKind::LoadLocal:
        return true;
    case ir::InstructionKind::LoadGlobal:
        // Hoisting a global load produces an SSA value live across the loop
        // back-edge, which the current backend mishandles (spurious rotation,
        // uninitialised bound). Leave global loads in place.
        return false;
    case ir::InstructionKind::StoreGlobal:
    case ir::InstructionKind::StoreLocal:
    case ir::InstructionKind::Call:
        return false;
    }
    return false;
}

std::unordered_set<int> collectNaturalLoop(const analysis::Cfg& cfg, int header, int backedge)
{
    std::unordered_set<int> loop;
    std::vector<int> stack;
    loop.insert(header);
    loop.insert(backedge);
    stack.push_back(backedge);
    while (!stack.empty()) {
        const int block = stack.back();
        stack.pop_back();
        for (int pred : cfg.predecessors[static_cast<std::size_t>(block)]) {
            if (pred < 0 || loop.find(pred) != loop.end()) {
                continue;
            }
            loop.insert(pred);
            if (pred != header) {
                stack.push_back(pred);
            }
        }
    }
    return loop;
}

bool runOnLoop(ir::Function& function, const analysis::Cfg& cfg, int header, int backedge)
{
    const std::unordered_set<int> loop = collectNaturalLoop(cfg, header, backedge);

    std::vector<int> outside;
    for (int pred : cfg.predecessors[static_cast<std::size_t>(header)]) {
        if (loop.find(pred) == loop.end()) {
            outside.push_back(pred);
        }
    }
    if (outside.size() != 1) {
        return false;
    }
    const int preheader = outside.front();
    if (preheader < 0 || preheader >= static_cast<int>(function.blocks.size())) {
        return false;
    }
    const ir::Terminator& preTerm = function.blocks[static_cast<std::size_t>(preheader)].terminator;
    if (preTerm.kind != ir::TerminatorKind::Jump || preTerm.trueBlock != header) {
        return false;
    }

    std::unordered_set<int> valuesDefinedInLoop;
    std::unordered_set<std::string> storedLocals;
    std::unordered_set<std::string> storedGlobals;
    bool hasCall = false;
    for (int bi : loop) {
        for (const ir::Instruction& inst : function.blocks[static_cast<std::size_t>(bi)].instructions) {
            if (definesValue(inst)) {
                valuesDefinedInLoop.insert(inst.dst.id);
            }
            if (inst.kind == ir::InstructionKind::StoreLocal && !inst.symbol.empty()) {
                storedLocals.insert(inst.symbol);
            } else if (inst.kind == ir::InstructionKind::StoreGlobal && !inst.symbol.empty()) {
                storedGlobals.insert(inst.symbol);
            } else if (inst.kind == ir::InstructionKind::Call) {
                hasCall = true;
            }
        }
    }

    std::vector<int> orderedBlocks(loop.begin(), loop.end());
    std::sort(orderedBlocks.begin(), orderedBlocks.end());

    std::unordered_set<int> invariant;
    std::vector<std::pair<int, int>> toHoist;
    std::unordered_set<long long> chosen;

    auto operandInvariant = [&](const ir::Operand& operand) {
        if (operand.isImmediate || operand.value.id < 0) {
            return true;
        }
        if (valuesDefinedInLoop.find(operand.value.id) == valuesDefinedInLoop.end()) {
            return true;
        }
        return invariant.find(operand.value.id) != invariant.end();
    };

    bool changed = true;
    while (changed) {
        changed = false;
        for (int bi : orderedBlocks) {
            const ir::BasicBlock& block = function.blocks[static_cast<std::size_t>(bi)];
            for (int ii = 0; ii < static_cast<int>(block.instructions.size()); ++ii) {
                const long long key = static_cast<long long>(bi) * 1000000LL + ii;
                if (chosen.find(key) != chosen.end()) {
                    continue;
                }
                const ir::Instruction& inst = block.instructions[static_cast<std::size_t>(ii)];
                if (!isPureHoistable(inst)) {
                    continue;
                }
                if (inst.kind == ir::InstructionKind::LoadLocal && storedLocals.find(inst.symbol) != storedLocals.end()) {
                    continue;
                }
                if (inst.kind == ir::InstructionKind::LoadGlobal
                    && (hasCall || storedGlobals.find(inst.symbol) != storedGlobals.end())) {
                    continue;
                }
                bool allInvariant = true;
                for (const ir::Operand& operand : inst.operands) {
                    if (!operandInvariant(operand)) {
                        allInvariant = false;
                        break;
                    }
                }
                if (!allInvariant) {
                    continue;
                }
                chosen.insert(key);
                toHoist.emplace_back(bi, ii);
                invariant.insert(inst.dst.id);
                changed = true;
            }
        }
    }

    if (toHoist.empty()) {
        return false;
    }

    std::sort(toHoist.begin(), toHoist.end());
    std::vector<ir::Instruction> hoisted;
    hoisted.reserve(toHoist.size());
    for (const auto& [bi, ii] : toHoist) {
        hoisted.push_back(function.blocks[static_cast<std::size_t>(bi)].instructions[static_cast<std::size_t>(ii)]);
    }

    std::vector<std::pair<int, int>> descending = toHoist;
    std::sort(descending.begin(), descending.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) {
            return a.first > b.first;
        }
        return a.second > b.second;
    });
    for (const auto& [bi, ii] : descending) {
        auto& instrs = function.blocks[static_cast<std::size_t>(bi)].instructions;
        instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(ii));
    }

    auto& preInstrs = function.blocks[static_cast<std::size_t>(preheader)].instructions;
    preInstrs.insert(preInstrs.end(), std::make_move_iterator(hoisted.begin()), std::make_move_iterator(hoisted.end()));
    return true;
}

class LicmPass final : public Pass {
public:
    std::string name() const override { return "licm"; }
    bool run(ir::Module& module) override
    {
        bool changed = false;
        for (ir::Function& function : module.functions) {
            if (functionContainsCall(function)) {
                continue;
            }
            for (int guard = 0; guard < 256; ++guard) {
                const analysis::Cfg cfg = analysis::buildCfg(function);
                bool localChanged = false;
                for (int header = 0; header < static_cast<int>(function.blocks.size()); ++header) {
                    if (!isWhileHeader(function.blocks[static_cast<std::size_t>(header)])) {
                        continue;
                    }
                    for (int pred : cfg.predecessors[static_cast<std::size_t>(header)]) {
                        if (pred > header && runOnLoop(function, cfg, header, pred)) {
                            localChanged = true;
                            changed = true;
                            break;
                        }
                    }
                    if (localChanged) {
                        break;
                    }
                }
                if (!localChanged) {
                    break;
                }
            }
        }
        return changed;
    }
};

} // namespace

std::unique_ptr<Pass> createLicmPass()
{
    return std::make_unique<LicmPass>();
}

} // namespace toyc::passes
