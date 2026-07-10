# Linux 可执行程序/动态库 编译

- 系统环境: Linux
- C++ 标准: Requires C++26+.
- 编译器推荐: Linux/gcc 16.1. 此前使用 gcc 13.2 编译时，部分协程函数会导致编译器自身崩溃

## 开始
- 安装或编译 Boost 1.91
- 安装可以通过系统包管理器直接安装，但需要注意版本，推荐和我们的开发版本一致 `1.91`
- 自行编译:
```sh
# https://github.com/boostorg/boost/releases/
# 下载 release/boost-xxx-cmake.tar.gz 解压到 agent/third_party/boost/
cd boost/
./bootstrap.sh

# 创建 third_party/boost-linux-build-debug 和 third_party/boost-linux-build-release 目录
boost_source_dir=$PWD

boost_install_debug_dir="${boost_source_dir}/../boost-linux-build-debug/"
mkdir -p "$boost_install_debug_dir"
boost_install_debug_dir=$(cd "$boost_install_debug_dir" && pwd)

boost_install_release_dir="${boost_source_dir}/../boost-linux-build-release/"
mkdir -p "$boost_install_release_dir"
boost_install_release_dir=$(cd "$boost_install_release_dir" && pwd)

cd "$boost_source_dir"

./b2 install --layout=system --prefix="${boost_install_debug_dir}" link=static runtime-link=shared runtime-debugging=on address-model=64 variant=debug

./b2 --clean-all

./b2 install --layout=system --prefix="${boost_install_release_dir}" link=static runtime-link=shared runtime-debugging=off address-model=64 variant=release

# 如果想重新构建，可以先执行清理:
# ./b2 --clean-all
```
### 源码编译 openssl
- 编译
```sh
cd {项目根目录}/agent/third_party/
wget https://github.com/openssl/openssl/releases/download/openssl-4.0.1/openssl-4.0.1.tar.gz
tar -xzvf openssl-4.0.1.tar.gz
cd openssl-4.0.1

openssl_source_dir=$PWD
openssl_build_dir="$openssl_source_dir/../OpenSSL-linux-build"
mkdir -p "$openssl_build_dir"
openssl_build_dir=$(cd "$openssl_build_dir" && pwd)

cd "$openssl_source_dir"

./Configure no-shared --pic --prefix="$openssl_build_dir" --openssldir="$openssl_build_dir" '-Wl,-rpath,$(LIBRPATH)'
make
make install
```
### agentxx 编译
- 启动编译 agentxx，会自动下载其他依赖库，编译成功后自动运行 命令行 client:
```sh
cd {项目根目录}/agent
./script/client_run.sh
```
- - release 编译可以运行:
```sh
cd {项目根目录}/agent
./script/release_build.sh
```

## 常见错误
- [FAQ 更多问题](FAQ.md)