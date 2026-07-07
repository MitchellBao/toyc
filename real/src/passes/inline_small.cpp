#include "pass_manager.h"

namespace toyc::passes {
namespace {

class InlineSmallPass final : public Pass {
public:
    std::string name() const override { return "inline-small"; }
    bool run(ir::Module&) override { return false; }
};

} // namespace

std::unique_ptr<Pass> createInlineSmallPass()
{
    return std::make_unique<InlineSmallPass>();
}

} // namespace toyc::passes
