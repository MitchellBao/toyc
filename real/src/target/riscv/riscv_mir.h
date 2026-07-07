#pragma once

#include <string>
#include <vector>

namespace toyc::riscv {

struct MachineInstruction {
    std::string opcode;
    std::vector<std::string> operands;
};

struct MachineBlock {
    std::string label;
    std::vector<MachineInstruction> instructions;
};

struct MachineFunction {
    std::string name;
    std::vector<MachineBlock> blocks;
};

} // namespace toyc::riscv
