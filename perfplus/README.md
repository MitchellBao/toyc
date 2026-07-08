# ToyC perfplus cases

`perfplus` contains hidden-like ToyC performance probes for optimization work.
The cases are intentionally more mixed than the public `tests/perf/p*.tc`
samples: they combine constant folding, algebraic reassociation, copy/CSE,
global store/load forwarding, loop closed forms, calls, nested loops, graph and
matrix shapes, and recursive pressure.

Run the passing suite from the repository root:

```powershell
python .\real\tests\compare_perf.py --compiler "E:\Code\Cpp\ToyC_Compiler\compiler.exe" --cases-dir "E:\Code\Cpp\ToyC_Compiler\perfplus" --simulator "E:\Code\Cpp\toyc_cpp_compiler\tools\rv32im_sim.py" --pattern "*.tc" --cc-timeout 30 --sim-timeout 60 --gcc-timeout 20
```

The suite is expected to compile and simulate correctly with the current
compiler. Use the `steps`, `asm_lines`, `mem`, `lw`, `sw`, `mv`, and `call`
columns to track optimization deltas.

`perfplus_known_fail` contains useful probes that currently expose optimizer
wrong-code. Do not include that directory in routine performance gating until
the corresponding bug is fixed.
