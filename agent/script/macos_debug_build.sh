#!/bin/bash

script_dir=$(cd "$(dirname "$0")" && pwd)
src_dir="$script_dir/../"
build_dir="$script_dir/../build/macos-debug"

# ===== 依赖库自构建 (Hermetic deps) =====
# 自动构建本项目所需的 Boost/OpenSSL (不依赖 Homebrew/系统安装库):
#   - 优先使用用户/环境变量指定的已安装路径 (BOOST_ROOT / OPENSSL_ROOT_DIR)
#   - 否则若 third_party 下已有预构建目录则复用
#   - 否则用 third_party/boost、openssl-4.0.1 源码自行编译, 产物落到
#     third_party/boost-macos-build-debug|OpenSSL-macos-build
# 跳过自动构建 (缺失时直接报错): AGENTXX_SKIP_AUTO_DEPS=1 ./macos_debug_build.sh
source "$script_dir/deps/libbuild.sh"
agxxdeps_src_dir="$src_dir/third_party"

# 并行编译任务数: 默认取 4 (macOS 内存通常有限, 每个 C++26 TU 约需 1~2GB)
# 内存不足/频繁编译器崩溃(ICE) 时调小，例如:
#   AGENTXX_BUILD_PARALLEL=2 ./macos_debug_build.sh
AGENTXX_BUILD_PARALLEL="${AGENTXX_BUILD_PARALLEL:=4}"
# libbuild.sh 使用同一并行数
agxxdeps_parallel="$AGENTXX_BUILD_PARALLEL"

# ===== hyperscan (x86_64 专用) =====
# 上游 hyperscan 仅支持 x86 (SSE4.2+/AVX), Apple Silicon (arm64) 无实现;
# 未显式设置时: arm64 自动关闭, x86_64 保持开启 (需要 ragel)
if [[ -z "${AGENTXX_ENABLE_HYPERSCAN:-}" ]]; then
    if [[ "$(uname -m)" == "arm64" || "$(uname -m)" == "aarch64" ]]; then
        AGENTXX_ENABLE_HYPERSCAN=OFF
    else
        AGENTXX_ENABLE_HYPERSCAN=ON
    fi
fi

# ===== 编译环境前置检查 (工具链/内存, 失败即清晰报错) =====
# 必须先于任何依赖下载/构建执行, 避免环境不全时浪费时间下载几百 MB 源码。
# 检查: 基础 shell 工具 (下载/解压)、cmake/make、c++26 编译器、
#       下载器 (curl/wget, 二选一)。
# 注: ragel 需求由下方按 AGENTXX_ENABLE_HYPERSCAN 处理; 内存仅提示 (ICE 场景)。
_need_basic=""
for _t in tar make python3; do
    command -v "$_t" >/dev/null 2>&1 || _need_basic="$_need_basic$_t "
done
command -v curl >/dev/null 2>&1 || command -v wget >/dev/null 2>&1 || _need_basic="${_need_basic}curl|wget "
if [[ -n "$_need_basic" ]]; then
    echo "[env] 缺少基础工具: $_need_basic"
    echo "  请安装: xcode-select --install (或 brew install curl python3)"
    exit 1
fi
_need_tools=""
for _t in cmake make; do
    command -v "$_t" >/dev/null 2>&1 || _need_tools="$_need_tools$_t "
done
if [[ -n "$_need_tools" ]]; then
    echo "[env] 缺少构建工具: $_need_tools"
    echo "  请安装: brew install cmake (或从 https://cmake.org/download/ 安装)"
    exit 1
fi
# c++ 编译器版本检查 (macOS debug 要求 c++26; Apple clang 16+ 才稳定支持,
# 低版本易触发编译器内部错误 ICE)
_cxx_ok=0
if command -v clang++ >/dev/null 2>&1; then
    _clang_ver=$(clang++ --version 2>/dev/null | head -1 | grep -o -E '[0-9]+' | head -1)
    if [[ "${_clang_ver:-0}" -ge 16 ]]; then
        _cxx_ok=1
    else
        echo "[env] clang++ 版本过低 (${_clang_ver:-未知}), 推荐 Apple clang >= 16 (c++26)"
        echo "  请更新 Xcode 命令行工具: xcode-select --install"
    fi
