#pragma once

#include "ir/module.h"

#include <iosfwd>

namespace toyc::riscv {

class AsmPrinter {
public:
    void print(const ir::Module& module, std::ostream& out, std::ostream* statsOut = nullptr) const;
};

} // namespace toyc::riscv
