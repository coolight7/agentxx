# Linux 可执行程序/动态库 编译

- 系统环境: Linux
- C++ 标准: Requires C++26+.
- 编译器推荐: Linux/gcc 16.1. 此前使用 gcc 13.2 编译时，部分协程函数会导致编译器自身崩溃

## 开始
- 安装或编译 Boost 1.92
- 安装可以通过系统包管理器直接安装，但需要注意版本，推荐和我们的开发版本一致 `1.92`
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

export CXXFLAGS="-fPIC"
export CFLAGS="-fPIC"

./b2 install --layout=system --prefix="${boost_install_debug_dir}" cflags=-fPIC cxxflags=-fPIC link=static runtime-link=shared runtime-debugging=on address-model=64 variant=debug 

./b2 install --layout=system --prefix="${boost_install_release_dir}" cflags=-fPIC cxxflags=-fPIC link=static runtime-link=shared runtime-debugging=off address-model=64 variant=release

# 如果调整了一些参数想重新构建，可以先执行清理:
# ./b2 --clean-all
# rm -rf bin.v2
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
cd {项目根目录}
./agent/script/linux_debug_build.sh
./agent/build/linux-debug/exec/agentxx_cli
```
- - release 编译可以运行:
```sh
cd {项目根目录}
./agent/script/linux_release_build.sh
./agent/build/linux-release/exec/agentxx_cli
```

## 产物布局

- 可执行文件: `agent/build/{platform}-{mode}/exec/agentxx_cli` / `agentxx_test` / `agentxx_benchmark`
- 插件动态库 (独立动态库模式): `agent/build/{platform}-{mode}/exec/plugins/<插件名>/` (含 `plugin.yaml` 清单时按目录分派)
- 共享库 (FFI): `agent/build/{platform}-{mode}/lib/libagentxx_shared.so` (导出 C 符号见 `agent/lib/ffi_symbols.map`)

## Debug 构建加速

Debug 构建脚本 (`script/linux_debug_build.sh`) 默认已启用以下加速手段，可直接使用:

| 手段 | 说明 |
| --- | --- |
| ccache | 缓存编译结果，增量/重复/切分支构建大幅提速。未安装时自动跳过 (`apt-get install ccache`)。缓存目录默认 `~/.cache/ccache-agentxx`，可用环境变量 `CCACHE_DIR` / `CCACHE_MAXSIZE` (默认 3G) 覆盖 |
| 快速链接器 | 自动检测并使用 mold (>25x 于默认 bfd，大体积二进制链接尤其明显)，其次 gold；均无则用默认。可用 `-DAGENTXX_LINKER=mold\|gold\|bfd` 强制指定 |
| PCH | 预编译稳定第三方头 (std/fmt/asio/boost.exception 等)，项目自身头不参与，改动项目头不会触发全量重编。可用 `-DAGENTXX_ENABLE_PCH=OFF` 关闭 |
| 并行度 | 并行任务数默认取 CPU 核数 (原为 4)，可用环境变量 `AGENTXX_BUILD_PARALLEL` 覆盖，例如内存不足/编译器 ICE 时: `AGENTXX_BUILD_PARALLEL=4 ./script/linux_debug_build.sh` |
| 源码单次编译 | GCC/Clang 下 lib 的 62 个源文件经 OBJECT 库只编译一次，动态/静态库复用同一批 .o (原为编译两次)，编译时间近乎减半 |

### 更快的可选配置

默认 Debug 构建保留 AddressSanitizer 与调试符号 (`-g`)。若日常迭代不需要 ASAN/断点符号，可通过 cmake 选项显著加快编译与链接 (产物缩小 3 倍左右):

```sh
cmake -B build/linux-debug -S agent \
    -DAGENTXX_ENABLE_ASAN=OFF \        # 关闭 AddressSanitizer
    ...其余参数与 linux_debug_build.sh 一致
```

- `AGENTXX_ENABLE_ASAN=OFF`: 去掉 `-fsanitize=address`，编译与链接均显著加快 (仍保留 `-fno-omit-frame-pointer` 便于调试/性能分析)
- 建议日常开发全开 OFF，提交前/定位疑难问题时再开回 ON 全量构建验证一次

## 常见错误
- [FAQ 更多问题](FAQ.md)

### linux 上运行到 网络相关的代码会卡很久?
- 可能是 asio 版本问题，在某些 Boost 版本(如 1.91)的asio可能在启用 io_uring 后有 bug，可以更换其他 Boost 版本。