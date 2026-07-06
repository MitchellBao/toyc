#include "ir_builder.h"

#include <stdexcept>

namespace toyc {

ir::Module IrBuilder::buildSkeleton(const Program& program)
{
    constants_.clear();
    ir::Module module;

    for (const auto& item : program.items) {
        if (const auto* declItem = dynamic_cast<const TopDecl*>(item.get())) {
            const auto init = evalConst(*declItem->decl->init);
            if (!init.has_value()) {
                throw std::runtime_error("IR builder expected constant global initializer");
            }
            module.globals.push_back(ir::Global{declItem->decl->isConst, declItem->decl->name, *init});
            if (declItem->decl->isConst) {
                constants_[declItem->decl->name] = *init;
            }
            continue;
        }

        if (const auto* funcItem = dynamic_cast<const TopFunc*>(item.get())) {
            ir::Function function;
            function.returnType = mapType(funcItem->func->returnType);
            function.name = funcItem->func->name;
            function.params.reserve(funcItem->func->params.size());
            for (const Param& param : funcItem->func->params) {
                function.params.push_back(param.name);
            }
            function.blocks.push_back(ir::BasicBlock{".entry", {}, {}});
            module.functions.push_back(std::move(function));
        }
    }

    return module;
}

std::optional<std::int32_t> IrBuilder::evalConst(const Expr& expr) const
{
    if (const auto* intExpr = dynamic_cast<const IntExpr*>(&expr)) {
        return intExpr->value;
    }
    if (const auto* name = dynamic_cast<const NameExpr*>(&expr)) {
        const auto found = constants_.find(name->name);
        if (found != constants_.end()) {
            return found->second;
        }
        return std::nullopt;
    }
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
        const auto value = evalConst(*unary->operand);
        if (!value.has_value()) {
            return std::nullopt;
        }
        switch (unary->op) {
        case UnaryOp::Plus:
            return *value;
        case UnaryOp::Minus:
            return -*value;
        case UnaryOp::Not:
            return *value == 0 ? 1 : 0;
        }
    }
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
        const auto lhs = evalConst(*binary->lhs);
        const auto rhs = evalConst(*binary->rhs);
        if (!lhs.has_value() || !rhs.has_value()) {
            return std::nullopt;
        }
        switch (binary->op) {
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

ir::Type IrBuilder::mapType(Type type) const
{
    return type == Type::Void ? ir::Type::Void : ir::Type::Int;
}

} // namespace toyc
