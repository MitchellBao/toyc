#include "liveness.h"

#include <algorithm>

namespace toyc::analysis {
namespace {

void touch(std::unordered_map<int, LiveInterval>& intervals, int value, int index)
{
    if (value < 0) {
        return;
    }
    auto& interval = intervals[value];
    if (interval.weight == 0) {
        interval.start = index;
        interval.end = index;
    } else {
        interval.start = std::min(interval.start, index);
        interval.end = std::max(interval.end, index);
    }
    ++interval.weight;
}

void touchOperand(std::unordered_map<int, LiveInterval>& intervals, const ir::Operand& operand, int index)
{
    if (!operand.isImmediate) {
        touch(intervals, operand.value.id, index);
    }
}

} // namespace

std::unordered_map<int, LiveInterval> computeLocalIntervals(const ir::Function& function)
{
    std::unordered_map<int, LiveInterval> intervals;
    int index = 0;
    for (const ir::BasicBlock& block : function.blocks) {
        for (const ir::Instruction& inst : block.instructions) {
            touch(intervals, inst.dst.id, index);
            for (const ir::Operand& operand : inst.operands) {
                touchOperand(intervals, operand, index);
            }
            ++index;
        }
        touchOperand(intervals, block.terminator.condition, index);
        touchOperand(intervals, block.terminator.returnValue, index);
        ++index;
    }
    return intervals;
}

} // namespace toyc::analysis
