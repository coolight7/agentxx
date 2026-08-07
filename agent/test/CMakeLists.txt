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

# 建议使用 c++ 26 
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)

if(XX_IS_MSVC_D)
  list(PREPEND CMAKE_FIND_LIBRARY_SUFFIXES .lib .a)
else()
  list(PREPEND CMAKE_FIND_LIBRARY_SUFFIXES .a)
endif()

add_executable(agentxx_test)

# 测试源按目录划分:
# - 根目录: 入口 test.cpp (模块注册)
# - core/:   agentxx (lib) 核心测试
# - client/: client (TUI/CLI) 测试, 仅 AGENTXX_BUILD_CLIENT 时编译其中的
#            client/src 实现源码 (见下方 AGENTXX_BUILD_CLIENT 分支)
file(GLOB test_SOURCES 
	CONFIGURE_DEPENDS
	"*.cpp" "*.cc" "*.cxx"
	"core/*.cpp" "core/*.cc" "core/*.cxx"
	"client/*.cpp" "client/*.cc" "client/*.cxx"
)
target_sources(agentxx_test PRIVATE
	${test_SOURCES}
)
# include path: 根目录 (test_framework.h 等共享头) + core/ + client/
# 使 test.cpp 的 #include "test_xxx.h" 与子目录测试的
# #include "test_framework.h" 均无需改动路径
target_include_directories(agentxx_test PRIVATE 
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/core/"
  "${CMAKE_CURRENT_SOURCE_DIR}/client/"
)

if (AGENTXX_BUILD_CLIENT)
  # build client test
  set(AGENTXX_CLIENT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../client/")
  file(GLOB_RECURSE client_SOURCES 
    CONFIGURE_DEPENDS
    "${AGENTXX_CLIENT_SOURCE_DIR}/src/*.cpp" "${AGENTXX_CLIENT_SOURCE_DIR}/src/*.cc" "${AGENTXX_CLIENT_SOURCE_DIR}/src/*.cxx"
  )
  target_sources(agentxx_test PRIVATE
    ${client_SOURCES}
  )
  target_include_directories(agentxx_test PRIVATE 
    "${AGENTXX_CLIENT_SOURCE_DIR}/include/"
  )
endif()

find_package(PkgConfig REQUIRED)

find_package(iconv REQUIRED)
find_package(uchardet REQUIRED)
find_package(fmt REQUIRED)
find_package(simdjson REQUIRED)
find_package(ZLIB REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Boost REQUIRED)
if (AGENTXX_ENABLE_BOOST_PROCESS)
  find_package(Boost REQUIRED COMPONENTS process)
endif()
find_package(yaml-cpp REQUIRED)
find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)
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
if (AGENTXX_ENABLE_HYPERSCAN OR AGENTXX_ENABLE_VECTORSCAN)
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