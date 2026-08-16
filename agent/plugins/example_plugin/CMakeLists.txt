# 原生示例插件 (插件系统测试/演示用, 见 test/core/test_plugins.cpp)
# - 编译为 MODULE 动态库 (不链接 libagentxx, 仅依赖纯 C ABI 头)
# - 输出到 ${AGENTXX_EXEC_INSTALL_PREFIX} (与 agentxx_test 同目录), 供 test_plugins 加载
set(PLUGIN_NAME example_plugin)
add_library(${PLUGIN_NAME} MODULE
  example_plugin.cpp
)
target_include_directories(${PLUGIN_NAME} PRIVATE
  "${AGENTXX_LIB_INCLUDE_DIR}"
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