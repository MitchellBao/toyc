param(
    [string]$Compiler = "..\..\compiler.exe"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$CompilerPath = Resolve-Path -LiteralPath (Join-Path $Root $Compiler)

$samples = @(
    @{
        Name = "perf_loop_locals"
        Source = @'
int main() {
    int i = 0;
    int s = 0;
    while (i < 1000000) {
        s = s + i;
        i = i + 1;
    }
    return s;
}
'@
    },
    @{
        Name = "perf_call_args"
        Source = @'
int add(int a, int b) {
    return a + b;
}
int f(int x) {
    return x + 1;
}
int g(int x) {
    return x * 2;
}
int main() {
    return add(1, 2) + f(g(10));
}
'@
    },
    @{
        Name = "perf_combined"
        Source = @'
int global = 3;
int id(int x) {
    return x;
}
int main() {
    int i = 0;
    int a = id(7);
    int b = id(9);
    int s = 0;
    while (i < 2000) {
        int t = a * b + a * b;
        s = s + t + global * 0 + i * 1;
        i = i + 1;
    }
    return s;
}
'@
    },
    @{
        Name = "perf_tail_recursion"
        Source = @'
int sum(int n, int acc) {
    if (n == 0) return acc;
    return sum(n - 1, acc + n);
}
int main() {
    return sum(1000, 0);
}
'@
    }
)

$opcodes = @("lw", "sw", "call", "j", "beq", "bne", "beqz", "bnez", "mul", "div", "rem", "mv", "li", "addi")

function Invoke-Compiler {
    param(
        [string]$Name,
        [string]$Source,
        [switch]$Optimize
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

function Compile-Mode {
    param(
        [string]$Name,
        [string]$Source,
        [switch]$Optimize
    )

    $mode = if ($Optimize) { "opt" } else { "plain" }
    $output = Join-Path $Root "$Name.$mode.perf.s"
    $result = Invoke-Compiler $Name $Source -Optimize:$Optimize
    if ($result.ExitCode -ne 0) {
        throw "$Name $mode compilation failed: $($result.Stderr)"
    }
    Set-Content -LiteralPath $output -Value $result.Stdout -Encoding ascii
    return Get-Content -LiteralPath $output -Raw
}

function Measure-Assembly {
    param(
        [string]$Name,
        [string]$Mode,
        [string]$Asm
    )

    $lines = @($Asm -split "`r?`n" | ForEach-Object { ($_ -replace '#.*$', '').Trim() } | Where-Object { $_.Length -gt 0 })
    $row = [ordered]@{
        Sample = $Name
        Mode = $Mode
        Lines = $lines.Count
    }
    foreach ($opcode in $opcodes) {
        $count = 0
        foreach ($line in $lines) {
            if ($line -match "^\s*$([regex]::Escape($opcode))(\s|$)") {
                ++$count
            }
        }
        $row[$opcode] = $count
    }
    [pscustomobject]$row
}

$results = @()
foreach ($sample in $samples) {
    $plainAsm = Compile-Mode $sample.Name $sample.Source
    $optAsm = Compile-Mode $sample.Name $sample.Source -Optimize
    $results += Measure-Assembly $sample.Name "plain" $plainAsm
    $results += Measure-Assembly $sample.Name "opt" $optAsm
}

$results | ConvertTo-Csv -NoTypeInformation
