# agentxx_codegraph —— CodeGraph 代码分析插件
# - 独立动态库 (MODULE), 链接 codegraph_core + tree_sitter 系 + sqlite3/simdjson/glob
# - 插件不链接 libagentxx (纯 C ABI 头), 仅经宿主 vtable 访问配置/注册工具
# - 仅当 AGENTXX_ENABLE_PLUGIN_CODEGRAPH 时由 plugins/CMakeLists.txt 添加本子目录
# - 输出到 ${AGENTXX_EXEC_INSTALL_PREFIX}/plugins/agentxx_codegraph/
#   (与 agentxx_test/agentxx_cli 同目录的 plugins/ 下, 供运行期加载)

if (NOT AGENTXX_ENABLE_PLUGIN_CODEGRAPH)
  message(FATAL_ERROR "[agentxx_plugins] agentxx_codegraph requires AGENTXX_ENABLE_PLUGIN_CODEGRAPH=ON")
endif()

set(PLUGIN_NAME agentxx_codegraph)
add_library(${PLUGIN_NAME} SHARED
  agentxx_codegraph.cpp
  codegraph_manager.cpp
)
# 依赖 (与 lib/CMakeLists.txt 一致; fmt/Glob 经 find_package 提供目标)
find_package(fmt REQUIRED)
find_package(Glob REQUIRED)

target_include_directories(${PLUGIN_NAME} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"          # codegraph_manager.h / codegraph_plugin.h
  "${AGENTXX_LIB_INCLUDE_DIR}"           # agentxx/plugin/plugin_api.h
  "${AGENTXX_INSTALL_DIR}/include"       # codegraph/ fmt/ simdjson.h sqlite3.h
  "${AGENTXX_INSTALL_DIR}/include/Glob-1.0" # glob/glob.h
  "${AGENTXX_INSTALL_DIR}/include/uchardet"
)
target_compile_definitions(${PLUGIN_NAME} PRIVATE
  AGENTXX_ENABLE_PLUGIN_CODEGRAPH=1
)

# codegraph_core 为静态库, 需连带其传递依赖 (与 lib/CMakeLists.txt
# _AGENTXX_CODEGRAPH_LIBS 一致 + sqlite3/simdjson/glob/uchardet/fmt)
target_link_libraries(${PLUGIN_NAME} PRIVATE
  codegraph_core
  tree_sitter
  ts_cpp ts_python ts_javascript ts_dart ts_typescript ts_tsx
  ts_markdown ts_markdown_inline ts_rust ts_bash ts_go ts_java
  ts_kotlin ts_php ts_swift ts_objc ts_csharp ts_sql ts_lua
  ts_yaml ts_json ts_xml ts_html ts_css ts_zig ts_ruby ts_glsl
  sqlite3
  simdjson
  Glob
  uchardet
  fmt::fmt
)
# pthread/dl/m 为 Linux 系统库; Windows/MSVC 无对应导入库 (std::thread 原生), 条件链接
if (UNIX)
  target_link_libraries(${PLUGIN_NAME} PRIVATE pthread dl m)
endif()
target_link_directories(${PLUGIN_NAME} PRIVATE
  "${AGENTXX_INSTALL_DIR}/lib"
  "${AGENTXX_INSTALL_DIR}/lib/Glob-1.0"
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
