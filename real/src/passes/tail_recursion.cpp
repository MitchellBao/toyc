#include "pass_manager.h"

#include <cstddef>
#include <vector>

namespace toyc::passes {
namespace {

ir::Value newValue(ir::Function& function)
{
    return ir::Value{function.nextValue++};
}

bool isSelfTailCall(const ir::Function& function, const ir::BasicBlock& block)
{
    if (block.instructions.empty() || block.terminator.kind != ir::TerminatorKind::Return || !block.terminator.hasReturnValue) {
        return false;
    }
    const ir::Instruction& call = block.instructions.back();
    return call.kind == ir::InstructionKind::Call
        && call.symbol == function.name
        && call.dst.id >= 0
        && !block.terminator.returnValue.isImmediate
        && block.terminator.returnValue.value == call.dst
        && call.operands.size() == function.params.size();
}

bool rewriteTailCall(ir::Function& function, ir::BasicBlock& block)
{
    if (!isSelfTailCall(function, block)) {
        return false;
    }

    const ir::Instruction call = block.instructions.back();
    block.instructions.pop_back();

    std::vector<ir::Value> temporaries;
    temporaries.reserve(call.operands.size());
    for (const ir::Operand& argument : call.operands) {
        ir::Instruction copy;
        copy.kind = ir::InstructionKind::Copy;
        copy.dst = newValue(function);
        copy.operands = {argument};
        temporaries.push_back(copy.dst);
        block.instructions.push_back(std::move(copy));
    }

    for (std::size_t i = 0; i < temporaries.size(); ++i) {
        ir::Instruction store;
        store.kind = ir::InstructionKind::StoreLocal;
        store.symbol = function.params[i];
        store.operands = {ir::Operand::ref(temporaries[i])};
        block.instructions.push_back(std::move(store));
    }

    block.terminator = {};
    block.terminator.kind = ir::TerminatorKind::Jump;
    block.terminator.trueBlock = 0;
    block.hasTerminator = true;
    return true;
}

class TailRecursionPass final : public Pass {
public:
    std::string name() const override { return "tail-recursion"; }
    bool run(ir::Module& module) override
    {
        bool changed = false;
        for (ir::Function& function : module.functions) {
            if (function.params.empty()) {
                continue;
            }
            for (ir::BasicBlock& block : function.blocks) {
                changed = rewriteTailCall(function, block) || changed;
            }
        }
        return changed;
    }
};

} // namespace

std::unique_ptr<Pass> createTailRecursionPass()
{
    return std::make_unique<TailRecursionPass>();
}

} // namespace toyc::passes
