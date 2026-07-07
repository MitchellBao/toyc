#include "verifier.h"

#include <stdexcept>
#include <string>
#include <unordered_set>

namespace toyc::ir {

void Verifier::verify(const Module& module) const
{
    std::unordered_set<std::string> globals;
    for (const Global& global : module.globals) {
        if (!globals.insert(global.name).second) {
            throw std::runtime_error("duplicate IR global: " + global.name);
        }
    }

    std::unordered_set<std::string> functions;
    for (const Function& function : module.functions) {
        if (!functions.insert(function.name).second) {
            throw std::runtime_error("duplicate IR function: " + function.name);
        }
        if (function.blocks.empty()) {
            throw std::runtime_error("IR function has no blocks: " + function.name);
        }
        for (int i = 0; i < static_cast<int>(function.blocks.size()); ++i) {
            const BasicBlock& block = function.blocks[static_cast<std::size_t>(i)];
            if (block.label.empty()) {
                throw std::runtime_error("IR block has empty label in function: " + function.name);
            }
            if (!block.hasTerminator) {
                throw std::runtime_error("IR block has no terminator: " + function.name + ":" + block.label);
            }
            const Terminator& term = block.terminator;
            if ((term.kind == TerminatorKind::Jump || term.kind == TerminatorKind::Branch)
                && (term.trueBlock < 0 || term.trueBlock >= static_cast<int>(function.blocks.size()))) {
                throw std::runtime_error("IR terminator has invalid true target in function: " + function.name);
            }
            if (term.kind == TerminatorKind::Branch
                && (term.falseBlock < 0 || term.falseBlock >= static_cast<int>(function.blocks.size()))) {
                throw std::runtime_error("IR branch has invalid false target in function: " + function.name);
            }
        }
    }
}

} // namespace toyc::ir
