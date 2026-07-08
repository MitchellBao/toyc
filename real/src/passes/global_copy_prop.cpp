#include "pass_manager.h"

#include "analysis/cfg.h"

#include <algorithm>
#include <unordered_map>
#include <string>
#include <vector>

namespace toyc::passes {
namespace {

using AliasMap = std::unordered_map<int, int>;
using LocalMap = std::unordered_map<std::string, int>;

bool sameOperand(const ir::Operand& lhs, const ir::Operand& rhs)
{
    if (lhs.isImmediate != rhs.isImmediate) {
        return false;
    }
    return lhs.isImmediate ? lhs.immediate == rhs.immediate : lhs.value.id == rhs.value.id;
}

struct Expr {
    enum class Kind {
        Operand,
        LoadLocal,
        Unary,
        Binary,
    };

    Kind kind = Kind::Operand;
    ir::Operand value;
    std::string symbol;
    ir::UnaryOpcode unaryOp = ir::UnaryOpcode::Plus;
    ir::BinaryOpcode binaryOp = ir::BinaryOpcode::Add;
    std::vector<Expr> operands;

    friend bool operator==(const Expr& lhs, const Expr& rhs)
    {
        return lhs.kind == rhs.kind
            && lhs.symbol == rhs.symbol
            && lhs.unaryOp == rhs.unaryOp
            && lhs.binaryOp == rhs.binaryOp
            && sameOperand(lhs.value, rhs.value)
            && lhs.operands == rhs.operands;
    }

    friend bool operator!=(const Expr& lhs, const Expr& rhs)
    {
        return !(lhs == rhs);
    }
};

using ExprMap = std::unordered_map<int, Expr>;
using GlobalMap = std::unordered_map<std::string, Expr>;

bool sameGlobals(const GlobalMap& lhs, const GlobalMap& rhs)
{
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (const auto& [symbol, operand] : lhs) {
        const auto found = rhs.find(symbol);
        if (found == rhs.end() || operand != found->second) {
            return false;
        }
    }
    return true;
}

struct State {
    AliasMap aliases;
    LocalMap locals;
    ExprMap exprs;
    GlobalMap globals;

