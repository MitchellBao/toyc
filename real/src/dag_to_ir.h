#pragma once

#include "dag.h"
#include "ir.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace toyc {

class DagToIrBuilder {
public:
    ir::Module buildSkeleton(const dag::Module& module) const;

private:
    std::optional<std::int32_t> evalConst(
        const dag::Module& module,
        dag::NodeId id,
        const std::unordered_map<std::string, std::int32_t>& constants) const;
    ir::Type mapType(Type type) const;
};

} // namespace toyc
