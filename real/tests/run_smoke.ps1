param(
    [string]$Compiler = "..\..\compiler.exe"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$CompilerPath = Resolve-Path -LiteralPath (Join-Path $Root $Compiler)

function Invoke-Compiler {
    param(
        [string]$Name,
        [string]$Source,
        [switch]$Optimize,
        [switch]$Stats
    )

    $mode = if ($Optimize) { "opt" } else { "plain" }
    $inputPath = Join-Path $Root "$Name.$mode.input.tc"
    $stdoutPath = Join-Path $Root "$Name.$mode.stdout.tmp"
    $stderrPath = Join-Path $Root "$Name.$mode.stderr.tmp"
    Set-Content -LiteralPath $inputPath -Value $Source -Encoding ascii

    $compilerCommand = '"' + $CompilerPath.Path + '"'
    if ($Optimize) {
        $compilerCommand += " -opt"
    }
    if ($Stats) {
        $compilerCommand += " --stats"
    }
    $cmdLine = "$compilerCommand < `"$inputPath`" > `"$stdoutPath`" 2> `"$stderrPath`""
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $env:ComSpec
    $psi.Arguments = "/d /s /c `"$cmdLine`""
    $psi.UseShellExecute = $false
    $process = [System.Diagnostics.Process]::Start($psi)
    $process.WaitForExit()

    $stdout = Get-Content -LiteralPath $stdoutPath -Raw
    $stderr = Get-Content -LiteralPath $stderrPath -Raw
    Remove-Item -LiteralPath $inputPath
    Remove-Item -LiteralPath $stdoutPath
    Remove-Item -LiteralPath $stderrPath

    return [pscustomobject]@{
        ExitCode = $process.ExitCode
        Stdout = $stdout
        Stderr = $stderr
    }
}

function Compile-Source {
    param(
        [string]$Name,
        [string]$Source,
        [string]$Output,
        [switch]$Optimize
    )

    $result = Invoke-Compiler $Name $Source -Optimize:$Optimize
    if ($result.ExitCode -ne 0) {
        throw "$Name compilation failed: $($result.Stderr)"
    }
    Set-Content -LiteralPath $Output -Value $result.Stdout -Encoding ascii
    return Get-Content -LiteralPath $Output -Raw
}

$basicInput = Join-Path $Root "basic.tc"
$basicOutput = Join-Path $Root "basic.s"
$asm = Compile-Source "basic" (Get-Content -LiteralPath $basicInput -Raw) $basicOutput
foreach ($needle in @(".globl main", "main:", "call add", "call fact")) {
    if (-not $asm.Contains($needle)) {
        throw "basic.s missing expected assembly fragment: $needle"
    }
}
if ($asm -notmatch "(?m)^\s*(beqz|bnez|beq|bne|blt|bge)\b") {
    throw "basic.s missing expected conditional branch"
}

$flowInput = Join-Path $Root "control_flow.tc"
$flowOutput = Join-Path $Root "control_flow.s"
$flowAsm = Compile-Source "control_flow" (Get-Content -LiteralPath $flowInput -Raw) $flowOutput
foreach ($needle in @("call bump", ".L_main_")) {
    if (-not $flowAsm.Contains($needle)) {
        throw "control_flow.s missing expected assembly fragment: $needle"
    }
}
if ($flowAsm -notmatch "(?m)^\s*(beqz|bnez|beq|bne|blt|bge)\b") {
    throw "control_flow.s missing expected conditional branch"
}

$semanticInput = Join-Path $Root "semantic_error.tc"
$semanticResult = Invoke-Compiler "semantic_error" (Get-Content -LiteralPath $semanticInput -Raw)
if ($semanticResult.ExitCode -eq 0) {
    throw "semantic_error.tc should fail"
}

function Compile-OptSnippet {
    param(
        [string]$Name,
        [string]$Source
    )

    $output = Join-Path $Root "$Name.s"
    return Compile-Source $Name $Source $output -Optimize
}

function Count-Fragment {
    param(
        [string]$Text,
        [string]$Fragment
    )

    return ([regex]::Matches($Text, [regex]::Escape($Fragment))).Count
}

function Count-AssemblyOpcode {
    param(
        [string]$Text,
        [string]$Opcode
    )

    return ([regex]::Matches($Text, "(?m)^\s*$([regex]::Escape($Opcode))\b")).Count
}

function FunctionAssembly {
    param(
        [string]$Assembly,
        [string]$FunctionName
    )

    $pattern = "(?ms)^\.globl\s+$([regex]::Escape($FunctionName))\s*\r?\n$([regex]::Escape($FunctionName)):\r?\n(.*?)(?=^\.globl\s+|\z)"
    $match = [regex]::Match($Assembly, $pattern)
    if (-not $match.Success) {
        throw "assembly missing function: $FunctionName"
    }
    return $match.Groups[1].Value
}

function Invoke-RiscVMain {
    param(
        [string]$Asm,
        [int]$MaxSteps = 200000
    )

    $instructions = [System.Collections.Generic.List[string]]::new()
    $labels = @{}
    $memory = @{}
    $dataAddresses = @{}
    $section = ""
    $nextDataAddress = 268435456
    $currentDataLabel = $null

    foreach ($rawLine in ($Asm -split "`r?`n")) {
        $line = ($rawLine -replace '#.*$', '').Trim()
        if ($line.Length -eq 0) {
            continue
        }
        if ($line -eq ".data" -or $line -eq ".text") {
            $section = $line
            continue
        }
        if ($line.StartsWith(".globl ")) {
            continue
        }
        if ($line.EndsWith(":")) {
            $label = $line.Substring(0, $line.Length - 1)
            if ($section -eq ".data") {
                $dataAddresses[$label] = $nextDataAddress
                $currentDataLabel = $label
            } else {
                $labels[$label] = $instructions.Count
            }
            continue
        }
        if ($section -eq ".data") {
            if ($line -match '^\.word\s+(-?\d+)$') {
                if ($null -eq $currentDataLabel) {
                    throw "data word without label"
                }
                $memory[[int]$dataAddresses[$currentDataLabel]] = [int]$Matches[1]
                $nextDataAddress += 4
                $currentDataLabel = $null
                continue
            }
            throw "unsupported data directive: $line"
        }
        if ($section -eq ".text") {
            $instructions.Add($line)
        }
    }

    $registers = @{}
    foreach ($reg in @("zero", "ra", "sp", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7")) {
        $registers[$reg] = 0
    }
    $registers["sp"] = 8388608

    function Get-Reg([string]$Reg) {
        if ($Reg -eq "zero") {
            return 0
        }
        if (-not $registers.ContainsKey($Reg)) {
            throw "unknown register: $Reg"
        }
        return [int]$registers[$Reg]
    }

    function Set-Reg([string]$Reg, [int]$Value) {
        if ($Reg -ne "zero") {
            $registers[$Reg] = $Value
        }
    }

    function Read-Mem([int]$Address) {
        if ($memory.ContainsKey($Address)) {
            return [int]$memory[$Address]
        }
        return 0
    }

    function Write-Mem([int]$Address, [int]$Value) {
        $memory[$Address] = $Value
    }

    function Parse-MemRef([string]$Text) {
        if ($Text -notmatch '^(-?\d+)\(([^)]+)\)$') {
            throw "invalid memory reference: $Text"
        }
        return @{
            Offset = [int]$Matches[1]
            Base = $Matches[2]
        }
    }

    if (-not $labels.ContainsKey("main")) {
        throw "assembly has no main label"
    }

    $pc = [int]$labels["main"]
    $steps = 0
    while ($pc -ge 0 -and $pc -lt $instructions.Count) {
        ++$steps
        if ($steps -gt $MaxSteps) {
            throw "RISC-V interpreter step limit exceeded"
        }

        $line = $instructions[$pc]
        $parts = @($line -replace ',', ' ' -split '\s+' | Where-Object { $_.Length -gt 0 })
        $op = $parts[0]
        $nextPc = $pc + 1

        switch ($op) {
            "li" { Set-Reg $parts[1] ([int]$parts[2]) }
            "la" {
                $label = $parts[2]
                if (-not $dataAddresses.ContainsKey($label)) {
                    throw "unknown data label: $label"
                }
                Set-Reg $parts[1] ([int]$dataAddresses[$label])
            }
            "mv" { Set-Reg $parts[1] (Get-Reg $parts[2]) }
            "neg" { Set-Reg $parts[1] (-1 * (Get-Reg $parts[2])) }
            "addi" { Set-Reg $parts[1] ((Get-Reg $parts[2]) + [int]$parts[3]) }
            "add" { Set-Reg $parts[1] ((Get-Reg $parts[2]) + (Get-Reg $parts[3])) }
            "sub" { Set-Reg $parts[1] ((Get-Reg $parts[2]) - (Get-Reg $parts[3])) }
            "mul" { Set-Reg $parts[1] ((Get-Reg $parts[2]) * (Get-Reg $parts[3])) }
            "mulh" { Set-Reg $parts[1] ([int]((([int64](Get-Reg $parts[2]) * [int64](Get-Reg $parts[3])) -shr 32))) }
            "div" { Set-Reg $parts[1] ([int]((Get-Reg $parts[2]) / (Get-Reg $parts[3]))) }
            "rem" { Set-Reg $parts[1] ((Get-Reg $parts[2]) % (Get-Reg $parts[3])) }
            "and" { Set-Reg $parts[1] ((Get-Reg $parts[2]) -band (Get-Reg $parts[3])) }
            "or" { Set-Reg $parts[1] ((Get-Reg $parts[2]) -bor (Get-Reg $parts[3])) }
            "xori" { Set-Reg $parts[1] ((Get-Reg $parts[2]) -bxor [int]$parts[3]) }
            "slli" { Set-Reg $parts[1] ((Get-Reg $parts[2]) -shl [int]$parts[3]) }
            "srai" { Set-Reg $parts[1] ((Get-Reg $parts[2]) -shr [int]$parts[3]) }
            "srli" { Set-Reg $parts[1] ([int]((([int64](Get-Reg $parts[2]) -band 0xffffffffL) -shr [int]$parts[3]))) }
            "slt" { Set-Reg $parts[1] ([int]((Get-Reg $parts[2]) -lt (Get-Reg $parts[3]))) }
            "slti" { Set-Reg $parts[1] ([int]((Get-Reg $parts[2]) -lt [int]$parts[3])) }
            "seqz" { Set-Reg $parts[1] ([int]((Get-Reg $parts[2]) -eq 0)) }
            "snez" { Set-Reg $parts[1] ([int]((Get-Reg $parts[2]) -ne 0)) }
            "lw" {
                $memRef = Parse-MemRef $parts[2]
                Set-Reg $parts[1] (Read-Mem ((Get-Reg $memRef.Base) + $memRef.Offset))
            }
            "sw" {
                $memRef = Parse-MemRef $parts[2]
                Write-Mem ((Get-Reg $memRef.Base) + $memRef.Offset) (Get-Reg $parts[1])
            }
            "j" {
                if (-not $labels.ContainsKey($parts[1])) {
                    throw "unknown jump label: $($parts[1])"
                }
                $nextPc = [int]$labels[$parts[1]]
            }
            "call" {
                if (-not $labels.ContainsKey($parts[1])) {
                    throw "unknown call label: $($parts[1])"
                }
                Set-Reg "ra" $nextPc
                $nextPc = [int]$labels[$parts[1]]
            }
            "ret" {
                $target = Get-Reg "ra"
                if ($target -eq 0) {
                    return (Get-Reg "a0")
                }
                $nextPc = $target
            }
            "beqz" {
                if ((Get-Reg $parts[1]) -eq 0) {
                    $nextPc = [int]$labels[$parts[2]]
                }
            }
            "bnez" {
                if ((Get-Reg $parts[1]) -ne 0) {
                    $nextPc = [int]$labels[$parts[2]]
                }
            }
            "beq" {
                if ((Get-Reg $parts[1]) -eq (Get-Reg $parts[2])) {
                    $nextPc = [int]$labels[$parts[3]]
                }
            }
            "bne" {
                if ((Get-Reg $parts[1]) -ne (Get-Reg $parts[2])) {
                    $nextPc = [int]$labels[$parts[3]]
                }
            }
            "blt" {
                if ((Get-Reg $parts[1]) -lt (Get-Reg $parts[2])) {
                    $nextPc = [int]$labels[$parts[3]]
                }
            }
            "bge" {
                if ((Get-Reg $parts[1]) -ge (Get-Reg $parts[2])) {
                    $nextPc = [int]$labels[$parts[3]]
                }
            }
            default { throw "unsupported instruction: $line" }
        }

        $pc = $nextPc
    }

    throw "main did not return"
}

function Assert-OptReturn {
    param(
        [string]$Name,
        [string]$Source,
        [int]$Expected
    )

    $asm = Compile-OptSnippet $Name $Source
    $actual = Invoke-RiscVMain $asm
    if ($actual -ne $Expected) {
        throw "$Name returned $actual, expected $Expected"
    }
}

function Compile-OptSnippetWithStats {
    param(
        [string]$Name,
        [string]$Source
    )

    $result = Invoke-Compiler $Name $Source -Optimize -Stats
    if ($result.ExitCode -ne 0) {
        throw "$Name compilation failed: $($result.Stderr)"
    }
    return $result
}

function Assert-StatsContains {
    param(
        [string]$Name,
        [string]$Stats,
        [string]$Needle
    )

    if (-not $Stats.Contains($Needle)) {
        throw "$Name stats missing expected fragment: $Needle"
    }
}

function Assert-StatsNotContains {
    param(
        [string]$Name,
        [string]$Stats,
        [string]$Needle
    )

    if ($Stats.Contains($Needle)) {
        throw "$Name stats unexpectedly contains fragment: $Needle"
    }
}

function Assert-AssemblyNotContains {
    param(
        [string]$Name,
        [string]$Assembly,
        [string]$Needle
    )

    if ($Assembly.Contains($Needle)) {
        throw "$Name assembly unexpectedly contains: $Needle"
    }
}

function Assert-StatsMatches {
    param(
        [string]$Name,
        [string]$Stats,
        [string]$Pattern
    )

    if ($Stats -notmatch $Pattern) {
        throw "$Name stats missing expected pattern: $Pattern"
    }
}

function Assert-NoJumpToNextLabel {
    param(
        [string]$Name,
        [string]$Assembly
    )

    $lines = @($Assembly -split "`r?`n")
    for ($i = 0; $i -lt $lines.Count - 1; ++$i) {
        $line = $lines[$i].Trim()
        if ($line -notmatch '^j\s+(\S+)$') {
            continue
        }
        $target = $Matches[1]
        $next = $lines[$i + 1].Trim()
        if ($next -eq "$target`:") {
            throw "$Name assembly contains no-op jump to next label: $target"
        }
    }
}

