#include "cfg.h"

namespace toyc::analysis {

Cfg buildCfg(const ir::Function& function)
{
    Cfg cfg;
    cfg.successors.resize(function.blocks.size());
    cfg.predecessors.resize(function.blocks.size());
    for (int i = 0; i < static_cast<int>(function.blocks.size()); ++i) {
        const ir::Terminator& term = function.blocks[static_cast<std::size_t>(i)].terminator;
        if (term.kind == ir::TerminatorKind::Jump) {
            cfg.successors[static_cast<std::size_t>(i)].push_back(term.trueBlock);
        } else if (term.kind == ir::TerminatorKind::Branch) {
            cfg.successors[static_cast<std::size_t>(i)].push_back(term.trueBlock);
            cfg.successors[static_cast<std::size_t>(i)].push_back(term.falseBlock);
        }
        for (int succ : cfg.successors[static_cast<std::size_t>(i)]) {
            if (succ >= 0 && succ < static_cast<int>(cfg.predecessors.size())) {
                cfg.predecessors[static_cast<std::size_t>(succ)].push_back(i);
            }
        }
    }
    return cfg;
}

} // namespace toyc::analysis
