# ===== 运行期动态库搜索路径: 优先使用 "产物自身所在目录" =====
# 背景: release 分发会把运行库 (Linux 的 libstdc++.so.6 / libgcc_s.so.1, Android
# 的 libc++.so, Windows 的 libc++.dll 等) 复制到 {build}/exec, 与可执行文件同
# 目录 (见 script/linux_release_build.sh / script/cross_windows_release_build.sh)。
# 但 ELF 加载器默认**不搜索可执行文件所在目录** (只按 LD_LIBRARY_PATH -> 产物
# 自带的 RUNPATH -> ld.so.cache -> 系统默认目录 查找), 产物不带 RUNPATH 时同目录
# 的运行库形同摆设: 目标机器上更旧/更小的系统 libstdc++ 会被优先加载, 出现
# "version `GLIBCXX_3.4.xx' not found" 之类的运行错误。
# 本文件提供统一入口, 为产物写入 "自身所在目录" 搜索项 (ELF 的 $ORIGIN),
# 加载器会先在其中查找, 再查 ld.so.cache 与系统默认目录。
#
# 平台差异:
# - ELF (Linux/Android): $ORIGIN, 由加载器展开为 "产物自身所在目录";
#   采用链接器默认的 DT_RUNPATH (新式 tag): 搜索顺序在 ld.so.cache 与系统默认
#   目录之前 (仅排在 LD_LIBRARY_PATH 之后), 语义与 patchelf 默认写法一致
# - macOS/iOS: 可执行文件用 @executable_path, 动态库用 @loader_path
# - Windows: DLL 搜索顺序本来就把可执行文件所在目录放在首位 (与 RPATH 无关),
#   MSVC/MinGW 链接参数中也没有对应项, 直接跳过
#
# 用法 (在 lib/client/test/benchmark/plugins 的 CMakeLists.txt 中):
# - 单个目标:
#   include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/agentxx_runtime_search_path.cmake")
#   agentxx_set_own_dir_rpath(<target> [<相对子目录> ...])
# - 目录级默认 (对本目录及子目录之后创建的目标生效):
#   agentxx_set_own_dir_rpath_defaults([<相对子目录> ...])
#
# 参数 (两个入口一致):
# - 相对子目录 (可选, 可多个): 追加 "相对产物自身目录" 的搜索目录, 例如插件
#   动态库位于 exec/plugins/<插件名>/ 时传 "../../" 可回查 exec 目录
#
# 说明: 同时设置 BUILD_RPATH (构建树内的产物) 与 INSTALL_RPATH (经
# install(TARGETS) 安装到 exec 的产物), 两者都由 CMake 写入链接/安装命令。
# 目录级默认入口额外设置 BUILD_WITH_INSTALL_RPATH (产物直接输出到最终位置, 链接期即写入最终值)。

# 内部: 计算某类目标的 "自身所在目录" 搜索项
# - [out_var] 输出变量: 搜索项列表 (当前平台无需设置时返回空)
# - [kind] 目标类别: "executable" 可执行文件 | "library" 动态库
# - 其余参数: 相对产物自身目录的追加子目录
function(_agentxx_own_dir_rpath_entries out_var kind)
  set(_rel_dirs ${ARGN})

  if (XX_IS_WIN_D)
    # Windows 加载器总是先在可执行文件所在目录查找 DLL, 无需设置
    set(${out_var} "" PARENT_SCOPE)
    return()
  endif ()

  if (XX_IS_MACOS_D OR XX_IS_IOS_D)
    # macOS/iOS: 可执行文件自身所在目录 / 动态库自身所在目录
    if (kind STREQUAL "executable")
      set(_base "@executable_path")
    else ()
      set(_base "@loader_path")
    endif ()
  else ()
    # ELF: 加载器展开 $ORIGIN 为 "产物自身所在目录"
    set(_base "\$ORIGIN")
  endif ()

  set(_entries "${_base}")
  foreach (_rel IN LISTS _rel_dirs)
    list(APPEND _entries "${_base}/${_rel}")
  endforeach ()
  set(${out_var} "${_entries}" PARENT_SCOPE)
endfunction()

# 为单个目标设置 "自身所在目录" 运行期搜索路径
# - 可执行文件: 优先从可执行文件所在目录搜索 (release 分发中即 exec 目录)
# - 动态库: 优先从动态库所在目录搜索
# - 静态库/OBJECT 库/自定义目标没有运行期依赖, 调用后直接返回 (无副作用)
function(agentxx_set_own_dir_rpath target)
  if (NOT TARGET ${target})
    message(FATAL_ERROR "[agentxx] agentxx_set_own_dir_rpath: 目标不存在: ${target}")
  endif ()
  get_target_property(_type ${target} TYPE)
  if (_type STREQUAL "EXECUTABLE")
    set(_kind "executable")
  elseif (_type STREQUAL "SHARED_LIBRARY" OR _type STREQUAL "MODULE_LIBRARY")
    set(_kind "library")
  else ()
    return()
  endif ()

  _agentxx_own_dir_rpath_entries(_entries "${_kind}" ${ARGN})
  if (NOT _entries)
    return()
  endif ()
  set_target_properties(${target} PROPERTIES
    BUILD_RPATH "${_entries}"     # 构建树内的产物
    INSTALL_RPATH "${_entries}"   # install(TARGETS) 安装到 exec 的产物
  )
endfunction()

# 目录级默认值: 对本目录及子目录 "之后创建" 的动态库目标生效, 等价于逐个目标
# 调用 agentxx_set_own_dir_rpath (按动态库处理)。插件动态库在各自的插件子目录
# 中创建 (数量多), 用本入口统一设置, 避免逐插件重复调用。
# 额外设置 BUILD_WITH_INSTALL_RPATH: 这类产物由自身 CMakeLists 直接输出到最终
# 位置 (不经 install(TARGETS)), 构建树内的文件就是发布文件, 因此
#   - 构建/安装期无需任何 RPATH 改写 (链接时直接写入最终值)
#   - 不会被追加 CMake 自动推导的构建树搜索路径 (链接目录等构建机绝对路径),
#     RUNPATH 保持为干净的 "自身所在目录 + 相对子目录"
function(agentxx_set_own_dir_rpath_defaults)
  _agentxx_own_dir_rpath_entries(_entries "library" ${ARGN})
  if (NOT _entries)
    return()
  endif ()
  # 目标属性 BUILD_RPATH/INSTALL_RPATH/BUILD_WITH_INSTALL_RPATH 在目标创建时由
  # 这些变量初始化, 故必须在 add_subdirectory 之前调用
  set(CMAKE_BUILD_RPATH "${_entries}" PARENT_SCOPE)
  set(CMAKE_INSTALL_RPATH "${_entries}" PARENT_SCOPE)
  set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE PARENT_SCOPE)
endfunction()
