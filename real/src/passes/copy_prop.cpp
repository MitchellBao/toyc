#include "pass_manager.h"

#include <unordered_map>

namespace toyc::passes {
namespace {

bool replaceOperand(ir::Operand& operand, const std::unordered_map<int, ir::Operand>& copies)
{
    if (operand.isImmediate) {
        return false;
    }
    const auto found = copies.find(operand.value.id);
    if (found == copies.end()) {
        return false;
    }
    operand = found->second;
    return true;
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
                for (ir::Instruction& inst : block.instructions) {
                    for (ir::Operand& operand : inst.operands) {
                        changed = replaceOperand(operand, copies) || changed;
                    }
                    if (inst.kind == ir::InstructionKind::Copy && inst.dst.id >= 0 && !inst.operands.empty()) {
                        copies[inst.dst.id] = inst.operands[0];
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
