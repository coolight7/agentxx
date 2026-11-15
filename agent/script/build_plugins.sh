#!/usr/bin/env bash
# build_plugins.sh —— 只重新编译插件动态库 (Linux / macOS 版)
#
# 背景: 插件的动态库目标属于 lib 子工程 (plugins/ 经 add_subdirectory 进 lib 的
# 构建树), 顶层 `cmake --build agent/build/linux-debug` 只会编译它们的 .obj,
# 不会产出 exec/plugins/<插件名>/*.dll(.so)。改了插件源码 / 抬了插件 API 代次之后,
# 必须对**内层 lib 构建目录**显式点名插件目标, 否则 exec/plugins 里还是旧的库
# (运行时会报 "插件 API 版本过低")。
#
# 本脚本就是这条命令的封装: 自动定位内层构建目录、列出可用插件目标、逐个编译并打印
# 每个目标的耗时与产物时间戳。
#
# 为什么提供两种模式:
# - 指定了插件名 (或 --per-target / --keep-going): 逐个目标调用 ——
#   每目标一行进度 (`(3/25) agentxx_math ok 10s`), 失败直接点名, 长构建不会看起来像卡住
# - 不指定目标 (编全部): 一次把全部目标交给 cmake —— 它们由构建系统并行处理,
#   总耗时最短; 失败时用 outputs 里的 "(未重新链接)" 定位未编成的插件
#
# 用法:
#   agent/script/build_plugins.sh                        # 编译全部插件 (debug)
#   agent/script/build_plugins.sh --list                 # 只列出可编译的插件目标
#   agent/script/build_plugins.sh agentxx_filesystem agentxx_math
#   agent/script/build_plugins.sh --config Release --build-dir agent/build/linux-release
#   agent/script/build_plugins.sh --jobs 8               # 每个目标内并行 8 个编译进程
#   agent/script/build_plugins.sh --per-target           # 编全部也逐个目标 (看进度)
#   agent/script/build_plugins.sh --keep-going           # 某个目标失败后继续编剩下的
#
# 退出码: 0 全部成功 / 其它 = cmake 的退出码 / 3 = 同一构建目录已有构建在跑
# 耗时: 全部插件大约几分钟 (取决于机器与是否命中增量), 单个插件通常 10~90 秒。
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
per_target=0
keep_going=0
targets_given=0
targets=()

# 文件的修改时间 (秒): GNU stat 与 BSD(macOS) stat 的参数不同, 依次尝试
file_mtime_epoch() {
    stat -c %Y "$1" 2>/dev/null && return 0
    stat -f %m "$1" 2>/dev/null && return 0
    echo 0
}

# 秒数格式化成本地时间: GNU date 用 -d @N, BSD(macOS) date 用 -r N
epoch_to_stamp() {
    date -d "@$1" '+%Y-%m-%d %H:%M:%S' 2>/dev/null && return 0
    date -r "$1" '+%Y-%m-%d %H:%M:%S' 2>/dev/null && return 0
    echo '?'
}

usage() {
    sed -n '2,32p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while (($# > 0)); do
    case "$1" in
        --build-dir)   build_dir="${2:?--build-dir 需要参数}"; shift 2 ;;
        --config)      config="${2:?--config 需要参数}"; shift 2 ;;
        --jobs|-j)     jobs="${2:?--jobs 需要参数}"; shift 2 ;;
        --list)        list_only=1; shift ;;
        --per-target)  per_target=1; shift ;;
        --keep-going)  keep_going=1; shift ;;
        -h|--help)     usage; exit 0 ;;
        -*)            echo "[build_plugins] 未知参数: $1" >&2; usage >&2; exit 2 ;;
        *)             targets+=("$1"); targets_given=1; shift ;;
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
echo "[build_plugins] jobs      : $([[ ${jobs} -gt 0 ]] && echo "${jobs}" || echo default)"

# 同一构建目录只允许一个插件构建在跑: 两个构建同时在一个构建树上干活会互等,
# 看起来就是"卡住" (上一轮被中断的构建还在后台跑时最容易遇到)。
# 用 mkdir 做原子加锁 (POSIX 上到处都有, 不依赖 flock)。
lock_dir="${lib_build}/.build_plugins.lock.d"
acquire_lock() {
    if mkdir "${lock_dir}" 2>/dev/null; then
        echo "$$" > "${lock_dir}/pid"
        date '+%Y-%m-%d %H:%M:%S' > "${lock_dir}/started"
        return 0
    fi
    other_pid="$(cat "${lock_dir}/pid" 2>/dev/null || true)"
    if [[ -n "${other_pid}" ]] && kill -0 "${other_pid}" 2>/dev/null; then
        echo "[build_plugins] 已有构建在跑: pid=${other_pid}" >&2
        echo "[build_plugins] 开始于: $(cat "${lock_dir}/started" 2>/dev/null || echo '?')" >&2
        echo "[build_plugins] 同时跑两个会在同一构建树上互等, 看起来像卡死 —— 等它结束再跑。" >&2
        echo "[build_plugins] 若确认是上次被中断留下的进程: 结束它, 或删掉 ${lock_dir} 后再跑。" >&2
        return 1
    fi
    echo "[build_plugins] 清理上一轮遗留的锁 (pid 已不在)"
    rm -rf "${lock_dir}"
    mkdir "${lock_dir}" 2>/dev/null || return 1
    echo "$$" > "${lock_dir}/pid"
    date '+%Y-%m-%d %H:%M:%S' > "${lock_dir}/started"
    return 0
}
if ! acquire_lock; then
    exit 3
