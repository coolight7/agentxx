# agentxx_audio_stream —— 音频流捕获插件 (Windows WASAPI)
# - 独立动态库 (MODULE), 仅依赖纯 C ABI 头 + fmt + simdjson
# - 从 libagentxx src/expand/audio_stream 拆分独立:
#   - 工具 agentxx_audio_stream (start/stop/status)
#   - 捕获帧经 publish 事件推送 (topic "agentxx_audio_stream.audio")
set(PLUGIN_NAME agentxx_audio_stream)
add_library(${PLUGIN_NAME} SHARED
  agentxx_audio_stream.cpp
  audio_stream.cpp
)
target_include_directories(${PLUGIN_NAME} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"          # audio_stream.h / audio_stream_plugin.h
  "${AGENTXX_LIB_INCLUDE_DIR}"           # agentxx/plugin/plugin_api.h
  "${AGENTXX_INSTALL_DIR}/include"       # simdjson.h fmt
)
find_package(fmt REQUIRED)
target_link_libraries(${PLUGIN_NAME} PRIVATE
  simdjson
  fmt::fmt
)
target_link_directories(${PLUGIN_NAME} PRIVATE
  "${AGENTXX_INSTALL_DIR}/lib"
)
if (WIN32)
  # audio_stream: WASAPI (mmdevapi) / COM (ole32) / 进程名查询 (psapi)
  target_link_libraries(${PLUGIN_NAME} PRIVATE
    mmdevapi
    ole32
    psapi
  )
endif()

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
