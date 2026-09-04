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
    -DAGENTXX_ENABLE_HYPERSCAN=ON \
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
strip --strip-unneeded "$build_dir/exec/libagentxx.so"
find "$build_dir/exec/plugins/" -type f -name "*.so" -exec strip --strip-unneeded {} \;

# ===== 捆绑 C++/系统运行时到 exec (release 发布分发) =====
# - 目标: {build}/exec 与 agentxx_cli 同目录携带 libstdc++/libgcc_s 等，
#   参考 client/CMakeLists.txt `install(... DESTINATION "${AGENTXX_EXEC_INSTALL_PREFIX}")`
#   的 exec 目录布局，解压即运行，无需目标机安装同版本 GCC。
# - 跳过: AGENTXX_SKIP_BUNDLE_RUNTIME=1 ./linux_release_build.sh
if [[ "${AGENTXX_SKIP_BUNDLE_RUNTIME:-0}" != "1" ]]; then
    EXEC_DIR="$build_dir/exec"
    CXX_BIN="${CXX:-c++}"
    echo "[runtime] bundle C++ runtime libs -> $EXEC_DIR (CXX_BIN=$CXX_BIN)"
    # 1) 经编译器自报路径拷贝 (最可靠，不依赖 ldd 输出格式)
    for _lib in libstdc++.so.6 libgcc_s.so.1 libatomic.so.1 libgomp.so.1; do
        _p=""
        if command -v "$CXX_BIN" >/dev/null 2>&1; then
            _p=$("$CXX_BIN" -print-file-name="$_lib" 2>/dev/null)
        fi
        if [[ "$_p" == /* && -f "$_p" ]]; then
            cp -L -v "$_p" "$EXEC_DIR/" || true
        fi
        unset _p
    done
    unset _lib
    # 2) 兜底: 经 ldd 解析 agentxx_cli 的实际动态依赖，补拷上述四类库
    #    (处理 CXX_BIN 未指向实际编译器 / 多 libstdc++ 并存的场景)
    if command -v ldd >/dev/null 2>&1 && [[ -f "$EXEC_DIR/agentxx_cli" ]]; then
        while read -r _line; do
            # ldd 行示例: libstdc++.so.6 => /lib/x86_64-linux-gnu/libstdc++.so.6 (0x...)
            case "$_line" in
                *libstdc++.so*|*libgcc_s.so*|*libatomic.so*|*libgomp.so*)
                    _src=$(echo "$_line" | sed -n 's/.*=> \([^ ]*\).*/\1/p')
                    if [[ "$_src" == /* && -f "$_src" ]]; then
                        cp -L -v "$_src" "$EXEC_DIR/" || true
                    fi
                    unset _src
                    ;;
            esac
        done < <(ldd "$EXEC_DIR/agentxx_cli" 2>/dev/null)
        unset _line
    fi
    # 3) RPATH 指向 $ORIGIN，使 exe/.so 优先从同目录加载捆绑的运行时
    #    - exe/libagentxx.so: $ORIGIN
    #    - exec/plugins/<name>/*.so: $ORIGIN:$ORIGIN/../.. (dlopen 插件需回查 exec/)
    if command -v patchelf >/dev/null 2>&1; then
        for _f in "$EXEC_DIR/agentxx_cli" "$EXEC_DIR/agentxx_benchmark" "$EXEC_DIR/agentxx_test"; do
            if [[ -f "$_f" ]]; then
                patchelf --set-rpath '$ORIGIN' "$_f" || echo "WARNING: patchelf failed: $_f"
            fi
        done
        unset _f
        if [[ -f "$EXEC_DIR/libagentxx.so" ]]; then
            patchelf --set-rpath '$ORIGIN' "$EXEC_DIR/libagentxx.so" || true
        fi
        if [[ -d "$EXEC_DIR/plugins" ]]; then
            find "$EXEC_DIR/plugins" -type f -name "*.so" -exec patchelf --set-rpath '$ORIGIN:$ORIGIN/../..' {} \; 2>/dev/null || true
        fi
        echo "[runtime] RPATH patched to \$ORIGIN"
    else
        echo "WARNING: patchelf not found, skip RPATH patch (sudo apt-get install patchelf)"
        echo "  bundled libs still copied, but loader will prefer system libstdc++ unless LD_LIBRARY_PATH=. "
    fi
    echo "[runtime] exec libs:"
    ls -lh "$EXEC_DIR/"*.so* 2>/dev/null || true
fi