#include "ir_codegen.h"

#include <ostream>
#include <stdexcept>

namespace toyc {

bool IrRiscVCodeGenerator::canGenerate(const ir::Module& module) const
{
    for (const ir::Function& function : module.functions) {
        for (const ir::BasicBlock& block : function.blocks) {
            if (!block.instructions.empty() || block.hasTerminator) {
                return false;
            }
        }
    }
    return false;
}

void IrRiscVCodeGenerator::generate(const ir::Module& module, std::ostream& out) const
{
    (void)module;
    (void)out;
    throw std::runtime_error("IR RISC-V backend is not enabled yet");
}

} // namespace toyc
