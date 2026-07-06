#include <iostream>
#include "parser.tab.hpp"



int main() {
    std::cout << "Starting toyC parser (C++)...\n";
    if (yyparse() == 0) {
        std::cout << "Parsing successful!" << std::endl;
    } else {
        std::cout << "Parsing failed." << std::endl;
    }
    return 0;
}