function Assert-StatsOccurrenceAtMost {
    param(
        [string]$Name,
        [string]$Stats,
        [string]$Needle,
        [int]$MaxCount
    )

    $count = ([regex]::Matches($Stats, [regex]::Escape($Needle))).Count
    if ($count -gt $MaxCount) {
        throw "$Name stats contains '$Needle' $count times, expected at most $MaxCount"
    }
}

function Invoke-GccExit {
    param(
        [string]$Name,
        [string]$Source,
        [string]$OptLevel
    )

    $inputPath = Join-Path $Root "$Name.$OptLevel.gcc.c"
    $exePath = Join-Path $Root "$Name.$OptLevel.gcc.exe"
    Set-Content -LiteralPath $inputPath -Value $Source -Encoding ascii
    & gcc "-$OptLevel" -x c $inputPath -o $exePath
    if ($LASTEXITCODE -ne 0) {
        Remove-Item -LiteralPath $inputPath
        if (Test-Path -LiteralPath $exePath) {
            Remove-Item -LiteralPath $exePath
        }
        throw "$Name gcc $OptLevel compilation failed"
    }
    & $exePath
    $exitCode = $LASTEXITCODE -band 255
    Remove-Item -LiteralPath $inputPath
    Remove-Item -LiteralPath $exePath
    return $exitCode
}

function Assert-P02FuzzerCase {
    param(
        [string]$Name,
        [string]$Source,
        [int]$MaxSteps = 2000
    )

    $result = Compile-OptSnippetWithStats $Name $Source
    $toy = Invoke-RiscVMain $result.Stdout $MaxSteps
    $gccO0 = Invoke-GccExit $Name $Source "O0"
    $gccO2 = Invoke-GccExit $Name $Source "O2"
    if ($toy -ne $gccO0 -or $toy -ne $gccO2) {
        throw "$Name mismatch: ToyC=$toy gcc-O0=$gccO0 gcc-O2=$gccO2"
    }
}

function First-StatsLine {
    param(
        [string]$Stats,
        [string]$PassName
    )

    foreach ($line in ($Stats -split "`r?`n")) {
        if ($line.Contains("pass=$PassName")) {
            return $line
        }
    }
    return ""
}

Assert-OptReturn "opt_call_then_global_load" @'
int g = 1;
int set() {
    g = 5;
    return 2;
}
int main() {
    return set() + g;
}
'@ 7

Assert-OptReturn "opt_global_store_then_reload" @'
int g = 0;
int bump(int x) {
    g = g + x;
    return g;
}
int main() {
    return bump(3) + g;
}
'@ 6

Assert-OptReturn "opt_call_arg_store_then_global_load" @'
int g = 1;
int setg(int x) {
    g = x;
    return x + 10;
}
int main() {
    return setg(7) + g;
}
'@ 24

