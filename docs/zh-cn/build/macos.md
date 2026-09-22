# macOS 可执行程序/动态库 编译

- 系统环境: macOS
- C++ 标准: Requires C++26+.
- 编译器推荐: Apple clang (Xcode Command Line Tools) 16+，推荐最新版 Xcode；也支持 Homebrew 的 GNU g++ 14+
- 架构: Apple Silicon (arm64) 与 Intel (x86_64) 均支持
- 关联: [Linux 编译](linux.md) / [Windows 编译](windows.md)

---
- 有两种方式开始编译
  - [自动编译脚本](#自动编译脚本); 自动处理了大部分操作
  - [手动编译](#手动编译); 手动控制 Boost、OpenSSL 等库的编译、版本、参数
---

## 自动编译脚本

- 执行构建脚本即可 (`agent/script/macos_debug_build.sh` / `macos_release_build.sh`)
- 脚本会自动准备全部编译环境与依赖库, 无需手动安装 brew 库，自动处理:
>
> 1. **环境前置检查**: `cmake` / `make` / `curl|wget` / `tar` / `python3`
>    与 C++26 编译器 (Apple clang>=16 或 g++>=14), 缺失时直接报错并给出安装提示。
> 2. **依赖自构建** (全部使用自己编译的库, 不用 Homebrew/系统安装库):
>    `Boost 1.92` (debug/release 两版) 与 `OpenSSL 4.0.1` 由源码自动编译到
>    `agent/third_party/boost-macos-build-{debug,release}/` 与
>    `agent/third_party/OpenSSL-macos-build/`; 已有产物自动复用。
>    `ragel` (hyperscan 代码生成器) 优先用系统已装版本, 缺失时下载源码自建
>    (仅在 `AGENTXX_ENABLE_HYPERSCAN=ON` 时需要)。
>
- 相关环境变量 (可选):

| 变量 | 说明 |
| --- | --- |
| `AGENTXX_SKIP_AUTO_DEPS=1` | 跳过自动构建 (依赖目录缺失时直接报错) |
| `AGENTXX_DEPS_FORCE=1` | 忽略复用, 强制重建依赖 (ragel 除外, 它优先复用系统版) |
| `BOOST_ROOT` / `OPENSSL_ROOT_DIR` | 手动指定已安装路径, 优先于自动构建 |
| `AGENTXX_ENABLE_HYPERSCAN=ON\|OFF` | hyperscan 开关; **arm64 默认 OFF** (见下), x86_64 默认 ON |
| `AGENTXX_BUILD_PARALLEL=N` | 并行任务数 (debug 默认 4, release 默认 CPU 核数) |
| `AGENTXX_ENABLE_MIMALLOC=OFF` | mimalloc 内存分配器开关; **macOS/iOS 上不可用且被强制关闭** (见下), 显式传 ON 也会被自动关闭 |
| `AGENTXX_MIMALLOC_LINK=SHARED` | 仅 Linux/Windows 生效; macOS 上无实际意义 (mimalloc 已关闭) |
| `AGENTXX_SKIP_STRIP=1` | release 跳过 strip 符号裁剪 |
| `AGENTXX_PACKAGE_RELEASE=1` | release 构建后自动打 `.tar.gz` 发布包 |

- **hyperscan 与 CPU 架构**:
  - 上游 hyperscan 仅实现了 x86 后端 (SSE4.2 / AVX)，**Apple Silicon (arm64) 无实现**。
    构建脚本在 arm64 上自动设置 `AGENTXX_ENABLE_HYPERSCAN=OFF`，正则能力回退到
    `std::regex` (功能一致, 仅性能略低)。
  - Intel (x86_64) mac 可保持默认 `ON`，此时需要 `ragel` (脚本会自动准备)。

## 手动编译

- 准备编译环境:
```sh
# 安装 Apple 命令行工具 (提供 clang++/make/git 等)
xcode-select --install

# 安装 cmake (可选 ccache 加速编译)
brew install cmake ccache
```
- 编译器要求 Apple clang >= 16 (或 g++ >= 14)

### 编译 Boost 1.92

- 可以通过 Homebrew 安装，但需要注意版本，推荐和我们的开发版本一致 `1.92`
- 自行编译:
```sh
# https://github.com/boostorg/boost/releases/
# 下载 release/boost-xxx-cmake.tar.gz 解压到 agent/third_party/boost/
cd boost/
./bootstrap.sh

# 创建 third_party/boost-macos-build-debug 和 third_party/boost-macos-build-release 目录
boost_source_dir=$PWD

boost_install_debug_dir="${boost_source_dir}/../boost-macos-build-debug/"
mkdir -p "$boost_install_debug_dir"
boost_install_debug_dir=$(cd "$boost_install_debug_dir" && pwd)

boost_install_release_dir="${boost_source_dir}/../boost-macos-build-release/"
mkdir -p "$boost_install_release_dir"
boost_install_release_dir=$(cd "$boost_install_release_dir" && pwd)

cd "$boost_source_dir"

./b2 install --layout=system --prefix="${boost_install_debug_dir}" link=static runtime-link=shared runtime-debugging=on address-model=64 variant=debug

./b2 install --layout=system --prefix="${boost_install_release_dir}" link=static runtime-link=shared runtime-debugging=off address-model=64 variant=release

# 如果调整了一些参数想重新构建，可以先执行清理:
# ./b2 --clean-all
# rm -rf bin.v2
```

### 源码编译 openssl

- 编译
```sh
cd {项目根目录}/agent/third_party/
curl -fL -o openssl-4.0.1.tar.gz https://github.com/openssl/openssl/archive/refs/tags/openssl-4.0.1.tar.gz
tar -xzf openssl-4.0.1.tar.gz
mv openssl-openssl-4.0.1 openssl-4.0.1
cd openssl-4.0.1

openssl_source_dir=$PWD
openssl_build_dir="$openssl_source_dir/../OpenSSL-macos-build"
mkdir -p "$openssl_build_dir"
openssl_build_dir=$(cd "$openssl_build_dir" && pwd)

cd "$openssl_source_dir"

# 静态库 (no-shared)；Configure 会自动识别 darwin64-arm64 / darwin64-x86_64
./Configure no-shared --prefix="$openssl_build_dir" --openssldir="$openssl_build_dir"
make -j"$(sysctl -n hw.ncpu)"
make install
```

### agentxx 编译

- 启动编译 agentxx，会自动下载其他依赖库，编译成功后自动运行 命令行 client:
```sh
cd {项目根目录}
./agent/script/macos_debug_build.sh
./agent/build/macos-debug/exec/agentxx_cli
```
- release 编译可以运行:
```sh
cd {项目根目录}
./agent/script/macos_release_build.sh
./agent/build/macos-release/exec/agentxx_cli
```

## 编译结果

- 可执行文件: `agent/build/{platform}-{mode}/exec/agentxx_cli` / `agentxx_test` / `agentxx_benchmark`
- 插件动态库 (独立动态库模式): `agent/build/{platform}-{mode}/exec/plugins/<插件名>/` (含 `plugin.yaml` 清单时按目录分派)
- 共享库 (FFI): `agent/build/{platform}-{mode}/exec/libagentxx.dylib` (导出 C 符号见 `agent/lib/ffi_symbols.map`)
- 运行期动态库搜索路径: 上述可执行文件与共享库都写入 `@executable_path` / `@loader_path`
  (RUNPATH 的 Mach-O 等价物)，即 **优先从产物自身所在目录** 搜索动态库依赖。
  该路径由 `agent/cmake/agentxx_runtime_search_path.cmake` 在构建/安装期写入
  (可执行文件为 `@executable_path`, `libagentxx.dylib` 为 `@loader_path`,
   `exec/plugins/<插件名>/*.dylib|*.so` 为 `@loader_path:@loader_path/../..` 以回查 exec)
- **代码签名**: arm64 上可执行文件/动态库在链接期会由 clang 自动 ad-hoc 签名,
  才能被系统加载; release 脚本 strip 后会重新执行 `codesign --force --sign -`
  恢复签名 (否则运行报 `zsh: killed`)。分发给其他机器时如从网络下载,
  可能还需去除隔离属性 (`xattr -dr com.apple.quarantine <文件>`)。
- **性能基准 (`agentxx_benchmark`)**: 可正常编译运行, 但其资源基准的细项指标
  (RSS/PSS/私有脏页、`smaps` 模块级分解、glibc 堆在用/碎片/`malloc_trim` 可回收、
  `/proc/meminfo` 总内存等) 依赖 Linux `/proc`/glibc, 在 macOS 上不可用, 相应字段
  会显示为 `n/a` 或 0; 建议在 Linux 上采集完整基准数据 (见
  [benchmark.md](../design/benchmark.md))。自身可执行路径已适配 macOS
  (`_NSGetExecutablePath`), 插件/FFI 场景仍可定位产物。

## 内存分配器 (mimalloc)

**macOS/iOS 上不可用 (构建时自动关闭)**。原因: Mach-O 使用**两层次命名空间**,
`libc++.dylib` 与系统框架对 `malloc/free` 的引用在链接期已绑定到 `libSystem`
(ordinal 固定), 运行期 dyld 不会再到主可执行文件解析; 主可执行文件里静态覆盖的
分配器只作用于自身 (且 `_malloc` 并不在导出符号表中)。于是进程内出现"程序侧
mimalloc / 框架侧系统分配器"并存, 任何跨模块传递并释放的指针都会在 `mi_free`
崩溃 (实测 Release `agentxx_cli` 启动即崩溃)。

- 这与 Linux/ELF 不同: GNU ld 会把可执行文件定义、且被共享库引用的 `malloc`
  放进 `.dynsym`, 运行期符号插入 (interposition) 使全进程统一走 mimalloc,
  因此在 Linux 上静态覆盖有效。
- 如需在 macOS 强制接管分配器, 只能用 `DYLD_INSERT_LIBRARIES` 预加载 mimalloc
  动态库 (进程外注入, 非本项目自包含分发方式), 故构建系统在 macOS/iOS 直接关闭
  该功能 (顶层 `CMakeLists.txt` 自动 `AGENTXX_ENABLE_MIMALLOC=OFF`)。
- macOS Release 构建因此使用系统分配器 (libmalloc), 内存/CPU 实测对比见
  [benchmark.md 第 9 节](../design/benchmark.md) (数据来自 Linux)。

## Debug 构建加速

Debug 构建脚本 (`script/macos_debug_build.sh`) 默认已启用以下加速手段，可直接使用:

| 手段 | 说明 |
| --- | --- |
| ccache | 缓存编译结果，增量/重复/切分支构建大幅提速。未安装时自动跳过 (`brew install ccache`)。缓存目录默认 `~/.cache/ccache-agentxx`，可用环境变量 `CCACHE_DIR` / `CCACHE_MAXSIZE` (默认 3G) 覆盖 |
| Ninja | 已安装 `ninja` 时自动使用 (`brew install ninja`)，否则回退 CMake 默认的 Unix Makefiles |
| PCH | 预编译稳定第三方头 (std/fmt/asio/boost.exception 等)，项目自身头不参与，改动项目头不会触发全量重编。release 默认开启，debug 脚本默认关闭。可用 `-DAGENTXX_ENABLE_PCH=ON/OFF` 控制 |
| 并行度 | debug 默认 4，release 默认 CPU 核数，可用环境变量 `AGENTXX_BUILD_PARALLEL` 覆盖，例如内存不足/编译器 ICE 时: `AGENTXX_BUILD_PARALLEL=2 ./script/macos_debug_build.sh` |
| 源码单次编译 | Clang 下 lib 的源文件经 OBJECT 库只编译一次，动态/静态库复用同一批 .o，编译时间近乎减半 |

### 更快的可选配置

默认 Debug 构建开启 sanitizer 插桩 (ASan + UBSan + 插件框架定向探针) 与调试符号 (`-g`)。若日常迭代不需要插桩/断点符号，可通过 cmake 选项显著加快编译与链接 (产物缩小 3 倍左右):

```sh
cmake -B build/macos-debug -S agent \
    -DAGENTXX_ENABLE_SANITIZER=OFF \   # 关闭 ASan + UBSan + 插件框架探针
    ...其余参数与 macos_debug_build.sh 一致
```

- `AGENTXX_ENABLE_SANITIZER=OFF`: 去掉 `-fsanitize=address` / `-fsanitize=undefined` (含插件框架定向探针)，编译与链接均显著加快 (仍保留 `-fno-omit-frame-pointer` 便于调试/性能分析)。插桩与定向探针只在非 Release 配置生效，Release 构建不含 sanitizer 参数
- `AGENTXX_ENABLE_LTO=OFF`: Release 构建关闭 LTO (`-flto=thin`)，链接更快、产物更易调试 (默认 ON)
- 建议日常开发全开 OFF，提交前/定位疑难问题时再开回 ON 全量构建验证一次

## 常见错误
- [FAQ 更多问题](FAQ.md)

### hyperscan 在 Apple Silicon 上编译失败 / 找不到 hs_*
- 上游 hyperscan 只有 x86 后端，arm64 上无法构建。构建脚本已自动
  `AGENTXX_ENABLE_HYPERSCAN=OFF`；手动 cmake 时请显式加上该参数，
  正则实现会自动回退到 `std::regex`。

### 运行时报 `zsh: killed` 或 `Killed: 9`
- arm64 macOS 要求可执行文件带有效代码签名。对产物做过 `strip` / 修改后签名失效，
  需重新 ad-hoc 签名:
```sh
codesign --force --sign - path/to/agentxx_cli
```
- 从网络下载的发布包可能带隔离属性，可执行:
```sh
xattr -dr com.apple.quarantine path/to/exec/
```

### 链接错误 `ld: unknown options: --gc-sections` / `--version-script`
- 这些是 ELF (Linux/Android) 专用链接选项。本项目已在 macOS 分支改用
  Mach-O 等价写法 (`-Wl,-dead_strip` / `-Wl,-exported_symbols_list`)，如遇到
  说明用了旧的构建缓存或第三方脚本注入了 Linux 选项，清理 build 目录后重试。

### 边运行边重建
- `exec/` 目录里的可执行文件、动态库、插件可能正被**运行中的** `agentxx_cli` 映射到内存。
- macOS 上覆盖正在运行的可执行文件通常被系统拒绝 (Text file busy) 或导致运行中的
  进程异常，建议先退出再重建。
