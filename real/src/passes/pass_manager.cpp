#include "pass_manager.h"

#include <memory>
#include <utility>

namespace toyc::passes {

void PassManager::add(std::unique_ptr<Pass> pass)
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

PassManager buildPipeline(bool optimize)
{
    PassManager manager;
    manager.add(createCanonicalizePass());
    manager.add(createSimplifyCfgPass());
    if (optimize) {
        manager.add(createConstPropPass());
        manager.add(createCopyPropPass());
        manager.add(createCsePass());
        manager.add(createDcePass());
        manager.add(createDsePass());
        manager.add(createLicmPass());
        manager.add(createTailRecursionPass());
        manager.add(createInlineSmallPass());
        manager.add(createConstPropPass());
        manager.add(createCopyPropPass());
        manager.add(createCsePass());
        manager.add(createDcePass());
        manager.add(createSimplifyCfgPass());
    }
    return manager;
}

} // namespace toyc::passes
