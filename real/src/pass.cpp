#include "pass.h"

#include <memory>
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

PassManager buildDefaultPassPipeline(bool optimize)
{
    PassManager manager;
    if (optimize) {
        manager.add(createLocalSimplifyPass());
        manager.add(createDeadInstructionPass());
        manager.add(createLocalSimplifyPass());
        manager.add(createDeadInstructionPass());
    }
    return manager;
}

} // namespace toyc
