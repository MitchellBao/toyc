#include "pass_manager.h"

#include <optional>
#include <unordered_map>

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

void replaceWithUnary(ir::Instruction& inst, ir::UnaryOpcode op, ir::Operand operand)
{
    inst.kind = ir::InstructionKind::Unary;
    inst.unaryOp = op;
    inst.operands = {operand};
    inst.symbol.clear();
    inst.hasSideEffect = false;
}

void replaceWithNotZero(ir::Instruction& inst, ir::Operand operand)
{
    inst.kind = ir::InstructionKind::Binary;
    inst.binaryOp = ir::BinaryOpcode::NotEqual;
    inst.operands = {operand, ir::Operand::imm(0)};
    inst.symbol.clear();
    inst.hasSideEffect = false;
}

bool isCompareOp(ir::BinaryOpcode op)
{
    switch (op) {
    case ir::BinaryOpcode::Equal:
    case ir::BinaryOpcode::NotEqual:
    case ir::BinaryOpcode::Less:
    case ir::BinaryOpcode::LessEqual:
    case ir::BinaryOpcode::Greater:
    case ir::BinaryOpcode::GreaterEqual:
        return true;
    default:
        return false;
    }
}

ir::BinaryOpcode invertCompareOp(ir::BinaryOpcode op)
{
    switch (op) {
    case ir::BinaryOpcode::Equal:
        return ir::BinaryOpcode::NotEqual;
    case ir::BinaryOpcode::NotEqual:
        return ir::BinaryOpcode::Equal;
    case ir::BinaryOpcode::Less:
        return ir::BinaryOpcode::GreaterEqual;
    case ir::BinaryOpcode::LessEqual:
        return ir::BinaryOpcode::Greater;
    case ir::BinaryOpcode::Greater:
        return ir::BinaryOpcode::LessEqual;
    case ir::BinaryOpcode::GreaterEqual:
        return ir::BinaryOpcode::Less;
    default:
        return op;
    }
}

struct CompareExpr {
    ir::BinaryOpcode op = ir::BinaryOpcode::Equal;
    ir::Operand lhs;
    ir::Operand rhs;
};

void replaceWithCompare(ir::Instruction& inst, const CompareExpr& expr, bool invert)
{
    inst.kind = ir::InstructionKind::Binary;
    inst.binaryOp = invert ? invertCompareOp(expr.op) : expr.op;
    inst.operands = {expr.lhs, expr.rhs};
    inst.symbol.clear();
    inst.hasSideEffect = false;
}

std::optional<CompareExpr> compareExprOf(
    const ir::Operand& operand,
    const std::unordered_map<int, CompareExpr>& compares)
{
    if (operand.isImmediate) {
        return std::nullopt;
    }
    const auto found = compares.find(operand.value.id);
    if (found == compares.end()) {
        return std::nullopt;
    }
    return found->second;
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
        if (samePureOperand(lhs, rhs)) {
            replaceWithConst(inst, 0);
            return true;
        }
        if (isImm(lhs, 0)) {
            replaceWithUnary(inst, ir::UnaryOpcode::Minus, rhs);
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
            replaceWithNotZero(inst, rhs);
            return true;
        }
        if (isImm(rhs, 1)) {
            replaceWithNotZero(inst, lhs);
            return true;
        }
        return false;
    case ir::BinaryOpcode::LogicalOr:
        if (isImm(lhs, 1) || isImm(rhs, 1)) {
            replaceWithConst(inst, 1);
            return true;
        }
        if (isImm(lhs, 0)) {
            replaceWithNotZero(inst, rhs);
            return true;
        }
        if (isImm(rhs, 0)) {
            replaceWithNotZero(inst, lhs);
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

bool simplifyCompareUse(ir::Instruction& inst, const std::unordered_map<int, CompareExpr>& compares)
{
    if (inst.kind == ir::InstructionKind::Unary && inst.unaryOp == ir::UnaryOpcode::Not && inst.operands.size() == 1) {
        if (const auto expr = compareExprOf(inst.operands[0], compares); expr.has_value()) {
            replaceWithCompare(inst, *expr, true);
            return true;
        }
        return false;
    }

    if (inst.kind != ir::InstructionKind::Binary || inst.operands.size() != 2) {
        return false;
    }
    if (inst.binaryOp != ir::BinaryOpcode::Equal && inst.binaryOp != ir::BinaryOpcode::NotEqual) {
        return false;
    }

    std::optional<CompareExpr> expr;
    bool compareWithOne = false;
    if (isImm(inst.operands[1], 0)) {
        expr = compareExprOf(inst.operands[0], compares);
    } else if (isImm(inst.operands[0], 0)) {
        expr = compareExprOf(inst.operands[1], compares);
    } else if (isImm(inst.operands[1], 1)) {
        expr = compareExprOf(inst.operands[0], compares);
        compareWithOne = true;
    } else if (isImm(inst.operands[0], 1)) {
        expr = compareExprOf(inst.operands[1], compares);
        compareWithOne = true;
    }
    if (!expr.has_value()) {
        return false;
    }

    const bool invert = compareWithOne
        ? inst.binaryOp == ir::BinaryOpcode::NotEqual
        : inst.binaryOp == ir::BinaryOpcode::Equal;
    replaceWithCompare(inst, *expr, invert);
    return true;
}

class AlgebraicSimplifyPass final : public Pass {
public:
    std::string name() const override { return "algebraic-simplify"; }
    bool run(ir::Module& module) override
    {
        bool changed = false;
        for (ir::Function& function : module.functions) {
            for (ir::BasicBlock& block : function.blocks) {
                std::unordered_map<int, CompareExpr> compares;
                for (ir::Instruction& inst : block.instructions) {
                    if (inst.dst.id >= 0) {
                        compares.erase(inst.dst.id);
                    }

                    changed = simplifyCompareUse(inst, compares) || changed;
                    changed = simplifyBinary(inst) || changed;

                    if (inst.dst.id >= 0
                        && inst.kind == ir::InstructionKind::Binary
                        && inst.operands.size() == 2
                        && isCompareOp(inst.binaryOp)) {
                        compares[inst.dst.id] = CompareExpr{inst.binaryOp, inst.operands[0], inst.operands[1]};
                    }
                    if (inst.kind == ir::InstructionKind::Call || inst.kind == ir::InstructionKind::StoreGlobal || inst.hasSideEffect) {
                        compares.clear();
                    }
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
