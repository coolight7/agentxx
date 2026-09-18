# ===== cxx_utilxx_base: 无重依赖基础工具库 =====
#
# 定位: 与宿主无关的基础设施 (日志/JSON/字符串/容器/环境/系统探测/取消令牌/
# 异步卸载), 供 cxx_utilxx / cxx_pluginxx / 各宿主 (agentxx、musicxx 等) 复用。
# 依赖: fmt / simdjson / Boost (头) / iconv + uchardet (可选, 字符编码) /
# liburing (可选, Linux 文件异步 I/O; 导出接口以库名 PkgConfig::uring 声明)
# 产物: 动态库与静态库同时产出, 命名规则与 libagentxx 一致
#       Release: libcxx_utilxx_base.so       libcxx_utilxx_base_static.a
#       Debug:   libcxx_utilxx_based.so      libcxx_utilxx_base_staticd.a
#
# 使用 (处理方):
#   find_package(cxx_utilxx_base REQUIRED)
#   # 条件依赖只声明库名, 由使用方在本机 find (是否需要见包 config 的开关):
#   if (CXX_UTILXX_BASE_LINUX_IO_URING_SUPPORTED)
#     pkg_check_modules(uring REQUIRED IMPORTED_TARGET liburing)
#   endif ()
#   target_link_libraries(<your_target> PRIVATE cxx_utilxx_base_static)  # 或 _shared

cmake_minimum_required(VERSION 3.10)
cmake_policy(SET CMP0074 NEW) # find_package 使用 <PackageName>_ROOT 变量
cmake_policy(SET CMP0091 NEW) # MSVC 运行时库由抽象层选择
cmake_policy(SET CMP0144 NEW) # find_package 使用大写的 _ROOT 变量
cmake_policy(SET CMP0167 OLD) # 不使用 CMake 内置 FindBoost 模块

if (NOT DEFINED AGENTXX_VERSION)
  set(AGENTXX_VERSION "0.1.0")
endif ()

project(cxx_utilxx_base VERSION ${AGENTXX_VERSION} LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)

if (XX_IS_MSVC_D)
  list(PREPEND CMAKE_FIND_LIBRARY_SUFFIXES .lib .a)
else ()
  list(PREPEND CMAKE_FIND_LIBRARY_SUFFIXES .a)
endif ()

# 平台/编译器宏 (XX_IS_*_D): 嵌套构建沿用上层判定, 独立构建本地推导
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/xx_platform_macros.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/xx_rpath.cmake")

option(CXX_UTILXX_BASE_ENABLE_CHARSET "Enable iconv/uchardet charset support." ON)
option(CXX_UTILXX_BASE_USE_BOOST_ASIO "Use Boost.Asio (define UTILXX_USE_BOOST_ASIO)." ON)
# Linux/Android 文件异步 I/O: asio 的底层实现是 io_uring, system.cpp 的运行时探测
# 直接调用 io_uring_queue_init/io_uring_queue_exit; 开启后导出接口以**库名**
# (PkgConfig::uring) 声明该依赖, 具体库由使用方解析 (见下方 io_uring 依赖处理)
option(CXX_UTILXX_BASE_LINUX_IO_URING_SUPPORTED "Linux io_uring supported (asio file async IO)." OFF)

# 库产物安装目录: 嵌套构建时由 superbuild 传入 (与 libagentxx 同目录, 便于
# 统一管理运行期搜索路径); 独立构建时随标准库目录
if (NOT DEFINED AGENTXX_EXEC_INSTALL_PREFIX)
  set(AGENTXX_EXEC_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}")
endif ()

find_package(PkgConfig REQUIRED)
find_package(Boost CONFIG REQUIRED)
find_package(fmt REQUIRED)
find_package(simdjson REQUIRED)
if (CXX_UTILXX_BASE_ENABLE_CHARSET)
  # 必须显式 CONFIG 模式: CMake >= 3.28 内置 FindIconv 模块会命中系统 iconv
  # (且只创建 Iconv::Iconv 目标), CONFIG 模式强制匹配本项目安装的 iconvConfig.cmake
  find_package(iconv CONFIG REQUIRED)
  find_package(uchardet REQUIRED)
