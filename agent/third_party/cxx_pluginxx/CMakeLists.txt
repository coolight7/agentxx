# ===== cxx_pluginxx: 插件框架内核 (纯 C ABI 基座 + 通用接口表 + 宿主运行时 + SDK) =====
#
# 定位: 与宿主领域无关的插件框架, 供 agentxx / musicxx 等宿主复用:
#   - include/pluginxx/api/     纯 C ABI (跨边界契约, 与领域无关的部分)
#   - include/pluginxx/kit/     插件侧 C++ SDK (header-only)
#   - include/pluginxx/runtime/ 宿主侧运行时 (header-only)
#   - include/pluginxx/host/    宿主侧通用实现 (装载/清单/注册簿记/通用表装配)
# 依赖: cxx_utilxx_base (取消令牌/日志/JSON 等) + fmt + Boost (仅头文件)
#       **禁止**依赖 neograph / OpenSSL / SQLite 等重依赖与 Boost 编译库
# 产物 (命名规则同 libagentxx):
#   Release: libcxx_pluginxx.so        libcxx_pluginxx_static.a
#   Debug:   libcxx_pluginxxd.so       libcxx_pluginxx_staticd.a
#
# 使用 (处理方):
#   find_package(cxx_pluginxx REQUIRED)
#   target_link_libraries(<your_target> PRIVATE cxx_pluginxx_static)   # 或 _shared

cmake_minimum_required(VERSION 3.10)
cmake_policy(SET CMP0074 NEW)
cmake_policy(SET CMP0091 NEW)
cmake_policy(SET CMP0144 NEW)
cmake_policy(SET CMP0167 OLD)

if (NOT DEFINED AGENTXX_VERSION)
  set(AGENTXX_VERSION "0.1.0")
endif ()

project(cxx_pluginxx VERSION ${AGENTXX_VERSION} LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)

if (XX_IS_MSVC_D)
  list(PREPEND CMAKE_FIND_LIBRARY_SUFFIXES .lib .a)
else ()
  list(PREPEND CMAKE_FIND_LIBRARY_SUFFIXES .a)
endif ()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/xx_platform_macros.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/xx_rpath.cmake")

if (NOT DEFINED AGENTXX_EXEC_INSTALL_PREFIX)
  set(AGENTXX_EXEC_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}")
endif ()

find_package(cxx_utilxx_base REQUIRED)
find_package(Boost CONFIG REQUIRED)   # 仅头文件 (dlopen/清单解析等不使用 Boost 编译库)
find_package(fmt REQUIRED)
# 插件清单 (plugin.yaml) 解析: yaml-cpp 编译库 (体积小, 非"重依赖"范畴)
find_package(yaml-cpp REQUIRED)

set(_CXX_PLUGINXX_SOURCES
  src/api_abi_check.c
  src/loader.cpp
  src/manifest.cpp
  src/version.cpp
)

if (XX_IS_MSVC_D)
  add_library(cxx_pluginxx_shared SHARED ${_CXX_PLUGINXX_SOURCES})
  add_library(cxx_pluginxx_static STATIC ${_CXX_PLUGINXX_SOURCES})
  set(_CXX_PLUGINXX_TARGETS cxx_pluginxx_shared cxx_pluginxx_static)
else ()
  add_library(cxx_pluginxx_obj OBJECT ${_CXX_PLUGINXX_SOURCES})
  set_target_properties(cxx_pluginxx_obj PROPERTIES POSITION_INDEPENDENT_CODE ON)
  add_library(cxx_pluginxx_shared SHARED $<TARGET_OBJECTS:cxx_pluginxx_obj>)
  add_library(cxx_pluginxx_static STATIC $<TARGET_OBJECTS:cxx_pluginxx_obj>)
  set(_CXX_PLUGINXX_TARGETS cxx_pluginxx_obj cxx_pluginxx_shared cxx_pluginxx_static)
endif ()

