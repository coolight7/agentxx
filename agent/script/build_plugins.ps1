# build_plugins.ps1 —— 只重新编译插件动态库 (Windows 版)
#
# 背景: 插件的动态库目标属于 lib 子工程 (plugins/ 经 add_subdirectory 进 lib 的
# 构建树), 顶层 `cmake --build agent/build/windows-debug` 只会编译它们的 .obj,
# 不会产出 exec/plugins/<插件名>/*.dll。改了插件源码 / 抬了插件 API 代次之后,
# 必须对**内层 lib 构建目录**显式点名插件目标, 否则 exec/plugins 里还是旧的 DLL
# (运行时会报 "插件 API 版本过低")。
#
# 本脚本就是这条命令的封装: 自动定位内层构建目录、列出可用插件目标、逐个编译并打印
# 每个目标的耗时与产物时间戳。
#
# 为什么提供两种模式:
# - 指定了插件名 (或 -PerTarget / -KeepGoing): 逐个目标调用 ——
#   每目标一行进度 (`(3/25) agentxx_math ok 10.8s`), 失败直接点名, 长构建不会看起来像卡住
# - 不指定目标 (编全部): 一次把全部目标交给 cmake —— 它们由构建系统并行处理,
#   总耗时最短; 失败时用 outputs 里的 "(未重新链接)" 定位未编成的插件
#
# 用法 (用 pwsh 运行; 自动化/无人值守建议带 -NoProfile):
#   pwsh -NoProfile -File agent/script/build_plugins.ps1                  # 编译全部插件 (Debug)
#   pwsh -NoProfile -File agent/script/build_plugins.ps1 -List            # 只列出可编译的插件目标
#   pwsh -NoProfile -File agent/script/build_plugins.ps1 agentxx_filesystem agentxx_math
#   pwsh -NoProfile -File agent/script/build_plugins.ps1 -Config Release -BuildDir agent/build/windows-release
#   pwsh -NoProfile -File agent/script/build_plugins.ps1 -Jobs 8          # 每个目标内并行 8 个编译进程
#   pwsh -NoProfile -File agent/script/build_plugins.ps1 -PerTarget       # 编全部也逐个目标 (看进度)
#   pwsh -NoProfile -File agent/script/build_plugins.ps1 -KeepGoing       # 某个目标失败后继续编剩下的
#
# 退出码: 0 全部成功 / 其它 = cmake 的退出码 / 3 = 同一构建目录已有构建在跑
# 耗时: 全部插件从零编译约 4~8 分钟 (取决于机器与是否命中增量), 单个插件通常 10~90 秒。
#       自动化调用时把命令超时设到 15 分钟以上。
#
# 前置条件: 先完成过一次完整构建 (script/windows_debug_build.bat 或 release 版),
# 内层 lib 构建目录已配置好; 本脚本不重新配置 CMake。

[CmdletBinding(PositionalBinding = $false)]
param(
    # 顶层构建目录 (含 agentxx_lib_repo-prefix 的那个目录)
    [string] $BuildDir = '',
    # 构建配置 (Debug / Release)
    [string] $Config = 'Debug',
    # 只编译指定的插件目标 (省略 = 全部); 只有它接受位置参数
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $Targets = @(),
    # 并行编译进程数 (0 = 用构建系统的默认值)
    [int] $Jobs = 0,
    # 只列出可编译的插件目标
    [switch] $List,
    # 逐个目标编译 (每目标一行进度与耗时, 失败能点名); 指定了插件名时默认就是逐个
    [switch] $PerTarget,
    # 某个目标失败后继续编译剩下的 (隐含 -PerTarget; 默认遇到失败就停)
    [switch] $KeepGoing
)

$ErrorActionPreference = 'Stop'

# 原生命令非零退出只写 $LASTEXITCODE, 不要变成异常 (PowerShell 7.3+ 的新开关)
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

# 与构建脚本保持一致的编译环境: 关掉 vcpkg 的用户级集成 (见 AGENTS.md 常见问题),
# MSVC 输出英文便于定位错误
$env:VCPKG_ROOT              = ''
$env:VCPkgLocalAppDataDisabled = '1'
if (-not $env:VSLANG) { $env:VSLANG = '1033' }

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$srcDir    = (Resolve-Path (Join-Path $scriptDir '..')).Path
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $srcDir 'build\windows-debug'
}
if (-not (Test-Path -LiteralPath $BuildDir)) {
    Write-Error "构建目录不存在: $BuildDir`n先跑一次完整构建 (agent/script/windows_debug_build.bat)，再回来编插件。"
    exit 1
}

