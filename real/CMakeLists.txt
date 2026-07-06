cmake_minimum_required(VERSION 3.12)
project(toyc_compiler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(toyc
    src/main.cpp
    src/lexer.cpp
    src/parser.cpp
    src/semantic.cpp
    src/codegen.cpp
)

target_include_directories(toyc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
