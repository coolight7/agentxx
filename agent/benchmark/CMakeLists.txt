cmake_minimum_required(VERSION 3.10)
cmake_policy(SET CMP0074 NEW)
cmake_policy(SET CMP0091 NEW)
cmake_policy(SET CMP0144 NEW)
cmake_policy(SET CMP0167 OLD)

project(agentxx_benchmark VERSION 0.1.0 LANGUAGES C CXX)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)

if(XX_IS_MSVC_D)
  list(PREPEND CMAKE_FIND_LIBRARY_SUFFIXES .lib .a)
else()
  list(PREPEND CMAKE_FIND_LIBRARY_SUFFIXES .a)
endif()

add_executable(agentxx_benchmark)

file(GLOB bench_SOURCES
  CONFIGURE_DEPENDS
  "*.cpp" "*.cc" "*.cxx"
)
target_sources(agentxx_benchmark PRIVATE
  ${bench_SOURCES}
)

set(AGENTXX_BENCH_OUTPUT_DIR "${AGENTXX_EXEC_INSTALL_PREFIX}/bench")
target_compile_definitions(agentxx_benchmark PRIVATE
  AGENTXX_BENCH_OUTPUT_DIR="${AGENTXX_BENCH_OUTPUT_DIR}"
)

target_include_directories(agentxx_benchmark PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

find_package(PkgConfig REQUIRED)

# iconv 必须 CONFIG 模式: 仅使用本项目 libiconv-native 安装的
# iconvConfig.cmake (提供 Iconv::iconv/Iconv::charset, agentxx_shared 的
# INTERFACE 链接依赖), 避免 CMake 内置 Findiconv.cmake 模块模式命中系统 iconv
find_package(iconv CONFIG REQUIRED)
find_package(uchardet REQUIRED)
find_package(fmt REQUIRED)
find_package(simdjson REQUIRED)
# SQLite3/CURL/Boost 同样必须 CONFIG 模式: 避免 CMake 内置
# FindSQLite3/FindCURL/FindBoost 模块 fallback 到系统库
# (sqlite3-cmake/curl/boost 均安装了自己的 config 文件,
# 位于 AGENTXX_INSTALL_DIR 或 BOOST_ROOT, 经 CMAKE_PREFIX_PATH 命中)
# OpenSSL 保持模块模式 (FindOpenSSL): 本项目 OpenSSL 为预构建目录
# (third_party/OpenSSL-windows-build), 无 OpenSSLConfig.cmake, 模块模式
# 经 OPENSSL_ROOT_DIR 精确指向项目预构建目录, 不会命中系统 OpenSSL
find_package(OpenSSL REQUIRED)
find_package(Boost CONFIG REQUIRED)
if (AGENTXX_ENABLE_BOOST_PROCESS)
  find_package(Boost CONFIG REQUIRED COMPONENTS process)
endif()
find_package(yaml-cpp REQUIRED)
find_package(CURL CONFIG REQUIRED)
find_package(SQLite3 CONFIG REQUIRED)
find_package(html2md REQUIRED)
find_package(Glob REQUIRED)

find_package(agentxx_static REQUIRED)

target_link_directories(agentxx_benchmark PUBLIC
  "${AGENTXX_INSTALL_DIR}/lib/"
)

target_link_libraries(agentxx_benchmark PRIVATE
  neograph_sqlite
  neograph_acp
  neograph_mcp
  neograph_async
  neograph_llm
  neograph_core
  yyjson
  agentxx_static
)
if (AGENTXX_ENABLE_HYPERSCAN)
  pkg_check_modules(hyperscan REQUIRED IMPORTED_TARGET libhs)
  target_link_libraries(agentxx_benchmark PRIVATE
    PkgConfig::hyperscan
    hs_runtime
  )
endif()
if (AGENTXX_LINUX_IO_URING_SUPPORTED)
  pkg_check_modules(uring REQUIRED IMPORTED_TARGET liburing)
  target_link_libraries(agentxx_benchmark PRIVATE
    PkgConfig::uring
  )
endif()

if (XX_IS_MSVC_D)
  target_compile_options(agentxx_benchmark PRIVATE
    "/bigobj"
  )
  target_compile_definitions(agentxx_benchmark PRIVATE
    strdup=_strdup
    strcasecmp=_stricmp
    strncasecmp=_strnicmp
  )
endif()
if (XX_IS_MACOS_D)
  target_link_options(agentxx_benchmark PRIVATE
    "-Wl,-rpath,@executable_path"
  )
endif()

install(TARGETS agentxx_benchmark DESTINATION "${AGENTXX_EXEC_INSTALL_PREFIX}")