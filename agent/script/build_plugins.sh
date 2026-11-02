#!/usr/bin/env bash
# build_plugins.sh —— 只重新编译插件动态库 (Linux / macOS 版)
#
# 背景: 插件的动态库目标属于 lib 子工程 (plugins/ 经 add_subdirectory 进 lib 的
# 构建树), 顶层 `cmake --build agent/build/linux-debug` 只会编译它们的 .obj,
# 不会产出 exec/plugins/<插件名>/*.dll(.so)。改了插件源码 / 抬了插件 API 代次之后,
# 必须对**内层 lib 构建目录**显式点名插件目标, 否则 exec/plugins 里还是旧的库
# (运行时会报 "插件 API 版本过低")。
#
# 本脚本就是这条命令的封装: 自动定位内层构建目录、列出可用插件目标、编译并打印
# 产物的时间戳。
#
# 用法:
#   agent/script/build_plugins.sh                        # 编译全部插件 (debug)
#   agent/script/build_plugins.sh --list                 # 只列出可编译的插件目标
#   agent/script/build_plugins.sh agentxx_filesystem agentxx_math
#   agent/script/build_plugins.sh --config Release --build-dir agent/build/linux-release
#   agent/script/build_plugins.sh --jobs 8
#
# 前置条件: 先完成过一次完整构建 (script/linux_debug_build.sh 等), 内层 lib 构建
# 目录已配置好; 本脚本只做增量编译, 不重新配置 CMake。

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src_dir="$(cd "${script_dir}/.." && pwd)"

case "$(uname -s)" in
    Darwin) default_build_dir="${src_dir}/build/macos-debug" ;;
    *)      default_build_dir="${src_dir}/build/linux-debug" ;;
esac

build_dir="${default_build_dir}"
config="Debug"
jobs=0
list_only=0
targets=()

usage() {
    sed -n '2,26p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while (($# > 0)); do
    case "$1" in
        --build-dir) build_dir="${2:?--build-dir 需要参数}"; shift 2 ;;
        --config)    config="${2:?--config 需要参数}"; shift 2 ;;
        --jobs|-j)   jobs="${2:?--jobs 需要参数}"; shift 2 ;;
        --list)      list_only=1; shift ;;
        -h|--help)   usage; exit 0 ;;
        -*)          echo "[build_plugins] 未知参数: $1" >&2; usage >&2; exit 2 ;;
        *)           targets+=("$1"); shift ;;
    esac
done

if [[ ! -d "${build_dir}" ]]; then
    echo "[build_plugins] 构建目录不存在: ${build_dir}" >&2
    echo "[build_plugins] 先跑一次完整构建 (script/linux_debug_build.sh)，再回来编插件。" >&2
    exit 1
fi

# 内层 lib 构建目录: CMakeCache.txt 在这里, 插件目标也在它的构建树上
lib_build="${build_dir}/agentxx_lib_repo-prefix/src/agentxx_lib_repo-build"
if [[ ! -f "${lib_build}/CMakeCache.txt" ]]; then
    echo "[build_plugins] 找不到已配置的内层构建目录: ${lib_build}" >&2
    echo "[build_plugins] 先跑一次完整构建; 本脚本只做增量编译, 不重新配置 CMake。" >&2
    exit 1
fi

# 可编译的插件目标: 插件目录里有 CMakeLists.txt 且构建树里存在同名目标
# (Ninja/Makefiles 都能用 `cmake --build --target help` 列出目标)
help_out="$(cmake --build "${lib_build}" --target help 2>/dev/null || true)"
if [[ -z "${help_out}" ]]; then
    echo "[build_plugins] 无法列出目标 (cmake --build --target help 无输出): ${lib_build}" >&2
    exit 1
fi

available=()
for dir in "${src_dir}"/plugins/*/; do
    name="$(basename "${dir}")"
    [[ -f "${dir}CMakeLists.txt" ]] || continue
    if grep -qE "^\.\.\. +${name}$" <<< "${help_out}"; then
        available+=("${name}")
    fi
done

if ((${#available[@]} == 0)); then
    echo "[build_plugins] 在 ${lib_build} 里没找到任何插件目标" >&2
    echo "[build_plugins] 可能这份构建配置里插件被关掉了 (AGENTXX_PLUGIN_BUILTIN_LIST)" >&2
    exit 1
fi

if ((list_only)); then
    printf '%s\n' "${available[@]}"
    exit 0
fi

if ((${#targets[@]} == 0)); then
    targets=("${available[@]}")
else
    for t in "${targets[@]}"; do
        found=0
        for a in "${available[@]}"; do
            [[ "${a}" == "${t}" ]] && found=1 && break
        done
        if ((found == 0)); then
            echo "[build_plugins] 未知的插件目标: ${t}" >&2
            echo "[build_plugins] 可用目标: ${available[*]}" >&2
            exit 1
        fi
    done
fi

echo "[build_plugins] build dir : ${lib_build}"
echo "[build_plugins] config    : ${config}"
echo "[build_plugins] targets   : ${targets[*]}"

cmake_args=(--build "${lib_build}" --config "${config}" --target "${targets[@]}")
if ((jobs > 0)); then
    cmake_args+=(--parallel "${jobs}")
fi
cmake "${cmake_args[@]}"

# 产物时间戳: 确认 exec/plugins 下的库确实被替换了 (这一步最容易漏)
exec_plugins="${build_dir}/exec/plugins"
if [[ -d "${exec_plugins}" ]]; then
    echo "[build_plugins] outputs:"
    for name in "${targets[@]}"; do
        newest="$(find "${exec_plugins}/${name}" -name '*.so' -o -name '*.dylib' 2>/dev/null |
            xargs -r ls -t 2>/dev/null | head -n 1 || true)"
        if [[ -n "${newest}" ]]; then
            printf '  %-34s %s  %s\n' "${name}" "$(date -r "${newest}" '+%Y-%m-%d %H:%M:%S' 2>/dev/null || true)" "$(basename "${newest}")"
        else
            printf '  %-34s (no output lib)\n' "${name}"
        fi
    done
fi
