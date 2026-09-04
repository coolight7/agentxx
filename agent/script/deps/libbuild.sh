#!/bin/bash
# =============================================================================
# agentxx 依赖库自构建公共函数库 (Hermetic deps)
# -----------------------------------------------------------------------------
# 被 script/ 下的构建脚本 source 使用 (linux_*/cross_android_* 等)。
# 目标: 让 Boost / OpenSSL / ragel 等依赖库全部使用本仓库自己编译的产物,
#       而不是系统 (apt/pkg) 安装的库, 保证跨机可复现构建。
#
# 产物统一落在 {src_dir}/third_party/ 下, 与项目内已有预构建目录布局一致:
#   boost-linux-build-debug|release    -> Linux 自行编译 Boost 1.92 (debug/release)
#   OpenSSL-linux-build                -> Linux 自行编译 OpenSSL
#   boost-android-build-{debug|release}/<abi> -> Android 自行交叉编译 Boost
#   OpenSSL-android-build/<abi>        -> Android 自行交叉编译 OpenSSL
#   (boost-android/ 为 Boost-for-Android 交叉构建工具源码目录)
#
# 公共约定:
#   - 所有函数带 `agxxdeps_` 前缀, 由调用脚本 source 后使用。
#   - 失败即 echo 错误并 return 非 0 (调用方需 set -e 或自行检查)。
#   - 完成判定只看关键产物文件是否存在 (兼容用户手动预构建的目录);
#     每次成功构建后会写独立 "完成标记" 文件 (third_party/.deps-<tag>) 供排查,
#     判定本身不依赖它。
#   - 环境变量 (可由用户/CI 覆盖):
#       AGENTXX_DEPS_DIR      依赖源码根 (默认 {src_dir}/third_party)
#       AGENTXX_BUILD_PARALLEL 并行任务数
#       AGENTXX_DEPS_FORCE    =1 时强制重建 (boost/openssl 忽略复用)
# =============================================================================

# 仅允许被 source
if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    echo "ERROR: libbuild.sh 是函数库, 请用 source 引入, 不要直接执行" >&2
    exit 2
fi

# 依赖源码根 (agent/third_party), 可被调用方预先覆盖
agxxdeps_src_dir="${AGENTXX_DEPS_DIR:-}"
if [[ -z "$agxxdeps_src_dir" ]]; then
    agxxdeps_src_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../third_party" && pwd)"
fi
agxxdeps_parallel="${AGENTXX_BUILD_PARALLEL:-$(nproc 2>/dev/null || echo 4)}"

# ===== 小工具 =====
agxxdeps_info()  { echo "[deps] $*"; }
agxxdeps_warn()  { echo "[deps][WARN] $*"; }
agxxdeps_error() { echo "[deps][ERROR] $*" >&2; }

# 完成标记路径 (每个依赖一个)
agxxdeps_mark() {
    echo "${agxxdeps_src_dir}/.deps-$1"
}

# 判定一个依赖是否已构建完成。
# 规则: 关键产物文件存在即视为完成 (兼容用户手动预构建、无标记文件的目录)。
# 用法: agxxdeps_done <tag> <dir> [关键文件...] (所有关键文件存在才 true)
agxxdeps_done() {
    local _tag="$1" _dir="$2"
    shift 2
    [[ -d "$_dir" ]] || return 1
    local f
    for f in "$@"; do
        [[ -e "$f" ]] || return 1
    done
    return 0
}

# ===== 下载工具 (curl 优先, wget 回退; 带超时/重试) =====
# 用法: agxxdeps_download <url> <dest_file> [sha256]
agxxdeps_download() {
    local url="$1" dest="$2" sha="${3:-}"
    if [[ -f "$dest" && -n "$sha" ]]; then
        if echo "$sha  $dest" | sha256sum -c - >/dev/null 2>&1; then
            agxxdeps_info "已缓存: $(basename "$dest")"
            return 0
        fi
        agxxdeps_info "校验和不匹配, 重新下载: $(basename "$dest")"
    fi
    mkdir -p "$(dirname "$dest")"
    agxxdeps_info "下载 $url"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL --retry 3 --connect-timeout 20 -o "$dest" "$url" || return 1
    elif command -v wget >/dev/null 2>&1; then
        wget -q -O "$dest" "$url" || return 1
    else
        agxxdeps_error "需要 curl 或 wget 下载依赖"
        return 1
    fi
    if [[ -n "$sha" ]]; then
        if ! echo "$sha  $dest" | sha256sum -c - >/dev/null 2>&1; then
            agxxdeps_error "下载文件校验失败: $dest"
            return 1
        fi
    fi
    return 0
}

