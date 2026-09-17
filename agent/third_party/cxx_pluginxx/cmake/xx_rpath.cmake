# ===== 运行期动态库搜索路径: 优先使用"产物自身所在目录" =====
#
# 背景: 分发时会把运行库 (Linux 的 libstdc++.so.6 / libgcc_s.so.1 等) 复制到
# 可执行文件同目录, 但 ELF 加载器默认**不搜索产物所在目录**, 不写 RUNPATH 时
# 目标机器上更旧的系统运行库会被优先加载。本文件为动态库写入 "自身所在目录"
# 搜索项 (ELF 的 $ORIGIN)。
#
# 平台差异:
# - ELF (Linux/Android): $ORIGIN, 由加载器展开为"产物自身所在目录"
# - macOS/iOS: 动态库用 @loader_path
# - Windows: DLL 搜索顺序本就把可执行文件目录放在首位, 无需设置
#
# 用法:
#   include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/xx_rpath.cmake")
#   cxx_lib_set_own_dir_rpath(<target> [<相对子目录> ...])

function(cxx_lib_set_own_dir_rpath target)
  if (NOT TARGET ${target})
    message(FATAL_ERROR "[cxx] rpath: 目标不存在: ${target}")
  endif ()
  get_target_property(_type ${target} TYPE)
  if (NOT _type STREQUAL "SHARED_LIBRARY" AND NOT _type STREQUAL "MODULE_LIBRARY")
    return()
  endif ()
  if (XX_IS_WIN_D)
    return()
  endif ()

  if (XX_IS_MACOS_D OR XX_IS_IOS_D)
    set(_base "@loader_path")
  else ()
    set(_base "\$ORIGIN")
  endif ()

  set(_entries "${_base}")
  foreach (_rel IN LISTS ARGN)
    list(APPEND _entries "${_base}/${_rel}")
  endforeach ()

  set_target_properties(${target} PROPERTIES
    BUILD_RPATH "${_entries}"
    INSTALL_RPATH "${_entries}"
  )
endfunction()
