cmake_minimum_required(VERSION 3.10)
# find_package 使用 <PackageName>_ROOT 变量
cmake_policy(SET CMP0074 NEW)
# MSVC 运行时库标志由抽象层选择
cmake_policy(SET CMP0091 NEW)
# find_package 使用大写的 _ROOT 变量
cmake_policy(SET CMP0144 NEW)
cmake_policy(SET CMP0167 OLD)

project(agentxx VERSION 0.1.0 LANGUAGES C CXX)
include(GNUInstallDirs) # CMAKE_INSTALL_*DIR

# 由顶层 superbuild 传入 (见 agent/CMakeLists.txt)，独立构建本目录时取默认值
option(AGENTXX_ENABLE_ASAN "Enable AddressSanitizer for Debug builds." ON)
option(AGENTXX_ENABLE_DEBUG_INFO "Enable debug info (-g) for Debug builds." ON)
option(AGENTXX_ENABLE_PCH "Enable precompiled headers (GCC/Clang)." ON)
# 可选内置合并编译: 启用的插件源文件直接编译进 libagentxx (运行期无需插件
# 动态库, 无 dlopen); 源文件/依赖库经 plugins/CMakeLists.txt 内置分支收集
# (add_subdirectory ../plugins), 入口清单 builtin_plugins.cpp 由 libagentxx 编译
option(AGENTXX_ENABLE_PLUGIN_BUILTIN "Merge enabled plugins into libagentxx (built-in, no runtime plugin dlls)." OFF)
# 内置合并模式下各插件的启用开关 (与 plugins/CMakeLists.txt 同名同默认值;
# 由顶层 superbuild 统一传入; 仅 AGENTXX_ENABLE_PLUGIN_BUILTIN=ON 时生效)
option(AGENTXX_ENABLE_PLUGIN_EXAMPLE "Merge example plugin into libagentxx (built-in)." ON)
option(AGENTXX_ENABLE_PLUGIN_COMPUTER_USE "Merge agentxx_computer_use plugin into libagentxx (built-in)." ON)
option(AGENTXX_ENABLE_PLUGIN_SCREEN_CAPTURE "Merge agentxx_screen_capture plugin into libagentxx (built-in)." ON)
# 嵌套构建时由顶层传入；独立构建时自动查找
find_program(CCACHE_PROGRAM ccache)

# 建议使用 c++ 26 
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)

if(XX_IS_MSVC_D)
  list(PREPEND CMAKE_FIND_LIBRARY_SUFFIXES .lib .a)
else()
  list(PREPEND CMAKE_FIND_LIBRARY_SUFFIXES .a)
endif()

file(GLOB_RECURSE agentxx_SOURCES
  CONFIGURE_DEPENDS
 "src/*.cpp" "src/*.cc" "src/*.cxx"
)

# ===== 可选: 内置插件合并编译 (AGENTXX_ENABLE_PLUGIN_BUILTIN=ON) =====
# - add_subdirectory(../plugins) 以内置模式收集启用的插件源文件/依赖库/
#   入口符号 (见 plugins/CMakeLists.txt 内置分支; 不创建任何插件动态库目标)
# - 生成的清单 builtin_plugins.cpp (改名后的入口符号静态表) 一并编译进
#   libagentxx, PluginManager 经 agentxx_get_builtin_plugins() 查询加载
if (AGENTXX_ENABLE_PLUGIN_BUILTIN)
  set(AGENTXX_IS_LIB_BUILTIN_SUBDIR 1)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../plugins" agentxx_plugins_builtin)
  list(APPEND agentxx_SOURCES ${AGENTXX_BUILTIN_MANIFEST_SOURCE})
  # 生成的清单提供 agentxx_get_builtin_plugins 实现; 默认空注册表
  # (src/plugins/builtin_plugin_registry.cpp) 经宏排除, 避免重复定义
  set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/builtin_plugin_registry.cpp"
    PROPERTIES COMPILE_DEFINITIONS AGENTXX_USE_BUILTIN_PLUGIN_MANIFEST=1
  )
