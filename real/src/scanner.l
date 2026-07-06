%{
#include "parser.tab.hpp"
// yylex 的声明已在 parser.tab.hpp 中通过 %code requires 提供
%}

%option noyywrap
%option nounput
%option noinput

%%

[ \t\n]+          { /* 忽略空白 */ }
"//".*            { /* 忽略单行注释 */ }

"int"             { return INT; }
"return"          { return RETURN; }
"if"              { return IF; }
"else"            { return ELSE; }
"while"           { return WHILE; }

[a-zA-Z_][a-zA-Z0-9_]*  { return IDENT; }
[0-9]+            { return NUMBER; }

"+"               { return PLUS; }
"-"               { return MINUS; }
"*"               { return STAR; }
"/"               { return SLASH; }
"="               { return ASSIGN; }
";"               { return SEMICOLON; }
"("               { return LPAREN; }
")"               { return RPAREN; }
"{"               { return LBRACE; }
"}"               { return RBRACE; }

.                 { /* 忽略其他字符 */ }

%%