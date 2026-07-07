#include "regalloc.h"

#include <algorithm>
#include <vector>

namespace toyc::riscv {

std::unordered_map<int, std::string> RegisterAllocator::allocate(const std::unordered_map<int, analysis::LiveInterval>& intervals) const
{
    std::vector<std::pair<int, analysis::LiveInterval>> ordered(intervals.begin(), intervals.end());
    std::sort(ordered.begin(), ordered.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.second.weight != rhs.second.weight) {
            return lhs.second.weight > rhs.second.weight;
        }
        return lhs.first < rhs.first;
    });

    static constexpr const char* regs[] = {"s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11"};
    std::unordered_map<int, std::string> result;
    int regIndex = 0;
    for (const auto& [value, interval] : ordered) {
        (void)interval;
        if (regIndex >= static_cast<int>(std::size(regs))) {
            break;
        }
        result.emplace(value, regs[regIndex++]);
    }
    return result;
}

} // namespace toyc::riscv
