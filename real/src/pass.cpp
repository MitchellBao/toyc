#include "pass.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace toyc {
namespace {

using ConstMap = std::unordered_map<int, std::int32_t>;
using CopyMap = std::unordered_map<int, ir::Operand>;

std::optional<std::int32_t> immediateValue(const ir::Operand& operand, const ConstMap& constants)
{
    if (operand.isImmediate) {
        return operand.immediate;
    }
    const auto found = constants.find(operand.value.id);
    if (found != constants.end()) {
        return found->second;
    }
    return std::nullopt;
}

ir::Operand resolveCopy(ir::Operand operand, const ConstMap& constants, const CopyMap& copies)
{
    for (int depth = 0; depth < 16 && !operand.isImmediate; ++depth) {
        const auto constant = constants.find(operand.value.id);
        if (constant != constants.end()) {
            return ir::Operand::imm(constant->second);
        }
        const auto copy = copies.find(operand.value.id);
        if (copy == copies.end()) {
            break;
        }
        operand = copy->second;
    }
    return operand;
}

std::int32_t foldUnary(ir::UnaryOpcode op, std::int32_t value)
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

std::optional<std::int32_t> foldBinary(ir::BinaryOpcode op, std::int32_t lhs, std::int32_t rhs)
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

class LocalSimplifyPass final : public IrPass {
public:
    std::string name() const override
    {
        return "local-simplify";
    }

    bool run(ir::Module& module) override
    {
        bool changed = false;
        for (ir::Function& function : module.functions) {
            for (ir::BasicBlock& block : function.blocks) {
                changed = simplifyBlock(block) || changed;
            }
        }
        return changed;
    }

private:
    bool simplifyBlock(ir::BasicBlock& block)
    {
        bool changed = false;
        ConstMap constants;
        CopyMap copies;
        std::unordered_map<std::string, ir::Operand> localValues;

        for (ir::Instruction& instruction : block.instructions) {
            for (ir::Operand& operand : instruction.operands) {
                const ir::Operand resolved = resolveCopy(operand, constants, copies);
                if (resolved.isImmediate != operand.isImmediate
                    || resolved.immediate != operand.immediate
                    || resolved.value.id != operand.value.id) {
                    operand = resolved;
                    changed = true;
                }
            }

            const int dst = instruction.dst.id;
            if (dst >= 0) {
                constants.erase(dst);
                copies.erase(dst);
            }

            switch (instruction.kind) {
            case ir::InstructionKind::Const:
                if (!instruction.operands.empty()) {
                    if (const auto value = immediateValue(instruction.operands.front(), constants)) {
                        constants[dst] = *value;
                    }
                }
                break;
            case ir::InstructionKind::Copy:
                if (!instruction.operands.empty()) {
                    const ir::Operand operand = resolveCopy(instruction.operands.front(), constants, copies);
                    if (const auto value = immediateValue(operand, constants)) {
                        makeConst(instruction, *value);
                        constants[dst] = *value;
                        changed = true;
                    } else {
                        instruction.operands.front() = operand;
                        copies[dst] = operand;
                    }
                }
                break;
            case ir::InstructionKind::Unary:
                if (!instruction.operands.empty()) {
                    if (const auto value = immediateValue(instruction.operands.front(), constants)) {
                        const std::int32_t folded = foldUnary(instruction.unaryOp, *value);
                        makeConst(instruction, folded);
                        constants[dst] = folded;
                        changed = true;
                    }
                }
                break;
            case ir::InstructionKind::Binary:
                changed = simplifyBinary(instruction, constants, copies) || changed;
                break;
            case ir::InstructionKind::LoadLocal: {
                const auto found = localValues.find(instruction.symbol);
                if (found != localValues.end()) {
                    const ir::Operand operand = resolveCopy(found->second, constants, copies);
                    if (const auto value = immediateValue(operand, constants)) {
                        makeConst(instruction, *value);
                        constants[dst] = *value;
                    } else {
                        makeCopy(instruction, operand);
                        copies[dst] = operand;
                    }
                    changed = true;
                }
                break;
            }
            case ir::InstructionKind::StoreLocal:
                if (!instruction.operands.empty()) {
                    localValues[instruction.symbol] = resolveCopy(instruction.operands.front(), constants, copies);
                }
                break;
            case ir::InstructionKind::LoadGlobal:
            case ir::InstructionKind::StoreGlobal:
            case ir::InstructionKind::Call:
                break;
            }
        }

        if (block.hasTerminator) {
            if (block.terminator.condition.value.id >= 0 || block.terminator.condition.isImmediate) {
                block.terminator.condition = resolveCopy(block.terminator.condition, constants, copies);
            }
            if (block.terminator.returnValue.value.id >= 0 || block.terminator.returnValue.isImmediate) {
                block.terminator.returnValue = resolveCopy(block.terminator.returnValue, constants, copies);
            }
        }

        return changed;
    }