# 解压 tar.gz / tar.bz2 / tar.xz / zip (按扩展名自动选择)
# 用法: agxxdeps_extract <archive> <dest_dir>
agxxdeps_extract() {
    local arc="$1" dir="$2"
    mkdir -p "$dir"
    agxxdeps_info "解压 $(basename "$arc") -> $dir"
    case "$arc" in
        *.tar.gz|*.tgz)  tar xzf "$arc" -C "$dir" ;;
        *.tar.bz2|*.tbz2) tar xjf "$arc" -C "$dir" ;;
        *.tar.xz|*.txz)  tar xJf "$arc" -C "$dir" ;;
        *.zip)           (command -v unzip >/dev/null 2>&1 && unzip -q -o "$arc" -d "$dir") \
                            || python3 -c "import zipfile,sys;zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "$arc" "$dir" ;;
        *) agxxdeps_error "无法识别的压缩格式: $arc"; return 1 ;;
    esac
    return $?
}

# ===== Boost (通用宿主构建: Linux/macOS; Windows 走各自 .bat) =====
# 用法: agxxdeps_ensure_boost <install_dir> <variant:debug|release> <tag>
# 依赖: 源码目录 {agxxdeps_src_dir}/boost (内含 bootstrap.sh, 由调用脚本保证)
# 源码缺失时自动从 archives.boost.io 下载 boost-1.92.0 (源码目录不入库)
agxxdeps_ensure_boost() {
    local install_dir="$1" variant="$2" tag="$3"
    local boost_src="$agxxdeps_src_dir/boost"
    if [[ "${AGENTXX_DEPS_FORCE:-0}" == "1" ]]; then
        rm -f "$(agxxdeps_mark "$tag")"
    fi
    if agxxdeps_done "$tag" "$install_dir" "$install_dir/include/boost/version.hpp" \
            "$install_dir/lib/libboost_filesystem.a"; then
        agxxdeps_info "Boost($variant) 已构建, 跳过: $install_dir"
        return 0
    fi
    if [[ ! -f "$boost_src/bootstrap.sh" ]]; then
        agxxdeps_warn "Boost 源码不存在: $boost_src, 尝试自动下载 boost-1.92.0 ..."
        local tmp_arc="$agxxdeps_src_dir/tools/_boost_1_92_0.tar.gz"
        rm -rf "$boost_src"
        mkdir -p "$agxxdeps_src_dir/tools"
        agxxdeps_download "https://archives.boost.io/release/1.92.0/source/boost_1_92_0.tar.gz" "$tmp_arc" || {
            agxxdeps_error "Boost 源码下载失败, 请手动下载解压到: $boost_src"
            return 1; }
        rm -rf "$agxxdeps_src_dir/tools/_boost_extract"
        mkdir -p "$agxxdeps_src_dir/tools/_boost_extract"
        agxxdeps_extract "$tmp_arc" "$agxxdeps_src_dir/tools/_boost_extract" || return 1
        mv "$agxxdeps_src_dir/tools/_boost_extract/boost_1_92_0" "$boost_src" || {
            agxxdeps_error "Boost 解压目录名异常"; return 1; }
        rm -rf "$agxxdeps_src_dir/tools/_boost_extract" "$tmp_arc"
    fi
    agxxdeps_info "=============================================="
    agxxdeps_info "开始自编译 Boost ($variant)"
    agxxdeps_info "  源码: $boost_src"
    agxxdeps_info "  安装: $install_dir"
    agxxdeps_info "  并行: $agxxdeps_parallel"
    agxxdeps_info "=============================================="
    mkdir -p "$install_dir"

    # bootstrap (生成 b2)
    if [[ ! -x "$boost_src/b2" ]]; then
        agxxdeps_info "bootstrap b2 ..."
        (cd "$boost_src" && ./bootstrap.sh) || {
            agxxdeps_error "Boost bootstrap 失败"; return 1; }
    fi
    export CXXFLAGS="-fPIC"
    export CFLAGS="-fPIC"
    if [[ "$variant" == "debug" ]]; then
        # debug: 与文档一致 (runtime-debugging=on)
        (cd "$boost_src" && ./b2 install --layout=system --prefix="$install_dir" \
            cflags=-fPIC cxxflags=-fPIC link=static runtime-link=shared \
            runtime-debugging=on address-model=64 variant=debug \
            -j"$agxxdeps_parallel") || return 1
    else
        (cd "$boost_src" && ./b2 install --layout=system --prefix="$install_dir" \
            cflags=-fPIC cxxflags=-fPIC link=static runtime-link=shared \
            runtime-debugging=off address-model=64 variant=release \
            -j"$agxxdeps_parallel") || return 1
    fi
    unset CXXFLAGS CFLAGS

    if [[ ! -f "$install_dir/include/boost/version.hpp" || \
          ! -f "$install_dir/lib/libboost_filesystem.a" ]]; then
        agxxdeps_error "Boost($variant) 安装产物缺失, 安装目录: $install_dir"
        return 1
    fi
    echo "$variant $(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$(agxxdeps_mark "$tag")"
    agxxdeps_info "Boost($variant) 完成: $install_dir"
    return 0
}

