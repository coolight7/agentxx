#!/bin/bash
# 交叉编译安卓 arm64-v8a 动态库 (libagentxx.so)
# ===== 前置: 自动交叉构建 Boost / OpenSSL for Android (Hermetic deps) =====
# - 依赖库全部自行交叉编译 (源码: third_party/boost、openssl-4.0.1), 不依赖
#   系统/apt 安装的宿主库
# - Boost 经 third_party/boost-android (Boost-for-Android) 交叉编译, 产物按 ABI
#   落到 third_party/boost-android-build-release/<abi>
# - OpenSSL 直接经 NDK clang 交叉编译, 产物按 ABI 落到
#   third_party/OpenSSL-android-build/<abi>
# 跳过自动构建 (缺失时报错): AGENTXX_SKIP_AUTO_DEPS=1 ./cross_android_release_build.sh
# 手动指定已构建产物: BOOST_ROOT=... OPENSSL_ROOT_DIR=... ./cross_android_release_build.sh
script_dir=$(cd "$(dirname "$0")" && pwd)
src_dir="$script_dir/../"
build_dir="$script_dir/../build/android-release"
abi_list=("arm64-v8a")
if [[ -n "${ANDROID_ABI:-}" ]]; then
    abi_list=("$ANDROID_ABI")
fi

source "$script_dir/deps/libbuild.sh"
agxxdeps_src_dir="$src_dir/third_party"

# AGENTXX_BUILD_PARALLEL 默认取 4 (与 linux_debug 一致; 若内存充足可自行调大)
AGENTXX_BUILD_PARALLEL="${AGENTXX_BUILD_PARALLEL:-4}"
agxxdeps_parallel="$AGENTXX_BUILD_PARALLEL"

# Android NDK 路径，可通过环境变量 ANDROID_NDK_ROOT 设置
NDK_ROOT=""
if [[ -n "${BOOST_ROOT:-}" && -n "${OPENSSL_ROOT_DIR:-}" ]]; then
    # 用户显式指定了两者, NDK 仅用于 cmake 工具链
    NDK_ROOT="${ANDROID_NDK_ROOT:-${ANDROID_NDK_HOME:-}}"
else
    NDK_ROOT=$(agxxdeps_ndk_root) || exit 1
fi
if [ -z "$NDK_ROOT" ]; then
    echo "ERROR: 请设置 ANDROID_NDK_ROOT 环境变量为 Android NDK 根目录"
    echo "  例如: export ANDROID_NDK_ROOT=/path/to/android-ndk"
    exit 1
fi

if [ ! -f "$NDK_ROOT/build/cmake/android.toolchain.cmake" ]; then
    echo "ERROR: 在 ANDROID_NDK_ROOT 路径下找不到 CMake 工具链文件"
    echo "  期望路径: $NDK_ROOT/build/cmake/android.toolchain.cmake"
    exit 1
fi

# 需要 ragel 时: android 默认 hyperscan OFF, 无需 ragel (保持 OFF)

