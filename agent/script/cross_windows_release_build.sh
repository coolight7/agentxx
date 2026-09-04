#!/bin/bash
# Linux 交叉编译 Windows x64 (MinGW-w64) Release 构建 — 全本地化 (Hermetic)
# - 宿主: Linux x86_64 (WSL 亦可)
# - 目标: Windows x64 (PE64, MinGW-w64 / UCRT)
# - 工具链: llvm-mingw (Clang + MinGW-w64 headers/libs), 自动下载至
#   agent/third_party/cross-windows/toolchain/
#   不依赖系统 apt 安装的 mingw (如 g++-mingw-w64 / mingw-w64-x86-64-dev)
# - 产物: {build}/exec/agentxx_cli.exe + libagentxx.dll + plugins/*.dll
#
# 设计目标:
#   - 所有交叉所需文件均落在 agent/third_party/cross-windows/ 内:
#     cross-windows/
#       toolchain/                  # llvm-mingw 解压后 (bin/x86_64-w64-mingw32-clang 等)
#         bin/
#         x86_64-w64-mingw32/
#         include/  lib/
#       boost-mingw-build-release/  # Boost for MinGW (静态库, 本脚本自动交叉编译)
#       openssl-mingw-build/        # OpenSSL for MinGW (静态库, 本脚本自动交叉编译)
#       .toolchain_version
#       README.md
#   - 仅依赖宿主基础工具: cmake >=3.10, ninja/make, ccache (可选), curl/wget, tar, xz
#   - 不依赖 apt 的 mingw 包; 系统若残留 mingw 不会被使用 (本脚本 PATH 优先本地工具链)
#
# 首次运行会自动:
#   1) 下载 llvm-mingw (约 88MB, 缓存于 /tmp, 解压至 cross-windows/toolchain)
#   2) 若 cross-windows/boost-mingw-build-release 不存在, 则用本地工具链
#      从 agent/third_party/boost/ 源码交叉编译 Boost (约 5-15 分钟)
#   3) 若 cross-windows/openssl-mingw-build 不存在, 则从
#      agent/third_party/openssl-4.0.1/ 源码交叉编译 OpenSSL (约 2-5 分钟)
#   4) CMake 配置 + 编译 + 安装 + strip
# 后续运行直接复用已下载/编译的产物, 秒级进入 CMake 阶段。
#
# 手动覆盖:
#   BOOST_ROOT=/path/to/boost-mingw \
#   OPENSSL_ROOT_DIR=/path/to/openssl-mingw \
#   ./cross_windows_release_build.sh          # 跳过自动构建, 使用指定路径
#   AGENTXX_MINGW_SKIP_AUTO_DEPS=1 ./cross_windows_release_build.sh  # 缺失时直接报错而非自动编译
#   AGENTXX_LLVM_MINGW_VERSION=20241030 ./cross_windows_release_build.sh  # 覆盖工具链版本
#   FORCE_SYSTEM_MINGW=1 ./cross_windows_release_build.sh # 强制回退到系统 mingw (不推荐)
#
# 产物目录:
#   agent/build/cross-windows-release/
#     - exec/agentxx_cli.exe
#     - exec/libagentxx.dll (+ libagentxx.dll.a)
#     - exec/plugins/*.dll
#     - exec/agentxx_benchmark.exe (若启用)
#     - agentxx-project-install/ (ExternalProject 中间安装)
#
# 参考: linux_release_build.sh / cross_android_release_build.sh / windows_release_build.bat
# 工具链: https://github.com/mstorsjo/llvm-mingw

set -e

script_dir=$(cd "$(dirname "$0")" && pwd)
src_dir="$script_dir/../"
# 保留用户要求的构建目录名: cross-windows-release-build
build_dir="$script_dir/../build/cross-windows-release-build"
cross_root="$src_dir/third_party/cross-windows"
toolchain_dir="$cross_root/toolchain"

# ===== 编译加速配置 (与 linux_release_build.sh 一致) =====
if command -v ccache >/dev/null 2>&1; then
    export CCACHE_DIR="${CCACHE_DIR:-$HOME/.cache/ccache-agentxx}"
    export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-3G}"
    export CCACHE_COMPRESS="${CCACHE_COMPRESS:-1}"
    export CCACHE_BASEDIR="${CCACHE_BASEDIR:-$(cd "$src_dir" && pwd)}"
fi
AGENTXX_BUILD_PARALLEL="${AGENTXX_BUILD_PARALLEL:-$(nproc)}"

# ===== 本地工具链配置 =====
LLVM_MINGW_VERSION="${AGENTXX_LLVM_MINGW_VERSION:-20241030}"
LLVM_MINGW_TARBALL="llvm-mingw-${LLVM_MINGW_VERSION}-ucrt-ubuntu-20.04-x86_64.tar.xz"
LLVM_MINGW_URL="https://github.com/mstorsjo/llvm-mingw/releases/download/${LLVM_MINGW_VERSION}/${LLVM_MINGW_TARBALL}"
# 备用: msvcrt 变体 (若 ucrt 下载失败)
LLVM_MINGW_TARBALL_MSVC="llvm-mingw-${LLVM_MINGW_VERSION}-msvcrt-ubuntu-20.04-x86_64.tar.xz"
LLVM_MINGW_URL_MSVC="https://github.com/mstorsjo/llvm-mingw/releases/download/${LLVM_MINGW_VERSION}/${LLVM_MINGW_TARBALL_MSVC}"

mkdir -p "$cross_root"

# 若用户强制使用系统 mingw, 跳过本地工具链逻辑
if [[ "${FORCE_SYSTEM_MINGW:-0}" == "1" ]]; then
    echo "[toolchain] FORCE_SYSTEM_MINGW=1, 将使用系统 mingw (不推荐, 需自行保证版本 >=13)"
    toolchain_dir=""
fi

