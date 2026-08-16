# 原生示例插件 (插件系统测试/演示用, 见 test/core/test_plugins.cpp)
# - 编译为 MODULE 动态库 (不链接 libagentxx, 仅依赖纯 C ABI 头)
# - 输出到 ${AGENTXX_EXEC_INSTALL_PREFIX} (与 agentxx_test 同目录), 供 test_plugins 加载
add_library(agentxx_plugin_example MODULE
  example_native.cpp
)
target_include_directories(agentxx_plugin_example PRIVATE
  "${AGENTXX_LIB_INCLUDE_DIR}"
)
set_target_properties(agentxx_plugin_example PROPERTIES
  PREFIX "lib"
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED OFF
  RUNTIME_OUTPUT_DIRECTORY "${AGENTXX_EXEC_INSTALL_PREFIX}/"
  LIBRARY_OUTPUT_DIRECTORY "${AGENTXX_EXEC_INSTALL_PREFIX}/"
)
