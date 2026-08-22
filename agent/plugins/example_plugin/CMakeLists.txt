# 原生示例插件 (插件系统测试/演示用, 见 test/core/test_plugins.cpp)
# - 默认: 编译为 MODULE 动态库 (不链接 libagentxx, 仅依赖纯 C ABI 头)
# - 内置模式 (AGENTXX_ENABLE_PLUGIN_BUILTIN=ON, 由 libagentxx 合并编译):
#   源文件编译为独立 OBJECT 库 (per-plugin 编译定义/包含路径互不影响),
#   目标文件由 libagentxx 合并进共享/静态库; 入口符号经编译定义改名避免
#   多插件合并时符号冲突 (纯编译期 -D, 不改插件源码)
set(PLUGIN_NAME example_plugin)

if (AGENTXX_ENABLE_PLUGIN_BUILTIN)
  # ===== 内置合并编译分支 =====
  set(_target abp_${PLUGIN_NAME})
  add_library(${_target} OBJECT
    ${CMAKE_CURRENT_SOURCE_DIR}/example_plugin.cpp
  )
  # 入口符号改名 (agentxx_plugin_entry → agentxx_plugin_builtin_entry_<插件名>;
  # 含 client 入口, 避免多插件合并编译时符号冲突; 与 plugins/CMakeLists.txt
  # 生成的 builtin_plugins.cpp 清单引用一致)
  target_compile_definitions(${_target} PRIVATE
    "AGENTXX_PLUGIN_BUILTIN=1"
    "agentxx_plugin_entry=agentxx_plugin_builtin_entry_${PLUGIN_NAME}"
    "agentxx_plugin_get_info=agentxx_plugin_builtin_get_info_${PLUGIN_NAME}"
    "agentxx_plugin_unload=agentxx_plugin_builtin_unload_${PLUGIN_NAME}"
    "agentxx_client_entry=agentxx_plugin_builtin_client_entry_${PLUGIN_NAME}"
    "agentxx_client_get_info=agentxx_plugin_builtin_client_get_info_${PLUGIN_NAME}"
    "agentxx_client_unload=agentxx_plugin_builtin_client_unload_${PLUGIN_NAME}"
  )
  target_include_directories(${_target} PRIVATE
    "${AGENTXX_LIB_INCLUDE_DIR}"           # agentxx/plugin/plugin_api.h
    "${AGENTXX_INSTALL_DIR}/include"       # fmt
  )
set_target_properties(${_target} PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED OFF
  )
  # 内置清单片段: 入口符号声明 + 表项 (由 plugins/CMakeLists.txt 汇总生成)
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
  # 资源: plugin.yaml (运行期目录配置推导 depends/入口; 入口缺失时回退内置)
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
# - 输出到 ${AGENTXX_EXEC_INSTALL_PREFIX} (与 agentxx_test 同目录), 供 test_plugins 加载
# - 导出符号限制: 仅 AGENTXX_PLUGIN_EXPORT 标记的入口符号导出
#   (plugins/CMakeLists.txt 统一配置 version script / -fvisibility=hidden /
#   MSVC 不自动导出; 此处不再设置 WINDOWS_EXPORT_ALL_SYMBOLS)
add_library(${PLUGIN_NAME} SHARED
  example_plugin.cpp
)
target_include_directories(${PLUGIN_NAME} PRIVATE
  "${AGENTXX_LIB_INCLUDE_DIR}"           # agentxx/plugin/plugin_api.h
  "${AGENTXX_INSTALL_DIR}/include"       # fmt
)
find_package(fmt REQUIRED)
target_link_libraries(${PLUGIN_NAME} PRIVATE
  fmt::fmt
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
