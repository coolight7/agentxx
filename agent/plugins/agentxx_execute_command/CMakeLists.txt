# agentxx_execute_command —— 命令执行工具插件 (bash / windows)
# - 默认: 独立动态库 (MODULE), 仅依赖纯 C ABI 头 + agentxx_util + asio/boost.process
# - 内置模式 (AGENTXX_ENABLE_PLUGIN_BUILTIN=ON 且本插件在 AGENTXX_PLUGIN_BUILTIN_LIST):
#   源文件编译为 OBJECT 库合并进 libagentxx
set(PLUGIN_NAME agentxx_execute_command)

# ===== 平台支持判定 =====
# Linux/Windows/macOS 有真实实现 (macOS 仅 bash); Android 复用 Linux 分支
# (bash 可用); 平台矩阵说明见 docs/agent/plugins.md 9.3.1
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/plugin_platform_support.cmake")
agentxx_plugin_platform_gate(${PLUGIN_NAME} _AGENTXX_PLATFORM_OK windows linux android macos)
if (NOT _AGENTXX_PLATFORM_OK)
  return()
endif ()

# asio 头文件路径 (BOOST_ROOT 由顶层 superbuild 传入; 独立构建时按构建脚本
# 约定推断默认路径, 与 agentxx_system_monitor 同模式)
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
  message(FATAL_ERROR "[${PLUGIN_NAME}] BOOST_ROOT not found (asio headers required)")
endif()
# boost 分发布局: ${BOOST_ROOT}/include/boost/...; 源码用 asio/... 前缀
# (映射 include/boost/), 而 asio 头内部用 <boost/asio/...> 形式 (映射 include/)
set(_BOOST_INCLUDES "${BOOST_ROOT}/include/" "${BOOST_ROOT}/include/boost/")

if (_AGENTXX_ADD_AS_BUILTIN)
  # ===== 内置合并编译分支 =====
  set(_target abp_${PLUGIN_NAME})
  add_library(${_target} OBJECT
    ${CMAKE_CURRENT_SOURCE_DIR}/agentxx_execute_command.cpp
  )
  target_compile_definitions(${_target} PRIVATE
    "AGENTXX_PLUGIN_BUILTIN=1"
    "agentxx_plugin_entry=agentxx_plugin_builtin_entry_${PLUGIN_NAME}"
    "agentxx_plugin_get_info=agentxx_plugin_builtin_get_info_${PLUGIN_NAME}"
    "agentxx_plugin_unload=agentxx_plugin_builtin_unload_${PLUGIN_NAME}"
  )
  target_include_directories(${_target} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"          # agentxx_execmd_plugin.h / execute_command_impl.h
    "${AGENTXX_LIB_INCLUDE_DIR}"           # agentxx/plugin/plugin_api.h
    "${AGENTXX_INSTALL_DIR}/include"       # fmt neograph
    ${_BOOST_INCLUDES}
  )
  target_link_libraries(${_target} PRIVATE agentxx_util)
  # boost.process v2 编译库 + Windows 系统库 (经 libagentxx 收集通道;
  # Boost::process imported target 由顶层 find_package 提供, 此处以裸名
  # 追加, 经 lib 自身 link_directories 解析, 与 _AGENTXX_LINK_LIBS 一致)
  set(AGENTXX_BUILTIN_PLUGIN_LINK_LIBS
    "${AGENTXX_BUILTIN_PLUGIN_LINK_LIBS}" boost_process PARENT_SCOPE)
  if (WIN32)
    set(AGENTXX_BUILTIN_PLUGIN_LINK_LIBS
      "${AGENTXX_BUILTIN_PLUGIN_LINK_LIBS}" ws2_32 mswsock bcrypt PARENT_SCOPE)
  endif()
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
  list(APPEND AGENTXX_BUILTIN_PLUGIN_NAMES ${PLUGIN_NAME})
  set(AGENTXX_BUILTIN_PLUGIN_NAMES "${AGENTXX_BUILTIN_PLUGIN_NAMES}" PARENT_SCOPE)
  set(AGENTXX_BUILTIN_MANIFEST_DECLS "${AGENTXX_BUILTIN_MANIFEST_DECLS}" PARENT_SCOPE)
  set(AGENTXX_BUILTIN_MANIFEST_ENTRIES "${AGENTXX_BUILTIN_MANIFEST_ENTRIES}" PARENT_SCOPE)
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
  agentxx_execute_command.cpp
)
target_include_directories(${PLUGIN_NAME} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"          # agentxx_execmd_plugin.h / execute_command_impl.h
  "${AGENTXX_LIB_INCLUDE_DIR}"           # agentxx/plugin/plugin_api.h
  "${AGENTXX_INSTALL_DIR}/include"       # fmt neograph
  ${_BOOST_INCLUDES}
)
if (NOT TARGET agentxx_util)
  # 独立构建本插件时经安装 config 导入; 内置合并模式 (经 libagentxx 构建)
  # 下 util 目标已存在于宿主工程, 跳过导入 (此时 install 目录尚未生成 config)
  find_package(agentxx_util REQUIRED)
