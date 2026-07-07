#pragma once

#include "analysis/liveness.h"

#include <string>
#include <unordered_map>

namespace toyc::riscv {

class RegisterAllocator {
public:
    std::unordered_map<int, std::string> allocate(const std::unordered_map<int, analysis::LiveInterval>& intervals) const;
};

} // namespace toyc::riscv
