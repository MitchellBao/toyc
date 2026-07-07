#include "pass_manager.h"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

namespace toyc::passes {
namespace {

std::unordered_set<std::string> collectLoadedLocals(const ir::Function& function)
{
    std::unordered_set<std::string> loaded;
    for (const ir::BasicBlock& block : function.blocks) {
        for (const ir::Instruction& inst : block.instructions) {
            if (inst.kind == ir::InstructionKind::LoadLocal && !inst.symbol.empty()) {
                loaded.insert(inst.symbol);
            }
        }
    }
    return loaded;
}

bool isOverwrittenBeforeRead(const ir::BasicBlock& block, std::size_t index)
{
    const std::string& symbol = block.instructions[index].symbol;
    for (std::size_t cursor = index + 1; cursor < block.instructions.size(); ++cursor) {
        const ir::Instruction& inst = block.instructions[cursor];
        if (inst.symbol != symbol) {
            continue;
        }
        if (inst.kind == ir::InstructionKind::LoadLocal) {
            return false;
        }
        if (inst.kind == ir::InstructionKind::StoreLocal) {
            return true;
        }
    }
    return false;
}

class DsePass final : public Pass {
public:
    std::string name() const override { return "dse"; }
    bool run(ir::Module& module) override
    {
        bool changed = false;
        for (ir::Function& function : module.functions) {
            const std::unordered_set<std::string> loadedLocals = collectLoadedLocals(function);
            for (ir::BasicBlock& block : function.blocks) {
                std::vector<bool> remove(block.instructions.size(), false);
                for (std::size_t i = 0; i < block.instructions.size(); ++i) {
                    const ir::Instruction& inst = block.instructions[i];
                    if (inst.kind != ir::InstructionKind::StoreLocal || inst.symbol.empty()) {
                        continue;
                    }
                    if (loadedLocals.find(inst.symbol) == loadedLocals.end() || isOverwrittenBeforeRead(block, i)) {
                        remove[i] = true;
                    }
                }

                if (std::any_of(remove.begin(), remove.end(), [](bool value) { return value; })) {
                    std::vector<ir::Instruction> kept;
                    kept.reserve(block.instructions.size());
                    for (std::size_t i = 0; i < block.instructions.size(); ++i) {
                        if (!remove[i]) {
                            kept.push_back(std::move(block.instructions[i]));
                        }
                    }
                    block.instructions = std::move(kept);
                    changed = true;
                }
            }
        }
        return changed;
    }
};

} // namespace

std::unique_ptr<Pass> createDsePass()
{
    return std::make_unique<DsePass>();
}

} // namespace toyc::passes