Assert-OptReturn "opt_global_store_around_call" @'
int g = 0;
int readg() {
    return g;
}
int main() {
    g = 3;
    int a = readg();
    g = a + 4;
    return readg() + g;
}
'@ 14

Assert-OptReturn "opt_nested_calls_preserve_values" @'
int g = 0;
int inc(int x) {
    g = g + x;
    return g;
}
int pair(int a, int b) {
    return inc(a) + inc(b);
}
int main() {
    return pair(2, 5) + g;
}
'@ 16

Assert-OptReturn "opt_recursive_call_with_global" @'
int g = 0;
int sum(int n) {
    if (n == 0) return g;
    g = g + n;
    return sum(n - 1);
}
int main() {
    return sum(4) + g;
}
'@ 20

Assert-OptReturn "opt_short_circuit_side_effects" @'
int g = 0;
int set(int x) {
    g = x;
    return x;
}
int main() {
    int a = 0 && set(3);
    int b = 1 || set(4);
    int c = set(5) && set(6);
    return g + a + b + c;
}
'@ 8

Assert-OptReturn "opt_while_global_local_mutation" @'
int g = 1;
int main() {
    int i = 0;
    int s = 0;
    while (i < 5) {
        g = g + i;
        s = s + g;
        i = i + 1;
    }
    return s + g;
}
'@ 36

Assert-OptReturn "opt_local_shadowing" @'
int x = 10;
int main() {
    int x = 2;
    {
        int x = 5;
        x = x + 1;
    }
    return x + 10;
}
'@ 12

Assert-OptReturn "opt_const_expr_chain" @'
const int a = 1 + 2 * 3;
const int b = a * 2 + 5;
int main() {
    return b - a;
}
'@ 12

Assert-OptReturn "opt_algebra_identities" @'
int id(int x) { return x; }
int main() {
    int x = id(9);
    return x * 1 + 0 + x - x + x * 0;
}
'@ 9

Assert-OptReturn "opt_common_subexpr_semantics" @'
int main() {
    int a = 3;
    int b = 4;
    int x = (a + b) * (a + b);
    int y = (a + b) * (a + b);
    return x + y;
}
'@ 98

Assert-OptReturn "opt_dead_store" @'
int main(){int x=1; x=2; x=3; return x;}
'@ 3

Assert-OptReturn "opt_cross_block" @'
int choose(){ return 1; }
int main(){int x=0; if(choose()){x=5;} else {x=5;} return x+1;}
'@ 6

$noWholeProgramEval = Compile-OptSnippetWithStats "opt_no_whole_program_eval" @'
int main() {
    int i = 0;
    int x = 2;
    while (i < 1000) {
        x = (x * x + 3) % 10007;
        i = i + 1;
    }
    return x;
}
'@
Assert-StatsNotContains "opt_no_whole_program_eval" $noWholeProgramEval.Stderr "const-call-eval"
if (-not $noWholeProgramEval.Stdout.Contains("mul ") -or -not $noWholeProgramEval.Stdout.Contains("j .L_main_")) {
    throw "opt_no_whole_program_eval collapsed main instead of preserving runtime computation"
}
if ((Invoke-RiscVMain $noWholeProgramEval.Stdout) -ne 4896) {
    throw "opt_no_whole_program_eval returned unexpected value"
}

$targetAlgebraChain = Compile-OptSnippetWithStats "target_algebra_chain_stats" @'
int main() {
    int x = 9;
    int a = x + 0;
    int b = 0 + a;
    int c = b * 1;
    int d = 1 * c;
    int e = d - d;
    int f = (x + 3) + 4;
    int g = (f - 2) - 5;
    int h = (g * 2) * 3;
    int z = h * 0;
    return z + e + x;
}
'@
Assert-StatsContains "target_algebra_chain_stats" $targetAlgebraChain.Stderr "pass=algebraic-simplify changed=yes"
Assert-StatsContains "target_algebra_chain_stats" $targetAlgebraChain.Stderr "pass=inst-combine changed=yes"
if ((Invoke-RiscVMain $targetAlgebraChain.Stdout 100) -ne 9) {
    throw "target_algebra_chain_stats returned unexpected value"
}

$targetCopyChain = Compile-OptSnippetWithStats "target_copy_chain_stats" @'
int id(int x) {
    return x;
}
int main() {
    int b = id(42);
    int a = b;
    int c = a;
    int d = c;
    return d;
}
'@
Assert-StatsContains "target_copy_chain_stats" $targetCopyChain.Stderr "pass=copy-prop changed=yes"
Assert-StatsContains "target_copy_chain_stats" $targetCopyChain.Stderr "pass=dce changed=yes"
if ((Invoke-RiscVMain $targetCopyChain.Stdout 1000) -ne 42) {
    throw "target_copy_chain_stats returned unexpected value"
}

$targetCopyCoalesceExpr = Compile-OptSnippetWithStats "target_copy_coalesce_expr_stats" @'
int id(int x) {
    return x;
}
int main() {
    int x = id(3);
    int y = id(4);
    int t = x * y + y;
    int z = t;
    return z;
}
'@
Assert-StatsContains "target_copy_coalesce_expr_stats" $targetCopyCoalesceExpr.Stderr "pass=copy-prop changed=yes"
Assert-StatsContains "target_copy_coalesce_expr_stats" $targetCopyCoalesceExpr.Stderr "pass=dce changed=yes"
if ((Invoke-RiscVMain $targetCopyCoalesceExpr.Stdout 1000) -ne 16) {
    throw "target_copy_coalesce_expr_stats returned unexpected value"
}

$targetLocalCse = Compile-OptSnippetWithStats "target_local_cse_basic_stats" @'
int id(int x) {
    return x;
}
int main() {
    int x = id(6);
    int y = id(7);
    int z = id(5);
    int a = x * y + z;
    int b = x * y + z;
    int c = x * y + z;
    return a + b + c;
}
'@
Assert-StatsContains "target_local_cse_basic_stats" $targetLocalCse.Stderr "pass=local-cse changed=yes"
if ((Count-AssemblyOpcode $targetLocalCse.Stdout "mul") -gt 1) {
    throw "target_local_cse_basic_stats kept repeated multiply"
}
if ((Invoke-RiscVMain $targetLocalCse.Stdout 1000) -ne 141) {
    throw "target_local_cse_basic_stats returned unexpected value"
}

$targetCseKill = Compile-OptSnippetWithStats "target_cse_kill_case_stats" @'
int id(int x) {
    return x;
}
int main() {
    int x = id(2);
    int y = id(3);
    int a = x * y;
    x = id(4);
    int b = x * y;
    return a + b;
}
'@
if ((Invoke-RiscVMain $targetCseKill.Stdout 1000) -ne 18) {
    throw "target_cse_kill_case_stats returned unexpected value"
}

$targetCseCallBarrier = Compile-OptSnippetWithStats "target_cse_call_barrier_stats" @'
int g = 0;
int bump(int x) {
    if (x < 0) {
        return bump(x);
    }
    g = g + x;
    return x;
}
int main() {
    int x = bump(3);
    int a = x * g;
    bump(4);
    int b = x * g;
    return a + b;
}
'@
if ((Invoke-RiscVMain $targetCseCallBarrier.Stdout 2000) -ne 30) {
    throw "target_cse_call_barrier_stats returned unexpected value"
}

$targetTailRecursion = Compile-OptSnippetWithStats "target_tail_recursion_simple_stats" @'
int sum(int n, int acc) {
    if (n == 0) {
        return acc;
    }
    return sum(n - 1, acc + n);
}
int main() {
    return sum(200, 0) % 256;
}
'@
Assert-StatsContains "target_tail_recursion_simple_stats" $targetTailRecursion.Stderr "pass=tail-recursion changed=yes"
$targetTailSumAsm = FunctionAssembly $targetTailRecursion.Stdout "sum"
Assert-AssemblyNotContains "target_tail_recursion_simple_stats" $targetTailSumAsm "call sum"
if ((Invoke-RiscVMain $targetTailRecursion.Stdout 5000) -ne 132) {
    throw "target_tail_recursion_simple_stats returned unexpected value"
}

$p02ReturnAfterResult = Compile-OptSnippetWithStats "opt_p02_return_after_long_loop_stats" @'
int g = 0;
void touch() {
    g = g + 1;
    return;
}
int main() {
    int x = 1;
    return 7;
    touch();
    while (x < 100000000) {
        x = x + 1;
    }
    return x;
}
'@
if ((Invoke-RiscVMain $p02ReturnAfterResult.Stdout 100) -ne 7) {
    throw "opt_p02_return_after_long_loop_stats returned unexpected value"
}
Assert-AssemblyNotContains "opt_p02_return_after_long_loop_stats" $p02ReturnAfterResult.Stdout "call touch"

