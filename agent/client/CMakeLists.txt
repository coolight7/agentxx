cmake_minimum_required(VERSION 3.10)
# find_package 使用 <PackageName>_ROOT 变量
cmake_policy(SET CMP0074 NEW)
# MSVC 运行时库标志由抽象层选择
cmake_policy(SET CMP0091 NEW)
# find_package 使用大写的 _ROOT 变量
cmake_policy(SET CMP0144 NEW)
cmake_policy(SET CMP0167 OLD)

project(agentxx VERSION ${AGENTXX_VERSION} LANGUAGES C CXX)
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
# 关闭 C++20 模块扫描 (不用 import std / 模块单元, 见 lib/CMakeLists.txt 同名段)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if(XX_IS_MSVC_D)
  list(PREPEND CMAKE_FIND_LIBRARY_SUFFIXES .lib .a)
else()
  list(PREPEND CMAKE_FIND_LIBRARY_SUFFIXES .a)
endif()

add_executable(agentxx)

file(GLOB_RECURSE client_SOURCES 
	CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.cc" "${CMAKE_CURRENT_SOURCE_DIR}/*.cxx"
)
# 源文件在文件末尾统一拆成多个 OBJECT 目标并行编译 (见 cmake/agentxx_split_sources.cmake)
target_include_directories(agentxx PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include/")
# neograph/json.h 内部 #include <yyjson.h> (构建期头, 见 lib/CMakeLists 说明):
# client 经 find_package(agentxx_static) 只得 install/include, 需补 neograph 源码 deps 路径
if (DEFINED neograph_SOURCE_DIR)
  target_include_directories(agentxx PRIVATE "${neograph_SOURCE_DIR}/deps/yyjson/")
endif()

# 注意: 不要添加 /FS /MP 编译选项 —— 顶层已统一管理; 命令行出现重复 /FS 时
# VS18 FileTracker 失效，每次构建都会全部重编 (勿删此注释)

# PCH: 只预编译稳定第三方头，加速编译 (与 lib 保持一致)。
# 第三方头须用 <...> 形式 (见 lib/CMakeLists.txt 说明)
# 注: 真正编译源文件的是文件末尾拆出来的 OBJECT 目标, PCH 统一挂到它们上

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
# 插件界面描述层 (TUI 渲染层使用其模型 + 降级适配)
find_package(cxx_pluginxx_ui REQUIRED)
find_package(agentxx_static REQUIRED)

target_link_directories(agentxx PUBLIC
  "${AGENTXX_INSTALL_DIR}/lib/"
)

target_link_libraries(agentxx PUBLIC
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
  # 界面描述层静态变体: TUI 渲染层直用其解析/适配/文本降级
  cxx_pluginxx_ui_static
  ftxui::ftxui
  markdown::markdown-ui
)
# 条件依赖显式链接 (目标已在依赖查找段按开关查出; hs_runtime 为 HyperScan
# 运行期库, 按裸库名经 cxx_utilxx 的 link 目录解析)
if (AGENTXX_ENABLE_HYPERSCAN)
  target_link_libraries(agentxx PRIVATE
    PkgConfig::hyperscan
    hs_runtime
  )
endif()
if (AGENTXX_LINUX_IO_URING_SUPPORTED)
  target_link_libraries(agentxx PRIVATE
    PkgConfig::uring
  )
endif()

if (XX_IS_MSVC_D)
	target_compile_options(agentxx PRIVATE 
		"/bigobj"
	)
	target_compile_definitions(agentxx PRIVATE
		strdup=_strdup
		strcasecmp=_stricmp
		strncasecmp=_strnicmp
	)
endif ()
# ===== 运行期动态库搜索路径: 优先从可执行文件所在目录搜索 =====
# release 分发会把 libstdc++.so.6 / libgcc_s.so.1 等运行库复制到 exec (与
# agentxx 同目录), 不带该搜索项时加载器不会看自身目录, 目标机器上的旧版
# 系统 libstdc++ 会被优先加载 (详见 cmake/agentxx_runtime_search_path.cmake)
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/agentxx_runtime_search_path.cmake")
agentxx_set_own_dir_rpath(agentxx)

# ===== 内存分配器: mimalloc (见 cmake/agentxx_mimalloc.cmake) =====
# 静态链接 (默认) 时分配器并入 agentxx; 动态链接时额外把分配器动态库
# 装到 exec 目录 (与产物同目录, 随产物分发)
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/agentxx_mimalloc.cmake")
agentxx_mimalloc_attach(agentxx)

# ===== 源文件拆成多个 OBJECT 目标并行编译 =====
# - VS 生成器只在工程之间并行, 单个 .vcxproj 内的编译是串行的;
#   GCC/Clang 的 -j 本身就按文件并行, 不拆 (见 cmake/agentxx_split_sources.cmake)
# - 编译/链接要求用生成器表达式从 agentxx 上拷, 所以本段放在文件末尾
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/agentxx_split_sources.cmake")
agentxx_split_object_sources(
  agentxx_client 8 _agentxx_client_obj_targets _agentxx_client_obj_files ${client_SOURCES})
target_sources(agentxx PRIVATE ${_agentxx_client_obj_files})
agentxx_inherit_usage_requirements(agentxx ${_agentxx_client_obj_targets})
if (AGENTXX_ENABLE_PCH)
  agentxx_pch_headers(_agentxx_client_pch_headers)
  agentxx_apply_pch("${_agentxx_client_pch_headers}" ${_agentxx_client_obj_targets})
endif()

install(TARGETS agentxx DESTINATION "${AGENTXX_EXEC_INSTALL_PREFIX}")