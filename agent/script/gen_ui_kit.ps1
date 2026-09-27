# 生成 agentxx 扩展 kit（插件界面描述层的 C++ 头与 JS 文件）
#
# 用法（需要 Dart SDK）：
#   pwsh -NoProfile -File agent/script/gen_ui_kit.ps1
#
# 说明：
# - 定义文件是 `agent/schema/agentxx-ui-kit.def.json`（合并库里子模块
#   `agent/third_party/cxx_pluginxx_ui` 的基础 kit：基础组件会并进来，同名组件以本文件为准）；
# - 生成器属于描述层库，必须在库根目录下运行（它按相对路径读 schema/ui.def.json）；
# - 产物（生成物，改 kit 后重跑本脚本并一起提交）：
#     agent/lib/include/agentxx/plugin/api/agentxx_ui_kit.g.h   插件 SDK 的 C++ kit（命名空间 agentxx::ui::kit）
#     agent/js/agentxx_ui_kit.js                                JS 插件用的 kit（全局 pluginxx.ui.kit）
#     docs/zh-cn/design/agentxx-ui-kit.md                       生成出来的组件说明
param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $Root
$libRoot = Join-Path $Root 'third_party/cxx_pluginxx_ui'
$defPath = Join-Path $Root 'schema/agentxx-ui-kit.def.json'
$sdkDir = Join-Path $Root 'lib/include/agentxx/plugin/api'
$jsDir = Join-Path $Root 'js'
$docsDir = Join-Path $repoRoot 'docs/zh-cn/design'

foreach ($p in @($libRoot, $defPath)) {
    if (-not (Test-Path $p)) {
        throw "找不到 $p （子模块没初始化？跑一次 git submodule update --init --recursive）"
    }
}
if (-not (Get-Command dart -ErrorAction SilentlyContinue)) {
    throw 'PATH 里没有 dart（描述层库的生成器是 Dart 写的）'
}

# 生成器只会往一个 --out 目录里写，先落到临时目录再按本仓库的目录结构摆放
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("agentxx-ui-kit-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
try {
    Push-Location $libRoot
    try {
        & dart run tools/gen_ui.dart --ext-kit $defPath --prefix agentxx `
            --namespace 'agentxx::ui::kit' --targets cpp,js --out $tmp
        if ($LASTEXITCODE -ne 0) { throw "生成器退出码 $LASTEXITCODE" }
    } finally {
        Pop-Location
    }

    New-Item -ItemType Directory -Force -Path $sdkDir, $jsDir, $docsDir | Out-Null
    Copy-Item (Join-Path $tmp 'agentxx_ui_kit.g.h') (Join-Path $sdkDir 'agentxx_ui_kit.g.h') -Force
    Copy-Item (Join-Path $tmp 'agentxx_ui_kit.js') (Join-Path $jsDir 'agentxx_ui_kit.js') -Force
    Copy-Item (Join-Path $tmp 'agentxx_ui_kit.md') (Join-Path $docsDir 'agentxx-ui-kit.md') -Force

    Write-Output '生成完成：'
    Write-Output ("  " + (Join-Path $sdkDir 'agentxx_ui_kit.g.h'))
    Write-Output ("  " + (Join-Path $jsDir 'agentxx_ui_kit.js'))
    Write-Output ("  " + (Join-Path $docsDir 'agentxx-ui-kit.md'))
    Write-Output ''
    Write-Output '提示：JS 插件把这一个文件贴在 plugin.js 之前（见 agent/js/README.md）'
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
