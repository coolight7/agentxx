#!/bin/bash
# ==============================================================================
# Agentxx 版本管理辅助脚本
#
# 用法:
#   ./bump_version.sh               # 显示当前版本
#   ./bump_version.sh <new_version> # 更新版本为 <new_version> (如 0.2.0, 1.0.0-rc1)
# ==============================================================================

set -e

script_dir=$(cd "$(dirname "$0")" && pwd)
agent_dir="$script_dir/../"
root_dir="$script_dir/../../"

version_file="$agent_dir/VERSION"
root_version_file="$root_dir/VERSION"

# 读取当前版本
if [[ -f "$version_file" ]]; then
    current_version=$(head -n 1 "$version_file" | tr -d '[:space:]')
else
    current_version="unknown"
fi

if [[ $# -eq 0 ]]; then
    echo "Current Agentxx version: $current_version"
    echo "Usage: $0 <new_version>"
    exit 0
fi

new_version="$1"

# 基础格式校验 (必须以数字.数字 开头)
if [[ ! "$new_version" =~ ^[0-9]+\.[0-9]+ ]]; then
    echo "Error: Invalid version format '$new_version'."
    echo "Expected format like: 0.1.0, 0.2.0, 1.0.0-rc1"
    exit 1
fi

echo "Updating Agentxx version: $current_version -> $new_version"

# 写入 agent/VERSION
printf "%s\n" "$new_version" > "$version_file"
echo "  Updated: $version_file"

# 写入根目录 VERSION (若存在或属于同一个仓库)
if [[ -d "$root_dir" ]]; then
    printf "%s\n" "$new_version" > "$root_version_file"
    echo "  Updated: $root_version_file"
fi

echo ""
echo "Version successfully updated to: $new_version"
echo "Next steps for release:"
echo "  1. git add agent/VERSION VERSION"
echo "  2. git commit -m \"chore: bump version to $new_version\""
echo "  3. git tag -a \"v$new_version\" -m \"Release v$new_version\""
echo "  4. ./linux_release_build.sh (or AGENTXX_PACKAGE_RELEASE=1 ./linux_release_build.sh)"