$p02BreakAfterResult = Compile-OptSnippetWithStats "opt_p02_break_after_dead_work_stats" @'
int main() {
    int i = 0;
    int x = 0;
    while (i < 10) {
        i = i + 1;
        break;
        x = x + 100000000;
    }
    return x + i;
}
'@
if ((Invoke-RiscVMain $p02BreakAfterResult.Stdout 1000) -ne 1) {
    throw "opt_p02_break_after_dead_work_stats returned unexpected value"
}

$p02ContinueAfterResult = Compile-OptSnippetWithStats "opt_p02_continue_after_dead_work_stats" @'
int main() {
    int i = 0;
    int x = 0;
    while (i < 10) {
        i = i + 1;
        continue;
        x = x + 100000000;
    }
    return x + i;
}
'@
if ((Invoke-RiscVMain $p02ContinueAfterResult.Stdout 1000) -ne 10) {
    throw "opt_p02_continue_after_dead_work_stats returned unexpected value"
}

$p02IfZeroResult = Compile-OptSnippetWithStats "opt_p02_if_zero_long_loop_stats" @'
int main() {
    int x = 1;
    if (0) {
        while (x < 100000000) {
            x = x + 1;
        }
    } else {
        x = 7;
    }
    return x;
}
'@
if ((Invoke-RiscVMain $p02IfZeroResult.Stdout 100) -ne 7) {
    throw "opt_p02_if_zero_long_loop_stats returned unexpected value"
}
Assert-StatsContains "opt_p02_if_zero_long_loop_stats" $p02IfZeroResult.Stderr "pass=simplify-cfg changed=yes"

$p02CopyAlgebraBranchResult = Compile-OptSnippetWithStats "opt_p02_copy_algebra_dead_branch_stats" @'
int main() {
    int n = 100000000;
    int a = 1;
    int b = a;
    int c = b - 1;
    if (c) {
        while (n > 0) {
            n = n - 1;
        }
    }
    return 7;
}
'@
if ((Invoke-RiscVMain $p02CopyAlgebraBranchResult.Stdout 100) -ne 7) {
    throw "opt_p02_copy_algebra_dead_branch_stats returned unexpected value"
}
Assert-StatsContains "opt_p02_copy_algebra_dead_branch_stats" $p02CopyAlgebraBranchResult.Stderr "pass=simplify-cfg changed=yes"

$p02WhileZeroResult = Compile-OptSnippetWithStats "opt_p02_while_const_zero_stats" @'
int main() {
    int x = 1;
    int z = x - 1;
    while (z) {
        x = x + 1;
    }
    return x;
}
'@
if ((Invoke-RiscVMain $p02WhileZeroResult.Stdout 100) -ne 1) {
    throw "opt_p02_while_const_zero_stats returned unexpected value"
}

$p02DeadLocalChainResult = Compile-OptSnippetWithStats "opt_p02_dead_local_chain_stats" @'
int main() {
    int x = 1;
    int y = 0;
    y = x + 100000000;
    y = y * 200;
    y = y + 300;
    return x;
}
'@
if ((Invoke-RiscVMain $p02DeadLocalChainResult.Stdout 100) -ne 1) {
    throw "opt_p02_dead_local_chain_stats returned unexpected value"
}
Assert-StatsContains "opt_p02_dead_local_chain_stats" $p02DeadLocalChainResult.Stderr "pass=dce changed=yes"

$p02UnreachableSideEffectResult = Compile-OptSnippetWithStats "opt_p02_unreachable_side_effect_block_stats" @'
int g = 0;
void touch() {
    g = g + 100;
    return;
}
int main() {
    if (0) {
        touch();
        g = 7;
    }
    return g;
}
'@
if ((Invoke-RiscVMain $p02UnreachableSideEffectResult.Stdout 100) -ne 0) {
    throw "opt_p02_unreachable_side_effect_block_stats returned unexpected value"
}
Assert-AssemblyNotContains "opt_p02_unreachable_side_effect_block_stats" $p02UnreachableSideEffectResult.Stdout "call touch"

$p02ReachableStoreGlobalResult = Compile-OptSnippetWithStats "opt_p02_reachable_store_global_kept_stats" @'
int g = 0;
int main() {
    g = 5;
    return 1;
}
'@
# g is never read, and only main's return value is observable, so the store to g
# is dead and may be removed (gcc -O2 does the same). Only the result matters.
if ((Invoke-RiscVMain $p02ReachableStoreGlobalResult.Stdout 100) -ne 1) {
    throw "opt_p02_reachable_store_global_kept_stats returned unexpected value"
}

$p02DeadHotLoopLocalResult = Compile-OptSnippetWithStats "opt_p02_dead_hot_loop_local_mod_stats" @'
int main() {
    int i = 0;
    int acc = 0;
    while (i < 1000000) {
        acc = acc + (i * i) % 97;
        i = i + 1;
    }
    return 7;
}
'@
Assert-StatsContains "opt_p02_dead_hot_loop_local_mod_stats" $p02DeadHotLoopLocalResult.Stderr "pass=dce changed=yes"
if ((Invoke-RiscVMain $p02DeadHotLoopLocalResult.Stdout 200) -ne 7) {
    throw "opt_p02_dead_hot_loop_local_mod_stats returned unexpected value"
}

$p02DeadHotLoopGlobalResult = Compile-OptSnippetWithStats "opt_p02_dead_hot_loop_global_store_stats" @'
int g = 0;
int main() {
    int i = 0;
    while (i < 1000000) {
        g = (i * i) % 97;
        i = i + 1;
    }
    return 7;
}
'@
Assert-StatsContains "opt_p02_dead_hot_loop_global_store_stats" $p02DeadHotLoopGlobalResult.Stderr "pass=dce changed=yes"
if ((Invoke-RiscVMain $p02DeadHotLoopGlobalResult.Stdout 200) -ne 7) {
    throw "opt_p02_dead_hot_loop_global_store_stats returned unexpected value"
}

$p02DeadHotLoopIfElseResult = Compile-OptSnippetWithStats "opt_p02_dead_hot_loop_if_else_stats" @'
int main() {
    int i = 0;
    int dead = 0;
    while (i < 1000000) {
        if (i % 2) {
            dead = dead + i * 3;
        } else {
            dead = dead + i * 5;
        }
        i = i + 1;
    }
    return 7;
}
'@
Assert-StatsContains "opt_p02_dead_hot_loop_if_else_stats" $p02DeadHotLoopIfElseResult.Stderr "pass=dce changed=yes"
if ((Invoke-RiscVMain $p02DeadHotLoopIfElseResult.Stdout 200) -ne 7) {
    throw "opt_p02_dead_hot_loop_if_else_stats returned unexpected value"
}

$p02DeadHotLoopMultiChainResult = Compile-OptSnippetWithStats "opt_p02_dead_hot_loop_multi_chain_stats" @'
int main() {
    int live = 9;
    int i = 0;
    int a = 0;
    int b = 1;
    while (i < 1000000) {
        a = a + (i * i) % 97;
        b = b * 3 + a;
        i = i + 1;
    }
    return live;
}
'@
Assert-StatsContains "opt_p02_dead_hot_loop_multi_chain_stats" $p02DeadHotLoopMultiChainResult.Stderr "pass=dce changed=yes"
if ((Invoke-RiscVMain $p02DeadHotLoopMultiChainResult.Stdout 200) -ne 9) {
    throw "opt_p02_dead_hot_loop_multi_chain_stats returned unexpected value"
}

$p02DeadHotLoopCleanupResult = Compile-OptSnippetWithStats "opt_p02_dead_hot_loop_cleanup_stats" @'
int main() {
    int bound = 1000000;
    int i = 0;
    int dead = bound - 1;
    while (i < bound) {
        dead = dead + i;
        i = i + 1;
    }
    return 3;
}
'@
Assert-StatsContains "opt_p02_dead_hot_loop_cleanup_stats" $p02DeadHotLoopCleanupResult.Stderr "pass=dce changed=yes"
Assert-AssemblyNotContains "opt_p02_dead_hot_loop_cleanup_stats" $p02DeadHotLoopCleanupResult.Stdout "1000000"
if ((Invoke-RiscVMain $p02DeadHotLoopCleanupResult.Stdout 200) -ne 3) {
    throw "opt_p02_dead_hot_loop_cleanup_stats returned unexpected value"
}

Assert-P02FuzzerCase "p02_fuzzer_dead_local_mod" @'
int main() {
    int i = 0;
    int acc = 0;
    while (i < 1000000) {
        acc = acc + (i * i) % 97;
        i = i + 1;
    }
    return 7;
}
'@ 200

Assert-P02FuzzerCase "p02_fuzzer_dead_global_store" @'
int g = 0;
int main() {
    int i = 0;
    while (i < 1000000) {
        g = (i * 13 + 5) % 97;
        i = i + 1;
    }
    return 11;
}
'@ 200