endif()

# ===== 目标创建 =====
# - GCC/Clang: 通过 OBJECT 库把 62 个源文件只编译一次，共享/静态库复用同一批 .o，
#   编译时间近乎减半。AGENTXX_EXPORTS 等宏在当前代码中已无实际引用，故可安全共用。
#   注意: 不能用 target_link_libraries(shared PRIVATE obj) 关联 OBJECT 库，
#   否则 PackageProject 生成的 install(EXPORT) 会因 obj 不在导出集中而报错；
#   改为把 usage requirements (宏/头文件路径) 统一设置到 obj 与共享/静态库上。
#   对共享/静态库用 PUBLIC 即可: 它们无源文件可编，编译部分静默忽略，
#   INTERFACE 部分照常传播给 client/test；obj 的 INTERFACE 部分无人消费，均无害。
# - MSVC: 保留原有双编译路径 (dllexport 等符号语义差异，避免行为回归)。
if (XX_IS_MSVC_D)
  # 创建 动态库/静态库
  add_library(agentxx_shared SHARED ${agentxx_SOURCES})
  add_library(agentxx_static STATIC ${agentxx_SOURCES})
  set(_AGENTXX_USAGE_TARGETS agentxx_shared agentxx_static)
else ()
  add_library(agentxx_obj OBJECT ${agentxx_SOURCES})
  set_target_properties(agentxx_obj PROPERTIES POSITION_INDEPENDENT_CODE ON)
  add_library(agentxx_shared SHARED $<TARGET_OBJECTS:agentxx_obj>)
  add_library(agentxx_static STATIC $<TARGET_OBJECTS:agentxx_obj>)
  set(_AGENTXX_USAGE_TARGETS agentxx_obj agentxx_shared agentxx_static)

  # PCH: 只预编译稳定第三方头 (std/fmt/asio/boost.exception)，项目自身头不参与，
  # 避免项目头文件改动时触发全量重编。
  # 第三方头须用 <...> 形式: 非 <> 形式会被 CMake 当作源目录相对路径解析，
  # 而 fmt/asio/boost 位于 SYSTEM 安装目录 (install/include)，会解析失败
  if (AGENTXX_ENABLE_PCH)
    target_precompile_headers(agentxx_obj PRIVATE
      <algorithm> <atomic> <chrono> <cstdint> <cstring> <deque> <exception>
      <filesystem> <fstream> <functional> <map> <memory> <mutex> <optional>
      <set> <sstream> <string> <string_view> <thread> <unordered_map>
      <utility> <variant> <vector>
      <fmt/format.h>
      <asio/awaitable.hpp>
      <boost/exception/exception.hpp>
    )
    # ccache + PCH 兼容 (GCC 特有): 不加 -fpch-preprocess 时 ccache 无法预处理
    # 含 -include PCH 的源文件，全部标记 uncacheable，改动即全量重编。
    # 仅对本目标生效，不影响第三方依赖的缓存
    if (CCACHE_PROGRAM AND XX_IS_GCC_D)
      target_compile_options(agentxx_obj PRIVATE -fpch-preprocess)
    endif()
  endif()
endif ()

# 动态库控制导出符号
target_compile_definitions(agentxx_shared PUBLIC AGENTXX_EXPORTS)

if (XX_IS_MSVC_D)
  target_compile_definitions(agentxx_shared PUBLIC
    strdup=_strdup
    strcasecmp=_stricmp
    strncasecmp=_strnicmp
  )
  target_compile_definitions(agentxx_static PUBLIC
    strdup=_strdup
    strcasecmp=_stricmp
    strncasecmp=_strnicmp
  )
