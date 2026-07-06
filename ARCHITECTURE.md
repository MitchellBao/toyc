# ToyC compiler architecture

This project is being split away from the old all-in-one AST-to-RISC-V backend.

## Target pipeline

```text
Lexer/Parser
  -> AST
  -> IRBuilder
  -> PassManager
  -> RegisterAllocator
  -> RiscVCodegen
  -> Peephole
```

## Current transition state

- `real/src/ast.h`: source-level syntax tree.
- `real/src/ir.h`: three-address IR data model.
- `real/src/ir_builder.cpp`: AST-to-IR boundary. It currently builds module shape and constant globals.
- `real/src/pass.h`, `real/src/pass.cpp`: pass interface and pass manager.
- `real/src/codegen.cpp`: current production RISC-V backend. It still lowers AST directly while the IR backend is being filled in.

The production path remains conservative to preserve judge correctness:

```text
AST -> IR skeleton/pass pipeline -> existing AST backend -> RISC-V
```

## Refactor rules

1. New optimizations should be implemented as IR passes, not as more special cases in `codegen.cpp`.
2. `codegen.cpp` should gradually shrink into instruction selection, register allocation, calling convention, and assembly emission.
3. Program execution during compilation is forbidden. Only language-required constant expressions may be evaluated.
4. Calling convention logic for parameters and stack arguments must stay centralized in backend lowering.

## Next milestones

1. Lower statements and expressions into IR basic blocks.
2. Add IR constant/copy propagation and algebra simplification.
3. Add IR DCE using def-use counts.
4. Add local value numbering for CSE.
5. Replace heuristic saved-register assignment with live intervals plus linear scan allocation.