endif ()

# ===== 可选依赖: io_uring =====
# system.cpp 的异步文件 I/O 探测在 -DASIO_HAS_IO_URING 下直接调用
# io_uring_queue_init/io_uring_queue_exit, 本库自身链接该实现; 导出接口只**声明
# 依赖名 PkgConfig::uring**, 不写死库文件路径 —— 使用方 (插件/可执行程序/宿主)
# 直接在自己的 CMakeLists 依赖查找段按开关写 pkg_check_modules(uring REQUIRED
# IMPORTED_TARGET liburing) (是否需要见包 config 的开关, agentxx 侧用顶层
# AGENTXX_LINUX_IO_URING_SUPPORTED)。静态变体不携带依赖二进制, 使用方漏链接时
# 会在运行期 (dlopen) 报 `undefined symbol: io_uring_queue_init`
if (CXX_UTILXX_BASE_LINUX_IO_URING_SUPPORTED)
  pkg_check_modules(uring REQUIRED IMPORTED_TARGET liburing)
endif ()

set(_CXX_UTILXX_BASE_SOURCES
  src/env.cpp
  src/json.cpp
  src/json_view.cpp
  src/log.cpp
  src/string_util.cpp
  src/system.cpp
)

# ===== 目标: 静态 + 动态同时产出 =====
# - GCC/Clang: OBJECT 库只编译一次, 动态/静态复用同一批目标文件
# - MSVC: 双编译路径 (导出符号等语义差异, 与 libagentxx 保持一致)
if (XX_IS_MSVC_D)
  add_library(cxx_utilxx_base_shared SHARED ${_CXX_UTILXX_BASE_SOURCES})
  add_library(cxx_utilxx_base_static STATIC ${_CXX_UTILXX_BASE_SOURCES})
  set(_CXX_UTILXX_BASE_TARGETS cxx_utilxx_base_shared cxx_utilxx_base_static)
else ()
  add_library(cxx_utilxx_base_obj OBJECT ${_CXX_UTILXX_BASE_SOURCES})
  set_target_properties(cxx_utilxx_base_obj PROPERTIES POSITION_INDEPENDENT_CODE ON)
  add_library(cxx_utilxx_base_shared SHARED $<TARGET_OBJECTS:cxx_utilxx_base_obj>)
  add_library(cxx_utilxx_base_static STATIC $<TARGET_OBJECTS:cxx_utilxx_base_obj>)
  set(_CXX_UTILXX_BASE_TARGETS cxx_utilxx_base_obj cxx_utilxx_base_shared cxx_utilxx_base_static)
endif ()

if (XX_IS_DEBUG_D)
  set_target_properties(cxx_utilxx_base_shared PROPERTIES OUTPUT_NAME "cxx_utilxx_based")
  set_target_properties(cxx_utilxx_base_static PROPERTIES OUTPUT_NAME "cxx_utilxx_base_staticd")
else ()
  set_target_properties(cxx_utilxx_base_shared PROPERTIES OUTPUT_NAME "cxx_utilxx_base")
  set_target_properties(cxx_utilxx_base_static PROPERTIES OUTPUT_NAME "cxx_utilxx_base_static")
endif ()

set_target_properties(${_CXX_UTILXX_BASE_TARGETS} PROPERTIES
  PREFIX "lib"
  POSITION_INDEPENDENT_CODE ON
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED OFF
  LINK_LIBRARIES_STRATEGY "REORDER_MINIMALLY"
)

