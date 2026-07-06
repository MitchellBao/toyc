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

class NoOpPass final : public IrPass {
public:
    std::string name() const override;
    bool run(ir::Module& module) override;
};

PassManager buildDefaultPassPipeline(bool optimize);

} // namespace toyc
