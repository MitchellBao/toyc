#include "pass.h"

#include <utility>

namespace toyc {

void PassManager::add(std::unique_ptr<IrPass> pass)
{
    passes_.push_back(std::move(pass));
}

bool PassManager::run(ir::Module& module)
{
    bool changed = false;
    for (const auto& pass : passes_) {
        changed = pass->run(module) || changed;
    }
    return changed;
}

std::string NoOpPass::name() const
{
    return "noop";
}

bool NoOpPass::run(ir::Module& module)
{
    (void)module;
    return false;
}

PassManager buildDefaultPassPipeline(bool optimize)
{
    PassManager manager;
    if (optimize) {
        manager.add(std::make_unique<NoOpPass>());
    }
    return manager;
}

} // namespace toyc
