#include "pass_manager.h"

#include "analysis/cfg.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace toyc::passes {
namespace {

std::optional<std::int32_t> evalUnary(ir::UnaryOpcode op, std::int32_t value)
{
    switch (op) {
    case ir::UnaryOpcode::Plus:
        return value;
    case ir::UnaryOpcode::Minus:
        return -value;
    case ir::UnaryOpcode::Not:
        return value == 0 ? 1 : 0;
    }
    return std::nullopt;
}

std::optional<std::int32_t> evalBinary(ir::BinaryOpcode op, std::int32_t lhs, std::int32_t rhs)
{
    switch (op) {
    case ir::BinaryOpcode::Add:
        return lhs + rhs;
    case ir::BinaryOpcode::Sub:
        return lhs - rhs;
    case ir::BinaryOpcode::Mul:
        return lhs * rhs;
    case ir::BinaryOpcode::Div:
        return rhs == 0 ? std::nullopt : std::optional<std::int32_t>{lhs / rhs};
    case ir::BinaryOpcode::Mod:
        return rhs == 0 ? std::nullopt : std::optional<std::int32_t>{lhs % rhs};
    case ir::BinaryOpcode::Equal:
        return lhs == rhs ? 1 : 0;
    case ir::BinaryOpcode::NotEqual:
        return lhs != rhs ? 1 : 0;
    case ir::BinaryOpcode::Less:
        return lhs < rhs ? 1 : 0;
    case ir::BinaryOpcode::LessEqual:
        return lhs <= rhs ? 1 : 0;
    case ir::BinaryOpcode::Greater:
        return lhs > rhs ? 1 : 0;
    case ir::BinaryOpcode::GreaterEqual:
        return lhs >= rhs ? 1 : 0;
    case ir::BinaryOpcode::LogicalAnd:
        return (lhs != 0 && rhs != 0) ? 1 : 0;
    case ir::BinaryOpcode::LogicalOr:
        return (lhs != 0 || rhs != 0) ? 1 : 0;
    }
    return std::nullopt;
}

std::optional<std::int32_t> knownOperand(
    const ir::Operand& operand,
    const std::unordered_map<int, std::int32_t>& constants)
{
    if (operand.isImmediate) {
        return operand.immediate;
    }
    const auto found = constants.find(operand.value.id);
    if (found == constants.end()) {
        return std::nullopt;
    }
    return found->second;
}

void updateKnownConstants(
    std::unordered_map<int, std::int32_t>& constants,
    const ir::Instruction& inst)
{
    if (inst.dst.id < 0) {
        return;
    }

    if (inst.kind == ir::InstructionKind::Const && !inst.operands.empty() && inst.operands[0].isImmediate) {
        constants[inst.dst.id] = inst.operands[0].immediate;
        return;
    }

    if (inst.kind == ir::InstructionKind::Copy && inst.operands.size() == 1) {
        if (const auto value = knownOperand(inst.operands[0], constants); value.has_value()) {
            constants[inst.dst.id] = *value;
        } else {
            constants.erase(inst.dst.id);
        }
        return;
    }

    if (inst.kind == ir::InstructionKind::Unary && inst.operands.size() == 1) {
        const auto value = knownOperand(inst.operands[0], constants);
        const auto folded = value.has_value() ? evalUnary(inst.unaryOp, *value) : std::nullopt;
        if (folded.has_value()) {
            constants[inst.dst.id] = *folded;
        } else {
            constants.erase(inst.dst.id);
        }
        return;
    }

    if (inst.kind == ir::InstructionKind::Binary && inst.operands.size() == 2) {
        const auto lhs = knownOperand(inst.operands[0], constants);
        const auto rhs = knownOperand(inst.operands[1], constants);
        const auto folded = lhs.has_value() && rhs.has_value() ? evalBinary(inst.binaryOp, *lhs, *rhs) : std::nullopt;
        if (folded.has_value()) {
            constants[inst.dst.id] = *folded;
        } else {
            constants.erase(inst.dst.id);
        }
        return;
    }

    constants.erase(inst.dst.id);
}

bool simplifyBranch(ir::Function& function)
{
    bool changed = false;
    for (ir::BasicBlock& block : function.blocks) {
        std::unordered_map<int, std::int32_t> constants;
        for (const ir::Instruction& inst : block.instructions) {
            updateKnownConstants(constants, inst);
        }

        if (!block.hasTerminator || block.terminator.kind != ir::TerminatorKind::Branch) {
            continue;
        }
        ir::Terminator& term = block.terminator;
        if (!term.condition.isImmediate) {
            if (const auto known = knownOperand(term.condition, constants); known.has_value()) {
                term.condition = ir::Operand::imm(*known);
            }
        }
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

bool mergeJumpToNextBlocks(ir::Function& function)
{
    bool changed = false;
    while (true) {
        const analysis::Cfg cfg = analysis::buildCfg(function);
        int mergeIndex = -1;
        for (int i = 0; i + 1 < static_cast<int>(function.blocks.size()); ++i) {
            const ir::BasicBlock& block = function.blocks[static_cast<std::size_t>(i)];
            const int next = i + 1;
            if (!block.hasTerminator
                || block.terminator.kind != ir::TerminatorKind::Jump
                || block.terminator.trueBlock != next
                || cfg.predecessors[static_cast<std::size_t>(next)].size() != 1
                || cfg.predecessors[static_cast<std::size_t>(next)].front() != i) {
                continue;
            }
            mergeIndex = i;
            break;
        }

        if (mergeIndex < 0) {
            break;
        }

        const int removed = mergeIndex + 1;
        ir::BasicBlock& block = function.blocks[static_cast<std::size_t>(mergeIndex)];
        ir::BasicBlock& nextBlock = function.blocks[static_cast<std::size_t>(removed)];
        block.instructions.insert(
            block.instructions.end(),
            std::make_move_iterator(nextBlock.instructions.begin()),
            std::make_move_iterator(nextBlock.instructions.end()));
        block.terminator = nextBlock.terminator;
        block.hasTerminator = nextBlock.hasTerminator;

        function.blocks.erase(function.blocks.begin() + removed);
        for (ir::BasicBlock& current : function.blocks) {
            auto remap = [removed](int& target) {
                if (target > removed) {
                    --target;
                }
            };
            if (current.terminator.kind == ir::TerminatorKind::Jump || current.terminator.kind == ir::TerminatorKind::Branch) {
                remap(current.terminator.trueBlock);
            }
            if (current.terminator.kind == ir::TerminatorKind::Branch) {
                remap(current.terminator.falseBlock);
            }
        }
        changed = true;
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
                localChanged = mergeJumpToNextBlocks(function) || localChanged;
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
