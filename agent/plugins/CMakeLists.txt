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
option(AGENTXX_ENABLE_PLUGIN_CODEGRAPH "Build agentxx_codegraph plugin (requires codegraph libs)." ON)
option(AGENTXX_ENABLE_PLUGIN_COMPUTER_USE "Build agentxx_computer_use plugin (ui_control)." ON)
option(AGENTXX_ENABLE_PLUGIN_SCREEN_CAPTURE "Build agentxx_screen_capture plugin (screen capture)." ON)
option(AGENTXX_ENABLE_PLUGIN_SYSTEM_MONITOR "Build agentxx_system_monitor plugin (CPU/GPU usage)." ON)
option(AGENTXX_ENABLE_PLUGIN_AUDIO_STREAM "Build agentxx_audio_stream plugin (audio capture)." ON)
option(AGENTXX_ENABLE_PLUGIN_TEXT_SELECTION "Build agentxx_text_selection_monitor plugin (text selection)." ON)

# 嵌套构建时由顶层传入 (见 agent/CMakeLists.txt)；独立构建时取默认值:
# - AGENTXX_INSTALL_DIR:      依赖库 (如 quickjs 的 libqjs.a / quickjs.h) 的 install 目录
# - AGENTXX_EXEC_INSTALL_PREFIX: 插件动态库输出目录 (与 agentxx_cli/agentxx_test 同目录,
#   便于运行期加载)
if (NOT DEFINED AGENTXX_INSTALL_DIR)
  set(AGENTXX_INSTALL_DIR "${CMAKE_BINARY_DIR}/agentxx-project-install/")
endif()
# 依赖库查找路径 (独立构建本目录时生效; 嵌套构建时顶层已传 CMAKE_PREFIX_PATH)
list(PREPEND CMAKE_PREFIX_PATH "${AGENTXX_INSTALL_DIR}")
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

# Windows/MSVC: MODULE 动态库默认不导出任何符号, 插件入口 (agentxx_plugin_entry /
# agentxx_plugin_get_info / agentxx_client_entry 等) 与跨翻译单元符号
# (如 agentxx_computer_use 的 uiControlExecute) 将无法被宿主 dlsym/GetProcAddress
# 找到。开启自动导出 (CMake 生成 .def 导出全部全局符号; 插件为独立进程内加载,
# 导出面不影响行为)
if (WIN32)
  set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

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
  add_subdirectory(example_plugin)
endif()

if (AGENTXX_ENABLE_PLUGIN_JS)
  add_subdirectory(agentxx_javascript_engine)
  if (AGENTXX_ENABLE_PLUGIN_EXAMPLE)
    add_subdirectory(example_js)
  endif()
endif()

# agentxx_codegraph (需 codegraph-cpp 库; 顶层 AGENTXX_ENABLE_PLUGIN_CODEGRAPH 时构建)
if (AGENTXX_ENABLE_PLUGIN_CODEGRAPH)
  add_subdirectory(agentxx_codegraph)
endif()

# agentxx_screen_capture (屏幕捕获; 独立插件, agentxx_computer_use depends 依赖它)
if (AGENTXX_ENABLE_PLUGIN_SCREEN_CAPTURE)
  add_subdirectory(agentxx_screen_capture)
endif()

# agentxx_computer_use (ui_control; 运行时依赖 agentxx_screen_capture, 配置时须同时加载)
if (AGENTXX_ENABLE_PLUGIN_COMPUTER_USE)
  add_subdirectory(agentxx_computer_use)
endif()

# agentxx_system_monitor (系统资源监控: CPU/内存/GPU; 从 lib expand 拆分)
if (AGENTXX_ENABLE_PLUGIN_SYSTEM_MONITOR)
  add_subdirectory(agentxx_system_monitor)
endif()

# agentxx_audio_stream (音频流捕获: 系统输出/程序输出/麦克风; 从 lib expand 拆分)
if (AGENTXX_ENABLE_PLUGIN_AUDIO_STREAM)
  add_subdirectory(agentxx_audio_stream)
endif()

# agentxx_text_selection_monitor (系统级文本选择事件流; 从 lib expand 拆分)
if (AGENTXX_ENABLE_PLUGIN_TEXT_SELECTION)
  add_subdirectory(agentxx_text_selection_monitor)
endif()