# 内层 lib 构建目录: CMakeCache.txt 在这里, 插件目标也在它的构建树上
$libBuild = Join-Path $BuildDir 'agentxx_lib_repo-prefix\src\agentxx_lib_repo-build'
if (-not (Test-Path -LiteralPath (Join-Path $libBuild 'CMakeCache.txt'))) {
    Write-Error @"
找不到已配置的内层构建目录: $libBuild
先跑一次完整构建 (agent/script/windows_debug_build.bat 或 windows_release_build.bat)，
它会把内层构建目录配好; 本脚本只做增量编译, 不重新配置 CMake。
"@
    exit 1
}

# 可编译的插件目标: 插件目标由 plugins/ 的 add_subdirectory 生成, 每个目标在
# 构建树里是 agentxx_plugins_builtin\<名字>\<名字>.vcxproj
$pluginRoot = Join-Path $libBuild 'agentxx_plugins_builtin'
$available  = @()
if (Test-Path -LiteralPath $pluginRoot) {
    $available = Get-ChildItem -LiteralPath $pluginRoot -Directory |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName ($_.Name + '.vcxproj')) } |
        ForEach-Object { $_.Name } |
        Sort-Object
}
if ($available.Count -eq 0) {
    Write-Error @"
在 $pluginRoot 下没找到任何插件目标 (缺少 <名字>\<名字>.vcxproj)。
可能原因: 这份构建配置里插件被关掉了 (AGENTXX_PLUGIN_BUILTIN_LIST)，
或者构建目录还没配置完成。
"@
    exit 1
}

if ($List) {
    $available | ForEach-Object { Write-Output $_ }
    exit 0
}

$selected = @()
if ($Targets.Count -gt 0) {
    foreach ($t in $Targets) {
        if ($available -notcontains $t) {
            Write-Error "未知的插件目标: $t`n可用目标: $($available -join ', ')"
            exit 1
        }
        $selected += $t
    }
} else {
    $selected = $available
}

Write-Output "[build_plugins] build dir : $libBuild"
Write-Output "[build_plugins] config    : $Config"
Write-Output "[build_plugins] targets   : $($selected -join ', ')"
Write-Output "[build_plugins] jobs      : $(if ($Jobs -gt 0) { $Jobs } else { 'default' })"

# 同一构建目录只允许一个插件构建在跑: 两个 MSBuild 同时在一个工程文件上干活会互等,
# 看起来就是"卡住" (上一轮被中断的构建还在后台跑时最容易遇到)。
$lockPath = Join-Path $libBuild '.build_plugins.lock'
$lockHeld = $false
if (Test-Path -LiteralPath $lockPath) {
    $holder = 0
    $holderStarted = ''
    try {
        $info = Get-Content -LiteralPath $lockPath -Raw | ConvertFrom-Json
        $holder        = [int] $info.pid
        $holderStarted = [string] $info.started
    } catch {
        # 锁文件坏了: 当作遗留处理
    }
    if ($holder -gt 0 -and (Get-Process -Id $holder -ErrorAction SilentlyContinue)) {
        # 这里不用 Write-Error: $ErrorActionPreference='Stop' 会把它变成终止错误,
        # 调方就拿不到下面约定的退出码 3 了
        Write-Output "[build_plugins] 已有构建在跑: pid=$holder 开始于 $holderStarted (同一个构建目录)"
        Write-Output '[build_plugins] 同时跑两个会在同一批 .vcxproj 上互等, 看起来像卡死 —— 等它结束再跑。'
        Write-Output '[build_plugins] 如果确认那是上次被中断留下的进程: 结束它, 或删掉锁文件后再跑:'
        Write-Output "[build_plugins]   $lockPath"
        exit 3
    }
    Write-Output '[build_plugins] 清理上一轮遗留的锁 (pid 已不在)'
    Remove-Item -LiteralPath $lockPath -Force -ErrorAction SilentlyContinue
}
try {
    $lockJson = (@{
            pid     = $PID
            started = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss')
            targets = ($selected -join ',')
        } | ConvertTo-Json -Compress)
    [IO.File]::WriteAllText($lockPath, $lockJson, (New-Object Text.UTF8Encoding $false))
    $lockHeld = $true
} catch {
    # 锁写不下去 (只读目录等) 不阻塞编译, 只是少了并发保护
}

