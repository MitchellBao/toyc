#pragma once

#include "ir/function.h"

#include <vector>

namespace toyc::analysis {

struct Cfg {
    std::vector<std::vector<int>> successors;
    std::vector<std::vector<int>> predecessors;
};

Cfg buildCfg(const ir::Function& function);

} // namespace toyc::analysis
