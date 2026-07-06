#include "dag_to_ir.h"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace toyc {

ir::Module DagToIrBuilder::buildSkeleton(const dag::Module& module) const
{
    ir::Module irModule;
    irModule.globals.reserve(module.globals.size());
    irModule.functions.reserve(module.functions.size());

    std::unordered_map<std::string, std::int32_t> constants;
    for (const dag::Global& global : module.globals) {
        const auto init = evalConst(module, global.init, constants);
        if (!init.has_value()) {
            throw std::runtime_error("DAG to IR expected constant global initializer");
        }
        irModule.globals.push_back(ir::Global{global.isConst, global.name, *init});
        if (global.isConst) {
            constants[global.name] = *init;
        }
    }

    for (const dag::Function& function : module.functions) {
        ir::Function irFunction;
        irFunction.returnType = mapType(function.returnType);
        irFunction.name = function.name;
        irFunction.params = function.params;
        irFunction.blocks.push_back(ir::BasicBlock{".entry", {}, {}, false});
        irModule.functions.push_back(std::move(irFunction));
    }

    return irModule;
}

std::optional<std::int32_t> DagToIrBuilder::evalConst(
    const dag::Module& module,
    dag::NodeId id,
    const std::unordered_map<std::string, std::int32_t>& constants) const
{
    if (id.id < 0 || id.id >= static_cast<int>(module.globalNodes.size())) {
        return std::nullopt;
    }

    const dag::Node& node = module.globalNodes[static_cast<std::size_t>(id.id)];
    if (node.kind == dag::NodeKind::Const) {
        return node.intValue;
    }
    if (node.kind == dag::NodeKind::Name) {
        const auto found = constants.find(node.symbol);
        if (found != constants.end()) {
            return found->second;
        }
        return std::nullopt;
    }
    if (node.kind == dag::NodeKind::Unary && node.inputs.size() == 1) {
        const auto value = evalConst(module, node.inputs.front(), constants);
        if (!value.has_value()) {
            return std::nullopt;
        }
        switch (node.unaryOp) {
        case UnaryOp::Plus:
            return *value;
        case UnaryOp::Minus:
            return -*value;
        case UnaryOp::Not:
            return *value == 0 ? 1 : 0;
        }
    }
    if (node.kind == dag::NodeKind::Binary && node.inputs.size() == 2) {
        const auto lhs = evalConst(module, node.inputs[0], constants);
        const auto rhs = evalConst(module, node.inputs[1], constants);
        if (!lhs.has_value() || !rhs.has_value()) {
            return std::nullopt;
        }
        switch (node.binaryOp) {
        case BinaryOp::LogicalOr:
            return (*lhs != 0 || *rhs != 0) ? 1 : 0;
        case BinaryOp::LogicalAnd:
            return (*lhs != 0 && *rhs != 0) ? 1 : 0;
        case BinaryOp::Equal:
            return *lhs == *rhs ? 1 : 0;
        case BinaryOp::NotEqual:
            return *lhs != *rhs ? 1 : 0;
        case BinaryOp::Less:
            return *lhs < *rhs ? 1 : 0;
        case BinaryOp::LessEqual:
            return *lhs <= *rhs ? 1 : 0;
        case BinaryOp::Greater:
            return *lhs > *rhs ? 1 : 0;
        case BinaryOp::GreaterEqual:
            return *lhs >= *rhs ? 1 : 0;
        case BinaryOp::Add:
            return *lhs + *rhs;
        case BinaryOp::Sub:
            return *lhs - *rhs;
        case BinaryOp::Mul:
            return *lhs * *rhs;
        case BinaryOp::Div:
            return *rhs == 0 ? std::nullopt : std::optional<std::int32_t>{*lhs / *rhs};
        case BinaryOp::Mod:
            return *rhs == 0 ? std::nullopt : std::optional<std::int32_t>{*lhs % *rhs};
        }
    }

    return std::nullopt;
}

ir::Type DagToIrBuilder::mapType(Type type) const
{
    return type == Type::Void ? ir::Type::Void : ir::Type::Int;
}

} // namespace toyc
