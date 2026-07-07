#pragma once

#include "frontend/ast.h"
#include "module.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace toyc {

class IrBuilder {
public:
    ir::Module build(const Program& program);

private:
    std::optional<std::int32_t> evalConst(const Expr& expr) const;
    ir::Type mapType(Type type) const;

    std::unordered_map<std::string, std::int32_t> constants_;
};

} // namespace toyc
