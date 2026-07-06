#include "ast.h"
#include "codegen.h"
#include "lexer.h"
#include "parser.h"
#include "semantic.h"

#include <exception>
#include <iostream>
#include <string>

int main(int argc, char* argv[])
{
    bool optimize = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "-opt") {
            optimize = true;
        }
    }

    try {
        toyc::Lexer lexer(std::cin);
        toyc::Parser parser(lexer.tokenize());
        auto program = parser.parseProgram();

        toyc::SemanticAnalyzer semantic;
        semantic.analyze(*program);

        toyc::RiscVCodeGenerator codegen({optimize});
        codegen.generate(*program, std::cout);
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << '\n';
        return 1;
    }

    return 0;
}
