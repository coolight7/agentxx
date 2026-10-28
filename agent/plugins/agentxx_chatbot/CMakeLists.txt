# agentxx_chatbot —— 聊天平台桥接插件 (先做 QQ)
#
# 把平台 (群聊 / 私聊) 的消息投给插件自己的根 agent 会话, 再把回复发回平台。
# 设计见 resource/history/chatbot/plan.md, 插件骨架参照 example_runtime。
#
# - 默认: 独立动态库 (MODULE), 只依赖纯 C ABI 头 + cxx_utilxx_static (HTTP/WS/散列)
#   + yaml-cpp (可选配置文件)
# - 内置模式: 源文件编译为 OBJECT 库合并进 libagentxx
set(PLUGIN_NAME agentxx_chatbot)

# ===== 平台支持判定 =====
# 纯网络 IO (asio/beast 经 cxx_utilxx_static), 全平台可用
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/plugin_platform_support.cmake")
agentxx_plugin_platform_gate(${PLUGIN_NAME} _AGENTXX_PLATFORM_OK windows linux macos android ios)
if (NOT _AGENTXX_PLATFORM_OK)
  return()
endif ()

# 源文件清单 (两分支共用)
set(_CHATBOT_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/agentxx_chatbot.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/chatbot_types.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/chatbot_core.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/chatbot_config.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/chatbot_router.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/chatbot_engine.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/chatbot_outbound.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/chatbot_tools.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/platform/platform.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/platform/fake/fake_platform.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/platform/qq/qq_api.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/platform/qq/qq_gateway.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/platform/qq/qq_media.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/platform/qq/qq_platform.cpp
)

if (_AGENTXX_ADD_AS_BUILTIN)
  # ===== 内置合并编译分支 =====
  set(_target abp_${PLUGIN_NAME})
  add_library(${_target} OBJECT ${_CHATBOT_SOURCES})
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
    "${CMAKE_CURRENT_SOURCE_DIR}"          # chatbot_*.h / platform/*.h
    "${AGENTXX_LIB_INCLUDE_DIR}"           # agentxx/plugin/api/plugin_api.h
    "${AGENTXX_INSTALL_DIR}/include"       # cxx_utilxx_static (Json) / yaml-cpp
  )
  find_package(yaml-cpp REQUIRED)
  # Boost.Asio 头 (源码写 `asio/xxx.hpp`, 由 `${Boost_INCLUDE_DIRS}/boost/` 命中
  # `boost/asio/xxx.hpp`; 两个 include 根缺一会回退到系统 Boost)
  find_package(Boost CONFIG REQUIRED)
  target_include_directories(${_target} SYSTEM PRIVATE
    "${Boost_INCLUDE_DIRS}"
    "${Boost_INCLUDE_DIRS}/boost/"
  )
  target_link_libraries(${_target} PRIVATE cxx_utilxx_static yaml-cpp::yaml-cpp)
  set_target_properties(${_target} PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED OFF
  )
  set(AGENTXX_BUILTIN_PLUGIN_OBJECT_TARGETS "${AGENTXX_BUILTIN_PLUGIN_OBJECT_TARGETS}" "${_target}" PARENT_SCOPE)
  list(APPEND AGENTXX_BUILTIN_PLUGIN_NAMES ${PLUGIN_NAME})
  set(AGENTXX_BUILTIN_PLUGIN_NAMES "${AGENTXX_BUILTIN_PLUGIN_NAMES}" PARENT_SCOPE)
  # 资源: plugin.yaml (运行期目录配置推导 depends/入口)
  set(_res "${AGENTXX_EXEC_INSTALL_PREFIX}/plugins/${PLUGIN_NAME}/plugin.yaml")
  add_custom_target(${PLUGIN_NAME}_builtin_resources ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/plugin.yaml" "${_res}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/plugin.yaml"
    COMMENT "Copy ${PLUGIN_NAME} plugin resource (builtin mode)")
  return()
endif()

# ===== 独立动态库编译 (默认) =====
add_library(${PLUGIN_NAME} SHARED ${_CHATBOT_SOURCES})

# ===== 依赖查找与链接 (顺序: 先依赖库的依赖, 再依赖库本体) =====
# 工具库条件依赖 (io_uring / hyperscan) 由 plugins/CMakeLists.txt 统一查找;
# 本处只导入依赖库本体, 随后按 imported target / 裸库名链接
if (NOT TARGET cxx_utilxx_static)
  find_package(cxx_utilxx REQUIRED)
endif()
find_package(yaml-cpp REQUIRED)

target_include_directories(${PLUGIN_NAME} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"          # chatbot_*.h / platform/*.h
  "${AGENTXX_LIB_INCLUDE_DIR}"           # agentxx/plugin/api/plugin_api.h
  "${AGENTXX_INSTALL_DIR}/include/"      # cxx_utilxx_static (Json) / yaml-cpp
)
# Boost.Asio 头 (源码写 `asio/xxx.hpp`, 由 `${Boost_INCLUDE_DIRS}/boost/` 命中
# `boost/asio/xxx.hpp`; 两个 include 根缺一会回退到系统 Boost, 与主程序混用)
find_package(Boost CONFIG REQUIRED)
target_include_directories(${PLUGIN_NAME} SYSTEM PRIVATE
  "${Boost_INCLUDE_DIRS}"
  "${Boost_INCLUDE_DIRS}/boost/"
)

# 裸库名依赖 (如 hs_runtime) 的解析目录: 与 cxx_utilxx* 导出接口的 link 目录一致
target_link_directories(${PLUGIN_NAME} PRIVATE
  "${AGENTXX_INSTALL_DIR}/lib/"
)

target_link_libraries(${PLUGIN_NAME} PRIVATE
  cxx_utilxx_static
  yaml-cpp::yaml-cpp
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
