#!/bin/bash

script_dir=$(cd "$(dirname "$0")" && pwd)
src_dir="$script_dir/../"
build_dir="$script_dir/../build/linux-release"

# ===== 编译加速配置 (与 linux_debug_build.sh 一致) =====
if command -v ccache >/dev/null 2>&1; then
    export CCACHE_DIR="${CCACHE_DIR:-$HOME/.cache/ccache-agentxx}"
    export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-3G}"
    export CCACHE_COMPRESS="${CCACHE_COMPRESS:-1}"
    export CCACHE_BASEDIR="${CCACHE_BASEDIR:-$(cd "$src_dir" && pwd)}"
fi
# 内存不足/频繁编译器崩溃(ICE) 时调小: AGENTXX_BUILD_PARALLEL=4 ./linux_release_build.sh
AGENTXX_BUILD_PARALLEL="${AGENTXX_BUILD_PARALLEL:-$(nproc)}"

BOOST_ROOT=$(cd "$src_dir/third_party/boost-linux-build-release/" && pwd)
OPENSSL_ROOT_DIR=$(cd "$src_dir/third_party/OpenSSL-linux-build/" && pwd)

cmake -B "$build_dir" -S "$src_dir" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DBOOST_ROOT="${BOOST_ROOT}" \
    -DOPENSSL_ROOT_DIR="${OPENSSL_ROOT_DIR}" \
    -DAGENTXX_BUILD_CLIENT=ON \
    -DAGENTXX_BUILD_TEST=ON \
    -DAGENTXX_BUILD_BENCHMARK=ON \
    -DAGENTXX_ENABLE_HYPERSCAN=OFF \
    -DAGENTXX_ENABLE_CODEGRAPH=OFF \
    -DAGENTXX_ENABLE_BOOST_PROCESS=ON \
    -DXX_IS_RELEASE_D=1 \
    -DCMAKE_BUILD_TYPE=Release

if [[ $? -ne 0 ]]; then
    echo "cmake config failed!"
    exit 1
fi

# [parallel] 并行编译会大幅增加内存占用，内存不够/经常编译器崩溃ICE 可以降低或指定为 1
echo "[build] parallel jobs: ${AGENTXX_BUILD_PARALLEL}"
cmake --build "$build_dir" --config Release --parallel "${AGENTXX_BUILD_PARALLEL}"

if [[ $? -ne 0 ]]; then
    echo "cmake build failed!"
    exit 1
fi

cmake --install "$build_dir" --config Release

if [[ $? -ne 0 ]]; then
    echo "cmake install failed!"
    exit 1
fi

strip --strip-all "$build_dir/exec/agentxx_cli"
strip --strip-all "$build_dir/exec/agentxx_benchmark"
strip --strip-all "$build_dir/exec/libagentxx.so"
strip --strip-all "$build_dir/exec/plugins/*/*.so"