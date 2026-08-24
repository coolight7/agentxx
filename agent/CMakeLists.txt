# The Flutter tooling requires that developers have CMake 3.10 or later
# installed. You should not increase this version, as doing so will cause
# the plugin to fail to compile for some customers of the plugin.
cmake_minimum_required(VERSION 3.10)
# find_package 使用 <PackageName>_ROOT 变量
cmake_policy(SET CMP0074 NEW)
# MSVC 运行时库标志由抽象层选择
cmake_policy(SET CMP0091 NEW)
# find_package 使用大写的 _ROOT 变量
cmake_policy(SET CMP0144 NEW)
cmake_policy(SET CMP0167 OLD)

project(agentxx_project 
  VERSION 0.1.0 
  LANGUAGES C CXX
)

# ccache
find_program(CCACHE_PROGRAM ccache)
if (CCACHE_PROGRAM)
  set(CMAKE_C_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
  set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
endif ()
set(CMAKE_FIND_DEBUG_MODE OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# option ==========
option(XX_IS_RELEASE_D "Release Build." ON)
option(AGENTXX_BUILD_CLIENT "Generate the client target." OFF)
option(AGENTXX_BUILD_TEST "Generate the test target." OFF)
option(AGENTXX_BUILD_BENCHMARK "Generate the benchmark target." OFF)
option(AGENTXX_ENABLE_HYPERSCAN "Add HyperScan for regex support. Use std::regex if disable." OFF)
option(AGENTXX_ENABLE_BOOST_PROCESS "Use Boost.process for execute command toolcall." ON)
option(AGENTXX_BUILD_PLUGINS "Generate the plugins target." ON)
option(AGENTXX_ENABLE_PLUGIN_CODEGRAPH "Add CodeGraph-CPP for code analysis." ON)
option(AGENTXX_ENABLE_PLUGIN_JS "Build QuickJS JS plugin engine (agentxx_javascript_engine)." ON)
option(AGENTXX_ENABLE_PLUGIN_SYSTEM_MONITOR "Build agentxx_system_monitor plugin (CPU/GPU usage)." ON)
option(AGENTXX_ENABLE_PLUGIN_AUDIO_STREAM "Build agentxx_audio_stream plugin (audio capture)." ON)
option(AGENTXX_ENABLE_PLUGIN_TEXT_SELECTION "Build agentxx_text_selection_monitor plugin (text selection)." ON)
option(AGENTXX_ENABLE_PLUGIN_EXAMPLE "Build example plugin." ON)
option(AGENTXX_ENABLE_PLUGIN_COMPUTER_USE "Build agentxx_computer_use plugin (ui_control)." ON)
option(AGENTXX_ENABLE_PLUGIN_SCREEN_CAPTURE "Build agentxx_screen_capture plugin (screen capture)." ON)
# 可选内置合并编译: 启用的插件源文件直接编译进 libagentxx, 运行期无需插件
# 动态库 (无 dlopen; 适合嵌入式/单文件分发)。开启后跳过独立插件动态库构建
# (agentxx_plugins_repo), 插件配置沿用插件目录路径 (plugin.yaml 资源仍拷贝,
# 入口文件缺失时 PluginManager 自动回退内置注册表, 见 plugins.md)
option(AGENTXX_ENABLE_PLUGIN_BUILTIN "Merge enabled plugins into libagentxx (built-in, no runtime plugin dlls)." OFF)
# ===== Debug 构建加速选项 =====
# - 关闭后 Debug 构建不再启用 AddressSanitizer，编译与链接显著加快
#   (仍保留 -fno-omit-frame-pointer 便于调试)，默认开启保持原有行为
option(AGENTXX_ENABLE_ASAN "Enable AddressSanitizer for Debug builds." ON)
# - 关闭后 Debug 构建不带调试符号 (-g)，进一步加快编译/链接
option(AGENTXX_ENABLE_DEBUG_INFO "Enable debug info (-g) for Debug builds." ON)
# - 预编译稳定的第三方头文件 (fmt/asio/boost.exception 等) 加速 lib/client/test 编译;
#   项目自身头文件不参与 PCH，改动时不会触发全量重编。仅 GCC/Clang 生效
option(AGENTXX_ENABLE_PCH "Enable precompiled headers (GCC/Clang)." ON)
# - 链接器选择 (GCC/Clang): auto 自动检测 mold -> gold -> 默认 bfd;
#   也可直接指定 mold/gold/bfd 等。mold 可大幅减少大体积二进制链接时间
set(AGENTXX_LINKER "auto" CACHE STRING "Linker for GCC/Clang: auto|mold|gold|bfd|...")

# 仅用于指示是否编译用于调试用途，不代表真实编译类型
# - [XX_IS_DEBUG_D] == 1 时，启用大部分调试日志等信息，因此 profile 之类注重
# 性能的编译将视为 release
if (XX_IS_RELEASE_D)
  set(XX_IS_DEBUG_D    0)
  set(XX_IS_RELEASE_D  1)
else()
  set(XX_IS_DEBUG_D    1)
  set(XX_IS_RELEASE_D  0)
endif()

# target system
set(XX_IS_LINUX_D    0)
set(XX_IS_WIN_D      0)
set(XX_IS_MACOS_D    0)
set(XX_IS_ANDROID_D  0)
set(XX_IS_IOS_D      0)

if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
  set(XX_IS_WIN_D 1)

  set(AGENTXX_WINDOWS_IOCP_SUPPORTED ON)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Android")
  set(XX_IS_ANDROID_D 1)
elseif (APPLE AND CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(XX_IS_MACOS_D 1)
elseif (APPLE AND CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(XX_IS_IOS_D 1)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(XX_IS_LINUX_D 1)

  if (EXISTS "/usr/include/linux/version.h")
    # 检查是否支持 [AGENTXX_LINUX_IO_URING_SUPPORTED]
    file(READ "/usr/include/linux/version.h" _linux_version_h)
    string(REGEX MATCH "#define LINUX_VERSION_CODE ([0-9]+)" _match "${_linux_version_h}")
    if(_match AND (CMAKE_MATCH_1 GREATER_EQUAL 330240))  # >= 5.10.0
        set(AGENTXX_LINUX_IO_URING_SUPPORTED ON)
    else()
        set(AGENTXX_LINUX_IO_URING_SUPPORTED OFF)
    endif()
  endif()
endif ()

# compiler ID
set(XX_IS_MSVC_D 0)
set(XX_IS_GCC_D 0)
set(XX_IS_CLANG_D 0)

if (MSVC)
  set(XX_IS_MSVC_D 1)
elseif (CMAKE_C_COMPILER_ID MATCHES "GNU")
  set(XX_IS_GCC_D 1)
elseif (CMAKE_C_COMPILER_ID MATCHES "Clang")
  set(XX_IS_CLANG_D 1)
else ()
  message(FATAL_ERROR "[agentxx] 不支持的编译器: ${CMAKE_C_COMPILER_ID}")
endif ()

message("[agentxx] Test Utf8 Log: 你好！")
message("[agentxx] Current Configure Type: ${CMAKE_CONFIGURATION_TYPES}")
message("[agentxx] Current Build Type: ${CMAKE_BUILD_TYPE}")
message("[agentxx] Current Compiler: ${CMAKE_C_COMPILER_ID}")

# windows/MSVC(Visual Studio 生成器) 嵌套构建输出中文乱码修复:
# - 构建脚本 (script/windows_*_build.bat) 会 `chcp 65001`, 子进程 (cl.exe、嵌套 MSBuild)
#   写入管道的诊断输出均为 UTF-8 字节;
# - 但外层 MSBuild 的 CustomBuild/ParallelCustomBuild 任务 (Microsoft.Build.CppTasks.Common.dll)
#   不支持 CMake 生成的 <UseUtf8Encoding>Always</UseUtf8Encoding> 元数据,
#   未显式指定 StdOutEncoding/StdErrEncoding 时默认按 OEM 代码页 (中文系统=936/GBK) 解码,
#   于是 ExternalProject 嵌套构建中编译器/内层 MSBuild 的中文输出被误解为乱码
#   (如 "内层" 显示为 "鍐呭眰": UTF-8 字节被按 GBK 解读);
# - MSBuild 会从项目文件所在目录逐级向上查找并自动导入 Directory.Build.targets
#   (本项目所有 vcxproj 均位于 CMAKE_BINARY_DIR 子树内, 故全部生效),
#   通过 BeforeTargets=CustomBuild 在构建步骤执行前给 CustomBuild 项注入
#   StdOutEncoding/StdErrEncoding=UTF-8, 强制按 UTF-8 解码子进程输出;
# - 仅当构建环境设置了 AgentxxBuildConsoleCP=65001 (见构建脚本) 时启用,
#   避免非 UTF-8 控制台 (未 chcp 65001) 场景误转码。
if (XX_IS_WIN_D AND XX_IS_MSVC_D AND CMAKE_GENERATOR MATCHES "Visual Studio")
  file(WRITE "${CMAKE_BINARY_DIR}/Directory.Build.targets"
"<Project>\n"
"  <!-- agentxx: 强制 MSBuild CustomBuild 任务按 UTF-8 解码子进程输出 (配合构建脚本 chcp 65001), 修复嵌套构建中文乱码 -->\n"
"  <Target Name=\"Agentxx_ForceUtf8CustomBuildOutput\" BeforeTargets=\"CustomBuild\" Condition=\"'$(AgentxxBuildConsoleCP)' == '65001'\">\n"
"    <ItemGroup>\n"
"      <CustomBuild>\n"
"        <StdOutEncoding>UTF-8</StdOutEncoding>\n"
"        <StdErrEncoding>UTF-8</StdErrEncoding>\n"
"      </CustomBuild>\n"
"    </ItemGroup>\n"
"  </Target>\n"
"</Project>\n"
  )
endif ()

set(_AGENTXX_COMMON_C_CXX_FLAGS "")
set(_AGENTXX_C_FLAGS "")
set(_AGENTXX_CXX_FLAGS "")

# 开发默认标准，建议 c17 + c++26
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)
if (XX_IS_MSVC_D)
  set(_AGENTXX_C_FLAGS "/std:c17")
  set(_AGENTXX_CXX_FLAGS "/std:c++latest")
else ()
  set(_AGENTXX_C_FLAGS "-std=c17")
  set(_AGENTXX_CXX_FLAGS "-std=c++26")
endif()
# msvc 时统一动态链接标准库 /MD /MDd
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
# 优先链接静态库，不影响链接标准库
if(XX_IS_MSVC_D)
  list(PREPEND CMAKE_FIND_LIBRARY_SUFFIXES .lib .a)
else()
  list(PREPEND CMAKE_FIND_LIBRARY_SUFFIXES .a)
endif()

include(CheckCXXCompilerFlag)
include(CheckCXXSourceRuns)

# /D_WIN32_WINNT=0x0A00 限制 win10 或更高:
# - httplib 要求 >= win10
if (XX_IS_MSVC_D) 
  set(_AGENTXX_COMMON_C_CXX_FLAGS "${_AGENTXX_COMMON_C_CXX_FLAGS} /D_WIN32_WINNT=0x0A00 /D__STDC_CONSTANT_MACROS=1 /DCPPHTTPLIB_OPENSSL_SUPPORT=1 /DBOOST_ASIO_STANDALONE=1 /DASIO_STANDALONE=1 /DBOOST_PROCESS_USE_STD_FS=1 /DBOOST_EXCEPTION_MINI_BOOST=1 /DYAML_CPP_STATIC_DEFINE=1")
  if (AGENTXX_WINDOWS_IOCP_SUPPORTED)
    set(_AGENTXX_COMMON_C_CXX_FLAGS "${_AGENTXX_COMMON_C_CXX_FLAGS} /DASIO_HAS_IOCP=1 /DBOOST_ASIO_HAS_IOCP=1 /DASIO_HAS_WINDOWS_RANDOM_ACCESS_HANDLE=1 /DBOOST_ASIO_HAS_WINDOWS_RANDOM_ACCESS_HANDLE=1")
    message("[agentxx] Enable asio/iocp")
  endif()
  if (AGENTXX_LINUX_IO_URING_SUPPORTED)
    set(_AGENTXX_COMMON_C_CXX_FLAGS "${_AGENTXX_COMMON_C_CXX_FLAGS} /DASIO_HAS_IO_URING=1 /DBOOST_ASIO_HAS_IO_URING=1")
    # set(_AGENTXX_COMMON_C_CXX_FLAGS "${_AGENTXX_COMMON_C_CXX_FLAGS} /DASIO_HAS_IO_URING_AS_DEFAULT=1 /DBOOST_ASIO_HAS_IO_URING_AS_DEFAULT=1")
    message("[agentxx] Enable asio/io_uring")
  endif()
else ()
  set(_AGENTXX_COMMON_C_CXX_FLAGS "${_AGENTXX_COMMON_C_CXX_FLAGS} -D__STDC_CONSTANT_MACROS=1 -DCPPHTTPLIB_OPENSSL_SUPPORT=1 -DBOOST_ASIO_STANDALONE=1 -DASIO_STANDALONE=1 -DBOOST_PROCESS_USE_STD_FS=1 -DYAML_CPP_STATIC_DEFINE=1")
  if (XX_IS_WIN_D)
    # 可能交叉编译
    set(_AGENTXX_COMMON_C_CXX_FLAGS "${_AGENTXX_COMMON_C_CXX_FLAGS} -D_WIN32_WINNT=0x0A00")
  endif()
  if (AGENTXX_WINDOWS_IOCP_SUPPORTED)
    set(_AGENTXX_COMMON_C_CXX_FLAGS "${_AGENTXX_COMMON_C_CXX_FLAGS} -DASIO_HAS_IOCP=1 -DBOOST_ASIO_HAS_IOCP=1 -DASIO_HAS_WINDOWS_RANDOM_ACCESS_HANDLE=1 -DBOOST_ASIO_HAS_WINDOWS_RANDOM_ACCESS_HANDLE=1")
    message("[agentxx] Enable asio/iocp")
  endif()
  if (AGENTXX_LINUX_IO_URING_SUPPORTED)
    set(_AGENTXX_COMMON_C_CXX_FLAGS "${_AGENTXX_COMMON_C_CXX_FLAGS} -DASIO_HAS_IO_URING=1 -DBOOST_ASIO_HAS_IO_URING=1")
    # set(_AGENTXX_COMMON_C_CXX_FLAGS "${_AGENTXX_COMMON_C_CXX_FLAGS} -DASIO_HAS_IO_URING_AS_DEFAULT=1 -DBOOST_ASIO_HAS_IO_URING_AS_DEFAULT=1")
    message("[agentxx] Enable asio/io_uring")
  endif()
endif()

message("[agentxx] CMAKE_CXX_COMPILER_ID: ${CMAKE_CXX_COMPILER_ID}")
if (XX_IS_MSVC_D)
  # 注意: 不要添加 /FS /MP —— VS18/MSVC 14.51 的 FileTracker 在命令行出现
  # 重复 /FS 时会失效 (子编译进程不写跟踪记录)，导致每次构建全量重编;
  # 且旗标经多层 CMake 传递极易重复叠加，故统一不使用这两个选项
  set(_AGENTXX_C_FLAGS   "${_AGENTXX_COMMON_C_CXX_FLAGS} ${_AGENTXX_C_FLAGS} /bigobj /utf-8 /wd4200 /D_GNU_SOURCE")
  set(_AGENTXX_CXX_FLAGS "${_AGENTXX_COMMON_C_CXX_FLAGS} ${_AGENTXX_CXX_FLAGS} /bigobj /utf-8 /wd4200 /Zc:__cplusplus")

  set(CMAKE_C_FLAGS_RELEASE          "${CMAKE_C_FLAGS_RELEASE}          ${_AGENTXX_C_FLAGS}   /Gy")
  set(CMAKE_CXX_FLAGS_RELEASE        "${CMAKE_CXX_FLAGS_RELEASE}        ${_AGENTXX_CXX_FLAGS} /Gy")
  set(CMAKE_C_FLAGS_MINSIZEREL       "${CMAKE_C_FLAGS_MINSIZEREL}       ${_AGENTXX_C_FLAGS}")
  set(CMAKE_CXX_FLAGS_MINSIZEREL     "${CMAKE_CXX_FLAGS_MINSIZEREL}     ${_AGENTXX_CXX_FLAGS}")
  set(CMAKE_C_FLAGS_RELWITHDEBINFO   "${CMAKE_C_FLAGS_RELWITHDEBINFO}   ${_AGENTXX_C_FLAGS}")
  set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} ${_AGENTXX_CXX_FLAGS}")
  set(CMAKE_C_FLAGS_DEBUG            "${CMAKE_C_FLAGS_DEBUG}            ${_AGENTXX_C_FLAGS}")
  set(CMAKE_CXX_FLAGS_DEBUG          "${CMAKE_CXX_FLAGS_DEBUG}          ${_AGENTXX_CXX_FLAGS}")

  if (XX_IS_RELEASE_D)
    set(_AGENTXX_LDFLAGS "${LDFLAGS} ${_AGENTXX_LDFLAGS} /OPT:REF /OPT:ICF")
  else ()
    # windows/msvc 使用 /fsanitize=address 时可能导致链接错误 (uchatdet)
    set(_AGENTXX_LDFLAGS "${LDFLAGS} ${_AGENTXX_LDFLAGS}")
  endif()

  # set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
else ()
  # XX_IS_CLANG_D OR XX_IS_GCC_D
  set(_AGENTXX_C_FLAGS   "${_AGENTXX_COMMON_C_CXX_FLAGS} ${_AGENTXX_C_FLAGS}   -fvisibility=default -ffunction-sections -fdata-sections -Wall -fPIC -pipe -D_GNU_SOURCE")
  # -include cstddef: boost::asio 的 config.hpp 推导 BOOST_ASIO_HAS_STD_ALIGNED_ALLOC
  # 时依赖 libstdc++ 的 _GLIBCXX_HAVE_ALIGNED_ALLOC 宏, 但 config.hpp 自身不 include
  # 系统头, 该宏是否已定义取决于使用方的 include 顺序, 导致跨 TU 宏状态不一致
  # (aligned_new 走 operator new 而 aligned_delete 走 std::free → ASAN alloc-dealloc
  # mismatch / ODR 违反, 链接顺序变化即暴露崩溃)。强制预包含 <cstddef> 使该宏
  # 在所有 TU 中于 config.hpp 处理前已定义, 统一推导结果。
  set(_AGENTXX_CXX_FLAGS "${_AGENTXX_COMMON_C_CXX_FLAGS} ${_AGENTXX_CXX_FLAGS} -fvisibility=default -ffunction-sections -fdata-sections -Wall -fPIC -pipe -include cstddef")
  
  set(CMAKE_C_FLAGS_RELEASE          "${_AGENTXX_C_FLAGS}   -O3 -flto ${CMAKE_C_FLAGS_RELEASE}")
  set(CMAKE_CXX_FLAGS_RELEASE        "${_AGENTXX_CXX_FLAGS} -O3 -flto ${CMAKE_CXX_FLAGS_RELEASE}")
  set(CMAKE_C_FLAGS_MINSIZEREL       "${_AGENTXX_C_FLAGS}   ${CMAKE_C_FLAGS_MINSIZEREL}")
  set(CMAKE_CXX_FLAGS_MINSIZEREL     "${_AGENTXX_CXX_FLAGS} ${CMAKE_CXX_FLAGS_MINSIZEREL}")
  set(CMAKE_C_FLAGS_RELWITHDEBINFO   "${_AGENTXX_C_FLAGS}   ${CMAKE_C_FLAGS_RELWITHDEBINFO}")
  set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${_AGENTXX_CXX_FLAGS} ${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
  # Debug 编译加速: -g / ASAN 均可通过选项关闭;
  # - 启用 ASAN 时: -fsanitize=address -fno-omit-frame-pointer -fno-optimize-sibling-calls
  # - 关闭 ASAN 时: 保留 -fno-omit-frame-pointer 便于调试/性能分析
  # - 关闭 DEBUG_INFO 时: 需同时剔除 CMake 为 GCC/Clang 默认追加的 -g
  #   (CMAKE_CXX_FLAGS_DEBUG 默认值即 "-g")，否则产物依旧膨胀
  set(_AGENTXX_DEBUG_EXTRA_FLAGS "")
  if (AGENTXX_ENABLE_DEBUG_INFO)
    string(APPEND _AGENTXX_DEBUG_EXTRA_FLAGS " -g")
  else()
    string(REGEX REPLACE "(^| )-g([0-9])?( |$)" " " CMAKE_C_FLAGS_DEBUG   "${CMAKE_C_FLAGS_DEBUG}")
    string(REGEX REPLACE "(^| )-g([0-9])?( |$)" " " CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
  endif()
  if (AGENTXX_ENABLE_ASAN)
    string(APPEND _AGENTXX_DEBUG_EXTRA_FLAGS " -fsanitize=address -fno-omit-frame-pointer -fno-optimize-sibling-calls")
  else()
    string(APPEND _AGENTXX_DEBUG_EXTRA_FLAGS " -fno-omit-frame-pointer")
  endif()
  set(CMAKE_C_FLAGS_DEBUG            "${_AGENTXX_C_FLAGS}   ${_AGENTXX_DEBUG_EXTRA_FLAGS} ${CMAKE_C_FLAGS_DEBUG}")
  set(CMAKE_CXX_FLAGS_DEBUG          "${_AGENTXX_CXX_FLAGS} ${_AGENTXX_DEBUG_EXTRA_FLAGS} ${CMAKE_CXX_FLAGS_DEBUG}")
  unset(_AGENTXX_DEBUG_EXTRA_FLAGS)

  if (XX_IS_RELEASE_D)
    set(_AGENTXX_LDFLAGS "${LDFLAGS} ${_AGENTXX_LDFLAGS} -Wl,--gc-sections -flto")
  else ()
    if (AGENTXX_ENABLE_ASAN)
      set(_AGENTXX_LDFLAGS "${LDFLAGS} ${_AGENTXX_LDFLAGS} -fsanitize=address")
    else ()
      set(_AGENTXX_LDFLAGS "${LDFLAGS} ${_AGENTXX_LDFLAGS}")
    endif()
  endif ()
endif ()
string(APPEND CMAKE_EXE_LINKER_FLAGS    " ${_AGENTXX_LDFLAGS}")
string(APPEND CMAKE_SHARED_LINKER_FLAGS " ${_AGENTXX_LDFLAGS}")

# 快速链接器 (GCC/Clang): mold > gold > 默认 bfd，大幅减少大体积二进制链接时间
# - auto: 自动检测 mold / ld.gold，均未安装时使用默认链接器
# - 也可通过 -DAGENTXX_LINKER=mold|gold|bfd 显式指定
# - 不适用于 MSVC (无对应 -fuse-ld 语义)
if (NOT XX_IS_MSVC_D)
  set(_AGENTXX_LINKER_FLAG "")
  if (AGENTXX_LINKER STREQUAL "auto")
    # NOTE: android 交叉编译时不可用宿主机的 mold/gold: NDK clang 的 LTO 链接
    # 需要 NDK 内的 LLVMgold.so 插件, 宿主链接器会因找不到插件而失败
    # (mold: fatal: could not open plugin file .../lib/LLVMgold.so)
    if (ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "Android")
      message("[agentxx] Android cross compile, use default linker")
    else ()
      find_program(_AGENTXX_MOLD_EXECUTABLE mold)
      find_program(_AGENTXX_GOLD_EXECUTABLE ld.gold)
      if (_AGENTXX_MOLD_EXECUTABLE)
        message("[agentxx] Use fast linker: mold")
        set(_AGENTXX_LINKER_FLAG "-fuse-ld=mold")
      elseif (_AGENTXX_GOLD_EXECUTABLE)
        message("[agentxx] Use fast linker: gold")
        set(_AGENTXX_LINKER_FLAG "-fuse-ld=gold")
      else ()
        message("[agentxx] Use default linker (bfd)")
      endif()
      unset(_AGENTXX_MOLD_EXECUTABLE CACHE)
      unset(_AGENTXX_GOLD_EXECUTABLE CACHE)
    endif()
  elseif (AGENTXX_LINKER STREQUAL "bfd")
    message("[agentxx] Use default linker (bfd)")
  else ()
    set(_AGENTXX_LINKER_FLAG "-fuse-ld=${AGENTXX_LINKER}")
  endif()
  string(APPEND CMAKE_EXE_LINKER_FLAGS    " ${_AGENTXX_LINKER_FLAG}")
  string(APPEND CMAKE_SHARED_LINKER_FLAGS " ${_AGENTXX_LINKER_FLAG}")
  unset(_AGENTXX_LINKER_FLAG)
endif()

unset(_AGENTXX_COMMON_C_CXX_FLAGS)
unset(_AGENTXX_C_FLAGS)
unset(_AGENTXX_CXX_FLAGS)

# exe install dir
set(AGENTXX_EXEC_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/exec")
message("[agentxx] AGENTXX_EXEC_INSTALL_PREFIX: ${AGENTXX_EXEC_INSTALL_PREFIX}")
message("[agentxx] CMAKE_CURRENT_SOURCE_DIR: ${CMAKE_CURRENT_SOURCE_DIR}")

# deps
set(AGENTXX_THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/")
set(AGENTXX_INSTALL_DIR "${CMAKE_BINARY_DIR}/agentxx-project-install/")

# - 添加搜索根目录到 [CMAKE_FIND_ROOT_PATH]，以兼容交叉编译
# - 搜索模式为 [ONLY] 隔离搜索，避免交叉编译时引用主机上的库导致各种异常
if (CMAKE_CROSSCOMPILING)
  set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY "ONLY")
  set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE "ONLY")
  set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE "ONLY")
endif()

# boost 系统安装库 或 自行编译
# https://github.com/boostorg/boost
# 下载 release/boost-xxx.7z 解压到 agent/third_party/boost/
# 自行按文档编译参考: /docs/build/linux.md 或 /docs/build/windows.md
set (Boost_USE_STATIC_LIBS ON)
set (Boost_USE_STATIC_RUNTIME OFF) # 是否静态链接标准库
set (Boost_USE_DEBUG_LIBS ${XX_IS_DEBUG_D})
set (Boost_USE_RELEASE_LIBS ${XX_IS_RELEASE_D})
set (Boost_USE_DEBUG_RUNTIME ${XX_IS_DEBUG_D})
set (Boost_USE_RELEASE_RUNTIME ${XX_IS_RELEASE_D})
if (NOT DEFINED BOOST_ROOT)
  message(FATAL_ERROR "[agentxx] Need to set BOOST_ROOT to the path of your Boost installation")
elseif (NOT EXISTS "${BOOST_ROOT}")
  message(FATAL_ERROR "[agentxx] BOOST_ROOT not exists: ${BOOST_ROOT}")
else ()
  file(TO_CMAKE_PATH "${BOOST_ROOT}" TEMP_BOOST_ROOT)
  set(BOOST_ROOT "${TEMP_BOOST_ROOT}" CACHE PATH "Boost root path" FORCE)
  unset(TEMP_BOOST_ROOT)
endif()

# openssl
set (OPENSSL_USE_STATIC_LIBS ON)
if (NOT DEFINED OPENSSL_ROOT_DIR)
  message(FATAL_ERROR "[agentxx] Need to set OPENSSL_ROOT_DIR to the path of your OpenSSL installation")
elseif (NOT EXISTS "${OPENSSL_ROOT_DIR}")
  message(FATAL_ERROR "[agentxx] OPENSSL_ROOT_DIR not exists: ${OPENSSL_ROOT_DIR}")
else ()
  file(TO_CMAKE_PATH "${OPENSSL_ROOT_DIR}" TEMP_OPENSSL_ROOT_DIR)
  set(OPENSSL_ROOT_DIR "${TEMP_OPENSSL_ROOT_DIR}" CACHE PATH "OpenSSL root path" FORCE)
  unset(TEMP_OPENSSL_ROOT_DIR)
endif()

if (CMAKE_CROSSCOMPILING)
  # 统一追加 [CMAKE_FIND_ROOT_PATH]，逐步添加可能导致自动补充搜索路径时 find_package 验证查找失败
  list (PREPEND CMAKE_FIND_ROOT_PATH "${AGENTXX_INSTALL_DIR}")
  list (APPEND CMAKE_FIND_ROOT_PATH 
    "${BOOST_ROOT}" 
    "${OPENSSL_ROOT_DIR}"
  )
endif ()
list (PREPEND CMAKE_PREFIX_PATH "${AGENTXX_INSTALL_DIR}")

set(_AGENTXX_COMMON_CMAKE_CACHE_ARGS
    "-DCMAKE_FIND_ROOT_PATH:STRING=${CMAKE_FIND_ROOT_PATH}"
    "-DCMAKE_PREFIX_PATH:STRING=${CMAKE_PREFIX_PATH}"
    "-DBOOST_ROOT:PATH=${BOOST_ROOT}"
    "-DBoost_ROOT:PATH=${BOOST_ROOT}"
    "-DOPENSSL_ROOT_DIR:PATH=${OPENSSL_ROOT_DIR}"
    "-DLDFLAGS:STRING=${_AGENTXX_LDFLAGS}"
)
set(_AGENTXX_COMMON_CMAKE_ARGS
    # "-DCMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}" # 传递后会导致 sqlite 编译检查出错
    "-DCMAKE_CROSSCOMPILING=${CMAKE_CROSSCOMPILING}"
    "-DCMAKE_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR}"
    "-DCMAKE_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
    "-DCMAKE_TOOLCHAIN_FILE:PATH=${CMAKE_TOOLCHAIN_FILE}"
    -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=${CMAKE_FIND_ROOT_PATH_MODE_LIBRARY}
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=${CMAKE_FIND_ROOT_PATH_MODE_INCLUDE}
    -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=${CMAKE_FIND_ROOT_PATH_MODE_PACKAGE}

    "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
    "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
    # ccache 等编译启动器需显式传给嵌套 (ExternalProject) 构建，否则子构建不会使用
    "-DCMAKE_C_COMPILER_LAUNCHER=${CMAKE_C_COMPILER_LAUNCHER}"
    "-DCMAKE_CXX_COMPILER_LAUNCHER=${CMAKE_CXX_COMPILER_LAUNCHER}"
    # 嵌套构建引用 CCACHE_PROGRAM 判断是否启用 ccache 兼容选项 (如 -fpch-preprocess)
    "-DCCACHE_PROGRAM=${CCACHE_PROGRAM}"
    # 链接器 (mold/gold/-fsanitize 等) 同样需要显式传递给嵌套构建
    "-DCMAKE_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}"
    "-DCMAKE_SHARED_LINKER_FLAGS=${CMAKE_SHARED_LINKER_FLAGS}"
    "-DCMAKE_INSTALL_PREFIX=${AGENTXX_INSTALL_DIR}"
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DCMAKE_EXPORT_COMPILE_COMMANDS=${CMAKE_EXPORT_COMPILE_COMMANDS}

    "-DCMAKE_MSVC_RUNTIME_LIBRARY=${CMAKE_MSVC_RUNTIME_LIBRARY}"
    -DCMAKE_POLICY_DEFAULT_CMP0074=NEW
    -DCMAKE_POLICY_DEFAULT_CMP0091=NEW
    -DCMAKE_POLICY_DEFAULT_CMP0144=NEW
    -DCMAKE_POLICY_DEFAULT_CMP0167=OLD

    "-DCMAKE_CONFIGURATION_TYPES=${CMAKE_CONFIGURATION_TYPES}"
    "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
    "-DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}"
    "-DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}"
    "-DCMAKE_C_FLAGS_RELEASE=${CMAKE_C_FLAGS_RELEASE}"
    "-DCMAKE_CXX_FLAGS_RELEASE=${CMAKE_CXX_FLAGS_RELEASE}"
    "-DCMAKE_C_FLAGS_MINSIZEREL=${CMAKE_C_FLAGS_MINSIZEREL}"
    "-DCMAKE_CXX_FLAGS_MINSIZEREL=${CMAKE_CXX_FLAGS_MINSIZEREL}"
    "-DCMAKE_C_FLAGS_RELWITHDEBINFO=${CMAKE_C_FLAGS_RELWITHDEBINFO}"
    "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=${CMAKE_CXX_FLAGS_RELWITHDEBINFO}"
    "-DCMAKE_C_FLAGS_DEBUG=${CMAKE_C_FLAGS_DEBUG}"
    "-DCMAKE_CXX_FLAGS_DEBUG=${CMAKE_CXX_FLAGS_DEBUG}"

    "-DBOOST_ROOT:PATH=${BOOST_ROOT}"
    "-DBoost_ROOT:PATH=${BOOST_ROOT}"
    -DBoost_USE_STATIC_LIBS=${Boost_USE_STATIC_LIBS}
    -DBoost_USE_STATIC_RUNTIME=${Boost_USE_STATIC_RUNTIME}
    -DBoost_USE_DEBUG_LIBS=${Boost_USE_DEBUG_LIBS}
    -DBoost_USE_RELEASE_LIBS=${Boost_USE_RELEASE_LIBS}
    -DBoost_USE_DEBUG_RUNTIME=${Boost_USE_DEBUG_RUNTIME}
    -DBoost_USE_RELEASE_RUNTIME=${Boost_USE_RELEASE_RUNTIME}
    "-DOPENSSL_ROOT_DIR:PATH=${OPENSSL_ROOT_DIR}"
    -DOPENSSL_USE_STATIC_LIBS=${OPENSSL_USE_STATIC_LIBS}
    -DYAML_CPP_STATIC_DEFINE=ON

    -DAGENTXX_ENABLE_ASAN=${AGENTXX_ENABLE_ASAN}
    -DAGENTXX_ENABLE_DEBUG_INFO=${AGENTXX_ENABLE_DEBUG_INFO}
    -DAGENTXX_ENABLE_PCH=${AGENTXX_ENABLE_PCH}
    
    -DXX_IS_DEBUG_D=${XX_IS_DEBUG_D}
    -DXX_IS_RELEASE_D=${XX_IS_RELEASE_D}

    -DXX_IS_LINUX_D=${XX_IS_LINUX_D}
    -DXX_IS_WIN_D=${XX_IS_WIN_D}
    -DXX_IS_MACOS_D=${XX_IS_MACOS_D}
    -DXX_IS_ANDROID_D=${XX_IS_ANDROID_D}
    -DXX_IS_IOS_D=${XX_IS_IOS_D}

    -DXX_IS_MSVC_D=${XX_IS_MSVC_D}
    -DXX_IS_GCC_D=${XX_IS_GCC_D}
    -DXX_IS_CLANG_D=${XX_IS_CLANG_D}

    "-DAGENTXX_INSTALL_DIR=${AGENTXX_INSTALL_DIR}"
    -DAGENTXX_ENABLE_HYPERSCAN=${AGENTXX_ENABLE_HYPERSCAN}
    -DAGENTXX_ENABLE_PLUGIN_CODEGRAPH=${AGENTXX_ENABLE_PLUGIN_CODEGRAPH}
    -DAGENTXX_ENABLE_BOOST_PROCESS=${AGENTXX_ENABLE_BOOST_PROCESS}
    -DAGENTXX_ENABLE_PLUGIN_JS=${AGENTXX_ENABLE_PLUGIN_JS}
    -DAGENTXX_ENABLE_PLUGIN_SYSTEM_MONITOR=${AGENTXX_ENABLE_PLUGIN_SYSTEM_MONITOR}
    -DAGENTXX_ENABLE_PLUGIN_AUDIO_STREAM=${AGENTXX_ENABLE_PLUGIN_AUDIO_STREAM}
    -DAGENTXX_ENABLE_PLUGIN_TEXT_SELECTION=${AGENTXX_ENABLE_PLUGIN_TEXT_SELECTION}
    -DAGENTXX_ENABLE_PLUGIN_EXAMPLE=${AGENTXX_ENABLE_PLUGIN_EXAMPLE}
    -DAGENTXX_ENABLE_PLUGIN_COMPUTER_USE=${AGENTXX_ENABLE_PLUGIN_COMPUTER_USE}
    -DAGENTXX_ENABLE_PLUGIN_SCREEN_CAPTURE=${AGENTXX_ENABLE_PLUGIN_SCREEN_CAPTURE}
    -DAGENTXX_ENABLE_PLUGIN_BUILTIN=${AGENTXX_ENABLE_PLUGIN_BUILTIN}

    "-DAGENTXX_EXEC_INSTALL_PREFIX=${AGENTXX_EXEC_INSTALL_PREFIX}"
    "-DAGENTXX_THIRD_PARTY_DIR=${AGENTXX_THIRD_PARTY_DIR}"
    -DAGENTXX_LINUX_IO_URING_SUPPORTED=${AGENTXX_LINUX_IO_URING_SUPPORTED}
    -DAGENTXX_WINDOWS_IOCP_SUPPORTED=${AGENTXX_WINDOWS_IOCP_SUPPORTED}
)
if (ANDROID_PLATFORM)
  list(APPEND _AGENTXX_COMMON_CMAKE_ARGS
    "-DCMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}"
    -DANDROID_ABI=${ANDROID_ABI}
    -DANDROID_PLATFORM=${ANDROID_PLATFORM}
    -DANDROID_STL=${ANDROID_STL}
  )
endif()

message("[agentxx] _AGENTXX_COMMON_CMAKE_ARGS: ${_AGENTXX_COMMON_CMAKE_ARGS}")
message("[agentxx] _AGENTXX_COMMON_CMAKE_CACHE_ARGS: ${_AGENTXX_COMMON_CMAKE_CACHE_ARGS}")
message("[agentxx] BOOST_ROOT: ${BOOST_ROOT}")
message("[agentxx] OPENSSL_ROOT_DIR: ${OPENSSL_ROOT_DIR}")
message("[agentxx] CMAKE_FIND_ROOT_PATH: ${CMAKE_FIND_ROOT_PATH}")

include(ExternalProject)
include(FetchContent)

# 断言查找依赖
find_package(Boost REQUIRED)
find_package(OpenSSL REQUIRED)

# liburing
if (AGENTXX_LINUX_IO_URING_SUPPORTED)
  set(LIBURING_SOURCE_DIR "${AGENTXX_THIRD_PARTY_DIR}/liburing")
  set(LIBURING_BINARY_DIR "${CMAKE_BINARY_DIR}/liburing-build")
  ExternalProject_Add(
    liburing_repo
    SOURCE_DIR "${LIBURING_SOURCE_DIR}"
    BINARY_DIR "${LIBURING_BINARY_DIR}"
    INSTALL_DIR "${AGENTXX_INSTALL_DIR}"
    CONFIGURE_COMMAND 
        cp -r "${LIBURING_SOURCE_DIR}" "${LIBURING_BINARY_DIR}/" && 
        cd "${LIBURING_BINARY_DIR}/liburing" && 
        "${LIBURING_BINARY_DIR}/liburing/configure"
        --prefix=${AGENTXX_INSTALL_DIR}
        --cc=${CMAKE_C_COMPILER}
        --cxx=${CMAKE_CXX_COMPILER}
    BUILD_COMMAND 
      cd "${LIBURING_BINARY_DIR}/liburing/" && make -j${CMAKE_BUILD_PARALLEL_LEVEL}
    INSTALL_COMMAND 
      cd "${LIBURING_BINARY_DIR}/liburing/" && make install
    TEST_COMMAND ""
  )
endif()

# simdjson
ExternalProject_Add(
  simdjson_repo
  SOURCE_DIR "${AGENTXX_THIRD_PARTY_DIR}/simdjson/"
  INSTALL_DIR "${AGENTXX_INSTALL_DIR}"
  CMAKE_ARGS
    ${_AGENTXX_COMMON_CMAKE_ARGS}
    -DSIMDJSON_BUILD_STATIC_LIB=ON
    -DSIMDJSON_INSTALL=ON
    -DSIMDJSON_STATIC_REFLECTION=OFF
    -DSIMDJSON_SINGLEHEADER=ON
    -DBUILD_SHARED_LIBS=OFF
    -DBUILD_STATIC_LIBS=ON
  CMAKE_CACHE_ARGS
    ${_AGENTXX_COMMON_CMAKE_CACHE_ARGS}
  TEST_COMMAND ""
)

# iconv
ExternalProject_Add(
  libiconv_repo
  SOURCE_DIR "${AGENTXX_THIRD_PARTY_DIR}/libiconv-native/"
  INSTALL_DIR "${AGENTXX_INSTALL_DIR}"
  CMAKE_ARGS
    ${_AGENTXX_COMMON_CMAKE_ARGS}
    -DICONV_ENABLE_EXTRA=OFF
    -DICONV_EMBED_MODE=OFF
    -DBUILD_SHARED_LIBS=OFF
    -DBUILD_STATIC_LIBS=ON
  CMAKE_CACHE_ARGS
    ${_AGENTXX_COMMON_CMAKE_CACHE_ARGS}
  TEST_COMMAND ""
)

# zlib
ExternalProject_Add(
  zlib_repo
  SOURCE_DIR "${AGENTXX_THIRD_PARTY_DIR}/zlib-ng/"
  INSTALL_DIR "${AGENTXX_INSTALL_DIR}"
  CMAKE_ARGS
    ${_AGENTXX_COMMON_CMAKE_ARGS}
    -DBUILD_TESTING=OFF
    -DZLIB_ALIASES=ON
    -DZLIB_COMPAT=ON
    -DBUILD_TEST=OFF
    -DINSTALL_UTILS=OFF
    -DBUILD_SHARED_LIBS=OFF
    -DBUILD_STATIC_LIBS=ON
  CMAKE_CACHE_ARGS
    ${_AGENTXX_COMMON_CMAKE_CACHE_ARGS}
  TEST_COMMAND ""
)

# sqlite
# https://sqlite.org/download.html
# 复制版本和链接 sqlite-amalgamation-xxx.zip 
ExternalProject_Add(
  sqlite3_repo
  SOURCE_DIR "${AGENTXX_THIRD_PARTY_DIR}/sqlite3-cmake/"
  INSTALL_DIR "${AGENTXX_INSTALL_DIR}"
  CMAKE_ARGS
    ${_AGENTXX_COMMON_CMAKE_ARGS}
    "-DSQLITE3_SOURCE_DIR="
    "-DSQLITE3_VERSION=3.53.2"
    "-DSQLITE3_URL=https://sqlite.org/2026/sqlite-amalgamation-3530200.zip"
    -Dsqlite3_ENABLE_THREADSAFE=ON
    -Dsqlite3_ENABLE_DYNAMIC_EXTENSIONS=OFF
    -Dsqlite3_ENABLE_MATH=ON
    -Dsqlite3_ENABLE_FTS3=OFF
    -Dsqlite3_ENABLE_FTS4=ON
    -Dsqlite3_ENABLE_FTS5=ON
    -Dsqlite3_ENABLE_RTREE=ON
    -Dsqlite3_ENABLE_SESSION=OFF
    -Dsqlite3_OMIT_DEPRECATED=ON
    -Dsqlite3_BUILD_SHARED_LIBS=OFF
    -DBUILD_SHARED_LIBS=OFF
    -DBUILD_STATIC_LIBS=ON
  CMAKE_CACHE_ARGS
    ${_AGENTXX_COMMON_CMAKE_CACHE_ARGS}
    -DHAVE_GNU_STRERROR_R_EXITCODE:STRING=0
  TEST_COMMAND ""
)

# fmt
ExternalProject_Add(
  fmt_repo
  SOURCE_DIR "${AGENTXX_THIRD_PARTY_DIR}/fmt/"
  INSTALL_DIR "${AGENTXX_INSTALL_DIR}"
  CMAKE_ARGS
    ${_AGENTXX_COMMON_CMAKE_ARGS}
    -DFMT_PEDANTIC=ON
    -DFMT_INSTALL=ON
    -DFMT_WERROR=OFF
    -DFMT_DOC=OFF
    -DFMT_TEST=OFF
    -DFMT_OS=OFF
    -DFMT_UNICODE=ON
    -DBUILD_SHARED_LIBS=OFF
    -DBUILD_STATIC_LIBS=ON
  CMAKE_CACHE_ARGS
    ${_AGENTXX_COMMON_CMAKE_CACHE_ARGS}
  TEST_COMMAND ""
)

# uchardet
ExternalProject_Add(
  uchardet_repo
  SOURCE_DIR "${AGENTXX_THIRD_PARTY_DIR}/uchardet/"
  INSTALL_DIR "${AGENTXX_INSTALL_DIR}"
  CMAKE_ARGS
    ${_AGENTXX_COMMON_CMAKE_ARGS}
    -DCHECK_SSE2=ON
    -DBUILD_BINARY=OFF
    -DBUILD_SHARED_LIBS=OFF
    -DBUILD_STATIC_LIBS=ON
  CMAKE_CACHE_ARGS
    ${_AGENTXX_COMMON_CMAKE_CACHE_ARGS}
  TEST_COMMAND ""
)

# curl
ExternalProject_Add(
  curl_repo
  # 先构建 zlib: curl configure 时 find_package(ZLIB) 才能命中本项目
  # 安装的 zlib (AGENTXX_INSTALL_DIR), 避免 fallback 系统 zlib;
  # openssl 为预构建目录 (third_party/OpenSSL-windows-build), 无构建顺序问题
  DEPENDS zlib_repo
  SOURCE_DIR "${AGENTXX_THIRD_PARTY_DIR}/curl/"
  INSTALL_DIR "${AGENTXX_INSTALL_DIR}"
  CMAKE_ARGS
    ${_AGENTXX_COMMON_CMAKE_ARGS}
    -DBUILD_CURL_EXE=OFF
    -DCURL_DISABLE_INSTALL=OFF
    -DCURL_BUILD_EVERYTHING=OFF
    -DENABLE_UNICODE=ON
    -DCURL_LTO=ON
    -DENABLE_DEBUG=OFF
    -DCURL_DROP_UNUSED=ON
    -DCURL_USE_CMAKECONFIG=ON
    -DCURL_USE_PKGCONFIG=ON
    -DCURL_ENABLE_EXPORT_TARGET=ON
    -DUSE_LIBIDN2=OFF
    -DCURL_USE_LIBPSL=OFF
    -DCURL_USE_LIBSSH2=OFF
    -DCURL_USE_LIBSSH=OFF
    -DCURL_USE_GSASL=OFF
    -DCURL_USE_GSSAPI=OFF
    -DCURL_USE_LIBBACKTRACE=OFF
    -DBUILD_LIBCURL_DOCS=OFF
    -DBUILD_MISC_DOCS=OFF
    -DENABLE_CURL_MANUAL=OFF
    -DENABLE_IPV6=ON
    -DCURL_ENABLE_SSL=ON
    -DCURL_USE_OPENSSL=ON
    -DBUILD_TESTING=OFF
    -DBUILD_EXAMPLES=OFF
    -DBUILD_SHARED_LIBS=OFF
    -DBUILD_STATIC_LIBS=ON
  CMAKE_CACHE_ARGS
    ${_AGENTXX_COMMON_CMAKE_CACHE_ARGS}
    "-DOPENSSL_ROOT_DIR:PATH=${OPENSSL_ROOT_DIR}"
  TEST_COMMAND ""
)

# codegraph-cpp
if (AGENTXX_ENABLE_PLUGIN_CODEGRAPH)
  ExternalProject_Add(
    codegraph_repo
    DEPENDS sqlite3_repo
    SOURCE_DIR "${AGENTXX_THIRD_PARTY_DIR}/codegraph-cpp/"
    INSTALL_DIR "${AGENTXX_INSTALL_DIR}"
    CMAKE_ARGS
      ${_AGENTXX_COMMON_CMAKE_ARGS}
      -DBUILD_SHARED_LIBS=OFF
      -DBUILD_STATIC_LIBS=ON
    CMAKE_CACHE_ARGS
      ${_AGENTXX_COMMON_CMAKE_CACHE_ARGS}
    TEST_COMMAND ""
  )
endif()
if (AGENTXX_ENABLE_HYPERSCAN)
  ExternalProject_Add(
    hyperscan_repo
    SOURCE_DIR "${AGENTXX_THIRD_PARTY_DIR}/hyperscan/"
    INSTALL_DIR "${AGENTXX_INSTALL_DIR}"
    CMAKE_ARGS
      ${_AGENTXX_COMMON_CMAKE_ARGS}
      -DBUILD_STATIC_AND_SHARED=OFF
      -DBUILD_EXAMPLES=OFF
      -DBUILD_TOOLS=OFF
      -DBUILD_UNIT=OFF
      -DDEBUG_OUTPUT=OFF
      # -DFAT_RUNTIME=OFF # auto
      -DBUILD_SHARED_LIBS=OFF
      -DBUILD_STATIC_LIBS=ON
      "-DCMAKE_POLICY_VERSION_MINIMUM:STRING=3.15"
      -DCMAKE_C_STANDARD=11
      -DCMAKE_CXX_STANDARD=11
    CMAKE_CACHE_ARGS
      ${_AGENTXX_COMMON_CMAKE_CACHE_ARGS}
      "-DBOOST_ROOT:PATH=${BOOST_ROOT}"
      "-DOPENSSL_ROOT_DIR:PATH=${OPENSSL_ROOT_DIR}"
    TEST_COMMAND ""
  )
else()
  message("[agentxx] Disable HyperScan")
endif()

# neograph
set(neograph_SOURCE_DIR "${AGENTXX_THIRD_PARTY_DIR}/neograph/")
ExternalProject_Add(
  neograph_repo
  DEPENDS 
    sqlite3_repo
    curl_repo
  SOURCE_DIR ${neograph_SOURCE_DIR}
  INSTALL_DIR "${AGENTXX_INSTALL_DIR}"
  CMAKE_ARGS
    ${_AGENTXX_COMMON_CMAKE_ARGS}
    -DNEOGRAPH_ENABLE_IO_URING=${AGENTXX_LINUX_IO_URING_SUPPORTED}
    -DNEOGRAPH_BUILD_EXAMPLES=OFF
    -DNEOGRAPH_BUILD_TESTS=OFF
    -DNEOGRAPH_BUILD_PYBIND=OFF
    -DNEOGRAPH_BUILD_A2A=OFF
    -DNEOGRAPH_BUILD_POSTGRES=OFF
    -DNEOGRAPH_BUILD_SQLITE=ON
    -DNEOGRAPH_BUILD_ASYNC=ON
    -DNEOGRAPH_BUILD_LLM=ON
    -DNEOGRAPH_BUILD_MCP=ON
    -DNEOGRAPH_BUILD_ACP=ON
    -DNEOGRAPH_BUILD_UTIL=ON
    -DNEOGRAPH_USE_LIBCURL=OFF
    -DNEOGRAPH_INSTALL_HEADERS=ON
    -DBUILD_SHARED_LIBS=OFF
    -DBUILD_STATIC_LIBS=ON
  CMAKE_CACHE_ARGS
    ${_AGENTXX_COMMON_CMAKE_CACHE_ARGS}
    "-DBOOST_ROOT:PATH=${BOOST_ROOT}"
    "-DOPENSSL_ROOT_DIR:PATH=${OPENSSL_ROOT_DIR}"
  TEST_COMMAND ""
)

# html2md
ExternalProject_Add(
  html2md_repo
  SOURCE_DIR "${AGENTXX_THIRD_PARTY_DIR}/html2md/"
  INSTALL_DIR "${AGENTXX_INSTALL_DIR}"
  CMAKE_ARGS
    ${_AGENTXX_COMMON_CMAKE_ARGS}
    -DBUILD_EXE=OFF
    -DBUILD_DOC=OFF
    -DBUILD_TEST=OFF
    -DPYTHON_BINDINGS=OFF
    -DBUILD_SHARED_LIBS=OFF
    -DBUILD_STATIC_LIBS=ON
  CMAKE_CACHE_ARGS
    ${_AGENTXX_COMMON_CMAKE_CACHE_ARGS}
  TEST_COMMAND ""
)

# yaml-cpp
ExternalProject_Add(
  yaml_cpp_repo
  SOURCE_DIR "${AGENTXX_THIRD_PARTY_DIR}/yaml-cpp/"
  INSTALL_DIR "${AGENTXX_INSTALL_DIR}"
  CMAKE_ARGS
    ${_AGENTXX_COMMON_CMAKE_ARGS}
    -DCMAKE_C_STANDARD=${CMAKE_C_STANDARD}
    -DCMAKE_CXX_STANDARD=23
    -DYAML_CPP_BUILD_CONTRIB=OFF
    -DYAML_CPP_BUILD_TOOLS=ON
    -DYAML_BUILD_SHARED_LIBS=OFF
    -DYAML_ENABLE_PIC=ON
    -DYAML_CPP_BUILD_TESTS=OFF
    -DYAML_CPP_INSTALL=ON
    -DBUILD_SHARED_LIBS=OFF
    -DBUILD_STATIC_LIBS=ON 
  CMAKE_CACHE_ARGS
    ${_AGENTXX_COMMON_CMAKE_CACHE_ARGS}
    "-DBOOST_ROOT:PATH=${BOOST_ROOT}"
    "-DOPENSSL_ROOT_DIR:PATH=${OPENSSL_ROOT_DIR}"
  TEST_COMMAND ""
)

# glob
ExternalProject_Add(
  glob_repo
  SOURCE_DIR "${AGENTXX_THIRD_PARTY_DIR}/glob/"
  INSTALL_DIR "${AGENTXX_INSTALL_DIR}"
  CMAKE_ARGS
    ${_AGENTXX_COMMON_CMAKE_ARGS}
  CMAKE_CACHE_ARGS
    ${_AGENTXX_COMMON_CMAKE_CACHE_ARGS}
  TEST_COMMAND ""
)

if (AGENTXX_BUILD_CLIENT)
  # FTXUI
  ExternalProject_Add(
    ftxui_repo
    SOURCE_DIR "${AGENTXX_THIRD_PARTY_DIR}/ftxui/"
    INSTALL_DIR "${AGENTXX_INSTALL_DIR}"
    CMAKE_ARGS
      ${_AGENTXX_COMMON_CMAKE_ARGS}
      -DFTXUI_BUILD_DOCS=OFF
      -DFTXUI_BUILD_EXAMPLES=OFF
      -DFTXUI_BUILD_MODULES=OFF
      -DFTXUI_BUILD_TESTS=OFF
      -DFTXUI_BUILD_TESTS_FUZZER=OFF
      -DFTXUI_CLANG_TIDY=OFF
      -DFTXUI_DEV_WARNINGS=ON
      -DFTXUI_ENABLE_COVERAGE=OFF
      -DFTXUI_ENABLE_INSTALL=ON
      -DFTXUI_QUIET=OFF
    CMAKE_CACHE_ARGS
      ${_AGENTXX_COMMON_CMAKE_CACHE_ARGS}
    TEST_COMMAND ""
  )

  # cmark-gfm; required by markdown-ui
  ExternalProject_Add(
    cmark_gfm_repo
    SOURCE_DIR "${AGENTXX_THIRD_PARTY_DIR}/cmark_gfm/"
    INSTALL_DIR "${AGENTXX_INSTALL_DIR}"
    CMAKE_ARGS
      ${_AGENTXX_COMMON_CMAKE_ARGS}
      -DCMARK_TESTS=OFF
      -DCMARK_STATIC=ON
      -DCMARK_SHARED=OFF
      -DCMARK_LIB_FUZZER=OFF
      -DCMARK_FUZZ_QUADRATIC=OFF
      -DBUILD_SHARED_LIBS=OFF
      -DBUILD_STATIC_LIBS=ON
    CMAKE_CACHE_ARGS
      ${_AGENTXX_COMMON_CMAKE_CACHE_ARGS}
    TEST_COMMAND ""
  )

  # markdown ftxui
  ExternalProject_Add(
    markdown_ftxui_repo
    DEPENDS
      ftxui_repo
      cmark_gfm_repo
    SOURCE_DIR "${AGENTXX_THIRD_PARTY_DIR}/markdown_ftxui/"
    INSTALL_DIR "${AGENTXX_INSTALL_DIR}"
    CMAKE_ARGS
      ${_AGENTXX_COMMON_CMAKE_ARGS}
      -DMARKDOWN_UI_BUILD_TESTS=OFF
      -DMARKDOWN_UI_BUILD_DEMO=OFF
      -DBUILD_SHARED_LIBS=OFF
      -DBUILD_STATIC_LIBS=ON
    CMAKE_CACHE_ARGS
      ${_AGENTXX_COMMON_CMAKE_CACHE_ARGS}
    TEST_COMMAND ""
  )
endif()

ExternalProject_Add(
  agentxx_lib_repo
  DEPENDS 
    simdjson_repo
    libiconv_repo
    zlib_repo
    sqlite3_repo
    fmt_repo
    uchardet_repo
    neograph_repo
    html2md_repo
    yaml_cpp_repo
    glob_repo
  SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/"
  INSTALL_DIR "${AGENTXX_INSTALL_DIR}"
  BUILD_ALWAYS TRUE
  INSTALL_ALWAYS TRUE 
  CMAKE_ARGS
    ${_AGENTXX_COMMON_CMAKE_ARGS}
    -Dneograph_SOURCE_DIR=${neograph_SOURCE_DIR}
  CMAKE_CACHE_ARGS
    ${_AGENTXX_COMMON_CMAKE_CACHE_ARGS}
  TEST_COMMAND ""
)

# QuickJS (JS 插件引擎 agentxx_javascript_engine 的运行时)
# - 静态库 libqjs.a + 头文件 install 到 AGENTXX_INSTALL_DIR
# - 关闭 CLI/示例/测试, 仅编译核心库
if (AGENTXX_ENABLE_PLUGIN_JS)
  ExternalProject_Add(
    quickjs_repo
    SOURCE_DIR "${AGENTXX_THIRD_PARTY_DIR}/quickjs/"
    INSTALL_DIR "${AGENTXX_INSTALL_DIR}"
    CMAKE_ARGS
      ${_AGENTXX_COMMON_CMAKE_ARGS}
      -DBUILD_SHARED_LIBS=OFF
      -DQJS_BUILD_CLI_STATIC=OFF
      -DQJS_BUILD_EXAMPLES=OFF
      -DQJS_BUILD_WERROR=OFF
    CMAKE_CACHE_ARGS
      ${_AGENTXX_COMMON_CMAKE_CACHE_ARGS}
    TEST_COMMAND ""
  )
endif()
if (AGENTXX_LINUX_IO_URING_SUPPORTED)
  add_dependencies(agentxx_lib_repo liburing_repo)
endif()
if (AGENTXX_ENABLE_HYPERSCAN)
  add_dependencies(agentxx_lib_repo hyperscan_repo)
endif()
if (AGENTXX_ENABLE_PLUGIN_CODEGRAPH)
  add_dependencies(agentxx_lib_repo codegraph_repo)
endif()
# 内置合并编译模式: libagentxx 需先构建插件依赖库 (quickjs/codegraph),
# 供 lib/CMakeLists.txt 内置分支 find_library + 链接
if (AGENTXX_ENABLE_PLUGIN_BUILTIN AND AGENTXX_ENABLE_PLUGIN_JS)
  add_dependencies(agentxx_lib_repo quickjs_repo)
endif()

# 插件 (独立编译, 见 plugins/CMakeLists.txt)
# - 各插件子目录 (example_plugin/javascript_engine/example_js) 自带 CMakeLists.txt,
#   经 plugins/CMakeLists.txt add_subdirectory 汇总; 也可独立构建:
#   cmake -B build -S agent/plugins -DAGENTXX_INSTALL_DIR=<agentxx-project-install>
# - 插件不链接 libagentxx, 仅依赖纯 C ABI 头; JS 插件 (javascript_engine/example_js)
#   需 quickjs 库
# - 内置合并编译模式 (AGENTXX_ENABLE_PLUGIN_BUILTIN=ON) 下跳过本目标: 插件
#   源文件由 agentxx_lib_repo (libagentxx) 直接合并编译, 无需插件动态库
if (AGENTXX_BUILD_PLUGINS AND NOT AGENTXX_ENABLE_PLUGIN_BUILTIN)
  ExternalProject_Add(
    agentxx_plugins_repo
    DEPENDS 
      agentxx_lib_repo
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/plugins/"
    INSTALL_DIR "${AGENTXX_INSTALL_DIR}"
    BUILD_ALWAYS TRUE
    INSTALL_ALWAYS TRUE
    CMAKE_ARGS
      ${_AGENTXX_COMMON_CMAKE_ARGS}
    CMAKE_CACHE_ARGS
      ${_AGENTXX_COMMON_CMAKE_CACHE_ARGS}
    TEST_COMMAND ""
    # 插件动态库直接输出到 ${AGENTXX_EXEC_INSTALL_PREFIX} (见各插件 CMakeLists.txt),
    # 无需 install 步骤
    INSTALL_COMMAND ""
  )
  if (AGENTXX_ENABLE_PLUGIN_JS)
    # 编译 agentxx_javascript_engine / example_js 需 quickjs 库
    add_dependencies(agentxx_plugins_repo quickjs_repo)
  endif()
endif()

if (AGENTXX_BUILD_CLIENT)
  ExternalProject_Add(
    agentxx_client_repo
    DEPENDS 
      agentxx_lib_repo
      ftxui_repo
      cmark_gfm_repo
      markdown_ftxui_repo
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/client/"
    INSTALL_DIR "${AGENTXX_INSTALL_DIR}"
    BUILD_ALWAYS TRUE
    INSTALL_ALWAYS TRUE
    CMAKE_ARGS
      ${_AGENTXX_COMMON_CMAKE_ARGS}
    CMAKE_CACHE_ARGS
      ${_AGENTXX_COMMON_CMAKE_CACHE_ARGS}
    TEST_COMMAND ""
  )
endif()
if (AGENTXX_BUILD_TEST)
  ExternalProject_Add(
    agentxx_test_repo
    DEPENDS 
      agentxx_lib_repo
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/test/"
    INSTALL_DIR "${AGENTXX_INSTALL_DIR}"
    BUILD_ALWAYS TRUE
    CMAKE_ARGS
      ${_AGENTXX_COMMON_CMAKE_ARGS}
      -DAGENTXX_BUILD_CLIENT=${AGENTXX_BUILD_CLIENT}
    CMAKE_CACHE_ARGS
      ${_AGENTXX_COMMON_CMAKE_CACHE_ARGS}
    TEST_COMMAND ""
  )
  if (AGENTXX_BUILD_CLIENT)
    add_dependencies(agentxx_test_repo agentxx_client_repo)
  endif()
  if (AGENTXX_BUILD_PLUGINS AND NOT AGENTXX_ENABLE_PLUGIN_BUILTIN)
    # 测试运行时加载插件 (与测试可执行同目录输出), 需先于测试编译;
    # 内置合并编译模式下插件随 libagentxx 构建 (资源目录由 lib 构建产出)
    add_dependencies(agentxx_test_repo agentxx_plugins_repo)
  endif()
endif()
if (AGENTXX_BUILD_BENCHMARK)
  ExternalProject_Add(
    agentxx_benchmark_repo
    DEPENDS agentxx_lib_repo
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/benchmark/"
    INSTALL_DIR "${AGENTXX_INSTALL_DIR}"
    BUILD_ALWAYS TRUE
    INSTALL_ALWAYS TRUE
    CMAKE_ARGS
      ${_AGENTXX_COMMON_CMAKE_ARGS}
    CMAKE_CACHE_ARGS
      ${_AGENTXX_COMMON_CMAKE_CACHE_ARGS}
    TEST_COMMAND ""
  )
endif()