# agentxx_system —— 系统信息工具插件 (当前日期时间)
# - 默认: 独立动态库 (MODULE), 仅依赖纯 C ABI 头 + fmt + neograph(json)
# - 内置模式:
#   源文件编译为 OBJECT 库合并进 libagentxx
set(PLUGIN_NAME agentxx_system)

# ===== 平台支持判定 =====
# 纯标准库时间处理, 全平台可用 (平台矩阵说明见 docs/agent/plugins.md 9.3.1)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/plugin_platform_support.cmake")
agentxx_plugin_platform_gate(${PLUGIN_NAME} _AGENTXX_PLATFORM_OK windows linux macos android ios)
if (NOT _AGENTXX_PLATFORM_OK)
  return() # 跳过本插件全部目标 (内置合并/独立动态库两模式均在此拦截)
endif ()

if (_AGENTXX_ADD_AS_BUILTIN)
  # ===== 内置合并编译分支 =====
  set(_target abp_${PLUGIN_NAME})
  add_library(${_target} OBJECT
    ${CMAKE_CURRENT_SOURCE_DIR}/agentxx_system.cpp
  )
  target_compile_definitions(${_target} PRIVATE
    "AGENTXX_PLUGIN_BUILTIN=1"
    "agentxx_plugin_agent_create=agentxx_plugin_agent_builtin_create_${PLUGIN_NAME}"
    "agentxx_plugin_agent_get_info=agentxx_plugin_agent_builtin_get_info_${PLUGIN_NAME}"
    "agentxx_plugin_agent_destroy=agentxx_plugin_agent_builtin_destroy_${PLUGIN_NAME}"
  )
  target_include_directories(${_target} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"          # agentxx_system_plugin.h / system_impl.h
    "${AGENTXX_LIB_INCLUDE_DIR}"           # agentxx/plugin/plugin_api.h
    "${AGENTXX_INSTALL_DIR}/include"       # fmt neograph
  )
  target_link_libraries(${_target} PRIVATE agentxx_util)
  set_target_properties(${_target} PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED OFF
  )
  set(AGENTXX_BUILTIN_PLUGIN_MANIFEST_DECLS
    "${AGENTXX_BUILTIN_PLUGIN_MANIFEST_DECLS}
extern \"C\" const AgentxxPluginInfo* agentxx_plugin_agent_builtin_get_info_${PLUGIN_NAME}(void);
extern \"C\" int agentxx_plugin_agent_builtin_create_${PLUGIN_NAME}(const AgentxxHost* host, void** plugin_ctx);
extern \"C\" void agentxx_plugin_agent_builtin_destroy_${PLUGIN_NAME}(void* plugin_ctx);")
  set(AGENTXX_BUILTIN_PLUGIN_MANIFEST_ENTRIES
    "${AGENTXX_BUILTIN_PLUGIN_MANIFEST_ENTRIES}
    {\"${PLUGIN_NAME}\", agentxx_plugin_agent_builtin_get_info_${PLUGIN_NAME}, agentxx_plugin_agent_builtin_create_${PLUGIN_NAME}, agentxx_plugin_agent_builtin_destroy_${PLUGIN_NAME}},")
  set(AGENTXX_BUILTIN_PLUGIN_OBJECT_TARGETS "${AGENTXX_BUILTIN_PLUGIN_OBJECT_TARGETS}" "${_target}" PARENT_SCOPE)
  list(APPEND AGENTXX_BUILTIN_PLUGIN_NAMES ${PLUGIN_NAME})
  set(AGENTXX_BUILTIN_PLUGIN_NAMES "${AGENTXX_BUILTIN_PLUGIN_NAMES}" PARENT_SCOPE)
  set(AGENTXX_BUILTIN_PLUGIN_MANIFEST_DECLS "${AGENTXX_BUILTIN_PLUGIN_MANIFEST_DECLS}" PARENT_SCOPE)
  set(AGENTXX_BUILTIN_PLUGIN_MANIFEST_ENTRIES "${AGENTXX_BUILTIN_PLUGIN_MANIFEST_ENTRIES}" PARENT_SCOPE)
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
  agentxx_system.cpp
)

