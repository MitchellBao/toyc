#pragma once

#include "ast.h"

#include <cstdint>
#include <string>
#include <vector>

namespace toyc::dag {

struct NodeId {
    int id = -1;
};

enum class NodeKind {
    Const,
    Name,
    Unary,
    Binary,
    Call,
    ExprStmt,
    Assign,
    Decl,
    Block,
    If,
    While,
    Break,
    Continue,
    Return,
};

struct Node {
    NodeKind kind = NodeKind::Const;
    Type type = Type::Int;
    std::int32_t intValue = 0;
    std::string symbol;
    UnaryOp unaryOp = UnaryOp::Plus;
    BinaryOp binaryOp = BinaryOp::Add;
    std::vector<NodeId> inputs;
    bool isConstDecl = false;
};

struct Global {
    bool isConst = false;
    std::string name;
    NodeId init;
};

struct Function {
    Type returnType = Type::Int;
    std::string name;
    std::vector<std::string> params;
    std::vector<Node> nodes;
    NodeId body;
};

struct Module {
    std::vector<Node> globalNodes;
    std::vector<Global> globals;
    std::vector<Function> functions;
};

const char* nodeKindName(NodeKind kind);

} // namespace toyc::dag
