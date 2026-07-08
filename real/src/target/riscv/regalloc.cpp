#include "regalloc.h"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace toyc::riscv {

// Linear-scan register allocation.
//
// Values are single-assignment, so each has a contiguous [start, end] live
// interval (min/max of its def and uses over the RPO-linearized instruction
// stream). Registers are reused across non-overlapping intervals: as each value
// begins, intervals that already ended free their registers. A value that is
// live across a call must land in a callee-saved register; others prefer
// caller-saved (and argument) registers and fall back to callee-saved. When no
// register is free, the live value with the furthest end is spilled (that value
// if it is the furthest), which keeps short, hot intervals in registers.
std::unordered_map<int, std::string> RegisterAllocator::allocate(
    const std::unordered_map<int, analysis::LiveInterval>& intervals,
    const std::unordered_set<int>& liveAcrossCalls,
    bool mayUseArgumentRegs,
    const std::unordered_set<std::string>& reservedRegs,
    int calleeSavedLimit) const
{
    auto reserved = [&](const char* reg) { return reservedRegs.find(reg) != reservedRegs.end(); };

    std::vector<std::string> freeCaller;
    for (const char* reg : {"t3", "t4", "t5"}) {
        if (!reserved(reg)) {
            freeCaller.push_back(reg);
        }
    }
    if (mayUseArgumentRegs) {
        for (const char* reg : {"a1", "a2", "a3", "a4", "a5", "a6", "a7"}) {
            if (!reserved(reg)) {
                freeCaller.push_back(reg);
            }
        }
    }

    std::vector<std::string> freeCallee;
    {
        static constexpr std::array<const char*, 11> calleeRegs =
            {"s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11"};
        const int limit = calleeSavedLimit >= 0 ? calleeSavedLimit : static_cast<int>(calleeRegs.size());
        int added = 0;
        for (const char* reg : calleeRegs) {
            if (added >= limit) {
                break;
            }
            if (!reserved(reg)) {
                freeCallee.push_back(reg);
                ++added;
            }
        }
    }

    std::vector<std::pair<int, analysis::LiveInterval>> ordered(intervals.begin(), intervals.end());
    std::sort(ordered.begin(), ordered.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.second.start != rhs.second.start) {
            return lhs.second.start < rhs.second.start;
        }
        if (lhs.second.end != rhs.second.end) {
            return lhs.second.end < rhs.second.end;
        }
        return lhs.first < rhs.first;
    });

    struct Active {
        int end;
        std::string reg;
        bool callee;
        int value;
        int weight;
    };
    std::vector<Active> active;
    std::unordered_map<int, std::string> result;

    for (const auto& [value, interval] : ordered) {
        // Expire intervals that ended strictly before this one starts.
        for (std::size_t i = 0; i < active.size();) {
            if (active[i].end < interval.start) {
                (active[i].callee ? freeCallee : freeCaller).push_back(active[i].reg);
                active[i] = active.back();
                active.pop_back();
            } else {
                ++i;
            }
        }

        const bool acrossCall = liveAcrossCalls.find(value) != liveAcrossCalls.end();

        std::string reg;
        bool callee = false;
        if (!acrossCall && !freeCaller.empty()) {
            reg = freeCaller.back();
            freeCaller.pop_back();
            callee = false;
        } else if (!freeCallee.empty()) {
            reg = freeCallee.back();
            freeCallee.pop_back();
            callee = true;
        }

        if (reg.empty()) {
            // No free register: spill the coldest value (lowest usage weight) so
            // hot loop values stay in registers. An across-call value can only
            // steal a callee-saved holder.
            int victim = -1;
            int victimWeight = interval.weight;
            for (std::size_t i = 0; i < active.size(); ++i) {
                if (acrossCall && !active[i].callee) {
                    continue;
                }
                if (active[i].weight < victimWeight) {
                    victimWeight = active[i].weight;
                    victim = static_cast<int>(i);
                }
            }
            if (victim >= 0) {
                result.erase(active[static_cast<std::size_t>(victim)].value);
                const std::string stolen = active[static_cast<std::size_t>(victim)].reg;
                const bool stolenCallee = active[static_cast<std::size_t>(victim)].callee;
                active[static_cast<std::size_t>(victim)] = Active{interval.end, stolen, stolenCallee, value, interval.weight};
                result[value] = stolen;
            }
            // else: current value is the coldest, stays spilled (no register).
            continue;
        }

        active.push_back(Active{interval.end, reg, callee, value, interval.weight});
        result[value] = reg;
    }

    return result;
}

} // namespace toyc::riscv
