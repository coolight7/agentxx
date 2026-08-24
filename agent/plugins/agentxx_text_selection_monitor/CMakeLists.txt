# agentxx_text_selection_monitor —— 系统级文本选择事件流插件 (Windows)
# - 默认: 独立动态库 (MODULE), 仅依赖纯 C ABI 头 + fmt + simdjson + asio (boost 头文件)
# - 内置模式 (AGENTXX_ENABLE_PLUGIN_BUILTIN=ON): 源文件编译为 OBJECT 库合并进
#   libagentxx; fmt/simdjson/asio 为 libagentxx 既有依赖, 仅补链接平台库
#   (uiautomationcore)
# - 从 libagentxx src/expand/text_selection_monitor 拆分独立:
#   - 工具 agentxx_text_selection_monitor (start/stop/status)
#   - 选中文本经 publish 事件推送 (topic "agentxx_text_selection_monitor.selection")
set(PLUGIN_NAME agentxx_text_selection_monitor)

# ===== 平台支持判定 =====
# 仅 Windows 有真实实现 (UIAutomation/IAccessible 文本选择 + CDP WebSocket);
# 其余平台源码为 "not supported on this platform" 空桩, 跳过编译
# (平台矩阵说明见 docs/agent/plugins.md 9.3.1)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/plugin_platform_support.cmake")
agentxx_plugin_platform_gate(${PLUGIN_NAME} _AGENTXX_PLATFORM_OK windows)
if (NOT _AGENTXX_PLATFORM_OK)
  return() # 跳过本插件全部目标 (内置合并/独立动态库两模式均在此拦截)
endif ()

if (_AGENTXX_ADD_AS_BUILTIN)
  # ===== 内置合并编译分支 =====
  # asio 头文件路径 (BOOST_ROOT 由顶层 superbuild 传入; 与动态分支一致)
  if (NOT DEFINED BOOST_ROOT)
    set(_BOOST_CANDIDATES
      "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/boost-linux-build-debug"
      "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/boost-linux-build-release"
      "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/boost-windows-build-debug"
      "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/boost-windows-build-release"
    )
    foreach (_bc ${_BOOST_CANDIDATES})
      if (EXISTS "${_bc}/include/boost/asio/steady_timer.hpp")
        set(BOOST_ROOT "${_bc}")
        break()
      endif()
    endforeach()
  endif()
  if (NOT DEFINED BOOST_ROOT OR NOT EXISTS "${BOOST_ROOT}/include/boost/asio/steady_timer.hpp")
    message(FATAL_ERROR "[agentxx_text_selection_monitor] BOOST_ROOT not found (asio headers required)")
  endif()

  set(_target abp_${PLUGIN_NAME})
  add_library(${_target} OBJECT
    ${CMAKE_CURRENT_SOURCE_DIR}/agentxx_text_selection_monitor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/text_selection_monitor.cpp
  )
  target_compile_definitions(${_target} PRIVATE
    "AGENTXX_PLUGIN_BUILTIN=1"
    "agentxx_plugin_entry=agentxx_plugin_builtin_entry_${PLUGIN_NAME}"
    "agentxx_plugin_get_info=agentxx_plugin_builtin_get_info_${PLUGIN_NAME}"
    "agentxx_plugin_unload=agentxx_plugin_builtin_unload_${PLUGIN_NAME}"
  )
  target_include_directories(${_target} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"          # text_selection_monitor.h / text_selection_monitor_plugin.h
    "${AGENTXX_LIB_INCLUDE_DIR}"           # agentxx/plugin/plugin_api.h
    "${AGENTXX_INSTALL_DIR}/include"       # simdjson.h fmt
    "${BOOST_ROOT}/include/"
    "${BOOST_ROOT}/include/boost/"
  )
  # 复用 agentxx_util 工具库 (libagentxx 已链接本库; 此处经 PRIVATE 链接仅
  # 传递编译宏/头路径: util 头文件依赖 XX_IS_* 宏与 include 路径)
  target_link_libraries(${_target} PRIVATE agentxx_util)
