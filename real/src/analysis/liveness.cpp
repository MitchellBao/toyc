#include "liveness.h"

#include "cfg.h"

#include <algorithm>
#include <cstddef>
#include <unordered_set>
#include <vector>

namespace toyc::analysis {
namespace {

void addUse(int value, std::unordered_set<int>& defined, std::unordered_set<int>& use)
{
    if (value >= 0 && defined.find(value) == defined.end()) {
        use.insert(value);
    }
}

} // namespace

// Live intervals for the linear-scan allocator.
//
// A single min/max scan over the instruction stream is not enough: a value that
// is live across a loop back-edge (e.g. a loop-invariant loaded once in the
// preheader and read every iteration) would appear to die at its last textual
// use, letting the allocator reuse its register later in the same loop body and
// clobber it. So we compute real backward dataflow liveness over the CFG and
// widen each value's interval to cover every block where it is live-in/out. With
// reverse-postorder block layout this yields a contiguous interval that spans
// the whole loop for loop-carried values.
std::unordered_map<int, LiveInterval> computeLocalIntervals(const ir::Function& function)
{
    const std::size_t nb = function.blocks.size();
    std::unordered_map<int, LiveInterval> intervals;
    if (nb == 0) {
        return intervals;
    }

    // Assign a linear index to every instruction and terminator slot, and record
    // each block's [start, end] index range plus raw def/use interval endpoints.
    std::vector<int> blockStart(nb, 0);
    std::vector<int> blockEnd(nb, 0);
    std::vector<std::unordered_set<int>> useSet(nb);
    std::vector<std::unordered_set<int>> defSet(nb);

    auto touch = [&](int value, int index) {
        if (value < 0) {
            return;
        }
        auto& iv = intervals[value];
        if (iv.weight == 0) {
            iv.start = index;
            iv.end = index;
        } else {
            iv.start = std::min(iv.start, index);
            iv.end = std::max(iv.end, index);
        }
        ++iv.weight;
    };

    int index = 0;
    for (std::size_t b = 0; b < nb; ++b) {
        const ir::BasicBlock& block = function.blocks[b];
        blockStart[b] = index;
        std::unordered_set<int> definedSoFar;
        for (const ir::Instruction& inst : block.instructions) {
            for (const ir::Operand& operand : inst.operands) {
                if (!operand.isImmediate) {
                    touch(operand.value.id, index);
                    addUse(operand.value.id, definedSoFar, useSet[b]);
                }
            }
            if (inst.dst.id >= 0) {
                touch(inst.dst.id, index);
                defSet[b].insert(inst.dst.id);
                definedSoFar.insert(inst.dst.id);
            }
            ++index;
        }
        // Terminator operands are uses at the block's final slot.
        if (!block.terminator.condition.isImmediate) {
            touch(block.terminator.condition.value.id, index);
            addUse(block.terminator.condition.value.id, definedSoFar, useSet[b]);
        }
        if (!block.terminator.returnValue.isImmediate) {
            touch(block.terminator.returnValue.value.id, index);
            addUse(block.terminator.returnValue.value.id, definedSoFar, useSet[b]);
        }
        blockEnd[b] = index;
        ++index;
    }

    // Backward dataflow: live-in[b] = use[b] ∪ (live-out[b] - def[b]);
    // live-out[b] = ∪ live-in[successors].
    const Cfg cfg = buildCfg(function);
    std::vector<std::unordered_set<int>> liveIn(nb);
    std::vector<std::unordered_set<int>> liveOut(nb);
    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t rev = 0; rev < nb; ++rev) {
            const std::size_t b = nb - rev - 1;
            std::unordered_set<int> out;
            for (int succ : cfg.successors[b]) {
                if (succ >= 0 && static_cast<std::size_t>(succ) < nb) {
                    out.insert(liveIn[static_cast<std::size_t>(succ)].begin(), liveIn[static_cast<std::size_t>(succ)].end());
                }
            }
            std::unordered_set<int> in = useSet[b];
            for (int v : out) {
                if (defSet[b].find(v) == defSet[b].end()) {
                    in.insert(v);
                }
            }
            if (in != liveIn[b] || out != liveOut[b]) {
                liveIn[b] = std::move(in);
                liveOut[b] = std::move(out);
                changed = true;
            }
        }
    }

    // Widen each value's interval to cover blocks where it is live-in / live-out.
    for (std::size_t b = 0; b < nb; ++b) {
        for (int v : liveIn[b]) {
            auto& iv = intervals[v];
            iv.start = std::min(iv.start, blockStart[b]);
            iv.end = std::max(iv.end, blockStart[b]);
        }
        for (int v : liveOut[b]) {
            auto& iv = intervals[v];
            iv.start = std::min(iv.start, blockEnd[b]);
            iv.end = std::max(iv.end, blockEnd[b]);
        }
    }

    return intervals;
}

} // namespace toyc::analysis
