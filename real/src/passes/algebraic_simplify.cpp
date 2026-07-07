#include "pass_manager.h"

namespace toyc::passes {
namespace {

bool isImm(const ir::Operand& operand, std::int32_t value)
{
    return operand.isImmediate && operand.immediate == value;
}

bool samePureOperand(const ir::Operand& lhs, const ir::Operand& rhs)
{
    if (lhs.isImmediate || rhs.isImmediate) {
        return lhs.isImmediate && rhs.isImmediate && lhs.immediate == rhs.immediate;
    }
    return lhs.value.id >= 0 && lhs.value.id == rhs.value.id;
}

void replaceWithCopy(ir::Instruction& inst, ir::Operand operand)
{
    inst.kind = ir::InstructionKind::Copy;
    inst.operands = {operand};
    inst.symbol.clear();
    inst.hasSideEffect = false;
}

void replaceWithConst(ir::Instruction& inst, std::int32_t value)
{
    inst.kind = ir::InstructionKind::Const;
    inst.operands = {ir::Operand::imm(value)};
    inst.symbol.clear();
    inst.hasSideEffect = false;
}

bool simplifyBinary(ir::Instruction& inst)
{
    if (inst.kind != ir::InstructionKind::Binary || inst.operands.size() != 2) {
        return false;
    }

    const ir::Operand lhs = inst.operands[0];
    const ir::Operand rhs = inst.operands[1];
    switch (inst.binaryOp) {
    case ir::BinaryOpcode::Add:
        if (isImm(rhs, 0)) {
            replaceWithCopy(inst, lhs);
            return true;
        }
        if (isImm(lhs, 0)) {
            replaceWithCopy(inst, rhs);
            return true;
        }
        return false;
    case ir::BinaryOpcode::Sub:
        if (isImm(rhs, 0)) {
            replaceWithCopy(inst, lhs);
            return true;
        }
        return false;
    case ir::BinaryOpcode::Mul:
        if (isImm(rhs, 1)) {
            replaceWithCopy(inst, lhs);
            return true;
        }
        if (isImm(lhs, 1)) {
            replaceWithCopy(inst, rhs);
            return true;
        }
        if (isImm(lhs, 0) || isImm(rhs, 0)) {
            replaceWithConst(inst, 0);
            return true;
        }
        return false;
    case ir::BinaryOpcode::Div:
        if (isImm(rhs, 1)) {
            replaceWithCopy(inst, lhs);
            return true;
        }
        return false;
    case ir::BinaryOpcode::Mod:
        if (isImm(rhs, 1)) {
            replaceWithConst(inst, 0);
            return true;
        }
        return false;
    case ir::BinaryOpcode::LogicalAnd:
        if (isImm(lhs, 0) || isImm(rhs, 0)) {
            replaceWithConst(inst, 0);
            return true;
        }
        if (isImm(lhs, 1)) {
            replaceWithCopy(inst, rhs);
            return true;
        }
        if (isImm(rhs, 1)) {
            replaceWithCopy(inst, lhs);
            return true;
        }
        return false;
    case ir::BinaryOpcode::LogicalOr:
        if (isImm(lhs, 1) || isImm(rhs, 1)) {
            replaceWithConst(inst, 1);
            return true;
        }
        if (isImm(lhs, 0)) {
            replaceWithCopy(inst, rhs);
            return true;
        }
        if (isImm(rhs, 0)) {
            replaceWithCopy(inst, lhs);
            return true;
        }
        return false;
    case ir::BinaryOpcode::Equal:
        if (samePureOperand(lhs, rhs)) {
            replaceWithConst(inst, 1);
            return true;
        }
        return false;
    case ir::BinaryOpcode::NotEqual:
        if (samePureOperand(lhs, rhs)) {
            replaceWithConst(inst, 0);
            return true;
        }
        return false;
    case ir::BinaryOpcode::Less:
        if (samePureOperand(lhs, rhs)) {
            replaceWithConst(inst, 0);
            return true;
        }
        return false;
    case ir::BinaryOpcode::LessEqual:
        if (samePureOperand(lhs, rhs)) {
            replaceWithConst(inst, 1);
            return true;
        }
        return false;
    case ir::BinaryOpcode::Greater:
        if (samePureOperand(lhs, rhs)) {
            replaceWithConst(inst, 0);
            return true;
        }
        return false;
    case ir::BinaryOpcode::GreaterEqual:
        if (samePureOperand(lhs, rhs)) {
            replaceWithConst(inst, 1);
            return true;
        }
        return false;
    }
    return false;
}

class AlgebraicSimplifyPass final : public Pass {
public:
    std::string name() const override { return "algebraic-simplify"; }
    bool run(ir::Module& module) override
    {
        bool changed = false;
        for (ir::Function& function : module.functions) {
            for (ir::BasicBlock& block : function.blocks) {
                for (ir::Instruction& inst : block.instructions) {
                    changed = simplifyBinary(inst) || changed;
                }
            }
        }
        return changed;
    }
};

} // namespace

std::unique_ptr<Pass> createAlgebraicSimplifyPass()
{
    return std::make_unique<AlgebraicSimplifyPass>();
}

} // namespace toyc::passes