    bool simplifyBinary(ir::Instruction& instruction, ConstMap& constants, CopyMap& copies)
    {
        if (instruction.operands.size() < 2) {
            return false;
        }

        const int dst = instruction.dst.id;
        const ir::Operand lhs = instruction.operands[0];
        const ir::Operand rhs = instruction.operands[1];
        const auto lhsConst = immediateValue(lhs, constants);
        const auto rhsConst = immediateValue(rhs, constants);

        if (lhsConst.has_value() && rhsConst.has_value()) {
            if (const auto folded = foldBinary(instruction.binaryOp, *lhsConst, *rhsConst)) {
                makeConst(instruction, *folded);
                constants[dst] = *folded;
                return true;
            }
        }

        switch (instruction.binaryOp) {
        case ir::BinaryOpcode::Add:
            if (rhsConst.has_value() && *rhsConst == 0) {
                makeCopy(instruction, lhs);
                copies[dst] = lhs;
                return true;
            }
            if (lhsConst.has_value() && *lhsConst == 0) {
                makeCopy(instruction, rhs);
                copies[dst] = rhs;
                return true;
            }
            break;
        case ir::BinaryOpcode::Sub:
            if (rhsConst.has_value() && *rhsConst == 0) {
                makeCopy(instruction, lhs);
                copies[dst] = lhs;
                return true;
            }
            if (sameOperand(lhs, rhs)) {
                makeConst(instruction, 0);
                constants[dst] = 0;
                return true;
            }
            break;
        case ir::BinaryOpcode::Mul:
            if (rhsConst.has_value() && *rhsConst == 1) {
                makeCopy(instruction, lhs);
                copies[dst] = lhs;
                return true;
            }
            if (lhsConst.has_value() && *lhsConst == 1) {
                makeCopy(instruction, rhs);
                copies[dst] = rhs;
                return true;
            }
            if ((rhsConst.has_value() && *rhsConst == 0) || (lhsConst.has_value() && *lhsConst == 0)) {
                makeConst(instruction, 0);
                constants[dst] = 0;
                return true;
            }
            break;
        case ir::BinaryOpcode::Div:
            if (rhsConst.has_value() && *rhsConst == 1) {
                makeCopy(instruction, lhs);
                copies[dst] = lhs;
                return true;
            }
            if (sameOperand(lhs, rhs)) {
                makeConst(instruction, 1);
                constants[dst] = 1;
                return true;
            }
            break;
        case ir::BinaryOpcode::Mod:
            if (rhsConst.has_value() && *rhsConst == 1) {
                makeConst(instruction, 0);
                constants[dst] = 0;
                return true;
            }
            if (sameOperand(lhs, rhs)) {
                makeConst(instruction, 0);
                constants[dst] = 0;
                return true;
            }
            break;
        case ir::BinaryOpcode::Equal:
        case ir::BinaryOpcode::LessEqual:
        case ir::BinaryOpcode::GreaterEqual:
            if (sameOperand(lhs, rhs)) {
                makeConst(instruction, 1);
                constants[dst] = 1;
                return true;
            }
            break;
        case ir::BinaryOpcode::NotEqual:
        case ir::BinaryOpcode::Less:
        case ir::BinaryOpcode::Greater:
            if (sameOperand(lhs, rhs)) {
                makeConst(instruction, 0);
                constants[dst] = 0;
                return true;
            }
            break;
        case ir::BinaryOpcode::LogicalAnd:
            if ((lhsConst.has_value() && *lhsConst == 0) || (rhsConst.has_value() && *rhsConst == 0)) {
                makeConst(instruction, 0);
                constants[dst] = 0;
                return true;
            }
            break;
        case ir::BinaryOpcode::LogicalOr:
            if ((lhsConst.has_value() && *lhsConst != 0) || (rhsConst.has_value() && *rhsConst != 0)) {
                makeConst(instruction, 1);
                constants[dst] = 1;
                return true;
            }
            break;
        }

        return false;
    }
};

class DeadInstructionPass final : public IrPass {
public:
    std::string name() const override
    {
        return "dead-instruction";
    }

