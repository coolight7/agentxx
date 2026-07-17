# Linux 交叉编译 android 动态库

- 系统环境: Linux
- C++ 标准: Requires C++26+.
- 编译器推荐: Linux/NDK r29/Clang 21.0.0

## 开始
- 自行编译 Boost 1.91.0 :
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

./build-android.sh --boost=1.91.0 \
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
- 启动编译 agentxx, release 编译可以运行:
```sh
cd {项目根目录}/agent
./script/android_release_build.sh
```

## 常见错误
- [FAQ 更多问题](FAQ.md)