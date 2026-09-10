#!/usr/bin/env bash
# 插件导出符号白名单校验 (Reset-v1 / R6)
#
# 契约 (见 docs/zh-cn/design/plugins.md §5 与各插件 CMakeLists):
# - 插件动态库只导出宿主按名查找的入口符号:
#   `agentxx_plugin_agent_{get_info,create,start,stop,destroy}` 与
#   `agentxx_plugin_client_{get_info,create,start,stop,destroy}`;
# - 第三方静态依赖与复用工具库的符号必须隐藏 (ELF: -fvisibility=hidden +
#   version script; macOS: -exported_symbols_list; MSVC: 仅 dllexport)。
#
# 用法:
#   ./agent/script/check_plugin_exports.sh [plugin-dir]
# 默认目录: agent/build/linux-debug/exec/plugins
# 退出码: 0 = 全部通过; 1 = 存在白名单外的导出符号; 2 = 用法/环境错误
set -u

plugin_dir="${1:-agent/build/linux-debug/exec/plugins}"

if [ ! -d "$plugin_dir" ]; then
    echo "[check_plugin_exports] plugin dir not found: $plugin_dir" >&2
    exit 2
fi

if ! command -v nm >/dev/null 2>&1; then
    echo "[check_plugin_exports] nm not available; skip" >&2
    exit 2
fi

allowed_re='^agentxx_plugin_(agent|client)_(get_info|create|start|stop|destroy)$'

checked=0
failed=0
while IFS= read -r so; do
    [ -n "$so" ] || continue
    checked=$((checked + 1))
    # macOS 需要 -gU; Linux 用 -D (动态符号表)
    if ! symbols=$(nm -D --defined-only "$so" 2>/dev/null); then
        symbols=$(nm -gU "$so" 2>/dev/null) || true
    fi
    unexpected=$(printf '%s\n' "$symbols" | awk '{print $NF}' | grep -v '^$' | grep -v -E "$allowed_re" || true)
    if [ -n "$unexpected" ]; then
        failed=$((failed + 1))
        echo "[check_plugin_exports] FAIL $(basename "$so") exports non-entry symbols:" >&2
        printf '%s\n' "$unexpected" | sed 's/^/    /' >&2
    fi
done < <(find "$plugin_dir" -name '*.so' -o -name '*.dylib' 2>/dev/null | sort)

if [ "$checked" -eq 0 ]; then
    echo "[check_plugin_exports] no plugin library found under $plugin_dir" >&2
    exit 2
fi

if [ "$failed" -ne 0 ]; then
    echo "[check_plugin_exports] $failed/$checked plugin(s) failed" >&2
    exit 1
fi

echo "[check_plugin_exports] OK: $checked plugin libraries export only entry symbols"
