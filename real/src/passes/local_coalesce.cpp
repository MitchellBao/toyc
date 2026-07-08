#include "pass_manager.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace toyc::passes {
namespace {

struct LocalStats {
    int loads = 0;
    int stores = 0;
    int firstStoreBlock = -1;
    int firstStoreIndex = -1;
    int sourceLoadValue = -1;
    std::string source;
};

void recordLocal(std::unordered_map<std::string, LocalStats>& stats, const std::string& symbol)
{
    if (!symbol.empty()) {
        (void)stats[symbol];
    }
}

bool replaceSymbol(ir::Instruction& inst, const std::unordered_map<std::string, std::string>& aliases)
{
    if ((inst.kind != ir::InstructionKind::LoadLocal && inst.kind != ir::InstructionKind::StoreLocal)
        || inst.symbol.empty()) {
        return false;
    }
    const auto found = aliases.find(inst.symbol);
    if (found == aliases.end()) {
        return false;
    }
    inst.symbol = found->second;
    return true;
}

bool removeNoOpLocalStores(ir::BasicBlock& block)
{
    bool changed = false;
    std::unordered_map<int, std::string> loadedFrom;
    std::vector<ir::Instruction> kept;
    kept.reserve(block.instructions.size());

    auto invalidateLocal = [&](const std::string& symbol) {
        for (auto iter = loadedFrom.begin(); iter != loadedFrom.end();) {
            if (iter->second == symbol) {
                iter = loadedFrom.erase(iter);
            } else {
                ++iter;
            }
        }
    };

    for (ir::Instruction& inst : block.instructions) {
        if ((inst.kind == ir::InstructionKind::Call || inst.kind == ir::InstructionKind::StoreGlobal || inst.hasSideEffect)) {
            loadedFrom.clear();
        }

        if (inst.kind == ir::InstructionKind::StoreLocal && !inst.symbol.empty()) {
            const bool noOp = inst.operands.size() == 1
                && !inst.operands[0].isImmediate
                && inst.operands[0].value.id >= 0
                && loadedFrom.find(inst.operands[0].value.id) != loadedFrom.end()
                && loadedFrom[inst.operands[0].value.id] == inst.symbol;
            if (noOp) {
                changed = true;
                continue;
            }
            invalidateLocal(inst.symbol);
        }

        if (inst.kind == ir::InstructionKind::LoadLocal && inst.dst.id >= 0 && !inst.symbol.empty()) {
            loadedFrom[inst.dst.id] = inst.symbol;
        } else if (inst.dst.id >= 0) {
            loadedFrom.erase(inst.dst.id);
        }
        kept.push_back(inst);
    }

    if (changed) {
        block.instructions = std::move(kept);
    }
    return changed;
}

std::unordered_map<int, std::string> collectLoadSources(const ir::Function& function)
{
    std::unordered_map<int, std::string> sources;
    for (const ir::BasicBlock& block : function.blocks) {
        for (const ir::Instruction& inst : block.instructions) {
            if (inst.kind == ir::InstructionKind::LoadLocal && inst.dst.id >= 0 && !inst.symbol.empty()) {
                sources[inst.dst.id] = inst.symbol;
            }
        }
    }
    return sources;
}

std::unordered_map<std::string, std::string> collectAliases(const ir::Function& function)
{
    std::unordered_map<std::string, LocalStats> stats;
    const std::unordered_map<int, std::string> loadSources = collectLoadSources(function);
    std::unordered_set<std::string> params(function.params.begin(), function.params.end());

    for (int blockIndex = 0; blockIndex < static_cast<int>(function.blocks.size()); ++blockIndex) {
        const ir::BasicBlock& block = function.blocks[static_cast<std::size_t>(blockIndex)];
        for (int instIndex = 0; instIndex < static_cast<int>(block.instructions.size()); ++instIndex) {
            const ir::Instruction& inst = block.instructions[static_cast<std::size_t>(instIndex)];
            if (inst.kind == ir::InstructionKind::LoadLocal) {
                recordLocal(stats, inst.symbol);
                ++stats[inst.symbol].loads;
                continue;
            }
            if (inst.kind != ir::InstructionKind::StoreLocal || inst.symbol.empty()) {
                continue;
            }

            LocalStats& local = stats[inst.symbol];
            ++local.stores;
            if (local.firstStoreBlock >= 0) {
                continue;
            }
            local.firstStoreBlock = blockIndex;
            local.firstStoreIndex = instIndex;
            if (inst.operands.size() == 1 && !inst.operands[0].isImmediate && inst.operands[0].value.id >= 0) {
                const auto found = loadSources.find(inst.operands[0].value.id);
                if (found != loadSources.end() && found->second != inst.symbol) {
                    local.sourceLoadValue = inst.operands[0].value.id;
                    local.source = found->second;
                    recordLocal(stats, local.source);
                }
            }
        }
    }

    std::unordered_set<std::string> writtenAfterFirstAlias;
    std::unordered_set<std::string> readAfterFirstAlias;
    for (const auto& [symbol, local] : stats) {
        if (local.source.empty() || local.firstStoreBlock < 0) {
            continue;
        }
        for (int blockIndex = local.firstStoreBlock; blockIndex < static_cast<int>(function.blocks.size()); ++blockIndex) {
            const ir::BasicBlock& block = function.blocks[static_cast<std::size_t>(blockIndex)];
            const int begin = blockIndex == local.firstStoreBlock ? local.firstStoreIndex + 1 : 0;
            for (int instIndex = begin; instIndex < static_cast<int>(block.instructions.size()); ++instIndex) {
                const ir::Instruction& inst = block.instructions[static_cast<std::size_t>(instIndex)];
                if (inst.kind == ir::InstructionKind::LoadLocal && inst.symbol == local.source) {
                    readAfterFirstAlias.insert(symbol);
                }
                if (inst.kind == ir::InstructionKind::StoreLocal && inst.symbol == local.source) {
                    writtenAfterFirstAlias.insert(symbol);
                }
            }
        }
    }

    std::unordered_map<std::string, std::string> aliases;
    for (const auto& [symbol, local] : stats) {
        if (local.source.empty()
            || params.find(local.source) != params.end()
            || local.stores < 2
            || local.firstStoreBlock != 0
            || local.sourceLoadValue < 0
            || readAfterFirstAlias.find(symbol) != readAfterFirstAlias.end()
            || writtenAfterFirstAlias.find(symbol) != writtenAfterFirstAlias.end()) {
            continue;
        }
        aliases.emplace(symbol, local.source);
    }
    return aliases;
}

class LocalCoalescePass final : public Pass {
public:
    std::string name() const override { return "local-coalesce"; }

    bool run(ir::Module& module) override
    {
        bool changed = false;
        for (ir::Function& function : module.functions) {
            const std::unordered_map<std::string, std::string> aliases = collectAliases(function);
            for (ir::BasicBlock& block : function.blocks) {
                if (!aliases.empty()) {
                    for (ir::Instruction& inst : block.instructions) {
                        changed = replaceSymbol(inst, aliases) || changed;
                    }
                }
                changed = removeNoOpLocalStores(block) || changed;
            }
        }
        return changed;
    }
};

} // namespace

std::unique_ptr<Pass> createLocalCoalescePass()
{
    return std::make_unique<LocalCoalescePass>();
}

} // namespace toyc::passes
