%{
#include <stdio.h>
// 可以在这里包含你将来要用的 C++ 头文件
%}

/* 声明 token */
%token INT RETURN IF ELSE WHILE
%token IDENT NUMBER
%token PLUS MINUS STAR SLASH ASSIGN
%token SEMICOLON LPAREN RPAREN LBRACE RBRACE

/* 在生成的头文件中声明 yylex 和 yyerror，统一用 C++ 链接 */
%code requires {
    int yylex(void);
    void yyerror(const char *s);
}

%%

program:
    function
;

function:
    INT IDENT LPAREN RPAREN compound_statement
;

compound_statement:
    LBRACE statement_list RBRACE
;

statement_list:
    /* empty */
  | statement_list statement
;

statement:
    RETURN expression SEMICOLON
;

expression:
    NUMBER
  | IDENT
  | expression PLUS expression
  | expression MINUS expression
  | expression STAR expression
  | expression SLASH expression
;

%%

void yyerror(const char *s) {
    fprintf(stderr, "Error: %s\n", s);
}