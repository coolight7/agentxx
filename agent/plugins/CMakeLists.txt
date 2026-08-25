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
# ---- 从 lib 内置工具迁移的编程工具插件 (同名同行为; 默认启用编译,
#      是否内置合并经 AGENTXX_PLUGIN_BUILTIN_LIST 门控) ----
option(AGENTXX_ENABLE_PLUGIN_PLANNING "Build agentxx_planning plugin (task planning write tool)." ON)
option(AGENTXX_ENABLE_PLUGIN_FILESYSTEM "Build agentxx_filesystem plugin (list/read/write/edit/glob/grep)." ON)
option(AGENTXX_ENABLE_PLUGIN_EXECUTE_COMMAND "Build agentxx_execute_command plugin (bash/windows command)." ON)
option(AGENTXX_ENABLE_PLUGIN_STRING "Build agentxx_string plugin (html2markdown/regexp)." ON)
option(AGENTXX_ENABLE_PLUGIN_SYSTEM "Build agentxx_system plugin (current date/time)." ON)
option(AGENTXX_ENABLE_PLUGIN_WEBSEARCH "Build agentxx_websearch plugin (search/fetch url)." ON)
option(AGENTXX_ENABLE_PLUGIN_RAG_SEARCH "Build agentxx_rag_search plugin (embedding based docs search)." ON)
# 内置合并编译模式: 启用的插件源文件直接编译进 libagentxx (运行期无需插件
# 动态库, 无 dlopen); 仅经 libagentxx 构建生效 (见下方"内置合并编译模式")
option(AGENTXX_ENABLE_PLUGIN_BUILTIN "Merge enabled plugins into libagentxx (built-in, no runtime plugin dlls)." OFF)
# 内置合并名单门控 (仅 AGENTXX_ENABLE_PLUGIN_BUILTIN=ON 时生效):
# - 未设置/空/"all": 全部启用插件内置合并进 libagentxx
# - 分号(或逗号)分隔的插件名列表 (如 "agentxx_filesystem;agentxx_planning"):
#   仅名单内插件内置合并, 名单外启用插件仍编译为独立动态库
set(AGENTXX_PLUGIN_BUILTIN_LIST "" CACHE STRING
  "Which enabled plugins to merge into libagentxx when AGENTXX_ENABLE_PLUGIN_BUILTIN=ON ('' or 'all' = all; semicolon list = only listed ones)")

# ===== 插件平台支持 =====
# 各插件并非全平台适配: 源码无对应平台的真实实现时 (仅有 "not supported/
# not implemented" 桩) 跳过编译该插件, 避免产出无功能意义的动态库。
# - 判定逻辑分散在各插件自身 CMakeLists.txt 开头 (仅插件清楚自己的实现情况):
#   声明支持的平台列表并经 cmake/plugin_platform_support.cmake 的
#   agentxx_plugin_platform_gate() 判定, 不支持则提前 return() 跳过本目录全部
#   目标; 内置合并模式下的清单登记 (AGENTXX_BUILTIN_PLUGIN_NAMES) 也由此天然
#   保持正确 (提前 return 时不会误登记)
# - 平台变量复用顶层传入的 XX_IS_*_D (独立构建时在共享辅助内按顶层规则推导)
# - 平台矩阵说明与依据: docs/agent/plugins.md 9.3.1

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
    foreach(_sym agentxx_plugin_get_info agentxx_plugin_create agentxx_plugin_destroy
                 agentxx_client_get_info agentxx_client_create agentxx_client_destroy)
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


