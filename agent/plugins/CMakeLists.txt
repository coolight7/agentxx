cmake_minimum_required(VERSION 3.10)
# find_package 使用 <PackageName>_ROOT 变量
cmake_policy(SET CMP0074 NEW)
# MSVC 运行时库标志由抽象层选择
cmake_policy(SET CMP0091 NEW)
# find_package 使用大写的 _ROOT 变量
cmake_policy(SET CMP0144 NEW)
# 移除 cmake 内置的 FindBoost 模块
cmake_policy(SET CMP0167 OLD)

project(agentxx_plugins VERSION ${AGENTXX_VERSION} LANGUAGES C CXX)

# 由顶层 superbuild 传入 (见 agent/CMakeLists.txt)，独立构建本目录时取默认值
option(AGENTXX_ENABLE_PLUGIN_EXAMPLE "Build example plugin." ON)
option(AGENTXX_ENABLE_PLUGIN_JS "Build QuickJS JS plugin engine (agentxx_javascript_engine)." ON)
option(AGENTXX_ENABLE_PLUGIN_CODEGRAPH "Build agentxx_codegraph plugin (requires codegraph libs)." ON)
option(AGENTXX_ENABLE_PLUGIN_COMPUTER_USE "Build agentxx_computer_use plugin (ui_control)." ON)
option(AGENTXX_ENABLE_PLUGIN_SCREEN_CAPTURE "Build agentxx_screen_capture plugin (screen capture)." ON)
option(AGENTXX_ENABLE_PLUGIN_SYSTEM_MONITOR "Build agentxx_system_monitor plugin (CPU/GPU usage)." ON)
option(AGENTXX_ENABLE_PLUGIN_AUDIO_STREAM "Build agentxx_audio_stream plugin (audio capture)." ON)
option(AGENTXX_ENABLE_PLUGIN_TEXT_SELECTION "Build agentxx_text_selection_monitor plugin (text selection)." ON)
option(AGENTXX_ENABLE_PLUGIN_PLANNING "Build agentxx_planning plugin (task planning write tool)." ON)
option(AGENTXX_ENABLE_PLUGIN_FILESYSTEM "Build agentxx_filesystem plugin (list/read/write/edit/glob/grep)." ON)
option(AGENTXX_ENABLE_PLUGIN_EXECUTE_COMMAND "Build agentxx_execute_command plugin (bash/windows command)." ON)
option(AGENTXX_ENABLE_PLUGIN_STRING "Build agentxx_string plugin (html2markdown/regexp)." ON)
option(AGENTXX_ENABLE_PLUGIN_MATH "Build agentxx_math plugin (math calculation)." ON)
option(AGENTXX_ENABLE_PLUGIN_SYSTEM "Build agentxx_system plugin (current date/time)." ON)
option(AGENTXX_ENABLE_PLUGIN_WEBSEARCH "Build agentxx_websearch plugin (search/fetch url)." ON)
option(AGENTXX_ENABLE_PLUGIN_RAG_SEARCH "Build agentxx_rag_search plugin (embedding based docs search)." ON)
option(AGENTXX_ENABLE_PLUGIN_EXECUTE_JAVASCRIPT "Build agentxx_execute_javascript plugin (JS execution via QuickJS, bash equivalent)." ON)

# ===== 插件平台支持 =====
# 各插件并非全平台适配: 源码无对应平台的真实实现时 (仅有 "not supported/
# not implemented" 桩) 跳过编译该插件, 避免产出无功能意义的动态库。
# - 判定逻辑分散在各插件自身 CMakeLists.txt 开头 (仅插件清楚自己的实现情况):
#   声明支持的平台列表并经 cmake/plugin_platform_support.cmake 的
#   agentxx_plugin_platform_gate() 判定, 不支持则提前 return() 跳过本目录全部
#   目标; 内置合并模式下的清单登记 (AGENTXX_BUILTIN_PLUGIN_NAMES) 也由此天然
#   保持正确 (提前 return 时不会误登记)
# - 平台矩阵说明与依据: docs/zh-cn/plugins.md 9.3.1