# ===== OpenSSL (宿主 Linux 构建) =====
# 用法: agxxdeps_ensure_openssl <install_dir> <tag>
# 依赖: 源码目录 {agxxdeps_src_dir}/openssl-4.0.1 (或 openssl*, 自动探测)
agxxdeps_ensure_openssl() {
    local install_dir="$1" tag="$2"
    local ossl_src=""
    # 兼容目录名: openssl-4.0.1 / openssl* / OpenSSL*
    for cand in "$agxxdeps_src_dir/openssl-4.0.1" "$agxxdeps_src_dir"/openssl* "$agxxdeps_src_dir"/OpenSSL*; do
        if [[ -f "$cand/Configure" && -f "$cand/Makefile" ]]; then
            ossl_src="$cand"; break
        fi
    done
    if [[ -z "$ossl_src" ]]; then
        for cand in "$agxxdeps_src_dir/openssl-4.0.1" "$agxxdeps_src_dir"/openssl* "$agxxdeps_src_dir"/OpenSSL*; do
            if [[ -f "$cand/Configure" ]]; then
                ossl_src="$cand"; break
            fi
        done
    fi
    if [[ "${AGENTXX_DEPS_FORCE:-0}" == "1" ]]; then
        rm -f "$(agxxdeps_mark "$tag")"
    fi
    if agxxdeps_done "$tag" "$install_dir" "$install_dir/include/openssl/opensslv.h"; then
        agxxdeps_info "OpenSSL 已构建, 跳过: $install_dir"
        return 0
    fi
    if [[ -z "$ossl_src" || ! -f "$ossl_src/Configure" ]]; then
        agxxdeps_warn "OpenSSL 源码不存在: $agxxdeps_src_dir/openssl-4.0.1, 尝试自动下载 ..."
        local tmp_arc="$agxxdeps_src_dir/tools/_openssl-4.0.1.tar.gz"
        mkdir -p "$agxxdeps_src_dir/tools"
        agxxdeps_download "https://github.com/openssl/openssl/archive/refs/tags/openssl-4.0.1.tar.gz" "$tmp_arc" || {
            agxxdeps_error "OpenSSL 源码下载失败, 请手动下载解压到: $agxxdeps_src_dir/openssl-4.0.1"
            return 1; }
        rm -rf "$agxxdeps_src_dir/tools/_ossl_extract"
        mkdir -p "$agxxdeps_src_dir/tools/_ossl_extract"
        agxxdeps_extract "$tmp_arc" "$agxxdeps_src_dir/tools/_ossl_extract" || return 1
        # github archive 解压目录名为 <repo>-<tag> (openssl-openssl-4.0.1)
        local _extracted=""
        _extracted=$(find "$agxxdeps_src_dir/tools/_ossl_extract" -maxdepth 1 -type d -name "openssl-*" | head -n1)
        if [[ -z "$_extracted" ]]; then
            agxxdeps_error "OpenSSL 解压目录名异常"
            return 1
        fi
        rm -rf "$agxxdeps_src_dir/openssl-4.0.1"
        mv "$_extracted" "$agxxdeps_src_dir/openssl-4.0.1"
        rm -rf "$agxxdeps_src_dir/tools/_ossl_extract" "$tmp_arc"
        ossl_src="$agxxdeps_src_dir/openssl-4.0.1"
    fi
    agxxdeps_info "=============================================="
    agxxdeps_info "开始自编译 OpenSSL (Linux)"
    agxxdeps_info "  源码: $ossl_src"
    agxxdeps_info "  安装: $install_dir"
    agxxdeps_info "=============================================="
    mkdir -p "$install_dir"

    # OpenSSL 仅支持源码内构建; 清掉可能的上次构建残留
    # (Configure 本身不接受 -j 并行参数, 并行在 make 阶段生效)
    (cd "$ossl_src" && make distclean >/dev/null 2>&1 || true)
    (cd "$ossl_src" && ./Configure no-shared --pic --prefix="$install_dir" \
        --openssldir="$install_dir" '-Wl,-rpath,$(LIBRPATH)' \
        && make -j"$agxxdeps_parallel" && make install) || {
            agxxdeps_error "OpenSSL 构建失败, 源码目录: $ossl_src"
            return 1
        }
    if [[ ! -f "$install_dir/include/openssl/opensslv.h" || \
          ( ! -f "$install_dir/lib/libssl.a" && ! -f "$install_dir/lib64/libssl.a" ) ]]; then
        agxxdeps_error "OpenSSL 安装产物缺失, 安装目录: $install_dir"
        return 1
    fi
    echo "linux $(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$(agxxdeps_mark "$tag")"
    agxxdeps_info "OpenSSL 完成: $install_dir"
    return 0
}