$failed    = @()
$exitCode  = 0
$runStart  = Get-Date
$total     = $selected.Count
$index     = 0
# 逐个目标编 vs 一次传全部目标:
# - 一次传全部 (默认, "全部插件"场景): cmake 把它们交给构建系统并行, 总耗时最短;
#   代价是失败时不点名具体目标 (用 outputs 里的 "(未重新链接)" 定位)
# - 逐个目标 (指定了插件名 / -PerTarget / -KeepGoing): 每个目标一行进度与耗时,
#   失败能直接点名, 也能继续编剩下的
$usePerTarget = $PerTarget -or $KeepGoing -or ($Targets.Count -gt 0)
$singleCall   = $false
$swTotal      = [Diagnostics.Stopwatch]::StartNew()
try {
    if ($usePerTarget) {
        foreach ($name in $selected) {
            $index++
            $cmakeArgs = @('--build', $libBuild, '--config', $Config, '--target', $name)
            if ($Jobs -gt 0) {
                $cmakeArgs += @('--parallel', $Jobs)
            }
            Write-Output ("[build_plugins] ({0}/{1}) {2} ..." -f $index, $total, $name)
            $sw = [Diagnostics.Stopwatch]::StartNew()
            & cmake @cmakeArgs
            $rc = $LASTEXITCODE
            $sw.Stop()
            if ($rc -ne 0) {
                $failed += $name
                if ($exitCode -eq 0) {
                    $exitCode = $rc
                }
                Write-Output ("[build_plugins] ({0}/{1}) {2} FAILED (exit {3}, {4:N1}s)" -f `
                        $index, $total, $name, $rc, $sw.Elapsed.TotalSeconds)
                if (-not $KeepGoing) {
                    break
                }
            } else {
                Write-Output ("[build_plugins] ({0}/{1}) {2} ok ({3:N1}s)" -f `
                        $index, $total, $name, $sw.Elapsed.TotalSeconds)
            }
        }
    } else {
        $singleCall = $true
        $cmakeArgs  = @('--build', $libBuild, '--config', $Config, '--target') + $selected
        if ($Jobs -gt 0) {
            $cmakeArgs += @('--parallel', $Jobs)
        }
        Write-Output ("[build_plugins] building {0} targets in one cmake call ..." -f $total)
        $sw = [Diagnostics.Stopwatch]::StartNew()
        & cmake @cmakeArgs
        $rc = $LASTEXITCODE
        $sw.Stop()
        if ($rc -ne 0) {
            $exitCode = $rc
            Write-Output ("[build_plugins] build FAILED (exit {0}, {1:N1}s)" -f $rc, $sw.Elapsed.TotalSeconds)
        } else {
            Write-Output ("[build_plugins] all targets ok ({0:N1}s)" -f $sw.Elapsed.TotalSeconds)
        }
    }
} finally {
    $swTotal.Stop()
    if ($lockHeld) {
        Remove-Item -LiteralPath $lockPath -Force -ErrorAction SilentlyContinue
    }
}

# 产物时间戳: 确认 exec/plugins 下的 DLL 确实被替换了 (这一步最容易漏)
# - 时间戳早于本次开始时间的目标标 "(未重新链接)": 增量构建没有重新链接它
$execPlugins = Join-Path $BuildDir 'exec\plugins'
if (Test-Path -LiteralPath $execPlugins) {
    Write-Output '[build_plugins] outputs:'
    foreach ($name in $selected) {
        $dir = Join-Path $execPlugins $name
        if (-not (Test-Path -LiteralPath $dir)) {
            Write-Output ("  {0,-34} (no output dir)" -f $name)
            continue
        }
        $dll = Get-ChildItem -LiteralPath $dir -Filter *.dll -Recurse -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending |
            Select-Object -First 1
        if ($dll) {
            $stale = if ($dll.LastWriteTime -lt $runStart) { '  (未重新链接)' } else { '' }
            Write-Output ("  {0,-34} {1}  {2}{3}" -f `
                    $name, $dll.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'), $dll.Name, $stale)
        } else {
            Write-Output ("  {0,-34} (no dll)" -f $name)
        }
    }
}

if ($failed.Count -gt 0) {
    Write-Output ("[build_plugins] failed targets: {0}" -f ($failed -join ', '))
} elseif ($singleCall -and $exitCode -ne 0) {
    Write-Output '[build_plugins] build failed: 看 outputs 里标了 "(未重新链接)" 的目标定位未编成的插件'
}
$okCount = if ($singleCall) {
    if ($exitCode -eq 0) { $total } else { 0 }
} else {
    $total - $failed.Count
}
Write-Output ("[build_plugins] done in {0:N1}s ({1}/{2} targets ok)" -f `
        $swTotal.Elapsed.TotalSeconds, $okCount, $total)
exit $exitCode
