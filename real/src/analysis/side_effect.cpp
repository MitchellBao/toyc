#include "side_effect.h"

namespace toyc::analysis {

bool mayHaveSideEffect(const ir::Instruction& instruction)
{
    return instruction.hasSideEffect
        || instruction.kind == ir::InstructionKind::StoreGlobal
        || instruction.kind == ir::InstructionKind::StoreLocal
        || instruction.kind == ir::InstructionKind::Call;
}

} // namespace toyc::analysis
