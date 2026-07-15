#include "pass_manager.h"

#include "analysis/cfg.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace toyc::passes {
namespace {

using ConstMap = std::unordered_map<int, std::int32_t>;

std::int32_t evalUnary(ir::UnaryOpcode op, std::int32_t value)
{
    switch (op) {
    case ir::UnaryOpcode::Plus:
        return value;
    case ir::UnaryOpcode::Minus:
        return -value;
    case ir::UnaryOpcode::Not:
        return value == 0 ? 1 : 0;
    }
    return value;
}

std::optional<std::int32_t> evalBinary(ir::BinaryOpcode op, std::int32_t lhs, std::int32_t rhs)
{
    switch (op) {
    case ir::BinaryOpcode::Add:
        return lhs + rhs;
    case ir::BinaryOpcode::Sub:
        return lhs - rhs;
    case ir::BinaryOpcode::Mul:
        return lhs * rhs;
    case ir::BinaryOpcode::Div:
        return rhs == 0 ? std::nullopt : std::optional<std::int32_t>{lhs / rhs};
    case ir::BinaryOpcode::Mod:
        return rhs == 0 ? std::nullopt : std::optional<std::int32_t>{lhs % rhs};
    case ir::BinaryOpcode::Equal:
        return lhs == rhs ? 1 : 0;
    case ir::BinaryOpcode::NotEqual:
        return lhs != rhs ? 1 : 0;
    case ir::BinaryOpcode::Less:
        return lhs < rhs ? 1 : 0;
    case ir::BinaryOpcode::LessEqual:
        return lhs <= rhs ? 1 : 0;
    case ir::BinaryOpcode::Greater:
        return lhs > rhs ? 1 : 0;
    case ir::BinaryOpcode::GreaterEqual:
        return lhs >= rhs ? 1 : 0;
    case ir::BinaryOpcode::LogicalAnd:
        return (lhs != 0 && rhs != 0) ? 1 : 0;
    case ir::BinaryOpcode::LogicalOr:
        return (lhs != 0 || rhs != 0) ? 1 : 0;
    }
    return std::nullopt;
}

std::optional<std::int32_t> knownOperand(const ir::Operand& operand, const ConstMap& constants)
{
    if (operand.isImmediate) {
        return operand.immediate;
    }
    const auto found = constants.find(operand.value.id);
    if (found == constants.end()) {
        return std::nullopt;
    }
    return found->second;
}

bool replaceOperand(ir::Operand& operand, const ConstMap& constants)
{
    if (operand.isImmediate) {
        return false;
    }
    const auto found = constants.find(operand.value.id);
    if (found == constants.end()) {
        return false;
    }
    operand = ir::Operand::imm(found->second);
    return true;
}

void transferInstruction(ConstMap& constants, const ir::Instruction& inst)
{
    if (inst.kind == ir::InstructionKind::Const && inst.dst.id >= 0 && !inst.operands.empty() && inst.operands[0].isImmediate) {
        constants[inst.dst.id] = inst.operands[0].immediate;
    } else if (inst.kind == ir::InstructionKind::Copy && inst.dst.id >= 0 && !inst.operands.empty()) {
        if (const auto value = knownOperand(inst.operands[0], constants); value.has_value()) {
            constants[inst.dst.id] = *value;
        } else {
            constants.erase(inst.dst.id);
        }
    } else if (inst.kind == ir::InstructionKind::Unary && inst.dst.id >= 0 && inst.operands.size() == 1) {
        if (const auto value = knownOperand(inst.operands[0], constants); value.has_value()) {
            constants[inst.dst.id] = evalUnary(inst.unaryOp, *value);
        } else {
            constants.erase(inst.dst.id);
        }
    } else if (inst.kind == ir::InstructionKind::Binary && inst.dst.id >= 0 && inst.operands.size() == 2) {
        const auto lhs = knownOperand(inst.operands[0], constants);
        const auto rhs = knownOperand(inst.operands[1], constants);
        if (lhs.has_value() && rhs.has_value()) {
            if (const auto value = evalBinary(inst.binaryOp, *lhs, *rhs); value.has_value()) {
                constants[inst.dst.id] = *value;
            } else {
                constants.erase(inst.dst.id);
            }
        } else {
            constants.erase(inst.dst.id);
        }
    } else if (inst.dst.id >= 0) {
        constants.erase(inst.dst.id);
    }
}

ConstMap transferBlock(ConstMap constants, const ir::BasicBlock& block)
{
    for (const ir::Instruction& inst : block.instructions) {
        transferInstruction(constants, inst);
    }
    return constants;
}

ConstMap intersectConstants(const ConstMap& lhs, const ConstMap& rhs)
{
    ConstMap result = lhs;
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

std::vector<int> executableSuccessors(const ir::BasicBlock& block, const ConstMap& constants)
{
    if (!block.hasTerminator) {
        return {};
    }
    const ir::Terminator& term = block.terminator;
    if (term.kind == ir::TerminatorKind::Jump) {
        return {term.trueBlock};
    }
    if (term.kind == ir::TerminatorKind::Branch) {
        const auto condition = knownOperand(term.condition, constants);
        if (condition.has_value()) {
            return {*condition != 0 ? term.trueBlock : term.falseBlock};
        }
        return {term.trueBlock, term.falseBlock};
    }
    return {};
}

bool rewriteBlock(ir::BasicBlock& block, ConstMap constants)
{
    bool changed = false;
    for (ir::Instruction& inst : block.instructions) {
        for (ir::Operand& operand : inst.operands) {
            changed = replaceOperand(operand, constants) || changed;
        }

        if (inst.kind == ir::InstructionKind::Unary && inst.dst.id >= 0 && inst.operands.size() == 1) {
            if (const auto value = knownOperand(inst.operands[0], constants); value.has_value()) {
                inst.kind = ir::InstructionKind::Const;
                inst.operands = {ir::Operand::imm(evalUnary(inst.unaryOp, *value))};
                constants[inst.dst.id] = inst.operands[0].immediate;
                changed = true;
                continue;
            }
        }

        if (inst.kind == ir::InstructionKind::Binary && inst.dst.id >= 0 && inst.operands.size() == 2) {
            const auto lhs = knownOperand(inst.operands[0], constants);
            const auto rhs = knownOperand(inst.operands[1], constants);
            if (lhs.has_value() && rhs.has_value()) {
                if (const auto value = evalBinary(inst.binaryOp, *lhs, *rhs); value.has_value()) {
                    inst.kind = ir::InstructionKind::Const;
                    inst.operands = {ir::Operand::imm(*value)};
                    constants[inst.dst.id] = *value;
                    changed = true;
                    continue;
                }
            }
        }

        transferInstruction(constants, inst);
    }
    changed = replaceOperand(block.terminator.condition, constants) || changed;
    changed = replaceOperand(block.terminator.returnValue, constants) || changed;
    return changed;
}

bool runOnFunction(ir::Function& function)
{
    if (function.blocks.empty()) {
        return false;
    }

    const analysis::Cfg cfg = analysis::buildCfg(function);
    std::vector<ConstMap> in(function.blocks.size());
    std::vector<ConstMap> out(function.blocks.size());
    std::vector<bool> executable(function.blocks.size(), false);
    executable[0] = true;

    bool dataChanged = true;
    for (int iteration = 0; dataChanged && iteration < 64; ++iteration) {
        dataChanged = false;
        for (int b = 0; b < static_cast<int>(function.blocks.size()); ++b) {
            if (!executable[static_cast<std::size_t>(b)]) {
                continue;
            }
            ConstMap nextIn;
            const auto& preds = cfg.predecessors[static_cast<std::size_t>(b)];
            if (b != 0) {
                bool haveExecutablePred = false;
                for (int pred : preds) {
                    if (pred < 0
                        || pred >= static_cast<int>(function.blocks.size())
                        || !executable[static_cast<std::size_t>(pred)]) {
                        continue;
                    }
                    if (!haveExecutablePred) {
                        nextIn = out[static_cast<std::size_t>(pred)];
                        haveExecutablePred = true;
                    } else {
                        nextIn = intersectConstants(nextIn, out[static_cast<std::size_t>(pred)]);
                    }
                }
            }

            ConstMap nextOut = transferBlock(nextIn, function.blocks[static_cast<std::size_t>(b)]);
            for (int succ : executableSuccessors(function.blocks[static_cast<std::size_t>(b)], nextOut)) {
                if (succ >= 0
                    && succ < static_cast<int>(function.blocks.size())
                    && !executable[static_cast<std::size_t>(succ)]) {
                    executable[static_cast<std::size_t>(succ)] = true;
                    dataChanged = true;
                }
            }
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

struct ImmutableLocalConstant {
    std::int32_t value = 0;
    std::size_t storeIndex = 0;
};

bool replaceImmutableLocalLoads(ir::Function& function)
{
    if (function.blocks.empty()) {
        return false;
    }

    std::unordered_map<std::string, int> storeCounts;
    for (const ir::BasicBlock& block : function.blocks) {
        for (const ir::Instruction& inst : block.instructions) {
            if (inst.kind == ir::InstructionKind::StoreLocal && !inst.symbol.empty()) {
                ++storeCounts[inst.symbol];
            }
        }
    }

    ConstMap entryConstants;
    std::unordered_map<std::string, ImmutableLocalConstant> immutable;
    const ir::BasicBlock& entry = function.blocks.front();
    for (std::size_t i = 0; i < entry.instructions.size(); ++i) {
        const ir::Instruction& inst = entry.instructions[i];
        if (inst.kind == ir::InstructionKind::StoreLocal
            && !inst.symbol.empty()
            && !inst.operands.empty()
            && storeCounts[inst.symbol] == 1) {
            if (const auto value = knownOperand(inst.operands[0], entryConstants); value.has_value()) {
                immutable[inst.symbol] = ImmutableLocalConstant{*value, i};
            }
        }
        transferInstruction(entryConstants, inst);
    }
    if (immutable.empty()) {
        return false;
    }

    bool changed = false;
    for (std::size_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        ir::BasicBlock& block = function.blocks[blockIndex];
        for (std::size_t instIndex = 0; instIndex < block.instructions.size(); ++instIndex) {
            ir::Instruction& inst = block.instructions[instIndex];
            if (inst.kind != ir::InstructionKind::LoadLocal || inst.dst.id < 0 || inst.symbol.empty()) {
                continue;
            }
            const auto found = immutable.find(inst.symbol);
            if (found == immutable.end()
                || (blockIndex == 0 && instIndex <= found->second.storeIndex)) {
                continue;
            }
            inst.kind = ir::InstructionKind::Const;
            inst.operands = {ir::Operand::imm(found->second.value)};
            inst.symbol.clear();
            inst.hasSideEffect = false;
            changed = true;
        }
    }
    return changed;
}

using LocalConstMap = std::unordered_map<std::string, std::int32_t>;

LocalConstMap intersectLocalConstants(const LocalConstMap& lhs, const LocalConstMap& rhs)
{
    LocalConstMap result = lhs;
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

void transferLocalConstants(LocalConstMap& locals, const ir::BasicBlock& block)
{
    for (const ir::Instruction& inst : block.instructions) {
        if (inst.kind != ir::InstructionKind::StoreLocal || inst.symbol.empty()) {
            continue;
        }
        if (!inst.operands.empty() && inst.operands[0].isImmediate) {
            locals[inst.symbol] = inst.operands[0].immediate;
        } else {
            locals.erase(inst.symbol);
        }
    }
}

bool replaceConstantLocalLoads(ir::Function& function)
{
    if (function.blocks.empty()) {
        return false;
    }

    const analysis::Cfg cfg = analysis::buildCfg(function);
    std::vector<LocalConstMap> in(function.blocks.size());
    std::vector<LocalConstMap> out(function.blocks.size());
    bool dataChanged = true;
    for (int iteration = 0; dataChanged && iteration < 64; ++iteration) {
        dataChanged = false;
        for (int b = 0; b < static_cast<int>(function.blocks.size()); ++b) {
            LocalConstMap nextIn;
            const auto& preds = cfg.predecessors[static_cast<std::size_t>(b)];
            if (b != 0 && !preds.empty()) {
                nextIn = out[static_cast<std::size_t>(preds.front())];
                for (std::size_t i = 1; i < preds.size(); ++i) {
                    nextIn = intersectLocalConstants(nextIn, out[static_cast<std::size_t>(preds[i])]);
                }
            }
            LocalConstMap nextOut = nextIn;
            transferLocalConstants(nextOut, function.blocks[static_cast<std::size_t>(b)]);
            if (nextIn != in[static_cast<std::size_t>(b)] || nextOut != out[static_cast<std::size_t>(b)]) {
                in[static_cast<std::size_t>(b)] = std::move(nextIn);
                out[static_cast<std::size_t>(b)] = std::move(nextOut);
                dataChanged = true;
            }
        }
    }

    bool changed = false;
    for (std::size_t b = 0; b < function.blocks.size(); ++b) {
        LocalConstMap locals = in[b];
        for (ir::Instruction& inst : function.blocks[b].instructions) {
            if (inst.kind == ir::InstructionKind::LoadLocal && inst.dst.id >= 0 && !inst.symbol.empty()) {
                if (const auto found = locals.find(inst.symbol); found != locals.end()) {
                    inst.kind = ir::InstructionKind::Const;
                    inst.operands = {ir::Operand::imm(found->second)};
                    inst.symbol.clear();
                    inst.hasSideEffect = false;
                    changed = true;
                }
            } else if (inst.kind == ir::InstructionKind::StoreLocal && !inst.symbol.empty()) {
                if (!inst.operands.empty() && inst.operands[0].isImmediate) {
                    locals[inst.symbol] = inst.operands[0].immediate;
                } else {
                    locals.erase(inst.symbol);
                }
            }
        }
    }
    return changed;
}

std::unordered_map<std::string, std::int32_t> readOnlyGlobalInitials(const ir::Module& module)
{
    std::unordered_set<std::string> storedGlobals;
    for (const ir::Function& function : module.functions) {
        for (const ir::BasicBlock& block : function.blocks) {
            for (const ir::Instruction& inst : block.instructions) {
                if (inst.kind == ir::InstructionKind::StoreGlobal && !inst.symbol.empty()) {
                    storedGlobals.insert(inst.symbol);
                }
            }
        }
    }

    std::unordered_map<std::string, std::int32_t> values;
    for (const ir::Global& global : module.globals) {
        if (storedGlobals.find(global.name) == storedGlobals.end()) {
            values.emplace(global.name, global.init);
        }
    }
    return values;
}

bool replaceReadOnlyGlobalLoads(ir::Module& module)
{
    const std::unordered_map<std::string, std::int32_t> globals = readOnlyGlobalInitials(module);
    if (globals.empty()) {
        return false;
    }

    bool changed = false;
    for (ir::Function& function : module.functions) {
        for (ir::BasicBlock& block : function.blocks) {
            for (ir::Instruction& inst : block.instructions) {
                if (inst.kind != ir::InstructionKind::LoadGlobal || inst.dst.id < 0 || inst.symbol.empty()) {
                    continue;
                }
                const auto found = globals.find(inst.symbol);
                if (found == globals.end()) {
                    continue;
                }
                inst.kind = ir::InstructionKind::Const;
                inst.operands = {ir::Operand::imm(found->second)};
                inst.symbol.clear();
                changed = true;
            }
        }
    }
    return changed;
}

class GlobalConstPropPass final : public Pass {
public:
    std::string name() const override { return "global-const-prop"; }

    bool run(ir::Module& module) override
    {
        bool changed = replaceReadOnlyGlobalLoads(module);
        for (ir::Function& function : module.functions) {
            changed = replaceImmutableLocalLoads(function) || changed;
            changed = runOnFunction(function) || changed;
            changed = replaceConstantLocalLoads(function) || changed;
        }
        return changed;
    }
};

} // namespace

std::unique_ptr<Pass> createGlobalConstPropPass()
{
    return std::make_unique<GlobalConstPropPass>();
}

} // namespace toyc::passes
