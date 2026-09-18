# agentxx_screen_capture —— 屏幕捕获插件 (Windows)
# - 默认: 独立动态库 (MODULE), 仅依赖纯 C ABI 头 + cxx_utilxx_base_static (Json/JsonView)
# - 内置模式: 源文件编译为 OBJECT 库合并进
#   libagentxx; cxx_utilxx_base_static/simdjson 为 libagentxx 既有依赖; DXGI/D3D11/WIC 等平台库
#   经 AGENTXX_BUILTIN_PLUGIN_LINK_LIBS 补链接 (libagentxx 不再无条件携带
#   插件专用库)
# - 提供: agentxx_screen_capture (单帧/全部屏幕/鼠标屏/流式推送)
# - 从 agentxx_computer_use 拆分独立; agentxx_computer_use 经 plugin.yaml
#   depends 声明依赖本插件 (须先于其加载)
set(PLUGIN_NAME agentxx_screen_capture)

# ===== 平台支持判定 =====
# 仅 Windows 有真实实现 (DXGI/D3D11 桌面复制 + GDI 回退 + WIC PNG 编码);
# 其余平台源码为 "not supported on this platform" 空桩, 跳过编译避免产出
# 无功能意义的动态库 (平台矩阵说明见 docs/zh-cn/plugins.md 9.3.1)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/plugin_platform_support.cmake")
agentxx_plugin_platform_gate(${PLUGIN_NAME} _AGENTXX_PLATFORM_OK windows)
if (NOT _AGENTXX_PLATFORM_OK)
  return() # 跳过本插件全部目标 (内置合并/独立动态库两模式均在此拦截)
endif ()

if (_AGENTXX_ADD_AS_BUILTIN)
  # ===== 内置合并编译分支 =====
  set(_target abp_${PLUGIN_NAME})
  add_library(${_target} OBJECT
    ${CMAKE_CURRENT_SOURCE_DIR}/agentxx_screen_capture.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/screen_capture.cpp
  )
  target_compile_definitions(${_target} PRIVATE
    "AGENTXX_PLUGIN_BUILTIN=1"
    "agentxx_plugin_agent_create=agentxx_plugin_agent_builtin_create_${PLUGIN_NAME}"
    "agentxx_plugin_agent_get_info=agentxx_plugin_agent_builtin_get_info_${PLUGIN_NAME}"
    "agentxx_plugin_agent_destroy=agentxx_plugin_agent_builtin_destroy_${PLUGIN_NAME}"
    "agentxx_plugin_agent_start=agentxx_plugin_agent_builtin_start_${PLUGIN_NAME}"
    "agentxx_plugin_agent_stop=agentxx_plugin_agent_builtin_stop_${PLUGIN_NAME}"
  )
  target_include_directories(${_target} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"          # screen_capture.h / screen_capture_plugin.h
    "${AGENTXX_LIB_INCLUDE_DIR}"           # agentxx/plugin/plugin_api.h
    "${AGENTXX_INSTALL_DIR}/include"       # cxx_utilxx_base_static (Json/JsonView) simdjson.h
  )
  # 复用 cxx_utilxx_base_static 工具库 (libagentxx 已链接本库; 此处经 PRIVATE 链接仅
  # 传递编译宏/头路径: util 头文件依赖 XX_IS_* 宏与 include 路径)
  target_link_libraries(${_target} PRIVATE cxx_utilxx_base_static)
  if (XX_IS_WIN_D)
    # screen_capture: DXGI/D3D11 (桌面捕获) + WIC windowscodecs (PNG 编码)
    # - 独立模式经下方独立分支 PRIVATE 链接; 内置模式经本收集通道并入 libagentxx
    set(AGENTXX_BUILTIN_PLUGIN_LINK_LIBS
      "${AGENTXX_BUILTIN_PLUGIN_LINK_LIBS}" d3d11 dxgi windowscodecs PARENT_SCOPE)
  endif()
set_target_properties(${_target} PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED OFF
  )
  set(AGENTXX_BUILTIN_PLUGIN_OBJECT_TARGETS "${AGENTXX_BUILTIN_PLUGIN_OBJECT_TARGETS}" "${_target}" PARENT_SCOPE)
  # 登记进内置插件清单 (子目录自登记: 平台不支持时本文件开头已提前 return,
  # 不会误登记; 原由父目录 plugins/CMakeLists.txt 无条件追加, 现统一走此通道)
  list(APPEND AGENTXX_BUILTIN_PLUGIN_NAMES ${PLUGIN_NAME})
  set(AGENTXX_BUILTIN_PLUGIN_NAMES "${AGENTXX_BUILTIN_PLUGIN_NAMES}" PARENT_SCOPE)
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
  agentxx_screen_capture.cpp
  screen_capture.cpp
)

# ===== 依赖查找与链接 (顺序: 先依赖库的依赖, 再依赖库本体) =====
# 工具库条件依赖 (io_uring / hyperscan) 由 plugins/CMakeLists.txt 统一查找;
# 本处只导入依赖库本体, 随后按 imported target / 裸库名链接
if (NOT TARGET cxx_utilxx_base_static)
  # 内置合并编译时 util 目标随 libagentxx 已存在 (且 install 目录尚未生成
  # config), 仅独立构建本插件时经安装 config 导入
  find_package(cxx_utilxx_base REQUIRED)
endif()
find_package(fmt REQUIRED)

target_include_directories(${PLUGIN_NAME} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"          # screen_capture.h / screen_capture_plugin.h
  "${AGENTXX_LIB_INCLUDE_DIR}"           # agentxx/plugin/plugin_api.h
  "${AGENTXX_INSTALL_DIR}/include/"      # cxx_utilxx_base_static (Json/JsonView) simdjson.h
)

# 裸库名依赖 (如 hs_runtime) 的解析目录: 与 cxx_utilxx* 导出接口的 link 目录一致
target_link_directories(${PLUGIN_NAME} PRIVATE
  "${AGENTXX_INSTALL_DIR}/lib/"
)

target_link_libraries(${PLUGIN_NAME} PRIVATE
  cxx_utilxx_base_static
  simdjson
  fmt::fmt
)
if (XX_IS_WIN_D)
  # screen_capture: DXGI/D3D11/GDI + WIC (PNG 编码)
  target_link_libraries(${PLUGIN_NAME} PRIVATE
    d3d11 dxgi windowscodecs
  )
endif()
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
