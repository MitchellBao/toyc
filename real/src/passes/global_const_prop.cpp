#include "pass_manager.h"

#include "analysis/cfg.h"

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace toyc::passes {
namespace {

using ConstMap = std::unordered_map<int, std::int32_t>;

std::int32_t evalUnary(ir::UnaryOpcode op, std::int32_t value)
{
    switch (op) {
    case ir::UnaryOpcode::Plus:
        return value;
    case ir::UnaryOpcode::Minus:
        return -value;
    case ir::UnaryOpcode::Not:
        return value == 0 ? 1 : 0;
    }
    return value;
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

std::optional<std::int32_t> knownOperand(const ir::Operand& operand, const ConstMap& constants)
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

bool replaceOperand(ir::Operand& operand, const ConstMap& constants)
{
    if (operand.isImmediate) {
        return false;
    }
    const auto found = constants.find(operand.value.id);
    if (found == constants.end()) {
        return false;
    }
    operand = ir::Operand::imm(found->second);
    return true;
}

void transferInstruction(ConstMap& constants, const ir::Instruction& inst)
{
    if (inst.kind == ir::InstructionKind::Const && inst.dst.id >= 0 && !inst.operands.empty() && inst.operands[0].isImmediate) {
        constants[inst.dst.id] = inst.operands[0].immediate;
    } else if (inst.kind == ir::InstructionKind::Copy && inst.dst.id >= 0 && !inst.operands.empty()) {
        if (const auto value = knownOperand(inst.operands[0], constants); value.has_value()) {
            constants[inst.dst.id] = *value;
        } else {
            constants.erase(inst.dst.id);
        }
    } else if (inst.kind == ir::InstructionKind::Unary && inst.dst.id >= 0 && inst.operands.size() == 1) {
        if (const auto value = knownOperand(inst.operands[0], constants); value.has_value()) {
            constants[inst.dst.id] = evalUnary(inst.unaryOp, *value);
        } else {
            constants.erase(inst.dst.id);
        }
    } else if (inst.kind == ir::InstructionKind::Binary && inst.dst.id >= 0 && inst.operands.size() == 2) {
        const auto lhs = knownOperand(inst.operands[0], constants);
        const auto rhs = knownOperand(inst.operands[1], constants);
        if (lhs.has_value() && rhs.has_value()) {
            if (const auto value = evalBinary(inst.binaryOp, *lhs, *rhs); value.has_value()) {
                constants[inst.dst.id] = *value;
            } else {
                constants.erase(inst.dst.id);
            }
        } else {
            constants.erase(inst.dst.id);
        }
    } else if (inst.dst.id >= 0) {
        constants.erase(inst.dst.id);
    }
}

ConstMap transferBlock(ConstMap constants, const ir::BasicBlock& block)
{
    for (const ir::Instruction& inst : block.instructions) {
        transferInstruction(constants, inst);
    }
    return constants;
}

ConstMap intersectConstants(const ConstMap& lhs, const ConstMap& rhs)
{
    ConstMap result = lhs;
    for (auto iter = result.begin(); iter != result.end();) {
        const auto found = rhs.find(iter->first);
        if (found == rhs.end() || found->second != iter->second) {
            iter = result.erase(iter);
        } else {
            ++iter;
        }
    }
    return result;
}

std::vector<int> executableSuccessors(const ir::BasicBlock& block, const ConstMap& constants)
{
    if (!block.hasTerminator) {
        return {};
    }
    const ir::Terminator& term = block.terminator;
    if (term.kind == ir::TerminatorKind::Jump) {
        return {term.trueBlock};
    }
    if (term.kind == ir::TerminatorKind::Branch) {
        const auto condition = knownOperand(term.condition, constants);
        if (condition.has_value()) {
            return {*condition != 0 ? term.trueBlock : term.falseBlock};
        }
        return {term.trueBlock, term.falseBlock};
    }
    return {};
}

bool rewriteBlock(ir::BasicBlock& block, ConstMap constants)
{
    bool changed = false;
    for (ir::Instruction& inst : block.instructions) {
        for (ir::Operand& operand : inst.operands) {
            changed = replaceOperand(operand, constants) || changed;
        }

        if (inst.kind == ir::InstructionKind::Unary && inst.dst.id >= 0 && inst.operands.size() == 1) {
            if (const auto value = knownOperand(inst.operands[0], constants); value.has_value()) {
                inst.kind = ir::InstructionKind::Const;
                inst.operands = {ir::Operand::imm(evalUnary(inst.unaryOp, *value))};
                constants[inst.dst.id] = inst.operands[0].immediate;
                changed = true;
                continue;
            }
        }

        if (inst.kind == ir::InstructionKind::Binary && inst.dst.id >= 0 && inst.operands.size() == 2) {
            const auto lhs = knownOperand(inst.operands[0], constants);
            const auto rhs = knownOperand(inst.operands[1], constants);
            if (lhs.has_value() && rhs.has_value()) {
                if (const auto value = evalBinary(inst.binaryOp, *lhs, *rhs); value.has_value()) {
                    inst.kind = ir::InstructionKind::Const;
                    inst.operands = {ir::Operand::imm(*value)};
                    constants[inst.dst.id] = *value;
                    changed = true;
                    continue;
                }
            }
        }

        transferInstruction(constants, inst);
    }
    changed = replaceOperand(block.terminator.condition, constants) || changed;
    changed = replaceOperand(block.terminator.returnValue, constants) || changed;
    return changed;
}

bool runOnFunction(ir::Function& function)
{
    if (function.blocks.empty()) {
        return false;
    }

    const analysis::Cfg cfg = analysis::buildCfg(function);
    std::vector<ConstMap> in(function.blocks.size());
    std::vector<ConstMap> out(function.blocks.size());
    std::vector<bool> executable(function.blocks.size(), false);
    executable[0] = true;

    bool dataChanged = true;
    for (int iteration = 0; dataChanged && iteration < 64; ++iteration) {
        dataChanged = false;
        for (int b = 0; b < static_cast<int>(function.blocks.size()); ++b) {
            if (!executable[static_cast<std::size_t>(b)]) {
                continue;
            }
            ConstMap nextIn;
            const auto& preds = cfg.predecessors[static_cast<std::size_t>(b)];
            if (b != 0) {
                bool haveExecutablePred = false;
                for (int pred : preds) {
                    if (pred < 0
                        || pred >= static_cast<int>(function.blocks.size())
                        || !executable[static_cast<std::size_t>(pred)]) {
                        continue;
                    }
                    if (!haveExecutablePred) {
                        nextIn = out[static_cast<std::size_t>(pred)];
                        haveExecutablePred = true;
                    } else {
                        nextIn = intersectConstants(nextIn, out[static_cast<std::size_t>(pred)]);
                    }
                }
            }

            ConstMap nextOut = transferBlock(nextIn, function.blocks[static_cast<std::size_t>(b)]);
            for (int succ : executableSuccessors(function.blocks[static_cast<std::size_t>(b)], nextOut)) {
                if (succ >= 0
                    && succ < static_cast<int>(function.blocks.size())
                    && !executable[static_cast<std::size_t>(succ)]) {
                    executable[static_cast<std::size_t>(succ)] = true;
                    dataChanged = true;
                }
            }
            if (nextIn != in[static_cast<std::size_t>(b)] || nextOut != out[static_cast<std::size_t>(b)]) {
                in[static_cast<std::size_t>(b)] = std::move(nextIn);
                out[static_cast<std::size_t>(b)] = std::move(nextOut);
                dataChanged = true;
            }
        }
    }

    bool changed = false;
    for (int b = 0; b < static_cast<int>(function.blocks.size()); ++b) {
        changed = rewriteBlock(function.blocks[static_cast<std::size_t>(b)], in[static_cast<std::size_t>(b)]) || changed;
    }
    return changed;
}

class GlobalConstPropPass final : public Pass {
public:
    std::string name() const override { return "global-const-prop"; }

    bool run(ir::Module& module) override
    {
        bool changed = false;
        for (ir::Function& function : module.functions) {
            changed = runOnFunction(function) || changed;
        }
        return changed;
    }
};

} // namespace

std::unique_ptr<Pass> createGlobalConstPropPass()
{
    return std::make_unique<GlobalConstPropPass>();
}

} // namespace toyc::passes
