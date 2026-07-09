#include "pass_manager.h"

#include "analysis/cfg.h"

#include <algorithm>
#include <cstddef>
#include <optional>
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

struct ReturnSlice {
    std::unordered_set<int> values;
    std::unordered_set<std::string> locals;
    std::unordered_set<std::string> globals;
};

bool markValue(const ir::Operand& operand, ReturnSlice& slice)
{
    if (!operand.isImmediate && operand.value.id >= 0) {
        return slice.values.insert(operand.value.id).second;
    }
    return false;
}

ReturnSlice computeReturnSlice(const ir::Module& module, const ir::Function& function)
{
    ReturnSlice slice;
    std::vector<std::string> globalNames;
    globalNames.reserve(module.globals.size());
    for (const ir::Global& global : module.globals) {
        globalNames.push_back(global.name);
    }

    bool hasCall = false;
    for (const ir::BasicBlock& block : function.blocks) {
        if (block.hasTerminator
            && block.terminator.kind == ir::TerminatorKind::Return
            && block.terminator.hasReturnValue) {
            markValue(block.terminator.returnValue, slice);
        }
        for (const ir::Instruction& inst : block.instructions) {
            if (inst.kind == ir::InstructionKind::Call) {
                hasCall = true;
            }
        }
    }

    // Calls are a conservative global barrier: an unknown callee may read or
    // write any global, so a loop that writes globals is not dead across calls.
    if (hasCall) {
        for (const std::string& global : globalNames) {
            slice.globals.insert(global);
        }
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (const ir::BasicBlock& block : function.blocks) {
            for (const ir::Instruction& inst : block.instructions) {
                const bool dstLive = inst.dst.id >= 0 && slice.values.find(inst.dst.id) != slice.values.end();
                if (dstLive) {
                    for (const ir::Operand& operand : inst.operands) {
                        changed = markValue(operand, slice) || changed;
                    }
                    if (inst.kind == ir::InstructionKind::LoadLocal && !inst.symbol.empty()) {
                        changed = slice.locals.insert(inst.symbol).second || changed;
                    } else if (inst.kind == ir::InstructionKind::LoadGlobal && !inst.symbol.empty()) {
                        changed = slice.globals.insert(inst.symbol).second || changed;
                    } else if (inst.kind == ir::InstructionKind::Call) {
                        for (const std::string& global : globalNames) {
                            changed = slice.globals.insert(global).second || changed;
                        }
                    }
                }

                if (inst.kind == ir::InstructionKind::StoreLocal
                    && !inst.symbol.empty()
                    && slice.locals.find(inst.symbol) != slice.locals.end()) {
                    for (const ir::Operand& operand : inst.operands) {
                        changed = markValue(operand, slice) || changed;
                    }
                } else if (inst.kind == ir::InstructionKind::StoreGlobal
                    && !inst.symbol.empty()
                    && slice.globals.find(inst.symbol) != slice.globals.end()) {
                    for (const ir::Operand& operand : inst.operands) {
                        changed = markValue(operand, slice) || changed;
                    }
                }
            }
        }
    }

    return slice;
}

struct LoopCandidate {
    int exit = -1;
    std::unordered_set<int> bodyBlocks;
};

std::optional<LoopCandidate> collectCanonicalWhileLoop(
    const ir::Function& function,
    const analysis::Cfg& cfg,
    int header,
    int bodyEntry,
    int exit)
{
    if (bodyEntry < 0
        || exit < 0
        || bodyEntry == header
        || bodyEntry == exit
        || bodyEntry >= static_cast<int>(function.blocks.size())
        || exit >= static_cast<int>(function.blocks.size())) {
        return std::nullopt;
    }

    LoopCandidate candidate;
    candidate.exit = exit;
    std::vector<int> stack = {bodyEntry};
    std::unordered_set<int> visiting;
    int backedgeCount = 0;

    while (!stack.empty()) {
        const int blockIndex = stack.back();
        stack.pop_back();
        if (blockIndex < 0 || blockIndex >= static_cast<int>(function.blocks.size())) {
            return std::nullopt;
        }
        if (blockIndex == header || blockIndex == exit) {
            return std::nullopt;
        }
        if (!candidate.bodyBlocks.insert(blockIndex).second) {
            continue;
        }

        visiting.insert(blockIndex);
        for (int succ : cfg.successors[static_cast<std::size_t>(blockIndex)]) {
            if (succ == header) {
                ++backedgeCount;
                continue;
            }
            if (succ == exit) {
                // A body-to-exit edge is a break-like early exit. Keep it.
                return std::nullopt;
            }
            if (succ < 0 || succ >= static_cast<int>(function.blocks.size())) {
                return std::nullopt;
            }
            if (visiting.find(succ) != visiting.end()) {
                // Nested or irreducible cycle. Keep it.
                return std::nullopt;
            }
            if (candidate.bodyBlocks.find(succ) == candidate.bodyBlocks.end()) {
                stack.push_back(succ);
            }
        }
        visiting.erase(blockIndex);
    }

    if (backedgeCount != 1 || candidate.bodyBlocks.empty()) {
        return std::nullopt;
    }

    return candidate;
}

