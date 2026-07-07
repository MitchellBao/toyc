#include "isel.h"

namespace toyc::riscv {

MachineFunction selectInstructions(const ir::Function& function)
{
    MachineFunction machine;
    machine.name = function.name;
    for (const ir::BasicBlock& block : function.blocks) {
        machine.blocks.push_back(MachineBlock{block.label, {}});
    }
    return machine;
}

} // namespace toyc::riscv
