# ===== 内存分配器 (mimalloc) 接入: 最终程序统一入口 =====
# https://github.com/microsoft/mimalloc
#
# 背景: 默认的 glibc / MSVC CRT 分配器在多线程、多尺寸混合的应用里容易留下碎片
# 与长期不归还的空闲页 (本项目此前用 M_ARENA_MAX / malloc_trim 缓解, 见
# agentxx/util/allocator_tuning.h)。启用 mimalloc 后由它接管 malloc/free/
# new/delete, 内存按线程本地堆分配, 空闲页及时归还系统。
#
# 作用范围 (只作用于**最终程序**, 不作用于动态库):
# - agentxx_cli / agentxx_test / agentxx_benchmark: 由本文件接入分配器
# - libagentxx.so (FFI/嵌入) 与插件动态库: 不接入, 跟随宿主进程的分配器
#   (动态库若自行覆盖 malloc, 宿主的 SQL/运行时等模块仍走各自的分配器, 反而
#    可能跨模块 free 不匹配); 内置合并进 libagentxx 的插件随最终程序一起使用
#   mimalloc
# - 宿主进程使用静态链接的插件源码 (内置合并) 时, 分配器由最终程序统一提供
#
# 用法 (在 client/test/benchmark 的目标定义之后调用; SHARED 模式会追加目标的
# 构建期搜索路径, 故需排在 agentxx_set_own_dir_rpath 之后):
#   include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/agentxx_mimalloc.cmake")
#   agentxx_mimalloc_attach(<target>)
#
# 开关 (顶层 agent/CMakeLists.txt 定义, 经嵌套构建下发):
# - AGENTXX_ENABLE_MIMALLOC: 是否启用 (Debug + sanitizer 场景顶层会自动关闭:
#   ASan 需要独占 malloc, 与分配器覆盖互斥)
# - AGENTXX_MIMALLOC_LINK: STATIC (默认, 分配器并入产物) / SHARED (链接
#   libmimalloc.so / mimalloc.dll)
#
# 链接机制说明 (两种方式都要让分配器**真正**生效, 而不是仅仅链上):
# - STATIC: 程序自身的 malloc/free 引用在链接期解析到 mimalloc 的定义, 定义随
#   程序进入动态符号表 (被 libc/libstdc++ 等动态库引用), 于是同一进程内动态库、
#   dlopen 的插件也一起走 mimalloc —— 全程只有一个分配器, 不会出现跨模块 free
#   不匹配。用 `-u malloc` 强制从静态库取出定义 malloc/free 的目标文件, 保证
#   覆盖一定生效 (避免依赖"恰好有未定义引用才提取归档成员"的提取时机)
# - SHARED: libmimalloc.so 排在 libc 之前加入依赖列表, 加载器按顺序解析符号,
#   同样全进程接管; 用 `-u mi_version` / `/INCLUDE:mi_version` 强制保留该依赖
#   (程序自身可能没有直接引用 mimalloc 符号, 否则 --as-needed / 按需导入会把
#   分配器库丢掉, 覆盖静默失效)

