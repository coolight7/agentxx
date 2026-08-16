# JS 解释器插件 (agentxx_javascript_engine)
# - 独立动态库, 链接 QuickJS (quickjs_repo install 的 libqjs.a)
# - 加载后可承载 type: js 脚本插件
# - 仅当 AGENTXX_ENABLE_PLUGIN_JS 时由 plugins/CMakeLists.txt 添加本子目录
# - quickjs-ng 的库目标名为 qjs → install 为 libqjs.a
find_library(QUICKJS_LIBRARY qjs
  PATHS "${AGENTXX_INSTALL_DIR}/lib"
  NO_DEFAULT_PATH
)
find_path(QUICKJS_INCLUDE_DIR quickjs.h
  PATHS "${AGENTXX_INSTALL_DIR}/include"
  NO_DEFAULT_PATH
)
if (NOT QUICKJS_LIBRARY OR NOT QUICKJS_INCLUDE_DIR)
  message(FATAL_ERROR
    "[agentxx_plugins] QuickJS not found in ${AGENTXX_INSTALL_DIR} "
    "(need libqjs.a + quickjs.h). Build agentxx (quickjs_repo) first, "
    "or pass -DAGENTXX_INSTALL_DIR=<path>")
endif()

set(PLUGIN_NAME agentxx_javascript_engine)
add_library(${PLUGIN_NAME} MODULE
  agentxx_javascript_engine.cpp
)
target_include_directories(${PLUGIN_NAME} PRIVATE
  "${AGENTXX_LIB_INCLUDE_DIR}"
  "${QUICKJS_INCLUDE_DIR}"
)
target_link_libraries(${PLUGIN_NAME} PRIVATE
  "${QUICKJS_LIBRARY}"
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