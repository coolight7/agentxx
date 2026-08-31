# agentxx_math —— 数学计算工具插件 (表达式求值)
# - 默认: 独立动态库 (MODULE), 仅依赖纯 C ABI 头 + fmt + neograph(json)
# - 内置模式 (AGENTXX_ENABLE_PLUGIN_BUILTIN=ON 且本插件在 AGENTXX_PLUGIN_BUILTIN_LIST):
#   源文件编译为 OBJECT 库合并进 libagentxx
set(PLUGIN_NAME agentxx_math)

# ===== 平台支持判定 =====
# 纯 C++ 数学库, 全平台可用 (平台矩阵说明见 docs/agent/plugins.md 9.3.1)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/plugin_platform_support.cmake")
agentxx_plugin_platform_gate(${PLUGIN_NAME} _AGENTXX_PLATFORM_OK windows linux android macos ios)
if (NOT _AGENTXX_PLATFORM_OK)
  return() # 跳过本插件全部目标 (内置合并/独立动态库两模式均在此拦截)
endif ()

if (_AGENTXX_ADD_AS_BUILTIN)
  # ===== 内置合并编译分支 =====
  set(_target abp_${PLUGIN_NAME})
  add_library(${_target} OBJECT
    ${CMAKE_CURRENT_SOURCE_DIR}/agentxx_math.cpp
  )
  target_compile_definitions(${_target} PRIVATE
    "AGENTXX_PLUGIN_BUILTIN=1"
    "agentxx_plugin_agent_create=agentxx_plugin_agent_builtin_create_${PLUGIN_NAME}"
    "agentxx_plugin_agent_get_info=agentxx_plugin_agent_builtin_get_info_${PLUGIN_NAME}"
    "agentxx_plugin_agent_destroy=agentxx_plugin_agent_builtin_destroy_${PLUGIN_NAME}"
  )
  target_include_directories(${_target} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"          # agentxx_math_plugin.h / math_impl.h
    "${AGENTXX_LIB_INCLUDE_DIR}"           # agentxx/plugin/plugin_api.h
    "${AGENTXX_INSTALL_DIR}/include"       # fmt neograph
  )
  target_link_libraries(${_target} PRIVATE agentxx_util)
  set_target_properties(${_target} PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED OFF
  )
  set(AGENTXX_BUILTIN_MANIFEST_DECLS
    "${AGENTXX_BUILTIN_MANIFEST_DECLS}
extern \"C\" const AgentxxPluginInfo* agentxx_plugin_agent_builtin_get_info_${PLUGIN_NAME}(void);
extern \"C\" int agentxx_plugin_agent_builtin_create_${PLUGIN_NAME}(const AgentxxHost* host, void** plugin_ctx);
extern \"C\" void agentxx_plugin_agent_builtin_destroy_${PLUGIN_NAME}(void* plugin_ctx);")
  set(AGENTXX_BUILTIN_MANIFEST_ENTRIES
    "${AGENTXX_BUILTIN_MANIFEST_ENTRIES}
    {\"${PLUGIN_NAME}\", agentxx_plugin_agent_builtin_get_info_${PLUGIN_NAME}, agentxx_plugin_agent_builtin_create_${PLUGIN_NAME}, agentxx_plugin_agent_builtin_destroy_${PLUGIN_NAME}},")
  set(AGENTXX_BUILTIN_PLUGIN_OBJECT_TARGETS "${AGENTXX_BUILTIN_PLUGIN_OBJECT_TARGETS}" "${_target}" PARENT_SCOPE)
  list(APPEND AGENTXX_BUILTIN_PLUGIN_NAMES ${PLUGIN_NAME})
  set(AGENTXX_BUILTIN_PLUGIN_NAMES "${AGENTXX_BUILTIN_PLUGIN_NAMES}" PARENT_SCOPE)
  set(AGENTXX_BUILTIN_MANIFEST_DECLS "${AGENTXX_BUILTIN_MANIFEST_DECLS}" PARENT_SCOPE)
  set(AGENTXX_BUILTIN_MANIFEST_ENTRIES "${AGENTXX_BUILTIN_MANIFEST_ENTRIES}" PARENT_SCOPE)
  set(_res "${AGENTXX_EXEC_INSTALL_PREFIX}/plugins/${PLUGIN_NAME}/plugin.yaml")
  # 资源拷贝
  add_custom_target(${PLUGIN_NAME}_builtin_resources ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/plugin.yaml" "${_res}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/plugin.yaml"
    COMMENT "Copy ${PLUGIN_NAME} plugin resource (builtin mode)")
  return()
endif()

# ===== 独立动态库编译 (默认) =====
add_library(${PLUGIN_NAME} SHARED
  agentxx_math.cpp
)

# ===== 依赖统一查找与链接 =====
if (NOT TARGET agentxx_util)
  find_package(agentxx_util REQUIRED)
endif()
find_package(html2md REQUIRED)
find_package(OpenSSL REQUIRED)

target_include_directories(${PLUGIN_NAME} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"          # agentxx_math_plugin.h / math_impl.h
  "${AGENTXX_LIB_INCLUDE_DIR}"           # agentxx/plugin/plugin_api.h
  "${AGENTXX_INSTALL_DIR}/include/"      # fmt neograph
)

# 裸库名依赖解析目录
target_link_directories(${PLUGIN_NAME} PRIVATE
  "${AGENTXX_INSTALL_DIR}/lib/"
  "${OPENSSL_ROOT_DIR}/lib64"
  "${OPENSSL_ROOT_DIR}/lib"
)

target_link_libraries(${PLUGIN_NAME} PRIVATE
  agentxx_util
  html2md
  OpenSSL::SSL
  OpenSSL::Crypto
  # === neograph vvv 链接顺序是必要的 (与 lib 自身一致) ===
  neograph_sqlite
  neograph_acp
  neograph_mcp
  neograph_mcp_types
  neograph_async
  neograph_llm
  neograph_core
  # === neograph ^^^
  yyjson
)
if (AGENTXX_ENABLE_HYPERSCAN)
  target_link_libraries(${PLUGIN_NAME} PRIVATE PkgConfig::hyperscan hs_runtime)
endif()
if (AGENTXX_LINUX_IO_URING_SUPPORTED)
  target_link_libraries(${PLUGIN_NAME} PRIVATE PkgConfig::uring)
endif()
if (NOT WIN32)
  set(_tail_libs crypto ssl)
  if (AGENTXX_ENABLE_HYPERSCAN)
    list(APPEND _tail_libs hs hs_runtime)
  endif()
  if (AGENTXX_LINUX_IO_URING_SUPPORTED)
    list(APPEND _tail_libs uring)
  endif()
  list(APPEND _tail_libs
    neograph_sqlite neograph_acp neograph_mcp neograph_mcp_types neograph_async neograph_llm neograph_core yyjson)
  target_link_libraries(${PLUGIN_NAME} PRIVATE
    "-Wl,--start-group"
    ${_tail_libs}
    "-Wl,--end-group"
  )
  unset(_tail_libs)
endif()

set_target_properties(${PLUGIN_NAME} PROPERTIES
  PREFIX "lib"
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED OFF
  RUNTIME_OUTPUT_DIRECTORY "${AGENTXX_EXEC_INSTALL_PREFIX}/plugins/${PLUGIN_NAME}/"
  LIBRARY_OUTPUT_DIRECTORY "${AGENTXX_EXEC_INSTALL_PREFIX}/plugins/${PLUGIN_NAME}/"
)
foreach(config DEBUG RELEASE MINSIZEREL RELWITHDEBINFO)
  set_target_properties(${PLUGIN_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_${config} "${AGENTXX_EXEC_INSTALL_PREFIX}/plugins/${PLUGIN_NAME}/"
    LIBRARY_OUTPUT_DIRECTORY_${config} "${AGENTXX_EXEC_INSTALL_PREFIX}/plugins/${PLUGIN_NAME}/"
  )
endforeach()

# plugin.yaml 拷贝到输出目录 (动态库旁)
set(_yaml_in "${CMAKE_CURRENT_SOURCE_DIR}/plugin.yaml")
set(_yaml_out "${AGENTXX_EXEC_INSTALL_PREFIX}/plugins/${PLUGIN_NAME}/plugin.yaml")
add_custom_command(TARGET ${PLUGIN_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_yaml_in}" "${_yaml_out}"
  COMMENT "Copy ${PLUGIN_NAME} plugin.yaml to output directory"
)