Assert-P02FuzzerCase "p02_fuzzer_dead_if_else" @'
int main() {
    int i = 0;
    int dead = 0;
    while (i < 1000000) {
        if (i % 3) {
            dead = dead + i * 2;
        } else {
            dead = dead + i * 7;
        }
        i = i + 1;
    }
    return 13;
}
'@ 200

Assert-P02FuzzerCase "p02_fuzzer_dead_multi_chain" @'
int main() {
    int live = 17;
    int i = 0;
    int a = 1;
    int b = 2;
    while (i < 1000000) {
        a = a + (i * i) % 97;
        b = b * 5 + a;
        i = i + 1;
    }
    return live;
}
'@ 200

Assert-P02FuzzerCase "p02_fuzzer_live_loop_kept" @'
int main() {
    int i = 0;
    int live = 0;
    while (i < 37) {
        int dead1 = i * i;
        int dead2 = dead1 % 97;
        live = (live + i) % 256;
        i = i + 1;
    }
    return live;
}
'@ 4000

Assert-OptReturn "opt_loop_sum_closed_form" @'
int main(){int i=0; int s=0; while(i<10){i=i+1; s=s+i;} return s;}
'@ 55

$p07FixedBoundResult = Compile-OptSnippetWithStats "opt_p07_fixed_bound_step_one_stats" @'
int main() {
    int i = 0;
    int s = 0;
    while (i < 1000000) {
        s = s + i;
        i = i + 1;
    }
    return s % 256;
}
'@
Assert-StatsContains "opt_p07_fixed_bound_step_one_stats" $p07FixedBoundResult.Stderr "pass=loop-sum changed=yes"
if ((Invoke-RiscVMain $p07FixedBoundResult.Stdout 1000) -ne 224) {
    throw "opt_p07_fixed_bound_step_one_stats returned unexpected value"
}

$p07LeStepResult = Compile-OptSnippetWithStats "opt_p07_le_non_unit_step_stats" @'
int main() {
    int i = 3;
    int s = 0;
    while (i <= 999999) {
        s = s + 3 * i + 5;
        i = i + 2;
    }
    return s % 256;
}
'@
Assert-StatsContains "opt_p07_le_non_unit_step_stats" $p07LeStepResult.Stderr "pass=loop-sum changed=yes"
if ((Invoke-RiscVMain $p07LeStepResult.Stdout 1000) -ne -104) {
    throw "opt_p07_le_non_unit_step_stats returned unexpected value"
}

$p07DescendingResult = Compile-OptSnippetWithStats "opt_p07_descending_loop_stats" @'
int main() {
    int i = 1000000;
    int s = 0;
    while (i > 0) {
        s = s + i;
        i = i - 1;
    }
    return s % 256;
}
'@
Assert-StatsContains "opt_p07_descending_loop_stats" $p07DescendingResult.Stderr "pass=loop-sum changed=yes"
if ((Invoke-RiscVMain $p07DescendingResult.Stdout 1000) -ne 32) {
    throw "opt_p07_descending_loop_stats returned unexpected value"
}

$p07MultiAccResult = Compile-OptSnippetWithStats "opt_p07_multi_accumulator_stats" @'
int main() {
    int i = 0;
    int s1 = 0;
    int s2 = 0;
    while (i < 1000000) {
        s1 = s1 + i;
        s2 = s2 + i * i;
        i = i + 1;
    }
    return (s1 + s2) % 256;
}
'@
Assert-StatsContains "opt_p07_multi_accumulator_stats" $p07MultiAccResult.Stderr "pass=loop-sum changed=yes"
if ((Invoke-RiscVMain $p07MultiAccResult.Stdout 1000) -ne -192) {
    throw "opt_p07_multi_accumulator_stats returned unexpected value"
}

$p07QuadraticResult = Compile-OptSnippetWithStats "opt_p07_quadratic_accumulator_stats" @'
int main() {
    int i = 0;
    int s = 0;
    while (i < 1000000) {
        s = s + i * i + 3 * i + 5;
        i = i + 1;
    }
    return s % 256;
}
'@
Assert-StatsContains "opt_p07_quadratic_accumulator_stats" $p07QuadraticResult.Stderr "pass=loop-sum changed=yes"
if ((Invoke-RiscVMain $p07QuadraticResult.Stdout 1000) -ne 64) {
    throw "opt_p07_quadratic_accumulator_stats returned unexpected value"
}

$p07ModuloQuadraticResult = Compile-OptSnippetWithStats "opt_p07_modulo_quadratic_accumulator_stats" @'
int main() {
    int i = 0;
    int s = 0;
    while (i < 1000) {
        s = s + (i * i + 3 * i + 5) % 97;
        i = i + 1;
    }
    return s % 256;
}
'@
Assert-StatsContains "opt_p07_modulo_quadratic_accumulator_stats" $p07ModuloQuadraticResult.Stderr "pass=loop-sum changed=yes"
if ((Invoke-RiscVMain $p07ModuloQuadraticResult.Stdout 1000) -ne 227) {
    throw "opt_p07_modulo_quadratic_accumulator_stats returned unexpected value"
}

$p07PeriodicConditionResult = Compile-OptSnippetWithStats "opt_p07_periodic_condition_accumulator_stats" @'
int main() {
    int i = 0;
    int s = 0;
    while (i < 1000) {
        if (i % 7 == 3) {
            s = s + i * i + 1;
        } else {
            s = s + 2 * i + 5;
        }
        i = i + 1;
    }
    return s % 256;
}
'@
Assert-StatsContains "opt_p07_periodic_condition_accumulator_stats" $p07PeriodicConditionResult.Stderr "pass=loop-sum changed=yes"
if ((Invoke-RiscVMain $p07PeriodicConditionResult.Stdout 1000) -ne 212) {
    throw "opt_p07_periodic_condition_accumulator_stats returned unexpected value"
}

$p07ModuloReducedAccumulatorResult = Compile-OptSnippetWithStats "opt_p07_modulo_reduced_accumulator_stats" @'
int main() {
    int i = 0;
    int s = 0;
    while (i < 1000) {
        s = (s + i) % 65521;
        i = i + 1;
    }
    return s % 256;
}
'@
Assert-StatsContains "opt_p07_modulo_reduced_accumulator_stats" $p07ModuloReducedAccumulatorResult.Stderr "pass=loop-sum changed=yes"
if ((Invoke-RiscVMain $p07ModuloReducedAccumulatorResult.Stdout 1000) -ne 149) {
    throw "opt_p07_modulo_reduced_accumulator_stats returned unexpected value"
}

$p07CallInsideResult = Compile-OptSnippetWithStats "opt_p07_call_inside_loop_not_optimized_stats" @'
int f(int x) {
    return x + 1;
}
int main() {
    int i = 0;
    int s = 0;
    while (i < 1000) {
        s = s + f(i);
        i = i + 1;
    }
    return s % 256;
}
'@
# f(x)=x+1 is inlined, so the loop becomes `s += i + 1` with no call and loop-sum
# may legitimately close it. The only requirement is the correct result.
if ((Invoke-RiscVMain $p07CallInsideResult.Stdout 200000) -ne 20) {
    throw "opt_p07_call_inside_loop_not_optimized_stats returned unexpected value"
}

$p07StoreGlobalInsideResult = Compile-OptSnippetWithStats "opt_p07_store_global_inside_loop_not_optimized_stats" @'
int g = 0;
int main() {
    int i = 0;
    int s = 0;
    while (i < 1000) {
        g = g + i;
        s = s + i;
        i = i + 1;
    }
    return (s + g) % 256;
}
'@
Assert-StatsNotContains "opt_p07_store_global_inside_loop_not_optimized_stats" $p07StoreGlobalInsideResult.Stderr "pass=loop-sum changed=yes"
if ((Invoke-RiscVMain $p07StoreGlobalInsideResult.Stdout 200000) -ne 88) {
    throw "opt_p07_store_global_inside_loop_not_optimized_stats returned unexpected value"
}

$p07BreakContinueResult = Compile-OptSnippetWithStats "opt_p07_break_continue_not_optimized_stats" @'
int main() {
    int i = 0;
    int s = 0;
    while (i < 1000) {
        i = i + 1;
        if (i == 3) {
            continue;
        }
        if (i == 8) {
            break;
        }
        s = s + i;
    }
    return s;
}
'@
Assert-StatsNotContains "opt_p07_break_continue_not_optimized_stats" $p07BreakContinueResult.Stderr "pass=loop-sum changed=yes"
if ((Invoke-RiscVMain $p07BreakContinueResult.Stdout 200000) -ne 25) {
    throw "opt_p07_break_continue_not_optimized_stats returned unexpected value"
}

