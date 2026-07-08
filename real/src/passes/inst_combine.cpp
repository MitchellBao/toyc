#include "pass_manager.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace toyc::passes {
namespace {

bool definesValue(const ir::Instruction& inst)
{
    return inst.dst.id >= 0
        && inst.kind != ir::InstructionKind::StoreGlobal
        && inst.kind != ir::InstructionKind::StoreLocal;
}

void countOperandUse(const ir::Operand& operand, std::unordered_map<int, int>& uses)
{
    if (!operand.isImmediate && operand.value.id >= 0) {
        ++uses[operand.value.id];
    }
}

std::unordered_map<int, int> computeUseCount(const ir::Function& function)
{
    std::unordered_map<int, int> uses;
    for (const ir::BasicBlock& block : function.blocks) {
        for (const ir::Instruction& inst : block.instructions) {
            for (const ir::Operand& operand : inst.operands) {
                countOperandUse(operand, uses);
            }
        }
        countOperandUse(block.terminator.condition, uses);
        countOperandUse(block.terminator.returnValue, uses);
    }
    return uses;
}

std::optional<std::int32_t> addConst(std::int32_t lhs, std::int32_t rhs)
{
    const std::int64_t value = static_cast<std::int64_t>(lhs) + rhs;
    if (value < INT32_MIN || value > INT32_MAX) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(value);
}

std::optional<std::int32_t> subConst(std::int32_t lhs, std::int32_t rhs)
{
    const std::int64_t value = static_cast<std::int64_t>(lhs) - rhs;
    if (value < INT32_MIN || value > INT32_MAX) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(value);
}

std::optional<std::int32_t> operandConst(
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

void rewriteAsCopy(ir::Instruction& inst, ir::Value value)
{
    inst.kind = ir::InstructionKind::Copy;
    inst.operands = {ir::Operand::ref(value)};
    inst.symbol.clear();
    inst.binaryOp = ir::BinaryOpcode::Add;
    inst.hasSideEffect = false;
}

struct AddConstExpr {
    ir::Operand base;
    std::int32_t constant = 0;
};

struct AffineExpr {
    ir::Operand base;
    std::int32_t scale = 1;
    std::int32_t constant = 0;
};

struct LinearTerm {
    ir::Operand base;
    std::int32_t coeff = 0;
};

struct LinearExpr {
    std::vector<LinearTerm> terms;
    std::int32_t constant = 0;
};

constexpr std::size_t kMaxLinearTerms = 6;

std::optional<AddConstExpr> addConstExprOf(
    const ir::Operand& operand,
    const std::unordered_map<int, AddConstExpr>& addConstExprs)
{
    if (operand.isImmediate) {
        return AddConstExpr{ir::Operand::imm(0), operand.immediate};
    }
    const auto found = addConstExprs.find(operand.value.id);
    if (found == addConstExprs.end()) {
        return AddConstExpr{operand, 0};
    }
    return found->second;
}

bool sameOperand(const ir::Operand& lhs, const ir::Operand& rhs)
{
    if (lhs.isImmediate != rhs.isImmediate) {
        return false;
    }
    return lhs.isImmediate ? lhs.immediate == rhs.immediate : lhs.value.id == rhs.value.id;
}

void rewriteAddConst(ir::Instruction& inst, const AddConstExpr& expr)
{
    if (expr.constant == 0) {
        inst.kind = ir::InstructionKind::Copy;
        inst.operands = {expr.base};
    } else {
        inst.kind = ir::InstructionKind::Binary;
        inst.binaryOp = ir::BinaryOpcode::Add;
        inst.operands = {expr.base, ir::Operand::imm(expr.constant)};
    }
    inst.symbol.clear();
    inst.hasSideEffect = false;
}

std::optional<AffineExpr> affineExprOf(
    const ir::Operand& operand,
    const std::unordered_map<int, AffineExpr>& affineExprs)
{
    if (operand.isImmediate) {
        return AffineExpr{ir::Operand::imm(0), 0, operand.immediate};
    }
    const auto found = affineExprs.find(operand.value.id);
    if (found == affineExprs.end()) {
        return AffineExpr{operand, 1, 0};
    }
    return found->second;
}

std::optional<std::int32_t> mulConst(std::int32_t lhs, std::int32_t rhs)
{
    const std::int64_t value = static_cast<std::int64_t>(lhs) * rhs;
    if (value < INT32_MIN || value > INT32_MAX) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(value);
}

void sortLinearTerms(LinearExpr& expr)
{
    std::sort(expr.terms.begin(), expr.terms.end(), [](const LinearTerm& lhs, const LinearTerm& rhs) {
        return lhs.base.value.id < rhs.base.value.id;
    });
}

std::optional<LinearExpr> normalizeLinear(LinearExpr expr)
{
    sortLinearTerms(expr);
    std::vector<LinearTerm> merged;
    merged.reserve(expr.terms.size());
    for (const LinearTerm& term : expr.terms) {
        if (term.base.isImmediate || term.coeff == 0) {
            continue;
        }
        if (!merged.empty() && sameOperand(merged.back().base, term.base)) {
            const auto coeff = addConst(merged.back().coeff, term.coeff);
            if (!coeff.has_value()) {
                return std::nullopt;
            }
            merged.back().coeff = *coeff;
            if (merged.back().coeff == 0) {
                merged.pop_back();
            }
        } else {
            merged.push_back(term);
        }
    }
    if (merged.size() > kMaxLinearTerms) {
        return std::nullopt;
    }
    expr.terms = std::move(merged);
    return expr;
}

std::optional<LinearExpr> linearExprOf(
    const ir::Operand& operand,
    const std::unordered_map<int, LinearExpr>& linearExprs)
{
    if (operand.isImmediate) {
        return LinearExpr{{}, operand.immediate};
    }
    const auto found = linearExprs.find(operand.value.id);
    if (found != linearExprs.end()) {
        return found->second;
    }
    return LinearExpr{{LinearTerm{operand, 1}}, 0};
}

std::optional<LinearExpr> combineLinear(ir::BinaryOpcode op, const LinearExpr& lhs, const LinearExpr& rhs)
{
    LinearExpr result;
    result.terms = lhs.terms;
    result.terms.reserve(lhs.terms.size() + rhs.terms.size());
    for (LinearTerm term : rhs.terms) {
        if (op == ir::BinaryOpcode::Sub) {
            const auto coeff = mulConst(term.coeff, -1);
            if (!coeff.has_value()) {
                return std::nullopt;
            }
            term.coeff = *coeff;
        }
        result.terms.push_back(term);
    }
    const auto constant = op == ir::BinaryOpcode::Add
        ? addConst(lhs.constant, rhs.constant)
        : subConst(lhs.constant, rhs.constant);
    if (!constant.has_value()) {
        return std::nullopt;
    }
    result.constant = *constant;
    return normalizeLinear(std::move(result));
}

std::optional<LinearExpr> mulLinearByConst(const LinearExpr& expr, std::int32_t factor)
{
    LinearExpr result;
    result.terms.reserve(expr.terms.size());
    for (const LinearTerm& term : expr.terms) {
        const auto coeff = mulConst(term.coeff, factor);
        if (!coeff.has_value()) {
            return std::nullopt;
        }
        result.terms.push_back(LinearTerm{term.base, *coeff});
    }
    const auto constant = mulConst(expr.constant, factor);
    if (!constant.has_value()) {
        return std::nullopt;
    }
    result.constant = *constant;
    return normalizeLinear(std::move(result));
}

bool rewriteLinear(ir::Instruction& inst, const LinearExpr& expr)
{
    if (expr.terms.empty()) {
        inst.kind = ir::InstructionKind::Const;
        inst.operands = {ir::Operand::imm(expr.constant)};
        inst.symbol.clear();
        inst.hasSideEffect = false;
        return true;
    }
    if (expr.terms.size() != 1) {
        return false;
    }

    const LinearTerm& term = expr.terms.front();
    if (term.coeff == 1) {
        rewriteAddConst(inst, AddConstExpr{term.base, expr.constant});
        return true;
    }
    if (expr.constant == 0) {
        if (term.coeff == -1) {
            inst.kind = ir::InstructionKind::Unary;
            inst.unaryOp = ir::UnaryOpcode::Minus;
            inst.operands = {term.base};
            inst.symbol.clear();
            inst.hasSideEffect = false;
            return true;
        }
        inst.kind = ir::InstructionKind::Binary;
        inst.binaryOp = ir::BinaryOpcode::Mul;
        inst.operands = {term.base, ir::Operand::imm(term.coeff)};
        inst.symbol.clear();
        inst.hasSideEffect = false;
        return true;
    }
    return false;
}

bool sameLinearShapeAsInstruction(const ir::Instruction& inst, const LinearExpr& expr)
{
    if (inst.kind == ir::InstructionKind::Const) {
        return expr.terms.empty()
            && inst.operands.size() == 1
            && inst.operands[0].isImmediate
            && inst.operands[0].immediate == expr.constant;
    }
    if (inst.kind == ir::InstructionKind::Copy) {
        return expr.constant == 0
            && expr.terms.size() == 1
            && expr.terms.front().coeff == 1
            && inst.operands.size() == 1
            && sameOperand(inst.operands[0], expr.terms.front().base);
    }
    if (inst.kind == ir::InstructionKind::Unary && inst.unaryOp == ir::UnaryOpcode::Minus) {
        return expr.constant == 0
            && expr.terms.size() == 1
            && expr.terms.front().coeff == -1
            && inst.operands.size() == 1
            && sameOperand(inst.operands[0], expr.terms.front().base);
    }
    if (inst.kind == ir::InstructionKind::Binary && inst.operands.size() == 2) {
        if (inst.binaryOp == ir::BinaryOpcode::Add
            && expr.terms.size() == 1
            && expr.terms.front().coeff == 1
            && expr.constant != 0
            && sameOperand(inst.operands[0], expr.terms.front().base)
            && inst.operands[1].isImmediate
            && inst.operands[1].immediate == expr.constant) {
            return true;
        }
        if (inst.binaryOp == ir::BinaryOpcode::Mul
            && expr.constant == 0
            && expr.terms.size() == 1
            && sameOperand(inst.operands[0], expr.terms.front().base)
            && inst.operands[1].isImmediate
            && inst.operands[1].immediate == expr.terms.front().coeff) {
            return true;
        }
    }
    return false;
}

std::optional<AffineExpr> combineAffine(ir::BinaryOpcode op, const AffineExpr& lhs, const AffineExpr& rhs)
{
    if (lhs.base.isImmediate && rhs.base.isImmediate) {
        if (op == ir::BinaryOpcode::Add) {
            if (const auto constant = addConst(lhs.constant, rhs.constant); constant.has_value()) {
                return AffineExpr{ir::Operand::imm(0), 0, *constant};
            }
        }
        if (op == ir::BinaryOpcode::Sub) {
            if (const auto constant = subConst(lhs.constant, rhs.constant); constant.has_value()) {
                return AffineExpr{ir::Operand::imm(0), 0, *constant};
            }
        }
        return std::nullopt;
    }
    if (lhs.base.isImmediate) {
        if (op != ir::BinaryOpcode::Add) {
            return std::nullopt;
        }
        if (const auto constant = addConst(lhs.constant, rhs.constant); constant.has_value()) {
            return AffineExpr{rhs.base, rhs.scale, *constant};
        }
        return std::nullopt;
    }
    if (rhs.base.isImmediate) {
        const auto constant = op == ir::BinaryOpcode::Add
            ? addConst(lhs.constant, rhs.constant)
            : subConst(lhs.constant, rhs.constant);
        if (constant.has_value()) {
            return AffineExpr{lhs.base, lhs.scale, *constant};
        }
        return std::nullopt;
    }
    if (!sameOperand(lhs.base, rhs.base)) {
        return std::nullopt;
    }

    const auto scale = op == ir::BinaryOpcode::Add
        ? addConst(lhs.scale, rhs.scale)
        : subConst(lhs.scale, rhs.scale);
    const auto constant = op == ir::BinaryOpcode::Add
        ? addConst(lhs.constant, rhs.constant)
        : subConst(lhs.constant, rhs.constant);
    if (!scale.has_value() || !constant.has_value()) {
        return std::nullopt;
    }
    return AffineExpr{lhs.base, *scale, *constant};
}

std::optional<AffineExpr> mulAffineByConst(const AffineExpr& expr, std::int32_t factor)
{
    const auto scale = mulConst(expr.scale, factor);
    const auto constant = mulConst(expr.constant, factor);
    if (!scale.has_value() || !constant.has_value()) {
        return std::nullopt;
    }
    return AffineExpr{expr.base, *scale, *constant};
}

bool rewriteAffine(ir::Instruction& inst, const AffineExpr& expr)
{
    if (expr.base.isImmediate || expr.scale == 0) {
        inst.kind = ir::InstructionKind::Const;
        inst.operands = {ir::Operand::imm(expr.constant)};
        inst.symbol.clear();
        inst.hasSideEffect = false;
        return true;
    }
    if (expr.scale == 1) {
        rewriteAddConst(inst, AddConstExpr{expr.base, expr.constant});
        return true;
    }
    if (expr.constant == 0) {
        inst.kind = ir::InstructionKind::Binary;
        inst.binaryOp = ir::BinaryOpcode::Mul;
        inst.operands = {expr.base, ir::Operand::imm(expr.scale)};
        inst.symbol.clear();
        inst.hasSideEffect = false;
        return true;
    }
    return false;
}

class InstCombinePass final : public Pass {
public:
    std::string name() const override { return "inst-combine"; }

    bool run(ir::Module& module) override
    {
        bool changed = false;
        for (ir::Function& function : module.functions) {
            const std::unordered_map<int, int> uses = computeUseCount(function);
            for (ir::BasicBlock& block : function.blocks) {
                std::unordered_map<int, std::int32_t> constants;
                std::unordered_map<int, AddConstExpr> addConstExprs;
                std::unordered_map<int, AffineExpr> affineExprs;
                std::unordered_map<int, LinearExpr> linearExprs;
                int chainTip = -1;
                ir::Instruction* chainStart = nullptr;
                std::int32_t chainDelta = 0;

                auto resetChain = [&]() {
                    chainTip = -1;
                    chainStart = nullptr;
                    chainDelta = 0;
                };

                for (ir::Instruction& inst : block.instructions) {
                    if (inst.kind == ir::InstructionKind::Const
                        && inst.dst.id >= 0
                        && !inst.operands.empty()
                        && inst.operands[0].isImmediate) {
                        constants[inst.dst.id] = inst.operands[0].immediate;
                        addConstExprs.erase(inst.dst.id);
                        affineExprs.erase(inst.dst.id);
                        linearExprs[inst.dst.id] = LinearExpr{{}, inst.operands[0].immediate};
                    } else if (inst.dst.id >= 0) {
                        constants.erase(inst.dst.id);
                        addConstExprs.erase(inst.dst.id);
                        affineExprs.erase(inst.dst.id);
                        linearExprs.erase(inst.dst.id);
                    }

                    const bool isAddSub = inst.kind == ir::InstructionKind::Binary
                        && inst.operands.size() == 2
                        && (inst.binaryOp == ir::BinaryOpcode::Add || inst.binaryOp == ir::BinaryOpcode::Sub);

                    const bool isAffineBinary = inst.kind == ir::InstructionKind::Binary
                        && inst.operands.size() == 2
                        && (inst.binaryOp == ir::BinaryOpcode::Add
                            || inst.binaryOp == ir::BinaryOpcode::Sub
                            || inst.binaryOp == ir::BinaryOpcode::Mul);

                    if (isAffineBinary && inst.dst.id >= 0) {
                        std::optional<LinearExpr> linear;
                        if (inst.binaryOp == ir::BinaryOpcode::Add || inst.binaryOp == ir::BinaryOpcode::Sub) {
                            const auto lhs = linearExprOf(inst.operands[0], linearExprs);
                            const auto rhs = linearExprOf(inst.operands[1], linearExprs);
                            if (lhs.has_value() && rhs.has_value()) {
                                linear = combineLinear(inst.binaryOp, *lhs, *rhs);
                            }
                        } else if (inst.binaryOp == ir::BinaryOpcode::Mul) {
                            const auto rhs = operandConst(inst.operands[1], constants);
                            if (rhs.has_value()) {
                                const auto lhs = linearExprOf(inst.operands[0], linearExprs);
                                if (lhs.has_value()) {
                                    linear = mulLinearByConst(*lhs, *rhs);
                                }
                            } else if (const auto lhs = operandConst(inst.operands[0], constants); lhs.has_value()) {
                                const auto rhsExpr = linearExprOf(inst.operands[1], linearExprs);
                                if (rhsExpr.has_value()) {
                                    linear = mulLinearByConst(*rhsExpr, *lhs);
                                }
                            }
                        }
                        if (linear.has_value()) {
                            linearExprs[inst.dst.id] = *linear;
                            if (!sameLinearShapeAsInstruction(inst, *linear) && rewriteLinear(inst, *linear)) {
                                if (inst.kind == ir::InstructionKind::Const && !inst.operands.empty()) {
                                    constants[inst.dst.id] = inst.operands[0].immediate;
                                    addConstExprs.erase(inst.dst.id);
                                    affineExprs.erase(inst.dst.id);
                                } else if (inst.kind == ir::InstructionKind::Copy) {
                                    constants.erase(inst.dst.id);
                                    affineExprs.erase(inst.dst.id);
                                    if (!inst.operands.empty()) {
                                        addConstExprs[inst.dst.id] = AddConstExpr{inst.operands[0], 0};
                                    }
                                } else if (inst.kind == ir::InstructionKind::Binary && inst.binaryOp == ir::BinaryOpcode::Add && inst.operands.size() == 2 && inst.operands[1].isImmediate) {
                                    constants.erase(inst.dst.id);
                                    affineExprs.erase(inst.dst.id);
                                    addConstExprs[inst.dst.id] = AddConstExpr{inst.operands[0], inst.operands[1].immediate};
                                } else {
                                    constants.erase(inst.dst.id);
                                    addConstExprs.erase(inst.dst.id);
                                    affineExprs.erase(inst.dst.id);
                                }
                                changed = true;
                                continue;
                            }
                        }

                        std::optional<AffineExpr> affine;
                        if (inst.binaryOp == ir::BinaryOpcode::Add || inst.binaryOp == ir::BinaryOpcode::Sub) {
                            const auto lhs = affineExprOf(inst.operands[0], affineExprs);
                            const auto rhs = affineExprOf(inst.operands[1], affineExprs);
                            if (lhs.has_value() && rhs.has_value()) {
                                affine = combineAffine(inst.binaryOp, *lhs, *rhs);
                            }
                        } else if (inst.binaryOp == ir::BinaryOpcode::Mul) {
                            const auto rhs = operandConst(inst.operands[1], constants);
                            if (rhs.has_value()) {
                                const auto lhs = affineExprOf(inst.operands[0], affineExprs);
                                if (lhs.has_value()) {
                                    affine = mulAffineByConst(*lhs, *rhs);
                                }
                            } else if (const auto lhs = operandConst(inst.operands[0], constants); lhs.has_value()) {
                                const auto rhsExpr = affineExprOf(inst.operands[1], affineExprs);
                                if (rhsExpr.has_value()) {
                                    affine = mulAffineByConst(*rhsExpr, *lhs);
                                }
                            }
                        }
                        if (affine.has_value()) {
                            affineExprs[inst.dst.id] = *affine;
                            const bool shouldRewrite =
                                affine->base.isImmediate
                                || affine->scale == 0
                                || (affine->scale == 1 && (inst.binaryOp != ir::BinaryOpcode::Add || !inst.operands[1].isImmediate || inst.operands[1].immediate != affine->constant))
                                || (affine->constant == 0 && affine->scale != 1 && inst.binaryOp != ir::BinaryOpcode::Mul);
                            if (shouldRewrite && rewriteAffine(inst, *affine)) {
                                if (inst.kind == ir::InstructionKind::Const && !inst.operands.empty()) {
                                    constants[inst.dst.id] = inst.operands[0].immediate;
                                    addConstExprs.erase(inst.dst.id);
                                } else if (inst.kind == ir::InstructionKind::Copy) {
                                    constants.erase(inst.dst.id);
                                    if (!inst.operands.empty()) {
                                        addConstExprs[inst.dst.id] = AddConstExpr{inst.operands[0], 0};
                                    }
                                } else if (inst.kind == ir::InstructionKind::Binary && inst.binaryOp == ir::BinaryOpcode::Add && inst.operands.size() == 2 && inst.operands[1].isImmediate) {
                                    addConstExprs[inst.dst.id] = AddConstExpr{inst.operands[0], inst.operands[1].immediate};
                                }
                                linearExprs.erase(inst.dst.id);
                                changed = true;
                                continue;
                            }
                        }
                    }

                    if (isAddSub && inst.dst.id >= 0) {
                        const auto lhsExpr = addConstExprOf(inst.operands[0], addConstExprs);
                        const auto rhsConst = operandConst(inst.operands[1], constants);
                        if (lhsExpr.has_value() && rhsConst.has_value() && !lhsExpr->base.isImmediate) {
                            const auto combined = inst.binaryOp == ir::BinaryOpcode::Add
                                ? addConst(lhsExpr->constant, *rhsConst)
                                : subConst(lhsExpr->constant, *rhsConst);
                            if (combined.has_value()) {
                                AddConstExpr next{lhsExpr->base, *combined};
                                if (!sameOperand(inst.operands[0], next.base)
                                    || inst.binaryOp != ir::BinaryOpcode::Add
                                    || !inst.operands[1].isImmediate
                                    || inst.operands[1].immediate != next.constant) {
                                    rewriteAddConst(inst, next);
                                    changed = true;
                                }
                                addConstExprs[inst.dst.id] = next;
                                continue;
                            }
                        }
                        if (inst.binaryOp == ir::BinaryOpcode::Sub && lhsExpr.has_value()) {
                            const auto rhsExpr = addConstExprOf(inst.operands[1], addConstExprs);
                            if (rhsExpr.has_value()
                                && !lhsExpr->base.isImmediate
                                && sameOperand(lhsExpr->base, rhsExpr->base)) {
                                if (const auto diff = subConst(lhsExpr->constant, rhsExpr->constant); diff.has_value()) {
                                    inst.kind = ir::InstructionKind::Const;
                                    inst.operands = {ir::Operand::imm(*diff)};
                                    inst.symbol.clear();
                                    inst.hasSideEffect = false;
                                    constants[inst.dst.id] = *diff;
                                    addConstExprs.erase(inst.dst.id);
                                    affineExprs.erase(inst.dst.id);
                                    linearExprs[inst.dst.id] = LinearExpr{{}, *diff};
                                    changed = true;
                                    continue;
                                }
                            }
                        }
                    }

                    if (isAddSub
                        && chainTip >= 0
                        && !inst.operands[0].isImmediate
                        && inst.operands[0].value.id == chainTip
                        && inst.dst.id >= 0
                        && inst.dst.id != chainTip) {
                        const auto tipUses = uses.find(chainTip);
                        if (tipUses == uses.end() || tipUses->second != 1) {
                            resetChain();
                        } else if (const auto rhs = operandConst(inst.operands[1], constants); rhs.has_value() && chainStart != nullptr) {
                            const std::int32_t delta = inst.binaryOp == ir::BinaryOpcode::Add ? *rhs : -*rhs;
                            const auto nextDelta = addConst(chainDelta, delta);
                            if (nextDelta.has_value()) {
                                chainDelta = *nextDelta;
                                chainStart->operands[1] = ir::Operand::imm(chainDelta);
                                rewriteAsCopy(inst, ir::Value{chainTip});
                                constants.erase(inst.dst.id);
                                addConstExprs.erase(inst.dst.id);
                                affineExprs.erase(inst.dst.id);
                                linearExprs.erase(inst.dst.id);
                                chainTip = inst.dst.id;
                                changed = true;
                                continue;
                            }
                            resetChain();
                        } else {
                            resetChain();
                        }
                    }

                    if (isAddSub
                        && inst.dst.id >= 0
                        && !inst.operands[0].isImmediate
                        && inst.dst.id != inst.operands[0].value.id) {
                        if (const auto rhs = operandConst(inst.operands[1], constants); rhs.has_value()) {
                            chainStart = &inst;
                            chainTip = inst.dst.id;
                            chainDelta = inst.binaryOp == ir::BinaryOpcode::Add ? *rhs : -*rhs;
                            addConstExprs[inst.dst.id] = AddConstExpr{inst.operands[0], chainDelta};
                            if (inst.binaryOp == ir::BinaryOpcode::Sub) {
                                inst.binaryOp = ir::BinaryOpcode::Add;
                                inst.operands[1] = ir::Operand::imm(chainDelta);
                                changed = true;
                            }
                            continue;
                        }
                    }

                    if (definesValue(inst) && inst.dst.id == chainTip) {
                        resetChain();
                    }
                    if (inst.kind == ir::InstructionKind::Call || inst.kind == ir::InstructionKind::StoreGlobal || inst.hasSideEffect) {
                        constants.clear();
                        addConstExprs.clear();
                        affineExprs.clear();
                        linearExprs.clear();
                        resetChain();
                    }
                }
            }
        }
        return changed;
    }
};

} // namespace

std::unique_ptr<Pass> createInstCombinePass()
{
    return std::make_unique<InstCombinePass>();
}

} // namespace toyc::passes
