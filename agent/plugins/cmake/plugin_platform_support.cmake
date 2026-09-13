# ===== 插件平台支持公共辅助 =====
# 各插件在自身 CMakeLists.txt 开头声明支持的平台并调用
# agentxx_plugin_platform_gate() 判定; 无实现的平台直接 return() 跳过编译
# (内置合并/独立动态库两模式均在子目录入口处拦截), 判定依据与平台矩阵说明
# 见 docs/zh-cn/plugins.md 9.3.1

# 判定当前目标平台是否在声明列表内
# 用法: agentxx_plugin_platform_gate(<插件名> <结果变量> [windows|linux|macos|android|ios ...])
# - 直接以 XX_IS_*_D 标志位匹配, 不引入中间平台标识变量
# - 平台列表为空 = 当前所有平台均无实现 → 一律跳过 (如 audio_stream)
# - 不支持时输出 Skip 提示并置 <结果变量> 为 OFF, 调用方据此 return() 跳过本插件;
#   支持时置 ON
function(agentxx_plugin_platform_gate name out_var)
  set(_ok OFF)
  foreach (_plat IN ITEMS ${ARGN})
    if (_plat STREQUAL "windows" AND XX_IS_WIN_D)
      set(_ok ON)
    elseif (_plat STREQUAL "linux" AND XX_IS_LINUX_D)
      set(_ok ON)
    elseif (_plat STREQUAL "macos" AND XX_IS_MACOS_D)
      set(_ok ON)
    elseif (_plat STREQUAL "android" AND XX_IS_ANDROID_D)
      set(_ok ON)
    elseif (_plat STREQUAL "ios" AND XX_IS_IOS_D)
      set(_ok ON)
    endif ()
  endforeach ()

  if (NOT _ok)
    # 仅用于 Skip 日志展示的当前平台名 (依据 XX_IS_*_D 推导)
    set(_current_platform "unknown")
    if (XX_IS_WIN_D)
      set(_current_platform "windows")
    elseif (XX_IS_LINUX_D)
      set(_current_platform "linux")
    elseif (XX_IS_ANDROID_D)
      set(_current_platform "android")
    elseif (XX_IS_MACOS_D)
      set(_current_platform "macos")
    elseif (XX_IS_IOS_D)
      set(_current_platform "ios")
    endif ()
    message(STATUS "[agentxx] Skip plugin '${name}': no implementation for platform '${_current_platform}'"
                   " (supported: ${ARGN})")
  endif ()

  set(${out_var} "${_ok}" PARENT_SCOPE)
endfunction()
