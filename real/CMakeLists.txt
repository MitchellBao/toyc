cmake_minimum_required(VERSION 3.12)
project(toyc_compiler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. 定位 MSYS2 中的 flex 和 bison
find_program(FLEX flex REQUIRED)
find_program(BISON bison REQUIRED)

# 2. 用 Bison 生成 parser.tab.cpp 和 parser.tab.hpp
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/parser.tab.cpp
           ${CMAKE_CURRENT_BINARY_DIR}/parser.tab.hpp
    COMMAND ${BISON}
        -o ${CMAKE_CURRENT_BINARY_DIR}/parser.tab.cpp
        --defines=${CMAKE_CURRENT_BINARY_DIR}/parser.tab.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/parser.y
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/parser.y
    COMMENT "Running bison to generate parser"
)

# 3. 用 Flex 生成 scanner.cpp
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/scanner.cpp
    COMMAND ${FLEX}
        -o ${CMAKE_CURRENT_BINARY_DIR}/scanner.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/scanner.l
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/scanner.l
            # 确保 parser 头文件先生成
            ${CMAKE_CURRENT_BINARY_DIR}/parser.tab.hpp
    COMMENT "Running flex to generate scanner"
)

# 4. 编译生成的文件和主程序
add_executable(toyc
    src/main.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/parser.tab.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/scanner.cpp
)

# 让编译器能找到生成的头文件 parser.tab.hpp
target_include_directories(toyc PRIVATE ${CMAKE_CURRENT_BINARY_DIR})