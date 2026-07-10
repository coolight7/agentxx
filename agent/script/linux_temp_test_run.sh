#!/bin/bash

script_dir=$(cd "$(dirname "$0")" && pwd)
src_dir="$script_dir/../"
build_dir="$script_dir/../build/temp/linux-debug"

BOOST_ROOT=$(cd "$src_dir/third_party/boost-build-debug/$abi" && pwd)
OPENSSL_ROOT_DIR=$(cd "$src_dir/third_party/OpenSSL/$abi" && pwd)

cmake -B "$build_dir" -S "$src_dir" \
    -DAGENTXX_BUILD_CLIENT=ON \
    -DAGENTXX_BUILD_TEST=ON \
    -DXX_BUILD_TYPE=DEBUG -DCMAKE_BUILD_TYPE=Debug

if [[ $? -ne 0 ]]; then
    echo "cmake config failed!"
    exit 1
fi

cmake --build "$build_dir" --config debug

if [[ $? -ne 0 ]]; then
    echo "cmake build failed!"
    exit 1
fi

cmake --install "$build_dir" --config Debug

if [[ $? -ne 0 ]]; then
    echo "cmake install failed!"
    exit 1
fi

LD_LIBRARY_PATH=$build_dir/exec $build_dir/exec/test_temp
