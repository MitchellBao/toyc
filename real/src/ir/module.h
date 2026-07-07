#pragma once

#include "function.h"

#include <cstdint>
#include <string>
#include <vector>

namespace toyc::ir {

struct Global {
    bool isConst = false;
    std::string name;
    std::int32_t init = 0;
};

struct Module {
    std::vector<Global> globals;
    std::vector<Function> functions;
};

} // namespace toyc::ir
