param(
    [string]$Compiler = "..\..\compiler.exe"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$CompilerPath = Resolve-Path -LiteralPath (Join-Path $Root $Compiler)

$basicInput = Join-Path $Root "basic.tc"
$basicOutput = Join-Path $Root "basic.s"
Get-Content -LiteralPath $basicInput -Raw | & $CompilerPath > $basicOutput
if ($LASTEXITCODE -ne 0) {
    throw "basic.tc compilation failed"
}

$asm = Get-Content -LiteralPath $basicOutput -Raw
foreach ($needle in @(".globl main", "main:", "call add", "call fact", "beqz")) {
    if (-not $asm.Contains($needle)) {
        throw "basic.s missing expected assembly fragment: $needle"
    }
}

$flowInput = Join-Path $Root "control_flow.tc"
$flowOutput = Join-Path $Root "control_flow.s"
Get-Content -LiteralPath $flowInput -Raw | & $CompilerPath > $flowOutput
if ($LASTEXITCODE -ne 0) {
    throw "control_flow.tc compilation failed"
}

$flowAsm = Get-Content -LiteralPath $flowOutput -Raw
foreach ($needle in @("call bump", "rem", ".L_or_true_", ".L_while_cond_")) {
    if (-not $flowAsm.Contains($needle)) {
        throw "control_flow.s missing expected assembly fragment: $needle"
    }
}

$optimizedLoop = @'
int main() {
    int i = 0;
    int s = 0;
    while (i < 10) {
        i = i + 1;
        s = s + i;
    }
    return s;
}
'@
$optOutput = Join-Path $Root "optimized_loop.s"
$optimizedLoop | & $CompilerPath -opt > $optOutput
if ($LASTEXITCODE -ne 0) {
    throw "optimized loop compilation failed"
}

$optAsm = Get-Content -LiteralPath $optOutput -Raw
if (-not ($optAsm.Contains("beqz") -or $optAsm.Contains("bnez"))) {
    throw "optimized loop condition was incorrectly folded away"
}

$semanticInput = Join-Path $Root "semantic_error.tc"
Get-Content -LiteralPath $semanticInput -Raw | & $CompilerPath > $null
if ($LASTEXITCODE -eq 0) {
    throw "semantic_error.tc should fail"
}

Write-Host "ToyC smoke tests passed"