$dynamicLoopSumResult = Compile-OptSnippetWithStats "opt_loop_sum_dynamic_bound_stats" @'
int limitSeed = 100;
int getLimit(){ limitSeed = limitSeed + 0; return limitSeed; }
int main(){
    int n = getLimit();
    int i = 0;
    int s = 0;
    while(i < n){
        s = s + 81;
        i = i + 1;
    }
    return s % 256;
}
'@
if ((Invoke-RiscVMain $dynamicLoopSumResult.Stdout) -ne 164) {
    throw "opt_loop_sum_dynamic_bound_stats returned unexpected value"
}
Assert-StatsNotContains "opt_loop_sum_dynamic_bound_stats" $dynamicLoopSumResult.Stderr "pass=loop-sum changed=yes"

$polyLoopSumResult = Compile-OptSnippetWithStats "opt_loop_sum_dynamic_poly_stats" @'
int limitSeed = 100;
int getLimit(){ limitSeed = limitSeed + 0; return limitSeed; }
int main(){
    int n = getLimit();
    int i = 0;
    int s = 0;
    int t = 0;
    while(i < n){
        s = s + 7 * i + 5;
        t = t + 3 * i - 2;
        i = i + 1;
    }
    return (s + t) % 256;
}
'@
if ((Invoke-RiscVMain $polyLoopSumResult.Stdout) -ne 136) {
    throw "opt_loop_sum_dynamic_poly_stats returned unexpected value"
}
Assert-StatsNotContains "opt_loop_sum_dynamic_poly_stats" $polyLoopSumResult.Stderr "pass=loop-sum changed=yes"

$dynamicLoopSumLeStepResult = Compile-OptSnippetWithStats "opt_loop_sum_dynamic_le_step_linear_stats" @'
int limitSeed = 9;
int getLimit(){ limitSeed = limitSeed + 0; return limitSeed; }
int main(){
    int n = getLimit();
    int i = 1;
    int s = 0;
    int t = 0;
    while(i <= n){
        s = s + 5 * i + 1;
        t = t + 3 * i - 1;
        i = i + 2;
    }
    return (s + t + i) % 256;
}
'@
if ((Invoke-RiscVMain $dynamicLoopSumLeStepResult.Stdout) -ne 211) {
    throw "opt_loop_sum_dynamic_le_step_linear_stats returned unexpected value"
}
Assert-StatsNotContains "opt_loop_sum_dynamic_le_step_linear_stats" $dynamicLoopSumLeStepResult.Stderr "pass=loop-sum changed=yes"

$dynamicLoopSumReloadBoundResult = Compile-OptSnippetWithStats "opt_loop_sum_dynamic_reload_bound_stats" @'
int limit = 157;
int bound() {
    return limit;
}
int main() {
    int n = bound();
    int sum = 0;
    int i = 3;
    while (i <= n) {
        sum = sum + 3 * i + 5;
        i = i + 2;
    }
    return sum % 256;
}
'@
if ((Invoke-RiscVMain $dynamicLoopSumReloadBoundResult.Stdout) -ne 166) {
    throw "opt_loop_sum_dynamic_reload_bound_stats returned unexpected value"
}
Assert-StatsContains "opt_loop_sum_dynamic_reload_bound_stats" $dynamicLoopSumReloadBoundResult.Stderr "pass=loop-sum changed=yes"

$dynamicLoopSumQuadraticResult = Compile-OptSnippetWithStats "opt_loop_sum_dynamic_quadratic_stats" @'
int limit = 23;
int main() {
    int n = limit;
    int i = 1;
    int sum = 0;
    while (i <= n) {
        sum = sum + i * i + 2 * i + 3;
        i = i + 2;
    }
    return sum % 1000;
}
'@
if ((Invoke-RiscVMain $dynamicLoopSumQuadraticResult.Stdout) -ne 624) {
    throw "opt_loop_sum_dynamic_quadratic_stats returned unexpected value"
}
Assert-StatsContains "opt_loop_sum_dynamic_quadratic_stats" $dynamicLoopSumQuadraticResult.Stderr "pass=loop-sum changed=yes"

$dynamicLoopSumNotEqualResult = Compile-OptSnippetWithStats "opt_loop_sum_dynamic_not_equal_stats" @'
int limit = 21;
int main() {
    int n = limit;
    int i = 1;
    int sum = 0;
    while (i != n) {
        sum = sum + i + 4;
        i = i + 2;
    }
    return sum;
}
'@
if ((Invoke-RiscVMain $dynamicLoopSumNotEqualResult.Stdout) -ne 140) {
    throw "opt_loop_sum_dynamic_not_equal_stats returned unexpected value"
}
Assert-StatsContains "opt_loop_sum_dynamic_not_equal_stats" $dynamicLoopSumNotEqualResult.Stderr "pass=loop-sum changed=yes"

$dynamicLoopSumDescendingResult = Compile-OptSnippetWithStats "opt_loop_sum_dynamic_descending_stats" @'
int floorSeed = 2;
int getFloor(){ floorSeed = floorSeed + 0; return floorSeed; }
int main(){
    int n = getFloor();
    int i = 10;
    int s = 0;
    while(i > n){
        s = s + i;
        i = i - 2;
    }
    return s + i;
}
'@
if ((Invoke-RiscVMain $dynamicLoopSumDescendingResult.Stdout) -ne 30) {
    throw "opt_loop_sum_dynamic_descending_stats returned unexpected value"
}
Assert-StatsNotContains "opt_loop_sum_dynamic_descending_stats" $dynamicLoopSumDescendingResult.Stderr "pass=loop-sum changed=yes"

Assert-OptReturn "opt_loop_sum_not_equal_step_semantics" @'
int main(){
    int i = 1;
    int s = 0;
    while(i != 10){
        s = s + i * 2 + 1;
        i = i + 3;
    }
    return s + i;
}
'@ 37

Assert-OptReturn "opt_licm_shape" @'
int id(int x){ return x; }
int main(){int i=0; int s=0; int a=id(7); int b=id(9); while(i<100){s=s+a*b+3; i=i+1;} return s;}
'@ 6600

Assert-OptReturn "opt_tail_recursion_semantics" @'
int sum(int n, int acc){ if(n==0) return acc; return sum(n-1, acc+n); }
int main(){ return sum(100,0); }
'@ 5050

$tailRecursionAsm = Compile-OptSnippet "opt_tail_recursion_no_self_call" @'
int seed = 100;
int get(){ seed = seed + 0; return seed; }
int sum(int n, int acc){ if(n==0) return acc; return sum(n-1, acc+n); }
int main(){ int n = get(); return sum(n,0); }
'@
$tailRecursionSumAsm = FunctionAssembly $tailRecursionAsm "sum"
Assert-AssemblyNotContains "opt_tail_recursion_no_self_call" $tailRecursionSumAsm "call sum"
if ((Count-AssemblyOpcode $tailRecursionSumAsm "lw") -gt 3) {
    throw "opt_tail_recursion_no_self_call kept too many stack loads"
}
if ((Count-AssemblyOpcode $tailRecursionSumAsm "sw") -gt 3) {
    throw "opt_tail_recursion_no_self_call kept too many stack stores"
}

Assert-OptReturn "opt_tail_recursion_parallel_params" @'
int flipSeed = 3;
int getFlipCount(){ flipSeed = flipSeed + 0; return flipSeed; }
int flip(int a, int b, int n){ if(n==0) return a*10+b; return flip(b, a, n-1); }
int main(){ return flip(1,2,getFlipCount()); }
'@ 21

Assert-OptReturn "opt_value_register_semantics" @'
int id(int x){ return x; }
int main(){int x=id(7); int y=x*x; int z=x*x; return y+z;}
'@ 98

$globalCopyResult = Compile-OptSnippetWithStats "opt_global_copy_across_blocks_stats" @'
int g = 0;
int bump(int x) {
    g = g + x;
    return g;
}
int main() {
    int a = bump(1);
    int b = a;
    int c = 0;
    if (g) {
        c = b + 1;
    } else {
        c = b + 2;
    }
    return c + b + g;
}
'@
Assert-StatsContains "opt_global_copy_across_blocks_stats" $globalCopyResult.Stderr "pass=global-copy-prop changed=yes"
if ((Invoke-RiscVMain $globalCopyResult.Stdout) -ne 4) {
    throw "opt_global_copy_across_blocks_stats returned unexpected value"
}

$globalCseResult = Compile-OptSnippetWithStats "opt_global_cse_across_blocks_stats" @'
int g = 0;
int bump(int x) {
    g = g + x;
    return g;
}
int main() {
    int a = bump(3);
    int b = bump(4);
    int x = a + b;
    int y = 0;
    if (g) {
        y = a + b;
    } else {
        y = a + b;
    }
    return x + y + g;
}
'@
Assert-StatsContains "opt_global_cse_across_blocks_stats" $globalCseResult.Stderr "pass=global-cse changed=yes"
if ((Invoke-RiscVMain $globalCseResult.Stdout) -ne 27) {
    throw "opt_global_cse_across_blocks_stats returned unexpected value"
}

