# agentxx_computer_use —— 计算机控制插件 (Windows)
# - 默认: 独立动态库 (MODULE), 仅依赖纯 C ABI 头 + codegraph::Json (simdjson)
# - 内置模式 (AGENTXX_ENABLE_PLUGIN_BUILTIN=ON): 源文件编译为 OBJECT 库合并进
#   libagentxx; simdjson/fmt 为 libagentxx 既有依赖, 无需补链接
# - 提供: agentxx_ui_control_keyboard_mouse
# - 屏幕捕获已拆分为独立插件 agentxx_screen_capture (见 plugin.yaml depends,
#   加载时须先于本插件, 拓扑排序自动处理; 内置模式同样适用)
set(PLUGIN_NAME agentxx_computer_use)

if (AGENTXX_ENABLE_PLUGIN_BUILTIN)
  # ===== 内置合并编译分支 =====
  set(_target abp_${PLUGIN_NAME})
  add_library(${_target} OBJECT
    ${CMAKE_CURRENT_SOURCE_DIR}/agentxx_computer_use.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ui_control.cpp
  )
  target_compile_definitions(${_target} PRIVATE
    "AGENTXX_PLUGIN_BUILTIN=1"
    "agentxx_plugin_entry=agentxx_plugin_builtin_entry_${PLUGIN_NAME}"
    "agentxx_plugin_get_info=agentxx_plugin_builtin_get_info_${PLUGIN_NAME}"
    "agentxx_plugin_unload=agentxx_plugin_builtin_unload_${PLUGIN_NAME}"
  )
  target_include_directories(${_target} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"          # computer_use_plugin.h
    "${AGENTXX_LIB_INCLUDE_DIR}"           # agentxx/plugin/plugin_api.h
    "${AGENTXX_INSTALL_DIR}/include"       # codegraph/core/json.hpp simdjson.h fmt
  )
  # 复用 agentxx_util 工具库 (libagentxx 已链接本库; 此处经 PRIVATE 链接仅
  # 传递编译宏/头路径: util 头文件依赖 XX_IS_* 宏与 include 路径)
  target_link_libraries(${_target} PRIVATE agentxx_util)
  if (WIN32)
    # ui_control: 鼠标/键盘注入与光标查询 (user32)
    # - 独立模式经下方独立分支 PRIVATE 链接; 内置模式经本收集通道并入 libagentxx
    set(AGENTXX_BUILTIN_PLUGIN_LINK_LIBS
      "${AGENTXX_BUILTIN_PLUGIN_LINK_LIBS}" user32 PARENT_SCOPE)
  endif()
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
  set(AGENTXX_BUILTIN_PLUGIN_OBJECT_TARGETS "${AGENTXX_BUILTIN_PLUGIN_OBJECT_TARGETS}" "${_target}" PARENT_SCOPE)
  set(AGENTXX_BUILTIN_MANIFEST_DECLS "${AGENTXX_BUILTIN_MANIFEST_DECLS}" PARENT_SCOPE)
  set(AGENTXX_BUILTIN_MANIFEST_ENTRIES "${AGENTXX_BUILTIN_MANIFEST_ENTRIES}" PARENT_SCOPE)
  # 资源: plugin.yaml (运行期目录配置推导 depends/入口)
  set(_res "${AGENTXX_EXEC_INSTALL_PREFIX}/plugins/${PLUGIN_NAME}/plugin.yaml")
  add_custom_command(OUTPUT "${_res}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/plugin.yaml" "${_res}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/plugin.yaml"
    COMMENT "Copy ${PLUGIN_NAME} plugin resource (builtin mode)")
  set(AGENTXX_BUILTIN_RESOURCE_OUTPUTS "${AGENTXX_BUILTIN_RESOURCE_OUTPUTS}" "${_res}" PARENT_SCOPE)
  return()
endif()

# ===== 独立动态库编译 (默认) =====
add_library(${PLUGIN_NAME} SHARED
  agentxx_computer_use.cpp
  ui_control.cpp
)
target_include_directories(${PLUGIN_NAME} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"          # computer_use_plugin.h
  "${AGENTXX_LIB_INCLUDE_DIR}"           # agentxx/plugin/plugin_api.h
  "${AGENTXX_INSTALL_DIR}/include"       # codegraph/core/json.hpp simdjson.h fmt
)
# codegraph::Json 为 header-only (simdjson 单头), 链接 simdjson 库
# 复用 agentxx_util 工具库 (插件静态链接, 与宿主副本符号互不冲突;
# find_package 经 AGENTXX_INSTALL_DIR 的 agentxx_util config 解析,
# 依赖链 fmt/SQLite3/uchardet/Iconv 由 config 内 find_dependency 传递)
find_package(agentxx_util REQUIRED)
find_package(fmt REQUIRED)
target_link_libraries(${PLUGIN_NAME} PRIVATE
  agentxx_util
  simdjson
  fmt::fmt
)
if (WIN32)
  # ui_control: 鼠标/键盘注入与光标查询 (user32)
  target_link_libraries(${PLUGIN_NAME} PRIVATE user32)
endif()
target_link_directories(${PLUGIN_NAME} PRIVATE
  "${AGENTXX_INSTALL_DIR}/lib"
)

set_target_properties(${PLUGIN_NAME} PROPERTIES
  PREFIX "lib"
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED OFF
  RUNTIME_OUTPUT_DIRECTORY "${AGENTXX_EXEC_INSTALL_PREFIX}/plugins/${PLUGIN_NAME}/"
  LIBRARY_OUTPUT_DIRECTORY "${AGENTXX_EXEC_INSTALL_PREFIX}/plugins/${PLUGIN_NAME}/"
)
add_custom_command(TARGET ${PLUGIN_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin.yaml"
    "${AGENTXX_EXEC_INSTALL_PREFIX}/plugins/${PLUGIN_NAME}/plugin.yaml"
  COMMENT "Copy ${PLUGIN_NAME} plugin resources to ${AGENTXX_EXEC_INSTALL_PREFIX}/plugins/${PLUGIN_NAME}"
)
