#include "parser.h"

#include <utility>

namespace toyc {

Parser::Parser(std::vector<Token> tokens)
    : tokens_(std::move(tokens))
{
}

std::unique_ptr<Program> Parser::parseProgram()
{
    auto program = std::make_unique<Program>();
    while (!check(TokenKind::End)) {
        program->items.push_back(parseTopLevel());
    }
    if (program->items.empty()) {
        fail("program cannot be empty");
    }
    return program;
}

const Token& Parser::peek(int offset) const
{
    const std::size_t index = pos_ + static_cast<std::size_t>(offset);
    if (index >= tokens_.size()) {
        return tokens_.back();
    }
    return tokens_[index];
}

bool Parser::check(TokenKind kind) const
{
    return peek().kind == kind;
}

bool Parser::match(TokenKind kind)
{
    if (!check(kind)) {
        return false;
    }
    ++pos_;
    return true;
}

const Token& Parser::consume(TokenKind kind, const std::string& message)
{
    if (!check(kind)) {
        fail(message);
    }
    return tokens_[pos_++];
}

void Parser::fail(const std::string& message) const
{
    throw ParseError(message + " at line " + std::to_string(peek().line) + ", column " + std::to_string(peek().column));
}

std::unique_ptr<TopLevel> Parser::parseTopLevel()
{
    if (check(TokenKind::Const)) {
        return std::make_unique<TopDecl>(parseConstDecl());
    }

    const Type type = parseType();
    std::string name = consume(TokenKind::Identifier, "expected identifier").text;
    if (match(TokenKind::LParen)) {
        return std::make_unique<TopFunc>(parseFuncAfterName(type, std::move(name)));
    }
    if (type != Type::Int) {
        fail("global variable declaration must use int");
    }
    return std::make_unique<TopDecl>(parseVarDeclAfterName(std::move(name)));
}

DeclPtr Parser::parseDecl()
{
    if (check(TokenKind::Const)) {
        return parseConstDecl();
    }
    return parseVarDecl();
}

DeclPtr Parser::parseConstDecl()
{
    consume(TokenKind::Const, "expected const");
    consume(TokenKind::Int, "expected int after const");
    std::string name = consume(TokenKind::Identifier, "expected const name").text;
    consume(TokenKind::Assign, "expected '=' in const declaration");
    auto init = parseExpr();
    consume(TokenKind::Semicolon, "expected ';' after const declaration");
    return std::make_unique<VarDecl>(true, std::move(name), std::move(init));
}

DeclPtr Parser::parseVarDecl()
{
    consume(TokenKind::Int, "expected int in variable declaration");
    std::string name = consume(TokenKind::Identifier, "expected variable name").text;
    return parseVarDeclAfterName(std::move(name));
}

DeclPtr Parser::parseVarDeclAfterName(std::string name)
{
    consume(TokenKind::Assign, "expected '=' in variable declaration");
    auto init = parseExpr();
    consume(TokenKind::Semicolon, "expected ';' after variable declaration");
    return std::make_unique<VarDecl>(false, std::move(name), std::move(init));
}

Type Parser::parseType()
{
    if (match(TokenKind::Int)) {
        return Type::Int;
    }
    if (match(TokenKind::Void)) {
        return Type::Void;
    }
    fail("expected type specifier");
}

FuncPtr Parser::parseFuncAfterName(Type returnType, std::string name)
{
    std::vector<Param> params;
    if (!check(TokenKind::RParen)) {
        params = parseParamList();
    }
    consume(TokenKind::RParen, "expected ')' after function parameters");
    auto body = parseBlock();
    return std::make_unique<FuncDef>(returnType, std::move(name), std::move(params), std::move(body));
}

std::vector<Param> Parser::parseParamList()
{
    std::vector<Param> params;
    do {
        consume(TokenKind::Int, "expected int parameter type");
        params.emplace_back(consume(TokenKind::Identifier, "expected parameter name").text);
    } while (match(TokenKind::Comma));
    return params;
}

BlockPtr Parser::parseBlock()
{
    consume(TokenKind::LBrace, "expected '{'");
    std::vector<StmtPtr> statements;
    while (!check(TokenKind::RBrace) && !check(TokenKind::End)) {
        statements.push_back(parseStmt());
    }
    consume(TokenKind::RBrace, "expected '}'");
    return std::make_unique<BlockStmt>(std::move(statements));
}

StmtPtr Parser::parseStmt()
{
    if (check(TokenKind::LBrace)) {
        return parseBlock();
    }
    if (match(TokenKind::Semicolon)) {
        return std::make_unique<EmptyStmt>();
    }
    if (check(TokenKind::Const) || check(TokenKind::Int)) {
        return std::make_unique<DeclStmt>(parseDecl());
    }
    if (match(TokenKind::If)) {
        consume(TokenKind::LParen, "expected '(' after if");
        auto cond = parseExpr();
        consume(TokenKind::RParen, "expected ')' after if condition");
        auto thenBranch = parseStmt();
        StmtPtr elseBranch;
        if (match(TokenKind::Else)) {
            elseBranch = parseStmt();
        }
        return std::make_unique<IfStmt>(std::move(cond), std::move(thenBranch), std::move(elseBranch));
    }
    if (match(TokenKind::While)) {
        consume(TokenKind::LParen, "expected '(' after while");
        auto cond = parseExpr();
        consume(TokenKind::RParen, "expected ')' after while condition");
        return std::make_unique<WhileStmt>(std::move(cond), parseStmt());
    }
    if (match(TokenKind::Break)) {
        consume(TokenKind::Semicolon, "expected ';' after break");
        return std::make_unique<BreakStmt>();
    }
    if (match(TokenKind::Continue)) {
        consume(TokenKind::Semicolon, "expected ';' after continue");
        return std::make_unique<ContinueStmt>();
    }
    if (match(TokenKind::Return)) {
        if (match(TokenKind::Semicolon)) {
            return std::make_unique<ReturnStmt>(nullptr);
        }
        auto value = parseExpr();
        consume(TokenKind::Semicolon, "expected ';' after return value");
        return std::make_unique<ReturnStmt>(std::move(value));
    }
    if (check(TokenKind::Identifier) && peek(1).kind == TokenKind::Assign) {
        std::string name = consume(TokenKind::Identifier, "expected assignment target").text;
        consume(TokenKind::Assign, "expected '='");
        auto value = parseExpr();
        consume(TokenKind::Semicolon, "expected ';' after assignment");
        return std::make_unique<AssignStmt>(std::move(name), std::move(value));
    }

    auto expr = parseExpr();
    consume(TokenKind::Semicolon, "expected ';' after expression");
    return std::make_unique<ExprStmt>(std::move(expr));
}

ExprPtr Parser::parseExpr()
{
    return parseLogicalOr();
}

ExprPtr Parser::parseLogicalOr()
{
    auto expr = parseLogicalAnd();
    while (match(TokenKind::Or)) {
        expr = std::make_unique<BinaryExpr>(BinaryOp::LogicalOr, std::move(expr), parseLogicalAnd());
    }
    return expr;
}

ExprPtr Parser::parseLogicalAnd()
{
    auto expr = parseRelation();
    while (match(TokenKind::And)) {
        expr = std::make_unique<BinaryExpr>(BinaryOp::LogicalAnd, std::move(expr), parseRelation());
    }
    return expr;
}

ExprPtr Parser::parseRelation()
{
    auto expr = parseAdd();
    while (true) {
        if (match(TokenKind::Less)) {
            expr = std::make_unique<BinaryExpr>(BinaryOp::Less, std::move(expr), parseAdd());
        } else if (match(TokenKind::Greater)) {
            expr = std::make_unique<BinaryExpr>(BinaryOp::Greater, std::move(expr), parseAdd());
        } else if (match(TokenKind::LessEqual)) {
            expr = std::make_unique<BinaryExpr>(BinaryOp::LessEqual, std::move(expr), parseAdd());
        } else if (match(TokenKind::GreaterEqual)) {
            expr = std::make_unique<BinaryExpr>(BinaryOp::GreaterEqual, std::move(expr), parseAdd());
        } else if (match(TokenKind::Equal)) {
            expr = std::make_unique<BinaryExpr>(BinaryOp::Equal, std::move(expr), parseAdd());
        } else if (match(TokenKind::NotEqual)) {
            expr = std::make_unique<BinaryExpr>(BinaryOp::NotEqual, std::move(expr), parseAdd());
        } else {
            return expr;
        }
    }
}

ExprPtr Parser::parseAdd()
{
    auto expr = parseMul();
    while (true) {
        if (match(TokenKind::Plus)) {
            expr = std::make_unique<BinaryExpr>(BinaryOp::Add, std::move(expr), parseMul());
        } else if (match(TokenKind::Minus)) {
            expr = std::make_unique<BinaryExpr>(BinaryOp::Sub, std::move(expr), parseMul());
        } else {
            return expr;
        }
    }
}

ExprPtr Parser::parseMul()
{
    auto expr = parseUnary();
    while (true) {
        if (match(TokenKind::Star)) {
            expr = std::make_unique<BinaryExpr>(BinaryOp::Mul, std::move(expr), parseUnary());
        } else if (match(TokenKind::Slash)) {
            expr = std::make_unique<BinaryExpr>(BinaryOp::Div, std::move(expr), parseUnary());
        } else if (match(TokenKind::Mod)) {
            expr = std::make_unique<BinaryExpr>(BinaryOp::Mod, std::move(expr), parseUnary());
        } else {
            return expr;
        }
    }
}

ExprPtr Parser::parseUnary()
{
    if (match(TokenKind::Plus)) {
        return std::make_unique<UnaryExpr>(UnaryOp::Plus, parseUnary());
    }
    if (match(TokenKind::Minus)) {
        return std::make_unique<UnaryExpr>(UnaryOp::Minus, parseUnary());
    }
    if (match(TokenKind::Not)) {
        return std::make_unique<UnaryExpr>(UnaryOp::Not, parseUnary());
    }
    return parsePrimary();
}

ExprPtr Parser::parsePrimary()
{
    if (match(TokenKind::Number)) {
        return std::make_unique<IntExpr>(tokens_[pos_ - 1].number);
    }
    if (match(TokenKind::Identifier)) {
        std::string name = tokens_[pos_ - 1].text;
        if (match(TokenKind::LParen)) {
            std::vector<ExprPtr> args;
            if (!check(TokenKind::RParen)) {
                args = parseArgList();
            }
            consume(TokenKind::RParen, "expected ')' after call arguments");
            return std::make_unique<CallExpr>(std::move(name), std::move(args));
        }
        return std::make_unique<NameExpr>(std::move(name));
    }
    if (match(TokenKind::LParen)) {
        auto expr = parseExpr();
        consume(TokenKind::RParen, "expected ')' after expression");
        return expr;
    }
    fail("expected expression");
}

std::vector<ExprPtr> Parser::parseArgList()
{
    std::vector<ExprPtr> args;
    do {
        args.push_back(parseExpr());
    } while (match(TokenKind::Comma));
    return args;
}

} // namespace toyc
