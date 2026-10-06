# gate.ps1 —— 一键质量门禁 (Windows 版; 计划 TST-8)
#
# 与 agent/script/gate.sh 对应的 Windows 实现: 构建 (可选) → 全模块 fail-fast 测试
# (含 boundaries 边界检查 / wire_roundtrip 协议往返 / config_validation 配置校验 /
# ui_snapshot UI 快照) → 基准 (可选)。
# 导出符号白名单与 SDK 反例编译依赖 nm/python3 与 bash, 在 Windows 上跳过 (请在
# Linux/macOS 侧跑 gate.sh 完成这两项)。
#
# sanitizer: 本项目 Debug 构建默认带 ASan (+ MSVC 无 UBSan), 即"跑 Debug 测试"本身
# 就是 sanitizer 门禁。
#
# 用法:
#   pwsh -File agent/script/gate.ps1
#   pwsh -File agent/script/gate.ps1 -Build
#   pwsh -File agent/script/gate.ps1 -Build -Bench -BuildDir agent/build/windows-debug
#
# 退出码: 0 = 全部通过; 1 = 有检查失败
[CmdletBinding()]
param(
    [switch]$Build,
    [switch]$Bench,
    [string]$BuildDir = "",
    [string]$Modules = ""
)

$ErrorActionPreference = "Continue"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (Resolve-Path (Join-Path $scriptDir "../..")).Path

if ([string]::IsNullOrEmpty($BuildDir)) {
    $BuildDir = Join-Path $repoRoot "agent/build/windows-debug"
}

$testExe = Join-Path $BuildDir "exec/agentxx_test.exe"
$testBuildDir = Join-Path $BuildDir "agentxx_test_repo-prefix/src/agentxx_test_repo-build"
$failures = 0
$summary = New-Object System.Collections.Generic.List[string]

function Record-Result([string]$Name, [string]$Status, [string]$Detail = "") {
    $line = "{0,-6} {1}" -f $Status.ToUpper(), $Name
    if ($Detail -ne "") { $line += "  ($Detail)" }
    $summary.Add($line)
    if ($Status -eq "fail") { $script:failures++ }
}

Write-Output "[gate] repo      : $repoRoot"
Write-Output "[gate] build dir : $BuildDir"

# ---------------- 1. 构建 ----------------
if ($Build) {
    Write-Output "[gate] building agentxx_test ..."
    & cmake --build $testBuildDir --config Debug --target agentxx_test --parallel 8
    if ($LASTEXITCODE -ne 0) {
        Record-Result "build" "fail" "cmake --build agentxx_test"
        Write-Output "`n===== gate summary ====="
        $summary | ForEach-Object { Write-Output $_ }
        exit 1
    }
    Record-Result "build" "ok"
} else {
    Record-Result "build" "skip" "use -Build"
}

# ---------------- 2. 单元测试 (fail-fast) ----------------
if (-not (Test-Path $testExe)) {
    Record-Result "unit tests" "fail" "missing $testExe"
} else {
    Write-Output "[gate] running unit tests (fail-fast) ..."
    if ([string]::IsNullOrEmpty($Modules)) {
        & $testExe -f
    } else {
        $moduleArgs = $Modules.Split(",") | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" }
        & $testExe -f @moduleArgs
    }
    if ($LASTEXITCODE -eq 0) {
        Record-Result "unit tests" "ok" "fail-fast, all modules"
    } else {
        Record-Result "unit tests" "fail" "see output above"
    }
}

# ---------------- 3. 基准 (可选) ----------------
if ($Bench) {
    $benchExe = Join-Path $BuildDir "exec/agentxx_benchmark.exe"
    if (Test-Path $benchExe) {
        Write-Output "[gate] running benchmark (report only) ..."
        & $benchExe
        if ($LASTEXITCODE -eq 0) {
            Record-Result "benchmark" "ok" "report under exec/bench"
        } else {
            Record-Result "benchmark" "fail"
        }
    } else {
        Record-Result "benchmark" "skip" "release build required: $benchExe"
    }
} else {
    Record-Result "benchmark" "skip" "use -Bench"
}

# ---------------- 4. 平台相关门禁 (bash 脚本) ----------------
Record-Result "plugin exports" "skip" "run agent/script/gate.sh on Linux/macOS (needs nm)"
Record-Result "sdk negative compile" "skip" "run agent/script/gate.sh on Linux/macOS (needs python3)"

Write-Output "`n===== gate summary ====="
$summary | ForEach-Object { Write-Output $_ }
if ($failures -ne 0) {
    Write-Output "[gate] FAILED: $failures check(s) failed"
    exit 1
}
Write-Output "[gate] OK"
