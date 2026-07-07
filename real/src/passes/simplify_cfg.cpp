#include "pass_manager.h"

namespace toyc::passes {
namespace {

class SimplifyCfgPass final : public Pass {
public:
    std::string name() const override { return "simplify-cfg"; }
    bool run(ir::Module&) override { return false; }
};

} // namespace

std::unique_ptr<Pass> createSimplifyCfgPass()
{
    return std::make_unique<SimplifyCfgPass>();
}

} // namespace toyc::passes