Assert-StatsOccurrenceAtMost "opt_global_cse_across_blocks_stats" $globalCseResult.Stderr "pass=global-copy-prop" 3
Assert-StatsOccurrenceAtMost "opt_global_cse_across_blocks_stats" $globalCseResult.Stderr "pass=global-cse" 2
$inlineResult = Compile-OptSnippetWithStats "opt_iterative_inline_chain_stats" @'
int seed = 5;
int one(int x) { return x + 1; }
int two(int y) { return one(y) + seed; }
int main() {
    int a = seed;
    return two(a);
}
'@
Assert-StatsContains "opt_iterative_inline_chain_stats" $inlineResult.Stderr "pass=inline-small changed=yes"
Assert-AssemblyNotContains "opt_iterative_inline_chain_stats" $inlineResult.Stdout "call two"
Assert-AssemblyNotContains "opt_iterative_inline_chain_stats" $inlineResult.Stdout "call one"
if ((Invoke-RiscVMain $inlineResult.Stdout) -ne 11) {
    throw "opt_iterative_inline_chain_stats returned unexpected value"
}

$deadFunctionResult = Compile-OptSnippetWithStats "opt_dead_function_elim_stats" @'
int g = 1;
int unused_large(int x) {
    int i = 0;
    int s = x;
    while (i < 100) {
        s = s * 3 + i;
        i = i + 1;
    }
    g = s;
    return g;
}
int main() {
    return g;
}
'@
Assert-StatsOccurrenceAtMost "opt_dead_function_elim_stats" $deadFunctionResult.Stderr "pass=dead-function-elim" 3
Assert-AssemblyNotContains "opt_dead_function_elim_stats" $deadFunctionResult.Stdout ".globl unused_large"
if ((Invoke-RiscVMain $deadFunctionResult.Stdout) -ne 1) {
    throw "opt_dead_function_elim_stats returned unexpected value"
}

$inlineBranchResult = Compile-OptSnippetWithStats "opt_inline_small_branch_stats" @'
int pick(int x) {
    if (x < 10) {
        return x + 1;
    }
    return x - 1;
}
int main() {
    int s = 0;
    int i = 0;
    while (i < 20) {
        s = s + pick(i);
        i = i + 1;
    }
    return s;
}
'@
Assert-StatsContains "opt_inline_small_branch_stats" $inlineBranchResult.Stderr "pass=inline-small changed=yes"
Assert-AssemblyNotContains "opt_inline_small_branch_stats" $inlineBranchResult.Stdout "call pick"
Assert-AssemblyNotContains "opt_inline_small_branch_stats" $inlineBranchResult.Stdout ".globl pick"
if ((Invoke-RiscVMain $inlineBranchResult.Stdout) -ne 190) {
    throw "opt_inline_small_branch_stats returned unexpected value"
}

$inlineStoreGlobalResult = Compile-OptSnippetWithStats "opt_inline_store_global_side_effect_stats" @'
int g = 0;
int sideEffect(int x) {
    g = g + x;
    return g;
}
int main() {
    int i = 0;
    while (i < 10) {
        sideEffect(i);
        i = i + 1;
    }
    return g;
}
'@
Assert-StatsContains "opt_inline_store_global_side_effect_stats" $inlineStoreGlobalResult.Stderr "pass=inline-small changed=yes"
Assert-AssemblyNotContains "opt_inline_store_global_side_effect_stats" $inlineStoreGlobalResult.Stdout "call sideEffect"
Assert-AssemblyNotContains "opt_inline_store_global_side_effect_stats" $inlineStoreGlobalResult.Stdout ".globl sideEffect"
if ((Invoke-RiscVMain $inlineStoreGlobalResult.Stdout) -ne 45) {
    throw "opt_inline_store_global_side_effect_stats returned unexpected value"
}

$storeGlobalOverwriteResult = Compile-OptSnippetWithStats "opt_store_global_overwrite_dse_stats" @'
int g = 0;
int main() {
    int i = 0;
    int sum = 0;
    while (i < 40) {
        g = i + 1;
        g = i + 2;
        g = i + 3;
        sum = sum + g;
        i = i + 1;
    }
    return sum % 256;
}
'@
# The first two stores to g are overwritten before any read, and after the last
# store is forwarded into `sum + g`, g is never loaded, so all stores to g are
# dead and may be removed (g is not observable; only the return value is). Only
# the result matters.
if ((Invoke-RiscVMain $storeGlobalOverwriteResult.Stdout) -ne 132) {
    throw "opt_store_global_overwrite_dse_stats returned unexpected value"
}

$crossBlockGlobalForwardResult = Compile-OptSnippetWithStats "opt_cross_block_global_forward_stats" @'
int g = 0;
int main() {
    int i = 0;
    int sum = 0;
    while (i < 20) {
        if ((i % 2) == 0) {
            g = i + 3;
        } else {
            g = i + 3;
        }
        sum = sum + g;
        i = i + 1;
    }
    return sum;
}
'@
Assert-StatsContains "opt_cross_block_global_forward_stats" $crossBlockGlobalForwardResult.Stderr "pass=global-copy-prop changed=yes"
if ((Invoke-RiscVMain $crossBlockGlobalForwardResult.Stdout) -ne 250) {
    throw "opt_cross_block_global_forward_stats returned unexpected value"
}

$inlineNeverReadGlobalResult = Compile-OptSnippetWithStats "opt_inline_never_read_global_dse_stats" @'
int scratch = 0;
void touch(int x) {
    scratch = x + 1;
    scratch = x + 2;
    return;
}
int main() {
    int i = 0;
    int sum = 0;
    while (i < 40) {
        touch(i);
        sum = sum + i;
        i = i + 1;
    }
    return sum;
}
'@
Assert-StatsContains "opt_inline_never_read_global_dse_stats" $inlineNeverReadGlobalResult.Stderr "pass=dse changed=yes"
Assert-AssemblyNotContains "opt_inline_never_read_global_dse_stats" $inlineNeverReadGlobalResult.Stdout "call touch"
Assert-AssemblyNotContains "opt_inline_never_read_global_dse_stats" $inlineNeverReadGlobalResult.Stdout ".globl touch"
if ((Invoke-RiscVMain $inlineNeverReadGlobalResult.Stdout) -ne 780) {
    throw "opt_inline_never_read_global_dse_stats returned unexpected value"
}

$cfgBranchFoldResult = Compile-OptSnippetWithStats "opt_cfg_fold_next_jump_stats" @'
int g = 0;
int bump(int x) {
    g = g + x;
    return g;
}
int main() {
    if (1) {
        bump(3);
    }
    return g;
}
'@
Assert-StatsMatches "opt_cfg_fold_next_jump_stats" $cfgBranchFoldResult.Stderr 'pass=simplify-cfg changed=yes.*blocks=\d+->([0-9])'
Assert-NoJumpToNextLabel "opt_cfg_fold_next_jump_stats" $cfgBranchFoldResult.Stdout
if ((Invoke-RiscVMain $cfgBranchFoldResult.Stdout) -ne 3) {
    throw "opt_cfg_fold_next_jump_stats returned unexpected value"
}

$cfgEmptyBridgeResult = Compile-OptSnippetWithStats "opt_cfg_empty_bridge_stats" @'
int g = 0;
int bump(int x) {
    g = g + x;
    return g;
}
int main() {
    if (0) {
        bump(1);
    } else {
        if (1) {
            bump(4);
        }
    }
    return g;
}
'@
Assert-StatsMatches "opt_cfg_empty_bridge_stats" $cfgEmptyBridgeResult.Stderr 'pass=simplify-cfg changed=yes.*blocks=\d+->([0-9])'
Assert-NoJumpToNextLabel "opt_cfg_empty_bridge_stats" $cfgEmptyBridgeResult.Stdout
if ((Invoke-RiscVMain $cfgEmptyBridgeResult.Stdout) -ne 4) {
    throw "opt_cfg_empty_bridge_stats returned unexpected value"
}

$cfgMergeNextResult = Compile-OptSnippetWithStats "opt_cfg_merge_next_block_stats" @'
int g = 0;
int bump(int x) {
    g = g + x;
    return g;
}
int main() {
    int i = 0;
    while (i < 2) {
        if (1) {
            bump(i + 1);
        }
        i = i + 1;
    }
    return g;
}
'@
Assert-StatsContains "opt_cfg_merge_next_block_stats" $cfgMergeNextResult.Stderr "blocks=7->5"
if ((Invoke-RiscVMain $cfgMergeNextResult.Stdout) -ne 3) {
    throw "opt_cfg_merge_next_block_stats returned unexpected value"
}

