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
                    } else if (inst.dst.id >= 0) {
                        constants.erase(inst.dst.id);
                    }

                    const bool isAddSub = inst.kind == ir::InstructionKind::Binary
                        && inst.operands.size() == 2
                        && (inst.binaryOp == ir::BinaryOpcode::Add || inst.binaryOp == ir::BinaryOpcode::Sub);

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
