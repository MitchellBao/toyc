#include "pass_manager.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace toyc::passes {
namespace {

constexpr std::size_t kMaxInlineInstructions = 120;

ir::Value newValue(ir::Function& function)
{
    return ir::Value{function.nextValue++};
}

bool isInlineCandidate(const ir::Function& function)
{
    if (function.blocks.size() != 1 || function.returnType != ir::Type::Int) {
        return false;
    }
    const ir::BasicBlock& block = function.blocks.front();
    if (block.instructions.size() > kMaxInlineInstructions
        || block.terminator.kind != ir::TerminatorKind::Return
        || !block.terminator.hasReturnValue) {
        return false;
    }
    for (const ir::Instruction& inst : block.instructions) {
        if (inst.kind == ir::InstructionKind::Call || inst.kind == ir::InstructionKind::StoreGlobal) {
            return false;
        }
    }
    return true;
}

ir::Operand remapOperand(const ir::Operand& operand, const std::unordered_map<int, ir::Value>& values)
{
    if (operand.isImmediate) {
        return operand;
    }
    const auto found = values.find(operand.value.id);
    if (found == values.end()) {
        return operand;
    }
    return ir::Operand::ref(found->second);
}

std::string remapSymbol(
    const std::string& symbol,
    const std::string& prefix,
    std::unordered_map<std::string, std::string>& symbols)
{
    if (symbol.empty()) {
        return symbol;
    }
    const auto found = symbols.find(symbol);
    if (found != symbols.end()) {
        return found->second;
    }
    const std::string renamed = prefix + symbol;
    symbols.emplace(symbol, renamed);
    return renamed;
}

std::vector<ir::Instruction> inlineCall(ir::Function& caller, const ir::Instruction& call, const ir::Function& callee)
{
    std::vector<ir::Instruction> result;
    std::unordered_map<int, ir::Value> values;
    std::unordered_map<std::string, std::string> symbols;
    const std::string prefix = "__inl_" + callee.name + "_" + std::to_string(call.dst.id) + "_";

    result.reserve(callee.params.size() + callee.blocks.front().instructions.size() + 1);
    for (std::size_t i = 0; i < callee.params.size(); ++i) {
        ir::Instruction store;
        store.kind = ir::InstructionKind::StoreLocal;
        store.symbol = remapSymbol(callee.params[i], prefix, symbols);
        store.operands = {call.operands[i]};
        result.push_back(std::move(store));
    }

    for (const ir::Instruction& inst : callee.blocks.front().instructions) {
        ir::Instruction cloned = inst;
        if (cloned.dst.id >= 0) {
            cloned.dst = newValue(caller);
            values.emplace(inst.dst.id, cloned.dst);
        }
        for (ir::Operand& operand : cloned.operands) {
            operand = remapOperand(operand, values);
        }
        if (cloned.kind == ir::InstructionKind::LoadLocal || cloned.kind == ir::InstructionKind::StoreLocal) {
            cloned.symbol = remapSymbol(cloned.symbol, prefix, symbols);
        }
        result.push_back(std::move(cloned));
    }

    ir::Instruction copyReturn;
    copyReturn.kind = ir::InstructionKind::Copy;
    copyReturn.dst = call.dst;
    copyReturn.operands = {remapOperand(callee.blocks.front().terminator.returnValue, values)};
    result.push_back(std::move(copyReturn));
    return result;
}

class InlineSmallPass final : public Pass {
public:
    std::string name() const override { return "inline-small"; }
    bool run(ir::Module& module) override
    {
        std::unordered_map<std::string, const ir::Function*> candidates;
        for (const ir::Function& function : module.functions) {
            if (isInlineCandidate(function)) {
                candidates.emplace(function.name, &function);
            }
        }

        bool changed = false;
        for (ir::Function& function : module.functions) {
            for (ir::BasicBlock& block : function.blocks) {
                std::vector<ir::Instruction> rewritten;
                rewritten.reserve(block.instructions.size());
                for (const ir::Instruction& inst : block.instructions) {
                    const auto found = inst.kind == ir::InstructionKind::Call ? candidates.find(inst.symbol) : candidates.end();
                    if (found == candidates.end()
                        || found->second->name == function.name
                        || inst.operands.size() != found->second->params.size()
                        || inst.dst.id < 0) {
                        rewritten.push_back(inst);
                        continue;
                    }
                    std::vector<ir::Instruction> expanded = inlineCall(function, inst, *found->second);
                    rewritten.insert(rewritten.end(), expanded.begin(), expanded.end());
                    changed = true;
                }
                block.instructions = std::move(rewritten);
            }
        }
        return changed;
    }
};

} // namespace

std::unique_ptr<Pass> createInlineSmallPass()
{
    return std::make_unique<InlineSmallPass>();
}

} // namespace toyc::passes
