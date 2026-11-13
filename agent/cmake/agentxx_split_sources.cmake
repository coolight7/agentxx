# 源文件拆分: 把一批源文件做成若干 OBJECT 目标, 让可执行文件/库真正并行编译
#
# 为什么需要: Visual Studio 生成器只在"工程之间"并行编译 —— 同一个 .vcxproj 里的
# ClCompile 是一个接一个跑的 (开 UseMultiToolTask 会隐式带上 /FS, 破坏 FileTracker
# 的按文件增量跟踪: 实测改 6 个文件会重编 56 个, 见 AGENTS.md 里 /FS /MP 的说明)。
# 把源文件分到多个 .vcxproj 才能吃满多核。GCC/Clang 的 make/ninja -j 本身按文件并行,
# 不拆分 (单目标只出一份 PCH, 反而更快)。
#
# 用法:
# ```cmake
# include("${CMAKE_CURRENT_LIST_DIR}/../cmake/agentxx_split_sources.cmake")
# agentxx_split_object_sources(agentxx_test 8 _obj_targets _obj_files ${sources})
# target_sources(agentxx_test PRIVATE ${_obj_files})
# agentxx_inherit_usage_requirements(agentxx_test ${_obj_targets})   # 拷贝 include/宏/链接
# ```
#
# 注意:
# - 生成的目标名是 `<base>_obj_0`、`<base>_obj_1` …
# - `agentxx_inherit_usage_requirements` 用生成器表达式在"生成阶段"读宿主目标的
#   INCLUDE_DIRECTORIES / COMPILE_DEFINITIONS / COMPILE_OPTIONS / LINK_LIBRARIES,
#   因此可以在宿主目标配置完成之后才调用, 不需要把每个 target_* 调用都改写一遍

if (NOT DEFINED AGENTXX_MSVC_SPLIT_OBJECTS)
  # MSVC 下把源文件拆成多个 OBJECT 目标并行编译; 其他编译器不拆
  option(AGENTXX_MSVC_SPLIT_OBJECTS
    "MSVC: split sources into several object targets so the VS generator can compile them in parallel"
    ON)
endif ()

# 把源文件拆成 OBJECT 目标
#
# - `args`:
#     - [base] 目标名前缀 (生成 <base>_obj_0 / <base>_obj_1 …)
#     - [chunk_size] 每块最多源文件数 (每块一个 MSBuild 工程; 块越小并行度越高,
#       但每块都要单独生成一份 PCH, 故不宜太小)
#     - [out_targets] 输出变量名: 创建的 OBJECT 目标列表
#     - [out_objects] 输出变量名: 可直接放进 add_library/add_executable 的
#       `$<TARGET_OBJECTS:...>` 列表
#     - 其余参数 = 源文件列表
function(agentxx_split_object_sources base chunk_size out_targets out_objects)
  set(_sources ${ARGN})
  set(_targets "")
  set(_split OFF)
  if (XX_IS_MSVC_D AND AGENTXX_MSVC_SPLIT_OBJECTS)
    set(_split ON)
  endif()

  if (_split)
    list(LENGTH _sources _count)
    set(_index 0)
    set(_part 0)
    while (_index LESS _count)
      set(_chunk "")
      set(_take 0)
      while (_index LESS _count AND _take LESS chunk_size)
        list(GET _sources ${_index} _one)
        list(APPEND _chunk "${_one}")
        math(EXPR _index "${_index} + 1")
        math(EXPR _take "${_take} + 1")
      endwhile()
      set(_target "${base}_obj_${_part}")
      add_library(${_target} OBJECT ${_chunk})
      set_target_properties(${_target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
      list(APPEND _targets "${_target}")
      math(EXPR _part "${_part} + 1")
    endwhile()
  else()
    add_library(${base}_obj OBJECT ${_sources})
    set_target_properties(${base}_obj PROPERTIES POSITION_INDEPENDENT_CODE ON)
    list(APPEND _targets "${base}_obj")
  endif()

  set(_objects "")
  foreach(_target IN LISTS _targets)
    list(APPEND _objects $<TARGET_OBJECTS:${_target}>)
  endforeach()
  set(${out_targets} "${_targets}" PARENT_SCOPE)
  set(${out_objects} "${_objects}" PARENT_SCOPE)
endfunction()

# 让 OBJECT 目标继承宿主目标的编译/链接要求
#
# - 用生成器表达式读宿主目标的属性, 调用时机在宿主目标配置完成之后即可
# - 链接项一并带过去, 是为了让被链接目标的 INTERFACE 头文件路径/宏也作用于编译
#
# - `args`:
#     - [host] 宿主目标 (可执行文件或库)
#     - 其余参数 = `agentxx_split_object_sources` 给出的 OBJECT 目标
function(agentxx_inherit_usage_requirements host)
  foreach(_target IN LISTS ARGN)
    target_include_directories(${_target} PRIVATE
      $<TARGET_PROPERTY:${host},INCLUDE_DIRECTORIES>)
    target_compile_definitions(${_target} PRIVATE
      $<TARGET_PROPERTY:${host},COMPILE_DEFINITIONS>)
    target_compile_options(${_target} PRIVATE
      $<TARGET_PROPERTY:${host},COMPILE_OPTIONS>)
    target_link_libraries(${_target} PRIVATE
      $<TARGET_PROPERTY:${host},LINK_LIBRARIES>)
  endforeach()
endfunction()

# 标准 PCH 头列表 (lib / client / test 共用)
#
# - 只放**稳定第三方头**: 项目自身的头不能进 PCH, 否则改项目头会触发全部重编
# - 第三方头必须用 <...> 形式: 非 <> 形式会被 CMake 当作源目录相对路径解析,
#   而 fmt/asio/boost 位于 SYSTEM 安装目录 (install/include), 会解析失败
#
# - `args`:
#     - [out_var] 输出变量名 (接到头文件列表)
function(agentxx_pch_headers out_var)
  set(_headers
    <algorithm> <atomic> <chrono> <cstdint> <cstring> <deque> <exception>
    <filesystem> <fstream> <functional> <map> <memory> <mutex> <optional>
    <set> <sstream> <string> <string_view> <thread> <unordered_map>
    <utility> <variant> <vector>
    <fmt/format.h>
    <asio/awaitable.hpp>
    <boost/exception/exception.hpp>
  )
  set(${out_var} "${_headers}" PARENT_SCOPE)
endfunction()

# 给 OBJECT 目标统一挂上 PCH 头列表
#
# - `args`:
#     - [headers] PCH 头列表 (第三方稳定头, 用 <...> 形式; 通常来自
#       [agentxx_pch_headers])
#     - 其余参数 = OBJECT 目标
function(agentxx_apply_pch headers)
  foreach(_target IN LISTS ARGN)
    target_precompile_headers(${_target} PRIVATE ${headers})
    # ccache + PCH 兼容 (GCC 特有): 不加 -fpch-preprocess 时 ccache 无法预处理
    # 含 -include PCH 的源文件, 全部标记 uncacheable, 改动即全部重编
    if (CCACHE_PROGRAM AND XX_IS_GCC_D)
      target_compile_options(${_target} PRIVATE -fpch-preprocess)
    endif()
  endforeach()
endfunction()
