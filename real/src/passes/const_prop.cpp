#include "pass_manager.h"

#include <cstdint>
#include <optional>
#include <unordered_map>

namespace toyc::passes {
namespace {

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

std::optional<std::int32_t> knownOperand(const ir::Operand& operand, const std::unordered_map<int, std::int32_t>& constants)
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

bool replaceOperand(ir::Operand& operand, const std::unordered_map<int, std::int32_t>& constants)
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

class ConstPropPass final : public Pass {
public:
    std::string name() const override { return "const-prop"; }

    bool run(ir::Module& module) override
    {
        bool changed = false;
        for (ir::Function& function : module.functions) {
            for (ir::BasicBlock& block : function.blocks) {
                std::unordered_map<int, std::int32_t> constants;
                for (ir::Instruction& inst : block.instructions) {
                    for (ir::Operand& operand : inst.operands) {
                        changed = replaceOperand(operand, constants) || changed;
                    }

                    if (inst.kind == ir::InstructionKind::Unary && inst.operands.size() == 1) {
                        if (const auto value = knownOperand(inst.operands[0], constants); value.has_value()) {
                            inst.kind = ir::InstructionKind::Const;
                            inst.operands = {ir::Operand::imm(evalUnary(inst.unaryOp, *value))};
                            constants[inst.dst.id] = inst.operands[0].immediate;
                            changed = true;
                            continue;
                        }
                    }

                    if (inst.kind == ir::InstructionKind::Binary && inst.operands.size() == 2) {
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

                    if (inst.kind == ir::InstructionKind::Const && inst.dst.id >= 0 && !inst.operands.empty() && inst.operands[0].isImmediate) {
                        constants[inst.dst.id] = inst.operands[0].immediate;
                    } else if (inst.kind == ir::InstructionKind::Copy && inst.dst.id >= 0 && !inst.operands.empty()) {
                        if (const auto value = knownOperand(inst.operands[0], constants); value.has_value()) {
                            constants[inst.dst.id] = *value;
                        } else {
                            constants.erase(inst.dst.id);
                        }
                    } else if (inst.dst.id >= 0) {
                        constants.erase(inst.dst.id);
                    }
                }
                changed = replaceOperand(block.terminator.condition, constants) || changed;
                changed = replaceOperand(block.terminator.returnValue, constants) || changed;
            }
        }
        return changed;
    }
};

} // namespace

std::unique_ptr<Pass> createConstPropPass()
{
    return std::make_unique<ConstPropPass>();
}

} // namespace toyc::passes