# --- 名单解析: AGENTXX_PLUGIN_BUILTIN_LIST → ALL 标志 / 具名名单 ---
set(_AGENTXX_BUILTIN_ALL OFF)
set(_AGENTXX_BUILTIN_NAMED "")
if (AGENTXX_ENABLE_PLUGIN_BUILTIN)
  # 内置合并须由 libagentxx 构建引入 (存在可合并的库目标); 本目录独立构建
  # 时无 libagentxx 可合并, 直接报错 (与既有行为一致)
  if (NOT AGENTXX_IS_LIB_BUILTIN_SUBDIR)
    message(FATAL_ERROR
      "[agentxx_plugins] AGENTXX_ENABLE_PLUGIN_BUILTIN=ON 须经 libagentxx 构建 "
      "(在顶层 agent/CMakeLists.txt 配置时开启; 本目录独立构建时请关闭该选项)")
  endif()
  if ("${AGENTXX_PLUGIN_BUILTIN_LIST}" STREQUAL "" OR "${AGENTXX_PLUGIN_BUILTIN_LIST}" STREQUAL "all")
    set(_AGENTXX_BUILTIN_ALL ON)
  else()
    # 宽容处理: 逗号分隔同样接受 (命令行 -D 传分号列表需转义, 逗号更顺手)
    string(REPLACE "," ";" _AGENTXX_BUILTIN_NAMED "${AGENTXX_PLUGIN_BUILTIN_LIST}")
    list(REMOVE_ITEM _AGENTXX_BUILTIN_NAMED "")
  endif()
endif()

# --- 每插件内置判定 + 统一装载 ---
# macro (非 function): 不产生新变量作用域, 子目录内置分支的 PARENT_SCOPE
# 收集变量直接回到本目录作用域; _AGENTXX_ADD_AS_BUILTIN 为目录作用域变量,
# add_subdirectory 后子目录继承本次判定值 (各插件 CMakeLists 开头消费),
# 实现"名单内插件内置合并 / 名单外插件独立动态库"的混合模式
macro(agentxx_load_plugin _dir_name _enable_var)
  if (${_enable_var})
    set(_AGENTXX_ADD_AS_BUILTIN OFF)
    if (AGENTXX_ENABLE_PLUGIN_BUILTIN
        AND (_AGENTXX_BUILTIN_ALL OR "${_dir_name}" IN_LIST _AGENTXX_BUILTIN_NAMED))
      set(_AGENTXX_ADD_AS_BUILTIN ON)
    endif()
    add_subdirectory(${_dir_name})
  endif()
endmacro()

# 内置收集变量初始化 (各插件内置分支追加, 经 PARENT_SCOPE/目录作用域传播
# 回本目录; 汇总后整体传回 libagentxx 合并进共享/静态库)
if (AGENTXX_ENABLE_PLUGIN_BUILTIN)
  # MSVC: 插件 OBJECT 库多目标并行编译时 PDB 写 (MSPDBSRV) 偶发 C1041
  # 竞态; 调试信息直接嵌入 .obj (/Z7, 不生成目标 PDB), 从根上消除
  if (MSVC)
    add_compile_options(/Z7)
  endif()

  set(AGENTXX_BUILTIN_PLUGIN_OBJECT_TARGETS "")
  set(AGENTXX_BUILTIN_PLUGIN_LINK_LIBS "")
  set(AGENTXX_BUILTIN_PLUGIN_NAMES "")
  set(AGENTXX_BUILTIN_MANIFEST_DECLS "")
  set(AGENTXX_BUILTIN_MANIFEST_ENTRIES "")
endif()

# ===== 各插件装载 (启用开关判定 + 内置/动态库双模式) =====
# - 平台不支持时由各插件自身 CMakeLists 开头的判定提前 return() 跳过
#   (见 cmake/plugin_platform_support.cmake 与各插件声明处); 内置清单登记
#   由各插件 builtin 分支自行追加, 提前 return 时天然不会误登记

agentxx_load_plugin(example_plugin AGENTXX_ENABLE_PLUGIN_EXAMPLE)
agentxx_load_plugin(example_resources AGENTXX_ENABLE_PLUGIN_EXAMPLE)

# JS 相关 (example_js 依赖 quickjs 引擎目标, 须随引擎同批装载)
agentxx_load_plugin(agentxx_javascript_engine AGENTXX_ENABLE_PLUGIN_JS)
agentxx_load_plugin(example_js AGENTXX_ENABLE_PLUGIN_EXAMPLE)