fi
if [[ "$_cxx_ok" == "0" ]] && command -v g++ >/dev/null 2>&1; then
    _gcc_ver=$(g++ -dumpversion 2>/dev/null | cut -d. -f1)
    if [[ "${_gcc_ver:-0}" -ge 14 ]]; then
        _cxx_ok=1
    else
        echo "[env] g++ 版本过低 (${_gcc_ver:-未知}), 推荐 g++ >= 14 (c++26)"
    fi
fi
if [[ "$_cxx_ok" == "0" ]]; then
    echo "[env] 未找到可用的 c++26 编译器 (Apple clang >= 16 或 g++ >= 14), 请先安装"
    exit 1
fi
# 提示并行内存风险 (每个 c++26 TU 编译约需 1~2GB)
_mem_gb=$(( $(sysctl -n hw.memsize 2>/dev/null || echo 0) / 1024 / 1024 / 1024 ))
if [[ "$_mem_gb" -gt 0 && "${AGENTXX_BUILD_PARALLEL}" -gt "$_mem_gb" ]]; then
    echo "[env] 提示: 并行数 ${AGENTXX_BUILD_PARALLEL} > 可用内存 ${_mem_gb}GB, 若频繁 ICE 请降低:"
    echo "  AGENTXX_BUILD_PARALLEL=4 ./macos_debug_build.sh"
fi
unset _need_tools _t _cxx_ok _gcc_ver _clang_ver _mem_gb _need_basic

if [[ "${AGENTXX_SKIP_AUTO_DEPS:-0}" != "1" ]]; then
    # 未显式指定 BOOST_ROOT 且本地无预构建产物时, 自动编译 Boost (debug)
    if [[ -z "${BOOST_ROOT:-}" && ! -f "$src_dir/third_party/boost-macos-build-debug/include/boost/version.hpp" ]]; then
        agxxdeps_ensure_boost "$src_dir/third_party/boost-macos-build-debug" "debug" "boost-macos-debug" || exit 1
    fi
    # 未显式指定 OPENSSL_ROOT_DIR 且本地无预构建产物时, 自动编译 OpenSSL
    if [[ -z "${OPENSSL_ROOT_DIR:-}" && ! -f "$src_dir/third_party/OpenSSL-macos-build/include/openssl/opensslv.h" ]]; then
        agxxdeps_ensure_openssl "$src_dir/third_party/OpenSSL-macos-build" "openssl-macos" "macos" || exit 1
    fi
fi

# ===== ragel (hyperscan 语法生成器, 需要时自动构建) =====
# AGENTXX_ENABLE_HYPERSCAN=ON 时需要 ragel (macOS 默认关闭, 见上);
# 自建产物目录优先加入 PATH (产物 > 系统), 自建失败自动回退系统已装 ragel
if [[ "${AGENTXX_ENABLE_HYPERSCAN}" != "OFF" && "${AGENTXX_SKIP_AUTO_DEPS:-0}" != "1" ]]; then
    if ! command -v ragel >/dev/null 2>&1; then
        # 注意: ensure 的日志走 stdout, 这里只取最后一行 (自建产物 bin 目录,
        # 成功时输出; 系统回退/失败时输出日志行, 下方 -d 判断会过滤)
        _ragel_bin=$(agxxdeps_ensure_ragel | tail -n1) || { echo "[deps] ragel 准备失败, 如需继续请设置 AGENTXX_ENABLE_HYPERSCAN=OFF"; exit 1; }
        if [[ -n "$_ragel_bin" && -d "$_ragel_bin" ]]; then
            export PATH="$_ragel_bin:$PATH"
        fi
        unset _ragel_bin
    fi
fi

