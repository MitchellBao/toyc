CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -pipe

TARGET := compiler
SOURCES := \
	real/src/main.cpp \
	real/src/frontend/lexer.cpp \
	real/src/frontend/parser.cpp \
	real/src/frontend/semantic.cpp \
	real/src/driver/compiler_pipeline.cpp \
	real/src/ir/instruction.cpp \
	real/src/ir/builder.cpp \
	real/src/ir/verifier.cpp \
	real/src/ir/printer.cpp \
	real/src/analysis/cfg.cpp \
	real/src/analysis/liveness.cpp \
	real/src/analysis/side_effect.cpp \
	real/src/passes/pass_manager.cpp \
	real/src/passes/canonicalize.cpp \
	real/src/passes/simplify_cfg.cpp \
	real/src/passes/const_prop.cpp \
	real/src/passes/global_const_prop.cpp \
	real/src/passes/global_copy_prop.cpp \
	real/src/passes/global_cse.cpp \
	real/src/passes/algebraic_simplify.cpp \
	real/src/passes/copy_prop.cpp \
	real/src/passes/local_coalesce.cpp \
	real/src/passes/cse.cpp \
	real/src/passes/inst_combine.cpp \
	real/src/passes/dce.cpp \
	real/src/passes/dse.cpp \
	real/src/passes/dead_function_elim.cpp \
	real/src/passes/licm.cpp \
	real/src/passes/loop_sum.cpp \
	real/src/passes/linearize_blocks.cpp \
	real/src/passes/inline_small.cpp \
	real/src/passes/tail_recursion.cpp \
	real/src/target/riscv/isel.cpp \
	real/src/target/riscv/frame.cpp \
	real/src/target/riscv/regalloc.cpp \
	real/src/target/riscv/peephole.cpp \
	real/src/target/riscv/asm_printer.cpp

HEADERS := \
	real/src/frontend/ast.h \
	real/src/frontend/lexer.h \
	real/src/frontend/parser.h \
	real/src/frontend/semantic.h \
	real/src/driver/options.h \
	real/src/driver/compiler_pipeline.h \
	real/src/ir/value.h \
	real/src/ir/instruction.h \
	real/src/ir/basic_block.h \
	real/src/ir/function.h \
	real/src/ir/module.h \
	real/src/ir/builder.h \
	real/src/ir/verifier.h \
	real/src/ir/printer.h \
	real/src/analysis/cfg.h \
	real/src/analysis/dominator.h \
	real/src/analysis/loop_info.h \
	real/src/analysis/liveness.h \
	real/src/analysis/side_effect.h \
	real/src/passes/pass_manager.h \
	real/src/target/riscv/riscv_mir.h \
	real/src/target/riscv/isel.h \
	real/src/target/riscv/frame.h \
	real/src/target/riscv/regalloc.h \
	real/src/target/riscv/peephole.h \
	real/src/target/riscv/asm_printer.h

.PHONY: all

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) -Ireal/src $(SOURCES) -o $(TARGET)
