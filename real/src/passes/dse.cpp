#include "pass_manager.h"

namespace toyc::passes {
namespace {

class DsePass final : public Pass {
public:
    std::string name() const override { return "dse"; }
    bool run(ir::Module&) override { return false; }
};

} // namespace

std::unique_ptr<Pass> createDsePass()
{
    return std::make_unique<DsePass>();
}

} // namespace toyc::passes