ensure_toolchain() {
    # Hermetic 校验: 必须为 llvm-mingw 且版本匹配，否则视为无效需重下
    # 旧版残留的 gcc10 sysroot 仅有 gcc 无 clang, 会在此被判定无效 (防止误用系统 libstdc++10 导致 valarray 冲突)
    if [[ -n "$toolchain_dir" && -x "$toolchain_dir/bin/x86_64-w64-mingw32-clang" && -f "$toolchain_dir/.toolchain_version" ]]; then
        local ver
        ver=$(cat "$toolchain_dir/.toolchain_version" 2>/dev/null | tr -d '[:space:]')
        if [[ "$ver" == "$LLVM_MINGW_VERSION" ]]; then
            return 0
        else
            echo "[toolchain] 版本不匹配: 期望 $LLVM_MINGW_VERSION, 实际 $ver, 将重新下载"
        fi
    elif [[ -n "$toolchain_dir" && -x "$toolchain_dir/bin/x86_64-w64-mingw32-clang" ]]; then
        # 有 clang 但无版本文件 (旧布局), 视为无效
        echo "[toolchain] 缺少版本文件, 将重新下载以确保 hermetic"
    fi
    if [[ -z "$toolchain_dir" ]]; then
        return 1
    fi
    # 若目录存在但校验失败，清理旧目录以便全新安装
    if [[ -d "$toolchain_dir" ]]; then
        echo "[toolchain] 清理旧工具链: $toolchain_dir"
        rm -rf "$toolchain_dir"
    fi
    echo "============================================"
    echo "[toolchain] 本地工具链不存在, 准备下载 llvm-mingw ${LLVM_MINGW_VERSION}"
    echo "  URL: $LLVM_MINGW_URL"
    echo "  目标: $toolchain_dir"
    echo "============================================"
    mkdir -p "$cross_root"
    local tmp_tarball="/tmp/${LLVM_MINGW_TARBALL}"
    local url="$LLVM_MINGW_URL"
    local tarball="$LLVM_MINGW_TARBALL"

    # 下载 (curl 优先, wget 回退, 均透传 http_proxy)
    local dl_ok=0
    if command -v curl >/dev/null 2>&1; then
        echo "[download] curl -L $url -> $tmp_tarball"
        if curl -L --progress-bar -o "$tmp_tarball" "$url"; then
            dl_ok=1
        else
            echo "[download] curl 失败, 尝试 msvcrt 变体: $LLVM_MINGW_URL_MSVC"
            url="$LLVM_MINGW_URL_MSVC"
            tarball="$LLVM_MINGW_TARBALL_MSVC"
            tmp_tarball="/tmp/${tarball}"
            if curl -L --progress-bar -o "$tmp_tarball" "$url"; then
                dl_ok=1
            fi
        fi
    elif command -v wget >/dev/null 2>&1; then
        echo "[download] wget $url -> $tmp_tarball"
        if wget -O "$tmp_tarball" "$url"; then
            dl_ok=1
        else
            echo "[download] wget 失败, 尝试 msvcrt 变体"
            url="$LLVM_MINGW_URL_MSVC"
            tarball="$LLVM_MINGW_TARBALL_MSVC"
            tmp_tarball="/tmp/${tarball}"
            if wget -O "$tmp_tarball" "$url"; then
                dl_ok=1
            fi
        fi
    else
        echo "ERROR: 未找到 curl/wget, 无法下载工具链"
        echo "  请手动下载: $url"
        echo "  并解压至: $toolchain_dir"
        return 1
    fi

    if [[ "$dl_ok" != "1" || ! -f "$tmp_tarball" ]]; then
        echo "ERROR: 工具链下载失败"
        echo "  请检查网络/代理 (http_proxy=$http_proxy) 或手动下载:"
        echo "    $LLVM_MINGW_URL"
        echo "    $LLVM_MINGW_URL_MSVC"
        return 1
    fi

    local sz
    sz=$(stat -c%s "$tmp_tarball" 2>/dev/null || stat -f%z "$tmp_tarball" 2>/dev/null || echo "?")
    echo "[download] 完成, 大小: $sz 字节, 正在解压..."

    # 解压
    local extract_tmp="$cross_root/_extract_tmp"
    rm -rf "$extract_tmp"
    mkdir -p "$extract_tmp"
    if ! tar -xf "$tmp_tarball" -C "$extract_tmp"; then
        echo "ERROR: 解压失败: $tmp_tarball"
        return 1
    fi
    # 查找解压出的顶级目录 (llvm-mingw-*)
    local extracted
    extracted=$(find "$extract_tmp" -maxdepth 1 -type d -name "llvm-mingw-*" | head -n1)
    if [[ -z "$extracted" || ! -d "$extracted" ]]; then
        # 某些版本直接解压出 bin/ 等到 extract_tmp
        if [[ -d "$extract_tmp/bin" ]]; then
            extracted="$extract_tmp"
        else
            echo "ERROR: 未找到解压后的 llvm-mingw 目录"
            ls -la "$extract_tmp" 2>&1 | head -n 20
            return 1
        fi
    fi
    rm -rf "$toolchain_dir"
    if [[ "$extracted" == "$extract_tmp" ]]; then
        mv "$extract_tmp" "$toolchain_dir"
    else
        mv "$extracted" "$toolchain_dir"
        rm -rf "$extract_tmp"
    fi
    rm -f "$tmp_tarball"
    chmod +x "$toolchain_dir/bin/"* 2>/dev/null || true
    echo "${LLVM_MINGW_VERSION}" > "$toolchain_dir/.toolchain_version"
    cat > "$cross_root/README.md" <<EOF2
# cross-windows 本地工具链 (Hermetic)

本目录为 Linux 交叉编译 Windows x64 的全本地化依赖, 不依赖系统 apt。

## 结构

- \`toolchain/\` — llvm-mingw ${LLVM_MINGW_VERSION} (Clang + MinGW-w64 headers/libs, UCRT)
  - \`bin/x86_64-w64-mingw32-clang\` / \`clang++\` / \`strip\` / \`windres\`
  - \`x86_64-w64-mingw32/include/\` / \`lib/\`
- \`boost-mingw-build-release/\` — Boost for MinGW (静态库, 由本脚本自动编译)
- \`openssl-mingw-build/\` — OpenSSL for MinGW (静态库, 由本脚本自动编译)

## 维护

- 删除 \`toolchain/\` 后下次构建会自动重新下载
- 删除 \`boost-mingw-build-release/\` / \`openssl-mingw-build/\` 后下次构建会自动重新编译
- 手动指定: \`BOOST_ROOT=/path ./cross_windows_release_build.sh\`
- 强制系统工具链: \`FORCE_SYSTEM_MINGW=1 ./cross_windows_release_build.sh\` (不推荐)

下载源: https://github.com/mstorsjo/llvm-mingw/releases/tag/${LLVM_MINGW_VERSION}
EOF2
    echo "[toolchain] 已安装至: $toolchain_dir"
    ls -lh "$toolchain_dir/bin/x86_64-w64-mingw32-clang"* 2>&1 | head -n 5
    return 0
}

# 若非强制系统, 则确保本地工具链存在
if [[ -n "$toolchain_dir" ]]; then
    if ! ensure_toolchain; then
        echo "ERROR: 本地工具链准备失败, 无法继续"
        exit 1
    fi
    # 优先本地工具链
    export PATH="$toolchain_dir/bin:$PATH"
fi

# ===== 工具链检查 (本地优先, 系统回退) =====
MINGW_CXX=""
MINGW_CC=""
MINGW_STRIP=""
MINGW_WINDRES=""

# 本地工具链优先
if [[ -n "$toolchain_dir" ]]; then
    if [[ -x "$toolchain_dir/bin/x86_64-w64-mingw32-clang++" ]]; then
        MINGW_CXX="$toolchain_dir/bin/x86_64-w64-mingw32-clang++"
        MINGW_CC="$toolchain_dir/bin/x86_64-w64-mingw32-clang"
    elif [[ -x "$toolchain_dir/bin/x86_64-w64-mingw32-g++" ]]; then
        MINGW_CXX="$toolchain_dir/bin/x86_64-w64-mingw32-g++"
        MINGW_CC="$toolchain_dir/bin/x86_64-w64-mingw32-gcc"
    fi
    if [[ -x "$toolchain_dir/bin/x86_64-w64-mingw32-strip" ]]; then
        MINGW_STRIP="$toolchain_dir/bin/x86_64-w64-mingw32-strip"
    elif [[ -x "$toolchain_dir/bin/llvm-strip" ]]; then
        MINGW_STRIP="$toolchain_dir/bin/llvm-strip"
    fi
    if [[ -x "$toolchain_dir/bin/x86_64-w64-mingw32-windres" ]]; then
        MINGW_WINDRES="$toolchain_dir/bin/x86_64-w64-mingw32-windres"
    elif [[ -x "$toolchain_dir/bin/llvm-windres" ]]; then
        MINGW_WINDRES="$toolchain_dir/bin/llvm-windres"
    fi
fi

# 系统回退 (仅当本地未找到时)
if [[ -z "$MINGW_CXX" ]]; then
    for cand in x86_64-w64-mingw32-g++-posix x86_64-w64-mingw32-g++ x86_64-w64-mingw32-clang++; do
        if command -v "$cand" >/dev/null 2>&1; then
            MINGW_CXX="$cand"
            break
        fi
    done
fi
if [[ -z "$MINGW_CC" ]]; then
    for cand in x86_64-w64-mingw32-gcc-posix x86_64-w64-mingw32-gcc x86_64-w64-mingw32-clang; do
        if command -v "$cand" >/dev/null 2>&1; then
            MINGW_CC="$cand"
            break
        fi
    done
fi
if [[ -z "$MINGW_STRIP" ]]; then
    if command -v x86_64-w64-mingw32-strip >/dev/null 2>&1; then
        MINGW_STRIP="x86_64-w64-mingw32-strip"
    elif command -v llvm-strip >/dev/null 2>&1; then
        MINGW_STRIP="llvm-strip"
    else
        MINGW_STRIP="strip"
    fi
fi
if [[ -z "$MINGW_WINDRES" ]]; then
    if command -v x86_64-w64-mingw32-windres >/dev/null 2>&1; then
        MINGW_WINDRES="x86_64-w64-mingw32-windres"
    fi
fi

# 是否为 Clang 工具链 (影响 CMake 旗标与 Boost toolset 选择)
USE_CLANG_MINGW=0
if [[ "$MINGW_CXX" == *"clang"* ]]; then
    USE_CLANG_MINGW=1
fi

# 若 GCC 版本过旧 (<13, 本项目需 C++26), 尝试切换到系统 clang (需已安装 clang-18 + 本地 sysroot)
check_gcc_version_sufficient() {
    local cc="$1"
    if [[ -z "$cc" ]]; then return 1; fi
    local ver_str
    ver_str=$("$cc" -dumpversion 2>/dev/null | head -n1)
    # 处理 "10-posix" 这类后缀, 提取首个数字段
    local ver_num
    ver_num=$(echo "$ver_str" | grep -oE '^[0-9]+' | head -n1)
    if [[ "$ver_num" =~ ^[0-9]+$ ]] && [[ "$ver_num" -ge 13 ]]; then
        return 0
    fi
    return 1
}
if [[ "$USE_CLANG_MINGW" == "0" && -n "$MINGW_CXX" ]]; then
    if ! check_gcc_version_sufficient "$MINGW_CXX"; then
        # 寻找可用 clang
        _clang_cxx=""
        _clang_cc=""
        for cand in clang++-18 clang++-17 clang++-16 clang++ clang; do
            if command -v "$cand" >/dev/null 2>&1; then
                _clang_cxx="$(command -v "$cand")"
                break
            fi
        done
        for cand in clang-18 clang-17 clang-16 clang; do
            if command -v "$cand" >/dev/null 2>&1; then
                _clang_cc="$(command -v "$cand")"
                break
            fi
        done
        if [[ -n "$_clang_cxx" && -n "$_clang_cc" ]]; then
            # 需有本地 sysroot 供 clang 使用 (headers/libs)
            if [[ -d "$toolchain_dir/usr/x86_64-w64-mingw32" || -d "/usr/x86_64-w64-mingw32" ]]; then
                echo "WARNING: 检测到 GCC MinGW 版本过旧 ($($MINGW_CXX --version 2>&1 | head -n1)), 需 >=13 以支持 C++26"
                echo "  将切换至 Clang 交叉编译: $_clang_cxx --target=x86_64-w64-mingw32"
                echo "  (如需强制使用旧版 GCC, 可设置 FORCE_GCC_MINGW=1 后重试)"
                if [[ "${FORCE_GCC_MINGW:-0}" != "1" ]]; then
                    MINGW_CXX="$_clang_cxx"
                    MINGW_CC="$_clang_cc"
                    USE_CLANG_MINGW=1
                    # 确保本地 sysroot 在 PATH/查找路径中 (供 clang 查找头文件)
                    export PATH="$toolchain_dir/bin:$toolchain_dir/usr/bin:$PATH"
                    # 若本地 sysroot 已通过 /usr/x86_64-w64-mingw32 symlink 暴露, 则无需额外操作
                    # 否则 CMake 工具链会通过 CMAKE_FIND_ROOT_PATH 找到
                    if command -v x86_64-w64-mingw32-strip >/dev/null 2>&1; then
                        MINGW_STRIP="x86_64-w64-mingw32-strip"
                    fi
                fi
            else
                echo "WARNING: GCC MinGW 版本过旧 ($($MINGW_CXX --version 2>&1 | head -n1)), 但未找到可用的 MinGW sysroot 供 Clang 使用"
            fi
        else
            echo "WARNING: GCC MinGW 版本过旧 ($($MINGW_CXX --version 2>&1 | head -n1)), 需 >=13, 建议安装 clang-18:"
            echo "  sudo apt-get install -y clang-18 lld-18"
        fi
        unset _clang_cxx _clang_cc
    fi
fi

if [[ -z "$MINGW_CXX" || -z "$MINGW_CC" ]]; then
    echo "ERROR: 未找到可用的 MinGW 交叉编译器"
    if [[ -n "$toolchain_dir" ]]; then
        echo "  本地工具链: $toolchain_dir/bin/x86_64-w64-mingw32-clang"
        echo "  请检查 toolchain 是否正确安装, 或删除 $toolchain_dir 后重试以下载"
    fi
    echo "  亦可强制使用系统工具链: FORCE_SYSTEM_MINGW=1 ./cross_windows_release_build.sh"
    echo "  (需自行安装: sudo apt-get install g++-mingw-w64-x86-64-posix 等, 但版本可能过旧)"
    exit 1
fi

# 可用性校验
if [[ ! -x "$MINGW_CXX" ]]; then
    echo "ERROR: 编译器不可执行: $MINGW_CXX"
    exit 1
fi

echo "============================================"
echo "  Linux 交叉编译 Windows x64 (Hermetic)"
echo "============================================"
echo "  Host:       $(uname -a)"
if [[ "$USE_CLANG_MINGW" == "1" ]]; then
    echo "  Compiler:   $MINGW_CXX --target=x86_64-w64-mingw32 ($($MINGW_CXX --version 2>&1 | head -n1))"
    echo "  Toolchain:  $toolchain_dir (llvm-mingw ${LLVM_MINGW_VERSION}, 本地)"
else
    echo "  Compiler:   $MINGW_CXX ($($MINGW_CXX --version 2>&1 | head -n1))"
    echo "  Toolchain:  $MINGW_CXX (系统)"
fi
echo "  Build Dir:  $build_dir"
echo "  CrossRoot:  $cross_root"
echo "  Parallel:   ${AGENTXX_BUILD_PARALLEL}"
echo "============================================"

# ===== 依赖路径探测 (本地 cross-windows 优先) =====
resolve_boost_root() {
    if [[ -n "${BOOST_ROOT:-}" && -d "$BOOST_ROOT" ]]; then
        (cd "$BOOST_ROOT" && pwd)
        return 0
    fi
    local candidates=(
        "$cross_root/boost-mingw-build-release"
        "$cross_root/boost-mingw"
        "$src_dir/third_party/cross-windows/boost-mingw-build-release"
        "$src_dir/third_party/boost-mingw-build-release"
        "$src_dir/third_party/boost-windows-mingw-build-release"
        "$src_dir/third_party/boost-mingw"
        "$src_dir/third_party/boost-windows-build-release"
    )
    for c in "${candidates[@]}"; do
        if [[ -f "$c/include/boost/version.hpp" ]]; then
            (cd "$c" && pwd)
            return 0
        fi
    done
    return 1
}

resolve_openssl_root() {
    if [[ -n "${OPENSSL_ROOT_DIR:-}" && -d "$OPENSSL_ROOT_DIR" ]]; then
        (cd "$OPENSSL_ROOT_DIR" && pwd)
        return 0
    fi
    local candidates=(
        "$cross_root/openssl-mingw-build"
        "$cross_root/openssl-mingw"
        "$src_dir/third_party/cross-windows/openssl-mingw-build"
        "$src_dir/third_party/OpenSSL-mingw-build"
        "$src_dir/third_party/OpenSSL-windows-mingw-build"
        "$src_dir/third_party/openssl-mingw-build"
        "$src_dir/third_party/OpenSSL-mingw"
        "$src_dir/third_party/OpenSSL-windows-build"
    )
    for c in "${candidates[@]}"; do
        if [[ -f "$c/include/openssl/opensslv.h" ]]; then
            (cd "$c" && pwd)
            return 0
        fi
    done
    return 1
}

# ===== 自动编译缺失依赖 (Boost / OpenSSL) =====
ensure_boost() {
    local boost_src="$src_dir/third_party/boost"
    local boost_install="$cross_root/boost-mingw-build-release"
    if [[ -f "$boost_install/include/boost/version.hpp" ]]; then
        return 0
    fi
    if [[ "${AGENTXX_MINGW_SKIP_AUTO_DEPS:-0}" == "1" ]]; then
        return 1
    fi
    if [[ ! -d "$boost_src" || ! -f "$boost_src/bootstrap.sh" ]]; then
        echo "ERROR: Boost 源码不存在: $boost_src"
        echo "  请下载 https://github.com/boostorg/boost/releases/ (如 boost-1.92.0) 并解压至 $boost_src"
        return 1
    fi
    echo "============================================"
    echo "[deps] Boost for MinGW 不存在, 开始自动交叉编译"
    echo "  源码: $boost_src"
    echo "  安装: $boost_install"
    echo "  工具链: $MINGW_CXX"
    echo "============================================"
    mkdir -p "$boost_install"
    local boost_build_tmp="$cross_root/_boost_build_tmp"
    rm -rf "$boost_build_tmp"
    mkdir -p "$boost_build_tmp"

    # 确保 b2 存在
    if [[ ! -x "$boost_src/b2" ]]; then
        echo "[boost] bootstrap..."
        (cd "$boost_src" && ./bootstrap.sh) || { echo "ERROR: boost bootstrap 失败"; return 1; }
    fi
    # user-config.jam
    local user_jam="$cross_root/user-config.jam"
    # 根据工具链类型选择 toolset
    if [[ "$USE_CLANG_MINGW" == "1" ]]; then
        # Clang 需显式指定 --target, 否则默认 host (linux) 导致找不到 windows.h
        # 同时指定 archiver/ranlib 为 llvm 工具链, 避免 b2 默认调用 llvm-ar 未找到
        # 额外定义 _WIN32_WINNT=0x0A00 (Win10) 以启用新版 Windows API (如 DeleteProcThreadAttributeList)
        cat > "$user_jam" <<EOFJAM
using clang : mingw : ${MINGW_CXX} : <compileflags>--target=x86_64-w64-mingw32 <linkflags>--target=x86_64-w64-mingw32 <archiver>llvm-ar <ranlib>llvm-ranlib ;
EOFJAM
        echo "[boost] user-config.jam (clang):"
        cat "$user_jam"
        echo "[boost] b2 toolset=clang-mingw ..."
        # 允许部分可选库失败 (如 context/thread 的 asm 在旧版 mingw 上), 只要核心库 (headers/filesystem) 成功即视为可用
        # 故捕获 b2 退出码, 但后续以文件存在性判定而非直接失败
        set +e
        (cd "$boost_src" && \
            ./b2 --user-config="$user_jam" \
                --layout=system --prefix="$boost_install" \
                --build-dir="$boost_build_tmp" \
                define=_WIN32_WINNT=0x0A00 \
                toolset=clang-mingw target-os=windows threadapi=win32 \
                link=static runtime-link=shared \
                address-model=64 architecture=x86 variant=release \
                -j"${AGENTXX_BUILD_PARALLEL}" install)
        _b2_rc=$?
        set -e
        if [[ $_b2_rc -ne 0 ]]; then
            echo "[boost] clang-mingw 退出码 $_b2_rc, 检查核心库是否已生成..."
            if [[ ! -f "$boost_install/include/boost/version.hpp" || ! -f "$boost_install/lib/libboost_filesystem.a" ]]; then
                echo "[boost] 核心库缺失, 尝试 gcc 兼容模式..."
                cat > "$user_jam" <<EOFJAM2
using gcc : mingw : ${MINGW_CXX} ;
EOFJAM2
                set +e
                (cd "$boost_src" && \
                    ./b2 --user-config="$user_jam" \
                        --layout=system --prefix="$boost_install" \
                        --build-dir="$boost_build_tmp" \
                        define=_WIN32_WINNT=0x0A00 \
                        toolset=gcc target-os=windows threadapi=win32 \
                        link=static runtime-link=shared \
                        address-model=64 architecture=x86 variant=release \
                        -j"${AGENTXX_BUILD_PARALLEL}" install)
                _b2_rc=$?
                set -e
                if [[ $_b2_rc -ne 0 && ! -f "$boost_install/lib/libboost_filesystem.a" ]]; then
                    echo "ERROR: Boost 交叉编译失败 (gcc 回退亦缺失核心库)"; return 1;
                fi
            else
                echo "[boost] 核心库已生成, 忽略部分可选库失败 (退出码 $_b2_rc)"
            fi
        fi
    else
        cat > "$user_jam" <<EOFJAM3
using gcc : mingw : ${MINGW_CXX} ;
EOFJAM3
        echo "[boost] user-config.jam (gcc):"
        cat "$user_jam"
        (cd "$boost_src" && \
            ./b2 --user-config="$user_jam" \
                --layout=system --prefix="$boost_install" \
                --build-dir="$boost_build_tmp" \
                toolset=gcc target-os=windows threadapi=win32 \
                link=static runtime-link=shared \
                address-model=64 architecture=x86 variant=release \
                -j"${AGENTXX_BUILD_PARALLEL}" install) || { echo "ERROR: Boost 交叉编译失败"; return 1; }
    fi
    rm -rf "$boost_build_tmp"
    if [[ ! -f "$boost_install/include/boost/version.hpp" ]]; then
        echo "ERROR: Boost 安装后仍未找到 version.hpp: $boost_install"
        return 1
    fi
    echo "[boost] 完成: $boost_install"
    ls -lh "$boost_install/lib/libboost_filesystem.a" 2>&1 | head -n 2
    return 0
}

ensure_openssl() {
    local openssl_src="$src_dir/third_party/openssl-4.0.1"
    # 兼容多种源码目录名
    if [[ ! -d "$openssl_src" ]]; then
        for cand in "$src_dir/third_party/openssl"* "$src_dir/third_party/OpenSSL"*; do
            if [[ -f "$cand/Configure" ]]; then
                openssl_src="$cand"
                break
            fi
        done
    fi
    local openssl_install="$cross_root/openssl-mingw-build"
    if [[ -f "$openssl_install/include/openssl/opensslv.h" ]]; then
        return 0
    fi
    if [[ "${AGENTXX_MINGW_SKIP_AUTO_DEPS:-0}" == "1" ]]; then
        return 1
    fi
    if [[ ! -f "$openssl_src/Configure" ]]; then
        echo "ERROR: OpenSSL 源码不存在: $openssl_src"
        echo "  请下载 https://github.com/openssl/openssl/releases/ (如 openssl-4.0.1) 并解压至 $src_dir/third_party/openssl-4.0.1"
        return 1
    fi
    echo "============================================"
    echo "[deps] OpenSSL for MinGW 不存在, 开始自动交叉编译"
    echo "  源码: $openssl_src"
    echo "  安装: $openssl_install"
    echo "  工具链: $MINGW_CXX (prefix x86_64-w64-mingw32-)"
    echo "============================================"
    mkdir -p "$openssl_install"
    # 清理旧的 Makefile 残留 (若之前为 Linux 编译过)
    (cd "$openssl_src" && make distclean 2>/dev/null || true)

    local cross_prefix="$toolchain_dir/bin/x86_64-w64-mingw32-"
    # 若 toolchain 为 llvm-mingw, 确保有 gcc wrapper 前缀
    if [[ ! -x "${cross_prefix}gcc" && -x "${cross_prefix}clang" ]]; then
        # 创建 gcc/clang 包装符号链接供 Configure 查找
        ln -sf "${cross_prefix}clang" "${cross_prefix}gcc" 2>/dev/null || true
        ln -sf "${cross_prefix}clang++" "${cross_prefix}g++" 2>/dev/null || true
        ln -sf "${cross_prefix}clang" "${cross_prefix}cc" 2>/dev/null || true
    fi
    # 确保前缀在 PATH (ar/ranlib 等通过 CROSS_COMPILE 前缀查找, 无需额外 AR/RANLIB)
    export PATH="$toolchain_dir/bin:$toolchain_dir/usr/bin:$PATH"
    # 清理可能残留的 AR/RANLIB 环境变量, 避免与 CROSS_COMPILE 双重前缀 (如 /.../x86_64-w64-mingw32-/.../ar)
    unset AR
    unset RANLIB

    (
        cd "$openssl_src"
        echo "[openssl] Configure mingw64 --cross-compile-prefix=$cross_prefix ..."
        # mingw64 目标会设置 CC=gcc 等, 但需确保交叉前缀正确
        # 旧版 binutils (2.38) 不支持 AVX512 VPCLMULQDQ 等新指令, 需禁用 asm 以避免汇编错误
        # (或升级 binutils, 但 no-asm 更通用, 仅影响性能, 不影响功能)
        ./Configure mingw64 --cross-compile-prefix="$cross_prefix" \
            no-shared no-tests no-docs no-asm \
            --prefix="$openssl_install" --openssldir="$openssl_install" || {
                echo "ERROR: OpenSSL Configure 失败"
                exit 1
            }
        echo "[openssl] make -j${AGENTXX_BUILD_PARALLEL} ..."
        make -j"${AGENTXX_BUILD_PARALLEL}" || { echo "ERROR: OpenSSL make 失败"; exit 1; }
        echo "[openssl] make install ..."
        make install || { echo "ERROR: OpenSSL make install 失败"; exit 1; }
    ) || return 1

    if [[ ! -f "$openssl_install/include/openssl/opensslv.h" ]]; then
        echo "ERROR: OpenSSL 安装后仍未找到 opensslv.h: $openssl_install"
        return 1
    fi
    echo "[openssl] 完成: $openssl_install"
    ls -lh "$openssl_install/lib/libssl.a" "$openssl_install/lib64/libssl.a" 2>&1 | head -n 5
    return 0
}

# 尝试自动补齐缺失依赖
if ! BOOST_ROOT=$(resolve_boost_root); then
    echo "[deps] 未找到 Boost for MinGW, 尝试自动编译..."
    if ! ensure_boost; then
        echo "ERROR: 未找到 Boost for MinGW 预编译库"
        echo "  期望路径: $cross_root/boost-mingw-build-release/"
        echo "  或手动指定: BOOST_ROOT=/path/to/boost-mingw-build-release ./cross_windows_release_build.sh"
        echo "  或跳过自动编译并报错: AGENTXX_MINGW_SKIP_AUTO_DEPS=1 ./cross_windows_release_build.sh"
        exit 1
    fi
    BOOST_ROOT=$(resolve_boost_root) || { echo "ERROR: Boost 自动编译后仍未找到"; exit 1; }
fi

if ! OPENSSL_ROOT_DIR=$(resolve_openssl_root); then
    echo "[deps] 未找到 OpenSSL for MinGW, 尝试自动编译..."
    if ! ensure_openssl; then
        echo "ERROR: 未找到 OpenSSL for MinGW 预编译库"
        echo "  期望路径: $cross_root/openssl-mingw-build/"
        echo "  或手动指定: OPENSSL_ROOT_DIR=/path/to/openssl-mingw-build ./cross_windows_release_build.sh"
        exit 1
    fi
    OPENSSL_ROOT_DIR=$(resolve_openssl_root) || { echo "ERROR: OpenSSL 自动编译后仍未找到"; exit 1; }
fi

# 兼容 lib / lib64 布局
if [[ ! -f "$OPENSSL_ROOT_DIR/lib/libssl.a" && ! -f "$OPENSSL_ROOT_DIR/lib64/libssl.a" ]]; then
    echo "WARNING: 在 OPENSSL_ROOT_DIR 下未找到 lib/libssl.a 或 lib64/libssl.a"
    echo "  OPENSSL_ROOT_DIR=$OPENSSL_ROOT_DIR"
    echo "  请确认 OpenSSL 已按 mingw64 目标正确安装 (make install)"
fi

# 若误用了 Linux 版 (ELF), 报错
if file "$BOOST_ROOT/lib/libboost_filesystem.a" 2>/dev/null | grep -q "ELF"; then
    echo "ERROR: 检测到 BOOST_ROOT 为 Linux (ELF) 版本, 无法用于 MinGW 交叉"
    echo "  BOOST_ROOT=$BOOST_ROOT"
    echo "  请删除 $BOOST_ROOT 并让脚本自动重编, 或手动按 mingw64 目标重编"
    exit 1
fi
if file "$OPENSSL_ROOT_DIR/lib/libssl.a" 2>/dev/null | grep -q "ELF"; then
    echo "ERROR: 检测到 OPENSSL_ROOT_DIR 为 Linux (ELF) 版本 (lib)"
    echo "  OPENSSL_ROOT_DIR=$OPENSSL_ROOT_DIR"
    exit 1
fi
if file "$OPENSSL_ROOT_DIR/lib64/libssl.a" 2>/dev/null | grep -q "ELF"; then
    echo "ERROR: 检测到 OPENSSL_ROOT_DIR 为 Linux (ELF) 版本 (lib64)"
    echo "  OPENSSL_ROOT_DIR=$OPENSSL_ROOT_DIR"
    exit 1
fi

echo "  BOOST_ROOT:        $BOOST_ROOT"
echo "  OPENSSL_ROOT_DIR:  $OPENSSL_ROOT_DIR"
echo "============================================"

# ===== 生成 MinGW 工具链文件 =====
mkdir -p "$build_dir"
TOOLCHAIN_FILE="$build_dir/toolchain-mingw64.cmake"
if [[ "$USE_CLANG_MINGW" == "1" ]]; then
    cat > "$TOOLCHAIN_FILE" <<EOF
# Auto-generated MinGW-w64 cross toolchain (Clang+MinGW Hermetic, cross_windows_release_build.sh)
# Host: Linux x86_64  Target: Windows x64 (MinGW UCRT via Clang)
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_SYSTEM_VERSION 10)

# 编译器 (Clang 交叉到 MinGW, 本地 hermetic)
set(CMAKE_C_COMPILER   "${MINGW_CC}")
set(CMAKE_CXX_COMPILER "${MINGW_CXX}")
set(CMAKE_C_COMPILER_TARGET   "x86_64-w64-mingw32")
set(CMAKE_CXX_COMPILER_TARGET "x86_64-w64-mingw32")
# windres
set(CMAKE_RC_COMPILER  "${MINGW_WINDRES}")
# ld.lld 优先 (llvm-mingw 自带)
find_program(_LLD_LINKER ld.lld PATHS "${toolchain_dir}/bin" NO_DEFAULT_PATH)
if (_LLD_LINKER)
  # clang 默认会调用 ld.lld (若安装), 无需额外设置
endif()

# 交叉编译搜索策略
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(THREADS_PREFER_PTHREAD_FLAG ON)

# 确保 _WIN32_WINNT 定义对 try_compile 也可见 (curl 等的 HAVE_WIN32_WINNT 检测依赖此)
# CMAKE_C/CXX_FLAGS_INIT 会被 try_compile 自动继承，而仅设置 CMAKE_C_FLAGS_RELEASE 则不会
set(CMAKE_C_FLAGS_INIT "-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00")
set(CMAKE_CXX_FLAGS_INIT "-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00")
add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00 _WIN32_IE=0x0A00 NTDDI_VERSION=0x0A000000)

# 本地 sysroot (llvm-mingw: toolchain/ 本身即 sysroot 根, 同时包含 generic-w64-mingw32)
list(APPEND CMAKE_FIND_ROOT_PATH "${toolchain_dir}" "${toolchain_dir}/x86_64-w64-mingw32" "${toolchain_dir}/generic-w64-mingw32")
EOF
else
    cat > "$TOOLCHAIN_FILE" <<EOF
# Auto-generated MinGW-w64 cross toolchain (GCC MinGW Hermetic, cross_windows_release_build.sh)
# Host: Linux x86_64  Target: Windows x64 (MinGW)
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_SYSTEM_VERSION 10)

# 编译器 (GCC MinGW, 本地 hermetic)
set(CMAKE_C_COMPILER   "${MINGW_CC}")
set(CMAKE_CXX_COMPILER "${MINGW_CXX}")
set(CMAKE_RC_COMPILER  "${MINGW_WINDRES}")

# 交叉编译搜索策略
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(THREADS_PREFER_PTHREAD_FLAG ON)

set(CMAKE_C_FLAGS_INIT "-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00")
set(CMAKE_CXX_FLAGS_INIT "-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00")
add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00 _WIN32_IE=0x0A00 NTDDI_VERSION=0x0A000000)

list(APPEND CMAKE_FIND_ROOT_PATH "${toolchain_dir}" "${toolchain_dir}/x86_64-w64-mingw32" "${toolchain_dir}/generic-w64-mingw32")
EOF
fi

# 兼容: 若存在 x86_64-w64-mingw32-pkg-config (本地或系统), 优先使用
if [[ -x "$toolchain_dir/bin/x86_64-w64-mingw32-pkg-config" ]]; then
    export PKG_CONFIG_EXECUTABLE="$toolchain_dir/bin/x86_64-w64-mingw32-pkg-config"
elif command -v x86_64-w64-mingw32-pkg-config >/dev/null 2>&1; then
    export PKG_CONFIG_EXECUTABLE=x86_64-w64-mingw32-pkg-config
fi
# 确保本地工具链在 PATH 最前 (供 ExternalProject 子进程继承)
export PATH="$toolchain_dir/bin:$PATH"

# 生成器选择: 优先 Ninja
CMAKE_GENERATOR_ARGS=()
if command -v ninja >/dev/null 2>&1; then
    CMAKE_GENERATOR_ARGS+=(-G Ninja)
fi

echo "[toolchain] $TOOLCHAIN_FILE"
cat "$TOOLCHAIN_FILE"

# ===== CMake 配置 =====
cmake -B "$build_dir" -S "$src_dir" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DBOOST_ROOT="${BOOST_ROOT}" \
    -DOPENSSL_ROOT_DIR="${OPENSSL_ROOT_DIR}" \
    -DAGENTXX_BUILD_CLIENT=ON \
    -DAGENTXX_BUILD_TEST=ON \
    -DAGENTXX_BUILD_BENCHMARK=ON \
    -DAGENTXX_ENABLE_HYPERSCAN=OFF \
    -DAGENTXX_ENABLE_BOOST_PROCESS=ON \
    -DAGENTXX_LINKER=bfd \
    -DXX_IS_RELEASE_D=1 \
    "${CMAKE_GENERATOR_ARGS[@]}"

if [[ $? -ne 0 ]]; then
    echo "cmake config failed!"
    exit 1
fi

# ===== 编译 =====
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

# ===== Strip (MinGW PE) =====
echo "[strip] using $MINGW_STRIP"
if [[ -f "$build_dir/exec/agentxx_cli.exe" ]]; then
    "$MINGW_STRIP" --strip-all "$build_dir/exec/agentxx_cli.exe" || true
fi
if [[ -f "$build_dir/exec/agentxx_cli" ]]; then
    "$MINGW_STRIP" --strip-all "$build_dir/exec/agentxx_cli" || true
fi
if [[ -f "$build_dir/exec/agentxx_benchmark.exe" ]]; then
    "$MINGW_STRIP" --strip-all "$build_dir/exec/agentxx_benchmark.exe" || true
fi
if [[ -f "$build_dir/exec/agentxx_benchmark" ]]; then
    "$MINGW_STRIP" --strip-all "$build_dir/exec/agentxx_benchmark" || true
fi
if [[ -f "$build_dir/exec/agentxx_test.exe" ]]; then
    "$MINGW_STRIP" --strip-all "$build_dir/exec/agentxx_test.exe" || true
fi
if [[ -f "$build_dir/exec/agentxx_test" ]]; then
    "$MINGW_STRIP" --strip-all "$build_dir/exec/agentxx_test" || true
fi
if [[ -d "$build_dir/exec" ]]; then
    find "$build_dir/exec" -type f \( -name "*.dll" -o -name "*.so" \) -exec "$MINGW_STRIP" --strip-unneeded {} \; 2>/dev/null || true
fi

# ===== 复制 MinGW 运行时 DLL (Windows 上直接运行所需) =====
# - client CMakeLists install TARGETS agentxx_cli DESTINATION
#   AGENTXX_EXEC_INSTALL_PREFIX (exec/); 交叉产物与 exe 同目录分发，
#   否则在纯净 Windows 上启动时报 "找不到 xxx.dll"。
# - Clang/llvm-mingw: libc++.dll / libunwind.dll
# - GCC/MinGW: libstdc++-6.dll / libgcc_s_seh-1.dll (+ sjlj/dw2 变体兼容)
# - 两者共有: libwinpthread-1.dll
# - UCRT (ucrtbase/api-ms-win-*) 为 Win10+ 系统自带，不复制。
# - 跳过: AGENTXX_SKIP_BUNDLE_RUNTIME=1 ./cross_windows_release_build.sh
if [[ "${AGENTXX_SKIP_BUNDLE_RUNTIME:-0}" != "1" && -n "${toolchain_dir:-}" ]]; then
    echo "[runtime] 复制 MinGW 运行时 DLL -> $build_dir/exec/"
    _rt_candidates=(libc++.dll libunwind.dll libstdc++-6.dll libgcc_s_seh-1.dll libgcc_s_sjlj-1.dll libgcc_s_dw2-1.dll libwinpthread-1.dll)
    _rt_search_dirs=()
    [[ -d "$toolchain_dir/x86_64-w64-mingw32/bin" ]] && _rt_search_dirs+=("$toolchain_dir/x86_64-w64-mingw32/bin")
    [[ -d "$toolchain_dir/bin" ]] && _rt_search_dirs+=("$toolchain_dir/bin")
    [[ -d "$toolchain_dir/x86_64-w64-mingw32/lib" ]] && _rt_search_dirs+=("$toolchain_dir/x86_64-w64-mingw32/lib")
    [[ -d "/usr/x86_64-w64-mingw32/lib" ]] && _rt_search_dirs+=("/usr/x86_64-w64-mingw32/lib")
    [[ -d "/usr/lib/gcc/x86_64-w64-mingw32" ]] && _rt_search_dirs+=($(find /usr/lib/gcc/x86_64-w64-mingw32 -maxdepth 2 -type d 2>/dev/null))
    for _dll in "${_rt_candidates[@]}"; do
        [[ -f "$build_dir/exec/$_dll" ]] && continue
        for _d in "${_rt_search_dirs[@]}"; do
            if [[ -f "$_d/$_dll" ]]; then
                cp -v "$_d/$_dll" "$build_dir/exec/" || true
                break
            fi
        done
    done
    unset _dll _d
    # 兜底: 解析产物实际依赖的 DLL 名 (objdump)，缺啥补啥
    _objdump=""
    if [[ -x "$toolchain_dir/bin/x86_64-w64-mingw32-objdump" ]]; then
        _objdump="$toolchain_dir/bin/x86_64-w64-mingw32-objdump"
    elif [[ -x "$toolchain_dir/bin/llvm-objdump" ]]; then
        _objdump="$toolchain_dir/bin/llvm-objdump"
    elif command -v x86_64-w64-mingw32-objdump >/dev/null 2>&1; then
        _objdump="x86_64-w64-mingw32-objdump"
    fi
    if [[ -n "$_objdump" && -f "$build_dir/exec/agentxx_cli.exe" ]]; then
        _need_dlls=$("$_objdump" -p "$build_dir/exec/agentxx_cli.exe" 2>/dev/null | grep -i "DLL Name" | awk '{print $NF}')
        for _dll in $_need_dlls; do
            case "$_dll" in
                KERNEL32.dll|USER32.dll|ADVAPI32.dll|SHELL32.dll|WS2_32.dll|MSWSOCK.dll|WINMM.dll|OLE32.dll|OLEAUT32.dll|GDI32.dll|CRYPT32.dll|BCRYPT.dll|NCRYPT.dll|SCHANNEL.dll|SECUR32.dll|IPHLPAPI.dll|DNSAPI.dll|WINHTTP.dll|WTSAPI32.dll|USERENV.dll|VERSION.dll|SHLWAPI.dll|COMDLG32.dll|COMCTL32.dll|IMM32.dll|SETUPAPI.dll|CFGMGR32.dll|POWRPROF.dll|NTDLL.dll|MSVCRT.dll|UCRTBASE.dll|API-MS-Win-*.dll|api-ms-win-*.dll) continue ;;
            esac
            if [[ ! -f "$build_dir/exec/$_dll" ]]; then
                for _d in "${_rt_search_dirs[@]}"; do
                    if [[ -f "$_d/$_dll" ]]; then
                        echo "[runtime] 按需补拷: $_dll"
                        cp -v "$_d/$_dll" "$build_dir/exec/" || true
                        break
                    fi
                done
            fi
        done
        unset _need_dlls _dll _d
    fi
    unset _objdump
    unset _rt_candidates _rt_search_dirs
    # 校验 (按实际工具链只要求对应的一组存在)
    if [[ "$USE_CLANG_MINGW" == "1" ]]; then
        for dll in libc++.dll libunwind.dll; do
            [[ -f "$build_dir/exec/$dll" ]] || echo "WARNING: 运行时 $dll 未找到 ($toolchain_dir/x86_64-w64-mingw32/bin/$dll)"
        done
    else
        for dll in libstdc++-6.dll libgcc_s_seh-1.dll; do
            [[ -f "$build_dir/exec/$dll" ]] || echo "WARNING: 运行时 $dll 未找到 (GCC MinGW sysroot)"
        done
    fi
    echo "[runtime] exec/*.dll:"
    ls -lh "$build_dir/exec/"*.dll 2>/dev/null || true
fi

echo ""
echo "============================================"
echo "  交叉编译完成! (Hermetic)"
echo "  输出目录: $build_dir/exec/"
echo "    - agentxx_cli.exe (Windows x64)"
echo "    - libagentxx.dll (+ libagentxx.dll.a)"
echo "    - plugins/*.dll"
echo "  安装目录: $build_dir/agentxx-project-install/"
echo "  工具链:   $toolchain_dir ($LLVM_MINGW_VERSION)"
echo "  CrossRoot: $cross_root"
echo "============================================"
echo "  在 Windows 上运行:"
echo "    agentxx_cli.exe"
echo "  或将 exec 目录整体拷贝到 Windows 主机后双击运行"
echo "============================================"

