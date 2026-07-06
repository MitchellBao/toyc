CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -pipe

TARGET := compiler
SOURCES := \
	real/src/main.cpp \
	real/src/lexer.cpp \
	real/src/parser.cpp \
	real/src/semantic.cpp \
	real/src/codegen.cpp

HEADERS := \
	real/src/ast.h \
	real/src/lexer.h \
	real/src/parser.h \
	real/src/semantic.h \
	real/src/codegen.h

.PHONY: all

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) -Ireal/src $(SOURCES) -o $(TARGET)
