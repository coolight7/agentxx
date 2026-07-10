#!/bin/bash
# 交叉编译安卓 arm64-v8a 动态库 (libagentxx.so)

script_dir=$(cd "$(dirname "$0")" && pwd)
src_dir="$script_dir/../"
build_dir="$script_dir/../build/android-release"
abi_list=("arm64-v8a")

# Android NDK 路径，可通过环境变量 ANDROID_NDK_ROOT 设置
if [ -z "$ANDROID_NDK_ROOT" ]; then
    echo "ERROR: 请设置 ANDROID_NDK_ROOT 环境变量为 Android NDK 根目录"
    echo "  例如: export ANDROID_NDK_ROOT=/path/to/android-ndk"
    exit 1
fi

if [ ! -f "$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake" ]; then
    echo "ERROR: 在 ANDROID_NDK_ROOT 路径下找不到 CMake 工具链文件"
    echo "  期望路径: $ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake"
    exit 1
fi

for abi in ${abi_list[@]}; do
    abi_build_dir="$build_dir/$abi"
    # 目标 ABI，默认 arm64-v8a
    ANDROID_ABI="${ANDROID_ABI:-$abi}"
    # 最低 API 级别
    ANDROID_PLATFORM="${ANDROID_PLATFORM:-android-21}"
    # NDK 工具链文件
    TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake"
    BOOST_ROOT=$(cd "$src_dir/third_party/boost-android-build-release/$abi" && pwd)
    OPENSSL_ROOT_DIR=$(cd "$src_dir/third_party/OpenSSL-android/$abi" && pwd)

    echo "============================================"
    echo "  Android 交叉编译动态库"
    echo "============================================"
    echo "  ANDROID_NDK_ROOT:  $ANDROID_NDK_ROOT"
    echo "  ANDROID_ABI:       $ANDROID_ABI"
    echo "  ANDROID_PLATFORM:  $ANDROID_PLATFORM"
    echo "  Build Dir:         $abi_build_dir"
    echo "  BOOST_ROOT:        $BOOST_ROOT"
    echo "  OPENSSL_ROOT_DIR:  $OPENSSL_ROOT_DIR"
    echo "============================================"

    cmake -B "$abi_build_dir" -S "$src_dir" \
        -DCMAKE_SYSTEM_NAME="Android" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
        -DANDROID_ABI="$ANDROID_ABI" \
        -DANDROID_PLATFORM="$ANDROID_PLATFORM" \
        -DANDROID_STL=c++_shared \
        -DCMAKE_BUILD_TYPE=Release \
        -DBOOST_ROOT="${BOOST_ROOT}" \
        -DOPENSSL_ROOT_DIR="${OPENSSL_ROOT_DIR}" \
        -DXX_BUILD_TYPE=RELEASE \
        -DAGENTXX_BUILD_CLIENT=OFF \
        -DAGENTXX_BUILD_TEST=OFF \
        -DAGENTXX_ENABLE_VECTORSCAN=OFF \
        -DAGENTXX_ENABLE_HYPERSCAN=OFF \
        -DAGENTXX_ENABLE_CODEGRAPH=OFF \
        -DAGENTXX_ENABLE_CUSTOM_CURL=ON \
        -DAGENTXX_ENABLE_BOOST_PROCESS=OFF \
        -G Ninja

    if [[ $? -ne 0 ]]; then
        echo "cmake config failed!"
        exit 1
    fi

    cmake --build "$abi_build_dir" --config Release

    if [[ $? -ne 0 ]]; then
        echo "cmake build failed!"
        exit 1
    fi

    cmake --install "$abi_build_dir" --config Release

    if [[ $? -ne 0 ]]; then
        echo "cmake install failed!"
        exit 1
    fi

    $ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip --strip-all "$abi_build_dir/exec/libagentxx.so"

    echo ""
    echo "============================================"
    echo "  编译完成!"
    echo "  输出目录: $abi_build_dir/agentxx-project-install/"
    echo "============================================"
done