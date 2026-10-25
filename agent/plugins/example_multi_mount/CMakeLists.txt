# 多挂载示例插件 (一次加载、多个 agent 各挂一份状态; 见 test/plugin/test_plugin_multi_mount.cpp)
# - 默认: 编译为 MODULE 动态库 (不链接 libagentxx, 仅依赖纯 C ABI 头)
# - 内置模式: 源文件编译为独立 OBJECT 库, 目标文件由 libagentxx 合并进共享/静态库;
#   入口符号经编译定义改名避免多插件合并时符号冲突 (纯编译期 -D, 不改插件源码)
set(PLUGIN_NAME example_multi_mount)

set(_BOOST_INCLUDES "${BOOST_ROOT}/include/" "${BOOST_ROOT}/include/boost/")

if (_AGENTXX_ADD_AS_BUILTIN)
  # ===== 内置合并编译分支 =====
  set(_target abp_${PLUGIN_NAME})
  add_library(${_target} OBJECT
    ${CMAKE_CURRENT_SOURCE_DIR}/example_multi_mount.cpp
  )
  # 入口符号改名 (与 plugins/CMakeLists.txt 生成的 builtin_plugins.cpp 清单引用一致)
  target_compile_definitions(${_target} PRIVATE
    "AGENTXX_PLUGIN_BUILTIN=1"
    "agentxx_plugin_agent_create=agentxx_plugin_agent_builtin_create_${PLUGIN_NAME}"
    "agentxx_plugin_agent_get_info=agentxx_plugin_agent_builtin_get_info_${PLUGIN_NAME}"
    "agentxx_plugin_agent_destroy=agentxx_plugin_agent_builtin_destroy_${PLUGIN_NAME}"
    "agentxx_plugin_agent_start=agentxx_plugin_agent_builtin_start_${PLUGIN_NAME}"
    "agentxx_plugin_agent_stop=agentxx_plugin_agent_builtin_stop_${PLUGIN_NAME}"
    "agentxx_plugin_agent_mount=agentxx_plugin_agent_builtin_mount_${PLUGIN_NAME}"
    "agentxx_plugin_agent_unmount=agentxx_plugin_agent_builtin_unmount_${PLUGIN_NAME}"
  )
  target_include_directories(${_target} PRIVATE
    "${AGENTXX_LIB_INCLUDE_DIR}"           # agentxx/plugin/plugin_api.h
    "${AGENTXX_INSTALL_DIR}/include"       # fmt
    ${_BOOST_INCLUDES}
  )
  set_target_properties(${_target} PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED OFF
  )
  set(AGENTXX_BUILTIN_PLUGIN_OBJECT_TARGETS "${AGENTXX_BUILTIN_PLUGIN_OBJECT_TARGETS}" "${_target}" PARENT_SCOPE)
  list(APPEND AGENTXX_BUILTIN_PLUGIN_NAMES ${PLUGIN_NAME})
  set(AGENTXX_BUILTIN_PLUGIN_NAMES "${AGENTXX_BUILTIN_PLUGIN_NAMES}" PARENT_SCOPE)
  # 资源: plugin.yaml (运行期目录配置推导 depends/入口; 入口缺失时回退内置)
  set(_res "${AGENTXX_EXEC_INSTALL_PREFIX}/plugins/${PLUGIN_NAME}/plugin.yaml")
  add_custom_target(${PLUGIN_NAME}_builtin_resources ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/plugin.yaml" "${_res}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/plugin.yaml"
    COMMENT "Copy ${PLUGIN_NAME} plugin resource (builtin mode)")
  return()
endif()

# ===== 独立动态库编译 (默认) =====
add_library(${PLUGIN_NAME} SHARED
  example_multi_mount.cpp
)

if (NOT TARGET cxx_utilxx_base_static)
  # 内置合并编译时 util 目标随 libagentxx 已存在 (且 install 目录尚未生成
  # config), 仅独立构建本插件时经安装 config 导入
  find_package(cxx_utilxx_base REQUIRED)
endif()

target_include_directories(${PLUGIN_NAME} PRIVATE
  "${AGENTXX_LIB_INCLUDE_DIR}"           # agentxx/plugin/plugin_api.h
  "${AGENTXX_INSTALL_DIR}/include"       # fmt
  ${_BOOST_INCLUDES}
)
target_link_libraries(${PLUGIN_NAME} PRIVATE
  cxx_utilxx_base_static
)
set_target_properties(${PLUGIN_NAME} PROPERTIES
  PREFIX "lib"
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED OFF
  RUNTIME_OUTPUT_DIRECTORY "${AGENTXX_EXEC_INSTALL_PREFIX}/plugins/${PLUGIN_NAME}/"
  LIBRARY_OUTPUT_DIRECTORY "${AGENTXX_EXEC_INSTALL_PREFIX}/plugins/${PLUGIN_NAME}/"
)
foreach(config DEBUG RELEASE MINSIZEREL RELWITHDEBINFO)
  # 多配置生成器会把产物再套一层 Debug/ 目录, 这里逐配置钉死到插件目录
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
