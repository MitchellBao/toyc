CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -pipe

TARGET := compiler
SOURCES := \
	real/src/main.cpp \
	real/src/lexer.cpp \
	real/src/parser.cpp \
	real/src/semantic.cpp \
	real/src/ast_optimizer.cpp \
	real/src/ir.cpp \
	real/src/ir_builder.cpp \
	real/src/pass.cpp \
    real/src/pass_simplify.cpp \
    real/src/pass_cse.cpp \
    real/src/pass_local.cpp \
    real/src/pass_loop.cpp \
    real/src/pass_dce.cpp \
	real/src/ir_codegen.cpp \
	real/src/codegen.cpp

HEADERS := \
	real/src/ast.h \
	real/src/lexer.h \
	real/src/parser.h \
	real/src/semantic.h \
	real/src/ast_optimizer.h \
	real/src/ir.h \
	real/src/ir_builder.h \
	real/src/ir_codegen.h \
	real/src/pass.h \
	real/src/codegen.h

.PHONY: all

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) -Ireal/src $(SOURCES) -o $(TARGET)
