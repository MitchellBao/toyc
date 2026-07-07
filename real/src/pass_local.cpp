#include "pass.h"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace toyc {
namespace {

using LocalSet = std::unordered_set<std::string>;
using LocalValueMap = std::unordered_map<std::string, ir::Operand>;

bool sameOperand(const ir::Operand& lhs, const ir::Operand& rhs)
{
    if (lhs.isImmediate != rhs.isImmediate) {
        return false;
    }
    if (lhs.isImmediate) {
        return lhs.immediate == rhs.immediate;
    }
    return lhs.value.id == rhs.value.id;
}

void makeConst(ir::Instruction& instruction, std::int32_t value)
{
    instruction.kind = ir::InstructionKind::Const;
    instruction.operands.clear();
    instruction.operands.push_back(ir::Operand::imm(value));
    instruction.symbol.clear();
    instruction.hasSideEffect = false;
}

void makeCopy(ir::Instruction& instruction, ir::Operand operand)
{
    instruction.kind = ir::InstructionKind::Copy;
    instruction.operands.clear();
    instruction.operands.push_back(operand);
    instruction.symbol.clear();
    instruction.hasSideEffect = false;
}

std::vector<std::vector<int>> buildSuccessors(const ir::Function& function)
{
    std::vector<std::vector<int>> successors(function.blocks.size());
    for (std::size_t index = 0; index < function.blocks.size(); ++index) {
        const ir::BasicBlock& block = function.blocks[index];
        if (!block.hasTerminator) {
            continue;
        }
        switch (block.terminator.kind) {
        case ir::TerminatorKind::Jump:
            if (block.terminator.trueBlock >= 0) {
                successors[index].push_back(block.terminator.trueBlock);
            }
            break;
        case ir::TerminatorKind::Branch:
            if (block.terminator.trueBlock >= 0) {
                successors[index].push_back(block.terminator.trueBlock);
            }
            if (block.terminator.falseBlock >= 0 && block.terminator.falseBlock != block.terminator.trueBlock) {
                successors[index].push_back(block.terminator.falseBlock);
            }
            break;
        case ir::TerminatorKind::Return:
            break;
        }
    }
    return successors;
}

std::vector<std::vector<int>> buildPredecessors(const ir::Function& function, const std::vector<std::vector<int>>& successors)
{
    std::vector<std::vector<int>> predecessors(function.blocks.size());
    for (std::size_t block = 0; block < successors.size(); ++block) {
        for (int successor : successors[block]) {
            if (successor >= 0 && successor < static_cast<int>(function.blocks.size())) {
                predecessors[successor].push_back(static_cast<int>(block));
            }
        }
    }
    return predecessors;
}

bool sameSet(const LocalSet& lhs, const LocalSet& rhs)
{
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (const std::string& value : lhs) {
        if (!rhs.contains(value)) {
            return false;
        }
    }
    return true;
}

LocalSet transferLive(LocalSet live, const ir::BasicBlock& block)
{
    for (auto iter = block.instructions.rbegin(); iter != block.instructions.rend(); ++iter) {
        const ir::Instruction& instruction = *iter;
        switch (instruction.kind) {
        case ir::InstructionKind::LoadLocal:
            live.insert(instruction.symbol);
            break;
        case ir::InstructionKind::StoreLocal:
            live.erase(instruction.symbol);
            break;
        default:
            break;
        }
    }
    return live;
}

LocalValueMap mergeLocalValues(const std::vector<int>& predecessors, const std::vector<LocalValueMap>& out)
{
    LocalValueMap merged;
    if (predecessors.empty()) {
        return merged;
    }

    merged = out[static_cast<std::size_t>(predecessors.front())];
    for (std::size_t index = 1; index < predecessors.size(); ++index) {
        const LocalValueMap& incoming = out[static_cast<std::size_t>(predecessors[index])];
        for (auto iter = merged.begin(); iter != merged.end();) {
            const auto found = incoming.find(iter->first);
            if (found == incoming.end() || !sameOperand(iter->second, found->second)) {
                iter = merged.erase(iter);
            } else {
                ++iter;
            }
        }
    }
    return merged;
}

bool sameMap(const LocalValueMap& lhs, const LocalValueMap& rhs)
{
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (const auto& [symbol, operand] : lhs) {
        const auto found = rhs.find(symbol);
        if (found == rhs.end() || !sameOperand(operand, found->second)) {
            return false;
        }
    }
    return true;
}

LocalValueMap transferValues(LocalValueMap values, const ir::BasicBlock& block)
{
    for (const ir::Instruction& instruction : block.instructions) {
        if (instruction.kind == ir::InstructionKind::StoreLocal && !instruction.operands.empty()) {
            values[instruction.symbol] = instruction.operands.front();
        }
    }
    return values;
}

class DeadStorePass final : public IrPass {
public:
    std::string name() const override
    {
        return "dead-store";
    }