# ===== ragel (源码构建; 生成 hyperscan 语法解析器) =====
# 安装至 {src_dir}/third_party/tools/ragel-<ver>/。
# 优先级: 本地自建产物 > 系统已装 > 下载自建 (ragel 只是构建期代码生成器,
#         不属于运行库, 系统有就直接用; 产物/系统都没有时才尝试下载自建)
# 用法: agxxdeps_ensure_ragel; 若使用了自建产物目录, 最后一行输出其 bin 目录
agxxdeps_ensure_ragel() {
    local ver="${AGENTXX_RAGEL_VERSION:-7.0.4}"
    local install_dir="$agxxdeps_src_dir/tools/ragel-$ver"
    local install_bin="$install_dir/bin/ragel"
    # 1) 本地自建产物优先
    if [[ -x "$install_bin" ]]; then
        agxxdeps_info "ragel 自建产物已存在, 跳过: $install_bin"
        echo "$install_dir/bin"
        return 0
    fi
    # 2) 系统已装直接使用 (生成器工具, 无需自建)
    if command -v ragel >/dev/null 2>&1 && [[ "${AGENTXX_DEPS_FORCE:-0}" != "1" ]]; then
        agxxdeps_info "使用系统 ragel: $(command -v ragel)"
        return 0
    fi
    # 3) 都没有时尝试下载自建 (需要 make/g++ 工具链)
    agxxdeps_info "=============================================="
    agxxdeps_info "开始自编译 ragel $ver (hyperscan 解析器)"
    agxxdeps_info "=============================================="
    local work="$agxxdeps_src_dir/tools/_ragel_build"
    local src="$work/ragel-$ver"
    rm -rf "$work"; mkdir -p "$work"
    local arc="$work/ragel-$ver.tar.gz"
    local dl_ok=0
    # colm.net 官方源优先, github 镜像兜底
    if agxxdeps_download "https://www.colm.net/files/ragel/ragel-$ver.tar.gz" "$arc"; then
        dl_ok=1
    elif agxxdeps_download \
        "https://github.com/colm.net/ragel/archive/refs/tags/$ver.tar.gz" "$arc"; then
        dl_ok=1
    fi
    if [[ "$dl_ok" != "1" ]]; then
        agxxdeps_error "ragel 下载失败 (需要 make/g++ 工具链); 可先安装系统 ragel 后重试"
        return 1
    fi
    agxxdeps_extract "$arc" "$work" || return 1
    # 源码树可能为 ragel-$ver/ 或 ragel-$ver/ragel/ (github archive 布局)
    [[ -d "$src" ]] || src="$work/ragel-$ver/ragel"
    [[ -f "$src/configure" ]] || src="$work/ragel-$ver"
    (
        cd "$src" || exit 1
        # 官方 tar.gz 自带 configure; github archive 需要 autoreconf
        if [[ ! -x ./configure ]]; then
            command -v autoreconf >/dev/null 2>&1 || { echo "no autoreconf"; exit 2; }
            autoreconf -i || exit 2
        fi
        ./configure --prefix="$install_dir" || exit 1
        make -j"$agxxdeps_parallel" || exit 1
        make install || exit 1
    ); local rc=$?
    rm -rf "$work"
    if [[ $rc -ne 0 || ! -x "$install_bin" ]]; then
        if command -v ragel >/dev/null 2>&1; then
            agxxdeps_warn "ragel 自建失败, 回退使用系统 ragel: $(command -v ragel)"
            return 0
        fi
        agxxdeps_error "ragel 构建失败, 且系统无 ragel; 请安装 (apt install ragel) 或关闭 hyperscan (AGENTXX_ENABLE_HYPERSCAN=OFF)"
        return 1
    fi
    agxxdeps_info "ragel $ver 完成: $install_bin"
    echo "$install_dir/bin"
    return 0
}

