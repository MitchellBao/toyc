#include "lexer.h"

#include <cctype>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

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
            static const std::unordered_map<std::string, TokenKind> keywords = {
                {"const", TokenKind::Const},
                {"int", TokenKind::Int},
                {"void", TokenKind::Void},
                {"return", TokenKind::Return},
                {"if", TokenKind::If},
                {"else", TokenKind::Else},
                {"while", TokenKind::While},
                {"break", TokenKind::Break},
                {"continue", TokenKind::Continue},
            };
            const auto found = keywords.find(text);
            tokens.push_back(Token{found == keywords.end() ? TokenKind::Identifier : found->second, text, 0, tokenLine, tokenColumn});
            continue;
        }

        if (std::isdigit(static_cast<unsigned char>(ch))) {
            std::string text;
            while (std::isdigit(static_cast<unsigned char>(peek()))) {
                text.push_back(advance());
            }
            const long long value = std::strtoll(text.c_str(), nullptr, 10);
            if (value > std::numeric_limits<std::int32_t>::max()) {
                throw std::runtime_error("integer literal out of range at line " + std::to_string(tokenLine));
            }
            tokens.push_back(Token{TokenKind::Number, text, static_cast<std::int32_t>(value), tokenLine, tokenColumn});
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