    bool run(ir::Module& module) override
    {
        bool changed = false;
        for (ir::Function& function : module.functions) {
            changed = runFunction(function) || changed;
        }
        return changed;
    }

private:
    bool runFunction(ir::Function& function)
    {
        const auto successors = buildSuccessors(function);
        std::vector<LocalSet> liveIn(function.blocks.size());
        std::vector<LocalSet> liveOut(function.blocks.size());

        bool changed = true;
        while (changed) {
            changed = false;
            for (int blockIndex = static_cast<int>(function.blocks.size()) - 1; blockIndex >= 0; --blockIndex) {
                LocalSet out;
                for (int successor : successors[static_cast<std::size_t>(blockIndex)]) {
                    out.insert(liveIn[static_cast<std::size_t>(successor)].begin(), liveIn[static_cast<std::size_t>(successor)].end());
                }
                LocalSet in = transferLive(out, function.blocks[static_cast<std::size_t>(blockIndex)]);
                if (!sameSet(out, liveOut[static_cast<std::size_t>(blockIndex)])
                    || !sameSet(in, liveIn[static_cast<std::size_t>(blockIndex)])) {
                    liveOut[static_cast<std::size_t>(blockIndex)] = std::move(out);
                    liveIn[static_cast<std::size_t>(blockIndex)] = std::move(in);
                    changed = true;
                }
            }
        }

        bool removed = false;
        for (std::size_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
            ir::BasicBlock& block = function.blocks[blockIndex];
            const bool preserveStores = block.label.rfind(".while.body", 0) == 0
                || std::any_of(successors[blockIndex].begin(), successors[blockIndex].end(), [&](int successor) {
                       return successor <= static_cast<int>(blockIndex);
                   });
            LocalSet live = liveOut[blockIndex];
            std::vector<ir::Instruction> kept;
            kept.reserve(block.instructions.size());

            for (auto iter = block.instructions.rbegin(); iter != block.instructions.rend(); ++iter) {
                ir::Instruction instruction = *iter;
                if (instruction.kind == ir::InstructionKind::LoadLocal) {
                    live.insert(instruction.symbol);
                    kept.push_back(std::move(instruction));
                    continue;
                }
                if (instruction.kind == ir::InstructionKind::StoreLocal) {
                    if (!preserveStores && !live.contains(instruction.symbol)) {
                        removed = true;
                        continue;
                    }
                    live.erase(instruction.symbol);
                }
                kept.push_back(std::move(instruction));
            }

            std::reverse(kept.begin(), kept.end());
            block.instructions = std::move(kept);
        }

        return removed;
    }
};

class LocalValuePropagationPass final : public IrPass {
public:
    std::string name() const override
    {
        return "local-value-propagation";
    }

    bool run(ir::Module& module) override
    {
        bool changed = false;
        for (ir::Function& function : module.functions) {
            changed = runFunction(function) || changed;
        }
        return changed;
    }

private:
    bool runFunction(ir::Function& function)
    {
        const auto successors = buildSuccessors(function);
        const auto predecessors = buildPredecessors(function, successors);
        std::vector<LocalValueMap> in(function.blocks.size());
        std::vector<LocalValueMap> out(function.blocks.size());

        bool changed = true;
        while (changed) {
            changed = false;
            for (std::size_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
                const bool isWhileHeader = function.blocks[blockIndex].label.rfind(".while.cond", 0) == 0;
                const bool hasBackedge = isWhileHeader && std::any_of(predecessors[blockIndex].begin(), predecessors[blockIndex].end(), [&](int predecessor) {
                    return predecessor >= static_cast<int>(blockIndex);
                });
                LocalValueMap nextIn = hasBackedge ? LocalValueMap{} : mergeLocalValues(predecessors[blockIndex], out);
                LocalValueMap nextOut = transferValues(nextIn, function.blocks[blockIndex]);
                if (!sameMap(nextIn, in[blockIndex]) || !sameMap(nextOut, out[blockIndex])) {
                    in[blockIndex] = std::move(nextIn);
                    out[blockIndex] = std::move(nextOut);
                    changed = true;
                }
            }
        }

        bool rewritten = false;
        for (std::size_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
            LocalValueMap values = in[blockIndex];
            for (ir::Instruction& instruction : function.blocks[blockIndex].instructions) {
                if (instruction.kind == ir::InstructionKind::LoadLocal) {
                    const auto found = values.find(instruction.symbol);
                    if (found != values.end()) {
                        if (found->second.isImmediate) {
                            makeConst(instruction, found->second.immediate);
                        } else {
                            makeCopy(instruction, found->second);
                        }
                        rewritten = true;
                    }
                    continue;
                }
                if (instruction.kind == ir::InstructionKind::StoreLocal && !instruction.operands.empty()) {
                    values[instruction.symbol] = instruction.operands.front();
                }
            }
        }

        return rewritten;
    }
};

} // namespace

std::unique_ptr<IrPass> createDeadStorePass()
{
    return std::make_unique<DeadStorePass>();
}

std::unique_ptr<IrPass> createLocalValuePropagationPass()
{
    return std::make_unique<LocalValuePropagationPass>();
}

} // namespace toyc
