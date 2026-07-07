#pragma once

#include "frontend/ast.h"
#include "options.h"

#include <iosfwd>

namespace toyc {

class CompilerPipeline {
public:
    explicit CompilerPipeline(CompilerOptions options);

    void emitAssembly(const Program& program, std::ostream& out) const;

private:
    CompilerOptions options_;
};

} // namespace toyc
