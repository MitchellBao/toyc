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

function Compile-OptSnippet {
    param(
        [string]$Name,
        [string]$Source
    )

    $output = Join-Path $Root "$Name.s"
    $Source | & $CompilerPath -opt > $output
    if ($LASTEXITCODE -ne 0) {
        throw "$Name optimized compilation failed"
    }
    return Get-Content -LiteralPath $output -Raw
}

function Count-Fragment {
    param(
        [string]$Text,
        [string]$Fragment
    )

    return ([regex]::Matches($Text, [regex]::Escape($Fragment))).Count
}

$deadStoreAsm = Compile-OptSnippet "opt_dead_store" @'
int main(){int x=1; x=2; x=3; return x;}
'@
if ((Count-Fragment $deadStoreAsm "mv s1, a0") -gt 1) {
    throw "optimized dead-store sample still keeps overwritten local stores"
}

$crossBlockAsm = Compile-OptSnippet "opt_cross_block" @'
int choose(){ return 1; }
int main(){int x=0; if(choose()){x=5;} else {x=5;} return x+1;}
'@
if (-not $crossBlockAsm.Contains("li a0, 6")) {
    throw "cross-block constant propagation did not fold common branch value"
}

$licmAsm = Compile-OptSnippet "opt_licm" @'
int id(int x){ return x; }
int main(){int i=0; int s=0; int a=id(7); int b=id(9); while(i<100){s=s+a*b+3; i=i+1;} return s;}
'@
if ($licmAsm -match "(?s)\.L_main_2:.*\bmul\b") {
    throw "LICM sample still multiplies invariant values inside the loop body"
}
if ($licmAsm -match "(?s)\.L_main_2:\s+j \.L_main_1") {
    throw "LICM sample incorrectly deleted the loop body updates"
}

$tailAsm = Compile-OptSnippet "opt_tail_recursion" @'
int sum(int n, int acc){ if(n==0) return acc; return sum(n-1, acc+n); }
int main(){ return sum(100,0); }
'@
if ((Count-Fragment $tailAsm "call sum") -gt 1) {
    throw "tail-recursive self call was not lowered to a loop"
}

$valueRegAsm = Compile-OptSnippet "opt_value_register" @'
int id(int x){ return x; }
int main(){int x=id(7); int y=x*x; int z=x*x; return y+z;}
'@
if ($valueRegAsm -match "(?s)\bmul a0, t0, a0\s+sw a0, -\d+\(s0\)") {
    throw "multi-use IR value was spilled instead of kept in a register"
}

Write-Host "ToyC smoke tests passed"
