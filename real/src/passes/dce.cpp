#include "pass_manager.h"

#include <algorithm>
#include <unordered_set>

namespace toyc::passes {
namespace {

void markOperand(const ir::Operand& operand, std::unordered_set<int>& used)
{
    if (!operand.isImmediate && operand.value.id >= 0) {
        used.insert(operand.value.id);
    }
}

bool isPure(const ir::Instruction& inst)
{
    return !inst.hasSideEffect
        && inst.kind != ir::InstructionKind::StoreGlobal
        && inst.kind != ir::InstructionKind::StoreLocal
        && inst.kind != ir::InstructionKind::Call;
}

class DcePass final : public Pass {
public:
    std::string name() const override { return "dce"; }

    bool run(ir::Module& module) override
    {
        bool changed = false;
        for (ir::Function& function : module.functions) {
            std::unordered_set<int> used;
            for (const ir::BasicBlock& block : function.blocks) {
                for (const ir::Instruction& inst : block.instructions) {
                    for (const ir::Operand& operand : inst.operands) {
                        markOperand(operand, used);
                    }
                }
                markOperand(block.terminator.condition, used);
                markOperand(block.terminator.returnValue, used);
            }
            for (ir::BasicBlock& block : function.blocks) {
                const auto oldSize = block.instructions.size();
                block.instructions.erase(
                    std::remove_if(block.instructions.begin(), block.instructions.end(), [&](const ir::Instruction& inst) {
                        return inst.dst.id >= 0 && used.find(inst.dst.id) == used.end() && isPure(inst);
                    }),
                    block.instructions.end());
                changed = changed || block.instructions.size() != oldSize;
            }
        }
        return changed;
    }
};

} // namespace

std::unique_ptr<Pass> createDcePass()
{
    return std::make_unique<DcePass>();
}

} // namespace toyc::passes
