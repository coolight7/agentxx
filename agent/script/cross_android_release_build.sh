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
    OPENSSL_ROOT_DIR=$(cd "$src_dir/third_party/OpenSSL-android-build/$abi" && pwd)

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

    # 启用插件编译: 插件动态库输出到 {build}/exec/plugins/
    # - AGENTXX_ENABLE_HYPERSCAN 在安卓未配置交叉编译依赖, 保持关闭 (插件会自动跳过对应条件链接)
    # - AGENTXX_ENABLE_PLUGIN_CODEGRAPH 开启 codegraph 插件 (顶层自动构建其依赖 codegraph-cpp/sqlite3)
    # 注意: 下方 cmake 参数使用续行符, 注释不能插入续行中间
    cmake -B "$abi_build_dir" -S "$src_dir" \
        -DCMAKE_SYSTEM_NAME="Android" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
        -DANDROID_ABI="$ANDROID_ABI" \
        -DANDROID_PLATFORM="$ANDROID_PLATFORM" \
        -DANDROID_STL=c++_shared \
        -DCMAKE_BUILD_TYPE=Release \
        -DBOOST_ROOT="${BOOST_ROOT}" \
        -DOPENSSL_ROOT_DIR="${OPENSSL_ROOT_DIR}" \
        -DXX_IS_RELEASE_D=1 \
        -DAGENTXX_BUILD_CLIENT=OFF \
        -DAGENTXX_BUILD_TEST=OFF \
        -DAGENTXX_ENABLE_HYPERSCAN=OFF \
        -DAGENTXX_ENABLE_BOOST_PROCESS=ON \
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

    "$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-unneeded "$abi_build_dir/exec/libagentxx.so"
    find "$abi_build_dir/exec/plugins/" -type f -name "*.so" -exec "$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-unneeded {} \;

    # ===== 捆绑 NDK C++ 运行时到 exec (release 发布分发) =====
    # - 本工程 -DANDROID_STL=c++_shared，libagentxx.so 动态依赖 libc++_shared.so，
    #   参考 client/CMakeLists.txt `install(... DESTINATION "${AGENTXX_EXEC_INSTALL_PREFIX}")`
    #   的 exec 目录布局：两者同目录分发，APK/JNI 加载时无需系统提供匹配版本。
    # - 跳过: AGENTXX_SKIP_BUNDLE_RUNTIME=1 ./cross_android_release_build.sh
    if [[ "${AGENTXX_SKIP_BUNDLE_RUNTIME:-0}" != "1" ]]; then
        # ANDROID_ABI(cmake 官方名) 与 NDK sysroot 目录名(llvm 三元组) 的映射
        case "$ANDROID_ABI" in
            arm64-v8a)   _ndk_triple="aarch64-linux-android" ;;
            armeabi-v7a) _ndk_triple="arm-linux-androideabi" ;;
            x86_64)      _ndk_triple="x86_64-linux-android" ;;
            x86)         _ndk_triple="i686-linux-android" ;;
            *)           _ndk_triple="" ;;
        esac
        _ndk_api="${ANDROID_PLATFORM##android-}"
        _cxx_shared=""
        if [[ -n "$_ndk_triple" && -n "$_ndk_api" ]]; then
            # libc++_shared.so 按 API 分级存放，优先精确 API，回退任意同 triple 版本
            _cand="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/$_ndk_triple/$_ndk_api/libc++_shared.so"
            if [[ -f "$_cand" ]]; then
                _cxx_shared="$_cand"
            else
                _cxx_shared=$(find "$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/$_ndk_triple" -maxdepth 2 -name "libc++_shared.so" 2>/dev/null | sort -V | tail -n1)
            fi
        fi
        if [[ -n "$_cxx_shared" && -f "$_cxx_shared" ]]; then
            echo "[runtime] bundle libc++_shared.so -> $abi_build_dir/exec/ ($_cxx_shared)"
            cp -v "$_cxx_shared" "$abi_build_dir/exec/" || true
            "$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-unneeded "$abi_build_dir/exec/libc++_shared.so" 2>/dev/null || true
        else
            echo "WARNING: libc++_shared.so not found for $ANDROID_ABI (triple=$_ndk_triple api=$_ndk_api)"
        fi
        unset _ndk_triple _ndk_api _cxx_shared _cand
        echo "[runtime] exec libs:"
        ls -lh "$abi_build_dir/exec/"*.so* 2>/dev/null || true
    fi

    echo ""
    echo "============================================"
    echo "  编译完成!"
    echo "  输出目录: $abi_build_dir/agentxx-project-install/"
    echo "============================================"
done