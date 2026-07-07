#include "pass.h"

#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>

namespace toyc {
namespace {

std::string operandKey(const ir::Operand& operand)
{
    if (operand.isImmediate) {
        return "i" + std::to_string(operand.immediate);
    }
    return "v" + std::to_string(operand.value.id);
}

void normalizeCommutative(ir::BinaryOpcode op, std::string& lhs, std::string& rhs)
{
    switch (op) {
    case ir::BinaryOpcode::Add:
    case ir::BinaryOpcode::Mul:
    case ir::BinaryOpcode::Equal:
    case ir::BinaryOpcode::NotEqual:
    case ir::BinaryOpcode::LogicalAnd:
    case ir::BinaryOpcode::LogicalOr:
        if (rhs < lhs) {
            std::swap(lhs, rhs);
        }
        break;
    default:
        break;
    }
}

std::string instructionKey(const ir::Instruction& instruction)
{
    std::ostringstream key;
    if (instruction.kind == ir::InstructionKind::Unary && instruction.operands.size() == 1) {
        key << "u:" << static_cast<int>(instruction.unaryOp) << ':' << operandKey(instruction.operands[0]);
        return key.str();
    }

    if (instruction.kind == ir::InstructionKind::Binary && instruction.operands.size() == 2) {
        std::string lhs = operandKey(instruction.operands[0]);
        std::string rhs = operandKey(instruction.operands[1]);
        normalizeCommutative(instruction.binaryOp, lhs, rhs);
        key << "b:" << static_cast<int>(instruction.binaryOp) << ':' << lhs << ':' << rhs;
        return key.str();
    }

    return {};
}

bool isCseCandidate(const ir::Instruction& instruction)
{
    return instruction.dst.id >= 0
        && !instruction.hasSideEffect
        && (instruction.kind == ir::InstructionKind::Unary || instruction.kind == ir::InstructionKind::Binary);
}

void makeCopy(ir::Instruction& instruction, ir::Value value)
{
    instruction.kind = ir::InstructionKind::Copy;
    instruction.operands.clear();
    instruction.operands.push_back(ir::Operand::ref(value));
    instruction.symbol.clear();
    instruction.hasSideEffect = false;
}

class LocalCsePass final : public IrPass {
public:
    std::string name() const override
    {
        return "local-cse";
    }

    bool run(ir::Module& module) override
    {
        bool changed = false;
        for (ir::Function& function : module.functions) {
            for (ir::BasicBlock& block : function.blocks) {
                changed = runBlock(block) || changed;
            }
        }
        return changed;
    }

private:
    bool runBlock(ir::BasicBlock& block)
    {
        bool changed = false;
        std::unordered_map<std::string, ir::Value> available;

        for (ir::Instruction& instruction : block.instructions) {
            if (!isCseCandidate(instruction)) {
                continue;
            }

            const std::string key = instructionKey(instruction);
            if (key.empty()) {
                continue;
            }

            const auto found = available.find(key);
            if (found != available.end()) {
                makeCopy(instruction, found->second);
                changed = true;
            } else {
                available.emplace(key, instruction.dst);
            }
        }

        return changed;
    }
};

} // namespace

std::unique_ptr<IrPass> createLocalCsePass()
{
    return std::make_unique<LocalCsePass>();
}

} // namespace toyc
