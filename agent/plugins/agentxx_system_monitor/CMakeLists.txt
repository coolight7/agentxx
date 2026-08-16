# agentxx_system_monitor —— 系统资源监控插件 (CPU/内存/GPU)
# - 独立动态库 (MODULE), 仅依赖纯 C ABI 头 + asio (boost 头文件) + fmt + simdjson
# - 从 libagentxx src/expand/get_cpu_gpu_use 拆分独立:
#   - 工具 agentxx_get_system_core_info (原内置工具迁移, 同名同行为)
#   - 能力 agentxx.system_usage (方法 query): 宿主 TUI 系统资源展示链路
#     (WireGetSystemUsage) 经能力调用获取使用率 JSON
set(PLUGIN_NAME agentxx_system_monitor)
add_library(${PLUGIN_NAME} MODULE
  agentxx_system_monitor.cpp
  cpu_gpu_monitor.cpp
)
target_include_directories(${PLUGIN_NAME} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"          # cpu_gpu_monitor.h / system_monitor_plugin.h
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

# asio 头文件 (cpu_gpu_monitor 使用 asio 协程; 头文件-only, 仅需 include 路径)
# - 顶层 superbuild 经 _AGENTXX_COMMON_CMAKE_ARGS 传入 BOOST_ROOT (见
#   agent/CMakeLists.txt); 独立构建本目录时按构建脚本约定推断默认路径
if (NOT DEFINED BOOST_ROOT)
  set(_BOOST_CANDIDATES
    "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/boost-linux-build-debug"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/boost-linux-build-release"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/boost-windows-build-debug"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/boost-windows-build-release"
  )
  foreach (_bc ${_BOOST_CANDIDATES})
    if (EXISTS "${_bc}/include/boost/asio/awaitable.hpp")
      set(BOOST_ROOT "${_bc}")
      break()
    endif()
  endforeach()
endif()
if (NOT DEFINED BOOST_ROOT OR NOT EXISTS "${BOOST_ROOT}/include/boost/asio/awaitable.hpp")
  message(FATAL_ERROR "[agentxx_system_monitor] BOOST_ROOT not found (asio headers required)")
endif()
# boost 分发布局: ${BOOST_ROOT}/include/boost/asio/...; 源码用 asio/... 前缀
# (映射 include/boost/), 而 asio 头内部用 <boost/asio/...> 形式 (映射 include/),
# 两者缺一会导致内部头回退到系统安装的 boost, 与宿主 asio 混用崩溃
target_include_directories(${PLUGIN_NAME} PRIVATE
  "${BOOST_ROOT}/include/"
  "${BOOST_ROOT}/include/boost/"
)

if (WIN32)
  # cpu_gpu_monitor Windows 分支: PDH (性能计数器) + DXGI (GPU 适配器枚举)
  target_link_libraries(${PLUGIN_NAME} PRIVATE
    pdh
    dxgi
  )
else()
  # asio 文件 I/O 在 -DASIO_HAS_IO_URING 下引用 io_uring 符号; 插件为 MODULE
  # 动态库, 不依赖宿主进程导出 (宿主为静态链接时符号可能不存在), 自链接
  find_library(AGENTXX_URING_LIB uring HINTS "${AGENTXX_INSTALL_DIR}/lib")
  if (AGENTXX_URING_LIB)
    target_link_libraries(${PLUGIN_NAME} PRIVATE ${AGENTXX_URING_LIB})
  endif()
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
