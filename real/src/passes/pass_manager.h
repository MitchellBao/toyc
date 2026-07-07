#pragma once

#include "ir/module.h"

#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

namespace toyc::passes {

class Pass {
public:
    virtual ~Pass() = default;
    virtual std::string name() const = 0;
    virtual bool run(ir::Module& module) = 0;
};

class PassManager {
public:
    explicit PassManager(bool collectStats = false, std::ostream* statsOut = nullptr);

    void add(std::unique_ptr<Pass> pass);
    bool run(ir::Module& module);

private:
    std::vector<std::unique_ptr<Pass>> passes_;
    bool collectStats_ = false;
    std::ostream* statsOut_ = nullptr;
};

PassManager buildPipeline(bool optimize, bool collectStats = false, std::ostream* statsOut = nullptr);

std::unique_ptr<Pass> createCanonicalizePass();
std::unique_ptr<Pass> createSimplifyCfgPass();
std::unique_ptr<Pass> createConstPropPass();
std::unique_ptr<Pass> createAlgebraicSimplifyPass();
std::unique_ptr<Pass> createCopyPropPass();
std::unique_ptr<Pass> createCsePass();
std::unique_ptr<Pass> createDcePass();
std::unique_ptr<Pass> createDsePass();
std::unique_ptr<Pass> createLicmPass();
std::unique_ptr<Pass> createLoopSumPass();
std::unique_ptr<Pass> createInlineSmallPass();
std::unique_ptr<Pass> createTailRecursionPass();

} // namespace toyc::passes