# ===== 编译加速配置 =====
# ccache: 缓存编译结果，重复/增量/切分支编译大幅提速。
# - 未安装时自动跳过 (brew install ccache)
# - 缓存目录默认 ~/.cache/ccache-agentxx，可用 CCACHE_DIR 覆盖
# - CCACHE_MAXSIZE 默认 3G (磁盘紧张可调小如 1G；磁盘充足建议调大如 30G)
# - CCACHE_BASEDIR 归一化绝对路径，项目目录移动后缓存仍可命中
if command -v ccache >/dev/null 2>&1; then
    export CCACHE_DIR="${CCACHE_DIR:-$HOME/.cache/ccache-agentxx}"
    export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-3G}"
    export CCACHE_COMPRESS="${CCACHE_COMPRESS:-1}"
    export CCACHE_BASEDIR="${CCACHE_BASEDIR:-$(cd "$src_dir" && pwd)}"
fi

if [[ -z "${BOOST_ROOT}" && -d "$src_dir/third_party/boost-macos-build-debug/" ]]; then
    BOOST_ROOT=$(cd "$src_dir/third_party/boost-macos-build-debug/" && pwd)
fi
if [[ -z "${OPENSSL_ROOT_DIR}" && -d "$src_dir/third_party/OpenSSL-macos-build/" ]]; then
    OPENSSL_ROOT_DIR=$(cd "$src_dir/third_party/OpenSSL-macos-build/" && pwd)
fi

# 默认生成器: 有 ninja 用 ninja (增量更快), 否则用 CMake 默认 (Unix Makefiles)
_generator_args=()
if command -v ninja >/dev/null 2>&1; then
    _generator_args=(-G Ninja)
fi

# ===== 读取统一版本号 (agent/VERSION) =====
_ver_file="$src_dir/VERSION"
if [[ ! -f "$_ver_file" ]]; then
    _ver_file="$src_dir/../VERSION"
fi
if [[ -f "$_ver_file" ]]; then
    AGENTXX_VERSION="${AGENTXX_VERSION:-$(head -n 1 "$_ver_file" | tr -d '[:space:]')}"
fi
echo "[version] Agentxx version: ${AGENTXX_VERSION:-0.1.0}"

cmake -B "$build_dir" -S "$src_dir" \
    "${_generator_args[@]}" \
    -DAGENTXX_VERSION="${AGENTXX_VERSION}" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DBOOST_ROOT="${BOOST_ROOT}" \
    -DOPENSSL_ROOT_DIR="${OPENSSL_ROOT_DIR}" \
    -DAGENTXX_BUILD_CLIENT=ON \
    -DAGENTXX_BUILD_TEST=ON \
    -DAGENTXX_BUILD_BENCHMARK=OFF \
    -DAGENTXX_ENABLE_HYPERSCAN="${AGENTXX_ENABLE_HYPERSCAN}" \
    -DAGENTXX_ENABLE_BOOST_PROCESS=ON \
    -DAGENTXX_ENABLE_PCH=OFF \
    -DXX_IS_RELEASE_D=0 \
    -DCMAKE_BUILD_TYPE=Debug \
    -DAGENTXX_ENABLE_MIMALLOC="${AGENTXX_ENABLE_MIMALLOC:-OFF}" \
    -DAGENTXX_MIMALLOC_LINK="${AGENTXX_MIMALLOC_LINK:-STATIC}"

if [[ $? -ne 0 ]]; then
    echo "cmake config failed!"
    exit 1
fi

# [parallel] 并行编译会大幅增加内存占用，内存不够/经常编译器崩溃ICE 可以降低或指定为 1
echo "[build] parallel jobs: ${AGENTXX_BUILD_PARALLEL}"
cmake --build "$build_dir" --config Debug --parallel "${AGENTXX_BUILD_PARALLEL}"

if [[ $? -ne 0 ]]; then
    echo "cmake build failed!"
    exit 1
fi

cmake --install "$build_dir" --config Debug

if [[ $? -ne 0 ]]; then
    echo "cmake install failed!"
    exit 1
fi
