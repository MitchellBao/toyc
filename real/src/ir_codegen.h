#pragma once

#include "ir.h"

#include <iosfwd>

namespace toyc {

class IrRiscVCodeGenerator {
public:
    bool canGenerate(const ir::Module& module) const;
    void generate(const ir::Module& module, std::ostream& out) const;
};

} // namespace toyc
