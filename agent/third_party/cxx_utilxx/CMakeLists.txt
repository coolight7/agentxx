# ===== cxx_utilxx: 重依赖工具库 (HTTP/WS/正则/路由/差异/worktree/散列) =====
#
# 定位: 基于 cxx_utilxx_base, 提供网络与通用工具; 依赖 OpenSSL / Boost
# (beast + process) / fmt / html2md, 可选 hyperscan (正则);
# 注: 数据库 (SQLite) 相关工具已迁回宿主 (agentxx/lib/.../util), 本库不含数据库依赖
# 文件异步 I/O 的 io_uring 依赖在 cxx_utilxx_base (见该库 CMakeLists)
# 产物 (命名规则: Release 无后缀, Debug 文件名追加 d):
#   Release: libcxx_utilxx.so        libcxx_utilxx_static.a
#   Debug:   libcxx_utilxxd.so       libcxx_utilxx_staticd.a
#
# 使用 (处理方; 顺序: 先查找条件依赖, 再 find_package 本库):
#   if (XX_LINUX_IO_URING_SUPPORTED)    # cxx_utilxx_base 声明该依赖
#     find_package(PkgConfig REQUIRED)
#     pkg_check_modules(uring REQUIRED IMPORTED_TARGET liburing)
#   endif ()
#   if (CXX_UTILXX_ENABLE_HYPERSCAN)    # 本库声明该依赖
#     pkg_check_modules(hyperscan REQUIRED IMPORTED_TARGET libhs)
#   endif ()
#   find_package(cxx_utilxx REQUIRED)
#   target_link_libraries(<your_target> PRIVATE cxx_utilxx_static)   # 或 _shared
# 理由: 导出接口以目标名 (PkgConfig::hyperscan + 裸库名 hs_runtime / PkgConfig::uring)
# 声明条件依赖, 目标须在 find_package 之前就绪 (见 cmake/cxx_utilxxConfig.cmake.in)

cmake_minimum_required(VERSION 3.10)
cmake_policy(SET CMP0074 NEW)
cmake_policy(SET CMP0091 NEW)
cmake_policy(SET CMP0144 NEW)
cmake_policy(SET CMP0167 OLD)

set(UTILXX_VERSION "0.1.0")
project(cxx_utilxx VERSION ${UTILXX_VERSION} LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)

if (XX_IS_MSVC_D)
  list(PREPEND CMAKE_FIND_LIBRARY_SUFFIXES .lib .a)
else ()
  list(PREPEND CMAKE_FIND_LIBRARY_SUFFIXES .a)
endif ()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/xx_platform_macros.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/xx_rpath.cmake")

option(CXX_UTILXX_ENABLE_HYPERSCAN "Use HyperScan for regex (std::regex if off)." OFF)
option(CXX_UTILXX_ENABLE_BOOST_PROCESS "Use Boost.process for worktree commands." ON)
# 注: Linux io_uring 依赖 (asio 文件异步 I/O) 已随实现 (system.cpp 的运行时探测)
# 移到 cxx_utilxx_base, 由该库的 CXX_UTILXX_BASE_LINUX_IO_URING_SUPPORTED 开关处理

if (NOT DEFINED XX_EXEC_INSTALL_PREFIX)
  set(XX_EXEC_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}")
endif ()

find_package(PkgConfig REQUIRED)
# 依赖库的依赖先于依赖库自身查找: cxx_utilxx_base 的导出接口以目标名
# PkgConfig::uring 声明 io_uring (其构建开关 CXX_UTILXX_BASE_LINUX_IO_URING_SUPPORTED
# 与顶层 XX_LINUX_IO_URING_SUPPORTED 同源, superbuild 同时下发给两者)
if (XX_LINUX_IO_URING_SUPPORTED)
  pkg_check_modules(uring REQUIRED IMPORTED_TARGET liburing)
endif ()
find_package(Boost CONFIG REQUIRED)
if (CXX_UTILXX_ENABLE_BOOST_PROCESS)
  find_package(Boost CONFIG REQUIRED COMPONENTS process)
endif ()
find_package(OpenSSL REQUIRED)
find_package(fmt REQUIRED)
find_package(html2md REQUIRED)
# 依赖库本体 (config 内含 find_dependency 链, 但条件依赖目标须已由上方查找就绪)
find_package(cxx_utilxx_base REQUIRED)

set(_CXX_UTILXX_SOURCES
  src/crypto.cpp
  src/http_client.cpp
  src/http_header.cpp
  src/http_server.cpp
  src/regex.cpp
  src/ws_client.cpp
)

# ===== 可选依赖: hyperscan =====
# 导出接口只**声明依赖库名称**, 不写死库文件路径 (与项目其它库导出接口同为
# 名称形式: fmt::fmt / OpenSSL::SSL ...), 具体库由处理方在本机 find:
#   PkgConfig::hyperscan —— 按本库包 config 的开关 cxx_utilxx_ENABLE_HYPERSCAN
#                           (或本构建开关 CXX_UTILXX_ENABLE_HYPERSCAN) 写
#                           pkg_check_modules(hyperscan REQUIRED IMPORTED_TARGET libhs),
#                           宿主构建按其顶层开关判断
#   hs_runtime           —— HyperScan 运行期库 (libhs.pc 未覆盖), 按裸库名声明,
#                           经下方 target_link_directories 传播的
#                           ${XX_INSTALL_DIR}/lib 解析 (该目录只有 .a, 静态链接)
# io_uring 属 cxx_utilxx_base (其 system.cpp 使用), 依赖经本库 INTERFACE 间接传递
set(_CXX_UTILXX_EXTRA_LIBS "")
set(_CXX_UTILXX_EXTRA_DEFS "")
if (CXX_UTILXX_ENABLE_HYPERSCAN)
  pkg_check_modules(hyperscan REQUIRED IMPORTED_TARGET libhs)
  list(APPEND _CXX_UTILXX_EXTRA_LIBS PkgConfig::hyperscan hs_runtime)
  set(_CXX_UTILXX_EXTRA_INCLUDES ${hyperscan_INCLUDE_DIRS})
  list(APPEND _CXX_UTILXX_EXTRA_DEFS UTILXX_ENABLE_HYPERSCAN=1)
