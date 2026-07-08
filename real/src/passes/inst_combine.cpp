#include "pass_manager.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>

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
                    } else if (inst.dst.id >= 0) {
                        constants.erase(inst.dst.id);
                        addConstExprs.erase(inst.dst.id);
                    }

                    const bool isAddSub = inst.kind == ir::InstructionKind::Binary
                        && inst.operands.size() == 2
                        && (inst.binaryOp == ir::BinaryOpcode::Add || inst.binaryOp == ir::BinaryOpcode::Sub);

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
