#pragma once

#include "ir/function.h"

#include <unordered_map>

namespace toyc::analysis {

struct LiveInterval {
    int start = 0;
    int end = 0;
    int weight = 0;
};

std::unordered_map<int, LiveInterval> computeLocalIntervals(const ir::Function& function);

} // namespace toyc::analysis
