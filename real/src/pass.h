#pragma once

#include "ir.h"

#include <memory>
#include <string>
#include <vector>

namespace toyc {

class IrPass {
public:
    virtual ~IrPass() = default;
    virtual std::string name() const = 0;
    virtual bool run(ir::Module& module) = 0;
};

class PassManager {
public:
    void add(std::unique_ptr<IrPass> pass);
    bool run(ir::Module& module);

private:
    std::vector<std::unique_ptr<IrPass>> passes_;
};

std::unique_ptr<IrPass> createLocalSimplifyPass();
std::unique_ptr<IrPass> createLocalCsePass();
std::unique_ptr<IrPass> createDeadInstructionPass();

PassManager buildDefaultPassPipeline(bool optimize);

} // namespace toyc
