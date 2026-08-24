cmake_minimum_required(VERSION 3.10)
# find_package 使用 <PackageName>_ROOT 变量
cmake_policy(SET CMP0074 NEW)
# MSVC 运行时库标志由抽象层选择
cmake_policy(SET CMP0091 NEW)
# find_package 使用大写的 _ROOT 变量
cmake_policy(SET CMP0144 NEW)
# 移除 cmake 内置的 FindBoost 模块
cmake_policy(SET CMP0167 OLD)

project(agentxx_test VERSION 0.1.0 LANGUAGES C CXX)
include(GNUInstallDirs)

# 由顶层 superbuild 传入 (见 agent/CMakeLists.txt)
option(AGENTXX_ENABLE_PCH "Enable precompiled headers (GCC/Clang)." ON)
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

add_executable(agentxx_test)

# 注: 示例插件 (example_plugin/javascript_engine/example_js) 已独立到 agent/plugins/
# 独立编译 (每个插件子目录自带 CMakeLists.txt), 见 plugins/CMakeLists.txt;
# 插件动态库输出到 ${AGENTXX_EXEC_INSTALL_PREFIX} (与测试可执行同目录),
# 供 test_plugins 加载, 由顶层 agentxx_plugins_repo ExternalProject 构建。

# 测试源按目录划分:
# - 根目录: 入口 test.cpp (模块注册)
# - core/:   agentxx (lib) 核心测试
# - client/: client (TUI/CLI) 测试, 仅 AGENTXX_BUILD_CLIENT 时编译其中的
#            client/src 实现源码 (见下方 AGENTXX_BUILD_CLIENT 分支)
file(GLOB test_root_SOURCES 
	CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.cc" "${CMAKE_CURRENT_SOURCE_DIR}/*.cxx"
)
file(GLOB test_core_SOURCES 
	CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/core/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/core/*.cc" "${CMAKE_CURRENT_SOURCE_DIR}/core/*.cxx"
)
target_sources(agentxx_test PRIVATE
	${test_root_SOURCES}
	${test_core_SOURCES}
)
# include path: 根目录 (test_framework.h 等共享头) + core/ + client/
# 使 test.cpp 的 #include "test_xxx.h" 与子目录测试的
# #include "test_framework.h" 均无需改动路径
target_include_directories(agentxx_test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/core/"
  "${CMAKE_CURRENT_SOURCE_DIR}/client/"
  # 迁移至插件的内置工具实现 (filesystem/string/system/websearch/rag_search/
  # execute_command 的 *_impl.h): 测试直测插件同一实现, 保证行为一致
  "${CMAKE_CURRENT_SOURCE_DIR}/../plugins/agentxx_planning/"
  "${CMAKE_CURRENT_SOURCE_DIR}/../plugins/agentxx_filesystem/"
  "${CMAKE_CURRENT_SOURCE_DIR}/../plugins/agentxx_execute_command/"
  "${CMAKE_CURRENT_SOURCE_DIR}/../plugins/agentxx_string/"
  "${CMAKE_CURRENT_SOURCE_DIR}/../plugins/agentxx_system/"
  "${CMAKE_CURRENT_SOURCE_DIR}/../plugins/agentxx_websearch/"
  "${CMAKE_CURRENT_SOURCE_DIR}/../plugins/agentxx_rag_search/"
)

# 注意: 不要添加 /FS /MP 编译选项 —— 顶层已统一管理; 命令行出现重复 /FS 时
# VS18 FileTracker 失效，每次构建都会全量重编 (勿删此注释)

# 内置合并编译模式 (顶层 AGENTXX_ENABLE_PLUGIN_BUILTIN=ON, 由 superbuild 传入):
# - 测试经插件目录路径加载内置插件 (入口文件缺失时 PluginManager 回退内置注册表),
#   与动态模式配置完全一致
# - 暴露编译期宏供测试区分断言 (如 dlHandle 为空/非空)
if (AGENTXX_ENABLE_PLUGIN_BUILTIN)
  target_compile_definitions(agentxx_test PRIVATE AGENTXX_ENABLE_PLUGIN_BUILTIN=1)
endif()

if (AGENTXX_BUILD_CLIENT)
  # build client test
  file(GLOB test_client_SOURCES 
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/client/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/client/*.cc" "${CMAKE_CURRENT_SOURCE_DIR}/client/*.cxx"
  )
  set(AGENTXX_CLIENT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../client/")
  file(GLOB_RECURSE client_SOURCES 
    CONFIGURE_DEPENDS
    "${AGENTXX_CLIENT_SOURCE_DIR}/src/*.cpp" "${AGENTXX_CLIENT_SOURCE_DIR}/src/*.cc" "${AGENTXX_CLIENT_SOURCE_DIR}/src/*.cxx"
  )
  target_sources(agentxx_test PRIVATE
    ${test_client_SOURCES}
    ${client_SOURCES}
  )
  target_include_directories(agentxx_test PRIVATE 
    "${AGENTXX_CLIENT_SOURCE_DIR}/include/"
  )
endif()

# PCH: 只预编译稳定第三方头，加速测试编译 (GCC/Clang，与 lib 保持一致)。
# 第三方头须用 <...> 形式 (见 lib/CMakeLists.txt 说明)
if (AGENTXX_ENABLE_PCH AND NOT XX_IS_MSVC_D)
  target_precompile_headers(agentxx_test PRIVATE
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
    target_compile_options(agentxx_test PRIVATE -fpch-preprocess)
  endif()
endif()

find_package(PkgConfig REQUIRED)

# iconv 必须 CONFIG 模式: 仅使用本项目 libiconv-native 安装的
# iconvConfig.cmake (提供 Iconv::iconv/Iconv::charset, agentxx_shared 的
# INTERFACE 链接依赖), 避免 CMake 内置 Findiconv.cmake 模块模式命中系统 iconv
find_package(iconv CONFIG REQUIRED)
find_package(uchardet REQUIRED)
find_package(fmt REQUIRED)
find_package(simdjson REQUIRED)
# ZLIB/SQLite3/CURL/Boost 同样必须 CONFIG 模式: 避免 CMake 内置
# FindZLIB/FindSQLite3/FindCURL/FindBoost 模块 fallback 到系统库
# (zlib-ng/sqlite3-cmake/curl/boost 均安装了自己的 config 文件,
# 位于 AGENTXX_INSTALL_DIR 或 BOOST_ROOT, 经 CMAKE_PREFIX_PATH 命中)
find_package(ZLIB CONFIG REQUIRED)
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
if (AGENTXX_BUILD_CLIENT)
  find_package(ftxui REQUIRED)
  find_package(markdown-ui REQUIRED)
endif()

find_package(agentxx_static REQUIRED)

target_link_directories(agentxx_test PUBLIC
  "${AGENTXX_INSTALL_DIR}/lib/"
)

target_link_libraries(agentxx_test PRIVATE
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
)
if (AGENTXX_ENABLE_HYPERSCAN)
  pkg_check_modules(hyperscan REQUIRED IMPORTED_TARGET libhs)
  target_link_libraries(agentxx_test PRIVATE 
    PkgConfig::hyperscan
    hs_runtime
  )
endif()
if (AGENTXX_LINUX_IO_URING_SUPPORTED)
  pkg_check_modules(uring REQUIRED IMPORTED_TARGET liburing)
  target_link_libraries(agentxx_test PRIVATE 
    PkgConfig::uring
  )
endif()
if (AGENTXX_BUILD_CLIENT)
  target_link_libraries(agentxx_test PRIVATE ftxui::ftxui markdown::markdown-ui)
  # 供 test.cpp 条件注册依赖 client 的测试模块
  target_compile_definitions(agentxx_test PRIVATE AGENTXX_BUILD_CLIENT=1)
endif()

if (XX_IS_MSVC_D)
	target_compile_options(agentxx_test PRIVATE 
		"/bigobj"
	)
	target_compile_definitions(agentxx_test PRIVATE
		strdup=_strdup
		strcasecmp=_stricmp
		strncasecmp=_strnicmp
	)
endif ()
if (XX_IS_MACOS_D)
  target_link_options(agentxx_test PRIVATE 
    "-Wl,-rpath,@executable_path"
  )
endif()

install(TARGETS agentxx_test DESTINATION "${AGENTXX_EXEC_INSTALL_PREFIX}")