%{
#include "ast.h"

#include <cstdlib>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

int yylex(void);
void yyerror(const char *s);

namespace toyc {
Program* g_program = nullptr;
}

template <typename T>
static std::vector<std::unique_ptr<T>> adoptVector(std::vector<T*>* raw)
{
    std::vector<std::unique_ptr<T>> result;
    if (raw != nullptr) {
        result.reserve(raw->size());
        for (T* item : *raw) {
            result.emplace_back(item);
        }
        delete raw;
    }
    return result;
}

static std::vector<toyc::Param> adoptParams(std::vector<toyc::Param*>* raw)
{
    std::vector<toyc::Param> result;
    if (raw != nullptr) {
        result.reserve(raw->size());
        for (toyc::Param* item : *raw) {
            result.push_back(std::move(*item));
            delete item;
        }
        delete raw;
    }
    return result;
}
%}

%define parse.error verbose

%code requires {
    #include "ast.h"
    int yylex(void);
    void yyerror(const char *s);
}

%union {
    int number;
    char* text;
    toyc::Type type;
    toyc::Program* program;
    toyc::TopLevel* top;
    std::vector<toyc::TopLevel*>* topList;
    toyc::Decl* decl;
    toyc::Stmt* stmt;
    toyc::BlockStmt* block;
    toyc::FuncDef* func;
    toyc::Param* param;
    std::vector<toyc::Param*>* paramList;
    toyc::Expr* expr;
    std::vector<toyc::Expr*>* exprList;
    std::vector<toyc::Stmt*>* stmtList;
}

%token CONST INT VOID RETURN IF ELSE WHILE BREAK CONTINUE
%token <text> ID
%token <number> NUMBER
%token PLUS MINUS STAR SLASH MOD ASSIGN
%token LT GT LE GE EQ NE AND OR NOT
%token SEMICOLON COMMA LPAREN RPAREN LBRACE RBRACE

%type <program> comp_unit
%type <topList> top_list
%type <top> top_item
%type <decl> decl const_decl var_decl
%type <func> func_def
%type <type> type_spec
%type <param> param
%type <paramList> param_list param_list_opt
%type <block> block
%type <stmtList> stmt_list
%type <stmt> stmt matched_stmt unmatched_stmt
%type <expr> expr lor_expr land_expr rel_expr add_expr mul_expr unary_expr primary_expr
%type <exprList> arg_list arg_list_opt

%nonassoc LOWER_THAN_ELSE
%nonassoc ELSE

%start comp_unit

%%

comp_unit:
    top_list
    {
        auto* program = new toyc::Program();
        for (toyc::TopLevel* item : *$1) {
            program->items.emplace_back(item);
        }
        delete $1;
        toyc::g_program = program;
        $$ = program;
    }
;

top_list:
    top_item
    {
        $$ = new std::vector<toyc::TopLevel*>();
        $$->push_back($1);
    }
  | top_list top_item
    {
        $1->push_back($2);
        $$ = $1;
    }
;

top_item:
    decl
    {
        $$ = new toyc::TopDecl(toyc::take($1));
    }
  | func_def
    {
        $$ = new toyc::TopFunc(toyc::take($1));
    }
;

decl:
    const_decl { $$ = $1; }
  | var_decl { $$ = $1; }
;

const_decl:
    CONST INT ID ASSIGN expr SEMICOLON
    {
        $$ = new toyc::VarDecl(true, $3, toyc::take($5));
        std::free($3);
    }
;

var_decl:
    INT ID ASSIGN expr SEMICOLON
    {
        $$ = new toyc::VarDecl(false, $2, toyc::take($4));
        std::free($2);
    }
;

type_spec:
    INT { $$ = toyc::Type::Int; }
  | VOID { $$ = toyc::Type::Void; }
;

func_def:
    type_spec ID LPAREN param_list_opt RPAREN block
    {
        $$ = new toyc::FuncDef($1, $2, adoptParams($4), toyc::take($6));
        std::free($2);
    }
;

param_list_opt:
    /* empty */
    {
        $$ = new std::vector<toyc::Param*>();
    }
  | param_list
    {
        $$ = $1;
    }
;

param_list:
    param
    {
        $$ = new std::vector<toyc::Param*>();
        $$->push_back($1);
    }
  | param_list COMMA param
    {
        $1->push_back($3);
        $$ = $1;
    }
;

param:
    INT ID
    {
        $$ = new toyc::Param($2);
        std::free($2);
    }
;

block:
    LBRACE stmt_list RBRACE
    {
        $$ = new toyc::BlockStmt(adoptVector($2));
    }
;

stmt_list:
    /* empty */
    {
        $$ = new std::vector<toyc::Stmt*>();
    }
  | stmt_list stmt
    {
        $1->push_back($2);
        $$ = $1;
    }
;

stmt:
    matched_stmt { $$ = $1; }
  | unmatched_stmt { $$ = $1; }
;

