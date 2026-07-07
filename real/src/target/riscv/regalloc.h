#pragma once

#include "analysis/liveness.h"

#include <unordered_set>
#include <string>
#include <unordered_map>

namespace toyc::riscv {

class RegisterAllocator {
public:
    std::unordered_map<int, std::string> allocate(
        const std::unordered_map<int, analysis::LiveInterval>& intervals,
        const std::unordered_set<int>& liveAcrossCalls = {},
        bool mayUseArgumentRegs = false) const;
};

} // namespace toyc::riscv
