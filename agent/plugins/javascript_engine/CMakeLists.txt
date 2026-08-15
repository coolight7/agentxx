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

add_library(agentxx_javascript_engine MODULE
  javascript_engine.cpp
)
target_include_directories(agentxx_javascript_engine PRIVATE
  "${AGENTXX_LIB_INCLUDE_DIR}"
  "${QUICKJS_INCLUDE_DIR}"
)
target_link_libraries(agentxx_javascript_engine PRIVATE
  "${QUICKJS_LIBRARY}"
)
set_target_properties(agentxx_javascript_engine PROPERTIES
  PREFIX "lib"
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED OFF
  RUNTIME_OUTPUT_DIRECTORY "${AGENTXX_EXEC_INSTALL_PREFIX}/"
  LIBRARY_OUTPUT_DIRECTORY "${AGENTXX_EXEC_INSTALL_PREFIX}/"
)
