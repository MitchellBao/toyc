#include "pass_manager.h"

#include "analysis/cfg.h"

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
constexpr std::size_t kMaxVoidInlineInstructions = 12;
constexpr int kMaxInlineRounds = 6;
constexpr int kMaxInlineSitesPerRound = 1024;

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
    if (recursive.find(function.name) != recursive.end()) {
        return false;
    }

    std::size_t instructions = 0;
    bool hasReturn = false;
    for (const ir::BasicBlock& block : function.blocks) {
        instructions += block.instructions.size();
        if (block.terminator.kind == ir::TerminatorKind::Return) {
            if (function.returnType == ir::Type::Int && !block.terminator.hasReturnValue) {
                return false;
            }
            if (function.returnType == ir::Type::Void && block.terminator.hasReturnValue) {
                return false;
            }
            hasReturn = true;
        }
        for (const ir::Instruction& inst : block.instructions) {
            if (inst.kind == ir::InstructionKind::Call) {
                return false;
            }
        }
    }
    if (instructions > kMaxInlineInstructions || !hasReturn) {
        return false;
    }

    const analysis::Cfg cfg = analysis::buildCfg(function);
    enum class VisitState { Unvisited, Visiting, Done };
    std::vector<VisitState> states(function.blocks.size(), VisitState::Unvisited);
    auto hasCycle = [&](auto&& self, int block) -> bool {
        states[static_cast<std::size_t>(block)] = VisitState::Visiting;
        for (int succ : cfg.successors[static_cast<std::size_t>(block)]) {
            if (succ < 0 || succ >= static_cast<int>(function.blocks.size())) {
                continue;
            }
            const auto state = states[static_cast<std::size_t>(succ)];
            if (state == VisitState::Visiting) {
                return true;
            }
            if (state == VisitState::Unvisited && self(self, succ)) {
                return true;
            }
        }
        states[static_cast<std::size_t>(block)] = VisitState::Done;
        return false;
    };
    for (int i = 0; i < static_cast<int>(function.blocks.size()); ++i) {
        if (states[static_cast<std::size_t>(i)] == VisitState::Unvisited && hasCycle(hasCycle, i)) {
            return false;
        }
    }

    return true;
}

