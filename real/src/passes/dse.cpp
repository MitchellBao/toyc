#include "pass_manager.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

namespace toyc::passes {
namespace {

std::vector<int> successors(const ir::BasicBlock& block)
{
    if (!block.hasTerminator) {
        return {};
    }
    switch (block.terminator.kind) {
    case ir::TerminatorKind::Jump:
        return block.terminator.trueBlock >= 0 ? std::vector<int>{block.terminator.trueBlock} : std::vector<int>{};
    case ir::TerminatorKind::Branch: {
        std::vector<int> result;
        if (block.terminator.trueBlock >= 0) {
            result.push_back(block.terminator.trueBlock);
        }
        if (block.terminator.falseBlock >= 0 && block.terminator.falseBlock != block.terminator.trueBlock) {
            result.push_back(block.terminator.falseBlock);
        }
        return result;
    }
    case ir::TerminatorKind::Return:
        return {};
    }
    return {};
}

void addAll(std::unordered_set<std::string>& target, const std::unordered_set<std::string>& source)
{
    for (const std::string& symbol : source) {
        target.insert(symbol);
    }
}

std::unordered_set<std::string> computeLiveIn(const ir::BasicBlock& block, std::unordered_set<std::string> live)
{
    for (auto it = block.instructions.rbegin(); it != block.instructions.rend(); ++it) {
        const ir::Instruction& inst = *it;
        if (inst.kind == ir::InstructionKind::StoreLocal && !inst.symbol.empty()) {
            live.erase(inst.symbol);
        } else if (inst.kind == ir::InstructionKind::LoadLocal && !inst.symbol.empty()) {
            live.insert(inst.symbol);
        }
    }
    return live;
}

std::vector<std::unordered_set<std::string>> computeLiveOut(const ir::Function& function)
{
    const std::size_t blockCount = function.blocks.size();
    std::vector<std::unordered_set<std::string>> liveIn(blockCount);
    std::vector<std::unordered_set<std::string>> liveOut(blockCount);

    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t reverse = 0; reverse < blockCount; ++reverse) {
            const std::size_t index = blockCount - reverse - 1;
            std::unordered_set<std::string> nextLiveOut;
            for (int succ : successors(function.blocks[index])) {
                if (succ >= 0 && static_cast<std::size_t>(succ) < blockCount) {
                    addAll(nextLiveOut, liveIn[static_cast<std::size_t>(succ)]);
                }
            }

            std::unordered_set<std::string> nextLiveIn = computeLiveIn(function.blocks[index], nextLiveOut);
            if (nextLiveOut != liveOut[index] || nextLiveIn != liveIn[index]) {
                liveOut[index] = std::move(nextLiveOut);
                liveIn[index] = std::move(nextLiveIn);
                changed = true;
            }
        }
    }
    return liveOut;
}

class DsePass final : public Pass {
public:
    std::string name() const override { return "dse"; }
    bool run(ir::Module& module) override
    {
        bool changed = false;
        for (ir::Function& function : module.functions) {
            const std::vector<std::unordered_set<std::string>> liveOut = computeLiveOut(function);
            for (std::size_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
                ir::BasicBlock& block = function.blocks[blockIndex];
                std::vector<bool> remove(block.instructions.size(), false);
                std::unordered_set<std::string> live = liveOut[blockIndex];
                for (std::size_t reverse = 0; reverse < block.instructions.size(); ++reverse) {
                    const std::size_t i = block.instructions.size() - reverse - 1;
                    const ir::Instruction& inst = block.instructions[i];
                    if (inst.kind == ir::InstructionKind::LoadLocal && !inst.symbol.empty()) {
                        live.insert(inst.symbol);
                    } else if (inst.kind == ir::InstructionKind::StoreLocal && !inst.symbol.empty()) {
                        if (live.find(inst.symbol) == live.end()) {
                            remove[i] = true;
                        } else {
                            live.erase(inst.symbol);
                        }
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