    bool run(ir::Module& module) override
    {
        bool changed = false;
        for (ir::Function& function : module.functions) {
            std::unordered_map<int, int> uses;
            countUses(function, uses);
            for (ir::BasicBlock& block : function.blocks) {
                auto oldSize = block.instructions.size();
                block.instructions.erase(
                    std::remove_if(block.instructions.begin(), block.instructions.end(), [&](const ir::Instruction& instruction) {
                        return removable(instruction) && uses[instruction.dst.id] == 0;
                    }),
                    block.instructions.end());
                changed = changed || oldSize != block.instructions.size();
            }
        }
        return changed;
    }

private:
    void countOperand(const ir::Operand& operand, std::unordered_map<int, int>& uses) const
    {
        if (!operand.isImmediate && operand.value.id >= 0) {
            ++uses[operand.value.id];
        }
    }

    void countUses(const ir::Function& function, std::unordered_map<int, int>& uses) const
    {
        for (const ir::BasicBlock& block : function.blocks) {
            for (const ir::Instruction& instruction : block.instructions) {
                for (const ir::Operand& operand : instruction.operands) {
                    countOperand(operand, uses);
                }
            }
            if (block.hasTerminator) {
                countOperand(block.terminator.condition, uses);
                countOperand(block.terminator.returnValue, uses);
            }
        }
    }

    bool removable(const ir::Instruction& instruction) const
    {
        if (instruction.dst.id < 0 || instruction.hasSideEffect) {
            return false;
        }
        switch (instruction.kind) {
        case ir::InstructionKind::Const:
        case ir::InstructionKind::Copy:
        case ir::InstructionKind::Unary:
        case ir::InstructionKind::Binary:
        case ir::InstructionKind::LoadGlobal:
        case ir::InstructionKind::LoadLocal:
            return true;
        case ir::InstructionKind::StoreGlobal:
        case ir::InstructionKind::StoreLocal:
        case ir::InstructionKind::Call:
            return false;
        }
        return false;
    }
};

} // namespace

void PassManager::add(std::unique_ptr<IrPass> pass)
{
    passes_.push_back(std::move(pass));
}

bool PassManager::run(ir::Module& module)
{
    bool changed = false;
    for (const auto& pass : passes_) {
        changed = pass->run(module) || changed;
    }
    return changed;
}

PassManager buildDefaultPassPipeline(bool optimize)
{
    PassManager manager;
    if (optimize) {
        manager.add(std::make_unique<LocalSimplifyPass>());
        manager.add(std::make_unique<DeadInstructionPass>());
        manager.add(std::make_unique<LocalSimplifyPass>());
        manager.add(std::make_unique<DeadInstructionPass>());
    }
    return manager;
}

} // namespace toyc
