param(
    [string]$Compiler = "..\..\compiler.exe"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$CompilerPath = Resolve-Path -LiteralPath (Join-Path $Root $Compiler)
$Clang = Get-Command clang -ErrorAction SilentlyContinue
if ($null -eq $Clang) {
    $LLVMClang = "C:\Program Files\LLVM\bin\clang.exe"
    if (Test-Path -LiteralPath $LLVMClang) {
        $Clang = Get-Item -LiteralPath $LLVMClang
    }
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
            "div" { Set-Reg $parts[1] ([int]((Get-Reg $parts[2]) / (Get-Reg $parts[3]))) }
            "rem" { Set-Reg $parts[1] ((Get-Reg $parts[2]) % (Get-Reg $parts[3])) }
            "and" { Set-Reg $parts[1] ((Get-Reg $parts[2]) -band (Get-Reg $parts[3])) }
            "or" { Set-Reg $parts[1] ((Get-Reg $parts[2]) -bor (Get-Reg $parts[3])) }
            "xori" { Set-Reg $parts[1] ((Get-Reg $parts[2]) -bxor [int]$parts[3]) }
            "slli" { Set-Reg $parts[1] ((Get-Reg $parts[2]) -shl [int]$parts[3]) }
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

function Compile-Source {
    param(
        [string]$Name,
        [string]$Source,
        [switch]$Optimize
    )

    $suffix = if ($Optimize) { "opt" } else { "ast" }
    $output = Join-Path $Root "$Name.$suffix.s"
    if ($Optimize) {
        $Source | & $CompilerPath -opt > $output
    } else {
        $Source | & $CompilerPath > $output
    }
    if ($LASTEXITCODE -ne 0) {
        throw "$Name $suffix compilation failed"
    }
    return Get-Content -LiteralPath $output -Raw
}

function Assert-RiscVAssemblyAccepted {
    param(
        [string]$Name,
        [string]$Asm,
        [string]$Suffix
    )

    if ($null -eq $Clang) {
        return
    }

    $asmPath = Join-Path $Root "$Name.$Suffix.llvm.s"
    $objPath = Join-Path $Root "$Name.$Suffix.o"
    Set-Content -LiteralPath $asmPath -Value $Asm -Encoding ascii
    & $Clang.FullName --target=riscv32-unknown-elf -march=rv32im -mabi=ilp32 -c $asmPath -o $objPath
    if ($LASTEXITCODE -ne 0) {
        throw "$Name $Suffix LLVM RISC-V assembly failed"
    }
}

function Assert-BackendsAgree {
    param(
        [string]$Name,
        [string]$Source,
        [int]$Expected
    )

    $plainAsm = Compile-Source $Name $Source
    $optAsm = Compile-Source $Name $Source -Optimize
    Assert-RiscVAssemblyAccepted $Name $plainAsm "plain"
    Assert-RiscVAssemblyAccepted $Name $optAsm "opt"
    $plainResult = Invoke-RiscVMain $plainAsm
    $optResult = Invoke-RiscVMain $optAsm
    if ($plainResult -ne $Expected) {
        throw "$Name plain backend returned $plainResult, expected $Expected"
    }
    if ($optResult -ne $Expected) {
        throw "$Name optimized backend returned $optResult, expected $Expected"
    }
    if ($plainResult -ne $optResult) {
        throw "$Name backend mismatch: plain=$plainResult opt=$optResult"
    }
}

Assert-BackendsAgree "backend_call_global" @'
int g = 1;
int setg(int x) {
    g = x;
    return x + 10;
}
int main() {
    return setg(7) + g;
}
'@ 24

Assert-BackendsAgree "backend_loop_short_circuit" @'
int g = 0;
int bump(int x) {
    g = g + x;
    return g;
}
int main() {
    int i = 0;
    int s = 0;
    while (i < 4) {
        if ((i != 2 && bump(i)) || i == 0) {
            s = s + g + i;
        }
        i = i + 1;
    }
    return s + g;
}
'@ 13

Assert-BackendsAgree "backend_many_args" @'
int sum9(int a,int b,int c,int d,int e,int f,int g,int h,int i) {
    return a+b+c+d+e+f+g+h+i;
}
int main() {
    return sum9(1,2,3,4,5,6,7,8,9);
}
'@ 45

Write-Host "ToyC backend comparison tests passed"
