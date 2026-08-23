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
# 内置合并编译模式: 启用的插件源文件直接编译进 libagentxx (运行期无需插件
# 动态库, 无 dlopen); 仅经 libagentxx 构建生效 (见下方"内置合并编译模式")
option(AGENTXX_ENABLE_PLUGIN_BUILTIN "Merge enabled plugins into libagentxx (built-in, no runtime plugin dlls)." OFF)

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

# 静态库优先 (与 lib/CMakeLists.txt 一致): pkg_check_modules 的 find_library
# 默认先找系统动态库 (如 liburing.so), 会导致插件动态库产生动态依赖
# (DT_NEEDED); 优先 .a 使 uring/hyperscan 等条件依赖全部静态链接
if (XX_IS_MSVC_D)
  list(PREPEND CMAKE_FIND_LIBRARY_SUFFIXES .lib .a)
else()
  list(PREPEND CMAKE_FIND_LIBRARY_SUFFIXES .a)
endif()

# agentxx_util 重依赖的 pkg-config 模块 (与 client/test 一致):
# - agentxx_util 的 PUBLIC 链接接口带 $<LINK_ONLY:PkgConfig::hyperscan> /
#   $<LINK_ONLY:PkgConfig::uring>, 链接本库的插件须定义同名 imported target
#   (目录 scope 定义, 子插件目录可见; 未链接 agentxx_util 的插件不受影响);
#   hs_runtime 裸库名经 agentxx_util 的 PUBLIC link_directories (install/lib)
#   自动解析, 无需额外配置
# - 未启用对应特性时 (AGENTXX_ENABLE_HYPERSCAN=0 / 无 io_uring) 插件不链接
#   本库的对应条件依赖, 无需定义
find_package(PkgConfig QUIET)
if (AGENTXX_ENABLE_HYPERSCAN)
  pkg_check_modules(hyperscan REQUIRED IMPORTED_TARGET libhs)
endif()
if (AGENTXX_LINUX_IO_URING_SUPPORTED)
  pkg_check_modules(uring REQUIRED IMPORTED_TARGET liburing)
endif()

