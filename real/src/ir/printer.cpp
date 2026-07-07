#include "printer.h"

#include <ostream>

namespace toyc::ir {
namespace {

void printOperand(const Operand& operand, std::ostream& out)
{
    if (operand.isImmediate) {
        out << operand.immediate;
    } else {
        out << '%' << operand.value.id;
    }
}

} // namespace

void Printer::print(const Module& module, std::ostream& out) const
{
    for (const Global& global : module.globals) {
        out << "global " << (global.isConst ? "const " : "") << global.name << " = " << global.init << '\n';
    }
    for (const Function& function : module.functions) {
        out << "func " << function.name << '(';
        for (std::size_t i = 0; i < function.params.size(); ++i) {
            if (i != 0) {
                out << ", ";
            }
            out << function.params[i];
        }
        out << ")\n";
        for (int i = 0; i < static_cast<int>(function.blocks.size()); ++i) {
            const BasicBlock& block = function.blocks[static_cast<std::size_t>(i)];
            out << block.label << ":\n";
            for (const Instruction& inst : block.instructions) {
                if (inst.dst.id >= 0) {
                    out << "  %" << inst.dst.id << " = ";
                } else {
                    out << "  ";
                }
                out << instructionKindName(inst.kind);
                if (!inst.symbol.empty()) {
                    out << ' ' << inst.symbol;
                }
                for (const Operand& operand : inst.operands) {
                    out << ' ';
                    printOperand(operand, out);
                }
                out << '\n';
            }
            out << "  " << terminatorKindName(block.terminator.kind);
            if (block.terminator.kind == TerminatorKind::Jump) {
                out << " bb" << block.terminator.trueBlock;
            } else if (block.terminator.kind == TerminatorKind::Branch) {
                out << ' ';
                printOperand(block.terminator.condition, out);
                out << " bb" << block.terminator.trueBlock << " bb" << block.terminator.falseBlock;
            } else if (block.terminator.hasReturnValue) {
                out << ' ';
                printOperand(block.terminator.returnValue, out);
            }
            out << '\n';
        }
    }
}

} // namespace toyc::ir
