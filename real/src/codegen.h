#pragma once

#include "ast.h"

#include <iosfwd>

namespace toyc {

struct CodegenOptions {
    bool optimize = false;
};

class RiscVCodeGenerator {
public:
    explicit RiscVCodeGenerator(CodegenOptions options = {});

    void generate(const Program& program, std::ostream& out);

private:
    CodegenOptions options_;
};

} // namespace toyc
