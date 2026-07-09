cmake_minimum_required(VERSION 3.10)
# find_package 使用 <PackageName>_ROOT 变量
cmake_policy(SET CMP0074 NEW)
# MSVC 运行时库标志由抽象层选择
cmake_policy(SET CMP0091 NEW)
# find_package 使用大写的 _ROOT 变量
cmake_policy(SET CMP0144 NEW)

project(agentxx VERSION 0.1.0 LANGUAGES C CXX)
include(GNUInstallDirs) # CMAKE_INSTALL_*DIR

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

# 创建 动态库/静态库
add_library(agentxx_shared SHARED ${agentxx_SOURCES})
add_library(agentxx_static STATIC ${agentxx_SOURCES})

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
target_compile_definitions(agentxx_shared PUBLIC
  __STDC_CONSTANT_MACROS
  XX_IS_DEBUG_D=${XX_IS_DEBUG_D}
  XX_IS_RELEASE_D=${XX_IS_RELEASE_D}
  XX_BUILD_TYPE=${XX_BUILD_TYPE}

  XX_IS_LINUX_D=${XX_IS_LINUX_D}
  XX_IS_WIN_D=${XX_IS_WIN_D}
  XX_IS_MACOS_D=${XX_IS_MACOS_D}
  XX_IS_ANDROID_D=${XX_IS_ANDROID_D}
  XX_IS_IOS_D=${XX_IS_IOS_D}

  NEOGRAPH_STATIC_BUILD
  NEOGRAPH_USE_BOOST_ASIO
)
target_compile_definitions(agentxx_static PUBLIC
  __STDC_CONSTANT_MACROS
  XX_IS_DEBUG_D=${XX_IS_DEBUG_D}
  XX_IS_RELEASE_D=${XX_IS_RELEASE_D}
  XX_BUILD_TYPE=${XX_BUILD_TYPE}

  XX_IS_LINUX_D=${XX_IS_LINUX_D}
  XX_IS_WIN_D=${XX_IS_WIN_D}
  XX_IS_MACOS_D=${XX_IS_MACOS_D}
  XX_IS_ANDROID_D=${XX_IS_ANDROID_D}
  XX_IS_IOS_D=${XX_IS_IOS_D}

  NEOGRAPH_STATIC_BUILD
  NEOGRAPH_USE_BOOST_ASIO
)
if (AGENTXX_ENABLE_VECTORSCAN)
  target_compile_definitions(agentxx_shared PUBLIC AGENTXX_ENABLE_VECTORSCAN=1)
  target_compile_definitions(agentxx_static PUBLIC AGENTXX_ENABLE_VECTORSCAN=1)
endif()
if (AGENTXX_ENABLE_HYPERSCAN)
  target_compile_definitions(agentxx_shared PUBLIC AGENTXX_ENABLE_HYPERSCAN=1)
  target_compile_definitions(agentxx_static PUBLIC AGENTXX_ENABLE_HYPERSCAN=1)
endif()
if (AGENTXX_ENABLE_CODEGRAPH)
  target_compile_definitions(agentxx_shared PUBLIC AGENTXX_ENABLE_CODEGRAPH=1)
  target_compile_definitions(agentxx_static PUBLIC AGENTXX_ENABLE_CODEGRAPH=1)
endif()
if (AGENTXX_ENABLE_BOOST_PROCESS)
  target_compile_definitions(agentxx_shared PUBLIC AGENTXX_ENABLE_BOOST_PROCESS=1)
  target_compile_definitions(agentxx_static PUBLIC AGENTXX_ENABLE_BOOST_PROCESS=1)
endif()

