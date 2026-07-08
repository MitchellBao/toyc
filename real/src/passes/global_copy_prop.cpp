#include "pass_manager.h"

#include "analysis/cfg.h"

#include <unordered_map>
#include <string>
#include <vector>

namespace toyc::passes {
namespace {

using AliasMap = std::unordered_map<int, int>;
using LocalMap = std::unordered_map<std::string, int>;
using GlobalMap = std::unordered_map<std::string, ir::Operand>;

bool sameOperand(const ir::Operand& lhs, const ir::Operand& rhs)
{
    if (lhs.isImmediate != rhs.isImmediate) {
        return false;
    }
    return lhs.isImmediate ? lhs.immediate == rhs.immediate : lhs.value.id == rhs.value.id;
}

bool sameGlobals(const GlobalMap& lhs, const GlobalMap& rhs)
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

struct State {
    AliasMap aliases;
    LocalMap locals;
    GlobalMap globals;

    friend bool operator==(const State& lhs, const State& rhs)
    {
        return lhs.aliases == rhs.aliases && lhs.locals == rhs.locals && sameGlobals(lhs.globals, rhs.globals);
    }

    friend bool operator!=(const State& lhs, const State& rhs)
    {
        return !(lhs == rhs);
    }
};

int resolveAlias(int value, const AliasMap& aliases)
{
    int current = value;
    for (int depth = 0; depth < 16; ++depth) {
        const auto found = aliases.find(current);
        if (found == aliases.end() || found->second == current) {
            return current;
        }
        current = found->second;
    }
    return current;
}

bool replaceOperand(ir::Operand& operand, const AliasMap& aliases)
{
    if (operand.isImmediate) {
        return false;
    }
    const int replacement = resolveAlias(operand.value.id, aliases);
    if (replacement == operand.value.id) {
        return false;
    }
    operand = ir::Operand::ref(ir::Value{replacement});
    return true;
}

void transferInstruction(State& state, const ir::Instruction& inst)
{
    if (inst.kind == ir::InstructionKind::StoreLocal && !inst.symbol.empty() && inst.operands.size() == 1) {
        if (inst.operands[0].isImmediate) {
            state.locals.erase(inst.symbol);
        } else {
            state.locals[inst.symbol] = resolveAlias(inst.operands[0].value.id, state.aliases);
        }
        return;
    }

    if (inst.kind == ir::InstructionKind::StoreGlobal && !inst.symbol.empty() && inst.operands.size() == 1) {
        ir::Operand stored = inst.operands[0];
        if (!stored.isImmediate) {
            stored = ir::Operand::ref(ir::Value{resolveAlias(stored.value.id, state.aliases)});
        }
        state.globals.clear();
        state.globals[inst.symbol] = stored;
        return;
    }

    if (inst.kind == ir::InstructionKind::Call) {
        state.globals.clear();
    }

    if (inst.dst.id < 0) {
        return;
    }

    if (inst.kind == ir::InstructionKind::Copy
        && inst.operands.size() == 1
        && !inst.operands[0].isImmediate) {
        state.aliases[inst.dst.id] = resolveAlias(inst.operands[0].value.id, state.aliases);
    } else if (inst.kind == ir::InstructionKind::LoadLocal && !inst.symbol.empty()) {
        const auto found = state.locals.find(inst.symbol);
        if (found != state.locals.end()) {
            state.aliases[inst.dst.id] = resolveAlias(found->second, state.aliases);
        } else {
            state.aliases.erase(inst.dst.id);
        }
    } else if (inst.kind == ir::InstructionKind::LoadGlobal && !inst.symbol.empty()) {
        const auto found = state.globals.find(inst.symbol);
        if (found != state.globals.end()) {
            if (found->second.isImmediate) {
                state.aliases.erase(inst.dst.id);
            } else {
                state.aliases[inst.dst.id] = resolveAlias(found->second.value.id, state.aliases);
            }
        } else {
            state.aliases.erase(inst.dst.id);
        }
    } else if (inst.kind == ir::InstructionKind::Call || inst.kind == ir::InstructionKind::StoreGlobal) {
        state.aliases.erase(inst.dst.id);
        state.globals.clear();
    } else {
        state.aliases.erase(inst.dst.id);
    }
}

State transferBlock(State state, const ir::BasicBlock& block)
{
    for (const ir::Instruction& inst : block.instructions) {
        transferInstruction(state, inst);
    }
    return state;
}

AliasMap intersectAliases(const AliasMap& lhs, const AliasMap& rhs)
{
    AliasMap result = lhs;
    for (auto iter = result.begin(); iter != result.end();) {
        const auto found = rhs.find(iter->first);
        if (found == rhs.end() || found->second != iter->second) {
            iter = result.erase(iter);
        } else {
            ++iter;
        }
    }
    return result;
}

LocalMap intersectLocals(const LocalMap& lhs, const LocalMap& rhs)
{
    LocalMap result = lhs;
    for (auto iter = result.begin(); iter != result.end();) {
        const auto found = rhs.find(iter->first);
        if (found == rhs.end() || found->second != iter->second) {
            iter = result.erase(iter);
        } else {
            ++iter;
        }
    }
    return result;
}

GlobalMap intersectGlobals(const GlobalMap& lhs, const GlobalMap& rhs)
{
    GlobalMap result = lhs;
    for (auto iter = result.begin(); iter != result.end();) {
        const auto found = rhs.find(iter->first);
        if (found == rhs.end() || !sameOperand(found->second, iter->second)) {
            iter = result.erase(iter);
        } else {
            ++iter;
        }
    }
    return result;
}

State intersectStates(const State& lhs, const State& rhs)
{
    return State{
        intersectAliases(lhs.aliases, rhs.aliases),
        intersectLocals(lhs.locals, rhs.locals),
        intersectGlobals(lhs.globals, rhs.globals)};
}

bool rewriteBlock(ir::BasicBlock& block, State state)
{
    bool changed = false;
    for (ir::Instruction& inst : block.instructions) {
        for (ir::Operand& operand : inst.operands) {
            changed = replaceOperand(operand, state.aliases) || changed;
        }

        if (inst.kind == ir::InstructionKind::LoadLocal && inst.dst.id >= 0 && !inst.symbol.empty()) {
            const auto found = state.locals.find(inst.symbol);
            if (found != state.locals.end()) {
                inst.kind = ir::InstructionKind::Copy;
                inst.operands = {ir::Operand::ref(ir::Value{resolveAlias(found->second, state.aliases)})};
                inst.symbol.clear();
                changed = true;
            }
        } else if (inst.kind == ir::InstructionKind::LoadGlobal && inst.dst.id >= 0 && !inst.symbol.empty()) {
            const auto found = state.globals.find(inst.symbol);
            if (found != state.globals.end()) {
                if (found->second.isImmediate) {
                    inst.kind = ir::InstructionKind::Const;
                    inst.operands = {found->second};
                } else {
                    inst.kind = ir::InstructionKind::Copy;
                    inst.operands = {ir::Operand::ref(ir::Value{resolveAlias(found->second.value.id, state.aliases)})};
                }
                inst.symbol.clear();
                changed = true;
            }
        }

        transferInstruction(state, inst);
    }
    changed = replaceOperand(block.terminator.condition, state.aliases) || changed;
    changed = replaceOperand(block.terminator.returnValue, state.aliases) || changed;
    return changed;
}

bool runOnFunction(ir::Function& function)
{
    if (function.blocks.empty()) {
        return false;
    }

    const analysis::Cfg cfg = analysis::buildCfg(function);
    std::vector<State> in(function.blocks.size());
    std::vector<State> out(function.blocks.size());

    bool dataChanged = true;
    for (int iteration = 0; dataChanged && iteration < 32; ++iteration) {
        dataChanged = false;
        for (int b = 0; b < static_cast<int>(function.blocks.size()); ++b) {
            State nextIn;
            const auto& preds = cfg.predecessors[static_cast<std::size_t>(b)];
            if (b != 0 && !preds.empty()) {
                nextIn = out[static_cast<std::size_t>(preds.front())];
                for (std::size_t i = 1; i < preds.size(); ++i) {
                    nextIn = intersectStates(nextIn, out[static_cast<std::size_t>(preds[i])]);
                }
            }

            State nextOut = transferBlock(nextIn, function.blocks[static_cast<std::size_t>(b)]);
            if (nextIn != in[static_cast<std::size_t>(b)] || nextOut != out[static_cast<std::size_t>(b)]) {
                in[static_cast<std::size_t>(b)] = std::move(nextIn);
                out[static_cast<std::size_t>(b)] = std::move(nextOut);
                dataChanged = true;
            }
        }
    }

    bool changed = false;
    for (int b = 0; b < static_cast<int>(function.blocks.size()); ++b) {
        changed = rewriteBlock(function.blocks[static_cast<std::size_t>(b)], in[static_cast<std::size_t>(b)]) || changed;
    }
    return changed;
}

class GlobalCopyPropPass final : public Pass {
public:
    std::string name() const override { return "global-copy-prop"; }

    bool run(ir::Module& module) override
    {
        bool changed = false;
        for (ir::Function& function : module.functions) {
            changed = runOnFunction(function) || changed;
        }
        return changed;
    }
};

} // namespace

std::unique_ptr<Pass> createGlobalCopyPropPass()
{
    return std::make_unique<GlobalCopyPropPass>();
}

} // namespace toyc::passes
