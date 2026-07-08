#include "pass_manager.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace toyc::passes {
namespace {

std::vector<std::string> calleesOf(const ir::Function& function)
{
    std::vector<std::string> result;
    for (const ir::BasicBlock& block : function.blocks) {
        for (const ir::Instruction& inst : block.instructions) {
            if (inst.kind == ir::InstructionKind::Call && !inst.symbol.empty()) {
                result.push_back(inst.symbol);
            }
        }
    }
    return result;
}

class DeadFunctionElimPass final : public Pass {
public:
    std::string name() const override { return "dead-function-elim"; }

    bool run(ir::Module& module) override
    {
        std::unordered_map<std::string, const ir::Function*> byName;
        for (const ir::Function& function : module.functions) {
            byName.emplace(function.name, &function);
        }

        std::unordered_set<std::string> reachable;
        std::vector<std::string> worklist;
        if (byName.find("main") != byName.end()) {
            reachable.insert("main");
            worklist.push_back("main");
        }

        while (!worklist.empty()) {
            const std::string current = worklist.back();
            worklist.pop_back();
            const auto found = byName.find(current);
            if (found == byName.end()) {
                continue;
            }
            for (const std::string& callee : calleesOf(*found->second)) {
                if (byName.find(callee) != byName.end() && reachable.insert(callee).second) {
                    worklist.push_back(callee);
                }
            }
        }

        const std::size_t before = module.functions.size();
        module.functions.erase(
            std::remove_if(module.functions.begin(), module.functions.end(), [&](const ir::Function& function) {
                return function.name != "main" && reachable.find(function.name) == reachable.end();
            }),
            module.functions.end());
        return module.functions.size() != before;
    }
};

} // namespace

std::unique_ptr<Pass> createDeadFunctionElimPass()
{
    return std::make_unique<DeadFunctionElimPass>();
}

} // namespace toyc::passes
