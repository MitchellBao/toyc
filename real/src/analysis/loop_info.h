#pragma once

#include "ir/function.h"

namespace toyc::analysis {

class LoopInfo {
public:
    void build(const ir::Function&) {}
};

} // namespace toyc::analysis
