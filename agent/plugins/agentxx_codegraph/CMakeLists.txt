# agentxx_codegraph —— CodeGraph 代码分析插件
# - 默认: 独立动态库 (MODULE), 链接 codegraph_core + tree_sitter 系 +
#   sqlite3/simdjson/glob (插件不链接 libagentxx, 仅依赖纯 C ABI 头)
# - 内置模式 (AGENTXX_ENABLE_PLUGIN_BUILTIN=ON): 源文件编译为 OBJECT 库合并进
#   libagentxx, codegraph_core/tree_sitter 系静态库由 libagentxx 一并链接
#   (顶层 superbuild 会先构建 codegraph_repo; sqlite3/simdjson/Glob/
#   uchardet/fmt 为 libagentxx 既有依赖, 不再重复链接)
# - 仅当 AGENTXX_ENABLE_PLUGIN_CODEGRAPH 时由 plugins/CMakeLists.txt 添加本子目录
set(PLUGIN_NAME agentxx_codegraph)

if (NOT AGENTXX_ENABLE_PLUGIN_CODEGRAPH)
  message(FATAL_ERROR "[agentxx_plugins] agentxx_codegraph requires AGENTXX_ENABLE_PLUGIN_CODEGRAPH=ON")
endif()

if (_AGENTXX_ADD_AS_BUILTIN)
  # ===== 内置合并编译分支 =====
  set(_target abp_${PLUGIN_NAME})
  add_library(${_target} OBJECT
    ${CMAKE_CURRENT_SOURCE_DIR}/agentxx_codegraph.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/codegraph_manager.cpp
  )
  target_compile_definitions(${_target} PRIVATE
    "AGENTXX_PLUGIN_BUILTIN=1"
    "AGENTXX_ENABLE_PLUGIN_CODEGRAPH=1"
    "agentxx_plugin_entry=agentxx_plugin_builtin_entry_${PLUGIN_NAME}"
    "agentxx_plugin_get_info=agentxx_plugin_builtin_get_info_${PLUGIN_NAME}"
    "agentxx_plugin_unload=agentxx_plugin_builtin_unload_${PLUGIN_NAME}"
  )
  target_include_directories(${_target} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"          # codegraph_manager.h / codegraph_plugin.h
    "${AGENTXX_LIB_INCLUDE_DIR}"           # agentxx/plugin/plugin_api.h
    "${AGENTXX_INSTALL_DIR}/include"       # codegraph/ fmt/ simdjson.h sqlite3.h
    "${AGENTXX_INSTALL_DIR}/include/Glob-1.0" # glob/glob.h
    "${AGENTXX_INSTALL_DIR}/include/uchardet"
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
  # codegraph_core 为静态库, 需连带其传递依赖 (与动态分支链接顺序一致;
  # sqlite3/simdjson/Glob/uchardet/fmt 由 libagentxx 既有链接提供)
  set(_codegraph_libs
    codegraph_core
    tree_sitter
    ts_cpp ts_python ts_javascript ts_dart ts_typescript ts_tsx
    ts_markdown ts_markdown_inline ts_rust ts_bash ts_go ts_java
    ts_kotlin ts_php ts_swift ts_objc ts_csharp ts_sql ts_lua
    ts_yaml ts_json ts_xml ts_html ts_css ts_zig ts_ruby ts_glsl
  )
  if (UNIX)
    # Android bionic 将 pthread 并入 libc, 无独立 libpthread 库, 仅链接 dl/m
    if (CMAKE_SYSTEM_NAME STREQUAL "Android")
      list(APPEND _codegraph_libs dl m)
    else()
      list(APPEND _codegraph_libs pthread dl m)
    endif()
  endif()
  set(AGENTXX_BUILTIN_PLUGIN_LINK_LIBS
    "${AGENTXX_BUILTIN_PLUGIN_LINK_LIBS}" ${_codegraph_libs} PARENT_SCOPE)
  set(AGENTXX_BUILTIN_PLUGIN_OBJECT_TARGETS "${AGENTXX_BUILTIN_PLUGIN_OBJECT_TARGETS}" "${_target}" PARENT_SCOPE)
  # 登记进内置插件清单 (子目录自登记: 平台不支持时本文件开头已提前 return,
  # 不会误登记; 原由父目录 plugins/CMakeLists.txt 无条件追加, 现统一走此通道)
  list(APPEND AGENTXX_BUILTIN_PLUGIN_NAMES ${PLUGIN_NAME})
  set(AGENTXX_BUILTIN_PLUGIN_NAMES "${AGENTXX_BUILTIN_PLUGIN_NAMES}" PARENT_SCOPE)
  set(AGENTXX_BUILTIN_MANIFEST_DECLS "${AGENTXX_BUILTIN_MANIFEST_DECLS}" PARENT_SCOPE)
  set(AGENTXX_BUILTIN_MANIFEST_ENTRIES "${AGENTXX_BUILTIN_MANIFEST_ENTRIES}" PARENT_SCOPE)
  # 资源: plugin.yaml (运行期目录配置推导 depends/入口)
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
  agentxx_codegraph.cpp
  codegraph_manager.cpp
)
# 依赖 (与 lib/CMakeLists.txt 一致; fmt/Glob 经 find_package 提供目标)
# 复用 agentxx_util 工具库 (插件静态链接, 与宿主副本符号互不冲突;
# find_package 经 AGENTXX_INSTALL_DIR 的 agentxx_util config 解析,
# 依赖链 fmt/SQLite3/uchardet/Iconv 由 config 内 find_dependency 传递)
if (NOT TARGET agentxx_util)
  # 独立构建本插件时经安装 config 导入; 内置合并模式 (经 libagentxx 构建)
  # 下 util 目标已存在于宿主工程, 跳过导入 (此时 install 目录尚未生成 config)
  find_package(agentxx_util REQUIRED)
