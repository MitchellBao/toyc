#include "pass.h"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>

namespace toyc {
namespace {

class DeadInstructionPass final : public IrPass {
public:
    std::string name() const override
    {
        return "dead-instruction";
    }

    bool run(ir::Module& module) override
    {
        bool changed = false;
        for (ir::Function& function : module.functions) {
            std::unordered_map<int, int> uses;
            countUses(function, uses);
            for (ir::BasicBlock& block : function.blocks) {
                const auto oldSize = block.instructions.size();
                block.instructions.erase(
                    std::remove_if(block.instructions.begin(), block.instructions.end(), [&](const ir::Instruction& instruction) {
                        return removable(instruction) && uses[instruction.dst.id] == 0;
                    }),
                    block.instructions.end());
                changed = changed || oldSize != block.instructions.size();
            }
        }
        return changed;
    }

private:
    void countOperand(const ir::Operand& operand, std::unordered_map<int, int>& uses) const
    {
        if (!operand.isImmediate && operand.value.id >= 0) {
            ++uses[operand.value.id];
        }
    }

    void countUses(const ir::Function& function, std::unordered_map<int, int>& uses) const
    {
        for (const ir::BasicBlock& block : function.blocks) {
            for (const ir::Instruction& instruction : block.instructions) {
                for (const ir::Operand& operand : instruction.operands) {
                    countOperand(operand, uses);
                }
            }
            if (block.hasTerminator) {
                countOperand(block.terminator.condition, uses);
                countOperand(block.terminator.returnValue, uses);
            }
        }
    }

    bool removable(const ir::Instruction& instruction) const
    {
        if (instruction.dst.id < 0 || instruction.hasSideEffect) {
            return false;
        }
        switch (instruction.kind) {
        case ir::InstructionKind::Const:
        case ir::InstructionKind::Copy:
        case ir::InstructionKind::Unary:
        case ir::InstructionKind::Binary:
        case ir::InstructionKind::LoadGlobal:
        case ir::InstructionKind::LoadLocal:
            return true;
        case ir::InstructionKind::StoreGlobal:
        case ir::InstructionKind::StoreLocal:
        case ir::InstructionKind::Call:
            return false;
        }
        return false;
    }
};

} // namespace

std::unique_ptr<IrPass> createDeadInstructionPass()
{
    return std::make_unique<DeadInstructionPass>();
}

} // namespace toyc
