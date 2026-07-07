#include "pass_manager.h"

namespace toyc::passes {
namespace {

class LicmPass final : public Pass {
public:
    std::string name() const override { return "licm"; }
    bool run(ir::Module&) override { return false; }
};

} // namespace

std::unique_ptr<Pass> createLicmPass()
{
    return std::make_unique<LicmPass>();
}

} // namespace toyc::passes