# ===== 插件动态库导出符号控制 =====
# - 目标: 严格限制导出面, 仅宿主 dlsym/GetProcAddress 按名查找的入口符号
#   (agentxx_plugin_get_info/entry/unload + agentxx_client_get_info/entry/unload,
#   见 plugin_api.h AGENTXX_PLUGIN_EXPORT) 导出, 其余全部隐藏:
#   - 插件自身 C++ 内部符号 (STL/异常/typeinfo 等)
#   - 第三方静态库符号 (simdjson/tree_sitter/io_uring/uchardet 等), 避免
#     污染宿主动态符号表、多插件同名符号冲突、以及宿主符号误绑定到插件副本
# - Linux/Android (ELF): 编译期 -fvisibility=hidden + 链接期 version script
#   (--version-script, global 白名单 + local: *); 两者都需, 前者隐藏插件
#   自身符号, 后者兜底隐藏静态库中按默认可见性编译的符号
# - macOS: -exported_symbols_list (符号带 _ 前缀)
# - Windows/MSVC: 不自动导出 (CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS 默认 OFF),
#   仅 AGENTXX_PLUGIN_EXPORT (__declspec(dllexport)) 标记的入口符号导出;
#   静态库链接进 DLL 的符号默认不导出, 无需额外处理
# - 内置合并编译模式 (AGENTXX_ENABLE_PLUGIN_BUILTIN) 下插件符号直接并入
#   libagentxx, 本分支不生效 (见上)
if (NOT WIN32)
  add_compile_options(-fvisibility=hidden -fvisibility-inlines-hidden)
  if (APPLE)
    # macOS: -exported_symbols_list (符号表带 _ 前缀)
    set(_AGENTXX_EXPORT_SYMS_FILE "${CMAKE_CURRENT_BINARY_DIR}/plugin_exported_symbols.txt")
    file(WRITE "${_AGENTXX_EXPORT_SYMS_FILE}" "\n")
    foreach(_sym agentxx_plugin_get_info agentxx_plugin_entry agentxx_plugin_unload
                 agentxx_client_get_info agentxx_client_entry agentxx_client_unload)
      file(APPEND "${_AGENTXX_EXPORT_SYMS_FILE}" "_${_sym}\n")
    endforeach()
    add_link_options("-Wl,-exported_symbols_list,${_AGENTXX_EXPORT_SYMS_FILE}")
  else()
    # ELF (Linux/Android): version script 白名单, local: * 隐藏其余全部符号
    # - 使用通配符而非精确符号名: 单端插件仅定义 agent 入口 (或 client 入口)
    #   之一, 精确名白名单在 Android lld (-Wl,--fatal-warnings, NDK 默认) 下
    #   会因 "symbol not defined" 报错; GNU ld 静默忽略但 lld 不会。
    #   通配符未匹配时不报错, 且仍保证非 agentxx_plugin_/agentxx_client_
    #   前缀的符号全部隐藏
    set(_AGENTXX_EXPORT_SYMS_FILE "${CMAKE_CURRENT_BINARY_DIR}/plugin_symbols.map")
    file(WRITE "${_AGENTXX_EXPORT_SYMS_FILE}" "# agentxx 插件动态库导出符号白名单
# 宿主仅经 dlsym/GetProcAddress 按名查找以下入口符号 (见 plugin_api.h
# AGENTXX_PLUGIN_EXPORT); local: * 隐藏其余全部符号 (含第三方静态库符号);
# 通配符形式兼容单端插件 (仅定义 agent 或 client 之一) 在 lld 下的链接
{
  global:
    agentxx_plugin_*;
    agentxx_client_*;
  local: *;
};
")
    add_link_options("-Wl,--version-script=${_AGENTXX_EXPORT_SYMS_FILE}")
  endif()
  unset(_AGENTXX_EXPORT_SYMS_FILE)
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

# ===== 内置合并编译模式 (可选) =====
# - AGENTXX_ENABLE_PLUGIN_BUILTIN=ON 时, 由 libagentxx 构建经
#   add_subdirectory 引入本目录 (AGENTXX_IS_LIB_BUILTIN_SUBDIR=1):
#   各插件子目录仅收集源文件/依赖库/入口符号, 不创建动态库目标;
#   生成的清单 (builtin_plugins.cpp) 由 libagentxx 编译, 运行期经
#   agentxx_get_builtin_plugins() 加载 (无 dlopen, 无插件动态库文件)
# - 资源文件 (plugin.yaml, example_js 的 plugin.js) 仍拷贝到
#   ${AGENTXX_EXEC_INSTALL_PREFIX}/plugins/<插件名>/ —— 运行期配置沿用
#   插件目录形态, loadPluginAsync 在入口文件缺失时自动回退内置注册表
#   (depends/级联卸载/拓扑排序全部复用现有机制)
# - 独立构建本目录时不支持内置模式 (无 libagentxx 目标可合并)
if (AGENTXX_ENABLE_PLUGIN_BUILTIN)
  if (NOT AGENTXX_IS_LIB_BUILTIN_SUBDIR)
    message(FATAL_ERROR
      "[agentxx_plugins] AGENTXX_ENABLE_PLUGIN_BUILTIN=ON 须经 libagentxx 构建 "
      "(在顶层 agent/CMakeLists.txt 配置时开启; 本目录独立构建时请关闭该选项)")
  endif()

  # MSVC: 插件 OBJECT 库多目标并行编译时 PDB 写 (MSPDBSRV) 偶发 C1041
  # 竞态; 调试信息直接嵌入 .obj (/Z7, 不生成目标 PDB), 从根上消除
  if (MSVC)
    add_compile_options(/Z7)
  endif()

  # 收集变量 (各插件子目录在 AGENTXX_ENABLE_PLUGIN_BUILTIN 分支内创建
  # OBJECT 库目标并追加, 经 PARENT_SCOPE 传播回本目录作用域; 汇总后
  # 整体传回 libagentxx, 目标文件合并进共享/静态库)
  set(AGENTXX_BUILTIN_PLUGIN_OBJECT_TARGETS "")
  set(AGENTXX_BUILTIN_PLUGIN_LINK_LIBS "")
  set(AGENTXX_BUILTIN_PLUGIN_NAMES "")
  set(AGENTXX_BUILTIN_MANIFEST_DECLS "")
  set(AGENTXX_BUILTIN_MANIFEST_ENTRIES "")
  set(AGENTXX_BUILTIN_RESOURCE_OUTPUTS "")

  # 启用条件与下方"各插件子目录"动态编译分支一致
  if (AGENTXX_ENABLE_PLUGIN_EXAMPLE)
    add_subdirectory(example_plugin)
    list(APPEND AGENTXX_BUILTIN_PLUGIN_NAMES example_plugin)
  endif()

  if (AGENTXX_ENABLE_PLUGIN_JS)
    add_subdirectory(agentxx_javascript_engine)
    list(APPEND AGENTXX_BUILTIN_PLUGIN_NAMES agentxx_javascript_engine)
    if (AGENTXX_ENABLE_PLUGIN_EXAMPLE)
      add_subdirectory(example_js)
      list(APPEND AGENTXX_BUILTIN_PLUGIN_NAMES example_js)
    endif()
  endif()

  if (AGENTXX_ENABLE_PLUGIN_CODEGRAPH)
    add_subdirectory(agentxx_codegraph)
    list(APPEND AGENTXX_BUILTIN_PLUGIN_NAMES agentxx_codegraph)
  endif()

  if (AGENTXX_ENABLE_PLUGIN_SCREEN_CAPTURE)
    add_subdirectory(agentxx_screen_capture)
    list(APPEND AGENTXX_BUILTIN_PLUGIN_NAMES agentxx_screen_capture)
  endif()

  if (AGENTXX_ENABLE_PLUGIN_COMPUTER_USE)
    add_subdirectory(agentxx_computer_use)
    list(APPEND AGENTXX_BUILTIN_PLUGIN_NAMES agentxx_computer_use)
  endif()

  if (AGENTXX_ENABLE_PLUGIN_SYSTEM_MONITOR)
    add_subdirectory(agentxx_system_monitor)
    list(APPEND AGENTXX_BUILTIN_PLUGIN_NAMES agentxx_system_monitor)
  endif()

  if (AGENTXX_ENABLE_PLUGIN_AUDIO_STREAM)
    add_subdirectory(agentxx_audio_stream)
    list(APPEND AGENTXX_BUILTIN_PLUGIN_NAMES agentxx_audio_stream)
  endif()

  if (AGENTXX_ENABLE_PLUGIN_TEXT_SELECTION)
    add_subdirectory(agentxx_text_selection_monitor)
    list(APPEND AGENTXX_BUILTIN_PLUGIN_NAMES agentxx_text_selection_monitor)
  endif()

  # 生成内置插件清单 (编译进 libagentxx; 引用各插件改名后的入口符号)
  list(LENGTH AGENTXX_BUILTIN_PLUGIN_NAMES AGENTXX_BUILTIN_PLUGIN_COUNT)
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/builtin_plugins.cpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/builtin_plugins.cpp"
    @ONLY
  )

  # 资源拷贝 (运行期目录配置需 plugin.yaml; example_js 另需 plugin.js)
  # - 与动态编译分支的 POST_BUILD 拷贝同目标目录; ALL 保证随 libagentxx 构建执行
  if (AGENTXX_BUILTIN_RESOURCE_OUTPUTS)
    add_custom_target(agentxx_builtin_plugin_resources ALL
      DEPENDS ${AGENTXX_BUILTIN_RESOURCE_OUTPUTS}
    )
  endif()

  message("[agentxx_plugins] Builtin plugins (merged into libagentxx): ${AGENTXX_BUILTIN_PLUGIN_NAMES}")

  # 收集结果传回 libagentxx (add_subdirectory 的父作用域)
  set(AGENTXX_BUILTIN_PLUGIN_OBJECT_TARGETS "${AGENTXX_BUILTIN_PLUGIN_OBJECT_TARGETS}" PARENT_SCOPE)
  set(AGENTXX_BUILTIN_PLUGIN_LINK_LIBS "${AGENTXX_BUILTIN_PLUGIN_LINK_LIBS}" PARENT_SCOPE)
  set(AGENTXX_BUILTIN_MANIFEST_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/builtin_plugins.cpp" PARENT_SCOPE)
  return()
endif()

# ===== 各插件子目录 (独立动态库编译, 默认) =====
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
