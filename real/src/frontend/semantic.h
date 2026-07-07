#pragma once

#include "ast.h"

#include <stdexcept>
#include <string>

namespace toyc {

class SemanticError final : public std::runtime_error {
public:
    explicit SemanticError(const std::string& message)
        : std::runtime_error(message)
    {
    }
};

class SemanticAnalyzer {
public:
    void analyze(const Program& program);
};

} // namespace toyc