find_package(PkgConfig REQUIRED)
# 添加依赖库后应当同步更新 [packageProject:DEPENDENCIES]
set (agentxx_DEPENDENCIES "Iconv;uchardet;fmt;simdjson;OpenSSL;Boost;yaml-cpp;CURL;SQLite3;html2md;Glob")
find_package(iconv REQUIRED)
find_package(uchardet REQUIRED)
find_package(fmt REQUIRED)
find_package(simdjson REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Boost REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(html2md REQUIRED)
find_package(Glob REQUIRED)

target_include_directories(agentxx_shared PUBLIC 
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  # 标记为 SYSTEM 防止 include 影响 warning 转 error 判断
  ${AGENTXX_INSTALL_DIR}/include/
  ${neograph_SOURCE_DIR}/deps/yyjson/
  ${neograph_SOURCE_DIR}/deps/
)
target_include_directories(agentxx_static PUBLIC 
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  # 标记为 SYSTEM 防止 include 影响 warning 转 error 判断
  ${AGENTXX_INSTALL_DIR}/include/
  ${neograph_SOURCE_DIR}/deps/yyjson/
  ${neograph_SOURCE_DIR}/deps/
)
if(Boost_FOUND)
  target_include_directories(agentxx_shared SYSTEM PUBLIC 
    ${Boost_INCLUDE_DIRS}
    ${Boost_INCLUDE_DIRS}/boost/
  )
  target_include_directories(agentxx_static SYSTEM PUBLIC 
    ${Boost_INCLUDE_DIRS}
    ${Boost_INCLUDE_DIRS}/boost/
  )
  target_link_directories(agentxx_shared PUBLIC 
    ${BOOST_LIBRARY_DIR}
  )
  target_link_directories(agentxx_static PUBLIC 
    ${BOOST_LIBRARY_DIR}
  )
endif()

# 统一输出名称 libagentxx
# - 不指定时，部分系统，如win可能编译输出缺少前缀 lib
# 生成 libagentxx.so / agentxx.dll
set_target_properties(agentxx_shared PROPERTIES
  OUTPUT_NAME "agentxx"
  PUBLIC_HEADER "include/agentxx/agentxx.h"
  PREFIX "lib"
  CXX_STANDARD 26
  LINK_LIBRARIES_STRATEGY "REORDER_MINIMALLY"
  VS_GLOBAL_VcpkgEnabled false
)
# 生成 libagentxx_static.a / agentxx_static.lib
set_target_properties(agentxx_static PROPERTIES
  OUTPUT_NAME "agentxx_static"
  PUBLIC_HEADER "include/agentxx/agentxx.h"
  PREFIX "lib"
  CXX_STANDARD 26
  LINK_LIBRARIES_STRATEGY "REORDER_MINIMALLY"
  VS_GLOBAL_VcpkgEnabled false
)
target_link_directories(agentxx_shared PUBLIC ${AGENTXX_INSTALL_DIR}/lib/)
target_link_directories(agentxx_static PUBLIC ${AGENTXX_INSTALL_DIR}/lib/)

if (AGENTXX_ENABLE_CODEGRAPH)
  target_include_directories(agentxx_shared PUBLIC ${AGENTXX_INSTALL_DIR}/include/tree_sitter/)
  target_include_directories(agentxx_static PUBLIC ${AGENTXX_INSTALL_DIR}/include/tree_sitter/)
  target_link_libraries(agentxx_shared PUBLIC 
    codegraph_core 
    tree_sitter
    ts_cpp
    ts_python
    ts_javascript
    ts_dart
    ts_typescript
    ts_tsx
    ts_markdown
    ts_markdown_inline
    ts_rust
    ts_bash
    ts_go
    ts_java
    ts_kotlin
    ts_php
    ts_swift
    ts_objc
    ts_csharp
    ts_sql
    ts_lua
    ts_yaml
    ts_json
    ts_xml
    ts_html
    ts_css
    ts_zig
    ts_ruby
    ts_glsl
  )
  target_link_libraries(agentxx_static PUBLIC 
    codegraph_core
    tree_sitter
    ts_cpp
    ts_python
    ts_javascript
    ts_dart
    ts_typescript
    ts_tsx
    ts_markdown
    ts_markdown_inline
    ts_rust
    ts_bash
    ts_go
    ts_java
    ts_kotlin
    ts_php
    ts_swift
    ts_objc
    ts_csharp
    ts_sql
    ts_lua
    ts_yaml
    ts_json
    ts_xml
    ts_html
    ts_css
    ts_zig
    ts_ruby
    ts_glsl
  )
endif()
if (AGENTXX_ENABLE_HYPERSCAN OR AGENTXX_ENABLE_VECTORSCAN)
  pkg_check_modules(hyperscan REQUIRED IMPORTED_TARGET libhs)
  target_link_libraries(agentxx_shared PRIVATE 
    PkgConfig::hyperscan
    hs_runtime
  )
  target_link_libraries(agentxx_static PRIVATE 
    PkgConfig::hyperscan 
    hs_runtime
  )
endif()
target_link_libraries(agentxx_shared PUBLIC
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
target_link_libraries(agentxx_static PUBLIC
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
if (AGENTXX_ENABLE_BOOST_PROCESS)
  find_package(Boost REQUIRED COMPONENTS process)
  target_link_libraries(agentxx_shared PUBLIC Boost::process)
  target_link_libraries(agentxx_static PUBLIC Boost::process)
endif()
if (XX_IS_WIN_D)
  target_link_libraries(agentxx_shared PUBLIC d3d11 dxgi dxguid winhttp ws2_32 oleacc ole32 user32 uuid)
  target_link_libraries(agentxx_static PUBLIC d3d11 dxgi dxguid winhttp ws2_32 oleacc ole32 user32 uuid)
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