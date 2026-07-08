#include "pass_manager.h"

#include <algorithm>
#include <string>
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

bool isPure(const ir::Instruction& inst, const std::unordered_set<std::string>& pureFunctions)
{
    if (inst.kind == ir::InstructionKind::Call) {
        return pureFunctions.find(inst.symbol) != pureFunctions.end();
    }
    return !inst.hasSideEffect
        && inst.kind != ir::InstructionKind::StoreGlobal
        && inst.kind != ir::InstructionKind::StoreLocal
        && inst.kind != ir::InstructionKind::Call;
}

bool isRoot(const ir::Instruction& inst, const std::unordered_set<std::string>& pureFunctions)
{
    return !isPure(inst, pureFunctions);
}

std::unordered_set<std::string> computePureFunctions(const ir::Module& module)
{
    std::unordered_map<std::string, const ir::Function*> byName;
    for (const ir::Function& function : module.functions) {
        byName[function.name] = &function;
    }

    std::unordered_set<std::string> pure;
    for (const ir::Function& function : module.functions) {
        pure.insert(function.name);
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (const ir::Function& function : module.functions) {
            if (pure.find(function.name) == pure.end()) {
                continue;
            }
            bool functionPure = true;
            for (const ir::BasicBlock& block : function.blocks) {
                for (const ir::Instruction& inst : block.instructions) {
                    if (inst.kind == ir::InstructionKind::Call) {
                        if (byName.find(inst.symbol) == byName.end() || pure.find(inst.symbol) == pure.end()) {
                            functionPure = false;
                            break;
                        }
                        continue;
                    }
                    if (inst.hasSideEffect || inst.kind == ir::InstructionKind::StoreGlobal) {
                        functionPure = false;
                        break;
                    }
                }
                if (!functionPure) {
                    break;
                }
            }
            if (!functionPure) {
                pure.erase(function.name);
                changed = true;
            }
        }
    }
    return pure;
}

class DcePass final : public Pass {
public:
    std::string name() const override { return "dce"; }

    bool run(ir::Module& module) override
    {
        bool changed = false;
        const std::unordered_set<std::string> pureFunctions = computePureFunctions(module);
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
                    if (isRoot(inst, pureFunctions)) {
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
                        return isPure(inst, pureFunctions)
                            && (inst.dst.id < 0 || liveValues.find(inst.dst.id) == liveValues.end());
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