endif ()

# ===== 目标: 静态 + 动态同时产出 =====
if (XX_IS_MSVC_D)
  add_library(cxx_utilxx_shared SHARED ${_CXX_UTILXX_SOURCES})
  add_library(cxx_utilxx_static STATIC ${_CXX_UTILXX_SOURCES})
  set(_CXX_UTILXX_TARGETS cxx_utilxx_shared cxx_utilxx_static)
else ()
  add_library(cxx_utilxx_obj OBJECT ${_CXX_UTILXX_SOURCES})
  set_target_properties(cxx_utilxx_obj PROPERTIES POSITION_INDEPENDENT_CODE ON)
  add_library(cxx_utilxx_shared SHARED $<TARGET_OBJECTS:cxx_utilxx_obj>)
  add_library(cxx_utilxx_static STATIC $<TARGET_OBJECTS:cxx_utilxx_obj>)
  set(_CXX_UTILXX_TARGETS cxx_utilxx_obj cxx_utilxx_shared cxx_utilxx_static)
endif ()

if (XX_IS_DEBUG_D)
  set_target_properties(cxx_utilxx_shared PROPERTIES OUTPUT_NAME "cxx_utilxxd")
  set_target_properties(cxx_utilxx_static PROPERTIES OUTPUT_NAME "cxx_utilxx_staticd")
else ()
  set_target_properties(cxx_utilxx_shared PROPERTIES OUTPUT_NAME "cxx_utilxx")
  set_target_properties(cxx_utilxx_static PROPERTIES OUTPUT_NAME "cxx_utilxx_static")
endif ()

set_target_properties(${_CXX_UTILXX_TARGETS} PROPERTIES
  PREFIX "lib"
  POSITION_INDEPENDENT_CODE ON
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED OFF
  LINK_LIBRARIES_STRATEGY "REORDER_MINIMALLY"
)

if (XX_IS_MSVC_D)
  set_target_properties(cxx_utilxx_shared PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif ()

set(_CXX_UTILXX_LINK_LIBS
  cxx_utilxx_base_static
  fmt::fmt
  OpenSSL::SSL
  OpenSSL::Crypto
  html2md
)
if (CXX_UTILXX_ENABLE_BOOST_PROCESS)
  list(APPEND _CXX_UTILXX_LINK_LIBS Boost::process)
endif ()
list(APPEND _CXX_UTILXX_LINK_LIBS ${_CXX_UTILXX_EXTRA_LIBS})

foreach (_t ${_CXX_UTILXX_TARGETS})
  target_compile_definitions(${_t} PUBLIC
    ${XX_PLATFORM_COMPILE_DEFINITIONS}
    BOOST_ASIO_STANDALONE=1
    ASIO_STANDALONE=1
    BOOST_PROCESS_USE_STD_FS=1
    BOOST_EXCEPTION_MINI_BOOST=1
    UTILXX_USE_BOOST_ASIO=1
  )
  if (XX_IS_WIN_D)
    target_compile_definitions(${_t} PUBLIC _WIN32_WINNT=0x0A00)
  endif ()
  target_compile_definitions(${_t} PUBLIC ${_CXX_UTILXX_EXTRA_DEFS})
  target_include_directories(${_t} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  )
  target_include_directories(${_t} SYSTEM PUBLIC
    "${Boost_INCLUDE_DIRS}"
    "${Boost_INCLUDE_DIRS}/boost/"
  )
  target_include_directories(${_t} SYSTEM PRIVATE
    "${XX_INSTALL_DIR}/include"
    ${_CXX_UTILXX_EXTRA_INCLUDES}
  )
  target_link_directories(${_t} PUBLIC "${XX_INSTALL_DIR}/lib/")
  target_link_libraries(${_t} PUBLIC ${_CXX_UTILXX_LINK_LIBS})
endforeach ()
unset(_t)

cxx_lib_set_own_dir_rpath(cxx_utilxx_shared)

# ===== 安装与导出 =====
install(TARGETS cxx_utilxx_shared cxx_utilxx_static
  EXPORT cxx_utilxxTargets
  LIBRARY DESTINATION "${XX_EXEC_INSTALL_PREFIX}"
  RUNTIME DESTINATION "${XX_EXEC_INSTALL_PREFIX}"
  ARCHIVE DESTINATION "${XX_EXEC_INSTALL_PREFIX}"
)
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(EXPORT cxx_utilxxTargets
  NAMESPACE ""
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cxx_utilxx"
)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cxx_utilxxConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/cxx_utilxxConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cxx_utilxx"
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/cxx_utilxxConfig.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cxx_utilxx"
)
