#pragma once

#include "instruction.h"

#include <string>
#include <vector>

namespace toyc::ir {

struct BasicBlock {
    std::string label;
    std::vector<Instruction> instructions;
    Terminator terminator;
    bool hasTerminator = false;
};

} // namespace toyc::ir