# 依赖库查找路径 (独立构建本目录时生效; 嵌套构建时顶层已传 CMAKE_PREFIX_PATH)
list(PREPEND CMAKE_PREFIX_PATH "${AGENTXX_INSTALL_DIR}")

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
#   (agentxx_plugin_agent_get_info/create/destroy + agentxx_plugin_client_get_info/create/destroy,
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
# - 内置合并编译模式下插件符号直接并入 libagentxx, 本分支不生效 (见上)
if (XX_IS_ANDROID_D OR XX_IS_LINUX_D)
  add_compile_options(-fvisibility=hidden -fvisibility-inlines-hidden)
  # ELF (Linux/Android): version script 白名单, local: * 隐藏其余全部符号
  # - 使用通配符而非精确符号名: 单端插件仅定义 agent 入口 (或 client 入口)
  #   之一, 精确名白名单在 Android lld (-Wl,--fatal-warnings, NDK 默认) 下
  #   会因 "symbol not defined" 报错; GNU ld 静默忽略但 lld 不会。
  #   通配符未匹配时不报错, 且仍保证非 agentxx_plugin_agent_/agentxx_plugin_client_
  #   前缀的符号全部隐藏
  set(_AGENTXX_EXPORT_SYMS_FILE "${CMAKE_CURRENT_BINARY_DIR}/plugin_symbols.map")
  file(WRITE "${_AGENTXX_EXPORT_SYMS_FILE}" "# agentxx 插件动态库导出符号白名单
# 宿主仅经 dlsym/GetProcAddress 按名查找以下入口符号 (见 plugin_api.h
# AGENTXX_PLUGIN_EXPORT); local: * 隐藏其余全部符号 (含第三方静态库符号);
# 通配符形式兼容单端插件 (仅定义 agent 或 client 之一) 在 lld 下的链接
{
global:
  agentxx_plugin_agent_*;
  agentxx_plugin_client_*;
local: *;
};
")
  add_link_options("-Wl,--version-script=${_AGENTXX_EXPORT_SYMS_FILE}")
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

if (NOT DEFINED neograph_SOURCE_DIR)
  set(neograph_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/neograph")
endif()

add_compile_definitions(
  __STDC_CONSTANT_MACROS
  XX_IS_DEBUG_D=${XX_IS_DEBUG_D}
  XX_IS_RELEASE_D=${XX_IS_RELEASE_D}
  XX_IS_LINUX_D=${XX_IS_LINUX_D}
  XX_IS_WIN_D=${XX_IS_WIN_D}
  XX_IS_MACOS_D=${XX_IS_MACOS_D}
  XX_IS_ANDROID_D=${XX_IS_ANDROID_D}
  XX_IS_IOS_D=${XX_IS_IOS_D}
  NEOGRAPH_STATIC_BUILD
  NEOGRAPH_USE_BOOST_ASIO
  CPPHTTPLIB_OPENSSL_SUPPORT
)

include_directories(
  "${AGENTXX_LIB_INCLUDE_DIR}"
  "${AGENTXX_INSTALL_DIR}/include"
  "${neograph_SOURCE_DIR}/deps/yyjson"
)

# --- 名单解析: AGENTXX_PLUGIN_BUILTIN_LIST → ALL 标志 / 具名名单 ---
set(_AGENTXX_BUILTIN_ALL OFF)
set(_AGENTXX_BUILTIN_NAMED "")
if ("${AGENTXX_PLUGIN_BUILTIN_LIST}" STREQUAL "all")
  set(_AGENTXX_BUILTIN_ALL ON)
else()
  # 宽容处理: 逗号分隔同样接受 (命令行 -D 传分号列表需转义, 逗号更顺手)
  string(REPLACE "," ";" _AGENTXX_BUILTIN_NAMED "${AGENTXX_PLUGIN_BUILTIN_LIST}")
  list(REMOVE_ITEM _AGENTXX_BUILTIN_NAMED "")
endif()

# --- 每插件内置判定 + 统一装载 ---
# macro (非 function): 不产生新变量作用域, 子目录内置分支的 PARENT_SCOPE
# 收集变量直接回到本目录作用域; _AGENTXX_ADD_AS_BUILTIN 为目录作用域变量,
# add_subdirectory 后子目录继承本次判定值 (各插件 CMakeLists 开头处理),
# 实现"名单内插件内置合并 / 名单外插件独立动态库"的混合模式
macro(agentxx_load_plugin _dir_name _enable_var)
  if (${_enable_var})
    set(_AGENTXX_ADD_AS_BUILTIN OFF)
    if ((_AGENTXX_BUILTIN_ALL OR "${_dir_name}" IN_LIST _AGENTXX_BUILTIN_NAMED))
      set(_AGENTXX_ADD_AS_BUILTIN ON)
    endif()
    add_subdirectory(${_dir_name})
  endif()
endmacro()

# 内置收集变量初始化 (各插件内置分支追加, 经 PARENT_SCOPE/目录作用域传播
# 回本目录; 汇总后整体传回 libagentxx 合并进共享/静态库)
# MSVC: 插件 OBJECT 库多目标并行编译时 PDB 写 (MSPDBSRV) 偶发 C1041
# 竞态; 调试信息直接嵌入 .obj (/Z7, 不生成目标 PDB), 从根上消除
if (MSVC)
  add_compile_options(/Z7)
endif()

set(AGENTXX_BUILTIN_PLUGIN_OBJECT_TARGETS "")
set(AGENTXX_BUILTIN_PLUGIN_LINK_LIBS "")
set(AGENTXX_BUILTIN_PLUGIN_NAMES "")
set(AGENTXX_BUILTIN_PLUGIN_MANIFEST_DECLS "")
set(AGENTXX_BUILTIN_PLUGIN_MANIFEST_ENTRIES "")

# ===== 各插件装载 (启用开关判定 + 内置/动态库双模式) =====
# - 平台不支持时由各插件自身 CMakeLists 开头的判定提前 return() 跳过
#   (见 cmake/plugin_platform_support.cmake 与各插件声明处); 内置清单登记
#   由各插件 builtin 分支自行追加, 提前 return 时天然不会误登记
# - [AGENTXX_PLUGIN_BUILTIN_LIST] 控制内嵌编译插件列表
agentxx_load_plugin(example_plugin AGENTXX_ENABLE_PLUGIN_EXAMPLE)
agentxx_load_plugin(example_resources AGENTXX_ENABLE_PLUGIN_EXAMPLE)
agentxx_load_plugin(example_graph_node AGENTXX_ENABLE_PLUGIN_EXAMPLE)
# JS 相关 (example_js 依赖 quickjs 引擎目标)
agentxx_load_plugin(agentxx_javascript_engine AGENTXX_ENABLE_PLUGIN_JS)
agentxx_load_plugin(example_js AGENTXX_ENABLE_PLUGIN_EXAMPLE)

# agentxx_computer_use (运行时依赖 agentxx_screen_capture)
agentxx_load_plugin(agentxx_screen_capture AGENTXX_ENABLE_PLUGIN_SCREEN_CAPTURE)
agentxx_load_plugin(agentxx_computer_use AGENTXX_ENABLE_PLUGIN_COMPUTER_USE)

agentxx_load_plugin(agentxx_codegraph AGENTXX_ENABLE_PLUGIN_CODEGRAPH)
agentxx_load_plugin(agentxx_system_monitor AGENTXX_ENABLE_PLUGIN_SYSTEM_MONITOR)
agentxx_load_plugin(agentxx_audio_stream AGENTXX_ENABLE_PLUGIN_AUDIO_STREAM)
agentxx_load_plugin(agentxx_text_selection_monitor AGENTXX_ENABLE_PLUGIN_TEXT_SELECTION)
agentxx_load_plugin(agentxx_execute_javascript AGENTXX_ENABLE_PLUGIN_EXECUTE_JAVASCRIPT)
agentxx_load_plugin(agentxx_string AGENTXX_ENABLE_PLUGIN_STRING)
agentxx_load_plugin(agentxx_math AGENTXX_ENABLE_PLUGIN_MATH)

agentxx_load_plugin(agentxx_planning AGENTXX_ENABLE_PLUGIN_PLANNING)
agentxx_load_plugin(agentxx_filesystem AGENTXX_ENABLE_PLUGIN_FILESYSTEM)
agentxx_load_plugin(agentxx_execute_command AGENTXX_ENABLE_PLUGIN_EXECUTE_COMMAND)
agentxx_load_plugin(agentxx_system AGENTXX_ENABLE_PLUGIN_SYSTEM)
agentxx_load_plugin(agentxx_websearch AGENTXX_ENABLE_PLUGIN_WEBSEARCH)
agentxx_load_plugin(agentxx_rag_search AGENTXX_ENABLE_PLUGIN_RAG_SEARCH)

# ===== 内置清单生成 =====
list(LENGTH AGENTXX_BUILTIN_PLUGIN_NAMES AGENTXX_BUILTIN_PLUGIN_COUNT)
set(AGENTXX_BUILTIN_PLUGIN_MANIFEST_DECLS "")
set(AGENTXX_BUILTIN_PLUGIN_MANIFEST_ENTRIES "")
set(AGENTXX_BUILTIN_PLUGIN_MANIFEST_YAML_ARRAY "")
foreach(_bp IN LISTS AGENTXX_BUILTIN_PLUGIN_NAMES)
  set(AGENTXX_BUILTIN_PLUGIN_MANIFEST_DECLS
    "${AGENTXX_BUILTIN_PLUGIN_MANIFEST_DECLS}
extern \"C\" const AgentxxPluginInfo* AGENTXX_PLUGIN_CALL agentxx_plugin_agent_builtin_get_info_${_bp}(void);
extern \"C\" int32_t AGENTXX_PLUGIN_CALL agentxx_plugin_agent_builtin_create_${_bp}(const AgentxxPluginHost* host, void** plugin_ctx);
extern \"C\" void AGENTXX_PLUGIN_CALL agentxx_plugin_agent_builtin_destroy_${_bp}(void* plugin_ctx);")
  set(AGENTXX_BUILTIN_PLUGIN_MANIFEST_ENTRIES
    "${AGENTXX_BUILTIN_PLUGIN_MANIFEST_ENTRIES}
    { { \"${_bp}\", sizeof(\"${_bp}\") - 1 }, agentxx_plugin_agent_builtin_get_info_${_bp}, agentxx_plugin_agent_builtin_create_${_bp}, agentxx_plugin_agent_builtin_destroy_${_bp} },")

  set(_yaml_path "${CMAKE_CURRENT_SOURCE_DIR}/${_bp}/plugin.yaml")
  if(EXISTS "${_yaml_path}")
    file(READ "${_yaml_path}" _yaml_raw)
    # 使用 raw string R"AGENTXX_YAML(... )AGENTXX_YAML" 包裹, 避免转义
    # 若 yaml 自身包含 )AGENTXX_YAML" 则退化为空串 (极罕见, 视作无清单)
    if(_yaml_raw MATCHES "\\)AGENTXX_YAML\"")
      message(WARNING "[agentxx_plugins] ${_bp}/plugin.yaml contains raw delimiter, embedding as empty")
      string(APPEND AGENTXX_BUILTIN_PLUGIN_MANIFEST_YAML_ARRAY "    { { \"${_bp}\", sizeof(\"${_bp}\") - 1 }, { \"\", 0 } },\n")
    else()
      string(APPEND AGENTXX_BUILTIN_PLUGIN_MANIFEST_YAML_ARRAY "    { { \"${_bp}\", sizeof(\"${_bp}\") - 1 }, { R\"AGENTXX_YAML(${_yaml_raw})AGENTXX_YAML\", sizeof(R\"AGENTXX_YAML(${_yaml_raw})AGENTXX_YAML\") - 1 } },\n")
    endif()
  else()
    message(WARNING "[agentxx_plugins] ${_bp}/plugin.yaml not found, embedding as empty")
    string(APPEND AGENTXX_BUILTIN_PLUGIN_MANIFEST_YAML_ARRAY "    { { \"${_bp}\", sizeof(\"${_bp}\") - 1 }, { \"\", 0 } },\n")
  endif()
endforeach()
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/builtin_plugins.cpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/builtin_plugins.cpp"
  @ONLY
)

message("[agentxx_plugins] Builtin plugins (merged into libagentxx): ${AGENTXX_BUILTIN_PLUGIN_NAMES}")

# 收集结果传回 libagentxx (add_subdirectory 的父作用域)
set(AGENTXX_BUILTIN_PLUGIN_OBJECT_TARGETS "${AGENTXX_BUILTIN_PLUGIN_OBJECT_TARGETS}" PARENT_SCOPE)
set(AGENTXX_BUILTIN_PLUGIN_LINK_LIBS "${AGENTXX_BUILTIN_PLUGIN_LINK_LIBS}" PARENT_SCOPE)
set(AGENTXX_BUILTIN_PLUGIN_MANIFEST_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/builtin_plugins.cpp" PARENT_SCOPE)
