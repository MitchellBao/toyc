#include "pass_manager.h"

namespace toyc::passes {
namespace {

class CanonicalizePass final : public Pass {
public:
    std::string name() const override { return "canonicalize"; }
    bool run(ir::Module&) override { return false; }
};

} // namespace

std::unique_ptr<Pass> createCanonicalizePass()
{
    return std::make_unique<CanonicalizePass>();
}

} // namespace toyc::passes
