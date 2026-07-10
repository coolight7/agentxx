#!/bin/bash

script_dir=$(cd "$(dirname "$0")" && pwd)
src_dir="$script_dir/../"
build_dir="$script_dir/../build/linux-release"

BOOST_ROOT=$(cd "$src_dir/third_party/boost-linux-build-release/" && pwd)
OPENSSL_ROOT_DIR=$(cd "$src_dir/third_party/OpenSSL-linux-build/" && pwd)

cmake -B "$build_dir" -S "$src_dir" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DBOOST_ROOT="${BOOST_ROOT}" \
    -DOPENSSL_ROOT_DIR="${OPENSSL_ROOT_DIR}" \
    -DAGENTXX_BUILD_CLIENT=ON \
    -DAGENTXX_BUILD_TEST=ON \
    -DAGENTXX_ENABLE_VECTORSCAN=OFF \
    -DAGENTXX_ENABLE_HYPERSCAN=OFF \
    -DAGENTXX_ENABLE_CODEGRAPH=OFF \
    -DAGENTXX_ENABLE_CUSTOM_CURL=ON \
    -DXX_BUILD_TYPE=RELEASE \
    -DCMAKE_BUILD_TYPE=Release

if [[ $? -ne 0 ]]; then
    echo "cmake config failed!"
    exit 1
fi

cmake --build "$build_dir" --config Release

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
strip --strip-all "$build_dir/exec/libagentxx.so"