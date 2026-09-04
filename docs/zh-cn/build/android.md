# Linux 交叉编译 android 动态库

- 系统环境: Linux
- C++ 标准: Requires C++26+.
- 编译器推荐: Linux/NDK r29/Clang 21.0.0

---
- 有两种方式开始编译 
  - [自动编译脚本](#自动编译脚本); 自动处理了大部分操作
  - [手动编译](#手动编译); 手动控制 Boost、OpenSSL 等库的编译、版本、参数
---

## 自动编译脚本

- 执行构建脚本即可 (`agent/script/cross_android_release_build.sh`)
- 编译脚本会自动准备全部编译环境与依赖库, 无需手动安装 apt 库，自动处理:
>
> 1. **环境前置检查**: `cmake` / `ninja` / `make` / `git` 与 `ANDROID_NDK_ROOT`
>    (需含 `build/cmake/android.toolchain.cmake`), 缺失时直接报错。
> 2. **依赖自交叉编译** (全部使用自己编译的库, 不用系统或 apt 安装库):
>    `Boost 1.92` 经 `third_party/boost-android` (缺失时自动 `git clone`)
>    交叉编译到 `third_party/boost-android-build-release/<abi>/`;
>    `OpenSSL 4.0.1` 经 NDK clang 交叉编译到
>    `third_party/OpenSSL-android-build/<abi>/`;
>    已有产物自动复用 (android 默认 `hyperscan OFF`, 不需要 ragel)。
>
- 相关环境变量 (可选):

| 变量 | 说明 |
| --- | --- |
| `ANDROID_NDK_ROOT` | **必需** (未手动指定 `BOOST_ROOT`+`OPENSSL_ROOT_DIR` 时), NDK 根目录 |
| `AGENTXX_SKIP_AUTO_DEPS=1` | 跳过自动构建 (依赖目录缺失时直接报错) |
| `AGENTXX_DEPS_FORCE=1` | 忽略复用, 强制重建依赖 |
| `BOOST_ROOT` / `OPENSSL_ROOT_DIR` | 手动指定已交叉编译产物路径, 优先于自动构建 |
| `AGENTXX_BUILD_PARALLEL=N` | 并行任务数 (默认 4) |

## 手动编译

- 准备编译环境, 需要安装 `cmake`、`ninja-build`、`make`、`git` 并设置
  `ANDROID_NDK_ROOT` 指向 NDK 目录

### 编译 Boost
- 手动自行编译 Boost 1.92 :
```sh
# https://github.com/boostorg/boost/releases/
# 下载 release/boost-xxx-cmake.tar.gz 解压到 agent/third_party/boost/

cd agent/third_party/
git clone https://github.com/coolight7/Boost-for-Android boost-android
cd boost-android

# 创建 third_party/boost-android-build-debug 和 third_party/boost-android-build-release 目录
boost_source_dir=$PWD

boost_install_debug_dir="${boost_source_dir}/../boost-android-build-debug/"
mkdir -p "$boost_install_debug_dir"
boost_install_debug_dir=$(cd "$boost_install_debug_dir" && pwd)

boost_install_release_dir="${boost_source_dir}/../boost-android-build-release/"
mkdir -p "$boost_install_release_dir"
boost_install_release_dir=$(cd "$boost_install_release_dir" && pwd)

cd "$boost_source_dir"

export CXXFLAGS="-fPIC"
export CFLAGS="-fPIC"

./build-android.sh --boost=1.92.0 \
    --prefix=$boost_install_release_dir \
    --toolchain=llvm \
    --layout=system \
    --arch=arm64-v8a,armeabi-v7a,x86,x86_64 \
    --target-version=21
```
### 源码编译 openssl
- 编译
```sh
cd {项目根目录}/agent/third_party/
wget https://github.com/openssl/openssl/releases/download/openssl-4.0.1/openssl-4.0.1.tar.gz
tar -xzvf openssl-4.0.1.tar.gz
cd openssl-4.0.1

openssl_source_dir=$PWD
openssl_build_dir="$openssl_source_dir/../OpenSSL-android-build"
mkdir -p "$openssl_build_dir"
openssl_build_dir=$(cd "$openssl_build_dir" && pwd)

cd "$openssl_source_dir"

# 修改 [ANDROID_NDK_ROOT] 为你的ndk路径
export ANDROID_NDK_ROOT="/home/coolight/app/android_sdk/ndk/29.0.14206865"
export PATH="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH"

./Configure --release android-arm64 -fpic no-shared no-apps no-docs no-tests no-ocsp --prefix="$openssl_build_dir/arm64-v8a" --openssldir="$openssl_build_dir/arm64-v8a" '-Wl,-rpath,$(LIBRPATH)' -D__ANDROID_API__=21
make -j
make install

./Configure --release android-armeabi-v7a -fpic no-shared no-apps no-docs no-tests no-ocsp --prefix="$openssl_build_dir/armeabi-v7a" --openssldir="$openssl_build_dir/armeabi-v7a" '-Wl,-rpath,$(LIBRPATH)' -D__ANDROID_API__=21
make -j
make install
```

### agentxx 编译
- 启动 release 编译可以执行:
```sh
cd {项目根目录}/agent
./script/cross_android_release_build.sh
```

## 编译结果

- 可执行文件: `agent/build/{platform}-{mode}/exec/agentxx_cli` / `agentxx_test` / `agentxx_benchmark`
- 插件动态库 (独立动态库模式): `agent/build/{platform}-{mode}/exec/plugins/<插件名>/` (含 `plugin.yaml` 清单时按目录分派)
- 共享库 (FFI): `agent/build/{platform}-{mode}/lib/libagentxx_shared.so` (导出 C 符号见 `agent/lib/ffi_symbols.map`)

## 常见错误
- [FAQ 更多问题](FAQ.md)