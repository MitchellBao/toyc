#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace toyc {

enum class Type {
    Int,
    Void,
};

enum class BinaryOp {
    LogicalOr,
    LogicalAnd,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Add,
    Sub,
    Mul,
    Div,
    Mod,
};

enum class UnaryOp {
    Plus,
    Minus,
    Not,
};

struct Expr;
struct Stmt;
struct Decl;
struct BlockStmt;
struct FuncDef;

using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;
using DeclPtr = std::unique_ptr<Decl>;
using BlockPtr = std::unique_ptr<BlockStmt>;
using FuncPtr = std::unique_ptr<FuncDef>;

template <typename T>
std::unique_ptr<T> take(T* ptr)
{
    return std::unique_ptr<T>(ptr);
}

struct Expr {
    virtual ~Expr() = default;
};

struct IntExpr final : Expr {
    explicit IntExpr(std::int32_t value) : value(value) {}
    std::int32_t value;
};

struct NameExpr final : Expr {
    explicit NameExpr(std::string name) : name(std::move(name)) {}
    std::string name;
};

struct CallExpr final : Expr {
    CallExpr(std::string callee, std::vector<ExprPtr> args)
        : callee(std::move(callee)), args(std::move(args))
    {
    }

    std::string callee;
    std::vector<ExprPtr> args;
};

struct UnaryExpr final : Expr {
    UnaryExpr(UnaryOp op, ExprPtr operand)
        : op(op), operand(std::move(operand))
    {
    }

    UnaryOp op;
    ExprPtr operand;
};

struct BinaryExpr final : Expr {
    BinaryExpr(BinaryOp op, ExprPtr lhs, ExprPtr rhs)
        : op(op), lhs(std::move(lhs)), rhs(std::move(rhs))
    {
    }

    BinaryOp op;
    ExprPtr lhs;
    ExprPtr rhs;
};

struct Decl {
    Decl(bool isConst, std::string name, ExprPtr init)
        : isConst(isConst), name(std::move(name)), init(std::move(init))
    {
    }
    virtual ~Decl() = default;

    bool isConst;
    std::string name;
    ExprPtr init;
};

struct VarDecl final : Decl {
    VarDecl(bool isConst, std::string name, ExprPtr init)
        : Decl(isConst, std::move(name), std::move(init))
    {
    }
};

struct Stmt {
    virtual ~Stmt() = default;
};

struct EmptyStmt final : Stmt {
};

struct ExprStmt final : Stmt {
    explicit ExprStmt(ExprPtr expr) : expr(std::move(expr)) {}
    ExprPtr expr;
};

struct AssignStmt final : Stmt {
    AssignStmt(std::string name, ExprPtr value)
        : name(std::move(name)), value(std::move(value))
    {
    }

    std::string name;
    ExprPtr value;
};

struct DeclStmt final : Stmt {
    explicit DeclStmt(DeclPtr decl) : decl(std::move(decl)) {}
    DeclPtr decl;
};

struct BlockStmt final : Stmt {
    explicit BlockStmt(std::vector<StmtPtr> statements = {})
        : statements(std::move(statements))
    {
    }

    std::vector<StmtPtr> statements;
};

struct IfStmt final : Stmt {
    IfStmt(ExprPtr cond, StmtPtr thenBranch, StmtPtr elseBranch)
        : cond(std::move(cond)),
          thenBranch(std::move(thenBranch)),
          elseBranch(std::move(elseBranch))
    {
    }

    ExprPtr cond;
    StmtPtr thenBranch;
    StmtPtr elseBranch;
};

struct WhileStmt final : Stmt {
    WhileStmt(ExprPtr cond, StmtPtr body)
        : cond(std::move(cond)), body(std::move(body))
    {
    }

    ExprPtr cond;
    StmtPtr body;
};

struct BreakStmt final : Stmt {
};

struct ContinueStmt final : Stmt {
};

struct ReturnStmt final : Stmt {
    explicit ReturnStmt(ExprPtr value) : value(std::move(value)) {}
    ExprPtr value;
};

struct Param {
    explicit Param(std::string name) : name(std::move(name)) {}
    std::string name;
};

struct FuncDef {
    FuncDef(Type returnType, std::string name, std::vector<Param> params, BlockPtr body)
        : returnType(returnType),
          name(std::move(name)),
          params(std::move(params)),
          body(std::move(body))
    {
    }

    Type returnType;
    std::string name;
    std::vector<Param> params;
    BlockPtr body;
};

struct TopLevel {
    virtual ~TopLevel() = default;
};

struct TopDecl final : TopLevel {
    explicit TopDecl(DeclPtr decl) : decl(std::move(decl)) {}
    DeclPtr decl;
};

struct TopFunc final : TopLevel {
    explicit TopFunc(FuncPtr func) : func(std::move(func)) {}
    FuncPtr func;
};

struct Program {
    std::vector<std::unique_ptr<TopLevel>> items;
};

} // namespace toyc