# agentxx_codegraph (需 codegraph-cpp 库; 顶层 AGENTXX_ENABLE_PLUGIN_CODEGRAPH 时构建)
agentxx_load_plugin(agentxx_codegraph AGENTXX_ENABLE_PLUGIN_CODEGRAPH)

# agentxx_screen_capture (屏幕捕获; 独立插件, agentxx_computer_use depends 依赖它;
# 与 computer_use 同为仅 Windows 实现, 平台支持须保持一致)
agentxx_load_plugin(agentxx_screen_capture AGENTXX_ENABLE_PLUGIN_SCREEN_CAPTURE)

# agentxx_computer_use (ui_control; 运行时依赖 agentxx_screen_capture, 配置时须同时加载)
agentxx_load_plugin(agentxx_computer_use AGENTXX_ENABLE_PLUGIN_COMPUTER_USE)

# agentxx_system_monitor (系统资源监控: CPU/内存/GPU; 从 lib expand 拆分;
# Windows/Linux 有真实实现, Android 复用 Linux 分支, macOS 为桩跳过)
agentxx_load_plugin(agentxx_system_monitor AGENTXX_ENABLE_PLUGIN_SYSTEM_MONITOR)

# agentxx_audio_stream (音频流捕获: 系统输出/程序输出/麦克风; 从 lib expand 拆分;
# 当前所有平台实现均未完成 —— Windows WASAPI 被源码 `&& false` 停用, 全平台跳过)
agentxx_load_plugin(agentxx_audio_stream AGENTXX_ENABLE_PLUGIN_AUDIO_STREAM)

# agentxx_text_selection_monitor (系统级文本选择事件流; 从 lib expand 拆分;
# 仅 Windows 有真实实现)
agentxx_load_plugin(agentxx_text_selection_monitor AGENTXX_ENABLE_PLUGIN_TEXT_SELECTION)

# ---- 从 lib 内置工具迁移的编程工具插件 (同名同行为; 默认全部内置编译,
#      经 AGENTXX_PLUGIN_BUILTIN_LIST 门控, 见文件头说明) ----
agentxx_load_plugin(agentxx_planning AGENTXX_ENABLE_PLUGIN_PLANNING)
agentxx_load_plugin(agentxx_filesystem AGENTXX_ENABLE_PLUGIN_FILESYSTEM)
agentxx_load_plugin(agentxx_execute_command AGENTXX_ENABLE_PLUGIN_EXECUTE_COMMAND)
agentxx_load_plugin(agentxx_string AGENTXX_ENABLE_PLUGIN_STRING)
agentxx_load_plugin(agentxx_system AGENTXX_ENABLE_PLUGIN_SYSTEM)
agentxx_load_plugin(agentxx_websearch AGENTXX_ENABLE_PLUGIN_WEBSEARCH)
agentxx_load_plugin(agentxx_rag_search AGENTXX_ENABLE_PLUGIN_RAG_SEARCH)

# ===== 内置清单生成 (存在任一实际内置的插件时) =====
if (AGENTXX_ENABLE_PLUGIN_BUILTIN)
  list(LENGTH AGENTXX_BUILTIN_PLUGIN_NAMES AGENTXX_BUILTIN_PLUGIN_COUNT)
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/builtin_plugins.cpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/builtin_plugins.cpp"
    @ONLY
  )


  message("[agentxx_plugins] Builtin plugins (merged into libagentxx): ${AGENTXX_BUILTIN_PLUGIN_NAMES}")

  # 收集结果传回 libagentxx (add_subdirectory 的父作用域)
  set(AGENTXX_BUILTIN_PLUGIN_OBJECT_TARGETS "${AGENTXX_BUILTIN_PLUGIN_OBJECT_TARGETS}" PARENT_SCOPE)
  set(AGENTXX_BUILTIN_PLUGIN_LINK_LIBS "${AGENTXX_BUILTIN_PLUGIN_LINK_LIBS}" PARENT_SCOPE)
  set(AGENTXX_BUILTIN_MANIFEST_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/builtin_plugins.cpp" PARENT_SCOPE)
endif()
