#pragma once

#include "module.h"

namespace toyc::ir {

class Verifier {
public:
    void verify(const Module& module) const;
};

} // namespace toyc::ir
