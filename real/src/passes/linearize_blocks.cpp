#include "pass_manager.h"

#include <cstddef>
#include <string>
#include <vector>

namespace toyc::passes {
namespace {

// Reorder each function's basic blocks into reverse-postorder from the entry.
//
// The backend computes register live intervals with a single linear scan over
// the block array (see analysis/computeLocalIntervals), so the array order must
// match control-flow order for intervals to be meaningful. The IR builder and
// later passes can leave blocks in an order where a block appears before its
// control-flow predecessors (e.g. a loop preheader placed after the loop). That
// makes a value defined in the preheader but used in the loop header get a
// backwards/garbled interval and the allocator mishandles it. Reverse-postorder
// guarantees every block appears after the block that dominates its entry, so a
// preheader precedes its loop header and hoisted invariants get sane intervals.
//
// This is behaviour-preserving: the backend emits an explicit terminator for
// every block, so only the terminator targets need remapping.

std::vector<int> successorsOf(const ir::BasicBlock& block)
{
    switch (block.terminator.kind) {
    case ir::TerminatorKind::Jump:
        if (block.terminator.trueBlock >= 0) {
            return {block.terminator.trueBlock};
        }
        return {};
    case ir::TerminatorKind::Branch: {
        std::vector<int> succ;
        if (block.terminator.trueBlock >= 0) {
            succ.push_back(block.terminator.trueBlock);
        }
        if (block.terminator.falseBlock >= 0 && block.terminator.falseBlock != block.terminator.trueBlock) {
            succ.push_back(block.terminator.falseBlock);
        }
        return succ;
    }
    case ir::TerminatorKind::Return:
        return {};
    }
    return {};
}

bool runOnFunction(ir::Function& function)
{
    const int n = static_cast<int>(function.blocks.size());
    if (n <= 1) {
        return false;
    }

    std::vector<char> visited(static_cast<std::size_t>(n), 0);
    std::vector<int> post;
    post.reserve(static_cast<std::size_t>(n));
    // Iterative post-order DFS from the entry block.
    std::vector<std::pair<int, std::size_t>> stack;
    stack.emplace_back(0, 0);
    visited[0] = 1;
    while (!stack.empty()) {
        auto& [node, next] = stack.back();
        const std::vector<int> succ = successorsOf(function.blocks[static_cast<std::size_t>(node)]);
        if (next < succ.size()) {
            const int child = succ[next];
            ++next;
            if (child >= 0 && child < n && !visited[static_cast<std::size_t>(child)]) {
                visited[static_cast<std::size_t>(child)] = 1;
                stack.emplace_back(child, 0);
            }
        } else {
            post.push_back(node);
            stack.pop_back();
        }
    }

    std::vector<int> order(post.rbegin(), post.rend()); // reverse post-order
    // Append any unreachable blocks (should not normally exist) to keep all
    // blocks and preserve a total order.
    for (int i = 0; i < n; ++i) {
        if (!visited[static_cast<std::size_t>(i)]) {
            order.push_back(i);
        }
    }

    // If already in reverse-postorder, nothing to do.
    bool identity = true;
    for (int i = 0; i < n; ++i) {
        if (order[static_cast<std::size_t>(i)] != i) {
            identity = false;
            break;
        }
    }
    if (identity) {
        return false;
    }

    std::vector<int> oldToNew(static_cast<std::size_t>(n), -1);
    for (int newIndex = 0; newIndex < n; ++newIndex) {
        oldToNew[static_cast<std::size_t>(order[static_cast<std::size_t>(newIndex)])] = newIndex;
    }

    std::vector<ir::BasicBlock> reordered;
    reordered.reserve(static_cast<std::size_t>(n));
    for (int newIndex = 0; newIndex < n; ++newIndex) {
        reordered.push_back(std::move(function.blocks[static_cast<std::size_t>(order[static_cast<std::size_t>(newIndex)])]));
    }
    for (ir::BasicBlock& block : reordered) {
        auto remap = [&](int& target) {
            if (target >= 0 && target < n) {
                target = oldToNew[static_cast<std::size_t>(target)];
            }
        };
        if (block.terminator.kind == ir::TerminatorKind::Jump || block.terminator.kind == ir::TerminatorKind::Branch) {
            remap(block.terminator.trueBlock);
        }
        if (block.terminator.kind == ir::TerminatorKind::Branch) {
            remap(block.terminator.falseBlock);
        }
    }

    function.blocks = std::move(reordered);
    return true;
}

class LinearizeBlocksPass final : public Pass {
public:
    std::string name() const override { return "linearize-blocks"; }
    bool run(ir::Module& module) override
    {
        bool changed = false;
        for (ir::Function& function : module.functions) {
            changed = runOnFunction(function) || changed;
        }
        return changed;
    }
};

} // namespace

std::unique_ptr<Pass> createLinearizeBlocksPass()
{
    return std::make_unique<LinearizeBlocksPass>();
}

} // namespace toyc::passes
