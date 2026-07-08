#include "pass_manager.h"

#include "analysis/cfg.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace toyc::passes {
namespace {

struct ExprInfo {
    int value = -1;
};

using ExprMap = std::unordered_map<std::string, ExprInfo>;

std::string operandKey(const ir::Operand& operand)
{
    if (operand.isImmediate) {
        return "#" + std::to_string(operand.immediate);
    }
    return "%" + std::to_string(operand.value.id);
}

bool isCommutative(ir::BinaryOpcode op)
{
    switch (op) {
    case ir::BinaryOpcode::Add:
    case ir::BinaryOpcode::Mul:
    case ir::BinaryOpcode::Equal:
    case ir::BinaryOpcode::NotEqual:
    case ir::BinaryOpcode::LogicalAnd:
    case ir::BinaryOpcode::LogicalOr:
        return true;
    case ir::BinaryOpcode::Sub:
    case ir::BinaryOpcode::Div:
    case ir::BinaryOpcode::Mod:
    case ir::BinaryOpcode::Less:
    case ir::BinaryOpcode::LessEqual:
    case ir::BinaryOpcode::Greater:
    case ir::BinaryOpcode::GreaterEqual:
        return false;
    }
    return false;
}

std::string expressionKey(const ir::Instruction& inst)
{
    if (inst.kind != ir::InstructionKind::Binary || inst.dst.id < 0 || inst.operands.size() != 2) {
        return {};
    }

    std::string lhs = operandKey(inst.operands[0]);
    std::string rhs = operandKey(inst.operands[1]);
    if (isCommutative(inst.binaryOp) && rhs < lhs) {
        std::swap(lhs, rhs);
    }

    std::ostringstream key;
    key << "bin:" << static_cast<int>(inst.binaryOp) << ':' << lhs << ':' << rhs;
    return key.str();
}

void transferInstruction(ExprMap& expressions, const ir::Instruction& inst)
{
    const std::string key = expressionKey(inst);
    if (!key.empty()) {
        expressions[key] = ExprInfo{inst.dst.id};
    }
}

ExprMap transferBlock(ExprMap expressions, const ir::BasicBlock& block)
{
    for (const ir::Instruction& inst : block.instructions) {
        transferInstruction(expressions, inst);
    }
    return expressions;
}

ExprMap intersectExpressions(const ExprMap& lhs, const ExprMap& rhs)
{
    ExprMap result = lhs;
    for (auto iter = result.begin(); iter != result.end();) {
        const auto found = rhs.find(iter->first);
        if (found == rhs.end() || found->second.value != iter->second.value) {
            iter = result.erase(iter);
        } else {
            ++iter;
        }
    }
    return result;
}

bool usesValue(const ir::Instruction& inst, int value)
{
    for (const ir::Operand& operand : inst.operands) {
        if (!operand.isImmediate && operand.value.id == value) {
            return true;
        }
    }
    return false;
}

bool rewriteBlock(ir::BasicBlock& block, ExprMap expressions)
{
    bool changed = false;
    for (ir::Instruction& inst : block.instructions) {
        const std::string key = expressionKey(inst);
        if (!key.empty()) {
            const auto found = expressions.find(key);
            if (found != expressions.end()
                && found->second.value != inst.dst.id
                && !usesValue(inst, inst.dst.id)) {
                inst.kind = ir::InstructionKind::Copy;
                inst.operands = {ir::Operand::ref(ir::Value{found->second.value})};
                inst.symbol.clear();
                changed = true;
            } else {
                expressions[key] = ExprInfo{inst.dst.id};
            }
        }
    }
    return changed;
}

bool runOnFunction(ir::Function& function)
{
    if (function.blocks.empty()) {
        return false;
    }

    const analysis::Cfg cfg = analysis::buildCfg(function);
    std::vector<ExprMap> in(function.blocks.size());
    std::vector<ExprMap> out(function.blocks.size());

    bool dataChanged = true;
    for (int iteration = 0; dataChanged && iteration < 32; ++iteration) {
        dataChanged = false;
        for (int b = 0; b < static_cast<int>(function.blocks.size()); ++b) {
            ExprMap nextIn;
            const auto& preds = cfg.predecessors[static_cast<std::size_t>(b)];
            if (b != 0 && !preds.empty()) {
                nextIn = out[static_cast<std::size_t>(preds.front())];
                for (std::size_t i = 1; i < preds.size(); ++i) {
                    nextIn = intersectExpressions(nextIn, out[static_cast<std::size_t>(preds[i])]);
                }
            }

            ExprMap nextOut = transferBlock(nextIn, function.blocks[static_cast<std::size_t>(b)]);
            if (nextIn.size() != in[static_cast<std::size_t>(b)].size()
                || nextOut.size() != out[static_cast<std::size_t>(b)].size()
                || intersectExpressions(nextIn, in[static_cast<std::size_t>(b)]).size() != nextIn.size()
                || intersectExpressions(nextOut, out[static_cast<std::size_t>(b)]).size() != nextOut.size()) {
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

class GlobalCsePass final : public Pass {
public:
    std::string name() const override { return "global-cse"; }

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

std::unique_ptr<Pass> createGlobalCsePass()
{
    return std::make_unique<GlobalCsePass>();
}

} // namespace toyc::passes
