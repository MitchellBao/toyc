#include "pass_manager.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace toyc::passes {
namespace {

constexpr int kMaxEvalSteps = 5000000;

std::optional<std::int32_t> evalUnary(ir::UnaryOpcode op, std::int32_t value)
{
    switch (op) {
    case ir::UnaryOpcode::Plus:
        return value;
    case ir::UnaryOpcode::Minus:
        return -value;
    case ir::UnaryOpcode::Not:
        return value == 0 ? 1 : 0;
    }
    return std::nullopt;
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

bool isEvaluableFunction(const ir::Function& function)
{
    if (function.returnType != ir::Type::Int || function.blocks.empty()) {
        return false;
    }
    for (const ir::BasicBlock& block : function.blocks) {
        for (const ir::Instruction& inst : block.instructions) {
            if (inst.kind == ir::InstructionKind::Call
                || inst.kind == ir::InstructionKind::LoadGlobal
                || inst.kind == ir::InstructionKind::StoreGlobal
                || inst.hasSideEffect) {
                return false;
            }
        }
    }
    return true;
}

class Evaluator {
public:
    explicit Evaluator(const ir::Function& function)
        : function_(function)
    {
    }

    std::optional<std::int32_t> run(const std::vector<std::int32_t>& args)
    {
        if (args.size() != function_.params.size()) {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < args.size(); ++i) {
            locals_[function_.params[i]] = args[i];
        }

        int blockIndex = 0;
        for (int steps = 0; steps < kMaxEvalSteps; ++steps) {
            if (blockIndex < 0 || blockIndex >= static_cast<int>(function_.blocks.size())) {
                return std::nullopt;
            }
            const ir::BasicBlock& block = function_.blocks[static_cast<std::size_t>(blockIndex)];
            for (const ir::Instruction& inst : block.instructions) {
                if (!execute(inst)) {
                    return std::nullopt;
                }
            }
            const ir::Terminator& term = block.terminator;
            switch (term.kind) {
            case ir::TerminatorKind::Jump:
                blockIndex = term.trueBlock;
                break;
            case ir::TerminatorKind::Branch:
                if (const auto condition = valueOf(term.condition); condition.has_value()) {
                    blockIndex = *condition != 0 ? term.trueBlock : term.falseBlock;
                    break;
                }
                return std::nullopt;
            case ir::TerminatorKind::Return:
                if (!term.hasReturnValue) {
                    return 0;
                }
                return valueOf(term.returnValue);
            }
        }
        return std::nullopt;
    }

private:
    std::optional<std::int32_t> valueOf(const ir::Operand& operand) const
    {
        if (operand.isImmediate) {
            return operand.immediate;
        }
        const auto found = values_.find(operand.value.id);
        if (found == values_.end()) {
            return std::nullopt;
        }
        return found->second;
    }

    bool execute(const ir::Instruction& inst)
    {
        switch (inst.kind) {
        case ir::InstructionKind::Const:
            if (inst.dst.id < 0 || inst.operands.empty() || !inst.operands[0].isImmediate) {
                return false;
            }
            values_[inst.dst.id] = inst.operands[0].immediate;
            return true;
        case ir::InstructionKind::Copy:
            if (inst.dst.id < 0 || inst.operands.empty()) {
                return false;
            }
            if (const auto value = valueOf(inst.operands[0]); value.has_value()) {
                values_[inst.dst.id] = *value;
                return true;
            }
            return false;
        case ir::InstructionKind::Unary:
            if (inst.dst.id < 0 || inst.operands.size() != 1) {
                return false;
            }
            if (const auto value = valueOf(inst.operands[0]); value.has_value()) {
                if (const auto result = evalUnary(inst.unaryOp, *value); result.has_value()) {
                    values_[inst.dst.id] = *result;
                    return true;
                }
            }
            return false;
        case ir::InstructionKind::Binary:
            if (inst.dst.id < 0 || inst.operands.size() != 2) {
                return false;
            }
            if (const auto lhs = valueOf(inst.operands[0]), rhs = valueOf(inst.operands[1]); lhs.has_value() && rhs.has_value()) {
                if (const auto result = evalBinary(inst.binaryOp, *lhs, *rhs); result.has_value()) {
                    values_[inst.dst.id] = *result;
                    return true;
                }
            }
            return false;
        case ir::InstructionKind::LoadLocal:
            if (inst.dst.id < 0) {
                return false;
            }
            if (const auto found = locals_.find(inst.symbol); found != locals_.end()) {
                values_[inst.dst.id] = found->second;
                return true;
            }
            return false;
        case ir::InstructionKind::StoreLocal:
            if (inst.operands.empty()) {
                return false;
            }
            if (const auto value = valueOf(inst.operands[0]); value.has_value()) {
                locals_[inst.symbol] = *value;
                return true;
            }
            return false;
        case ir::InstructionKind::Call:
        case ir::InstructionKind::LoadGlobal:
        case ir::InstructionKind::StoreGlobal:
            return false;
        }
        return false;
    }

    const ir::Function& function_;
    std::unordered_map<int, std::int32_t> values_;
    std::unordered_map<std::string, std::int32_t> locals_;
};

class ConstCallEvalPass final : public Pass {
public:
    std::string name() const override { return "const-call-eval"; }

    bool run(ir::Module& module) override
    {
        std::unordered_map<std::string, const ir::Function*> evaluable;
        for (const ir::Function& function : module.functions) {
            if (isEvaluableFunction(function)) {
                evaluable.emplace(function.name, &function);
            }
        }

        bool changed = false;
        for (ir::Function& function : module.functions) {
            for (ir::BasicBlock& block : function.blocks) {
                for (ir::Instruction& inst : block.instructions) {
                    if (inst.kind != ir::InstructionKind::Call || inst.dst.id < 0) {
                        continue;
                    }
                    const auto found = evaluable.find(inst.symbol);
                    if (found == evaluable.end()) {
                        continue;
                    }
                    std::vector<std::int32_t> args;
                    args.reserve(inst.operands.size());
                    bool allImmediate = true;
                    for (const ir::Operand& operand : inst.operands) {
                        if (!operand.isImmediate) {
                            allImmediate = false;
                            break;
                        }
                        args.push_back(operand.immediate);
                    }
                    if (!allImmediate) {
                        continue;
                    }
                    if (const auto result = Evaluator(*found->second).run(args); result.has_value()) {
                        inst.kind = ir::InstructionKind::Const;
                        inst.operands = {ir::Operand::imm(*result)};
                        inst.symbol.clear();
                        inst.hasSideEffect = false;
                        changed = true;
                    }
                }
            }
        }
        for (ir::Function& function : module.functions) {
            if (!function.params.empty() || !isEvaluableFunction(function)) {
                continue;
            }
            const auto result = Evaluator(function).run({});
            if (!result.has_value()) {
                continue;
            }

            ir::BasicBlock block;
            block.label = ".entry";
            block.hasTerminator = true;
            block.terminator.kind = ir::TerminatorKind::Return;
            block.terminator.hasReturnValue = true;
            block.terminator.returnValue = ir::Operand::imm(*result);
            function.blocks.clear();
            function.blocks.push_back(std::move(block));
            function.nextValue = 0;
            changed = true;
        }
        return changed;
    }
};

} // namespace

std::unique_ptr<Pass> createConstCallEvalPass()
{
    return std::make_unique<ConstCallEvalPass>();
}

} // namespace toyc::passes
