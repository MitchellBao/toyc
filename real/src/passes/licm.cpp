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

bool definesValue(const ir::Instruction& inst)
{
    return inst.dst.id >= 0
        && inst.kind != ir::InstructionKind::StoreGlobal
        && inst.kind != ir::InstructionKind::StoreLocal;
}

bool isHoistablePure(const ir::Instruction& inst)
{
    if (inst.hasSideEffect) {
        return false;
    }
    if (inst.kind == ir::InstructionKind::Binary
        && (inst.binaryOp == ir::BinaryOpcode::Div || inst.binaryOp == ir::BinaryOpcode::Mod)) {
        return definesValue(inst)
            && inst.operands.size() == 2
            && inst.operands[1].isImmediate
            && inst.operands[1].immediate != 0;
    }
    switch (inst.kind) {
    case ir::InstructionKind::Const:
    case ir::InstructionKind::Copy:
    case ir::InstructionKind::Unary:
    case ir::InstructionKind::Binary:
    case ir::InstructionKind::LoadGlobal:
    case ir::InstructionKind::LoadLocal:
        return definesValue(inst);
    case ir::InstructionKind::StoreGlobal:
    case ir::InstructionKind::StoreLocal:
    case ir::InstructionKind::Call:
        return false;
    }
    return false;
}

void markOperandUse(const ir::Operand& operand, std::unordered_set<int>& used)
{
    if (!operand.isImmediate && operand.value.id >= 0) {
        used.insert(operand.value.id);
    }
}

std::vector<int> operandValueIds(const ir::Instruction& inst)
{
    std::vector<int> values;
    for (const ir::Operand& operand : inst.operands) {
        if (!operand.isImmediate && operand.value.id >= 0) {
            values.push_back(operand.value.id);
        }
    }
    return values;
}

