# ToyC Compiler Architecture

## Target Score Strategy

The current grading formula makes performance important after functional correctness is stable.
With 100 functional points and a full report, a performance score around 50 reaches roughly 90 total points; 60+ gives a safer margin.

The architecture therefore favors a stable optimized compiler path over more experimental layers.

## Production Pipeline

```text
Lexer
  -> Parser
  -> AST
  -> SemanticAnalyzer
  -> Safe AST Optimizer
  -> IR Builder
  -> IR Pass Pipeline
  -> IR RISC-V Backend
  -> Assembly
```

The non-optimized path may keep using the old AST backend as a correctness fallback.
The `-opt` path must use IR:

```text
Program ast = parse();
SemanticAnalyzer().analyze(ast);

if (-opt) {
    Program optimized = optimizeAst(ast);
    ir::Module module = IrBuilder().build(optimized);
    buildDefaultPassPipeline(true).run(module);
    IrRiscVCodeGenerator().generate(module, stdout);
} else {
    RiscVCodeGenerator({false}).generate(ast, stdout);
}
```

## Removed Layers

The following layers were removed from the production design:

- Flex/Bison sources: the project uses the hand-written C++ lexer/parser.
- DAG layer: ToyC is small enough for direct AST -> IR lowering.

The deleted DAG path was:

```text
AST -> DAG -> IR
```

The replacement is:

```text
AST -> IR
```

This avoids duplicate IR entry points and keeps optimization work centered on one representation.

## Module Ownership

### Front End

Files:

- `real/src/lexer.*`
- `real/src/parser.*`
- `real/src/ast.h`

Responsibilities:

- Tokenize and parse ToyC into AST.
- Preserve ToyC syntax, expression precedence, and short-circuit structure.
- Do not perform optimization here.

### Semantic Analysis

Files:

- `real/src/semantic.*`

Responsibilities:

- Validate declarations, scopes, function ordering, return paths, `break` / `continue`, and `void` value usage.
- Keep legality checks separate from optimization.

### Safe AST Optimizer

Files:

- `real/src/ast_optimizer.*`

Responsibilities:

- Fold literal and immutable-const expressions.
- Remove unreachable code after terminal statements.
- Remove pure expression statements.
- Simplify safe `if` / `while (0)` cases.

Hard rules:

1. Mutable facts must not cross loop boundaries.
2. Mutable facts must be cleared before optimizing loop conditions.
3. Function calls are effectful unless explicitly proven otherwise.
4. Global stores are observable.
5. Short-circuit side effects must be preserved.

### IR

Files:

- `real/src/ir.*`
- `real/src/ir_builder.*`

Responsibilities:

- Represent functions as basic blocks with explicit terminators.
- Represent computations as values and side-effecting instructions.
- Lower local variables, globals, calls, branches, loops, and returns.

Current IR is intentionally not SSA. Three-address code plus basic blocks is enough for the immediate performance target.

### IR Passes

Files:

- `real/src/pass.*`
- `real/src/pass_simplify.cpp`
- `real/src/pass_cse.cpp`
- `real/src/pass_dce.cpp`

Responsibilities:

- Local constant folding and algebraic simplification.
- Copy propagation.
- Basic-block local common subexpression elimination.
- Dead pure instruction removal.

Side-effect rule:

- Calls, global stores, and local stores are not removed by generic dead-instruction cleanup.
- More aggressive dead-store cleanup must prove that no later load observes the store.

### IR RISC-V Backend

Files:

- `real/src/ir_codegen.*`

Responsibilities:

- Emit RISC-V32 assembly from IR.
- Own stack layout, calls, labels, branches, and register placement.
- Prefer hot locals in saved registers to reduce loop stack traffic.

Backend priorities:

1. Preserve functional correctness.
2. Keep loop variables in registers when possible.
3. Avoid unnecessary `ra` saves in leaf functions.
4. Use immediate instructions such as `addi` and `slti` where safe.

### Legacy AST Backend

Files:

- `real/src/codegen.*`

Responsibilities:

- Remain as non-optimized fallback until the IR backend is fully trusted.
- Do not grow new generic optimizations here.

## Performance Work Plan

### Stage 1: Safe `-opt`

- Route `-opt` through IR.
- Preserve loop exits.
- Add smoke tests for optimized loops.
- Keep functional score at 100.

### Stage 2: IR Optimizations

- Constant propagation.
- Copy propagation.
- Local CSE.
- Dead pure instruction cleanup.
- Algebraic simplification.

These target `p01_const`, `p02_dead_code`, `p03_copy`, `p04_common_subexpr`, `p05_algebra`, `p11_global_const_prop`, and `p12_const_expr_chain`.

### Stage 3: Backend Performance

- Hot local register allocation.
- Leaf function prologue reduction.
- Strength reduction for safe immediate cases.
- Tail recursion lowering.

These target `p06_tail_recursion`, `p07_loop`, `p09_advanced_graph`, and `p10_advanced_matrix`.

## Testing Rules

Every optimization bug must add a regression test before the fix.

Required smoke coverage:

- Minimal return.
- Function call and recursion.
- `while` under `-opt` must contain a conditional exit branch.
- `break` and `continue`.
- Short-circuit side effects.
- Global variable and global const use.
- Semantic error path.
