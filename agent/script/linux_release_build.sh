#!/bin/bash

script_dir=$(cd "$(dirname "$0")" && pwd)
src_dir="$script_dir/../"
build_dir="$script_dir/../build/linux-release"

# ===== 依赖库自构建 (Hermetic deps) =====
# 自动构建本项目所需的 Boost/OpenSSL (不依赖系统 apt 库):
#   - 优先使用用户/环境变量指定的已安装路径 (BOOST_ROOT / OPENSSL_ROOT_DIR)
#   - 否则若 third_party 下已有预构建目录则复用
#   - 否则用 third_party/boost、openssl-4.0.1 源码自行编译, 产物落到
#     third_party/boost-linux-build-release|OpenSSL-linux-build
# 跳过自动构建 (缺失时直接报错): AGENTXX_SKIP_AUTO_DEPS=1 ./linux_release_build.sh
source "$script_dir/deps/libbuild.sh"
agxxdeps_src_dir="$src_dir/third_party"

if [[ "${AGENTXX_SKIP_AUTO_DEPS:-0}" != "1" ]]; then
    # 未显式指定 BOOST_ROOT 且本地无预构建产物时, 自动编译 Boost (release)
    if [[ -z "${BOOST_ROOT:-}" && ! -f "$src_dir/third_party/boost-linux-build-release/include/boost/version.hpp" ]]; then
        agxxdeps_ensure_boost "$src_dir/third_party/boost-linux-build-release" "release" "boost-linux-release" || exit 1
    fi
    # 未显式指定 OPENSSL_ROOT_DIR 且本地无预构建产物时, 自动编译 OpenSSL
    if [[ -z "${OPENSSL_ROOT_DIR:-}" && ! -f "$src_dir/third_party/OpenSSL-linux-build/include/openssl/opensslv.h" ]]; then
        agxxdeps_ensure_openssl "$src_dir/third_party/OpenSSL-linux-build" "openssl-linux" || exit 1
    fi
fi

# ===== ragel (hyperscan 语法生成器, 需要时自动构建) =====
# AGENTXX_ENABLE_HYPERSCAN 未显式设为 OFF 时默认 ON, 需 ragel;
# 自建产物目录优先加入 PATH (产物 > 系统), 自建失败自动回退系统已装 ragel
if [[ "${AGENTXX_ENABLE_HYPERSCAN:-ON}" != "OFF" && "${AGENTXX_SKIP_AUTO_DEPS:-0}" != "1" ]]; then
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

# ===== 编译加速配置 (与 linux_debug_build.sh 一致) =====
if command -v ccache >/dev/null 2>&1; then
    export CCACHE_DIR="${CCACHE_DIR:-$HOME/.cache/ccache-agentxx}"
    export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-3G}"
    export CCACHE_COMPRESS="${CCACHE_COMPRESS:-1}"
    export CCACHE_BASEDIR="${CCACHE_BASEDIR:-$(cd "$src_dir" && pwd)}"
fi
# 内存不足/频繁编译器崩溃(ICE) 时调小: AGENTXX_BUILD_PARALLEL=4 ./linux_release_build.sh
AGENTXX_BUILD_PARALLEL="${AGENTXX_BUILD_PARALLEL:-$(nproc)}"
# libbuild.sh 使用同一并行数 (必须在 env check 之后定义, 依赖检查需快进)
agxxdeps_parallel="$AGENTXX_BUILD_PARALLEL"

if [[ -z "${BOOST_ROOT}" && -d "$src_dir/third_party/boost-linux-build-release/" ]]; then
    BOOST_ROOT=$(cd "$src_dir/third_party/boost-linux-build-release/" && pwd)
fi
if [[ -z "${OPENSSL_ROOT_DIR}" && -d "$src_dir/third_party/OpenSSL-linux-build/" ]]; then
    OPENSSL_ROOT_DIR=$(cd "$src_dir/third_party/OpenSSL-linux-build/" && pwd)
fi

# ===== 编译环境前置检查 (工具链/内存, 失败即清晰报错) =====
# 检查: cmake/ninja/make、c++26 编译器 (gcc>=14 / clang>=18)。
# 注: ragel 需求已在上方按 AGENTXX_ENABLE_HYPERSCAN 处理; 内存仅提示 (ICE 场景)。
_need_tools=""
for _t in cmake make; do
    command -v "$_t" >/dev/null 2>&1 || _need_tools="$_need_tools$_t "
done
command -v ninja >/dev/null 2>&1 || command -v ninja-build >/dev/null 2>&1 || _need_tools="${_need_tools}ninja "
if [[ -n "$_need_tools" ]]; then
    echo "[env] 缺少构建工具: $_need_tools"
    echo "  请安装: sudo apt-get install -y cmake ninja-build make"
    exit 1
fi
# c++ 编译器版本检查 (本项目要求 c++26; gcc 14+/clang 18+ 才稳定支持,
# 低版本易触发编译器内部错误 ICE)
_cxx_ok=0
if command -v g++ >/dev/null 2>&1; then
    _gcc_ver=$(g++ -dumpversion 2>/dev/null | cut -d. -f1)
    if [[ "${_gcc_ver:-0}" -ge 14 ]]; then
        _cxx_ok=1
    else
        echo "[env] g++ 版本过低 (${_gcc_ver:-未知}), 推荐 g++ >= 14 (c++26); 否则易触发编译器内部错误"
        echo "  安装: sudo apt-get install -y g++-14 && sudo update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-14 100"
    fi
