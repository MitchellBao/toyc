#include "lexer.h"

#include <cctype>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace toyc {
namespace {

bool isIdentStart(char ch)
{
    return std::isalpha(static_cast<unsigned char>(ch)) || ch == '_';
}

bool isIdentBody(char ch)
{
    return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
}

TokenKind keywordKind(const std::string& text)
{
    switch (text.size()) {
    case 2:
        if (text == "if") {
            return TokenKind::If;
        }
        break;
    case 3:
        if (text == "int") {
            return TokenKind::Int;
        }
        break;
    case 4:
        if (text == "void") {
            return TokenKind::Void;
        }
        if (text == "else") {
            return TokenKind::Else;
        }
        break;
    case 5:
        if (text == "const") {
            return TokenKind::Const;
        }
        if (text == "while") {
            return TokenKind::While;
        }
        if (text == "break") {
            return TokenKind::Break;
        }
        break;
    case 6:
        if (text == "return") {
            return TokenKind::Return;
        }
        break;
    case 8:
        if (text == "continue") {
            return TokenKind::Continue;
        }
        break;
    default:
        break;
    }
    return TokenKind::Identifier;
}

} // namespace

Lexer::Lexer(std::istream& input)
{
    std::ostringstream buffer;
    buffer << input.rdbuf();
    source_ = buffer.str();
}

std::vector<Token> Lexer::tokenize()
{
    std::vector<Token> tokens;
    tokens.reserve(source_.size() / 2 + 1);
    while (true) {
        skipWhitespaceAndComments();
        const int tokenLine = line_;
        const int tokenColumn = column_;
        const char ch = peek();
        if (ch == '\0') {
            tokens.push_back(Token{TokenKind::End, {}, 0, tokenLine, tokenColumn});
            return tokens;
        }

        if (isIdentStart(ch)) {
            std::string text;
            while (isIdentBody(peek())) {
                text.push_back(advance());
            }
            tokens.push_back(Token{keywordKind(text), std::move(text), 0, tokenLine, tokenColumn});
            continue;
        }

        if (std::isdigit(static_cast<unsigned char>(ch))) {
            std::string text;
            long long value = 0;
            while (std::isdigit(static_cast<unsigned char>(peek()))) {
                const char digit = advance();
                text.push_back(digit);
                value = value * 10 + (digit - '0');
                if (value > std::numeric_limits<std::int32_t>::max()) {
                    throw std::runtime_error("integer literal out of range at line " + std::to_string(tokenLine));
                }
            }
            tokens.push_back(Token{TokenKind::Number, std::move(text), static_cast<std::int32_t>(value), tokenLine, tokenColumn});
            continue;
        }

        advance();
        switch (ch) {
        case '+':
            tokens.push_back(Token{TokenKind::Plus, "+", 0, tokenLine, tokenColumn});
            break;
        case '-':
            tokens.push_back(Token{TokenKind::Minus, "-", 0, tokenLine, tokenColumn});
            break;
        case '*':
            tokens.push_back(Token{TokenKind::Star, "*", 0, tokenLine, tokenColumn});
            break;
        case '/':
            tokens.push_back(Token{TokenKind::Slash, "/", 0, tokenLine, tokenColumn});
            break;
        case '%':
            tokens.push_back(Token{TokenKind::Mod, "%", 0, tokenLine, tokenColumn});
            break;
        case ';':
            tokens.push_back(Token{TokenKind::Semicolon, ";", 0, tokenLine, tokenColumn});
            break;
        case ',':
            tokens.push_back(Token{TokenKind::Comma, ",", 0, tokenLine, tokenColumn});
            break;
        case '(':
            tokens.push_back(Token{TokenKind::LParen, "(", 0, tokenLine, tokenColumn});
            break;
        case ')':
            tokens.push_back(Token{TokenKind::RParen, ")", 0, tokenLine, tokenColumn});
            break;
        case '{':
            tokens.push_back(Token{TokenKind::LBrace, "{", 0, tokenLine, tokenColumn});
            break;
        case '}':
            tokens.push_back(Token{TokenKind::RBrace, "}", 0, tokenLine, tokenColumn});
            break;
        case '=':
            if (match('=')) {
                tokens.push_back(Token{TokenKind::Equal, "==", 0, tokenLine, tokenColumn});
            } else {
                tokens.push_back(Token{TokenKind::Assign, "=", 0, tokenLine, tokenColumn});
            }
            break;
        case '!':
            if (match('=')) {
                tokens.push_back(Token{TokenKind::NotEqual, "!=", 0, tokenLine, tokenColumn});
            } else {
                tokens.push_back(Token{TokenKind::Not, "!", 0, tokenLine, tokenColumn});
            }
            break;
        case '<':
            if (match('=')) {
                tokens.push_back(Token{TokenKind::LessEqual, "<=", 0, tokenLine, tokenColumn});
            } else {
                tokens.push_back(Token{TokenKind::Less, "<", 0, tokenLine, tokenColumn});
            }
            break;
        case '>':
            if (match('=')) {
                tokens.push_back(Token{TokenKind::GreaterEqual, ">=", 0, tokenLine, tokenColumn});
            } else {
                tokens.push_back(Token{TokenKind::Greater, ">", 0, tokenLine, tokenColumn});
            }
            break;
        case '&':
            if (!match('&')) {
                throw std::runtime_error("expected '&' after '&' at line " + std::to_string(tokenLine));
            }
            tokens.push_back(Token{TokenKind::And, "&&", 0, tokenLine, tokenColumn});
            break;
        case '|':
            if (!match('|')) {
                throw std::runtime_error("expected '|' after '|' at line " + std::to_string(tokenLine));
            }
            tokens.push_back(Token{TokenKind::Or, "||", 0, tokenLine, tokenColumn});
            break;
        default:
            throw std::runtime_error("invalid character at line " + std::to_string(tokenLine));
        }
    }
}

char Lexer::peek(int offset) const
{
    const std::size_t index = pos_ + static_cast<std::size_t>(offset);
    return index < source_.size() ? source_[index] : '\0';
}

char Lexer::advance()
{
    const char ch = peek();
    if (ch == '\0') {
        return ch;
    }
    ++pos_;
    if (ch == '\n') {
        ++line_;
        column_ = 1;
    } else {
        ++column_;
    }
    return ch;
}

bool Lexer::match(char expected)
{
    if (peek() != expected) {
        return false;
    }
    advance();
    return true;
}

void Lexer::skipWhitespaceAndComments()
{
    while (true) {
        while (std::isspace(static_cast<unsigned char>(peek()))) {
            advance();
        }
        if (peek() == '/' && peek(1) == '/') {
            while (peek() != '\n' && peek() != '\0') {
                advance();
            }
            continue;
        }
        if (peek() == '/' && peek(1) == '*') {
            advance();
            advance();
            while (!(peek() == '*' && peek(1) == '/')) {
                if (peek() == '\0') {
                    throw std::runtime_error("unterminated block comment");
                }
                advance();
            }
            advance();
            advance();
            continue;
        }
        break;
    }
}

} // namespace toyc
