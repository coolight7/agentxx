# JS 示例插件 (统一模型: C++ 壳 + 脚本资源)
# - libexample_js.so: C++ 壳 (MODULE, 经能力调用委派脚本给引擎)
# - plugin.yaml + plugin.js 拷贝到同目录 (壳推导同目录 plugin.js)
# - 仅当 AGENTXX_ENABLE_PLUGIN_JS 时由 plugins/CMakeLists.txt 添加本子目录
add_library(example_js MODULE
  libexample_js.cpp
)
target_include_directories(example_js PRIVATE
  "${AGENTXX_LIB_INCLUDE_DIR}"
)
set_target_properties(example_js PROPERTIES
  PREFIX "lib"
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED OFF
  RUNTIME_OUTPUT_DIRECTORY "${AGENTXX_EXEC_INSTALL_PREFIX}/plugins/example_js/"
  LIBRARY_OUTPUT_DIRECTORY "${AGENTXX_EXEC_INSTALL_PREFIX}/plugins/example_js/"
)
add_custom_command(TARGET example_js POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin.yaml"
    "${AGENTXX_EXEC_INSTALL_PREFIX}/plugins/example_js/plugin.yaml"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin.js"
    "${AGENTXX_EXEC_INSTALL_PREFIX}/plugins/example_js/plugin.js"
  COMMENT "Copy example_js plugin resources to ${AGENTXX_EXEC_INSTALL_PREFIX}/plugins/example_js"
)
