#pragma once

#include "ir/module.h"
#include "riscv_mir.h"

namespace toyc::riscv {

MachineFunction selectInstructions(const ir::Function& function);

} // namespace toyc::riscv