fi
trap 'rm -rf "${lock_dir}"' EXIT INT TERM

failed=()
exit_code=0
overall_start=$(date +%s)
total=${#targets[@]}
index=0
# 逐个目标 vs 一次传全部: 见头部注释
use_per_target=0
if ((per_target || keep_going || targets_given)); then
    use_per_target=1
fi
single_call=0
if ((use_per_target == 0)); then
    single_call=1
    cmake_args=(--build "${lib_build}" --config "${config}" --target)
    cmake_args+=("${targets[@]}")
    if ((jobs > 0)); then
        cmake_args+=(--parallel "${jobs}")
    fi
    echo "[build_plugins] building ${total} targets in one cmake call ..."
    target_start=$(date +%s)
    if cmake "${cmake_args[@]}"; then
        echo "[build_plugins] all targets ok ($(($(date +%s) - target_start))s)"
    else
        exit_code=$?
        echo "[build_plugins] build FAILED (exit ${exit_code}, $(($(date +%s) - target_start))s)" >&2
    fi
else
    for name in "${targets[@]}"; do
        index=$((index + 1))
        cmake_args=(--build "${lib_build}" --config "${config}" --target "${name}")
        if ((jobs > 0)); then
            cmake_args+=(--parallel "${jobs}")
        fi
        echo "[build_plugins] (${index}/${total}) ${name} ..."
        target_start=$(date +%s)
        if cmake "${cmake_args[@]}"; then
            echo "[build_plugins] (${index}/${total}) ${name} ok ($(($(date +%s) - target_start))s)"
        else
            rc=$?
            failed+=("${name}")
            if ((exit_code == 0)); then
                exit_code=${rc}
            fi
            echo "[build_plugins] (${index}/${total}) ${name} FAILED (exit ${rc}, $(($(date +%s) - target_start))s)" >&2
            if ((keep_going == 0)); then
                break
            fi
        fi
    done
fi

# 产物时间戳: 确认 exec/plugins 下的库确实被替换了 (这一步最容易漏)
# - 修改时间早于本次开始时间的目标标 "(未重新链接)": 增量构建没有重新链接它
exec_plugins="${build_dir}/exec/plugins"
if [[ -d "${exec_plugins}" ]]; then
    echo "[build_plugins] outputs:"
    for name in "${targets[@]}"; do
        dir="${exec_plugins}/${name}"
        if [[ ! -d "${dir}" ]]; then
            printf '  %-34s (no output dir)\n' "${name}"
            continue
        fi
        newest=""
        newest_epoch=0
        while IFS= read -r lib; do
            lib_epoch="$(file_mtime_epoch "${lib}")"
            if ((lib_epoch > newest_epoch)); then
                newest="${lib}"
                newest_epoch=${lib_epoch}
            fi
        done < <(find "${dir}" -type f \( -name '*.so' -o -name '*.dylib' \) 2>/dev/null)
        if [[ -n "${newest}" ]]; then
            stale=""
            if ((newest_epoch < overall_start)); then
                stale="  (未重新链接)"
            fi
            printf '  %-34s %s  %s%s\n' "${name}" "$(epoch_to_stamp "${newest_epoch}")" "$(basename "${newest}")" "${stale}"
        else
            printf '  %-34s (no output lib)\n' "${name}"
        fi
    done
fi

if ((${#failed[@]} > 0)); then
    echo "[build_plugins] failed targets: ${failed[*]}"
elif ((single_call && exit_code != 0)); then
    echo "[build_plugins] build failed: 看 outputs 里标了 \"(未重新链接)\" 的目标定位未编成的插件"
fi
if ((single_call)); then
    if ((exit_code == 0)); then ok_count=${total}; else ok_count=0; fi
else
    ok_count=$((total - ${#failed[@]}))
fi
echo "[build_plugins] done in $(( $(date +%s) - overall_start ))s (${ok_count}/${total} targets ok)"
exit "${exit_code}"