# 把 mimalloc 接入指定目标 (分配器成为该程序的 malloc/free)
#
# - `target` 目标名 (可执行文件; 静态/动态库不接入, 见文件头说明)
function(agentxx_mimalloc_attach target)
  if (NOT AGENTXX_ENABLE_MIMALLOC)
    # 顶层未启用, 或在 Debug + sanitizer 场景被自动关闭 (见顶层选项说明)
    return()
  endif ()
  if (NOT TARGET ${target})
    message(FATAL_ERROR "[agentxx] agentxx_mimalloc_attach: 目标不存在: ${target}")
  endif ()

  find_package(mimalloc REQUIRED)

  if (AGENTXX_MIMALLOC_LINK STREQUAL "SHARED")
    # 动态链接: 分配器由 libmimalloc.so / mimalloc.dll 提供
    target_link_libraries(${target} PRIVATE mimalloc)
    if (XX_IS_MSVC_D)
      # Windows: 保证导入表里保留 mimalloc.dll 条目 (否则 DLL 不会被加载,
      # 覆盖静默失效); mimalloc.dll 依赖同目录的 mimalloc-redirect.dll, 后者
      # 在加载时把 CRT 的分配入口改指到 mimalloc
      target_link_options(${target} PRIVATE "/INCLUDE:mi_version")
    elseif (XX_IS_MACOS_D OR XX_IS_IOS_D)
      # Mach-O 符号名带前导下划线, ld64 的 -u 不做前缀补全, 需写 _mi_version
      target_link_options(${target} PRIVATE "LINKER:-u,_mi_version")
    else ()
      target_link_options(${target} PRIVATE "LINKER:-u,mi_version")
    endif ()

    # 运行期搜索路径: 构建树内直接运行产物时, 从分配器所在目录取库
    # (安装后的产物由 INSTALL_RPATH=$ORIGIN 从自身目录取, 见下方拷贝规则)
    get_target_property(_mi_location mimalloc IMPORTED_LOCATION)
    if (_mi_location)
      get_filename_component(_mi_lib_dir "${_mi_location}" DIRECTORY)
      get_target_property(_mi_build_rpath ${target} BUILD_RPATH)
      if (NOT _mi_build_rpath)
        set(_mi_build_rpath "")
      endif ()
      set_target_properties(${target} PROPERTIES
        BUILD_RPATH "${_mi_build_rpath};${_mi_lib_dir}")
      unset(_mi_lib_dir)
    endif ()
    unset(_mi_location)

    # 把分配器动态库放进产物目录 (exec): 与可执行文件同目录, 随产物一起分发
    # - ELF/macOS: 用 SONAME 文件名拷贝 (加载器按 DT_NEEDED 的 SONAME 查找,
    #   直接拷贝真实文件 libmimalloc.so.3.5 会找不到)
    # - Windows: 加壳拷贝 mimalloc.dll 与其依赖 mimalloc-redirect.dll
    if (XX_IS_WIN_D)
      set(_mi_deploy_files "$<TARGET_FILE:mimalloc>")
      if (EXISTS "${AGENTXX_INSTALL_DIR}/bin/mimalloc-redirect.dll")
        list(APPEND _mi_deploy_files "${AGENTXX_INSTALL_DIR}/bin/mimalloc-redirect.dll")
      endif ()
      install(FILES ${_mi_deploy_files} DESTINATION "${AGENTXX_EXEC_INSTALL_PREFIX}")
      unset(_mi_deploy_files)
    else ()
      install(FILES "$<TARGET_FILE:mimalloc>"
        DESTINATION "${AGENTXX_EXEC_INSTALL_PREFIX}"
        RENAME "$<TARGET_SONAME_FILE_NAME:mimalloc>")
    endif ()
    message("[agentxx] mimalloc (SHARED) -> ${target}")
  else ()
    # 静态链接 (默认): 分配器静态库并入产物, 无额外运行库依赖
    target_link_libraries(${target} PRIVATE mimalloc-static)
    if (XX_IS_MSVC_D)
      target_link_options(${target} PRIVATE "/INCLUDE:malloc")
    elseif (XX_IS_MACOS_D OR XX_IS_IOS_D)
      # Mach-O 符号名带前导下划线 (ld64 的 -u 不自动补全)
      target_link_options(${target} PRIVATE "LINKER:-u,_malloc")
    else ()
      target_link_options(${target} PRIVATE "LINKER:-u,malloc")
    endif ()
    message("[agentxx] mimalloc (STATIC) -> ${target}")
  endif ()

  # 供程序代码按需使用 (包含 <mimalloc.h> 记录版本 / 输出统计);
  # 头文件路径随上面链接的导入目标传播 (INTERFACE_INCLUDE_DIRECTORIES)
  target_compile_definitions(${target} PRIVATE AGENTXX_ENABLE_MIMALLOC_D=1)
endfunction()
