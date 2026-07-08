#include "pass_manager.h"

#include <cstddef>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace toyc::passes {
namespace {

constexpr std::size_t kMaxInlineInstructions = 400;
constexpr int kMaxInlineRounds = 6;

ir::Value newValue(ir::Function& function)
{
    return ir::Value{function.nextValue++};
}

std::vector<std::string> calleesOf(const ir::Function& function)
{
    std::vector<std::string> result;
    for (const ir::BasicBlock& block : function.blocks) {
        for (const ir::Instruction& inst : block.instructions) {
            if (inst.kind == ir::InstructionKind::Call && !inst.symbol.empty()) {
                result.push_back(inst.symbol);
            }
        }
    }
    return result;
}

std::unordered_set<std::string> recursiveFunctions(const ir::Module& module)
{
    std::unordered_map<std::string, std::vector<std::string>> graph;
    for (const ir::Function& function : module.functions) {
        graph[function.name] = calleesOf(function);
    }

    std::unordered_map<std::string, int> index;
    std::unordered_map<std::string, int> lowlink;
    std::vector<std::string> stack;
    std::unordered_set<std::string> onStack;
    std::unordered_set<std::string> recursive;
    int nextIndex = 0;

    auto strongConnect = [&](auto&& self, const std::string& name) -> void {
        index[name] = nextIndex;
        lowlink[name] = nextIndex;
        ++nextIndex;
        stack.push_back(name);
        onStack.insert(name);

        for (const std::string& callee : graph[name]) {
            if (graph.find(callee) == graph.end()) {
                continue;
            }
            if (index.find(callee) == index.end()) {
                self(self, callee);
                lowlink[name] = std::min(lowlink[name], lowlink[callee]);
            } else if (onStack.find(callee) != onStack.end()) {
                lowlink[name] = std::min(lowlink[name], index[callee]);
            }
        }

        if (lowlink[name] != index[name]) {
            return;
        }

        std::vector<std::string> component;
        while (!stack.empty()) {
            const std::string member = stack.back();
            stack.pop_back();
            onStack.erase(member);
            component.push_back(member);
            if (member == name) {
                break;
            }
        }

        if (component.size() > 1) {
            recursive.insert(component.begin(), component.end());
            return;
        }
        const std::string& only = component.front();
        const auto found = graph.find(only);
        if (found != graph.end()
            && std::find(found->second.begin(), found->second.end(), only) != found->second.end()) {
            recursive.insert(only);
        }
    };

    for (const ir::Function& function : module.functions) {
        if (index.find(function.name) == index.end()) {
            strongConnect(strongConnect, function.name);
        }
    }
    return recursive;
}

bool isInlineCandidate(const ir::Function& function, const std::unordered_set<std::string>& recursive)
{
    if (recursive.find(function.name) != recursive.end()
        || function.blocks.size() != 1
        || function.returnType != ir::Type::Int) {
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

std::unordered_map<std::string, const ir::Function*> collectCandidates(const ir::Module& module)
{
    const std::unordered_set<std::string> recursive = recursiveFunctions(module);
    std::unordered_map<std::string, const ir::Function*> candidates;
    for (const ir::Function& function : module.functions) {
        if (isInlineCandidate(function, recursive)) {
            candidates.emplace(function.name, &function);
        }
    }
    return candidates;
}

bool inlineOneRound(ir::Module& module)
{
    const std::unordered_map<std::string, const ir::Function*> candidates = collectCandidates(module);
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

bool eliminateDeadFunctions(ir::Module& module)
{
    std::unordered_map<std::string, const ir::Function*> byName;
    for (const ir::Function& function : module.functions) {
        byName.emplace(function.name, &function);
    }

    std::unordered_set<std::string> reachable;
    std::vector<std::string> worklist;
    if (byName.find("main") != byName.end()) {
        reachable.insert("main");
        worklist.push_back("main");
    }

    while (!worklist.empty()) {
        const std::string current = worklist.back();
        worklist.pop_back();
        const auto found = byName.find(current);
        if (found == byName.end()) {
            continue;
        }
        for (const std::string& callee : calleesOf(*found->second)) {
            if (byName.find(callee) != byName.end() && reachable.insert(callee).second) {
                worklist.push_back(callee);
            }
        }
    }

    const std::size_t before = module.functions.size();
    module.functions.erase(
        std::remove_if(module.functions.begin(), module.functions.end(), [&](const ir::Function& function) {
            return function.name != "main" && reachable.find(function.name) == reachable.end();
        }),
        module.functions.end());
    return module.functions.size() != before;
}

class InlineSmallPass final : public Pass {
public:
    std::string name() const override { return "inline-small"; }
    bool run(ir::Module& module) override
    {
        bool changed = false;
        for (int round = 0; round < kMaxInlineRounds; ++round) {
            if (!inlineOneRound(module)) {
                break;
            }
            changed = true;
        }
        changed = eliminateDeadFunctions(module) || changed;
        return changed;
    }
};

} // namespace

std::unique_ptr<Pass> createInlineSmallPass()
{
    return std::make_unique<InlineSmallPass>();
}

} // namespace toyc::passes