endif()
foreach(_agentxx_target ${_AGENTXX_USAGE_TARGETS})
  target_compile_definitions(${_agentxx_target} PUBLIC
    __STDC_CONSTANT_MACROS
    XX_IS_DEBUG_D=${XX_IS_DEBUG_D}
    XX_IS_RELEASE_D=${XX_IS_RELEASE_D}

    XX_IS_LINUX_D=${XX_IS_LINUX_D}
    XX_IS_WIN_D=${XX_IS_WIN_D}
    XX_IS_MACOS_D=${XX_IS_MACOS_D}
    XX_IS_ANDROID_D=${XX_IS_ANDROID_D}
    XX_IS_IOS_D=${XX_IS_IOS_D}

    NEOGRAPH_STATIC_BUILD
    NEOGRAPH_USE_BOOST_ASIO
    CPPHTTPLIB_OPENSSL_SUPPORT
  )
  if (AGENTXX_ENABLE_HYPERSCAN)
    target_compile_definitions(${_agentxx_target} PUBLIC AGENTXX_ENABLE_HYPERSCAN=1)
  endif()
  if (AGENTXX_ENABLE_BOOST_PROCESS)
    target_compile_definitions(${_agentxx_target} PUBLIC AGENTXX_ENABLE_BOOST_PROCESS=1)
  endif()
endforeach()
unset(_agentxx_target)

