#pragma once

#include "basic_block.h"

#include <string>
#include <vector>

namespace toyc::ir {

struct Function {
    Type returnType = Type::Int;
    std::string name;
    std::vector<std::string> params;
    std::vector<BasicBlock> blocks;
    int nextValue = 0;
};

} // namespace toyc::ir