bool loopHasOnlyDeadEffects(
    const ir::Function& function,
    int header,
    const LoopCandidate& loop,
    const ReturnSlice& slice)
{
    std::vector<int> region;
    region.reserve(loop.bodyBlocks.size() + 1);
    region.push_back(header);
    for (int block : loop.bodyBlocks) {
        region.push_back(block);
    }

    bool conditionLocalUpdated = false;
    std::unordered_set<std::string> conditionLocals;
    for (const ir::Instruction& inst : function.blocks[static_cast<std::size_t>(header)].instructions) {
        if (inst.kind == ir::InstructionKind::LoadLocal && !inst.symbol.empty()) {
            conditionLocals.insert(inst.symbol);
        }
        if (inst.kind == ir::InstructionKind::StoreLocal || inst.kind == ir::InstructionKind::StoreGlobal) {
            return false;
        }
    }

    for (int blockIndex : region) {
        const ir::BasicBlock& block = function.blocks[static_cast<std::size_t>(blockIndex)];
        if (block.hasTerminator && block.terminator.kind == ir::TerminatorKind::Return) {
            return false;
        }

        for (const ir::Instruction& inst : block.instructions) {
            if (inst.kind == ir::InstructionKind::Call) {
                return false;
            }
            if (inst.hasSideEffect && inst.kind != ir::InstructionKind::StoreGlobal) {
                return false;
            }
            if (inst.dst.id >= 0 && slice.values.find(inst.dst.id) != slice.values.end()) {
                return false;
            }
            if (inst.kind == ir::InstructionKind::StoreLocal && !inst.symbol.empty()) {
                if (slice.locals.find(inst.symbol) != slice.locals.end()) {
                    return false;
                }
                if (conditionLocals.find(inst.symbol) != conditionLocals.end()) {
                    conditionLocalUpdated = true;
                }
            } else if (inst.kind == ir::InstructionKind::StoreGlobal && !inst.symbol.empty()) {
                if (slice.globals.find(inst.symbol) != slice.globals.end()) {
                    return false;
                }
            }
        }
    }

    // Avoid removing arbitrary while(1)-style nontermination. This is still not
    // a full termination proof; it only keeps this pass scoped to canonical
    // while loops whose condition is advanced by a loop-carried local update.
    return !conditionLocals.empty() && conditionLocalUpdated;
}

bool eliminateDeadHotLoops(ir::Module& module, ir::Function& function)
{
    bool changed = false;
    const ReturnSlice slice = computeReturnSlice(module, function);
    const analysis::Cfg cfg = analysis::buildCfg(function);

    for (int header = 0; header < static_cast<int>(function.blocks.size()); ++header) {
        ir::BasicBlock& headerBlock = function.blocks[static_cast<std::size_t>(header)];
        if (!headerBlock.hasTerminator || headerBlock.terminator.kind != ir::TerminatorKind::Branch) {
            continue;
        }

        const int trueBlock = headerBlock.terminator.trueBlock;
        const int falseBlock = headerBlock.terminator.falseBlock;
        std::optional<LoopCandidate> loop = collectCanonicalWhileLoop(function, cfg, header, trueBlock, falseBlock);
        if (!loop.has_value()) {
            loop = collectCanonicalWhileLoop(function, cfg, header, falseBlock, trueBlock);
        }
        if (!loop.has_value() || !loopHasOnlyDeadEffects(function, header, *loop, slice)) {
            continue;
        }

        headerBlock.instructions.clear();
        headerBlock.terminator.kind = ir::TerminatorKind::Jump;
        headerBlock.terminator.trueBlock = loop->exit;
        headerBlock.terminator.falseBlock = -1;
        headerBlock.terminator.condition = {};
        headerBlock.terminator.returnValue = {};
        headerBlock.terminator.hasReturnValue = false;
        changed = true;
    }

    return changed;
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
            changed = eliminateDeadHotLoops(module, function) || changed;

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