# ===== Android NDK 路径探测 =====
# 用法: agxxdeps_ndk_root; 成功时输出 NDK 路径
agxxdeps_ndk_root() {
    local ndk="$ANDROID_NDK_ROOT"
    if [[ -z "$ndk" ]]; then
        ndk="$ANDROID_NDK_HOME"
    fi
    if [[ -z "$ndk" || ! -f "$ndk/build/cmake/android.toolchain.cmake" ]]; then
        agxxdeps_error "未找到 Android NDK (请设置 ANDROID_NDK_ROOT)"
        return 1
    fi
    echo "$ndk"
}

# ===== Boost for Android (Boost-for-Android 交叉编译) =====
# 用法: agxxdeps_ensure_boost_android <ndk> <install_dir> <abi> <variant>
#   安装目录按 ABI 分子目录, 如 boost-android-build-release/arm64-v8a
agxxdeps_ensure_boost_android() {
    local ndk="$1" install_dir="$2" abi="$3" variant="$4"
    local bfa="$agxxdeps_src_dir/boost-android"
    local tag="boost-android-${variant}"
    if [[ "${AGENTXX_DEPS_FORCE:-0}" == "1" ]]; then
        rm -f "$(agxxdeps_mark "$tag")"
    fi
    if agxxdeps_done "$tag" "$install_dir/$abi" \
            "$install_dir/$abi/include/boost/version.hpp" \
            "$install_dir/$abi/lib/libboost_filesystem.a"; then
        agxxdeps_info "Boost-Android($variant/$abi) 已构建, 跳过"
        return 0
    fi
    if [[ ! -x "$bfa/build-android.sh" ]]; then
        agxxdeps_warn "boost-android 工具不存在: $bfa, 尝试自动 clone ..."
        rm -rf "$bfa"
        (cd "$agxxdeps_src_dir" && git clone --depth 1 \
            https://github.com/coolight7/Boost-for-Android boost-android) || {
                agxxdeps_error "boost-android clone 失败; 可手动: cd $agxxdeps_src_dir && git clone https://github.com/coolight7/Boost-for-Android boost-android"
                return 1; }
    fi
    agxxdeps_info "=============================================="
    agxxdeps_info "开始自编译 Boost for Android ($variant/$abi)"
    agxxdeps_info "  NDK: $ndk"
    agxxdeps_info "  安装: $install_dir/$abi"
    agxxdeps_info "=============================================="
    mkdir -p "$install_dir"
    export CXXFLAGS="-fPIC"
    export CFLAGS="-fPIC"
    # --clean: 清掉脚本缓存, 保证每次全新 (boost 源码目录由脚本管理)
    # 注意: 脚本内部会下载/解压 boost_1_92_0 并 patch (首次较慢)
    (cd "$bfa" && ./build-android.sh "$ndk" \
        --boost=1.92.0 \
        --toolchain=llvm \
        --layout=system \
        --arch="$abi" \
        --target-version=21 \
        --prefix="$install_dir") || {
            unset CXXFLAGS CFLAGS
            agxxdeps_error "Boost-Android 构建失败 ($variant/$abi)"
            return 1
        }
    unset CXXFLAGS CFLAGS
    if [[ ! -f "$install_dir/$abi/include/boost/version.hpp" || \
          ! -f "$install_dir/$abi/lib/libboost_filesystem.a" ]]; then
        agxxdeps_error "Boost-Android($abi) 产物缺失: $install_dir/$abi"
        return 1
    fi
    echo "$abi $variant $(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$(agxxdeps_mark "$tag")"
    agxxdeps_info "Boost-Android($variant/$abi) 完成"
    return 0
}