endif()
find_package(fmt REQUIRED)
find_package(Glob REQUIRED)

target_include_directories(${PLUGIN_NAME} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"          # codegraph_manager.h / codegraph_plugin.h
  "${AGENTXX_LIB_INCLUDE_DIR}"           # agentxx/plugin/plugin_api.h
  "${AGENTXX_INSTALL_DIR}/include"       # codegraph/ fmt/ simdjson.h sqlite3.h
  "${AGENTXX_INSTALL_DIR}/include/Glob-1.0" # glob/glob.h
  "${AGENTXX_INSTALL_DIR}/include/uchardet"
)
target_compile_definitions(${PLUGIN_NAME} PRIVATE
  AGENTXX_ENABLE_PLUGIN_CODEGRAPH=1
)

# codegraph_core 为静态库, 需连带其传递依赖 (与 lib/CMakeLists.txt
# _AGENTXX_CODEGRAPH_LIBS 一致 + sqlite3/simdjson/glob/uchardet/fmt)
target_link_libraries(${PLUGIN_NAME} PRIVATE
  agentxx_util
  codegraph_core
  tree_sitter
  ts_cpp ts_python ts_javascript ts_dart ts_typescript ts_tsx
  ts_markdown ts_markdown_inline ts_rust ts_bash ts_go ts_java
  ts_kotlin ts_php ts_swift ts_objc ts_csharp ts_sql ts_lua
  ts_yaml ts_json ts_xml ts_html ts_css ts_zig ts_ruby ts_glsl
  sqlite3
  simdjson
  Glob
  uchardet
  fmt::fmt
)
# pthread/dl/m 为 Linux 系统库; Windows/MSVC 无对应导入库 (std::thread 原生), 条件链接
# Android bionic 将 pthread 并入 libc, 无独立 libpthread 库, 仅链接 dl/m
if (UNIX)
  if (CMAKE_SYSTEM_NAME STREQUAL "Android")
    target_link_libraries(${PLUGIN_NAME} PRIVATE dl m)
  else()
    target_link_libraries(${PLUGIN_NAME} PRIVATE pthread dl m)
  endif()
endif()
target_link_directories(${PLUGIN_NAME} PRIVATE
  "${AGENTXX_INSTALL_DIR}/lib"
  "${AGENTXX_INSTALL_DIR}/lib/Glob-1.0"
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