    friend bool operator==(const State& lhs, const State& rhs)
    {
        return lhs.aliases == rhs.aliases
            && lhs.locals == rhs.locals
            && lhs.exprs == rhs.exprs
            && sameGlobals(lhs.globals, rhs.globals);
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

ir::Operand resolvedOperand(ir::Operand operand, const AliasMap& aliases)
{
    if (!operand.isImmediate) {
        operand = ir::Operand::ref(ir::Value{resolveAlias(operand.value.id, aliases)});
    }
    return operand;
}

Expr operandExpr(ir::Operand operand, const State& state)
{
    operand = resolvedOperand(operand, state.aliases);
    if (!operand.isImmediate) {
        const auto found = state.exprs.find(operand.value.id);
        if (found != state.exprs.end()) {
            return found->second;
        }
    }
    Expr expr;
    expr.kind = Expr::Kind::Operand;
    expr.value = operand;
    return expr;
}

std::vector<Expr> operandExprs(const std::vector<ir::Operand>& operands, const State& state)
{
    std::vector<Expr> result;
    result.reserve(operands.size());
    for (const ir::Operand& operand : operands) {
        result.push_back(operandExpr(operand, state));
    }
    return result;
}

void rememberExpression(State& state, const ir::Instruction& inst)
{
    if (inst.dst.id < 0) {
        return;
    }

    if (inst.kind == ir::InstructionKind::Const && inst.operands.size() == 1 && inst.operands[0].isImmediate) {
        Expr expr;
        expr.kind = Expr::Kind::Operand;
        expr.value = inst.operands[0];
        state.exprs[inst.dst.id] = std::move(expr);
        return;
    }

    if (inst.kind == ir::InstructionKind::Copy && inst.operands.size() == 1) {
        state.exprs[inst.dst.id] = operandExpr(inst.operands[0], state);
        return;
    }

    if (inst.kind == ir::InstructionKind::Unary && inst.operands.size() == 1 && !inst.hasSideEffect) {
        Expr expr;
        expr.kind = Expr::Kind::Unary;
        expr.unaryOp = inst.unaryOp;
        expr.operands = operandExprs(inst.operands, state);
        state.exprs[inst.dst.id] = std::move(expr);
        return;
    }

    if (inst.kind == ir::InstructionKind::Binary && inst.operands.size() == 2 && !inst.hasSideEffect) {
        Expr expr;
        expr.kind = Expr::Kind::Binary;
        expr.binaryOp = inst.binaryOp;
        expr.operands = operandExprs(inst.operands, state);
        state.exprs[inst.dst.id] = std::move(expr);
        return;
    }

    state.exprs.erase(inst.dst.id);
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
        state.globals[inst.symbol] = operandExpr(inst.operands[0], state);
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
        rememberExpression(state, inst);
    } else if (inst.kind == ir::InstructionKind::LoadLocal && !inst.symbol.empty()) {
        const auto found = state.locals.find(inst.symbol);
        if (found != state.locals.end()) {
            state.aliases[inst.dst.id] = resolveAlias(found->second, state.aliases);
            state.exprs[inst.dst.id] = operandExpr(ir::Operand::ref(ir::Value{found->second}), state);
        } else {
            state.aliases.erase(inst.dst.id);
            Expr expr;
            expr.kind = Expr::Kind::LoadLocal;
            expr.symbol = inst.symbol;
            state.exprs[inst.dst.id] = std::move(expr);
        }
    } else if (inst.kind == ir::InstructionKind::LoadGlobal && !inst.symbol.empty()) {
        const auto found = state.globals.find(inst.symbol);
        if (found != state.globals.end()) {
            if (found->second.kind == Expr::Kind::Operand && found->second.value.isImmediate) {
                state.aliases.erase(inst.dst.id);
            } else if (found->second.kind == Expr::Kind::Operand) {
                state.aliases[inst.dst.id] = resolveAlias(found->second.value.value.id, state.aliases);
            }
            state.exprs[inst.dst.id] = found->second;
        } else {
            state.aliases.erase(inst.dst.id);
            state.exprs.erase(inst.dst.id);
        }
    } else if (inst.kind == ir::InstructionKind::Call || inst.kind == ir::InstructionKind::StoreGlobal) {
        state.aliases.erase(inst.dst.id);
        state.exprs.erase(inst.dst.id);
        state.globals.clear();
    } else {
        state.aliases.erase(inst.dst.id);
        rememberExpression(state, inst);
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
        if (found == rhs.end() || found->second != iter->second) {
            iter = result.erase(iter);
        } else {
            ++iter;
        }
    }
    return result;
}

ExprMap intersectExprs(const ExprMap& lhs, const ExprMap& rhs)
{
    ExprMap result = lhs;
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

State intersectStates(const State& lhs, const State& rhs)
{
    return State{
        intersectAliases(lhs.aliases, rhs.aliases),
        intersectLocals(lhs.locals, rhs.locals),
        intersectExprs(lhs.exprs, rhs.exprs),
        intersectGlobals(lhs.globals, rhs.globals)};
}

ir::Value newValue(ir::Function& function)
{
    return ir::Value{function.nextValue++};
}

ir::Operand materializeExpr(
    const Expr& expr,
    const AliasMap& aliases,
    ir::Function& function,
    std::vector<ir::Instruction>& prefix)
{
    if (expr.kind == Expr::Kind::Operand) {
        return resolvedOperand(expr.value, aliases);
    }

    ir::Instruction inst;
    inst.dst = newValue(function);
    switch (expr.kind) {
    case Expr::Kind::Operand:
        break;
    case Expr::Kind::LoadLocal:
        inst.kind = ir::InstructionKind::LoadLocal;
        inst.symbol = expr.symbol;
        break;
    case Expr::Kind::Unary:
        inst.kind = ir::InstructionKind::Unary;
        inst.unaryOp = expr.unaryOp;
        for (const Expr& operand : expr.operands) {
            inst.operands.push_back(materializeExpr(operand, aliases, function, prefix));
        }
        break;
    case Expr::Kind::Binary:
        inst.kind = ir::InstructionKind::Binary;
        inst.binaryOp = expr.binaryOp;
        for (const Expr& operand : expr.operands) {
            inst.operands.push_back(materializeExpr(operand, aliases, function, prefix));
        }
        break;
    }
    prefix.push_back(std::move(inst));
    return ir::Operand::ref(prefix.back().dst);
}

bool rewriteFromExpr(
    ir::Instruction& inst,
    const Expr& expr,
    const AliasMap& aliases,
    ir::Function& function,
    std::vector<ir::Instruction>& prefix)
{
    switch (expr.kind) {
    case Expr::Kind::Operand:
        if (expr.value.isImmediate) {
            inst.kind = ir::InstructionKind::Const;
            inst.operands = {expr.value};
        } else {
            inst.kind = ir::InstructionKind::Copy;
            inst.operands = {resolvedOperand(expr.value, aliases)};
        }
        break;
    case Expr::Kind::LoadLocal:
        inst.kind = ir::InstructionKind::LoadLocal;
        inst.symbol = expr.symbol;
        inst.operands.clear();
        break;
    case Expr::Kind::Unary:
        inst.kind = ir::InstructionKind::Unary;
        inst.unaryOp = expr.unaryOp;
        inst.operands.clear();
        for (const Expr& operand : expr.operands) {
            inst.operands.push_back(materializeExpr(operand, aliases, function, prefix));
        }
        break;
    case Expr::Kind::Binary:
        inst.kind = ir::InstructionKind::Binary;
        inst.binaryOp = expr.binaryOp;
        inst.operands.clear();
        for (const Expr& operand : expr.operands) {
            inst.operands.push_back(materializeExpr(operand, aliases, function, prefix));
        }
        break;
    }
    if (expr.kind != Expr::Kind::LoadLocal) {
        inst.symbol.clear();
    }
    inst.hasSideEffect = false;
    return true;
}

bool rewriteBlock(ir::Function& function, ir::BasicBlock& block, State state)
{
    bool changed = false;
    std::vector<ir::Instruction> rewritten;
    rewritten.reserve(block.instructions.size());
    for (ir::Instruction& inst : block.instructions) {
        const ir::Instruction original = inst;
        std::vector<ir::Instruction> prefix;
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
                changed = rewriteFromExpr(inst, found->second, state.aliases, function, prefix) || changed;
            }
        }

        transferInstruction(state, original);
        for (ir::Instruction& materialized : prefix) {
            rewritten.push_back(std::move(materialized));
        }
        rewritten.push_back(std::move(inst));
    }
    block.instructions = std::move(rewritten);
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
        changed = rewriteBlock(function, function.blocks[static_cast<std::size_t>(b)], in[static_cast<std::size_t>(b)]) || changed;
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