bool isNarrowVoidInlineCandidate(const ir::Function& function)
{
    if (function.returnType != ir::Type::Void || function.blocks.size() > 2) {
        return false;
    }

    std::size_t instructions = 0;
    bool sawStoreGlobal = false;
    for (const ir::BasicBlock& block : function.blocks) {
        instructions += block.instructions.size();
        if (block.terminator.kind == ir::TerminatorKind::Return && block.terminator.hasReturnValue) {
            return false;
        }
        for (const ir::Instruction& inst : block.instructions) {
            switch (inst.kind) {
            case ir::InstructionKind::Const:
            case ir::InstructionKind::Copy:
            case ir::InstructionKind::LoadLocal:
            case ir::InstructionKind::StoreLocal:
            case ir::InstructionKind::Unary:
            case ir::InstructionKind::Binary:
                break;
            case ir::InstructionKind::StoreGlobal:
                sawStoreGlobal = true;
                break;
            case ir::InstructionKind::LoadGlobal:
            case ir::InstructionKind::Call:
                return false;
            }
        }
    }
    return sawStoreGlobal && instructions <= kMaxVoidInlineInstructions;
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

ir::Terminator remapTerminator(
    const ir::Terminator& term,
    int inlineBase,
    int continuationIndex,
    const std::string& returnSymbol,
    const std::unordered_map<int, ir::Value>& values,
    std::vector<ir::Instruction>& instructions)
{
    ir::Terminator remapped = term;
    if (term.kind == ir::TerminatorKind::Jump) {
        remapped.trueBlock = inlineBase + term.trueBlock;
        return remapped;
    }
    if (term.kind == ir::TerminatorKind::Branch) {
        remapped.condition = remapOperand(term.condition, values);
        remapped.trueBlock = inlineBase + term.trueBlock;
        remapped.falseBlock = inlineBase + term.falseBlock;
        return remapped;
    }

    if (term.hasReturnValue) {
        ir::Instruction storeReturn;
        storeReturn.kind = ir::InstructionKind::StoreLocal;
        storeReturn.symbol = returnSymbol;
        storeReturn.operands = {remapOperand(term.returnValue, values)};
        instructions.push_back(std::move(storeReturn));
    }
    remapped.kind = ir::TerminatorKind::Jump;
    remapped.trueBlock = continuationIndex;
    remapped.falseBlock = -1;
    remapped.condition = {};
    remapped.returnValue = {};
    remapped.hasReturnValue = false;
    return remapped;
}

void inlineCallAt(ir::Function& caller, int blockIndex, int instructionIndex, const ir::Function& callee)
{
    ir::BasicBlock& callBlock = caller.blocks[static_cast<std::size_t>(blockIndex)];
    const ir::Instruction call = callBlock.instructions[static_cast<std::size_t>(instructionIndex)];
    std::vector<ir::Instruction> suffix(
        std::make_move_iterator(callBlock.instructions.begin() + instructionIndex + 1),
        std::make_move_iterator(callBlock.instructions.end()));
    callBlock.instructions.erase(callBlock.instructions.begin() + instructionIndex, callBlock.instructions.end());
    const ir::Terminator continuationTerminator = callBlock.terminator;
    const bool continuationHasTerminator = callBlock.hasTerminator;

    const int inlineBase = static_cast<int>(caller.blocks.size());
    const int continuationIndex = inlineBase + static_cast<int>(callee.blocks.size());
    callBlock.terminator.kind = ir::TerminatorKind::Jump;
    callBlock.terminator.trueBlock = inlineBase;
    callBlock.terminator.falseBlock = -1;
    callBlock.terminator.condition = {};
    callBlock.terminator.returnValue = {};
    callBlock.terminator.hasReturnValue = false;
    callBlock.hasTerminator = true;

    std::unordered_map<int, ir::Value> values;
    std::unordered_map<std::string, std::string> symbols;
    const std::string prefix = "__inl_" + callee.name + "_" + std::to_string(blockIndex) + "_" + std::to_string(instructionIndex) + "_";
    for (const ir::BasicBlock& block : callee.blocks) {
        for (const ir::Instruction& inst : block.instructions) {
            if (inst.dst.id >= 0) {
                values.emplace(inst.dst.id, newValue(caller));
            }
        }
    }

    std::vector<ir::Instruction> paramStores;
    paramStores.reserve(callee.params.size());
    for (std::size_t i = 0; i < callee.params.size(); ++i) {
        ir::Instruction store;
        store.kind = ir::InstructionKind::StoreLocal;
        store.symbol = remapSymbol(callee.params[i], prefix, symbols);
        store.operands = {call.operands[i]};
        paramStores.push_back(std::move(store));
    }
    const std::string returnSymbol = prefix + "__return";

    std::vector<ir::BasicBlock> newBlocks;
    newBlocks.reserve(callee.blocks.size() + 1);
    for (int i = 0; i < static_cast<int>(callee.blocks.size()); ++i) {
        const ir::BasicBlock& block = callee.blocks[static_cast<std::size_t>(i)];
        ir::BasicBlock clonedBlock;
        clonedBlock.label = prefix + block.label;
        if (i == 0) {
            clonedBlock.instructions.insert(
                clonedBlock.instructions.end(),
                std::make_move_iterator(paramStores.begin()),
                std::make_move_iterator(paramStores.end()));
        }
        clonedBlock.instructions.reserve(clonedBlock.instructions.size() + block.instructions.size() + 1);
        for (const ir::Instruction& inst : block.instructions) {
            ir::Instruction cloned = inst;
            if (cloned.dst.id >= 0) {
                cloned.dst = values.at(inst.dst.id);
            }
            for (ir::Operand& operand : cloned.operands) {
                operand = remapOperand(operand, values);
            }
            if (cloned.kind == ir::InstructionKind::LoadLocal || cloned.kind == ir::InstructionKind::StoreLocal) {
                cloned.symbol = remapSymbol(cloned.symbol, prefix, symbols);
            }
            clonedBlock.instructions.push_back(std::move(cloned));
        }
        clonedBlock.terminator = remapTerminator(
            block.terminator,
            inlineBase,
            continuationIndex,
            returnSymbol,
            values,
            clonedBlock.instructions);
        clonedBlock.hasTerminator = true;
        newBlocks.push_back(std::move(clonedBlock));
    }

    ir::BasicBlock continuation;
    continuation.label = prefix + "cont";
    continuation.instructions = std::move(suffix);
    if (call.dst.id >= 0) {
        ir::Instruction loadReturn;
        loadReturn.kind = ir::InstructionKind::LoadLocal;
        loadReturn.dst = call.dst;
        loadReturn.symbol = returnSymbol;
        continuation.instructions.insert(continuation.instructions.begin(), std::move(loadReturn));
    }
    continuation.terminator = continuationTerminator;
    continuation.hasTerminator = continuationHasTerminator;
    newBlocks.push_back(std::move(continuation));

    caller.blocks.insert(
        caller.blocks.end(),
        std::make_move_iterator(newBlocks.begin()),
        std::make_move_iterator(newBlocks.end()));
}

std::unordered_map<std::string, const ir::Function*> collectCandidates(const ir::Module& module)
{
    const std::unordered_set<std::string> recursive = recursiveFunctions(module);
    std::unordered_map<std::string, const ir::Function*> candidates;
    for (const ir::Function& function : module.functions) {
        if (isInlineCandidate(function, recursive)
            && (function.returnType != ir::Type::Void || isNarrowVoidInlineCandidate(function))) {
            candidates.emplace(function.name, &function);
        }
    }
    return candidates;
}

bool inlineFirstEligibleCall(ir::Module& module)
{
    const std::unordered_map<std::string, const ir::Function*> candidates = collectCandidates(module);
    for (ir::Function& function : module.functions) {
        const int blockCount = static_cast<int>(function.blocks.size());
        for (int blockIndex = 0; blockIndex < blockCount; ++blockIndex) {
            ir::BasicBlock& block = function.blocks[static_cast<std::size_t>(blockIndex)];
            for (int instructionIndex = 0; instructionIndex < static_cast<int>(block.instructions.size()); ++instructionIndex) {
                const ir::Instruction& inst = block.instructions[static_cast<std::size_t>(instructionIndex)];
                const auto found = inst.kind == ir::InstructionKind::Call ? candidates.find(inst.symbol) : candidates.end();
                if (found == candidates.end()
                    || found->second->name == function.name
                    || inst.operands.size() != found->second->params.size()
                    || (found->second->returnType == ir::Type::Int && inst.dst.id < 0)) {
                    continue;
                }
                inlineCallAt(function, blockIndex, instructionIndex, *found->second);
                return true;
            }
        }
    }
    return false;
}

bool inlineOneRound(ir::Module& module)
{
    bool changed = false;
    for (int sites = 0; sites < kMaxInlineSitesPerRound; ++sites) {
        if (!inlineFirstEligibleCall(module)) {
            break;
        }
        changed = true;
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
