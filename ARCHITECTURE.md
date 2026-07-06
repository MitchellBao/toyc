# ToyC compiler architecture

The project is being kept in three ownership areas so optimization work does not keep growing inside `codegen.cpp`.

## Pipeline

```text
Lexer/Parser
  -> AST
  -> AST Optimizer
  -> DAG
  -> IR
  -> IR Pass Pipeline
  -> IR RISC-V Backend
  -> Assembly
```

The current production path is conservative:

```text
AST -> AST Optimizer -> DAG skeleton -> IR skeleton/pass pipeline -> existing AST backend -> RISC-V
```

The IR backend exists as an explicit boundary but is not enabled until it can cover full ToyC semantics.

## 1. AST/DAG/IR Front-Middle Boundary

Owned files:

- `real/src/dag.h`, `real/src/dag.cpp`: DAG data model.
- `real/src/dag_builder.h`, `real/src/dag_builder.cpp`: AST to DAG.
- `real/src/dag_to_ir.h`, `real/src/dag_to_ir.cpp`: DAG to IR boundary.
- `real/src/ir.h`, `real/src/ir.cpp`: IR data model.
- `real/src/ir_builder.h`, `real/src/ir_builder.cpp`: legacy AST to IR lowering kept as a reference while DAG-to-IR is filled in.

Rules:

1. DAG owns expression sharing and front-end normalization.
2. IR owns control-flow, values, effects, and backend-facing operations.
3. No RISC-V instruction selection belongs here.

## 2. IR Optimization Passes

Owned files:

- `real/src/pass.h`, `real/src/pass.cpp`: pass interface and pipeline wiring only.
- `real/src/pass_simplify.cpp`: local constant/copy propagation and algebra simplification.
- `real/src/pass_dce.cpp`: dead instruction cleanup.

Rules:

1. Add each major pass in its own file, for example `pass_cse.cpp`, `pass_loop.cpp`, `pass_regalloc.cpp`.
2. `pass.cpp` should not contain optimization algorithms.
3. Passes must preserve side effects: calls, global stores, and short-circuit behavior are not removable unless proven safe.

## 3. IR Backend

Owned files:

- `real/src/ir_codegen.h`, `real/src/ir_codegen.cpp`: optimized IR to RISC-V backend boundary.
- `real/src/codegen.h`, `real/src/codegen.cpp`: current production AST backend and compatibility wrapper.

Rules:

1. New backend work should target `IrRiscVCodeGenerator`.
2. Calling convention, stack layout, register assignment, and instruction selection belong in backend code.
3. Generic optimizations should not be added to `codegen.cpp`; put them in DAG/IR passes first.

## Immediate Milestones

1. Fill `DagToIrBuilder` beyond skeleton: statements, control-flow, calls, locals, globals.
2. Move remaining generic simplifications from `codegen.cpp` into DAG/IR passes.
3. Implement `IrRiscVCodeGenerator` for full ToyC, then switch production output to IR backend.
4. Add local value numbering/CSE and loop-invariant code motion as separate pass files.