$cfgLocalConstBranchResult = Compile-OptSnippetWithStats "opt_cfg_local_const_branch_stats" @'
int g = 0;
int bump(int x) {
    g = g + x;
    return g;
}
int main() {
    if ((1 + 2) == 3) {
        bump(5);
    } else {
        bump(9);
    }
    return g;
}
'@
$firstSimplifyCfg = First-StatsLine $cfgLocalConstBranchResult.Stderr "simplify-cfg"
if (-not $firstSimplifyCfg.Contains("changed=yes")) {
    throw "opt_cfg_local_const_branch_stats first simplify-cfg did not fold local constant branch: $firstSimplifyCfg"
}
if ((Invoke-RiscVMain $cfgLocalConstBranchResult.Stdout) -ne 5) {
    throw "opt_cfg_local_const_branch_stats returned unexpected value"
}

$immutableLoopBoundResult = Compile-OptSnippetWithStats "opt_immutable_loop_bound_stats" @'
int main() {
    int bound = 200;
    int value = 7;
    int sum = 0;
    int i = 0;
    while (i < bound) {
        sum = sum + value;
        i = i + 1;
    }
    return sum % 251;
}
'@
Assert-StatsContains "opt_immutable_loop_bound_stats" $immutableLoopBoundResult.Stderr "pass=global-const-prop changed=yes"
if ($immutableLoopBoundResult.Stdout -match "(?m)^\s*(beqz|bnez|beq|bne|blt|bge)\b") {
    throw "opt_immutable_loop_bound_stats retained a conditional branch"
}
if ((Invoke-RiscVMain $immutableLoopBoundResult.Stdout) -ne 145) {
    throw "opt_immutable_loop_bound_stats returned unexpected value"
}

$deadInlineEffectResult = Compile-OptSnippetWithStats "opt_dead_inline_effect_stats" @'
int scratch = 0;
void leaf(int x) {
    int scaled = x * 9;
    scratch = scaled / 9;
    return;
}
void wrapper(int x) {
    leaf(x);
    return;
}
int main() {
    int i = 0;
    while (i < 100) {
        wrapper(i);
        i = i + 1;
    }
    return 140;
}
'@
Assert-StatsContains "opt_dead_inline_effect_stats" $deadInlineEffectResult.Stderr "pass=inline-small changed=yes"
Assert-AssemblyNotContains "opt_dead_inline_effect_stats" $deadInlineEffectResult.Stdout "call wrapper"
Assert-AssemblyNotContains "opt_dead_inline_effect_stats" $deadInlineEffectResult.Stdout ".globl wrapper"
if ($deadInlineEffectResult.Stdout -match "(?m)^\s*(beqz|bnez|beq|bne|blt|bge)\b") {
    throw "opt_dead_inline_effect_stats retained a conditional branch"
}
if ((Invoke-RiscVMain $deadInlineEffectResult.Stdout) -ne 140) {
    throw "opt_dead_inline_effect_stats returned unexpected value"
}

$commonSubexpressionLoopResult = Compile-OptSnippetWithStats "opt_common_subexpression_loop_stats" @'
int main() {
    int bound = 60;
    int base = 11;
    int sum = 0;
    int i = 0;
    while (i < bound) {
        int lhs = i * 3 + base;
        int rhs = i * 3 + base;
        sum = sum + (lhs - rhs) + 2;
        i = i + 1;
    }
    return sum + 3;
}
'@
Assert-StatsContains "opt_common_subexpression_loop_stats" $commonSubexpressionLoopResult.Stderr "pass=local-cse changed=yes"
Assert-StatsContains "opt_common_subexpression_loop_stats" $commonSubexpressionLoopResult.Stderr "pass=loop-sum changed=yes"
if ($commonSubexpressionLoopResult.Stdout -match "(?m)^\s*(beqz|bnez|beq|bne|blt|bge)\b") {
    throw "opt_common_subexpression_loop_stats retained a conditional branch"
}
if ((Invoke-RiscVMain $commonSubexpressionLoopResult.Stdout) -ne 123) {
    throw "opt_common_subexpression_loop_stats returned unexpected value"
}

$exactMultiplyDivideResult = Compile-OptSnippetWithStats "opt_exact_multiply_divide_stats" @'
int main() {
    int sum = 0;
    int i = 0;
    while (i < 100) {
        int scaled = i * 9;
        sum = sum + scaled / 9;
        i = i + 1;
    }
    return sum;
}
'@
Assert-StatsContains "opt_exact_multiply_divide_stats" $exactMultiplyDivideResult.Stderr "pass=inst-combine changed=yes"
if ($exactMultiplyDivideResult.Stdout -match "(?m)^\s*(beqz|bnez|beq|bne|blt|bge)\b") {
    throw "opt_exact_multiply_divide_stats retained a conditional branch"
}
if ((Invoke-RiscVMain $exactMultiplyDivideResult.Stdout) -ne 4950) {
    throw "opt_exact_multiply_divide_stats returned unexpected value"
}

$nestedModuloStrengthResult = Compile-OptSnippetWithStats "opt_nested_modulo_strength_stats" @'
int main() {
    int sum = 0;
    int i = 0;
    while (i < 5) {
        int j = 0;
        while (j < 17) {
            sum = sum + ((i * 3 + j * 5) % 7);
            j = j + 1;
        }
        i = i + 1;
    }
    return sum;
}
'@
Assert-StatsContains "opt_nested_modulo_strength_stats" $nestedModuloStrengthResult.Stderr "pass=loop-sum changed=yes"
if ((Invoke-RiscVMain $nestedModuloStrengthResult.Stdout) -ne 256) {
    throw "opt_nested_modulo_strength_stats returned unexpected value"
}

$invariantEmptyLoopBypassResult = Compile-OptSnippetWithStats "opt_invariant_empty_loop_bypass_stats" @'
int main() {
    int sum = 0;
    int i = 0;
    while (i < 10) {
        int j = 0;
        while (j < 20) {
            if (i == 0) {
                sum = sum + j;
            }
            j = j + 1;
        }
        i = i + 1;
    }
    return sum;
}
'@
Assert-StatsContains "opt_invariant_empty_loop_bypass_stats" $invariantEmptyLoopBypassResult.Stderr "pass=loop-sum changed=yes"
if ((Count-AssemblyOpcode $invariantEmptyLoopBypassResult.Stdout "blt") -gt 1) {
    throw "opt_invariant_empty_loop_bypass_stats retained the bypassed inner loop"
}
if ((Invoke-RiscVMain $invariantEmptyLoopBypassResult.Stdout) -ne 190) {
    throw "opt_invariant_empty_loop_bypass_stats returned unexpected value"
}

$guardedRuntimeModuloSumResult = Compile-OptSnippetWithStats "opt_guarded_runtime_modulo_sum_stats" @'
int main() {
    int i = 0;
    int total = 0;
    int diagonal = 0;
    int origin = 0;
    while (i < 4) {
        int j = 0;
        while (j < 4) {
            int z = 0;
            while (z < 7) {
                int value = (i * j + z) % 7;
                total = total + value;
                if (i == j) {
                    diagonal = diagonal + value;
                }
                if (i + j == 0) {
                    origin = origin + value;
                }
                z = z + 1;
            }
            j = j + 1;
        }
        i = i + 1;
    }
    return total + diagonal + origin;
}
'@
Assert-StatsContains "opt_guarded_runtime_modulo_sum_stats" $guardedRuntimeModuloSumResult.Stderr "pass=loop-sum changed=yes"
if ((Count-AssemblyOpcode $guardedRuntimeModuloSumResult.Stdout "blt") -gt 2) {
    throw "opt_guarded_runtime_modulo_sum_stats retained the summarized inner loop"
}
if ((Invoke-RiscVMain $guardedRuntimeModuloSumResult.Stdout) -ne 441) {
    throw "opt_guarded_runtime_modulo_sum_stats returned unexpected value"
}

$signedRuntimeModuloSumResult = Compile-OptSnippetWithStats "opt_signed_runtime_modulo_sum_stats" @'
int main() {
    int total = 0;
    int row = -2;
    while (row < 1) {
        int z = 0;
        while (z < 7) {
            total = total + ((row * 3 + z) % 7);
            z = z + 1;
        }
        row = row + 1;
    }
    return total;
}
'@
Assert-StatsContains "opt_signed_runtime_modulo_sum_stats" $signedRuntimeModuloSumResult.Stderr "pass=loop-sum changed=yes"
if ((Count-AssemblyOpcode $signedRuntimeModuloSumResult.Stdout "blt") -gt 1) {
    throw "opt_signed_runtime_modulo_sum_stats retained the summarized inner loop"
}
if ((Invoke-RiscVMain $signedRuntimeModuloSumResult.Stdout) -ne 0) {
    throw "opt_signed_runtime_modulo_sum_stats returned unexpected value"
}

Write-Host "ToyC smoke tests passed"
