# ===== 平台/编译器宏推导 (XX_IS_*_D) =====
#
# 本文件被三个自研库 (cxx_utilxx_base / cxx_utilxx / cxx_pluginxx) 共用:
# - 嵌套构建 (agentxx superbuild) 时, 顶层已把 XX_IS_*_D 变量经
#   _AGENTXX_COMMON_CMAKE_ARGS 传入, 此处直接沿用, 保证与主工程判定一致;
# - 独立构建 (第三方/其他宿主直接 add_subdirectory 或 find_package 前构建) 时,
#   上层未传入, 此处按 CMAKE_SYSTEM_NAME / 编译器 ID 本地推导。
#
# 用法: include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/xx_platform_macros.cmake")
# 结果: 定义变量 XX_IS_<PLATFORM>_D / XX_IS_<TOOLCHAIN>_D (取值 0 或 1)

if (NOT DEFINED XX_IS_LINUX_D)
  set(XX_IS_LINUX_D 0)
endif ()
if (NOT DEFINED XX_IS_WIN_D)
  set(XX_IS_WIN_D 0)
endif ()
if (NOT DEFINED XX_IS_MACOS_D)
  set(XX_IS_MACOS_D 0)
endif ()
if (NOT DEFINED XX_IS_ANDROID_D)
  set(XX_IS_ANDROID_D 0)
endif ()
if (NOT DEFINED XX_IS_IOS_D)
  set(XX_IS_IOS_D 0)
endif ()
if (NOT DEFINED XX_IS_MSVC_D)
  set(XX_IS_MSVC_D 0)
endif ()
if (NOT DEFINED XX_IS_GCC_D)
  set(XX_IS_GCC_D 0)
endif ()
if (NOT DEFINED XX_IS_CLANG_D)
  set(XX_IS_CLANG_D 0)
endif ()
if (NOT DEFINED XX_IS_MINGW_D)
  set(XX_IS_MINGW_D 0)
endif ()
if (NOT DEFINED XX_IS_DEBUG_D)
  if (CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    set(XX_IS_DEBUG_D 1)
  else ()
    set(XX_IS_DEBUG_D 0)
  endif ()
endif ()
if (NOT DEFINED XX_IS_RELEASE_D)
  if (XX_IS_DEBUG_D)
    set(XX_IS_RELEASE_D 0)
  else ()
    set(XX_IS_RELEASE_D 1)
  endif ()
endif ()

# 平台判定 (仅在上层未给出任何平台标志时执行)
if (NOT XX_IS_LINUX_D AND NOT XX_IS_WIN_D AND NOT XX_IS_MACOS_D
    AND NOT XX_IS_ANDROID_D AND NOT XX_IS_IOS_D)
  if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(XX_IS_WIN_D 1)
  elseif (CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(XX_IS_ANDROID_D 1)
  elseif (APPLE AND CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(XX_IS_MACOS_D 1)
  elseif (APPLE AND CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(XX_IS_IOS_D 1)
  elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(XX_IS_LINUX_D 1)
  endif ()
endif ()

# 编译器判定 (仅在上层未给出任何编译器标志时执行)
if (NOT XX_IS_MSVC_D AND NOT XX_IS_GCC_D AND NOT XX_IS_CLANG_D)
  if (MSVC)
    set(XX_IS_MSVC_D 1)
  elseif (CMAKE_C_COMPILER_ID MATCHES "GNU")
    set(XX_IS_GCC_D 1)
  elseif (CMAKE_C_COMPILER_ID MATCHES "Clang")
    set(XX_IS_CLANG_D 1)
  endif ()
endif ()

# MinGW: Windows 目标 + 非 MSVC 编译器
if (XX_IS_WIN_D AND NOT XX_IS_MSVC_D)
  set(XX_IS_MINGW_D 1)
endif ()

# 供各库的 CMakeLists 统一施加到自己的全部目标 (含 INTERFACE 传播)
set(XX_PLATFORM_COMPILE_DEFINITIONS
  XX_IS_DEBUG_D=${XX_IS_DEBUG_D}
  XX_IS_RELEASE_D=${XX_IS_RELEASE_D}
  XX_IS_LINUX_D=${XX_IS_LINUX_D}
  XX_IS_WIN_D=${XX_IS_WIN_D}
  XX_IS_MACOS_D=${XX_IS_MACOS_D}
  XX_IS_ANDROID_D=${XX_IS_ANDROID_D}
  XX_IS_IOS_D=${XX_IS_IOS_D}
  XX_IS_MINGW_D=${XX_IS_MINGW_D}
  XX_IS_MSVC_D=${XX_IS_MSVC_D}
  XX_IS_GCC_D=${XX_IS_GCC_D}
  XX_IS_CLANG_D=${XX_IS_CLANG_D}
)

message("[xx] platform: linux=${XX_IS_LINUX_D} win=${XX_IS_WIN_D} macos=${XX_IS_MACOS_D} "
        "android=${XX_IS_ANDROID_D} ios=${XX_IS_IOS_D} | toolchain: msvc=${XX_IS_MSVC_D} "
        "gcc=${XX_IS_GCC_D} clang=${XX_IS_CLANG_D} mingw=${XX_IS_MINGW_D} | debug=${XX_IS_DEBUG_D}")
