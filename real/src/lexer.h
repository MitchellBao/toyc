#pragma once

#include <cstdint>
#include <istream>
#include <string>
#include <vector>

namespace toyc {

enum class TokenKind {
    End,
    Invalid,
    Const,
    Int,
    Void,
    Return,
    If,
    Else,
    While,
    Break,
    Continue,
    Identifier,
    Number,
    Plus,
    Minus,
    Star,
    Slash,
    Mod,
    Assign,
    Less,
    Greater,
    LessEqual,
    GreaterEqual,
    Equal,
    NotEqual,
    And,
    Or,
    Not,
    Semicolon,
    Comma,
    LParen,
    RParen,
    LBrace,
    RBrace,
};

struct Token {
    TokenKind kind = TokenKind::Invalid;
    std::string text;
    std::int32_t number = 0;
    int line = 1;
    int column = 1;
};

class Lexer {
public:
    explicit Lexer(std::istream& input);

    std::vector<Token> tokenize();

private:
    char peek(int offset = 0) const;
    char advance();
    bool match(char expected);
    void skipWhitespaceAndComments();
    Token makeToken(TokenKind kind, std::string text = {}, std::int32_t number = 0) const;

    std::string source_;
    std::size_t pos_ = 0;
    int line_ = 1;
    int column_ = 1;
};

} // namespace toyc
