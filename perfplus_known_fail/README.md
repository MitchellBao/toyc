# Known failing perfplus probes

These cases are intentionally kept outside `perfplus` because the current
compiler does not yet compile them correctly under `-opt`.

Run them separately when working on the specific optimizer bug:

```powershell
python .\real\tests\compare_perf.py --compiler "E:\Code\Cpp\ToyC_Compiler\compiler.exe" --cases-dir "E:\Code\Cpp\ToyC_Compiler\perfplus_known_fail" --simulator "E:\Code\Cpp\toyc_cpp_compiler\tools\rv32im_sim.py" --pattern "*.tc" --cc-timeout 30 --sim-timeout 60 --gcc-timeout 20
```

Current cases:

- `kf01_dynamic_bound_linear_loop_sum.tc`: dynamic-bound loop-sum currently
  returns `0` instead of the GCC reference result `166`.
