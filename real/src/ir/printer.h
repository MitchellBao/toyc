#pragma once

#include "module.h"

#include <iosfwd>

namespace toyc::ir {

class Printer {
public:
    void print(const Module& module, std::ostream& out) const;
};

} // namespace toyc::ir
