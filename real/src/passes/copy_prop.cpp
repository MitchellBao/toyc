#include "pass_manager.h"

#include <unordered_map>

namespace toyc::passes {
namespace {

bool replaceOperand(ir::Operand& operand, const std::unordered_map<int, ir::Operand>& copies)
{
    if (operand.isImmediate) {
        return false;
    }
    ir::Operand resolved = operand;
    for (int depth = 0; depth < 16 && !resolved.isImmediate && resolved.value.id >= 0; ++depth) {
        const auto found = copies.find(resolved.value.id);
        if (found == copies.end()) {
            break;
        }
        resolved = found->second;
    }
    if ((resolved.isImmediate && operand.isImmediate)
        || (!resolved.isImmediate && !operand.isImmediate && resolved.value.id == operand.value.id)) {
        return false;
    }
    operand = resolved;
    return true;
}

ir::Operand resolveCopy(ir::Operand operand, const std::unordered_map<int, ir::Operand>& copies)
{
    for (int depth = 0; depth < 16 && !operand.isImmediate && operand.value.id >= 0; ++depth) {
        const auto found = copies.find(operand.value.id);
        if (found == copies.end()) {
            break;
        }
        operand = found->second;
    }
    return operand;
}

bool mayClobberLocals(const ir::Instruction& inst)
{
    return inst.kind == ir::InstructionKind::Call
        || inst.kind == ir::InstructionKind::StoreGlobal
        || inst.hasSideEffect;
}

class CopyPropPass final : public Pass {
public:
    std::string name() const override { return "copy-prop"; }

    bool run(ir::Module& module) override
    {
        bool changed = false;
        for (ir::Function& function : module.functions) {
            for (ir::BasicBlock& block : function.blocks) {
                std::unordered_map<int, ir::Operand> copies;
                std::unordered_map<std::string, ir::Operand> localValues;
                for (ir::Instruction& inst : block.instructions) {
                    for (ir::Operand& operand : inst.operands) {
                        changed = replaceOperand(operand, copies) || changed;
                    }

                    if (mayClobberLocals(inst)) {
                        localValues.clear();
                    }

                    if (inst.kind == ir::InstructionKind::LoadLocal && inst.dst.id >= 0) {
                        const auto found = localValues.find(inst.symbol);
                        if (found != localValues.end()) {
                            inst.kind = ir::InstructionKind::Copy;
                            inst.operands = {found->second};
                            inst.symbol.clear();
                            changed = true;
                        }
                    } else if (inst.kind == ir::InstructionKind::StoreLocal && !inst.symbol.empty() && !inst.operands.empty()) {
                        localValues[inst.symbol] = inst.operands[0];
                    }

                    if (inst.kind == ir::InstructionKind::Copy && inst.dst.id >= 0 && !inst.operands.empty()) {
                        copies[inst.dst.id] = resolveCopy(inst.operands[0], copies);
                    } else if (inst.dst.id >= 0) {
                        copies.erase(inst.dst.id);
                    }
                }
                changed = replaceOperand(block.terminator.condition, copies) || changed;
                changed = replaceOperand(block.terminator.returnValue, copies) || changed;
            }
        }
        return changed;
    }
};

} // namespace

std::unique_ptr<Pass> createCopyPropPass()
{
    return std::make_unique<CopyPropPass>();
}

} // namespace toyc::passes