endif()
# boost.process v2 为编译库 (非 header-only): get_utf8_category 等
# BOOST_PROCESS_V2_DECL 符号实现于 libboost_process.a; 经 find_library 直接
# 定位归档 (不用 find_package(Boost), 其系统 include 会干扰 neograph/yyjson)
if (WIN32)
  set(_BP_LIBNAME libboost_process)
else()
  set(_BP_LIBNAME libboost_process.a)
endif()
find_library(AGENTXX_BOOST_PROCESS_LIB ${_BP_LIBNAME}
  PATHS "${BOOST_ROOT}/lib" "${AGENTXX_INSTALL_DIR}/lib" NO_DEFAULT_PATH
)
if (AGENTXX_BOOST_PROCESS_LIB)
  target_link_libraries(${PLUGIN_NAME} PRIVATE ${AGENTXX_BOOST_PROCESS_LIB})
else()
  message(WARNING "[${PLUGIN_NAME}] libboost_process not found, windows tool may fail to load")
endif()

if (WIN32)
  # boost.process v2 直传路径: socket/pipe 相关系统库
  target_link_libraries(${PLUGIN_NAME} PRIVATE ws2_32 mswsock bcrypt)
else()
  # asio 文件/pipe I/O 在 -DASIO_HAS_IO_URING 下引用 io_uring 符号;
  # 只查找本项目 liburing 安装的库, 避免 fallback 到系统 liburing
  find_library(AGENTXX_URING_LIB uring PATHS "${AGENTXX_INSTALL_DIR}/lib" NO_DEFAULT_PATH)
  if (AGENTXX_URING_LIB)
    target_link_libraries(${PLUGIN_NAME} PRIVATE ${AGENTXX_URING_LIB})
  endif()
endif()



# 循环符号依赖兜底: libhs/libcrypto 等静态归档成员相互引用 (如 libhs 引用
# CRYPTO_memcmp、util 引用 html2md), 链接器单遍扫描无法自解析; 在链接行
# 尾部以绝对路径重复提供相关归档 (target 形式会被去重/重排, 裸路径不会);
# 仅独立动态库分支需要 (内置合并进 libagentxx 由宿主统一链接)
find_library(AGENTXX_TAIL_LIBHS hs PATHS "${AGENTXX_INSTALL_DIR}/lib" NO_DEFAULT_PATH)
find_library(AGENTXX_TAIL_HS_RUNTIME hs_runtime PATHS "${AGENTXX_INSTALL_DIR}/lib" NO_DEFAULT_PATH)
find_library(AGENTXX_TAIL_URING uring PATHS "${AGENTXX_INSTALL_DIR}/lib" NO_DEFAULT_PATH)
find_library(AGENTXX_TAIL_CRYPTO crypto PATHS "${OPENSSL_ROOT_DIR}/lib64" "${OPENSSL_ROOT_DIR}/lib" NO_DEFAULT_PATH)
find_library(AGENTXX_TAIL_SSL ssl PATHS "${OPENSSL_ROOT_DIR}/lib64" "${OPENSSL_ROOT_DIR}/lib" NO_DEFAULT_PATH)
# util 归裁剪收录的对象还可能引用 neograph 系 (mcp from_json 等), 一并尾部提供
set(AGENTXX_TAIL_NEOGRAPH
  "${AGENTXX_INSTALL_DIR}/lib/libneograph_core.a"
  "${AGENTXX_INSTALL_DIR}/lib/libneograph_async.a"
  "${AGENTXX_INSTALL_DIR}/lib/libneograph_llm.a"
  "${AGENTXX_INSTALL_DIR}/lib/libneograph_acp.a"
  "${AGENTXX_INSTALL_DIR}/lib/libneograph_mcp.a"
  "${AGENTXX_INSTALL_DIR}/lib/libneograph_sqlite.a"
  "${AGENTXX_INSTALL_DIR}/lib/libyyjson.a"
)
find_package(html2md REQUIRED)
target_link_libraries(${PLUGIN_NAME} PRIVATE
  agentxx_util
  ${AGENTXX_TAIL_LIBHS} ${AGENTXX_TAIL_HS_RUNTIME} ${AGENTXX_TAIL_URING}
  ${AGENTXX_TAIL_CRYPTO} ${AGENTXX_TAIL_SSL}
  ${AGENTXX_TAIL_NEOGRAPH}
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
