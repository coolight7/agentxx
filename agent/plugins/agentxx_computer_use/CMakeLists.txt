# agentxx_computer_use —— 计算机控制插件 (Windows)
# - 独立动态库 (MODULE), 仅依赖纯 C ABI 头 + codegraph::Json (simdjson)
# - 提供: agentxx_ui_control_keyboard_mouse
# - 屏幕捕获已拆分为独立插件 agentxx_screen_capture (见 plugin.yaml depends,
#   加载时须先于本插件, 拓扑排序自动处理)
set(PLUGIN_NAME agentxx_computer_use)
add_library(${PLUGIN_NAME} MODULE
  agentxx_computer_use.cpp
  ui_control.cpp
)
target_include_directories(${PLUGIN_NAME} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"          # computer_use_plugin.h
  "${AGENTXX_LIB_INCLUDE_DIR}"           # agentxx/plugin/plugin_api.h
  "${AGENTXX_INSTALL_DIR}/include"       # codegraph/core/json.hpp simdjson.h fmt
)
# codegraph::Json 为 header-only (simdjson 单头), 链接 simdjson 库
find_package(fmt REQUIRED)
target_link_libraries(${PLUGIN_NAME} PRIVATE
  simdjson
  fmt::fmt
)
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
