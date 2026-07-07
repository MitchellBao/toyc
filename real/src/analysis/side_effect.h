#pragma once

#include "ir/instruction.h"

namespace toyc::analysis {

bool mayHaveSideEffect(const ir::Instruction& instruction);

} // namespace toyc::analysis
