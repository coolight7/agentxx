#!/usr/bin/env bash
# gate.sh —— 一键质量门禁 (计划 TST-8)
#
# 在干净环境跑一条命令即可完成本轮改动要求的检查:
#   1. 构建 (可选, --build): 只构建门禁需要的目标 (agentxx_test / 插件)
#   2. 单元测试: 全模块 fail-fast (含边界、协议往返、配置校验、UI 快照、装配快照)
#   3. 源码/构建配置边界: `boundaries` 模块 (随上面一起跑)
#   4. 插件导出符号白名单: check_plugin_exports.sh (Linux/macOS, 需 nm)
#   5. SDK 反例编译: check_sdk_negative_compile.sh (需 python3 + compile_commands.json)
#   6. 基准阈值 (可选, --bench): 跑 agentxx_benchmark, 仅报告不做阈值判定
#
# sanitizer: 本项目的 Debug 构建默认带 ASan + UBSan (AGENTXX_ENABLE_SANITIZER=ON),
# 因此"跑 Debug 测试"本身即 sanitizer 门禁; 传 --release 时跳过该结论说明。
#
# 用法:
#   agent/script/gate.sh                 # 用已有构建产物跑门禁
#   agent/script/gate.sh --build         # 先构建再跑 (Debug)
#   agent/script/gate.sh --build --bench # 附带基准
#   agent/script/gate.sh --build-dir agent/build/linux-debug
#
# 退出码: 0 = 全部通过; 1 = 有检查失败; 2 = 用法/环境错误
set -uo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../.." && pwd)"

do_build=0
do_bench=0
build_dir=""
modules=""

while [ $# -gt 0 ]; do
    case "$1" in
        --build) do_build=1; shift ;;
        --bench) do_bench=1; shift ;;
        --build-dir) build_dir="${2:-}"; shift 2 ;;
        --modules) modules="${2:-}"; shift 2 ;;
        -h|--help) sed -n '2,30p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "[gate] unknown arg: $1" >&2; exit 2 ;;
    esac
done

# 默认构建目录: Linux 用 linux-debug, macOS 用 macos-debug
if [ -z "${build_dir}" ]; then
    if [ "$(uname -s)" = "Darwin" ]; then
        build_dir="${repo_root}/agent/build/macos-debug"
    else
        build_dir="${repo_root}/agent/build/linux-debug"
    fi
fi

test_exe="${build_dir}/exec/agentxx_test"
plugins_dir="${build_dir}/exec/plugins"
test_build_dir="${build_dir}/agentxx_test_repo-prefix/src/agentxx_test_repo-build"

failures=0
declare -a summary

record() { # record <name> <status> <detail>
    local name="$1" status="$2" detail="${3:-}"
    if [ "$status" = "ok" ]; then
        summary+=("PASS  ${name}${detail:+  (${detail})}")
    elif [ "$status" = "skip" ]; then
        summary+=("SKIP  ${name}${detail:+  (${detail})}")
    else
        summary+=("FAIL  ${name}${detail:+  (${detail})}")
        failures=$((failures + 1))
    fi
}

echo "[gate] repo      : ${repo_root}"
echo "[gate] build dir : ${build_dir}"

# ---------------- 1. 构建 ----------------
if [ "${do_build}" = "1" ]; then
    echo "[gate] building agentxx_test ..."
    if ! cmake --build "${test_build_dir}" --target agentxx_test --parallel "$(nproc 2>/dev/null || echo 4)"; then
        record "build" "fail" "cmake --build agentxx_test"
        printf '\n===== gate summary =====\n'
        printf '%s\n' "${summary[@]}"
        exit 1
    fi
    record "build" "ok"
else
    record "build" "skip" "use --build"
fi

# ---------------- 2/3. 单元测试 + 边界 (fail-fast) ----------------
if [ ! -x "${test_exe}" ]; then
    record "unit tests" "fail" "missing ${test_exe}"
else
    echo "[gate] running unit tests (fail-fast) ..."
    if [ -n "${modules}" ]; then
        # 逗号分隔的模块名 → 空格分隔的参数
        IFS=',' read -r -a module_args <<<"${modules}"
        "${test_exe}" -f "${module_args[@]}"
    else
        "${test_exe}" -f
    fi
    if [ $? -eq 0 ]; then
        record "unit tests" "ok" "fail-fast, all modules"
    else
        record "unit tests" "fail" "see output above"
    fi
fi

# ---------------- 4. 插件导出符号白名单 ----------------
if [ -d "${plugins_dir}" ]; then
    echo "[gate] checking plugin export whitelist ..."
    if "${script_dir}/check_plugin_exports.sh" "${plugins_dir}"; then
        record "plugin exports" "ok"
    else
        rc=$?
        if [ "${rc}" = "2" ]; then
            record "plugin exports" "skip" "nm unavailable or no plugin library"
        else
            record "plugin exports" "fail"
        fi
    fi
else
    record "plugin exports" "skip" "no plugin dir at ${plugins_dir}"
fi

# ---------------- 5. SDK 反例编译 ----------------
if [ -f "${test_build_dir}/compile_commands.json" ] && command -v python3 >/dev/null 2>&1; then
    echo "[gate] checking SDK negative compile samples ..."
    if "${script_dir}/check_sdk_negative_compile.sh" "${test_build_dir}"; then
        record "sdk negative compile" "ok"
    else
        record "sdk negative compile" "fail"
    fi
else
    record "sdk negative compile" "skip" "no compile_commands.json (set CMAKE_EXPORT_COMPILE_COMMANDS) or python3"
fi

# ---------------- 6. 基准 (可选) ----------------
if [ "${do_bench}" = "1" ]; then
    bench_exe="${build_dir}/exec/agentxx_benchmark"
    if [ -x "${bench_exe}" ]; then
        echo "[gate] running benchmark (report only) ..."
        if "${bench_exe}"; then
            record "benchmark" "ok" "report under exec/bench"
        else
            record "benchmark" "fail"
        fi
    else
        record "benchmark" "skip" "release build required: ${bench_exe}"
    fi
else
    record "benchmark" "skip" "use --bench"
fi

printf '\n===== gate summary =====\n'
printf '%s\n' "${summary[@]}"
if [ "${failures}" -ne 0 ]; then
    echo "[gate] FAILED: ${failures} check(s) failed"
    exit 1
fi
echo "[gate] OK"
