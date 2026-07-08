#include "pass_manager.h"

#include <cstddef>
#include <cstdlib>
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

bool constCallEvalDisabled()
{
    const char* value = std::getenv("TOYC_DISABLE_CONST_CALL_EVAL");
    return value != nullptr && value[0] != '\0' && std::string(value) != "0";
}

// Runs a group of sub-passes repeatedly until a full round makes no change, up
// to maxRounds. This is what gives p08-style combined cases real convergence:
// inline / const / copy / algebra / CSE / CFG / DCE / DSE / loop can each expose
// new work for the others, and we keep iterating until the module is stable
// instead of relying on a fixed hand-unrolled pass count. Per-pass stats are
// still emitted so downstream tooling (and the smoke suite) see each pass.
class FixedPointGroup final : public Pass {
public:
    FixedPointGroup(std::string name, int maxRounds, bool collectStats, std::ostream* statsOut)
        : name_(std::move(name)), maxRounds_(maxRounds), collectStats_(collectStats), statsOut_(statsOut)
    {
    }

    void add(std::unique_ptr<Pass> pass) { passes_.push_back(std::move(pass)); }

    std::string name() const override { return name_; }

    bool run(ir::Module& module) override
    {
        bool anyChanged = false;
        for (int round = 0; round < maxRounds_; ++round) {
            bool roundChanged = false;
            for (const auto& pass : passes_) {
                const IrStats before = collectStats_ && statsOut_ != nullptr ? measureIr(module) : IrStats{};
                const bool passChanged = pass->run(module);
                // Only emit a stats line when the pass actually changed the IR.
                // A convergent group runs each pass many times; printing every
                // no-op run would flood the log and inflate occurrence counts.
                if (passChanged && collectStats_ && statsOut_ != nullptr) {
                    printStatsLine(*statsOut_, pass->name(), before, measureIr(module), passChanged);
                }
                roundChanged = passChanged || roundChanged;
            }
            anyChanged = anyChanged || roundChanged;
            if (!roundChanged) {
                break;
            }
        }
        return anyChanged;
    }

private:
    std::vector<std::unique_ptr<Pass>> passes_;
    std::string name_;
    int maxRounds_ = 1;
    bool collectStats_ = false;
    std::ostream* statsOut_ = nullptr;
};

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
    const bool disableConstCallEval = constCallEvalDisabled();
    manager.add(createCanonicalizePass());
    manager.add(createSimplifyCfgPass());
    if (optimize) {
        // One convergent optimization group. The order follows the intended
        // combined pipeline: inline -> const/copy/algebra/CSE -> CFG cleanup ->
        // DSE/DCE -> loop optimization -> tail-recursion. The group re-runs
        // until a whole round changes nothing (bounded), so a pass that exposes
        // work for an earlier pass (e.g. inline exposing a loop, or CFG cleanup
        // exposing a dead store) is picked up on the next round.
        auto makeCoreGroup = [&](std::string name) {
            auto group = std::make_unique<FixedPointGroup>(std::move(name), 8, collectStats, statsOut);
            group->add(createInlineSmallPass());
            group->add(createGlobalConstPropPass());
            group->add(createConstPropPass());
            group->add(createAlgebraicSimplifyPass());
            group->add(createCopyPropPass());
            group->add(createLocalCoalescePass());
            group->add(createCsePass());
            group->add(createInstCombinePass());
            group->add(createGlobalCopyPropPass());
            group->add(createGlobalCsePass());
            group->add(createSimplifyCfgPass());
            group->add(createDsePass());
            group->add(createDcePass());
            group->add(createLicmPass());
            // Lay blocks in reverse-postorder before loop-sum: its pre-loop
            // constant analysis scans blocks with index < header, which is only
            // valid when block index matches control-flow order (a preheader can
            // otherwise land after the header, e.g. after inlining the bound).
            group->add(createLinearizeBlocksPass());
            group->add(createLoopSumPass());
            group->add(createTailRecursionPass());
            return group;
        };

        manager.add(makeCoreGroup("opt-converge"));
        // const-call-eval can collapse a now-pure function/loop to a constant;
        // run it once, then converge again so the constant propagates and the
        // remaining dead code / CFG is cleaned up.
        if (!disableConstCallEval) {
            manager.add(createConstCallEvalPass());
        }
        manager.add(makeCoreGroup("opt-converge-post"));
        manager.add(createDeadFunctionElimPass());
        manager.add(createSimplifyCfgPass());
    }
    // Lay blocks out in reverse-postorder so the backend's linear live-interval
    // scan matches control-flow order. Runs in both plain and optimized modes.
    manager.add(createLinearizeBlocksPass());
    return manager;
}

} // namespace toyc::passes