# ===== 依赖统一查找与链接 =====
# 仿 agent/lib/CMakeLists.txt 四件套模式: find_package 导入依赖 target /
# target_include_directories 声明头路径 / target_link_directories 提供裸库名
# 解析目录 / target_link_libraries 以 imported target + 裸库名链接
if (NOT TARGET agentxx_util)
  # 独立构建本插件时经安装 config 导入; 内置合并模式 (经 libagentxx 构建)
  # 下 util 目标已存在于宿主工程, 跳过导入 (此时 install 目录尚未生成 config)
  find_package(agentxx_util REQUIRED)
endif()
find_package(html2md REQUIRED)
# OpenSSL 保持模块模式 (FindOpenSSL, 与 lib 一致): 本项目 OpenSSL 为预构建
# 目录 (无 config), OPENSSL_ROOT_DIR 由顶层 superbuild 传入精确指向预构建
# 目录, 不会命中系统 OpenSSL (独立构建未传时回退系统库)
find_package(OpenSSL REQUIRED)

target_include_directories(${PLUGIN_NAME} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"          # agentxx_system_plugin.h / system_impl.h
  "${AGENTXX_LIB_INCLUDE_DIR}"           # agentxx/plugin/plugin_api.h
  "${AGENTXX_INSTALL_DIR}/include/"      # fmt neograph
)

# 裸库名依赖解析目录 (与 lib 一致): neograph 系/yyjson/hs_runtime 位于
# AGENTXX_INSTALL_DIR/lib; OpenSSL 预构建归档目录供下方 ELF 尾部兜底裸名
# (crypto/ssl) 解析
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
  # hyperscan pkg-config 模块由 plugins/CMakeLists.txt 统一定义 (与
  # lib/client/test 一致); hs_runtime 为裸名经上方 link_directories 解析
  target_link_libraries(${PLUGIN_NAME} PRIVATE PkgConfig::hyperscan hs_runtime)
endif()
if (AGENTXX_LINUX_IO_URING_SUPPORTED)
  # io_uring 同上由总控统一定义 (util 的 asio 重模块在 -DASIO_HAS_IO_URING
  # 下引用其符号)
  target_link_libraries(${PLUGIN_NAME} PRIVATE PkgConfig::uring)
endif()
if (NOT XX_IS_WIN_D)
  # ELF (GNU ld/lld) 单遍扫描无法自解析相互引用的静态归档成员 (libhs↔
  # libcrypto、util↔html2md/neograph); 尾部以裸库名重复提供打破循环
  # (裸名字符串不被 CMake 去重, 等价于原先的绝对路径补链; MSVC 多遍解析
  # 无需此兜底), 解析目录由上方 target_link_directories 提供
  set(_tail_libs crypto ssl)
  if (AGENTXX_ENABLE_HYPERSCAN)
    list(APPEND _tail_libs hs hs_runtime)
  endif()
  if (AGENTXX_LINUX_IO_URING_SUPPORTED)
    list(APPEND _tail_libs uring)
  endif()
  list(APPEND _tail_libs
    neograph_sqlite neograph_acp neograph_mcp neograph_mcp_types neograph_async neograph_llm neograph_core yyjson)
  # group 迭代扫描: crypto/ssl↔hs↔neograph 归档成员相互引用链较长, 单遍
    # 扫描两次不足以收敛, start-group 让 ld 反复解析直至无新未定义符号
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
add_custom_command(TARGET ${PLUGIN_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin.yaml"
    "${AGENTXX_EXEC_INSTALL_PREFIX}/plugins/${PLUGIN_NAME}/plugin.yaml"
  COMMENT "Copy ${PLUGIN_NAME} plugin resources to ${AGENTXX_EXEC_INSTALL_PREFIX}/plugins/${PLUGIN_NAME}"
)
