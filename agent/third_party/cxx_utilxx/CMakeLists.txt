# ===== cxx_utilxx: 重依赖工具库 (HTTP/WS/SQLite/正则/路由/差异/worktree/散列) =====
#
# 定位: 基于 cxx_utilxx_base, 提供网络与存储类工具; 依赖 OpenSSL / SQLite / Boost
# (beast + process) / fmt / html2md, 可选 hyperscan (正则) 与 io_uring (文件异步 IO)。
# 产物 (命名规则同 libagentxx):
#   Release: libcxx_utilxx.so        libcxx_utilxx_static.a
#   Debug:   libcxx_utilxxd.so       libcxx_utilxx_staticd.a
#
# 使用 (处理方):
#   find_package(cxx_utilxx REQUIRED)
#   target_link_libraries(<your_target> PRIVATE cxx_utilxx_static)   # 或 _shared

cmake_minimum_required(VERSION 3.10)
cmake_policy(SET CMP0074 NEW)
cmake_policy(SET CMP0091 NEW)
cmake_policy(SET CMP0144 NEW)
cmake_policy(SET CMP0167 OLD)

if (NOT DEFINED AGENTXX_VERSION)
  set(AGENTXX_VERSION "0.1.0")
endif ()

project(cxx_utilxx VERSION ${AGENTXX_VERSION} LANGUAGES C CXX)

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
option(CXX_UTILXX_LINUX_IO_URING_SUPPORTED "Linux io_uring supported (asio file async IO)." OFF)

if (NOT DEFINED AGENTXX_EXEC_INSTALL_PREFIX)
  set(AGENTXX_EXEC_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}")
endif ()

find_package(cxx_utilxx_base REQUIRED)
find_package(Boost CONFIG REQUIRED)
if (CXX_UTILXX_ENABLE_BOOST_PROCESS)
  find_package(Boost CONFIG REQUIRED COMPONENTS process)
endif ()
find_package(OpenSSL REQUIRED)
find_package(SQLite3 CONFIG REQUIRED)
find_package(fmt REQUIRED)
find_package(html2md REQUIRED)

set(_CXX_UTILXX_SOURCES
  src/crypto.cpp
  src/http_client.cpp
  src/http_header.cpp
  src/http_server.cpp
  src/regex.cpp
  src/settings_db.cpp
  src/sqlite.cpp
  src/ws_client.cpp
)

# ===== 可选依赖: hyperscan / io_uring =====
# 解析为库文件绝对路径后写入 INTERFACE: 处理方无需自行 pkg_check_modules
# (与旧 agentxx_util 的 PkgConfig::hyperscan 传递方式不同, 更易被第三方复用)
set(_CXX_UTILXX_EXTRA_LIBS "")
set(_CXX_UTILXX_EXTRA_DEFS "")
if (CXX_UTILXX_ENABLE_HYPERSCAN)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(hyperscan REQUIRED IMPORTED_TARGET libhs)
  list(APPEND _CXX_UTILXX_EXTRA_LIBS ${hyperscan_LINK_LIBRARIES})
  set(_CXX_UTILXX_EXTRA_INCLUDES ${hyperscan_INCLUDE_DIRS})
  find_library(CXX_UTILXX_HS_RUNTIME_LIB NAMES hs_runtime
    HINTS "${AGENTXX_INSTALL_DIR}/lib" "${hyperscan_LIBRARY_DIRS}")
  if (CXX_UTILXX_HS_RUNTIME_LIB)
    list(APPEND _CXX_UTILXX_EXTRA_LIBS "${CXX_UTILXX_HS_RUNTIME_LIB}")
  else ()
    list(APPEND _CXX_UTILXX_EXTRA_LIBS hs_runtime)
  endif ()
  list(APPEND _CXX_UTILXX_EXTRA_DEFS UTILXX_ENABLE_HYPERSCAN=1)
endif ()
if (CXX_UTILXX_LINUX_IO_URING_SUPPORTED)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(uring REQUIRED IMPORTED_TARGET liburing)
  list(APPEND _CXX_UTILXX_EXTRA_LIBS ${uring_LINK_LIBRARIES})
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
  SQLite::SQLite3
  html2md
)
if (CXX_UTILXX_ENABLE_BOOST_PROCESS)
  list(APPEND _CXX_UTILXX_LINK_LIBS Boost::process)
endif ()
list(APPEND _CXX_UTILXX_LINK_LIBS ${_CXX_UTILXX_EXTRA_LIBS})

foreach (_t ${_CXX_UTILXX_TARGETS})
  target_compile_definitions(${_t} PUBLIC
    ${XX_PLATFORM_COMPILE_DEFINITIONS}
    XX_VERSION_STRING="${AGENTXX_VERSION}"
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
    "${AGENTXX_INSTALL_DIR}/include"
    ${_CXX_UTILXX_EXTRA_INCLUDES}
  )
  target_link_directories(${_t} PUBLIC "${AGENTXX_INSTALL_DIR}/lib/")
  target_link_libraries(${_t} PUBLIC ${_CXX_UTILXX_LINK_LIBS})
endforeach ()
unset(_t)

cxx_lib_set_own_dir_rpath(cxx_utilxx_shared)

# ===== 安装与导出 =====
install(TARGETS cxx_utilxx_shared cxx_utilxx_static
  EXPORT cxx_utilxxTargets
  LIBRARY DESTINATION "${AGENTXX_EXEC_INSTALL_PREFIX}"
  RUNTIME DESTINATION "${AGENTXX_EXEC_INSTALL_PREFIX}"
  ARCHIVE DESTINATION "${AGENTXX_EXEC_INSTALL_PREFIX}"
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