# ===== OpenSSL for Android =====
# 用法: agxxdeps_ensure_openssl_android <ndk> <install_dir> <abi>
#   安装目录按 ABI 分子目录, 如 OpenSSL-android-build/arm64-v8a
agxxdeps_ensure_openssl_android() {
    local ndk="$1" install_dir="$2" abi="$3"
    local tag="openssl-android"
    local ossl_src=""
    for cand in "$agxxdeps_src_dir/openssl-4.0.1" "$agxxdeps_src_dir"/openssl* "$agxxdeps_src_dir"/OpenSSL*; do
        if [[ -f "$cand/Configure" ]]; then
            ossl_src="$cand"; break
        fi
    done
    if [[ "${AGENTXX_DEPS_FORCE:-0}" == "1" ]]; then
        rm -f "$(agxxdeps_mark "$tag")"
    fi
    if agxxdeps_done "$tag" "$install_dir/$abi" \
            "$install_dir/$abi/include/openssl/opensslv.h"; then
        agxxdeps_info "OpenSSL-Android($abi) 已构建, 跳过"
        return 0
    fi
    if [[ -z "$ossl_src" || ! -f "$ossl_src/Configure" ]]; then
        agxxdeps_warn "OpenSSL 源码不存在: $agxxdeps_src_dir/openssl-4.0.1, 尝试自动下载 ..."
        local tmp_arc="$agxxdeps_src_dir/tools/_openssl-4.0.1.tar.gz"
        mkdir -p "$agxxdeps_src_dir/tools"
        agxxdeps_download "https://github.com/openssl/openssl/archive/refs/tags/openssl-4.0.1.tar.gz" "$tmp_arc" || {
            agxxdeps_error "OpenSSL 源码下载失败, 请手动下载解压到: $agxxdeps_src_dir/openssl-4.0.1"
            return 1; }
        rm -rf "$agxxdeps_src_dir/tools/_ossl_extract"
        mkdir -p "$agxxdeps_src_dir/tools/_ossl_extract"
        agxxdeps_extract "$tmp_arc" "$agxxdeps_src_dir/tools/_ossl_extract" || return 1
        # github archive 解压目录名为 <repo>-<tag> (openssl-openssl-4.0.1)
        local _extracted=""
        _extracted=$(find "$agxxdeps_src_dir/tools/_ossl_extract" -maxdepth 1 -type d -name "openssl-*" | head -n1)
        if [[ -z "$_extracted" ]]; then
            agxxdeps_error "OpenSSL 解压目录名异常"
            return 1
        fi
        rm -rf "$agxxdeps_src_dir/openssl-4.0.1"
        mv "$_extracted" "$agxxdeps_src_dir/openssl-4.0.1"
        rm -rf "$agxxdeps_src_dir/tools/_ossl_extract" "$tmp_arc"
        ossl_src="$agxxdeps_src_dir/openssl-4.0.1"
    fi
    agxxdeps_info "=============================================="
    agxxdeps_info "开始自编译 OpenSSL for Android ($abi)"
    agxxdeps_info "  NDK: $ndk"
    agxxdeps_info "  安装: $install_dir/$abi"
    agxxdeps_info "=============================================="
    mkdir -p "$install_dir"

    # 与 docs/zh-cn/build/android.md 一致的构建方式:
    # openssl 的 android-* 目标自行读取 ANDROID_NDK_ROOT 并推导 clang 前缀
    local ndk_bin="$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin"
    export ANDROID_NDK_ROOT="$ndk"
    export PATH="$ndk_bin:$PATH"
    local target="$abi"
    case "$abi" in
        arm64-v8a)   target="android-arm64" ;;
        armeabi-v7a) target="android-armeabi-v7a" ;;
        x86_64)      target="android-x86_64" ;;
        x86)         target="android-x86" ;;
        *) agxxdeps_error "不支持的 Android ABI: $abi"; return 1 ;;
    esac

    (cd "$ossl_src" && make distclean >/dev/null 2>&1 || true)
    (cd "$ossl_src" && \
        ./Configure "$target" -fpic no-shared no-apps no-docs no-tests no-ocsp \
            --prefix="$install_dir/$abi" --openssldir="$install_dir/$abi" \
            '-Wl,-rpath,$(LIBRPATH)' -D__ANDROID_API__=21 \
        && make -j"$agxxdeps_parallel" \
        && make install) || {
            agxxdeps_error "OpenSSL-Android($abi) 构建失败, 源码目录: $ossl_src"
            return 1
        }
    if [[ ! -f "$install_dir/$abi/include/openssl/opensslv.h" || \
          ! -f "$install_dir/$abi/lib/libssl.a" ]]; then
        agxxdeps_error "OpenSSL-Android($abi) 产物缺失: $install_dir/$abi"
        return 1
    fi
    echo "$abi $(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$(agxxdeps_mark "$tag")"
    agxxdeps_info "OpenSSL-Android($abi) 完成"
    return 0
}
