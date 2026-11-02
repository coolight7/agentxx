# build_plugins.ps1 —— 只重新编译插件动态库 (Windows 版)
#
# 背景: 插件的动态库目标属于 lib 子工程 (plugins/ 经 add_subdirectory 进 lib 的
# 构建树), 顶层 `cmake --build agent/build/windows-debug` 只会编译它们的 .obj,
# 不会产出 exec/plugins/<插件名>/*.dll。改了插件源码 / 抬了插件 API 代次之后,
# 必须对**内层 lib 构建目录**显式点名插件目标, 否则 exec/plugins 里还是旧的 DLL
# (运行时会报 "插件 API 版本过低")。
#
# 本脚本就是这条命令的封装: 自动定位内层构建目录、列出可用插件目标、编译并打印
# 产物的时间戳。
#
# 用法 (用 pwsh 运行, 与 gate.ps1 一致):
#   pwsh -File agent/script/build_plugins.ps1                     # 编译全部插件 (Debug)
#   pwsh -File agent/script/build_plugins.ps1 -List               # 只列出可编译的插件目标
#   pwsh -File agent/script/build_plugins.ps1 agentxx_filesystem agentxx_math
#   pwsh -File agent/script/build_plugins.ps1 -Config Release -BuildDir agent/build/windows-release
#   pwsh -File agent/script/build_plugins.ps1 -Jobs 8
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
    [switch] $List
)

$ErrorActionPreference = 'Stop'

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

$cmakeArgs = @('--build', $libBuild, '--config', $Config, '--target') + $selected
if ($Jobs -gt 0) {
    $cmakeArgs += @('--parallel', $Jobs)
}
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) {
    Write-Error "编译失败 (cmake 退出码 $LASTEXITCODE)"
    exit $LASTEXITCODE
}

# 产物时间戳: 确认 exec/plugins 下的 DLL 确实被替换了 (这一步最容易漏)
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
            Write-Output ("  {0,-34} {1}  {2}" -f $name, $dll.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'), $dll.Name)
        } else {
            Write-Output ("  {0,-34} (no dll)" -f $name)
        }
    }
}
exit 0