for abi in "${abi_list[@]}"; do
    abi_build_dir="$build_dir/$abi"
    # 目标 ABI
    ANDROID_ABI="$abi"
    # 最低 API 级别
    ANDROID_PLATFORM="${ANDROID_PLATFORM:-android-21}"
    # NDK 工具链文件
    TOOLCHAIN_FILE="$NDK_ROOT/build/cmake/android.toolchain.cmake"

    # ===== 前置: 自动交叉构建该 ABI 的 Boost / OpenSSL =====
    if [[ "${AGENTXX_SKIP_AUTO_DEPS:-0}" != "1" ]]; then
        # 未显式指定 BOOST_ROOT 且该 ABI 无预构建产物时, 自动交叉编译 Boost
        if [[ -z "${BOOST_ROOT:-}" && ! -f "$src_dir/third_party/boost-android-build-release/$abi/include/boost/version.hpp" ]]; then
            agxxdeps_ensure_boost_android "$NDK_ROOT" \
                "$src_dir/third_party/boost-android-build-release" "$abi" "release" || exit 1
        fi
        # 未显式指定 OPENSSL_ROOT_DIR 且该 ABI 无预构建产物时, 自动交叉编译 OpenSSL
        if [[ -z "${OPENSSL_ROOT_DIR:-}" && ! -f "$src_dir/third_party/OpenSSL-android-build/$abi/include/openssl/opensslv.h" ]]; then
            agxxdeps_ensure_openssl_android "$NDK_ROOT" \
                "$src_dir/third_party/OpenSSL-android-build" "$abi" || exit 1
        fi
    fi
    target_boost_root="${BOOST_ROOT:-}"
    if [[ -z "$target_boost_root" ]]; then
        target_boost_root=$(cd "$src_dir/third_party/boost-android-build-release/$abi" && pwd)
    fi
    target_openssl_root_dir="${OPENSSL_ROOT_DIR:-}"
    if [[ -z "$target_openssl_root_dir" ]]; then
        target_openssl_root_dir=$(cd "$src_dir/third_party/OpenSSL-android-build/$abi" && pwd)
    fi
    # ===== 编译环境前置检查 =====
    _env_fail=0
    for _t in cmake make git; do
        command -v "$_t" >/dev/null 2>&1 || { echo "[env] 缺少构建工具: $_t"; _env_fail=1; }
    done
    command -v ninja >/dev/null 2>&1 || command -v ninja-build >/dev/null 2>&1 || { echo "[env] 缺少 ninja (sudo apt-get install -y ninja-build)"; _env_fail=1; }
    # 宿主 g++: 仅用于编译 b2 构建引擎/生成工具, 版本不做强制要求
    if [[ "$_env_fail" == "1" ]]; then
        echo "  请安装: sudo apt-get install -y cmake ninja-build make git"
        exit 1
    fi
    unset _env_fail _t
    for _depdir in "$target_boost_root" "$target_openssl_root_dir"; do
        if [[ ! -d "$_depdir" ]]; then
            echo "ERROR: 依赖目录不存在: $_depdir"
            echo "  请删除后重试让脚本自动交叉编译, 或设置 AGENTXX_SKIP_AUTO_DEPS=1 跳过"
            exit 1
        fi
    done
    unset _depdir

    echo "============================================"
    echo "  Android 交叉编译动态库"
    echo "============================================"
    echo "  ANDROID_NDK_ROOT:  $NDK_ROOT"
    echo "  ANDROID_ABI:       $ANDROID_ABI"
    echo "  ANDROID_PLATFORM:  $ANDROID_PLATFORM"
    echo "  Build Dir:         $abi_build_dir"
    echo "  BOOST_ROOT:        $target_boost_root"
    echo "  OPENSSL_ROOT_DIR:  $target_openssl_root_dir"
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
        -DBOOST_ROOT="${target_boost_root}" \
        -DOPENSSL_ROOT_DIR="${target_openssl_root_dir}" \
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

    echo "[build] parallel jobs: ${AGENTXX_BUILD_PARALLEL}"
    cmake --build "$abi_build_dir" --config Release --parallel "${AGENTXX_BUILD_PARALLEL}"

    if [[ $? -ne 0 ]]; then
        echo "cmake build failed!"
        exit 1
    fi

    cmake --install "$abi_build_dir" --config Release

    if [[ $? -ne 0 ]]; then
        echo "cmake install failed!"
        exit 1
    fi

    # ===== 复制 NDK C++ 运行时到 exec (release 发布分发) =====
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
            _cand="$NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/$_ndk_triple/$_ndk_api/libc++_shared.so"
            if [[ -f "$_cand" ]]; then
                _cxx_shared="$_cand"
            else
                _cxx_shared=$(find "$NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/$_ndk_triple" -maxdepth 2 -name "libc++_shared.so" 2>/dev/null | sort -V | tail -n1)
            fi
        fi
        if [[ -n "$_cxx_shared" && -f "$_cxx_shared" ]]; then
            echo "[runtime] bundle libc++_shared.so -> $abi_build_dir/exec/ ($_cxx_shared)"
            cp -v "$_cxx_shared" "$abi_build_dir/exec/" || true
        else
            echo "WARNING: libc++_shared.so not found for $ANDROID_ABI (triple=$_ndk_triple api=$_ndk_api)"
        fi
        unset _ndk_triple _ndk_api _cxx_shared _cand
    fi

    # 对 exec 目录下所有产物动态库进行 strip
    find "$abi_build_dir/exec" -type f -name "*.so" -exec "$NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-unneeded {} +

    echo "[runtime] exec libs:"
    ls -lh "$abi_build_dir/exec/"*.so* 2>/dev/null || true

    echo ""
    echo "============================================"
    echo "  编译完成!"
    echo "  动态库目录: $abi_build_dir/exec/"
    echo "  输出目录:   $abi_build_dir/agentxx-project-install/"
    echo "============================================"
done
