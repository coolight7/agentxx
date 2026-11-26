# 生成 / 校验 agentxx 的 id 头（声明源 agent/schema/ids.def.json）
#
# 用法（仓库根目录下）：
#   pwsh -NoProfile -File agent/script/gen_ids.ps1           # 生成
#   pwsh -NoProfile -File agent/script/gen_ids.ps1 -Check    # 只校验（不一致时退出码 1）
#
# 三份产物都是提交物：改完 ids.def.json 必须重新生成并提交。
param(
    [switch]$Check
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Push-Location $root
try {
    $args = @('run', 'agent/tool/gen_ids.dart')
    if ($Check) { $args += '--check' }
    & dart @args
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