if (XX_IS_MSVC_D)
  # 导出本库自有符号 (只扫描本目标的目标文件, 不重导出链接进来的静态库符号)
  set_target_properties(cxx_utilxx_base_shared PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif ()

# ===== usage requirements: 施加到全部目标 (obj 供编译, shared/static 供传播) =====
set(_CXX_UTILXX_BASE_LINK_LIBS fmt::fmt simdjson::simdjson)
if (CXX_UTILXX_BASE_ENABLE_CHARSET)
  list(APPEND _CXX_UTILXX_BASE_LINK_LIBS uchardet::libuchardet Iconv::iconv)
  # Iconv::charset 仅 libiconv-native 的 config 提供 (Windows 需要 charset 支持)
  if (TARGET Iconv::charset)
    list(APPEND _CXX_UTILXX_BASE_LINK_LIBS Iconv::charset)
  endif ()
endif ()
if (CXX_UTILXX_BASE_LINUX_IO_URING_SUPPORTED)
  # 导出接口只声明依赖名 PkgConfig::uring (不写死库文件路径): 使用方自行
  # pkg_check_modules(uring REQUIRED IMPORTED_TARGET liburing) 解析;
  # 本库自身 (obj/shared 供链接) 也经该目标链接, 静态变体经 INTERFACE 传给使用方
  list(APPEND _CXX_UTILXX_BASE_LINK_LIBS PkgConfig::uring)
endif ()
if (XX_IS_WIN_D)
  # asio 网络栈: ws2_32 (socket) / mswsock (AcceptEx 等, MinGW 需显式链接)
  list(APPEND _CXX_UTILXX_BASE_LINK_LIBS ws2_32 mswsock)
endif ()

foreach (_t ${_CXX_UTILXX_BASE_TARGETS})
  target_compile_definitions(${_t} PUBLIC
    ${XX_PLATFORM_COMPILE_DEFINITIONS}
    XX_VERSION_STRING="${AGENTXX_VERSION}"
    BOOST_ASIO_STANDALONE=1
    ASIO_STANDALONE=1
    BOOST_PROCESS_USE_STD_FS=1
  )
  if (CXX_UTILXX_BASE_USE_BOOST_ASIO)
    target_compile_definitions(${_t} PUBLIC UTILXX_USE_BOOST_ASIO=1)
  endif ()
  if (XX_IS_WIN_D)
    target_compile_definitions(${_t} PUBLIC _WIN32_WINNT=0x0A00)
  endif ()
  target_include_directories(${_t} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  )
  # asio 前缀映射: 源码写 asio/xxx.hpp, 由 ${Boost_INCLUDE_DIRS}/boost/ 命中
  # boost/asio/xxx.hpp; boost asio 内部 <boost/asio/...> 由 ${Boost_INCLUDE_DIRS} 命中。
  # 两个 include 根缺一会让内部头回退到系统 Boost, 与宿主 asio 混用
  target_include_directories(${_t} SYSTEM PUBLIC
    "${Boost_INCLUDE_DIRS}"
    "${Boost_INCLUDE_DIRS}/boost/"
  )
  # 依赖全部 PUBLIC 传递 (静态库处理者需连带链接)
  target_link_libraries(${_t} PUBLIC ${_CXX_UTILXX_BASE_LINK_LIBS})
  if (CXX_UTILXX_BASE_LINUX_IO_URING_SUPPORTED)
    # 本库自身编译 system.cpp 需要 <liburing.h>: 只作本库内部编译用, 不进导出接口
    target_include_directories(${_t} SYSTEM PRIVATE ${uring_INCLUDE_DIRS})
  endif ()
endforeach ()
unset(_t)

# 动态库自身目录搜索路径 ($ORIGIN, 仅 ELF 生效)
cxx_lib_set_own_dir_rpath(cxx_utilxx_base_shared)

# ===== 安装与导出 =====
install(TARGETS cxx_utilxx_base_shared cxx_utilxx_base_static
  EXPORT cxx_utilxx_baseTargets
  DESTINATION "${AGENTXX_EXEC_INSTALL_PREFIX}"
  LIBRARY DESTINATION "${AGENTXX_EXEC_INSTALL_PREFIX}"
  RUNTIME DESTINATION "${AGENTXX_EXEC_INSTALL_PREFIX}"
  ARCHIVE DESTINATION "${AGENTXX_EXEC_INSTALL_PREFIX}"
)
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(EXPORT cxx_utilxx_baseTargets
  NAMESPACE ""
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cxx_utilxx_base"
)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cxx_utilxx_baseConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/cxx_utilxx_baseConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cxx_utilxx_base"
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/cxx_utilxx_baseConfig.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cxx_utilxx_base"
)
