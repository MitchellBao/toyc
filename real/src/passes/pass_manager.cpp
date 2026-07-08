#include "pass_manager.h"

#include <cstddef>
#include <memory>
#include <ostream>
#include <utility>

namespace toyc::passes {
namespace {

struct IrStats {
    std::size_t functions = 0;
    std::size_t blocks = 0;
    std::size_t instructions = 0;
    std::size_t terminators = 0;
};

IrStats measureIr(const ir::Module& module)
{
    IrStats stats;
    stats.functions = module.functions.size();
    for (const ir::Function& function : module.functions) {
        stats.blocks += function.blocks.size();
        for (const ir::BasicBlock& block : function.blocks) {
            stats.instructions += block.instructions.size();
            if (block.hasTerminator) {
                ++stats.terminators;
            }
        }
    }
    return stats;
}

void printStatsLine(std::ostream& out, const std::string& passName, const IrStats& before, const IrStats& after, bool changed)
{
    out << "[toycc] pass=" << passName
        << " changed=" << (changed ? "yes" : "no")
        << " functions=" << before.functions << "->" << after.functions
        << " blocks=" << before.blocks << "->" << after.blocks
        << " ir_inst=" << before.instructions << "->" << after.instructions
        << " terminators=" << before.terminators << "->" << after.terminators
        << '\n';
}

} // namespace

PassManager::PassManager(bool collectStats, std::ostream* statsOut)
    : collectStats_(collectStats), statsOut_(statsOut)
{
}

void PassManager::add(std::unique_ptr<Pass> pass)
{
    passes_.push_back(std::move(pass));
}

bool PassManager::run(ir::Module& module)
{
    bool changed = false;
    for (const auto& pass : passes_) {
        const IrStats before = collectStats_ && statsOut_ != nullptr ? measureIr(module) : IrStats{};
        const bool passChanged = pass->run(module);
        if (collectStats_ && statsOut_ != nullptr) {
            printStatsLine(*statsOut_, pass->name(), before, measureIr(module), passChanged);
        }
        changed = passChanged || changed;
    }
    return changed;
}

PassManager buildPipeline(bool optimize, bool collectStats, std::ostream* statsOut)
{
    PassManager manager(collectStats, statsOut);
    manager.add(createCanonicalizePass());
    manager.add(createSimplifyCfgPass());
    if (optimize) {
        for (int iteration = 0; iteration < 4; ++iteration) {
            manager.add(createGlobalConstPropPass());
            manager.add(createConstCallEvalPass());
            manager.add(createConstPropPass());
            manager.add(createAlgebraicSimplifyPass());
            manager.add(createCopyPropPass());
            manager.add(createCsePass());
            manager.add(createInstCombinePass());
            manager.add(createSimplifyCfgPass());
            manager.add(createDcePass());
        }
        manager.add(createDsePass());
        manager.add(createLicmPass());
        manager.add(createLoopSumPass());
        manager.add(createTailRecursionPass());
        manager.add(createGlobalCopyPropPass());
        manager.add(createGlobalCsePass());
        manager.add(createInlineSmallPass());
        for (int iteration = 0; iteration < 3; ++iteration) {
            manager.add(createGlobalConstPropPass());
            manager.add(createConstCallEvalPass());
            manager.add(createConstPropPass());
            manager.add(createAlgebraicSimplifyPass());
            manager.add(createCopyPropPass());
            manager.add(createCsePass());
            manager.add(createInstCombinePass());
            manager.add(createDcePass());
            manager.add(createDsePass());
            manager.add(createLicmPass());
            manager.add(createLoopSumPass());
            manager.add(createSimplifyCfgPass());
        }
        manager.add(createGlobalCopyPropPass());
        manager.add(createGlobalCsePass());
        manager.add(createSimplifyCfgPass());
    }
    return manager;
}

} // namespace toyc::passes
