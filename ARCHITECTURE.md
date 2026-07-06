# ToyC compiler architecture

This project is being split away from the old all-in-one AST-to-RISC-V backend.

## Target pipeline

```text
Lexer/Parser
  -> AST
  -> AST Optimizer
  -> IRBuilder
  -> PassManager
  -> RegisterAllocator
  -> RiscVCodegen
  -> Peephole
```

## Current transition state

- `real/src/ast.h`: source-level syntax tree.
- `real/src/ast_optimizer.cpp`: conservative source-level middle-end used by the current production backend.
- `real/src/ir.h`: three-address IR data model.
- `real/src/ir_builder.cpp`: AST-to-IR boundary. It lowers globals, function bodies, expressions, and short-circuit control flow into basic-block IR.
- `real/src/pass.h`, `real/src/pass.cpp`: pass interface, pass manager, local IR simplification, and dead-instruction cleanup.
- `real/src/codegen.cpp`: current production RISC-V backend. It still lowers AST directly while the IR backend is being filled in.

The production path remains conservative to preserve judge correctness:

```text
AST -> AST optimizer -> IR/pass pipeline -> existing AST backend -> RISC-V
```

## Refactor rules

1. New optimizations should be implemented as IR passes, not as more special cases in `codegen.cpp`.
2. `codegen.cpp` should gradually shrink into instruction selection, register allocation, calling convention, and assembly emission.
3. Production optimizations that must affect today's backend can live in `ast_optimizer.cpp` until the IR backend is complete.
4. Program execution during compilation is forbidden. Only language-required constant expressions and local expression folds may be evaluated.
5. Calling convention logic for parameters and stack arguments must stay centralized in backend lowering.

## Next milestones

1. Add local value numbering for IR CSE.
2. Add loop analysis plus loop-invariant code motion.
3. Move the remaining generic simplifications out of `codegen.cpp` after they are covered by AST/IR passes.
4. Replace heuristic saved-register assignment with live intervals plus linear scan allocation.
