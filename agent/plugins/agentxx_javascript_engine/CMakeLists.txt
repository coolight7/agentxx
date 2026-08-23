# JS 解释器插件 (agentxx_javascript_engine)
# - 默认: 独立动态库, 链接 QuickJS (quickjs_repo install 的 libqjs.a)
# - 内置模式 (AGENTXX_ENABLE_PLUGIN_BUILTIN=ON): 源文件编译为 OBJECT 库合并进
#   libagentxx, quickjs 库由 libagentxx 一并链接 (顶层 superbuild 会先构建
#   quickjs_repo)
set(PLUGIN_NAME agentxx_javascript_engine)

# quickjs 依赖查找 (两种模式共用; quickjs-ng 的库目标名为 qjs → libqjs.a)
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

find_package(agentxx_util REQUIRED)

if (AGENTXX_ENABLE_PLUGIN_BUILTIN)
  # ===== 内置合并编译分支 =====
  set(_target abp_${PLUGIN_NAME})
  add_library(${_target} OBJECT
    ${CMAKE_CURRENT_SOURCE_DIR}/agentxx_javascript_engine.cpp
  )
  target_compile_definitions(${_target} PRIVATE
    "AGENTXX_PLUGIN_BUILTIN=1"
    "agentxx_plugin_entry=agentxx_plugin_builtin_entry_${PLUGIN_NAME}"
    "agentxx_plugin_get_info=agentxx_plugin_builtin_get_info_${PLUGIN_NAME}"
    "agentxx_plugin_unload=agentxx_plugin_builtin_unload_${PLUGIN_NAME}"
  )
  target_include_directories(${_target} PRIVATE
    "${AGENTXX_LIB_INCLUDE_DIR}"
    "${QUICKJS_INCLUDE_DIR}"
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
  set(AGENTXX_BUILTIN_PLUGIN_OBJECT_TARGETS "${AGENTXX_BUILTIN_PLUGIN_OBJECT_TARGETS}" "${_target}" PARENT_SCOPE)
  # 登记进内置插件清单 (子目录自登记: 平台不支持时本文件开头已提前 return,
  # 不会误登记; 原由父目录 plugins/CMakeLists.txt 无条件追加, 现统一走此通道)
  list(APPEND AGENTXX_BUILTIN_PLUGIN_NAMES ${PLUGIN_NAME})
  set(AGENTXX_BUILTIN_PLUGIN_NAMES "${AGENTXX_BUILTIN_PLUGIN_NAMES}" PARENT_SCOPE)
  set(AGENTXX_BUILTIN_PLUGIN_LINK_LIBS "${AGENTXX_BUILTIN_PLUGIN_LINK_LIBS}" "${QUICKJS_LIBRARY}" PARENT_SCOPE)
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
# - 加载后可承载 type: js 脚本插件
# - 仅当 AGENTXX_ENABLE_PLUGIN_JS 时由 plugins/CMakeLists.txt 添加本子目录
add_library(${PLUGIN_NAME} SHARED
  agentxx_javascript_engine.cpp
)
target_include_directories(${PLUGIN_NAME} PRIVATE
  "${AGENTXX_LIB_INCLUDE_DIR}"
  "${QUICKJS_INCLUDE_DIR}"
)
target_link_libraries(${PLUGIN_NAME} PRIVATE
  agentxx_util
  "${QUICKJS_LIBRARY}"
)
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
