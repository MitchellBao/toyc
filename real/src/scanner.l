%{
#include "parser.tab.hpp"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>

static char* copyText(const char* text)
{
    const std::size_t len = std::strlen(text);
    char* result = static_cast<char*>(std::malloc(len + 1));
    if (result == nullptr) {
        std::abort();
    }
    std::memcpy(result, text, len + 1);
    return result;
}
%}

%option noyywrap
%option nounput
%option noinput

%x COMMENT

%%

[ \t\r\n]+        { /* skip whitespace */ }
"//".*            { /* skip line comment */ }
"/*"              { BEGIN(COMMENT); }
<COMMENT>"*/"     { BEGIN(INITIAL); }
<COMMENT>\n       { /* skip comment newline */ }
<COMMENT>.        { /* skip comment body */ }
<COMMENT><<EOF>>  { yyerror("unterminated block comment"); return 0; }

"const"           { return CONST; }
"int"             { return INT; }
"void"            { return VOID; }
"return"          { return RETURN; }
"if"              { return IF; }
"else"            { return ELSE; }
"while"           { return WHILE; }
"break"           { return BREAK; }
"continue"        { return CONTINUE; }

"&&"              { return AND; }
"||"              { return OR; }
"<="              { return LE; }
">="              { return GE; }
"=="              { return EQ; }
"!="              { return NE; }

"+"               { return PLUS; }
"-"               { return MINUS; }
"*"               { return STAR; }
"/"               { return SLASH; }
"%"               { return MOD; }
"="               { return ASSIGN; }
"<"               { return LT; }
">"               { return GT; }
"!"               { return NOT; }
";"               { return SEMICOLON; }
","               { return COMMA; }
"("               { return LPAREN; }
")"               { return RPAREN; }
"{"               { return LBRACE; }
"}"               { return RBRACE; }

[0-9]+            {
                      errno = 0;
                      long value = std::strtol(yytext, nullptr, 10);
                      if (errno != 0 || value < INT_MIN || value > INT_MAX) {
                          yyerror("integer literal out of range");
                          return 0;
                      }
                      yylval.number = static_cast<int>(value);
                      return NUMBER;
                  }

[_A-Za-z][_A-Za-z0-9]* {
                      yylval.text = copyText(yytext);
                      return ID;
                  }

.                 { yyerror("invalid character"); return 0; }

%%