matched_stmt:
    block
    {
        $$ = $1;
    }
  | SEMICOLON
    {
        $$ = new toyc::EmptyStmt();
    }
  | expr SEMICOLON
    {
        $$ = new toyc::ExprStmt(toyc::take($1));
    }
  | ID ASSIGN expr SEMICOLON
    {
        $$ = new toyc::AssignStmt($1, toyc::take($3));
        std::free($1);
    }
  | decl
    {
        $$ = new toyc::DeclStmt(toyc::take($1));
    }
  | IF LPAREN expr RPAREN matched_stmt ELSE matched_stmt
    {
        $$ = new toyc::IfStmt(toyc::take($3), toyc::take($5), toyc::take($7));
    }
  | WHILE LPAREN expr RPAREN matched_stmt
    {
        $$ = new toyc::WhileStmt(toyc::take($3), toyc::take($5));
    }
  | BREAK SEMICOLON
    {
        $$ = new toyc::BreakStmt();
    }
  | CONTINUE SEMICOLON
    {
        $$ = new toyc::ContinueStmt();
    }
  | RETURN SEMICOLON
    {
        $$ = new toyc::ReturnStmt(nullptr);
    }
  | RETURN expr SEMICOLON
    {
        $$ = new toyc::ReturnStmt(toyc::take($2));
    }
;

unmatched_stmt:
    IF LPAREN expr RPAREN stmt %prec LOWER_THAN_ELSE
    {
        $$ = new toyc::IfStmt(toyc::take($3), toyc::take($5), nullptr);
    }
  | IF LPAREN expr RPAREN matched_stmt ELSE unmatched_stmt
    {
        $$ = new toyc::IfStmt(toyc::take($3), toyc::take($5), toyc::take($7));
    }
  | WHILE LPAREN expr RPAREN unmatched_stmt
    {
        $$ = new toyc::WhileStmt(toyc::take($3), toyc::take($5));
    }
;

expr:
    lor_expr { $$ = $1; }
;

lor_expr:
    land_expr { $$ = $1; }
  | lor_expr OR land_expr
    {
        $$ = new toyc::BinaryExpr(toyc::BinaryOp::LogicalOr, toyc::take($1), toyc::take($3));
    }
;

land_expr:
    rel_expr { $$ = $1; }
  | land_expr AND rel_expr
    {
        $$ = new toyc::BinaryExpr(toyc::BinaryOp::LogicalAnd, toyc::take($1), toyc::take($3));
    }
;

rel_expr:
    add_expr { $$ = $1; }
  | rel_expr LT add_expr
    {
        $$ = new toyc::BinaryExpr(toyc::BinaryOp::Less, toyc::take($1), toyc::take($3));
    }
  | rel_expr GT add_expr
    {
        $$ = new toyc::BinaryExpr(toyc::BinaryOp::Greater, toyc::take($1), toyc::take($3));
    }
  | rel_expr LE add_expr
    {
        $$ = new toyc::BinaryExpr(toyc::BinaryOp::LessEqual, toyc::take($1), toyc::take($3));
    }
  | rel_expr GE add_expr
    {
        $$ = new toyc::BinaryExpr(toyc::BinaryOp::GreaterEqual, toyc::take($1), toyc::take($3));
    }
  | rel_expr EQ add_expr
    {
        $$ = new toyc::BinaryExpr(toyc::BinaryOp::Equal, toyc::take($1), toyc::take($3));
    }
  | rel_expr NE add_expr
    {
        $$ = new toyc::BinaryExpr(toyc::BinaryOp::NotEqual, toyc::take($1), toyc::take($3));
    }
;

add_expr:
    mul_expr { $$ = $1; }
  | add_expr PLUS mul_expr
    {
        $$ = new toyc::BinaryExpr(toyc::BinaryOp::Add, toyc::take($1), toyc::take($3));
    }
  | add_expr MINUS mul_expr
    {
        $$ = new toyc::BinaryExpr(toyc::BinaryOp::Sub, toyc::take($1), toyc::take($3));
    }
;

mul_expr:
    unary_expr { $$ = $1; }
  | mul_expr STAR unary_expr
    {
        $$ = new toyc::BinaryExpr(toyc::BinaryOp::Mul, toyc::take($1), toyc::take($3));
    }
  | mul_expr SLASH unary_expr
    {
        $$ = new toyc::BinaryExpr(toyc::BinaryOp::Div, toyc::take($1), toyc::take($3));
    }
  | mul_expr MOD unary_expr
    {
        $$ = new toyc::BinaryExpr(toyc::BinaryOp::Mod, toyc::take($1), toyc::take($3));
    }
;

unary_expr:
    primary_expr { $$ = $1; }
  | PLUS unary_expr
    {
        $$ = new toyc::UnaryExpr(toyc::UnaryOp::Plus, toyc::take($2));
    }
  | MINUS unary_expr
    {
        $$ = new toyc::UnaryExpr(toyc::UnaryOp::Minus, toyc::take($2));
    }
  | NOT unary_expr
    {
        $$ = new toyc::UnaryExpr(toyc::UnaryOp::Not, toyc::take($2));
    }
;

primary_expr:
    ID
    {
        $$ = new toyc::NameExpr($1);
        std::free($1);
    }
  | NUMBER
    {
        $$ = new toyc::IntExpr($1);
    }
  | LPAREN expr RPAREN
    {
        $$ = $2;
    }
  | ID LPAREN arg_list_opt RPAREN
    {
        $$ = new toyc::CallExpr($1, adoptVector($3));
        std::free($1);
    }
;

arg_list_opt:
    /* empty */
    {
        $$ = new std::vector<toyc::Expr*>();
    }
  | arg_list
    {
        $$ = $1;
    }
;

arg_list:
    expr
    {
        $$ = new std::vector<toyc::Expr*>();
        $$->push_back($1);
    }
  | arg_list COMMA expr
    {
        $1->push_back($3);
        $$ = $1;
    }
;

%%

void yyerror(const char *s)
{
    std::fprintf(stderr, "parse error: %s\n", s);
}
