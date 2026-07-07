#include "pass_manager.h"

#include "analysis/cfg.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace toyc::passes {
namespace {

bool simplifyBranch(ir::Function& function)
{
    bool changed = false;
    for (ir::BasicBlock& block : function.blocks) {
        if (!block.hasTerminator || block.terminator.kind != ir::TerminatorKind::Branch) {
            continue;
        }
        ir::Terminator& term = block.terminator;
        if (term.trueBlock == term.falseBlock) {
            term.kind = ir::TerminatorKind::Jump;
            term.condition = {};
            changed = true;
            continue;
        }
        if (term.condition.isImmediate) {
            const int target = term.condition.immediate != 0 ? term.trueBlock : term.falseBlock;
            term.kind = ir::TerminatorKind::Jump;
            term.trueBlock = target;
            term.falseBlock = -1;
            term.condition = {};
            changed = true;
        }
    }
    return changed;
}

void remapOperandTarget(int& target, const std::vector<int>& oldToNew)
{
    if (target >= 0 && target < static_cast<int>(oldToNew.size())) {
        target = oldToNew[static_cast<std::size_t>(target)];
    }
}

bool removeUnreachableBlocks(ir::Function& function)
{
    if (function.blocks.empty()) {
        return false;
    }

    const analysis::Cfg cfg = analysis::buildCfg(function);
    std::vector<bool> reachable(function.blocks.size(), false);
    std::vector<int> stack = {0};
    reachable[0] = true;
    while (!stack.empty()) {
        const int block = stack.back();
        stack.pop_back();
        for (int succ : cfg.successors[static_cast<std::size_t>(block)]) {
            if (succ >= 0 && succ < static_cast<int>(reachable.size()) && !reachable[static_cast<std::size_t>(succ)]) {
                reachable[static_cast<std::size_t>(succ)] = true;
                stack.push_back(succ);
            }
        }
    }

    if (std::all_of(reachable.begin(), reachable.end(), [](bool value) { return value; })) {
        return false;
    }

    std::vector<int> oldToNew(function.blocks.size(), -1);
    std::vector<ir::BasicBlock> kept;
    kept.reserve(function.blocks.size());
    for (int i = 0; i < static_cast<int>(function.blocks.size()); ++i) {
        if (reachable[static_cast<std::size_t>(i)]) {
            oldToNew[static_cast<std::size_t>(i)] = static_cast<int>(kept.size());
            kept.push_back(std::move(function.blocks[static_cast<std::size_t>(i)]));
        }
    }

    for (ir::BasicBlock& block : kept) {
        if (block.terminator.kind == ir::TerminatorKind::Jump || block.terminator.kind == ir::TerminatorKind::Branch) {
            remapOperandTarget(block.terminator.trueBlock, oldToNew);
        }
        if (block.terminator.kind == ir::TerminatorKind::Branch) {
            remapOperandTarget(block.terminator.falseBlock, oldToNew);
        }
    }

    function.blocks = std::move(kept);
    return true;
}

bool redirectEmptyJumpBlocks(ir::Function& function)
{
    const analysis::Cfg cfg = analysis::buildCfg(function);
    bool changed = false;
    for (int i = 0; i < static_cast<int>(function.blocks.size()); ++i) {
        const ir::BasicBlock& candidate = function.blocks[static_cast<std::size_t>(i)];
        if (!candidate.instructions.empty()
            || !candidate.hasTerminator
            || candidate.terminator.kind != ir::TerminatorKind::Jump
            || candidate.terminator.trueBlock == i
            || cfg.predecessors[static_cast<std::size_t>(i)].empty()) {
            continue;
        }

        const int target = candidate.terminator.trueBlock;
        for (int pred : cfg.predecessors[static_cast<std::size_t>(i)]) {
            ir::Terminator& term = function.blocks[static_cast<std::size_t>(pred)].terminator;
            if ((term.kind == ir::TerminatorKind::Jump || term.kind == ir::TerminatorKind::Branch) && term.trueBlock == i) {
                term.trueBlock = target;
                changed = true;
            }
            if (term.kind == ir::TerminatorKind::Branch && term.falseBlock == i) {
                term.falseBlock = target;
                changed = true;
            }
        }
    }
    return changed;
}

class SimplifyCfgPass final : public Pass {
public:
    std::string name() const override { return "simplify-cfg"; }
    bool run(ir::Module& module) override
    {
        bool changed = false;
        for (ir::Function& function : module.functions) {
            bool localChanged = false;
            do {
                localChanged = false;
                localChanged = simplifyBranch(function) || localChanged;
                localChanged = redirectEmptyJumpBlocks(function) || localChanged;
                localChanged = removeUnreachableBlocks(function) || localChanged;
                changed = localChanged || changed;
            } while (localChanged);
        }
        return changed;
    }
};

} // namespace

std::unique_ptr<Pass> createSimplifyCfgPass()
{
    return std::make_unique<SimplifyCfgPass>();
}

} // namespace toyc::passes
