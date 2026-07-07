#include "pass_manager.h"

#include <sstream>
#include <string>
#include <unordered_map>

namespace toyc::passes {
namespace {

std::string operandKey(const ir::Operand& operand)
{
    if (operand.isImmediate) {
        return "#" + std::to_string(operand.immediate);
    }
    return "%" + std::to_string(operand.value.id);
}

std::string instructionKey(const ir::Instruction& inst)
{
    if (inst.hasSideEffect || inst.kind == ir::InstructionKind::Call || inst.kind == ir::InstructionKind::StoreGlobal || inst.kind == ir::InstructionKind::StoreLocal) {
        return {};
    }
    std::ostringstream key;
    key << static_cast<int>(inst.kind) << ':' << static_cast<int>(inst.binaryOp) << ':' << static_cast<int>(inst.unaryOp) << ':' << inst.symbol;
    for (const ir::Operand& operand : inst.operands) {
        key << ':' << operandKey(operand);
    }
    return key.str();
}

class CsePass final : public Pass {
public:
    std::string name() const override { return "local-cse"; }

    bool run(ir::Module& module) override
    {
        bool changed = false;
        for (ir::Function& function : module.functions) {
            for (ir::BasicBlock& block : function.blocks) {
                std::unordered_map<std::string, ir::Value> seen;
                for (ir::Instruction& inst : block.instructions) {
                    if (inst.kind == ir::InstructionKind::StoreGlobal || inst.kind == ir::InstructionKind::Call) {
                        seen.clear();
                    }
                    const std::string key = instructionKey(inst);
                    if (key.empty() || inst.dst.id < 0) {
                        continue;
                    }
                    const auto found = seen.find(key);
                    if (found != seen.end()) {
                        inst.kind = ir::InstructionKind::Copy;
                        inst.operands = {ir::Operand::ref(found->second)};
                        inst.symbol.clear();
                        changed = true;
                    } else {
                        seen.emplace(key, inst.dst);
                    }
                }
            }
        }
        return changed;
    }
};

} // namespace

std::unique_ptr<Pass> createCsePass()
{
    return std::make_unique<CsePass>();
}

} // namespace toyc::passes
