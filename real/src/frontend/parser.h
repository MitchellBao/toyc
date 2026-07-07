#pragma once

#include "ast.h"
#include "lexer.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace toyc {

class ParseError final : public std::runtime_error {
public:
    explicit ParseError(const std::string& message)
        : std::runtime_error(message)
    {
    }
};

class Parser {
public:
    explicit Parser(std::vector<Token> tokens);

    std::unique_ptr<Program> parseProgram();

private:
    const Token& peek(int offset = 0) const;
    bool check(TokenKind kind) const;
    bool match(TokenKind kind);
    const Token& consume(TokenKind kind, const std::string& message);
    [[noreturn]] void fail(const std::string& message) const;

    std::unique_ptr<TopLevel> parseTopLevel();
    DeclPtr parseDecl();
    DeclPtr parseConstDecl();
    DeclPtr parseVarDecl();
    DeclPtr parseVarDeclAfterName(std::string name);
    Type parseType();
    FuncPtr parseFuncAfterName(Type returnType, std::string name);
    std::vector<Param> parseParamList();
    BlockPtr parseBlock();
    StmtPtr parseStmt();
    ExprPtr parseExpr();
    ExprPtr parseLogicalOr();
    ExprPtr parseLogicalAnd();
    ExprPtr parseRelation();
    ExprPtr parseAdd();
    ExprPtr parseMul();
    ExprPtr parseUnary();
    ExprPtr parsePrimary();
    std::vector<ExprPtr> parseArgList();

    std::vector<Token> tokens_;
    std::size_t pos_ = 0;
};

} // namespace toyc
