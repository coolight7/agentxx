#!/bin/bash

script_dir=$(cd "$(dirname "$0")" && pwd)
src_dir="$script_dir/../"
build_dir="$script_dir/../build/linux-debug"

# ===== 编译加速配置 =====
# ccache: 缓存编译结果，重复/增量/切分支编译大幅提速。
# - 未安装时自动跳过 (apt-get install ccache)
# - 缓存目录默认 ~/.cache/ccache-agentxx，可用 CCACHE_DIR 覆盖
# - CCACHE_MAXSIZE 默认 3G (磁盘紧张可调小如 1G；磁盘充足建议调大如 30G)
# - CCACHE_BASEDIR 归一化绝对路径，项目目录移动后缓存仍可命中
if command -v ccache >/dev/null 2>&1; then
    export CCACHE_DIR="${CCACHE_DIR:-$HOME/.cache/ccache-agentxx}"
    export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-3G}"
    export CCACHE_COMPRESS="${CCACHE_COMPRESS:-1}"
    export CCACHE_BASEDIR="${CCACHE_BASEDIR:-$(cd "$src_dir" && pwd)}"
fi

# 并行编译任务数: 默认取 4
# 内存不足/频繁编译器崩溃(ICE) 时调小，例如:
#   AGENTXX_BUILD_PARALLEL=4 ./linux_debug_build.sh
AGENTXX_BUILD_PARALLEL="${AGENTXX_BUILD_PARALLEL:=4}"

BOOST_ROOT=$(cd "$src_dir/third_party/boost-linux-build-debug/" && pwd)
OPENSSL_ROOT_DIR=$(cd "$src_dir/third_party/OpenSSL-linux-build/" && pwd)

cmake -B "$build_dir" -S "$src_dir" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DBOOST_ROOT="${BOOST_ROOT}" \
    -DOPENSSL_ROOT_DIR="${OPENSSL_ROOT_DIR}" \
    -DAGENTXX_BUILD_CLIENT=ON \
    -DAGENTXX_BUILD_TEST=ON \
    -DAGENTXX_BUILD_BENCHMARK=OFF \
    -DAGENTXX_ENABLE_HYPERSCAN=ON \
    -DAGENTXX_ENABLE_BOOST_PROCESS=ON \
    -DAGENTXX_ENABLE_PCH=OFF \
    -DXX_IS_RELEASE_D=0 \
    -DCMAKE_BUILD_TYPE=Debug

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
