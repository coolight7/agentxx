cmake_minimum_required(VERSION 3.10)
# find_package 使用 <PackageName>_ROOT 变量
cmake_policy(SET CMP0074 NEW)
# MSVC 运行时库标志由抽象层选择
cmake_policy(SET CMP0091 NEW)
# find_package 使用大写的 _ROOT 变量
cmake_policy(SET CMP0144 NEW)
cmake_policy(SET CMP0167 OLD)

project(agentxx_cli VERSION ${AGENTXX_VERSION} LANGUAGES C CXX)
include(GNUInstallDirs)

# 全局版本宏注入
add_compile_definitions(
  AGENTXX_VERSION_STRING="${AGENTXX_VERSION}"
  AGENTXX_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
  AGENTXX_VERSION_MINOR=${PROJECT_VERSION_MINOR}
  AGENTXX_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

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
# neograph/json.h 内部 #include <yyjson.h> (构建期头, 见 lib/CMakeLists 说明):
# client 经 find_package(agentxx_static) 只得 install/include, 需补 neograph 源码 deps 路径
if (DEFINED neograph_SOURCE_DIR)
  target_include_directories(agentxx_cli PRIVATE "${neograph_SOURCE_DIR}/deps/yyjson/")
endif()

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

# ===== 依赖查找 (顺序: 先依赖库的依赖, 再依赖库本体) =====
# 1) 工具库的条件依赖 (io_uring / HyperScan): 工具库导出接口只声明目标名,
#    故先在本机查找出这些目标, 再 find_package 工具库 / agentxx_static
#    (开关与工具库构建时同源, 详见 lib/CMakeLists.txt 同名段落)
find_package(PkgConfig REQUIRED)
if (AGENTXX_LINUX_IO_URING_SUPPORTED)
  # io_uring: cxx_utilxx_base 的 asio 文件异步 I/O 探测引用其符号
  pkg_check_modules(uring REQUIRED IMPORTED_TARGET liburing)
endif ()
if (AGENTXX_ENABLE_HYPERSCAN)
  # HyperScan: cxx_utilxx 的正则实现引用其符号 (hs_runtime 按裸库名随其接口解析)
  pkg_check_modules(hyperscan REQUIRED IMPORTED_TARGET libhs)
endif ()

# 2) 第三方库: 一律用本项目自编译/自带的库 (安装于 AGENTXX_INSTALL_DIR 或
#    BOOST_ROOT, 经 CMAKE_PREFIX_PATH 命中): iconv/ZLIB/SQLite3/CURL/Boost 必须
#    CONFIG 模式 (避免内置 Find* 模块 fallback 到系统库); OpenSSL 为预构建目录
#    (无 config) 故用模块模式, 经 OPENSSL_ROOT_DIR 精确指向
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
# client (TUI/CLI) 自身界面库
find_package(ftxui REQUIRED)
find_package(markdown-ui REQUIRED)

# 3) 依赖库本体: 工具库 (config 内含 find_dependency 链) 与 libagentxx
find_package(cxx_utilxx_base REQUIRED)
find_package(cxx_utilxx REQUIRED)
find_package(cxx_pluginxx REQUIRED)
find_package(agentxx_static REQUIRED)

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
# 条件依赖显式链接 (目标已在依赖查找段按开关查出; hs_runtime 为 HyperScan
# 运行期库, 按裸库名经 cxx_utilxx 的 link 目录解析)
if (AGENTXX_ENABLE_HYPERSCAN)
  target_link_libraries(agentxx_cli PRIVATE
    PkgConfig::hyperscan
    hs_runtime
  )
endif()
if (AGENTXX_LINUX_IO_URING_SUPPORTED)
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
# ===== 运行期动态库搜索路径: 优先从可执行文件所在目录搜索 =====
# release 分发会把 libstdc++.so.6 / libgcc_s.so.1 等运行库复制到 exec (与
# agentxx_cli 同目录), 不带该搜索项时加载器不会看自身目录, 目标机器上的旧版
# 系统 libstdc++ 会被优先加载 (详见 cmake/agentxx_runtime_search_path.cmake)
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/agentxx_runtime_search_path.cmake")
agentxx_set_own_dir_rpath(agentxx_cli)

# ===== 内存分配器: mimalloc (见 cmake/agentxx_mimalloc.cmake) =====
# 静态链接 (默认) 时分配器并入 agentxx_cli; 动态链接时额外把分配器动态库
# 装到 exec 目录 (与产物同目录, 随产物分发)
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/agentxx_mimalloc.cmake")
agentxx_mimalloc_attach(agentxx_cli)

install(TARGETS agentxx_cli DESTINATION "${AGENTXX_EXEC_INSTALL_PREFIX}")