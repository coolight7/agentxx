# agentxx_websearch —— 网络访问工具插件 (search / fetch / fetch markdown)
# - 默认: 独立动态库 (MODULE), 仅依赖纯 C ABI 头 + cxx_utilxx_static + html2md
#   (HttpClient 的 fetchMarkdown 内部使用 html2md 转换)
# - 内置模式:
#   源文件编译为 OBJECT 库合并进 libagentxx
set(PLUGIN_NAME agentxx_websearch)

# ===== 平台支持判定 =====
# 纯网络 IO (asio/beast 经 cxx_utilxx_static), 全平台可用
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/plugin_platform_support.cmake")
agentxx_plugin_platform_gate(${PLUGIN_NAME} _AGENTXX_PLATFORM_OK windows linux macos android ios)
if (NOT _AGENTXX_PLATFORM_OK)
  return()
endif ()

if (_AGENTXX_ADD_AS_BUILTIN)
  # ===== 内置合并编译分支 =====
  set(_target abp_${PLUGIN_NAME})
  add_library(${_target} OBJECT
    ${CMAKE_CURRENT_SOURCE_DIR}/agentxx_websearch.cpp
  )
  target_compile_definitions(${_target} PRIVATE
    "AGENTXX_PLUGIN_BUILTIN=1"
    "agentxx_plugin_agent_create=agentxx_plugin_agent_builtin_create_${PLUGIN_NAME}"
    "agentxx_plugin_agent_get_info=agentxx_plugin_agent_builtin_get_info_${PLUGIN_NAME}"
    "agentxx_plugin_agent_destroy=agentxx_plugin_agent_builtin_destroy_${PLUGIN_NAME}"
    "agentxx_plugin_agent_start=agentxx_plugin_agent_builtin_start_${PLUGIN_NAME}"
    "agentxx_plugin_agent_stop=agentxx_plugin_agent_builtin_stop_${PLUGIN_NAME}"
    "agentxx_plugin_client_create=agentxx_plugin_client_builtin_create_${PLUGIN_NAME}"
    "agentxx_plugin_client_get_info=agentxx_plugin_client_builtin_get_info_${PLUGIN_NAME}"
    "agentxx_plugin_client_destroy=agentxx_plugin_client_builtin_destroy_${PLUGIN_NAME}"
    "agentxx_plugin_client_start=agentxx_plugin_client_builtin_start_${PLUGIN_NAME}"
    "agentxx_plugin_client_stop=agentxx_plugin_client_builtin_stop_${PLUGIN_NAME}"
  )
  target_include_directories(${_target} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"          # agentxx_websearch_plugin.h / websearch_impl.h
    "${AGENTXX_LIB_INCLUDE_DIR}"           # agentxx/plugin/plugin_api.h
    "${AGENTXX_INSTALL_DIR}/include"       # cxx_utilxx_static (Json/JsonView) html2md
  )
  # 复用 cxx_utilxx_static 工具库 (HttpClient 实现经静态裁剪自动收录);
  # html2md 以裸库名追加进 libagentxx 收集通道 (与 agentxx_string 同理)
  target_link_libraries(${_target} PRIVATE cxx_utilxx_static)
  set(AGENTXX_BUILTIN_PLUGIN_LINK_LIBS
    "${AGENTXX_BUILTIN_PLUGIN_LINK_LIBS}" html2md PARENT_SCOPE)
  set_target_properties(${_target} PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED OFF
  )
  set(AGENTXX_BUILTIN_PLUGIN_OBJECT_TARGETS "${AGENTXX_BUILTIN_PLUGIN_OBJECT_TARGETS}" "${_target}" PARENT_SCOPE)
  list(APPEND AGENTXX_BUILTIN_PLUGIN_NAMES ${PLUGIN_NAME})
  set(AGENTXX_BUILTIN_PLUGIN_NAMES "${AGENTXX_BUILTIN_PLUGIN_NAMES}" PARENT_SCOPE)
  set(_res "${AGENTXX_EXEC_INSTALL_PREFIX}/plugins/${PLUGIN_NAME}/plugin.yaml")
  # 资源拷贝 (自包含 ALL 目标: 跨目录 OUTPUT 不进构建图, 见 plugins.md;
  # ALL 保证随 libagentxx 构建执行; 仅内置合并分支存在本目标)
  add_custom_target(${PLUGIN_NAME}_builtin_resources ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/plugin.yaml" "${_res}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/plugin.yaml"
    COMMENT "Copy ${PLUGIN_NAME} plugin resource (builtin mode)")
  return()
endif()

# ===== 独立动态库编译 (默认) =====
add_library(${PLUGIN_NAME} SHARED
  agentxx_websearch.cpp
)

# ===== 依赖查找与链接 (顺序: 先依赖库的依赖, 再依赖库本体) =====
# 工具库条件依赖 (io_uring / hyperscan) 由 plugins/CMakeLists.txt 统一查找;
# 本处只导入依赖库本体, 随后按 imported target / 裸库名链接
if (NOT TARGET cxx_utilxx_static)
  # 内置合并编译时 util 目标随 libagentxx 已存在 (且 install 目录尚未生成
  # config), 仅独立构建本插件时经安装 config 导入
  find_package(cxx_utilxx REQUIRED)
endif()
find_package(html2md REQUIRED)

target_include_directories(${PLUGIN_NAME} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"          # agentxx_websearch_plugin.h / websearch_impl.h
  "${AGENTXX_LIB_INCLUDE_DIR}"           # agentxx/plugin/plugin_api.h
  "${AGENTXX_INSTALL_DIR}/include/"      # cxx_utilxx_static (Json/JsonView) html2md
)

# 裸库名依赖 (如 hs_runtime) 的解析目录: 与 cxx_utilxx* 导出接口的 link 目录一致
target_link_directories(${PLUGIN_NAME} PRIVATE
  "${AGENTXX_INSTALL_DIR}/lib/"
)

target_link_libraries(${PLUGIN_NAME} PRIVATE
  cxx_utilxx_static
  html2md
)
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
add_custom_command(TARGET ${PLUGIN_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin.yaml"
    "${AGENTXX_EXEC_INSTALL_PREFIX}/plugins/${PLUGIN_NAME}/plugin.yaml"
  COMMENT "Copy ${PLUGIN_NAME} plugin resources to ${AGENTXX_EXEC_INSTALL_PREFIX}/plugins/${PLUGIN_NAME}"
)
