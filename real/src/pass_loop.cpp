#include "pass.h"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace toyc {
namespace {

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

bool operandInvariant(const ir::Operand& operand, const std::unordered_set<int>& variantValues)
{
    return operand.isImmediate || !variantValues.contains(operand.value.id);
}

bool pureHoistCandidate(const ir::Instruction& instruction)
{
    if (instruction.dst.id < 0 || instruction.hasSideEffect) {
        return false;
    }
    switch (instruction.kind) {
    case ir::InstructionKind::Const:
    case ir::InstructionKind::Copy:
    case ir::InstructionKind::Unary:
    case ir::InstructionKind::Binary:
    case ir::InstructionKind::LoadLocal:
        return true;
    case ir::InstructionKind::LoadGlobal:
    case ir::InstructionKind::StoreGlobal:
    case ir::InstructionKind::StoreLocal:
    case ir::InstructionKind::Call:
        return false;
    }
    return false;
}

bool instructionInvariant(
    const ir::Instruction& instruction,
    const std::unordered_set<int>& variantValues,
    const std::unordered_set<std::string>& modifiedLocals)
{
    if (!pureHoistCandidate(instruction)) {
        return false;
    }
    if (instruction.kind == ir::InstructionKind::LoadLocal && modifiedLocals.contains(instruction.symbol)) {
        return false;
    }
    return std::all_of(instruction.operands.begin(), instruction.operands.end(), [&](const ir::Operand& operand) {
        return operandInvariant(operand, variantValues);
    });
}

bool isJumpTo(const ir::BasicBlock& block, int target)
{
    return block.hasTerminator
        && block.terminator.kind == ir::TerminatorKind::Jump
        && block.terminator.trueBlock == target;
}

class LoopInvariantCodeMotionPass final : public IrPass {
public:
    std::string name() const override
    {
        return "loop-invariant-code-motion";
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
        bool changed = false;
        const auto successors = buildSuccessors(function);
        const auto predecessors = buildPredecessors(function, successors);

        for (std::size_t condIndex = 0; condIndex < function.blocks.size(); ++condIndex) {
            ir::BasicBlock& cond = function.blocks[condIndex];
            if (!cond.hasTerminator || cond.terminator.kind != ir::TerminatorKind::Branch) {
                continue;
            }

            const int bodyIndex = cond.terminator.trueBlock;
            if (bodyIndex < 0 || bodyIndex >= static_cast<int>(function.blocks.size())) {
                continue;
            }
            ir::BasicBlock& body = function.blocks[static_cast<std::size_t>(bodyIndex)];
            if (!isJumpTo(body, static_cast<int>(condIndex))) {
                continue;
            }

            int preheaderIndex = -1;
            for (int predecessor : predecessors[condIndex]) {
                if (predecessor != bodyIndex) {
                    if (preheaderIndex != -1) {
                        preheaderIndex = -1;
                        break;
                    }
                    preheaderIndex = predecessor;
                }
            }
            if (preheaderIndex < 0 || !isJumpTo(function.blocks[static_cast<std::size_t>(preheaderIndex)], static_cast<int>(condIndex))) {
                continue;
            }

            changed = hoistFromBody(function.blocks[static_cast<std::size_t>(preheaderIndex)], body) || changed;
        }

        return changed;
    }

    bool hoistFromBody(ir::BasicBlock& preheader, ir::BasicBlock& body)
    {
        std::unordered_set<int> variantValues;
        std::unordered_set<std::string> modifiedLocals;
        for (const ir::Instruction& instruction : body.instructions) {
            if (instruction.dst.id >= 0) {
                variantValues.insert(instruction.dst.id);
            }
            if (instruction.kind == ir::InstructionKind::StoreLocal) {
                modifiedLocals.insert(instruction.symbol);
            }
        }

        std::vector<ir::Instruction> hoisted;
        std::vector<ir::Instruction> kept;
        hoisted.reserve(body.instructions.size());
        kept.reserve(body.instructions.size());

        bool changed = false;
        for (ir::Instruction& instruction : body.instructions) {
            if (instructionInvariant(instruction, variantValues, modifiedLocals)) {
                variantValues.erase(instruction.dst.id);
                hoisted.push_back(std::move(instruction));
                changed = true;
            } else {
                kept.push_back(std::move(instruction));
            }
        }

        body.instructions = std::move(kept);
        if (!changed) {
            return false;
        }

        preheader.instructions.insert(
            preheader.instructions.end(),
            std::make_move_iterator(hoisted.begin()),
            std::make_move_iterator(hoisted.end()));
        return true;
    }
};

class TailRecursionPass final : public IrPass {
public:
    std::string name() const override
    {
        return "tail-recursion";
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
        if (function.blocks.empty() || function.params.empty()) {
            return false;
        }

        bool changed = false;
        for (ir::BasicBlock& block : function.blocks) {
            if (!block.hasTerminator
                || block.terminator.kind != ir::TerminatorKind::Return
                || !block.terminator.hasReturnValue
                || block.terminator.returnValue.isImmediate) {
                continue;
            }

            const int callValue = block.terminator.returnValue.value.id;
            auto callIter = std::find_if(block.instructions.begin(), block.instructions.end(), [&](const ir::Instruction& instruction) {
                return instruction.dst.id == callValue
                    && instruction.kind == ir::InstructionKind::Call
                    && instruction.symbol == function.name
                    && instruction.operands.size() == function.params.size();
            });
            if (callIter == block.instructions.end()) {
                continue;
            }

            std::vector<ir::Operand> args = callIter->operands;
            block.instructions.erase(callIter);
            for (std::size_t index = 0; index < args.size(); ++index) {
                ir::Instruction store;
                store.kind = ir::InstructionKind::StoreLocal;
                store.symbol = function.params[index];
                store.operands.push_back(args[index]);
                block.instructions.push_back(std::move(store));
            }

            block.terminator = ir::Terminator{};
            block.terminator.kind = ir::TerminatorKind::Jump;
            block.terminator.trueBlock = 0;
            block.hasTerminator = true;
            changed = true;
        }
        return changed;
    }
};

} // namespace

std::unique_ptr<IrPass> createLoopInvariantCodeMotionPass()
{
    return std::make_unique<LoopInvariantCodeMotionPass>();
}

std::unique_ptr<IrPass> createTailRecursionPass()
{
    return std::make_unique<TailRecursionPass>();
}

} // namespace toyc
