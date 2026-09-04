> 文档自动翻译自[zh-cn](/docs/zh-cn/build/windows.md)版 (This document is automatically translated from the [zh-cn](/docs/zh-cn/build/windows.md) version.)

# Windows Executable / Dynamic Library Build Guide

- OS Environment: Windows
- C++ Standard: Requires C++26+.
- Compiler: MSVC
- Related: [Cross-compiling Windows Executables and Dynamic Libraries on Linux](cross-linux-for-windows.md)

## Getting Started
### Installing Visual Studio 2026
- Recommended: VS 2026. You can also try older versions like VS 2022 (compatibility unverified).
### cmake, pkg-config
- Install CMake:
    - https://cmake.org/download/
    - Choose the Windows installer, e.g.: `cmake-x.x.x-windows-x86_64.msi`
    - https://github.com/Kitware/CMake/releases/download/v4.4.0-rc2/cmake-4.4.0-rc2-windows-x86_64.msi
- Install pkg-config:
```pwsh
# Open PowerShell as Administrator
Set-ExecutionPolicy Bypass -Scope Process -Force; [System.Net.ServicePointManager]::SecurityProtocol = [System.Net.ServicePointManager]::SecurityProtocol -bor 3072; iex ((New-Object System.Net.WebClient).DownloadString('https://community.chocolatey.org/install.ps1'))

choco install pkgconfiglite

pkg-config --version
```
### Compiling Boost
- Install or compile Boost 1.92. Matching our development version `1.92` is recommended.
- To compile manually using Windows CMD:
```sh
# https://github.com/boostorg/boost/releases/
# Download release/boost-xxx-cmake.7z and extract to agent/third_party/boost/
cd boost\
.\bootstrap.bat

# Switch to cmd
cmd

# Create third_party/boost-windows-build-debug and third_party/boost-windows-build-release directories, then return to boost directory
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

# Compile and install release / debug
.\b2.exe install --layout=system --prefix="%boost_install_release_dir%" link=static runtime-link=shared runtime-debugging=off address-model=64 variant=release

.\b2.exe install --layout=system --prefix="%boost_install_debug_dir%" link=static runtime-link=shared runtime-debugging=on   address-model=64 variant=debug

# To rebuild from scratch, clean first:
# .\b2.exe --clean-all
```
### Downloading Precompiled OpenSSL
- Go to `https://slproweb.com/products/Win32OpenSSL.html`. Scroll down and find `Win64 OpenSSL vx.x.x` (make sure it does NOT say "Light"). Download either the `EXE` or `MSI` installer and run it. Set the installation directory to `{PROJECT_ROOT}/agent/third_party/OpenSSL-windows-build/`.
### Installing Ragel
- Required for compiling high-performance regex library support (`vectorize` / `hyperscan`). If you do not want to install it, you can modify [debug_build.bat](/agent/script/debug_build.bat) to set `-DAGENTXX_ENABLE_HYPERSCAN=OFF`.
- Open CMD and check if Ragel is installed:
```sh
ragel -v
```
- If an error like `'ragel' is not recognized as an internal or external command` occurs, it is not installed. You can install it quickly using winget:
```sh
winget install --id=PolarGoose.Ragel -e

# View installation path and copy Ragel.exe location
dir %localappdata%\Microsoft\WinGet\Links\
# Equivalent to:
%localappdata%\Microsoft\WinGet\Links\Ragel.exe -v
```
### Compiling agentxx
- Launch agentxx compilation. Other dependencies will be downloaded automatically. After successful compilation, the command-line client will run automatically:
```sh
cd {PROJECT_ROOT}
./agent/script/windows_debug_build.sh
./agent/build/windows-debug/exec/agentxx_cli
```
- For release compilation:
```sh
cd {PROJECT_ROOT}
./agent/script/windows_release_build.sh
./agent/build/windows-release/exec/agentxx_cli
```

## Common Issues
- [FAQ for more issues](FAQ.md)

### Link Error: uchardet.lib(uchardet.obj) : error LNK2038
- MSVC-compiled libraries on Windows are split into Debug and Release versions, as well as static vs dynamic C++ runtime linking (4 configurations in total).
- This error occurs when a Release library is linked into a Debug build, or a Debug library is linked into a Release build.
- For uchardet, edit its `CMakeLists.txt` to remove or comment out the following block, then wipe the build cache and recompile:
```cmake
if (CMAKE_BUILD_TYPE MATCHES Debug)
    set(version_suffix .debug)
    add_compile_options("-fsanitize=address")
    add_link_options("-fsanitize=address")
endif (CMAKE_BUILD_TYPE MATCHES Debug)
```
- Certain libraries require defining `CMAKE_MSVC_RUNTIME_LIBRARY` and enabling `CMAKE_POLICY_DEFAULT_CMP0091=NEW` (requires CMake 3.16+). Example specifying dynamic CRT linking (if static CRT linking is desired, change `MultiThreaded$<$<CONFIG:Debug>:Debug>DLL` to `MultiThreaded$<$<CONFIG:Debug>:Debug>`):
```cmake
# If you can edit the project's CMakeLists.txt, add:
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# If importing via dependency management, add to `CMAKE_ARGS`:
ExternalProject_Add(
  glob_repo
  SOURCE_DIR "path/to/glob/"
  CMAKE_ARGS
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    "-DCMAKE_POLICY_DEFAULT_CMP0091=NEW"
)
```
### Link Error: error C1128
- Simply add `/bigobj` to the compiler flags:
```cmake
if (MSVC)
	target_compile_options(your_target PRIVATE 
		"/bigobj"
	)
endif ()
```
