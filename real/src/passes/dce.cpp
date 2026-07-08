#include "pass_manager.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

bool isRoot(const ir::Instruction& inst)
{
    return !isPure(inst) || inst.dst.id < 0;
}

class DcePass final : public Pass {
public:
    std::string name() const override { return "dce"; }

    bool run(ir::Module& module) override
    {
        bool changed = false;
        for (ir::Function& function : module.functions) {
            std::unordered_map<int, const ir::Instruction*> definitions;
            for (const ir::BasicBlock& block : function.blocks) {
                for (const ir::Instruction& inst : block.instructions) {
                    if (inst.dst.id >= 0) {
                        definitions[inst.dst.id] = &inst;
                    }
                }
            }

            std::unordered_set<int> liveValues;
            std::vector<int> worklist;
            auto markLive = [&](const ir::Operand& operand) {
                if (!operand.isImmediate && operand.value.id >= 0 && liveValues.insert(operand.value.id).second) {
                    worklist.push_back(operand.value.id);
                }
            };

            for (const ir::BasicBlock& block : function.blocks) {
                for (const ir::Instruction& inst : block.instructions) {
                    if (isRoot(inst)) {
                        for (const ir::Operand& operand : inst.operands) {
                            markLive(operand);
                        }
                    }
                }
                markLive(block.terminator.condition);
                markLive(block.terminator.returnValue);
            }

            while (!worklist.empty()) {
                const int value = worklist.back();
                worklist.pop_back();
                const auto definition = definitions.find(value);
                if (definition == definitions.end()) {
                    continue;
                }
                for (const ir::Operand& operand : definition->second->operands) {
                    markLive(operand);
                }
            }

            for (ir::BasicBlock& block : function.blocks) {
                const auto oldSize = block.instructions.size();
                block.instructions.erase(
                    std::remove_if(block.instructions.begin(), block.instructions.end(), [&](const ir::Instruction& inst) {
                        return inst.dst.id >= 0 && liveValues.find(inst.dst.id) == liveValues.end() && isPure(inst);
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
