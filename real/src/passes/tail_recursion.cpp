#include "pass_manager.h"

namespace toyc::passes {
namespace {

class TailRecursionPass final : public Pass {
public:
    std::string name() const override { return "tail-recursion"; }
    bool run(ir::Module&) override { return false; }
};

} // namespace

std::unique_ptr<Pass> createTailRecursionPass()
{
    return std::make_unique<TailRecursionPass>();
}

} // namespace toyc::passes