set_target_properties(${_target} PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED OFF
  )
  set(AGENTXX_BUILTIN_MANIFEST_DECLS
    "${AGENTXX_BUILTIN_MANIFEST_DECLS}
extern \"C\" const AgentxxPluginInfo* agentxx_plugin_builtin_get_info_${PLUGIN_NAME}(void);
extern \"C\" int agentxx_plugin_builtin_entry_${PLUGIN_NAME}(const AgentxxHost* host, void** plugin_ctx);
extern \"C\" void agentxx_plugin_builtin_unload_${PLUGIN_NAME}(void* plugin_ctx);")
  set(AGENTXX_BUILTIN_MANIFEST_ENTRIES
    "${AGENTXX_BUILTIN_MANIFEST_ENTRIES}
    {\"${PLUGIN_NAME}\", agentxx_plugin_builtin_get_info_${PLUGIN_NAME}, agentxx_plugin_builtin_entry_${PLUGIN_NAME}, agentxx_plugin_builtin_unload_${PLUGIN_NAME}},")
  if (WIN32)
    # text_selection_monitor: UIAutomation (uiautomationcore) / IAccessible (oleacc) /
    # COM (ole32) / CDP WebSocket (ws2_32, winhttp) / 窗口与钩子 (user32)
    # - 独立模式经下方独立分支 PRIVATE 链接; 内置模式经本收集通道并入 libagentxx
    set(AGENTXX_BUILTIN_PLUGIN_LINK_LIBS
      "${AGENTXX_BUILTIN_PLUGIN_LINK_LIBS}" uiautomationcore oleacc ole32 user32 ws2_32 winhttp PARENT_SCOPE)
  else()
    # asio 文件 I/O 在 -DASIO_HAS_IO_URING 下引用 io_uring 符号;
    # libagentxx 已按 AGENTXX_LINUX_IO_URING_SUPPORTED 条件链接 liburing
  endif()
  set(AGENTXX_BUILTIN_PLUGIN_OBJECT_TARGETS "${AGENTXX_BUILTIN_PLUGIN_OBJECT_TARGETS}" "${_target}" PARENT_SCOPE)
  # 登记进内置插件清单 (子目录自登记: 平台不支持时本文件开头已提前 return,
  # 不会误登记; 原由父目录 plugins/CMakeLists.txt 无条件追加, 现统一走此通道)
  list(APPEND AGENTXX_BUILTIN_PLUGIN_NAMES ${PLUGIN_NAME})
  set(AGENTXX_BUILTIN_PLUGIN_NAMES "${AGENTXX_BUILTIN_PLUGIN_NAMES}" PARENT_SCOPE)
  set(AGENTXX_BUILTIN_MANIFEST_DECLS "${AGENTXX_BUILTIN_MANIFEST_DECLS}" PARENT_SCOPE)
  set(AGENTXX_BUILTIN_MANIFEST_ENTRIES "${AGENTXX_BUILTIN_MANIFEST_ENTRIES}" PARENT_SCOPE)
  # 资源: plugin.yaml (运行期目录配置推导 depends/入口)
  set(_res "${AGENTXX_EXEC_INSTALL_PREFIX}/plugins/${PLUGIN_NAME}/plugin.yaml")
  # 资源拷贝 (自包含 ALL 目标: 跨目录 OUTPUT 不进构建图, 见 plugins.md;
  # ALL 保证随 libagentxx 构建执行; 仅内置合并分支存在本目标)
  add_custom_target(${PLUGIN_NAME}_builtin_resources ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/plugin.yaml" "${_res}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/plugin.yaml"
    COMMENT "Copy ${PLUGIN_NAME} plugin resource (builtin mode)")
  return()
endif()

# ===== 独立动态库编译 (默认) =====
add_library(${PLUGIN_NAME} SHARED
  agentxx_text_selection_monitor.cpp
  text_selection_monitor.cpp
)
target_include_directories(${PLUGIN_NAME} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"          # text_selection_monitor.h / text_selection_monitor_plugin.h
  "${AGENTXX_LIB_INCLUDE_DIR}"           # agentxx/plugin/plugin_api.h
  "${AGENTXX_INSTALL_DIR}/include"       # simdjson.h fmt
)
# 复用 agentxx_util 工具库 (插件静态链接, 与宿主副本符号互不冲突;
# find_package 经 AGENTXX_INSTALL_DIR 的 agentxx_util config 解析,
# 依赖链 fmt/SQLite3/uchardet/Iconv 由 config 内 find_dependency 传递)
if (NOT TARGET agentxx_util)
  # 独立构建本插件时经安装 config 导入; 内置合并模式 (经 libagentxx 构建)
  # 下 util 目标已存在于宿主工程, 跳过导入 (此时 install 目录尚未生成 config)
  find_package(agentxx_util REQUIRED)
endif()
find_package(fmt REQUIRED)
target_link_libraries(${PLUGIN_NAME} PRIVATE
  agentxx_util
  simdjson
  fmt::fmt
)
target_link_directories(${PLUGIN_NAME} PRIVATE
  "${AGENTXX_INSTALL_DIR}/lib"
)

# asio 头文件 (text_selection_monitor 使用 asio io_context/steady_timer;
# 头文件-only, 仅需 include 路径) —— 与 agentxx_system_monitor 相同策略
if (NOT DEFINED BOOST_ROOT)
  set(_BOOST_CANDIDATES
    "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/boost-linux-build-debug"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/boost-linux-build-release"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/boost-windows-build-debug"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/boost-windows-build-release"
  )
  foreach (_bc ${_BOOST_CANDIDATES})
    if (EXISTS "${_bc}/include/boost/asio/steady_timer.hpp")
      set(BOOST_ROOT "${_bc}")
      break()
    endif()
  endforeach()
endif()
if (NOT DEFINED BOOST_ROOT OR NOT EXISTS "${BOOST_ROOT}/include/boost/asio/steady_timer.hpp")
  message(FATAL_ERROR "[agentxx_text_selection_monitor] BOOST_ROOT not found (asio headers required)")
endif()
# boost 分发布局: ${BOOST_ROOT}/include/boost/asio/...; 源码用 asio/... 前缀
# (映射 include/boost/), 而 asio 头内部用 <boost/asio/...> 形式 (映射 include/),
# 两者缺一会导致内部头回退到系统安装的 boost, 与宿主 asio 混用崩溃
target_include_directories(${PLUGIN_NAME} PRIVATE
  "${BOOST_ROOT}/include/"
  "${BOOST_ROOT}/include/boost/"
)

if (WIN32)
  # text_selection_monitor: UIAutomation (uiautomationcore) / IAccessible (oleacc) /
  # COM (ole32) / CDP WebSocket (ws2_32, winhttp) / 窗口与钩子 (user32)
  target_link_libraries(${PLUGIN_NAME} PRIVATE
    uiautomationcore
    oleacc
    ole32
    user32
    ws2_32
    winhttp
  )
else()
  # asio 文件 I/O 在 -DASIO_HAS_IO_URING 下引用 io_uring 符号; 插件为 MODULE
  # 动态库, 不依赖宿主进程导出 (宿主为静态链接时符号可能不存在), 自链接
  # NO_DEFAULT_PATH: 只查找本项目 liburing 安装的库 (AGENTXX_INSTALL_DIR/lib),
  # 避免 fallback 到系统 liburing
  find_library(AGENTXX_URING_LIB uring PATHS "${AGENTXX_INSTALL_DIR}/lib" NO_DEFAULT_PATH)
  if (AGENTXX_URING_LIB)
    target_link_libraries(${PLUGIN_NAME} PRIVATE ${AGENTXX_URING_LIB})
  endif()
endif()

set_target_properties(${PLUGIN_NAME} PROPERTIES
  PREFIX "lib"
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED OFF
  RUNTIME_OUTPUT_DIRECTORY "${AGENTXX_EXEC_INSTALL_PREFIX}/plugins/${PLUGIN_NAME}/"
  LIBRARY_OUTPUT_DIRECTORY "${AGENTXX_EXEC_INSTALL_PREFIX}/plugins/${PLUGIN_NAME}/"
)
foreach(config DEBUG RELEASE MINSIZEREL RELWITHDEBINFO)
  # 统一复制到 exec 目录时直接将 动态库文件 放到 插件目录下，否则可能会放到 `exec/plugins/agentxx_xxx_plugin/Debug/libxxx.dll`
  # 修正后统一放到 `exec/plugins/plugin_xxx/libxxx.dll`
  set_target_properties(${PLUGIN_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_${config} "${AGENTXX_EXEC_INSTALL_PREFIX}/plugins/${PLUGIN_NAME}/"
    LIBRARY_OUTPUT_DIRECTORY_${config} "${AGENTXX_EXEC_INSTALL_PREFIX}/plugins/${PLUGIN_NAME}/"
  )
endforeach()
add_custom_command(TARGET ${PLUGIN_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin.yaml"
    "${AGENTXX_EXEC_INSTALL_PREFIX}/plugins/${PLUGIN_NAME}/plugin.yaml"
  COMMENT "Copy ${PLUGIN_NAME} plugin resources to ${AGENTXX_EXEC_INSTALL_PREFIX}/plugins/${PLUGIN_NAME}"
)