find_package(PkgConfig REQUIRED)
# 添加依赖库后应当同步更新 [packageProject:DEPENDENCIES]
set (agentxx_DEPENDENCIES "Iconv;uchardet;fmt;simdjson;ZLIB;OpenSSL;Boost;yaml-cpp;CURL;SQLite3;html2md;Glob")
find_package(iconv REQUIRED)
find_package(uchardet REQUIRED)
find_package(fmt REQUIRED)
find_package(simdjson REQUIRED)
find_package(ZLIB REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Boost REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(html2md REQUIRED)
find_package(Glob REQUIRED)

foreach(_agentxx_target ${_AGENTXX_USAGE_TARGETS})
  target_include_directories(${_agentxx_target} PUBLIC 
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    # 标记为 SYSTEM 防止 include 影响 warning 转 error 判断
    "${AGENTXX_INSTALL_DIR}/include/"
    "${neograph_SOURCE_DIR}/deps/yyjson/"
    "${neograph_SOURCE_DIR}/deps/"
  )
  if(Boost_FOUND)
    target_include_directories(${_agentxx_target} SYSTEM PUBLIC 
      "${Boost_INCLUDE_DIRS}"
      "${Boost_INCLUDE_DIRS}/boost/"
    )
    target_link_directories(${_agentxx_target} PUBLIC 
      "${BOOST_LIBRARY_DIRS}"
    )
  endif()
endforeach()
unset(_agentxx_target)

# 统一输出名称 libagentxx
# - 不指定时，部分系统，如win可能编译输出缺少前缀 lib
if (XX_IS_DEBUG_D)
  # 生成 libagentxxd.so
  set_target_properties(agentxx_shared PROPERTIES
    OUTPUT_NAME "agentxxd"
    PUBLIC_HEADER "include/agentxx/agentxx.h"
    PREFIX "lib"
    CXX_STANDARD 26
    LINK_LIBRARIES_STRATEGY "REORDER_MINIMALLY"
  )
  # 生成 libagentxx_staticd.a
  set_target_properties(agentxx_static PROPERTIES
    OUTPUT_NAME "agentxx_staticd"
    PUBLIC_HEADER "include/agentxx/agentxx.h"
    PREFIX "lib"
    CXX_STANDARD 26
    LINK_LIBRARIES_STRATEGY "REORDER_MINIMALLY"
  )
else()
  # 生成 libagentxx.so
  set_target_properties(agentxx_shared PROPERTIES
    OUTPUT_NAME "agentxx"
    PUBLIC_HEADER "include/agentxx/agentxx.h"
    PREFIX "lib"
    CXX_STANDARD 26
    LINK_LIBRARIES_STRATEGY "REORDER_MINIMALLY"
  )
  # 生成 libagentxx_static.a
  set_target_properties(agentxx_static PROPERTIES
    OUTPUT_NAME "agentxx_static"
    PUBLIC_HEADER "include/agentxx/agentxx.h"
    PREFIX "lib"
    CXX_STANDARD 26
    LINK_LIBRARIES_STRATEGY "REORDER_MINIMALLY"
  )
endif()
target_link_directories(agentxx_shared PUBLIC "${AGENTXX_INSTALL_DIR}/lib/")
target_link_directories(agentxx_static PUBLIC "${AGENTXX_INSTALL_DIR}/lib/")

# OBJECT 库自身也要链接这些依赖 (PRIVATE):
# 依赖的 usage requirements (include 目录等) 需要作用于 agentxx_obj 的编译，
# 否则编译期找不到 Glob/uchardet/OpenSSL/neograph 等头文件。
# 注意: 不能让共享/静态库链接 agentxx_obj (见上方说明)，此处仅 obj -> 依赖。
if (TARGET agentxx_obj)
  set(_AGENTXX_OBJ_LINK_TARGETS agentxx_obj)
else()
  set(_AGENTXX_OBJ_LINK_TARGETS "")
endif()

if (AGENTXX_ENABLE_HYPERSCAN)
  pkg_check_modules(hyperscan REQUIRED IMPORTED_TARGET libhs)
  foreach(_agentxx_target ${_AGENTXX_OBJ_LINK_TARGETS})
    target_link_libraries(${_agentxx_target} PRIVATE PkgConfig::hyperscan hs_runtime)
  endforeach()
  target_link_libraries(agentxx_shared PRIVATE 
    PkgConfig::hyperscan
    hs_runtime
  )
  target_link_libraries(agentxx_static PRIVATE 
    PkgConfig::hyperscan 
    hs_runtime
  )
endif()
set(_AGENTXX_LINK_LIBS
  # === neograph vvv 链接顺序是必要的
  neograph_sqlite
  neograph_acp
  neograph_mcp
  neograph_async
  neograph_llm
  neograph_core
  # === neograph ^^^
  html2md
  Glob
  ZLIB::ZLIB
  SQLite::SQLite3
  CURL::libcurl
  yaml-cpp::yaml-cpp
  OpenSSL::SSL
  OpenSSL::Crypto
  simdjson::simdjson
  yyjson
  fmt::fmt
  Iconv::Iconv
  charset
  uchardet::libuchardet
)
foreach(_agentxx_target ${_AGENTXX_OBJ_LINK_TARGETS})
  target_link_libraries(${_agentxx_target} PRIVATE ${_AGENTXX_LINK_LIBS})
endforeach()
target_link_libraries(agentxx_shared PUBLIC ${_AGENTXX_LINK_LIBS})
target_link_libraries(agentxx_static PUBLIC ${_AGENTXX_LINK_LIBS})
unset(_AGENTXX_LINK_LIBS)
if (AGENTXX_ENABLE_BOOST_PROCESS)
  find_package(Boost REQUIRED COMPONENTS process)
  foreach(_agentxx_target ${_AGENTXX_OBJ_LINK_TARGETS})
    target_link_libraries(${_agentxx_target} PRIVATE Boost::process)
  endforeach()
  target_link_libraries(agentxx_shared PUBLIC Boost::process)
  target_link_libraries(agentxx_static PUBLIC Boost::process)
endif()
# 内置插件合并编译: 依赖库 (quickjs/codegraph 系/平台库) 与 include 路径
# - 收集自 plugins/CMakeLists.txt 内置分支 (各插件子目录追加 PARENT_SCOPE)
# - 链接策略与 _AGENTXX_LINK_LIBS 一致 (obj PRIVATE 供编译; 共享/静态库
#   PUBLIC 保证静态库消费者连带链接)
if (AGENTXX_ENABLE_PLUGIN_BUILTIN)
  # 内置插件 OBJECT 库目标文件合并进共享/静态库 (各插件 OBJECT 库自带
  # per-plugin 编译定义/包含路径, 与 lib 自身源码互不影响)
  foreach(_plugin_target ${AGENTXX_BUILTIN_PLUGIN_OBJECT_TARGETS})
    target_sources(agentxx_shared PRIVATE $<TARGET_OBJECTS:${_plugin_target}>)
    target_sources(agentxx_static PRIVATE $<TARGET_OBJECTS:${_plugin_target}>)
  endforeach()
  # 内置插件依赖库 (quickjs/codegraph 系/平台库; 收集自 plugins 内置分支,
  # 与 _AGENTXX_LINK_LIBS 相同链接策略: obj PRIVATE 供编译; 共享/静态库
  # PUBLIC 保证静态库消费者连带链接)
  foreach(_agentxx_target ${_AGENTXX_OBJ_LINK_TARGETS})
    target_link_libraries(${_agentxx_target} PRIVATE ${AGENTXX_BUILTIN_PLUGIN_LINK_LIBS})
  endforeach()
  target_link_libraries(agentxx_shared PUBLIC ${AGENTXX_BUILTIN_PLUGIN_LINK_LIBS})
  target_link_libraries(agentxx_static PUBLIC ${AGENTXX_BUILTIN_PLUGIN_LINK_LIBS})
  # 插件资源 (plugin.yaml 等) 先于 lib 目标产出 (运行期目录配置依赖)
  if (TARGET agentxx_builtin_plugin_resources)
    add_dependencies(agentxx_shared agentxx_builtin_plugin_resources)
    add_dependencies(agentxx_static agentxx_builtin_plugin_resources)
    if (TARGET agentxx_obj)
      add_dependencies(agentxx_obj agentxx_builtin_plugin_resources)
    endif()
  endif()
endif()
unset(_AGENTXX_OBJ_LINK_TARGETS)
if (XX_IS_WIN_D)
  target_link_libraries(agentxx_shared PUBLIC d3d11 dxgi dxguid winhttp ws2_32 oleacc ole32 user32 uuid mmdevapi)
  target_link_libraries(agentxx_static PUBLIC d3d11 dxgi dxguid winhttp ws2_32 oleacc ole32 user32 uuid mmdevapi)
endif()

if (AGENTXX_LINUX_IO_URING_SUPPORTED)
  pkg_check_modules(uring REQUIRED IMPORTED_TARGET liburing)
  # - pkg-config 找到的库用 PRIVATE，否则 install 时会被写入依赖中，名称却是当前项目自定义的 PkgConfig::uring,
  # 导致难以被找到，所以这里用 PRIVATE 链接，依赖当前项目的库重新 Find 再链接 liburing.a 即可
  target_link_libraries(agentxx_shared PRIVATE PkgConfig::uring)
  target_link_libraries(agentxx_static PRIVATE PkgConfig::uring)
endif()

install(TARGETS agentxx_shared agentxx_static 
  DESTINATION "${AGENTXX_EXEC_INSTALL_PREFIX}"
)

include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/CPM.cmake")
CPMAddPackage("gh:TheLartians/PackageProject.cmake@1.6.0")
packageProject(
  NAME agentxx_shared 
  VERSION ${PROJECT_VERSION}
  BINARY_DIR "${PROJECT_BINARY_DIR}"
  INCLUDE_DIR "${PROJECT_SOURCE_DIR}/include/"
  INCLUDE_DESTINATION include/
  VERSION_HEADER "agentxx/version.h"
  TARGETS agentxx_shared
  CPACK YES
  DISABLE_VERSION_SUFFIX YES
  DEPENDENCIES "${agentxx_DEPENDENCIES}"
)
packageProject(
  NAME agentxx_static 
  VERSION ${PROJECT_VERSION}
  BINARY_DIR "${PROJECT_BINARY_DIR}"
  INCLUDE_DIR "${PROJECT_SOURCE_DIR}/include/"
  INCLUDE_DESTINATION include/
  VERSION_HEADER "agentxx/version.h"
  TARGETS agentxx_static
  CPACK YES
  DISABLE_VERSION_SUFFIX YES
  DEPENDENCIES "${agentxx_DEPENDENCIES}"
)