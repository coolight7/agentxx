cmake_minimum_required(VERSION 3.10)
# find_package 使用 <PackageName>_ROOT 变量
cmake_policy(SET CMP0074 NEW)
# MSVC 运行时库标志由抽象层选择
cmake_policy(SET CMP0091 NEW)
# find_package 使用大写的 _ROOT 变量
cmake_policy(SET CMP0144 NEW)
# 移除 cmake 内置的 FindBoost 模块
cmake_policy(SET CMP0167 OLD)

project(agentxx_plugins VERSION 0.1.0 LANGUAGES C CXX)

# 由顶层 superbuild 传入 (见 agent/CMakeLists.txt)，独立构建本目录时取默认值
option(AGENTXX_ENABLE_PLUGIN_EXAMPLE "Build example plugin." ON)
option(AGENTXX_ENABLE_PLUGIN_JS "Build QuickJS JS plugin engine (agentxx_javascript_engine)." ON)

# 嵌套构建时由顶层传入 (见 agent/CMakeLists.txt)；独立构建时取默认值:
# - AGENTXX_INSTALL_DIR:      依赖库 (如 quickjs 的 libqjs.a / quickjs.h) 的 install 目录
# - AGENTXX_EXEC_INSTALL_PREFIX: 插件动态库输出目录 (与 agentxx_cli/agentxx_test 同目录,
#   便于运行期加载)
if (NOT DEFINED AGENTXX_INSTALL_DIR)
  set(AGENTXX_INSTALL_DIR "${CMAKE_BINARY_DIR}/agentxx-project-install/")
endif()
if (NOT DEFINED AGENTXX_EXEC_INSTALL_PREFIX)
  set(AGENTXX_EXEC_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/exec")
endif()
if (NOT DEFINED XX_IS_MSVC_D)
  if (MSVC)
    set(XX_IS_MSVC_D 1)
  else ()
    set(XX_IS_MSVC_D 0)
  endif()
endif()

# 插件仅依赖纯 C ABI 头 (见 plugin_api.h)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)

if(XX_IS_MSVC_D)
  list(PREPEND CMAKE_FIND_LIBRARY_SUFFIXES .lib .a)
else()
  list(PREPEND CMAKE_FIND_LIBRARY_SUFFIXES .a)
endif()

# libagentxx 头文件搜索路径 (插件不链接 libagentxx, 仅依赖纯 C ABI 头)
set(AGENTXX_LIB_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../lib/include")
if (NOT EXISTS "${AGENTXX_LIB_INCLUDE_DIR}")
  message(FATAL_ERROR "[agentxx_plugins] libagentxx include dir not found: ${AGENTXX_LIB_INCLUDE_DIR}")
endif()

# ===== 各插件子目录 =====
# - 每个插件子目录自带 CMakeLists.txt, 可独立编译
# - 仅 AGENTXX_ENABLE_PLUGIN_JS 时编译 JS 相关插件 (依赖 quickjs 库)
if (AGENTXX_ENABLE_PLUGIN_EXAMPLE)
  add_subdirectory(example_native)
endif()

if (AGENTXX_ENABLE_PLUGIN_JS)
  add_subdirectory(javascript_engine)
  if (AGENTXX_ENABLE_PLUGIN_EXAMPLE)
    add_subdirectory(example_js)
  endif()
endif()