fi
if [[ "$_cxx_ok" == "0" ]] && command -v clang++ >/dev/null 2>&1; then
    _clang_ver=$(clang++ --version 2>/dev/null | head -1 | grep -o -E '[0-9]+' | head -1)
    if [[ "${_clang_ver:-0}" -ge 18 ]]; then
        _cxx_ok=1
    else
        echo "[env] clang++ 版本过低 (${_clang_ver:-未知}), 推荐 clang++ >= 18"
    fi
fi
if [[ "$_cxx_ok" == "0" ]]; then
    echo "[env] 未找到可用的 c++26 编译器 (g++>=14 或 clang++>=18), 请先安装"
    exit 1
fi
# 提示并行内存风险 (每个 c++26 TU 编译约需 1~2GB)
_mem_gb=$(free -g 2>/dev/null | awk '/^Mem:/{print $2}')
if [[ -n "$_mem_gb" && "$_mem_gb" -gt 0 && "${AGENTXX_BUILD_PARALLEL}" -gt "$_mem_gb" ]]; then
    echo "[env] 提示: 并行数 ${AGENTXX_BUILD_PARALLEL} > 可用内存 ${_mem_gb}GB, 若频繁 ICE 请降低:"
    echo "  AGENTXX_BUILD_PARALLEL=4 ./linux_release_build.sh"
fi
unset _need_tools _t _cxx_ok _gcc_ver _clang_ver _mem_gb

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
    -DAGENTXX_VERSION="${AGENTXX_VERSION}" \
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

# ===== 复制 C++/系统运行时到 exec (release 发布分发) =====
# - 目标: {build}/exec 与 agentxx_cli 同目录携带 libstdc++/libgcc_s 等，
#   参考 client/CMakeLists.txt `install(... DESTINATION "${AGENTXX_EXEC_INSTALL_PREFIX}")`
#   的 exec 目录布局，解压即运行，无需目标机安装同版本 GCC。
# - 跳过: AGENTXX_SKIP_BUNDLE_RUNTIME=1 ./linux_release_build.sh
if [[ "${AGENTXX_SKIP_BUNDLE_RUNTIME:-0}" != "1" ]]; then
    EXEC_DIR="$build_dir/exec"
    CXX_BIN="${CXX:-c++}"
    echo "[runtime] bundle C++ runtime libs -> $EXEC_DIR (CXX_BIN=$CXX_BIN)"
    # 1) 主产物 libstdc++/libgcc_s (经编译器自报路径拷贝, 不依赖 ldd 输出格式)
    for _lib in libstdc++.so.6 libgcc_s.so.1; do
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
    # 2) 反查 exec 下全部产物的真实动态依赖 (ldd), 将解析到绝对路径的
    #    非系统库按需补拷 (不预设列表: 避免拷贝 libatomic/libgomp 等
    #    x86_64 上根本用不到的库)。agentxx_cli/agentxx_test 等 ldd 可见,
    #    运行期 dlopen 的 plugins/*.so 则逐个单独扫描。
    if command -v ldd >/dev/null 2>&1; then
        while IFS= read -r _f; do
            [[ -f "$_f" ]] || continue
            echo "[runtime] scan deps of: $_f"
            while IFS= read -r _line; do
                case "$_line" in
                    *' => '/*) ;;
                    *) continue ;;
                esac
                _src=$(echo "$_line" | sed -n 's/.*=> \([^ ]*\) .*/\1/p')
                _name=$(basename "$_src")
                # 跳过: 标准库/系统自带的 (libc/libm/libdl/ld-linux/libpthread/librt/libmvec...)
                case "$_name" in
                    libstdc++.so*|libgcc_s.so*|libc.so*|libm.so*|libdl.so*|ld-linux*|libpthread.so*|librt.so*|libmvec.so*|libresolv.so*|libutil.so*|libcrypt.so*) continue ;;
                esac
                if [[ ! -f "$EXEC_DIR/$_name" ]]; then
                    if [[ "$_src" == /* && -f "$_src" ]]; then
                        echo "[runtime] extra dep: $_name <- $_src"
                        cp -L -v "$_src" "$EXEC_DIR/" || true
                    else
                        echo "WARNING: no absolute path for dep $_name of $_f (may need manual copy)"
                    fi
                fi
                unset _src _name
            done < <(ldd "$_f" 2>/dev/null)
            unset _line
        done < <(find "$EXEC_DIR" -type f \( -name "agentxx_*" -o -name "lib*.so*" \) ! -name "*.a" 2>/dev/null | sort -u)
        unset _f
    fi
    # 3) RPATH 指向 $ORIGIN，使 exe/.so 优先从同目录加载复制的运行时
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

strip --strip-all "$build_dir/exec/agentxx_cli"
strip --strip-all "$build_dir/exec/agentxx_benchmark"
strip --strip-unneeded "$build_dir/exec/libagentxx.so"
find "$build_dir/exec/" -type f -name "*.so*" -exec strip --strip-unneeded {} \;

# ===== 归档发布包 (AGENTXX_PACKAGE_RELEASE=1 时自动执行) =====
if [[ "${AGENTXX_PACKAGE_RELEASE:-0}" == "1" && -n "$AGENTXX_VERSION" ]]; then
    dist_dir="$src_dir/build/dist"
    mkdir -p "$dist_dir"
    pkg_file="$dist_dir/agentxx-v${AGENTXX_VERSION}-linux-x86_64.tar.gz"
    echo "[package] Packing release archive -> $pkg_file"
    tar -czf "$pkg_file" -C "$build_dir/exec" .
    echo "[package] Done: $pkg_file"
fi