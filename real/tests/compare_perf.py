#!/usr/bin/env python3
"""Compare ToyC compiler performance on shared .tc cases.

The script compiles each ToyC file with the current compiler, optionally with a
reference compiler, runs generated RISC-V assembly in the rv32im simulator, and
prints a CSV-like table with correctness and size/runtime proxy metrics.
"""

from __future__ import annotations

import argparse
import csv
import glob
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path


OPCODES = (
    "lw",
    "sw",
    "call",
    "j",
    "beq",
    "bne",
    "beqz",
    "bnez",
    "mul",
    "mulh",
    "div",
    "rem",
    "mv",
    "li",
    "addi",
)


def run_command(cmd: list[str], *, input_text: str | None = None, timeout: float) -> subprocess.CompletedProcess:
    return subprocess.run(
        cmd,
        input=input_text,
        text=True,
        capture_output=True,
        timeout=timeout,
    )


def gcc_exit(source: str, workdir: Path, timeout: float) -> int | None:
    exe = workdir / "ref.exe"
    result = run_command(
        ["gcc", "-O2", "-x", "c", "-o", str(exe), "-"],
        input_text=source,
        timeout=timeout,
    )
    if result.returncode != 0:
        return None
    run = run_command([str(exe)], timeout=timeout)
    return run.returncode & 0xFF


def compile_toyc(compiler: Path, source: str, *, optimize: bool, timeout: float) -> tuple[str | None, str]:
    cmd = [str(compiler)]
    if optimize:
        cmd.append("-opt")
    try:
        result = run_command(cmd, input_text=source, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, "compile-timeout"
    if result.returncode != 0:
        detail = result.stderr.strip().replace("\n", " ")[:160]
        return None, f"cc-exit={result.returncode}:{detail}"
    return result.stdout, "OK"


def simulate(simulator: Path, asm: str, workdir: Path, max_steps: int, timeout: float) -> tuple[int | None, int | None, str]:
    asm_path = workdir / "out.s"
    asm_path.write_text(asm, encoding="ascii", errors="ignore")
    started = time.monotonic()
    try:
        result = subprocess.run(
            [sys.executable, str(simulator), str(asm_path), str(max_steps)],
            capture_output=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return None, None, "sim-timeout"
    elapsed = time.monotonic() - started
    stderr = result.stderr.decode(errors="replace")
    match = re.search(r"STEPS:(\d+)", stderr)
    steps = int(match.group(1)) if match else None
    if result.returncode == 254:
        detail = stderr.strip().replace("\n", " ")[:160]
        return None, steps, f"sim-error:{detail}"
    return result.returncode & 0xFF, steps, f"OK:{elapsed:.3f}s"


def assembly_metrics(asm: str) -> dict[str, int]:
    lines: list[str] = []
    for raw in asm.splitlines():
        line = raw.split("#", 1)[0].strip()
        if line:
            lines.append(line)
    metrics = {"asm_lines": len(lines)}
    for opcode in OPCODES:
        metrics[opcode] = sum(1 for line in lines if line == opcode or line.startswith(opcode + " ") or line.startswith(opcode + "\t"))
    metrics["mem"] = metrics["lw"] + metrics["sw"]
    return metrics


def collect_cases(cases_dir: Path, patterns: list[str], limit: int | None) -> list[Path]:
    cases: list[Path] = []
    for pattern in patterns:
        matches = sorted(Path(p) for p in glob.glob(str(cases_dir / pattern)))
        if not matches and Path(pattern).exists():
            matches = [Path(pattern)]
        cases.extend(matches)
    unique = sorted(dict.fromkeys(cases))
    return unique[:limit] if limit is not None else unique


def measure_compiler(
    label: str,
    compiler: Path,
    source: str,
    expected: int | None,
    simulator: Path,
    workdir: Path,
    args: argparse.Namespace,
) -> dict[str, object]:
    asm, compile_status = compile_toyc(compiler, source, optimize=args.opt, timeout=args.cc_timeout)
    row: dict[str, object] = {
        "compiler": label,
        "status": compile_status,
        "got": "",
        "ok": "no",
        "steps": "",
    }
    if asm is None:
        return row

    got, steps, sim_status = simulate(simulator, asm, workdir, args.max_steps, args.sim_timeout)
    metrics = assembly_metrics(asm)
    row.update(metrics)
    row["status"] = sim_status
    row["got"] = "" if got is None else got
    row["steps"] = "" if steps is None else steps
    row["ok"] = "yes" if expected is not None and got == expected and sim_status.startswith("OK") else "no"
    return row


def main() -> int:
    repo = Path(__file__).resolve().parents[2]
    default_cases = Path(r"F:\toyc_cpp_compiler\tests\perf")
    default_sim = Path(r"F:\toyc_cpp_compiler\tools\rv32im_sim.py")

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, default=repo / "compiler.exe")
    parser.add_argument("--reference-compiler", type=Path)
    parser.add_argument("--cases-dir", type=Path, default=default_cases)
    parser.add_argument("--simulator", type=Path, default=default_sim)
    parser.add_argument("--pattern", action="append")
    parser.add_argument("--limit", type=int)
    parser.add_argument("--opt", action="store_true", default=True)
    parser.add_argument("--no-opt", dest="opt", action="store_false")
    parser.add_argument("--max-steps", type=int, default=200_000_000)
    parser.add_argument("--cc-timeout", type=float, default=20.0)
    parser.add_argument("--gcc-timeout", type=float, default=20.0)
    parser.add_argument("--sim-timeout", type=float, default=60.0)
    args = parser.parse_args()

    patterns = args.pattern if args.pattern is not None else ["*.tc"]
    cases = collect_cases(args.cases_dir, patterns, args.limit)
    if not cases:
        print(f"no cases found in {args.cases_dir}", file=sys.stderr)
        return 2

    compilers: list[tuple[str, Path]] = [("current", args.compiler)]
    if args.reference_compiler is not None:
        compilers.append(("reference", args.reference_compiler))

    fieldnames = [
        "case",
        "expected",
        "compiler",
        "ok",
        "got",
        "status",
        "steps",
        "asm_lines",
        "mem",
        *OPCODES,
    ]
    writer = csv.DictWriter(sys.stdout, fieldnames=fieldnames, extrasaction="ignore")
    writer.writeheader()

    failures = 0
    with tempfile.TemporaryDirectory() as temp:
        workdir = Path(temp)
        for case in cases:
            source = case.read_text(encoding="utf-8")
            expected = gcc_exit(source, workdir, args.gcc_timeout)
            for label, compiler in compilers:
                row = measure_compiler(label, compiler, source, expected, args.simulator, workdir, args)
                row["case"] = case.name
                row["expected"] = "SKIP" if expected is None else expected
                if expected is None:
                    row["ok"] = "skip"
                writer.writerow(row)
                if row.get("ok") not in ("yes", "skip"):
                    failures += 1
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
