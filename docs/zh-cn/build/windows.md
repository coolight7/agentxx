# Windows 可执行程序/动态库 编译

- 系统环境: Windows
- C++ 标准: Requires C++26+.
- 编译器: MSVC
- 关联: [Linux交叉编译windows可执行程序、动态库](cross-linux-for-windows.md)

## 编译环境准备

### 安装 Visual-Studio-2026
- 推荐 VS2026；也可以尝试 VS2022 未验证是否可以编译通过

### 安装 CMake
- 安装 cmake
    - https://cmake.org/download/
    - 选择win版本安装，如: cmake-x.x.x-windows-x86_64.msi
    - https://github.com/Kitware/CMake/releases/download/v4.4.0-rc2/cmake-4.4.0-rc2-windows-x86_64.msi

### pkg-config
- 安装 pkg-config:
```pwsh
# 管理员身份打开 powershell
Set-ExecutionPolicy Bypass -Scope Process -Force; [System.Net.ServicePointManager]::SecurityProtocol = [System.Net.ServicePointManager]::SecurityProtocol -bor 3072; iex ((New-Object System.Net.WebClient).DownloadString('https://community.chocolatey.org/install.ps1'))

choco install pkgconfiglite

pkg-config --version
```

---
- 接下来有两种方式开始编译 
  - [自动编译脚本](#自动编译脚本); 自动处理了大部分操作
  - [手动编译](#手动编译); 手动控制 Boost、OpenSSL 等库的编译、版本、参数
---

## 自动编译脚本

- 执行构建脚本即可 (`agent/script/windows_debug_build.bat` / `agent/script/windows_release_build.bat`) 
- 编译脚本会自动准备全部编译环境与依赖库, 自动处理:
>
> 1. **环境前置检查**: `cmake`、`git` (缺源码时自动下载)、
>    Visual Studio (MSVC, 需要 `VsDevCmd.bat`, vswhere 或目录扫描定位,
>    无硬编码安装路径), 缺失时直接报错并给出安装提示。
> 2. **依赖准备**:
>    `Boost 1.92` (debug/release 两版) 用本机 MSVC `b2` 自动编译到
>    `agent/third_party/boost-windows-build-{debug,release}/`;
>    `OpenSSL 4.x` 自动下载 slproweb Win64 **预编译**安装包
>    (sha256 校验) 静默安装到 `agent/third_party/OpenSSL-windows-build/`,
>    `ragel` (hyperscan 代码生成器) 优先用系统已装版本
>    (`winget install PolarGoose.Ragel`), 缺失时下载预编译包暂存到
>    `agent/third_party/tools/ragel-*/bin/`。
>
- 相关环境变量 (可选, cmd 中用 `set` 设置):

| 变量 | 说明 |
| --- | --- |
| `AGENTXX_SKIP_AUTO_DEPS=1` | 跳过自动准备 (依赖目录缺失时 cmake 直接报错) |
| `AGENTXX_OPENSSL_VERSION=x.y.z` | 固定 OpenSSL 预编译版本 (默认 manifest 最新 4.x) |
| `BOOST_ROOT` / `OPENSSL_ROOT_DIR` | 手动指定已安装路径, 优先于自动准备 |
| `AGENTXX_BUILD_PARALLEL=N` | 并行任务数 (默认 4) |
| `AGENTXX_ENABLE_HYPERSCAN=OFF` | 传给 cmake 关闭 hyperscan, 则不需要 ragel |
| `AGENTXX_MIMALLOC_LINK=SHARED` | 传给 cmake 改为动态链接 mimalloc (默认 `STATIC`) |

## 内存分配器 (mimalloc)

默认使用 [mimalloc](https://github.com/microsoft/mimalloc) 接管最终程序的
`malloc/free/new/delete` (源码为 `agent/third_party/mimalloc` 子模块, 只作用于
`agentxx_cli` / `agentxx_test` / `agentxx_benchmark`; `libagentxx.dll` 与插件动态库
跟随宿主分配器):

| cmake 参数 | 默认 | 说明 |
| --- | --- | --- |
| `-DAGENTXX_ENABLE_MIMALLOC=OFF` | ON | 关闭后回到 CRT 分配器 |
| `-DAGENTXX_MIMALLOC_LINK=SHARED` | STATIC | 链接 `mimalloc.dll` (并把 `mimalloc.dll` + `mimalloc-redirect.dll` 装到 `exec/`) |

- 注意: MSVC 使用动态 CRT (`/MD`) 时, **静态链接的 mimalloc 不会覆盖 CRT 分配器**
  (上游以 `_DLL` 判定, 避免与 CRT 分配器混用), 此时只是链上而未接管; 要在 Windows 上
  真正接管分配器请用 `-DAGENTXX_MIMALLOC_LINK=SHARED`, 由 `mimalloc-redirect.dll`
  在加载期把 CRT 的分配入口改指到 mimalloc
- 非 Release 构建默认开启 ASan (需独占 `malloc`), 此时顶层会**自动关闭** mimalloc

## 手动编译

### 编译 Boost
- 安装或编译 Boost 1.92，推荐和我们的开发版本一致 `1.92`
- 自行编译, 如果使用 windows CMD:
```sh
# https://github.com/boostorg/boost/releases/
# 下载 release/boost-xxx-cmake.7z 解压到 agent/third_party/boost/
cd boost\
.\bootstrap.bat

# 切换到 cmd
cmd

# 创建 third_party/boost-windows-build-debug 和 third_party/boost-windows-build-release 目录，并回到 boost 目录
set "boost_source_dir=%CD%"

set "boost_install_debug_dir=%CD%/../boost-windows-build-debug"
mkdir "%boost_install_debug_dir%"
cd "%boost_install_debug_dir%"
set "boost_install_debug_dir=%CD%"

set "boost_install_release_dir=%CD%/../boost-windows-build-release"
mkdir "%boost_install_release_dir%"
cd "%boost_install_release_dir%"
set "boost_install_release_dir=%CD%"

cd "%boost_source_dir%"

# release / debug 编译/安装
.\b2.exe install --layout=system --prefix="%boost_install_release_dir%" link=static runtime-link=shared runtime-debugging=off address-model=64 variant=release

.\b2.exe install --layout=system --prefix="%boost_install_debug_dir%" link=static runtime-link=shared runtime-debugging=on   address-model=64 variant=debug

# 如果想重新构建，可以先执行清理:
# .\b2.exe --clean-all
```

### 编译 openssl
- 有两种方式，任选一个:
  1. 下载预编译包: 前往下载 `https://slproweb.com/products/Win32OpenSSL.html`, 进入网页后往下滑动，找到 `Win64 OpenSSL vx.x.x`，注意没有末尾 Light，下载其 `EXE` 或 `MSI` 都行，然后运行安装，安装目录选择到 `{项目根目录}/agent/third_party/OpenSSL-windows-build/` 即可
  2. 从源码自行编译 (可选, 需要 perl): 用本机 MSVC 从源码编译 (`no-asm`, 无需 nasm), 参考 `agent/third_party/openssl-4.0.1/NOTES-WINDOWS.md`:
```sh
perl Configure VC-WIN64A no-shared no-asm no-tests no-docs nmake
nmake install_sw
```
  - - 安装目录选择到 `{项目根目录}/agent/third_party/OpenSSL-windows-build/` 即可

### 安装 Ragel
- 编译高性能正则表达式库支持`vectorize`/`hyperscan`需要，不想安装也可以在 cmake 参数加 `-DAGENTXX_ENABLE_HYPERSCAN=OFF`关闭即可。
- 构建脚本会优先用系统已装版本, 缺失时自动下载预编译包暂存, 因此手动安装可选:
- 打开 cmd，执行命令看看是否有 ragel:
```sh
ragel -v
```
- 报错`未找到可执行文件或脚本`就是没安装，可以用 winget 快速安装:
```sh
winget install --id=PolarGoose.Ragel -e

# 查看安装目录，复制 Ragel.exe 所在路径
dir %localappdata%\Microsoft\WinGet\Links\
# 效果也等同于:
%localappdata%\Microsoft\WinGet\Links\Ragel.exe -v
```

### agentxx 编译
- - 启动编译 agentxx, Boost/OpenSSL 会在缺失时自动源码构建, 其他依赖库由 cmake 自动下载构建:
```bat
cd {项目根目录}
agent\script\windows_debug_build.bat
agent\build\windows-debug\exec\agentxx_cli
```
- - release 编译可以运行:
```bat
cd {项目根目录}
agent\script\windows_release_build.bat
agent\build\windows-release\exec\agentxx_cli
```

## 编译结果

- 可执行文件: `agent/build/{platform}-{mode}/exec/agentxx_cli.exe` / `agentxx_test.exe` / `agentxx_benchmark.exe`
- 插件动态库 (独立动态库模式): `agent/build/{platform}-{mode}/exec/plugins/<插件名>/` (含 `plugin.yaml` 清单时按目录分派)
- 共享库 (FFI): `agent/build/{platform}-{mode}/lib/libagentxx_shared.dll` (导出 C 符号见 `agent/lib/ffi_symbols.map`)

## Release 构建的 LTO (链接期优化)
- Release 构建默认开启 LTO: MSVC 用 `/GL` (编译) + `/LTCG` (链接), GCC/Clang 用 `-flto`
- 如需对照排查 (例如怀疑跨模块内联引起行为差异), 可加 `-DAGENTXX_ENABLE_LTO=OFF` 关闭
- 全部产物 (lib/插件/client/test/三个基础库/依赖库) **都参与 LTO**, 没有例外项

### 符号导出: 默认隐藏, 仅导出显式标注的 API
- `cxx_utilxx_base` / `cxx_utilxx` / `cxx_pluginxx` 的动态库不再使用 CMake 的
  `WINDOWS_EXPORT_ALL_SYMBOLS` 自动导出, 改为在公开头文件用 `UTILXX_BASE_API` /
  `UTILXX_API` / `PLUGINXX_API` 标注导出符号; 未标注的一律不导出 (静态链入的
  第三方符号、std 模板实例都不再泄漏):
  - 动态库编译时得到 `dllexport`; 静态链接的使用方由目标接口自动定义
    `CXX_UTILXX_BASE_STATIC` 等宏 → 宏为空 (避免 dllimport)
  - MSVC 与 GCC/Clang 同一套标注 (后者展开为 `__attribute__((visibility("default")))`)
  - 效果 (Windows Release 实测): `libcxx_utilxx_base.dll` 导出 3144 → 208,
    `libcxx_utilxx.dll` 46163 → 119, `libcxx_pluginxx.dll` 1273 → 27,
    且逐符号比对确认公开头文件声明的 API 一个不少
- 为什么必须这样: `WINDOWS_EXPORT_ALL_SYMBOLS` 要解析每个 `.obj` 的符号表来生成
  `.def`, 而 `/GL` (LTO) 产物只有编译器中间表示、没有符号表 (dumpbin 显示
  `File Type: ANONYMOUS OBJECT`), 二者天生互斥 —— 显式导出同时解决了这个冲突,
  并让链接器能安全裁剪内部符号

### HyperScan 的 SIMD 基线
- 发布给不同用户的机器可能是 SSE4.2 基线 (x86-64-v2), 也可能没有 AVX2/AVX-512,
  因此 hyperscan 的 fat runtime (运行期按 CPU 自动选型的多微架构分发) **默认关闭**,
  改为编译期基线 `AGENTXX_HYPERSCAN_MARCH` (默认 `x86-64-v2`)。
  fat runtime 的实现依赖 `nm` + `objcopy --redefine-syms` 给各 variant 的 obj
  重命名符号, 与 LTO object 无符号表的特性互斥 (二者只能择一), 关闭后 hyperscan
  也能参与 LTO
- 需要运行期分发时可加 `-DAGENTXX_HYPERSCAN_FAT_RUNTIME=ON` (仅 Linux 生效),
  此时 hyperscan 会自动退化为非 LTO 构建

## 常见错误
- [FAQ 更多问题](FAQ.md)

### 生成错误 MSB3073 / unrecognized file format in 'xxx.obj, 0'
- 现象: 链接动态库时 `Auto build dll exports` 步骤失败, 提示 `cmake -E __create_def` 无法识别 obj 文件
- 原因: 该目标同时开启了 `WINDOWS_EXPORT_ALL_SYMBOLS` (自动导出全部符号) 与 `/GL` (LTO);
  `/GL` 产物是编译器中间表示, obj 内没有符号表, CMake 无法据此生成 `.def`
- 处理: 两者不可同时使用, 任选其一:
    - 关闭 LTO: cmake 参数加 `-DAGENTXX_ENABLE_LTO=OFF`
    - 该库改用显式导出 (推荐: `__declspec(dllexport)` 标注 + 默认不导出;
      本项目三个基础库即如此, 见头文件中的 `UTILXX_BASE_API` / `UTILXX_API` / `PLUGINXX_API`)
    - 仅对该库的嵌套构建剥离 LTO 参数 (顶层 `agent/CMakeLists.txt` 的 `agentxx_strip_lto_args`,
      本项目仅用于开启 fat runtime 时的 hyperscan)

### 链接错误 uchardet.lib(uchardet.obj) : error LNK2038
- windows上 msvc 编译出来的库分为 debug 和 release 版本，且区分 静态链接c++标准库 和 动态链接c++标准库，因此一共分为 4种 情况
- 这个报错是由于在构建 debug 库中链接了 release 版本的库，或者在构建 release 库中链接了 debug 版本的库
- 对于 uchardet 需要修改其 CMakeLists.txt，删除或注释掉这段内容，然后删除编译缓存重新编译即可:
```cmake
if (CMAKE_BUILD_TYPE MATCHES Debug)
    set(version_suffix .debug)
    add_compile_options("-fsanitize=address")
    add_link_options("-fsanitize=address")
endif (CMAKE_BUILD_TYPE MATCHES Debug)
```
- 有些库编译需要定义 `CMAKE_MSVC_RUNTIME_LIBRARY`，并开启 `CMAKE_POLICY_DEFAULT_CMP0091=NEW` 即可，这需要`cmake 3.16+`。这里举例声明固定为动态链接标准库；如果希望静态链接标准库，可以把 `MultiThreaded$<$<CONFIG:Debug>:Debug>DLL` 改为 `MultiThreaded$<$<CONFIG:Debug>:Debug>` 即可.
```cmake
# 如果可以修改项目的 CMakeLists.txt，则增加: 
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# 如果是导入依赖库，可以添加`CMAKE_ARGS`变量:
ExternalProject_Add(
  glob_repo
  SOURCE_DIR "path/to/glob/"
  CMAKE_ARGS
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    "-DCMAKE_POLICY_DEFAULT_CMP0091=NEW"
)
```
### 链接错误 error C1128
- 编译参数添加 `/bigobj` 即可.
```cmake
if (MSVC)
	target_compile_options(your_target PRIVATE 
		"/bigobj"
	)
endif ()
```