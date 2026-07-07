#include "driver/compiler_pipeline.h"
#include "frontend/ast.h"
#include "frontend/lexer.h"
#include "frontend/parser.h"
#include "frontend/semantic.h"

#include <exception>
#include <iostream>
#include <string>

int main(int argc, char* argv[])
{
    bool optimize = false;
    bool emitStats = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-opt") {
            optimize = true;
        } else if (arg == "--stats" || arg == "-stats") {
            emitStats = true;
        }
    }

    try {
        toyc::Lexer lexer(std::cin);
        toyc::Parser parser(lexer.tokenize());
        auto program = parser.parseProgram();

        toyc::SemanticAnalyzer semantic;
        semantic.analyze(*program);

        toyc::CompilerPipeline pipeline({optimize, emitStats});
        pipeline.emitAssembly(*program, std::cout);
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << '\n';
        return 1;
    }

    return 0;
}
