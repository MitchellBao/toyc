# ToyC Compiler Architecture

This version is a hand-written LLVM/QBE-style compiler architecture. The old
production AST backend, AST optimizer, IR backend selector, and text-cost
backend chooser have been removed. Both normal mode and `-opt` now use one
compiler pipeline.

## Production Pipeline

```text
stdin
  -> frontend::Lexer
  -> frontend::Parser
  -> frontend AST
  -> frontend::SemanticAnalyzer
  -> ir::IrBuilder
  -> ir::Verifier
  -> passes::PassManager
  -> ir::Verifier
  -> riscv::AsmPrinter
  -> stdout
```

Plain mode and optimized mode differ only by pass pipeline:

```text
plain:
  AST -> IR -> canonicalize -> simplify-cfg -> RISC-V

-opt:
  AST -> IR
      -> canonicalize
      -> simplify-cfg
      -> const-prop
      -> copy-prop
      -> local-cse
      -> dce
      -> dse
      -> licm
      -> tail-recursion
      -> inline-small
      -> second cleanup round
      -> RISC-V
```

The current `licm`, `tail-recursion`, `inline-small`, and `dse` passes are
architectural slots with conservative implementations. They are intentionally
separate files so future optimization work does not grow back into a monolithic
backend.

## Module Layout

```text
real/src/
  frontend/
    ast.h
    lexer.*
    parser.*
    semantic.*

  driver/
    options.h
    compiler_pipeline.*

  ir/
    value.h
    instruction.*
    basic_block.h
    function.h
    module.h
    builder.*
    verifier.*
    printer.*

  analysis/
    cfg.*
    dominator.h
    loop_info.h
    liveness.*
    side_effect.*

  passes/
    pass_manager.*
    canonicalize.cpp
    simplify_cfg.cpp
    const_prop.cpp
    copy_prop.cpp
    cse.cpp
    dce.cpp
    dse.cpp
    licm.cpp
    inline_small.cpp
    tail_recursion.cpp

  target/riscv/
    riscv_mir.h
    isel.*
    frame.*
    regalloc.*
    peephole.*
    asm_printer.*
```

## Removed Production Layers

Deleted files:

- `real/src/codegen.*`
- `real/src/ast_optimizer.*`
- `real/src/ir_codegen.*`
- old flat `real/src/ir.*`
- old flat `real/src/ir_builder.*`
- old flat `real/src/pass*`

Removed responsibilities:

- AST-to-RISC-V production backend.
- AST-level optimization as a main optimization layer.
- Generating two assemblies and choosing by static text cost.
- Environment-variable backend forcing for AST/IR/auto.
- One giant backend file owning semantic-ish facts, optimization, frame layout,
  register choices, and assembly emission at the same time.

## IR Design

The IR is a typed, basic-block, three-address representation:

- `ir::Module` owns globals and functions.
- `ir::Function` owns parameters, blocks, and value numbering.
- `ir::BasicBlock` owns instructions plus one explicit terminator.
- `ir::Instruction` represents pure computations, loads, stores, and calls.
- `ir::Terminator` represents jump, branch, and return.

The current IR is not full SSA yet. Local variables are still represented by
`LoadLocal` and `StoreLocal`. That keeps the first refactor stable, but it is
also the main reason loop-heavy code still has many stack loads/stores. The
next major performance step should be a mem2reg pass that promotes ToyC locals
to SSA-like values and introduces phi nodes.

## Backend Design

The RISC-V target is split into the same conceptual layers as a larger compiler:

- `isel.*`: instruction-selection boundary.
- `riscv_mir.h`: machine IR data structures.
- `frame.*`: stack-frame policy.
- `regalloc.*`: register allocation boundary.
- `asm_printer.*`: RISC-V assembly emission.
- `peephole.*`: final mechanical assembly cleanup.

The first implementation keeps emission conservative: locals and temporaries are
stack allocated, and function calls follow a simple RISC-V calling convention.
The important architectural point is that stack layout, liveness, register
allocation, and printing are now separable units instead of one god file.

The frame layout uses positive offsets from the adjusted `sp`, so callees cannot
overwrite caller locals or temporaries. Outgoing stack arguments are reserved at
the bottom of the caller frame.

## Optimization Safety Rules

1. A pass may not move facts across CFG edges unless it is CFG-aware.
2. `Call`, `StoreGlobal`, and global loads after those side effects must be
   treated conservatively.
3. Short-circuit expressions are lowered as explicit control flow before
   optimization.
4. The verifier runs before and after the pass pipeline.
5. Backend peephole optimizations must be mechanical and local.

## Current Performance Baseline

The architecture refactor favors correctness and maintainability first. The
current conservative backend still emits many `lw`/`sw` instructions in loops.
Local analysis after the refactor showed representative optimized loop bodies
still containing stack traffic, for example:

- `perf_loop_locals opt`: loop body 25 lines, `lw=10`, `sw=8`.
- `perf_combined opt`: loop body 62 lines, `lw=26`, `sw=20`.
- `perf_p08_like opt`: loop body 70 lines, `lw=30`, `sw=23`.

This is expected for the first architectural cut. The old tangled backend had
some registerization tricks, but they lived in the wrong place. The new target
is to reintroduce them through `analysis/liveness.*`, `target/riscv/regalloc.*`,
and a future SSA/mem2reg pass.

## Next Performance Work

Recommended order:

1. Add `mem2reg` and phi support to remove most local `LoadLocal`/`StoreLocal`.
2. Make `dse` remove overwritten local stores after mem2reg or with local
   memory SSA-like reasoning.
3. Teach `regalloc` to assign hot loop values to `s1`-`s11` and have
   `asm_printer` use allocated registers instead of always spilling values.
4. Implement tail-recursion lowering as parameter parallel assignment plus jump
   to entry.
5. Implement small non-recursive function inlining before register allocation.
6. Replace branch-to-jump patterns with direct inverted branches in peephole.
7. Make LICM depend on `cfg`, `dominator`, and `loop_info` instead of pattern
   matching fixed loop shapes.

## Validation

Useful local commands:

```powershell
mingw32-make
powershell -ExecutionPolicy Bypass -File real/tests/run_smoke.ps1
powershell -ExecutionPolicy Bypass -File real/tests/run_backend_compare.ps1
powershell -ExecutionPolicy Bypass -File real/tests/analyze_perf.ps1 -Compiler "..\..\compiler.exe"
```

The compiler interface remains:

```powershell
compiler.exe < input.tc > output.s
compiler.exe -opt < input.tc > output.s
```