if (XX_IS_DEBUG_D)
  set_target_properties(cxx_pluginxx_shared PROPERTIES OUTPUT_NAME "cxx_pluginxxd")
  set_target_properties(cxx_pluginxx_static PROPERTIES OUTPUT_NAME "cxx_pluginxx_staticd")
else ()
  set_target_properties(cxx_pluginxx_shared PROPERTIES OUTPUT_NAME "cxx_pluginxx")
  set_target_properties(cxx_pluginxx_static PROPERTIES OUTPUT_NAME "cxx_pluginxx_static")
endif ()

set_target_properties(${_CXX_PLUGINXX_TARGETS} PROPERTIES
  PREFIX "lib"
  POSITION_INDEPENDENT_CODE ON
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED OFF
  LINK_LIBRARIES_STRATEGY "REORDER_MINIMALLY"
)

if (XX_IS_MSVC_D)
  set_target_properties(cxx_pluginxx_shared PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
else ()
  # ELF 导出面控制: 只导出本库命名空间符号, 隐藏静态链入的第三方符号
  set(_CXX_PLUGINXX_MAP "${CMAKE_CURRENT_BINARY_DIR}/pluginxx_symbols.map")
  file(WRITE "${_CXX_PLUGINXX_MAP}" "# cxx_pluginxx 动态库导出白名单 (仅本库命名空间符号)
{
global:
  *pluginxx*;
  *utilxx*;
local: *;
};
")
  target_link_options(cxx_pluginxx_shared PRIVATE "-Wl,--version-script=${_CXX_PLUGINXX_MAP}")
  unset(_CXX_PLUGINXX_MAP)
endif ()

foreach (_t ${_CXX_PLUGINXX_TARGETS})
  target_compile_definitions(${_t} PUBLIC
    ${XX_PLATFORM_COMPILE_DEFINITIONS}
    XX_VERSION_STRING="${AGENTXX_VERSION}"
    BOOST_ASIO_STANDALONE=1
    ASIO_STANDALONE=1
    UTILXX_USE_BOOST_ASIO=1
  )
  if (XX_IS_WIN_D)
    target_compile_definitions(${_t} PUBLIC _WIN32_WINNT=0x0A00)
  endif ()
  target_include_directories(${_t} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  )
  target_include_directories(${_t} SYSTEM PUBLIC
    "${Boost_INCLUDE_DIRS}"
    "${Boost_INCLUDE_DIRS}/boost/"
  )
  target_link_libraries(${_t} PUBLIC cxx_utilxx_base_static fmt::fmt yaml-cpp::yaml-cpp)
endforeach ()
unset(_t)

if (XX_IS_WIN_D)
  # dlopen 等价实现 (LoadLibrary) 与动态库路径处理
  target_link_libraries(cxx_pluginxx_shared PUBLIC ws2_32)
  target_link_libraries(cxx_pluginxx_static PUBLIC ws2_32)
else ()
  target_link_libraries(cxx_pluginxx_shared PUBLIC ${CMAKE_DL_LIBS})
  target_link_libraries(cxx_pluginxx_static PUBLIC ${CMAKE_DL_LIBS})
endif ()

cxx_lib_set_own_dir_rpath(cxx_pluginxx_shared)

# ===== 安装与导出 =====
install(TARGETS cxx_pluginxx_shared cxx_pluginxx_static
  EXPORT cxx_pluginxxTargets
  LIBRARY DESTINATION "${AGENTXX_EXEC_INSTALL_PREFIX}"
  RUNTIME DESTINATION "${AGENTXX_EXEC_INSTALL_PREFIX}"
  ARCHIVE DESTINATION "${AGENTXX_EXEC_INSTALL_PREFIX}"
)
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(EXPORT cxx_pluginxxTargets
  NAMESPACE ""
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cxx_pluginxx"
)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cxx_pluginxxConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/cxx_pluginxxConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cxx_pluginxx"
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/cxx_pluginxxConfig.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cxx_pluginxx"
)
