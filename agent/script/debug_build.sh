#!/bin/bash

script_dir=$(cd "$(dirname "$0")" && pwd)
src_dir="$script_dir/../"
build_dir="$script_dir/../build/debug"

cmake -B "$build_dir" -S "$src_dir" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DAGENTXX_BUILD_CLIENT=ON \
    -DAGENTXX_BUILD_TEST=ON \
    -DAGENTXX_ENABLE_VECTORSCAN=ON \
    -DAGENTXX_ENABLE_HYPERSCAN=OFF \
    -DAGENTXX_ENABLE_CODEGRAPH=ON \
    -DAGENTXX_ENABLE_CUSTOM_CURL=ON \
    -DXX_BUILD_TYPE=DEBUG \
    -DCMAKE_BUILD_TYPE=Debug

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
