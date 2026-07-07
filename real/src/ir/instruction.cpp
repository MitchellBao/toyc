#include "instruction.h"

namespace toyc::ir {

const char* instructionKindName(InstructionKind kind)
{
    switch (kind) {
    case InstructionKind::Const:
        return "const";
    case InstructionKind::Copy:
        return "copy";
    case InstructionKind::Unary:
        return "unary";
    case InstructionKind::Binary:
        return "binary";
    case InstructionKind::LoadGlobal:
        return "load_global";
    case InstructionKind::StoreGlobal:
        return "store_global";
    case InstructionKind::LoadLocal:
        return "load_local";
    case InstructionKind::StoreLocal:
        return "store_local";
    case InstructionKind::Call:
        return "call";
    }
    return "unknown";
}

const char* terminatorKindName(TerminatorKind kind)
{
    switch (kind) {
    case TerminatorKind::Jump:
        return "jump";
    case TerminatorKind::Branch:
        return "branch";
    case TerminatorKind::Return:
        return "return";
    }
    return "unknown";
}

} // namespace toyc::ir