bool isWhileHeader(const ir::BasicBlock& block)
{
    return block.label.find(".while.cond") == 0;
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

std::unordered_map<int, std::unordered_set<int>> computeLoopDominators(
    const analysis::Cfg& cfg,
    const std::unordered_set<int>& loop,
    int header)
{
    std::unordered_map<int, std::unordered_set<int>> dominators;
    for (int block : loop) {
        auto& dom = dominators[block];
        if (block == header) {
            dom.insert(header);
        } else {
            dom.insert(loop.begin(), loop.end());
        }
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (int block : loop) {
            if (block == header) {
                continue;
            }

            std::unordered_set<int> next;
            bool hasLoopPred = false;
            for (int pred : cfg.predecessors[static_cast<std::size_t>(block)]) {
                if (loop.find(pred) == loop.end()) {
                    continue;
                }
                const auto predDom = dominators.find(pred);
                if (predDom == dominators.end()) {
                    continue;
                }
                if (!hasLoopPred) {
                    next = predDom->second;
                    hasLoopPred = true;
                } else {
                    for (auto iter = next.begin(); iter != next.end();) {
                        if (predDom->second.find(*iter) == predDom->second.end()) {
                            iter = next.erase(iter);
                        } else {
                            ++iter;
                        }
                    }
                }
            }
            next.insert(block);
            if (next != dominators[block]) {
                dominators[block] = std::move(next);
                changed = true;
            }
        }
    }

    return dominators;
}

std::unordered_set<int> collectLiveOutValues(const ir::Function& function, const std::unordered_set<int>& loop)
{
    std::unordered_set<int> liveOut;
    for (int i = 0; i < static_cast<int>(function.blocks.size()); ++i) {
        if (loop.find(i) != loop.end()) {
            continue;
        }
        const ir::BasicBlock& block = function.blocks[static_cast<std::size_t>(i)];
        for (const ir::Instruction& inst : block.instructions) {
            for (const ir::Operand& operand : inst.operands) {
                markOperandUse(operand, liveOut);
            }
        }
        markOperandUse(block.terminator.condition, liveOut);
        markOperandUse(block.terminator.returnValue, liveOut);
    }
    return liveOut;
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

bool runOnLoop(ir::Function& function, const analysis::Cfg& cfg, int header, int backedge)
{
    const std::unordered_set<int> loop = collectNaturalLoop(cfg, header, backedge);
    std::vector<int> outsidePredecessors;
    for (int pred : cfg.predecessors[static_cast<std::size_t>(header)]) {
        if (loop.find(pred) == loop.end()) {
            outsidePredecessors.push_back(pred);
        }
    }
    if (outsidePredecessors.size() != 1) {
        return false;
    }

    const int preheader = outsidePredecessors.front();
    if (preheader < 0 || preheader >= static_cast<int>(function.blocks.size())) {
        return false;
    }
    const ir::Terminator& preheaderTerm = function.blocks[static_cast<std::size_t>(preheader)].terminator;
    if (preheaderTerm.kind != ir::TerminatorKind::Jump || preheaderTerm.trueBlock != header) {
        return false;
    }
    std::unordered_map<int, int> defCount;
    std::unordered_set<std::string> storedLocals;
    std::unordered_set<std::string> storedGlobals;
    bool hasCall = false;
    for (int blockIndex : loop) {
        const ir::BasicBlock& block = function.blocks[static_cast<std::size_t>(blockIndex)];
        for (const ir::Instruction& inst : block.instructions) {
            if (definesValue(inst)) {
                ++defCount[inst.dst.id];
            }
            if (inst.kind == ir::InstructionKind::StoreGlobal && !inst.symbol.empty()) {
                storedGlobals.insert(inst.symbol);
            }
            if (inst.kind == ir::InstructionKind::StoreLocal && !inst.symbol.empty()) {
                storedLocals.insert(inst.symbol);
            }
            if (inst.kind == ir::InstructionKind::Call) {
                hasCall = true;
            }
        }
    }

    const std::unordered_set<int> liveOut = collectLiveOutValues(function, loop);
    if (hasCall) {
        return false;
    }

    std::unordered_set<int> invariantValues;
    std::vector<std::pair<int, std::size_t>> toHoist;
    const auto dominators = computeLoopDominators(cfg, loop, header);
    std::unordered_set<int> requiredBlocks;
    const auto backedgeDominators = dominators.find(backedge);
    if (backedgeDominators == dominators.end()) {
        return false;
    }
    requiredBlocks = backedgeDominators->second;

    std::vector<int> orderedBlocks(loop.begin(), loop.end());
    std::sort(orderedBlocks.begin(), orderedBlocks.end());
    for (int blockIndex : orderedBlocks) {
        if (blockIndex == header || requiredBlocks.find(blockIndex) == requiredBlocks.end()) {
            continue;
        }
        const ir::BasicBlock& block = function.blocks[static_cast<std::size_t>(blockIndex)];
        for (std::size_t instIndex = 0; instIndex < block.instructions.size(); ++instIndex) {
            const ir::Instruction& inst = block.instructions[instIndex];
            if (!isHoistablePure(inst)) {
                continue;
            }
            if (defCount[inst.dst.id] != 1 || liveOut.find(inst.dst.id) != liveOut.end()) {
                continue;
            }
            if (inst.kind == ir::InstructionKind::LoadLocal && storedLocals.find(inst.symbol) != storedLocals.end()) {
                continue;
            }
            if (inst.kind == ir::InstructionKind::LoadGlobal && (hasCall || storedGlobals.find(inst.symbol) != storedGlobals.end())) {
                continue;
            }

            bool operandsInvariant = true;
            for (int value : operandValueIds(inst)) {
                const auto found = defCount.find(value);
                const bool definedInLoop = found != defCount.end() && found->second > 0;
                if (definedInLoop && invariantValues.find(value) == invariantValues.end()) {
                    operandsInvariant = false;
                    break;
                }
            }
            if (!operandsInvariant) {
                continue;
            }

            toHoist.emplace_back(blockIndex, instIndex);
            invariantValues.insert(inst.dst.id);
        }
    }

    if (toHoist.empty()) {
        return false;
    }

    std::vector<ir::Instruction> hoisted;
    hoisted.reserve(toHoist.size());
    for (const auto& [blockIndex, instIndex] : toHoist) {
        hoisted.push_back(function.blocks[static_cast<std::size_t>(blockIndex)].instructions[instIndex]);
    }

    for (auto iter = toHoist.rbegin(); iter != toHoist.rend(); ++iter) {
        auto& instructions = function.blocks[static_cast<std::size_t>(iter->first)].instructions;
        instructions.erase(instructions.begin() + static_cast<std::ptrdiff_t>(iter->second));
    }

    auto& preheaderInstructions = function.blocks[static_cast<std::size_t>(preheader)].instructions;
    preheaderInstructions.insert(preheaderInstructions.end(), std::make_move_iterator(hoisted.begin()), std::make_move_iterator(hoisted.end()));
    return true;
}

class LicmPass final : public Pass {
public:
    std::string name() const override { return "licm"; }
    bool run(ir::Module& module) override
    {
        bool changed = false;
        bool localChanged = true;
        while (localChanged) {
            localChanged = false;
            for (ir::Function& function : module.functions) {
                if (functionContainsCall(function)) {
                    continue;
                }
                const analysis::Cfg cfg = analysis::buildCfg(function);
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
                if (localChanged) {
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
