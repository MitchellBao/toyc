#pragma once

#include "ast.h"
#include "dag.h"

#include <string>
#include <unordered_map>

namespace toyc {

class DagBuilder {
public:
    dag::Module build(const Program& program);

private:
    dag::NodeId lowerExpr(const Expr& expr);
    dag::NodeId lowerStmt(const Stmt& stmt);
    dag::NodeId lowerBlock(const BlockStmt& block);
    dag::NodeId addNode(dag::Node node);
    std::string exprKey(const Expr& expr) const;

    std::vector<dag::Node>* currentNodes_ = nullptr;
    std::unordered_map<std::string, dag::NodeId> exprMemo_;
};

} // namespace toyc
