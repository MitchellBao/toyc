#include "dag.h"

namespace toyc::dag {

const char* nodeKindName(NodeKind kind)
{
    switch (kind) {
    case NodeKind::Const:
        return "const";
    case NodeKind::Name:
        return "name";
    case NodeKind::Unary:
        return "unary";
    case NodeKind::Binary:
        return "binary";
    case NodeKind::Call:
        return "call";
    case NodeKind::ExprStmt:
        return "expr_stmt";
    case NodeKind::Assign:
        return "assign";
    case NodeKind::Decl:
        return "decl";
    case NodeKind::Block:
        return "block";
    case NodeKind::If:
        return "if";
    case NodeKind::While:
        return "while";
    case NodeKind::Break:
        return "break";
    case NodeKind::Continue:
        return "continue";
    case NodeKind::Return:
        return "return";
    }
    return "unknown";
}

} // namespace toyc::dag
