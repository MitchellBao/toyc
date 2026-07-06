#include "dag_builder.h"

#include <sstream>
#include <stdexcept>
#include <utility>

namespace toyc {

dag::Module DagBuilder::build(const Program& program)
{
    dag::Module module;

    for (const auto& item : program.items) {
        if (const auto* declItem = dynamic_cast<const TopDecl*>(item.get())) {
            currentNodes_ = &module.globalNodes;
            exprMemo_.clear();
            const dag::NodeId init = lowerExpr(*declItem->decl->init);
            module.globals.push_back(dag::Global{declItem->decl->isConst, declItem->decl->name, init});
            currentNodes_ = nullptr;
            continue;
        }

        if (const auto* funcItem = dynamic_cast<const TopFunc*>(item.get())) {
            dag::Function function;
            function.returnType = funcItem->func->returnType;
            function.name = funcItem->func->name;
            function.params.reserve(funcItem->func->params.size());
            for (const Param& param : funcItem->func->params) {
                function.params.push_back(param.name);
            }

            currentNodes_ = &function.nodes;
            exprMemo_.clear();
            function.body = lowerBlock(*funcItem->func->body);
            module.functions.push_back(std::move(function));
            currentNodes_ = nullptr;
        }
    }

    return module;
}

dag::NodeId DagBuilder::lowerExpr(const Expr& expr)
{
    const std::string key = exprKey(expr);
    if (!key.empty()) {
        const auto found = exprMemo_.find(key);
        if (found != exprMemo_.end()) {
            return found->second;
        }
    }

    dag::Node node;
    if (const auto* intExpr = dynamic_cast<const IntExpr*>(&expr)) {
        node.kind = dag::NodeKind::Const;
        node.intValue = intExpr->value;
    } else if (const auto* name = dynamic_cast<const NameExpr*>(&expr)) {
        node.kind = dag::NodeKind::Name;
        node.symbol = name->name;
    } else if (const auto* call = dynamic_cast<const CallExpr*>(&expr)) {
        node.kind = dag::NodeKind::Call;
        node.symbol = call->callee;
        node.inputs.reserve(call->args.size());
        for (const ExprPtr& arg : call->args) {
            node.inputs.push_back(lowerExpr(*arg));
        }
    } else if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
        node.kind = dag::NodeKind::Unary;
        node.unaryOp = unary->op;
        node.inputs.push_back(lowerExpr(*unary->operand));
    } else if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
        node.kind = dag::NodeKind::Binary;
        node.binaryOp = binary->op;
        node.inputs.push_back(lowerExpr(*binary->lhs));
        node.inputs.push_back(lowerExpr(*binary->rhs));
    } else {
        throw std::runtime_error("unknown expression in DAG builder");
    }

    const dag::NodeId id = addNode(std::move(node));
    if (!key.empty()) {
        exprMemo_[key] = id;
    }
    return id;
}

dag::NodeId DagBuilder::lowerStmt(const Stmt& stmt)
{
    dag::Node node;
    if (dynamic_cast<const EmptyStmt*>(&stmt) != nullptr) {
        node.kind = dag::NodeKind::Block;
        return addNode(std::move(node));
    }
    if (const auto* exprStmt = dynamic_cast<const ExprStmt*>(&stmt)) {
        node.kind = dag::NodeKind::ExprStmt;
        node.inputs.push_back(lowerExpr(*exprStmt->expr));
        return addNode(std::move(node));
    }
    if (const auto* assign = dynamic_cast<const AssignStmt*>(&stmt)) {
        node.kind = dag::NodeKind::Assign;
        node.symbol = assign->name;
        node.inputs.push_back(lowerExpr(*assign->value));
        return addNode(std::move(node));
    }
    if (const auto* declStmt = dynamic_cast<const DeclStmt*>(&stmt)) {
        node.kind = dag::NodeKind::Decl;
        node.symbol = declStmt->decl->name;
        node.isConstDecl = declStmt->decl->isConst;
        node.inputs.push_back(lowerExpr(*declStmt->decl->init));
        return addNode(std::move(node));
    }
    if (const auto* block = dynamic_cast<const BlockStmt*>(&stmt)) {
        return lowerBlock(*block);
    }
    if (const auto* ifStmt = dynamic_cast<const IfStmt*>(&stmt)) {
        node.kind = dag::NodeKind::If;
        node.inputs.push_back(lowerExpr(*ifStmt->cond));
        node.inputs.push_back(lowerStmt(*ifStmt->thenBranch));
        if (ifStmt->elseBranch) {
            node.inputs.push_back(lowerStmt(*ifStmt->elseBranch));
        }
        return addNode(std::move(node));
    }
    if (const auto* whileStmt = dynamic_cast<const WhileStmt*>(&stmt)) {
        node.kind = dag::NodeKind::While;
        node.inputs.push_back(lowerExpr(*whileStmt->cond));
        node.inputs.push_back(lowerStmt(*whileStmt->body));
        return addNode(std::move(node));
    }
    if (dynamic_cast<const BreakStmt*>(&stmt) != nullptr) {
        node.kind = dag::NodeKind::Break;
        return addNode(std::move(node));
    }
    if (dynamic_cast<const ContinueStmt*>(&stmt) != nullptr) {
        node.kind = dag::NodeKind::Continue;
        return addNode(std::move(node));
    }
    if (const auto* ret = dynamic_cast<const ReturnStmt*>(&stmt)) {
        node.kind = dag::NodeKind::Return;
        if (ret->value) {
            node.inputs.push_back(lowerExpr(*ret->value));
        }
        return addNode(std::move(node));
    }

    throw std::runtime_error("unknown statement in DAG builder");
}

dag::NodeId DagBuilder::lowerBlock(const BlockStmt& block)
{
    dag::Node node;
    node.kind = dag::NodeKind::Block;
    node.inputs.reserve(block.statements.size());
    for (const StmtPtr& stmt : block.statements) {
        node.inputs.push_back(lowerStmt(*stmt));
    }
    return addNode(std::move(node));
}

dag::NodeId DagBuilder::addNode(dag::Node node)
{
    if (currentNodes_ == nullptr) {
        throw std::runtime_error("DAG builder has no current node list");
    }
    const dag::NodeId id{static_cast<int>(currentNodes_->size())};
    currentNodes_->push_back(std::move(node));
    return id;
}

std::string DagBuilder::exprKey(const Expr& expr) const
{
    std::ostringstream out;
    if (const auto* intExpr = dynamic_cast<const IntExpr*>(&expr)) {
        out << "i:" << intExpr->value;
        return out.str();
    }
    if (const auto* name = dynamic_cast<const NameExpr*>(&expr)) {
        out << "n:" << name->name;
        return out.str();
    }
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
        const std::string operand = exprKey(*unary->operand);
        if (operand.empty()) {
            return {};
        }
        out << "u:" << static_cast<int>(unary->op) << ':' << operand;
        return out.str();
    }
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
        const std::string lhs = exprKey(*binary->lhs);
        const std::string rhs = exprKey(*binary->rhs);
        if (lhs.empty() || rhs.empty()) {
            return {};
        }
        out << "b:" << static_cast<int>(binary->op) << ':' << lhs << ':' << rhs;
        return out.str();
    }
    return {};
}

} // namespace toyc
