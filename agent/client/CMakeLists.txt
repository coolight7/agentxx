cmake_minimum_required(VERSION 3.10)
# find_package 使用 <PackageName>_ROOT 变量
cmake_policy(SET CMP0074 NEW)
# MSVC 运行时库标志由抽象层选择
cmake_policy(SET CMP0091 NEW)
# find_package 使用大写的 _ROOT 变量
cmake_policy(SET CMP0144 NEW)
cmake_policy(SET CMP0167 OLD)

project(agentxx_cli VERSION 0.1.0 LANGUAGES C CXX)
include(GNUInstallDirs)

# 嵌套构建时由顶层传入；独立构建时自动查找
find_program(CCACHE_PROGRAM ccache)

# 建议使用 c++ 26 
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)

if(XX_IS_MSVC_D)
  list(PREPEND CMAKE_FIND_LIBRARY_SUFFIXES .lib .a)
else()
  list(PREPEND CMAKE_FIND_LIBRARY_SUFFIXES .a)
endif()

add_executable(agentxx_cli)

file(GLOB_RECURSE client_SOURCES 
	CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.cc" "${CMAKE_CURRENT_SOURCE_DIR}/*.cxx"
)
target_sources(agentxx_cli PRIVATE
	${client_SOURCES}
)

target_include_directories(agentxx_cli PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include/")

# 注意: 不要添加 /FS /MP 编译选项 —— 顶层已统一管理; 命令行出现重复 /FS 时
# VS18 FileTracker 失效，每次构建都会全量重编 (勿删此注释)

# PCH: 只预编译稳定第三方头，加速编译 (GCC/Clang，与 lib 保持一致)。
# 第三方头须用 <...> 形式 (见 lib/CMakeLists.txt 说明)
if (AGENTXX_ENABLE_PCH AND NOT XX_IS_MSVC_D)
  target_precompile_headers(agentxx_cli PRIVATE
    <algorithm> <atomic> <chrono> <cstdint> <cstring> <deque> <exception>
    <filesystem> <fstream> <functional> <map> <memory> <mutex> <optional>
    <set> <sstream> <string> <string_view> <thread> <unordered_map>
    <utility> <variant> <vector>
    <fmt/format.h>
    <asio/awaitable.hpp>
    <boost/exception/exception.hpp>
  )
  # ccache + PCH 兼容 (GCC 特有)，仅对本目标生效
  if (CCACHE_PROGRAM AND XX_IS_GCC_D)
    target_compile_options(agentxx_cli PRIVATE -fpch-preprocess)
  endif()
endif()

find_package(PkgConfig REQUIRED)

# 以下依赖一律只使用本项目自编译/自带的库 (均安装于 AGENTXX_INSTALL_DIR
# 或 BOOST_ROOT, 经 CMAKE_PREFIX_PATH 命中):
# - iconv/ZLIB/SQLite3/CURL/Boost 必须显式 CONFIG 模式: 避免 CMake 内置
#   Findiconv/FindZLIB/FindSQLite3/FindCURL/FindBoost 模块 fallback 到系统库
# - OpenSSL 保持模块模式 (FindOpenSSL): 本项目为预构建目录无 config,
#   经 OPENSSL_ROOT_DIR 精确指向项目目录
find_package(iconv CONFIG REQUIRED)
find_package(uchardet REQUIRED)
find_package(fmt REQUIRED)
find_package(simdjson REQUIRED)
find_package(ZLIB CONFIG REQUIRED)
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

find_package(ftxui REQUIRED)
find_package(markdown-ui REQUIRED)

target_link_directories(agentxx_cli PUBLIC
  "${AGENTXX_INSTALL_DIR}/lib/"
)

target_link_libraries(agentxx_cli PUBLIC
  # === neograph vvv 链接顺序是必要的
  neograph_sqlite
  neograph_acp
  neograph_mcp
  neograph_async
  neograph_llm
  neograph_core
  # === neograph ^^^
  yyjson
	agentxx_static
  ftxui::ftxui
  markdown::markdown-ui
)
if (AGENTXX_ENABLE_HYPERSCAN)
  pkg_check_modules(hyperscan REQUIRED IMPORTED_TARGET libhs)
  target_link_libraries(agentxx_cli PRIVATE 
    PkgConfig::hyperscan
    hs_runtime
  )
endif()
if (AGENTXX_LINUX_IO_URING_SUPPORTED)
  pkg_check_modules(uring REQUIRED IMPORTED_TARGET liburing)
  target_link_libraries(agentxx_cli PRIVATE 
    PkgConfig::uring
  )
endif()

if (XX_IS_MSVC_D)
	target_compile_options(agentxx_cli PRIVATE 
		"/bigobj"
	)
	target_compile_definitions(agentxx_cli PRIVATE
		strdup=_strdup
		strcasecmp=_stricmp
		strncasecmp=_strnicmp
	)
endif ()
if (XX_IS_MACOS_D)
  target_link_options(agentxx_cli PRIVATE 
    "-Wl,-rpath,@executable_path"
  )
endif()

install(TARGETS agentxx_cli DESTINATION "${AGENTXX_EXEC_INSTALL_PREFIX}")