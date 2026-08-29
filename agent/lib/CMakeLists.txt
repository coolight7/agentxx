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
# 内置合并名单门控: 空/"all" = 全部启用插件内置; 分号列表 = 仅名单内插件内置,
# 名单外插件仍编译为独立动态库 (混合模式)。由顶层 superbuild 统一传入。
if (NOT DEFINED AGENTXX_PLUGIN_BUILTIN_LIST)
  set(AGENTXX_PLUGIN_BUILTIN_LIST "" CACHE STRING
    "Which enabled plugins to merge into libagentxx ('' or 'all' = all; list = only listed ones)")
endif()
# 内置合并模式下各插件的启用开关 (与 plugins/CMakeLists.txt 同名同默认值;
# 由顶层 superbuild 统一传入; 仅 AGENTXX_ENABLE_PLUGIN_BUILTIN=ON 时生效)
option(AGENTXX_ENABLE_PLUGIN_EXAMPLE "Merge example plugin into libagentxx (built-in)." ON)
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

# ===== 工具函数静态库 agentxx_util (主程序与插件共同复用) =====
# - 插件不链接 libagentxx (纯 C ABI 契约), 但 agentxx 内置插件 (与主程序同一
#   superbuild 构建、依赖环境齐全) 可复用 `agent/lib/include/agentxx/util` 的
#   全部工具函数 (字符串/编码转换/路径/http/sqlite/regex 等)。这些代码必须
#   静态链接进主程序与各自插件内 (AGENTS.md 插件设计约束: 兼容不同版本的复用
#   代码编译的主程序/插件), 故独立为静态库, 由 libagentxx 与插件各自链接一份
#   (插件侧符号经导出限制隐藏, 与宿主副本互不冲突)
# - 定位: 本库面向 agentxx 内置插件 (便捷复用); 第三方插件完全不需要它
#   (纯 C ABI 头即可, 甚至不用 C++), 不 find_package 本库即零影响
# - 收录范围: **src/util/ 全部源文件** (http_client/http_server/ws_client/
#   string_util/util/sqlite/settings_db/log/regex/http_header) + include/agentxx/
#   util/ 全部头文件
# - 依赖: **全部 PUBLIC 链接 + PUBLIC include 传递** (内置插件链接本库后直接
#   获得全部 util 能力, 无需自行配置任何依赖):
#   - 轻量 (imported target, config find_dependency 链解析): fmt / SQLite3 /
#     uchardet / iconv
#   - 重依赖: neograph 系 + yyjson (裸库名, 经 PUBLIC link_directories 传播
#     解析; 链接顺序敏感, 与 lib 自身一致); OpenSSL (imported target,
#     find_dependency 链解析, include 路径随 INTERFACE 传递)
#   - 条件 pkg-config 模块: hyperscan (AGENTXX_ENABLE_HYPERSCAN) / uring
#     (AGENTXX_LINUX_IO_URING_SUPPORTED) —— 消费方项目须定义同名 PkgConfig::*
#     target (见 plugins/CMakeLists.txt, 与 client/test 做法一致)
#   - PUBLIC include (install 头/neograph deps/Boost) 由下方 _AGENTXX_USAGE_TARGETS
#     循环提供并随 INTERFACE 导出
# - 裁剪机制: 静态库 (archive) 按目标文件提取 —— 插件只链接实际引用的符号
#   所属的 .cpp.o, 未使用的模块 (如插件不调用 http_client) 整个目标文件不
#   被提取, 其重依赖符号不会进入插件动态库 (DT_NEEDED 实测仅系统库)
# - 平台/编译模式宏 (XX_IS_* 等) 经 PUBLIC 编译定义随 INTERFACE 传播:
#   util 头文件内部 #if XX_IS_WIN_D 等分支依赖这些宏, 插件链接本库后自动获得
# - 注意: 本目标必须在下方内置插件 add_subdirectory 之前创建 (内置模式下
#   插件 OBJECT 库经 target_link_libraries(PRIVATE agentxx_util) 获得
#   编译宏/头路径); 其链接依赖在 find_package 之后另行声明 (见下方)
file(GLOB AGENTXX_UTIL_SOURCES
  CONFIGURE_DEPENDS
  "src/util/*.cpp"
)
# util 全部源文件已独立为 agentxx_util 静态库, 从 libagentxx 源文件集排除
# (避免重复编译; libagentxx 内对 util 函数的调用经链接 agentxx_util 解析)
list(FILTER agentxx_SOURCES EXCLUDE REGEX "/src/util/.*\\.cpp$")

add_library(agentxx_util STATIC ${AGENTXX_UTIL_SOURCES})
set_target_properties(agentxx_util PROPERTIES
  POSITION_INDEPENDENT_CODE ON   # 静态库目标文件并入 libagentxx 共享库/插件动态库
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED OFF
  PREFIX "lib"
  OUTPUT_NAME "agentxx_util"
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
  set(_AGENTXX_USAGE_TARGETS agentxx_shared agentxx_static agentxx_util)
else ()
  add_library(agentxx_obj OBJECT ${agentxx_SOURCES})
  set_target_properties(agentxx_obj PROPERTIES POSITION_INDEPENDENT_CODE ON)
  add_library(agentxx_shared SHARED $<TARGET_OBJECTS:agentxx_obj>)
  add_library(agentxx_static STATIC $<TARGET_OBJECTS:agentxx_obj>)
  set(_AGENTXX_USAGE_TARGETS agentxx_obj agentxx_shared agentxx_static agentxx_util)

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
  # 注意: 禁止在此添加 /FS /MP 等旗标 ——
  # VS18/MSVC 14.51 的 FileTracker 在命令行出现重复 /FS 时会失效 (子编译进程
  # 不写 per-file 跟踪记录)，导致每次构建都判定全部源文件过期而全量重编;
  # 且旗标经 superbuild 多层传递极易重复叠加，故统一不使用这两个选项
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
# (agentxx_util 为 libagentxx 的 PUBLIC 依赖: 消费方 find_package
#  agentxx_shared/agentxx_static 时经 find_dependency(agentxx_util) 传递解析)
set (agentxx_DEPENDENCIES "iconv CONFIG;uchardet;fmt;simdjson;ZLIB CONFIG;OpenSSL;Boost CONFIG;yaml-cpp;CURL CONFIG;SQLite3 CONFIG;html2md;Glob;agentxx_util")
# 必须显式 CONFIG 模式: CMake >= 3.28 内置 Findiconv.cmake 模块在混合模式下
# 永远优先于本项目安装的 iconvConfig.cmake (且仅创建 Iconv::Iconv 目标,
# 无 Iconv::iconv/Iconv::charset, 还可能导致命中系统 iconv);
# CONFIG 模式强制匹配本项目 libiconv-native 安装的 iconvConfig.cmake
find_package(iconv CONFIG REQUIRED)
find_package(uchardet REQUIRED)
find_package(fmt REQUIRED)
find_package(simdjson REQUIRED)
# ZLIB/SQLite3/CURL/Boost 同样必须 CONFIG 模式: 避免 CMake 内置
# FindZLIB/FindSQLite3/FindCURL/FindBoost 模块 fallback 到系统库
# (zlib-ng/sqlite3-cmake/curl/boost 均安装了自己的 config 文件,
# 位于 AGENTXX_INSTALL_DIR 或 BOOST_ROOT, 经 CMAKE_PREFIX_PATH 命中)
find_package(ZLIB CONFIG REQUIRED)
# OpenSSL 保持模块模式 (FindOpenSSL): 本项目 OpenSSL 为预构建目录
# (third_party/OpenSSL-windows-build), 无 OpenSSLConfig.cmake, 模块模式
# 经 OPENSSL_ROOT_DIR 精确指向项目预构建目录, 不会命中系统 OpenSSL
find_package(OpenSSL REQUIRED)
find_package(Boost CONFIG REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(CURL CONFIG REQUIRED)
find_package(SQLite3 CONFIG REQUIRED)
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

# agentxx_util 链接依赖: **全部 PUBLIC** (agentxx 内置插件便捷库, 链接后即得
# 全部 util 能力; 第三方插件不 find_package 本库即零影响)
# - 轻量 (imported target): 插件 find_package(agentxx_util) 后经 config 的
#   find_dependency 链解析 —— string_util.cpp (uchardet 编码检测 + iconv
#   编码转换), util.cpp (fmt), sqlite.cpp/settings_db.cpp (sqlite3)
# - 重依赖 (裸库名 + imported target 混合):
#   - neograph 系 + yyjson 为裸库名 (与 lib 自身 _AGENTXX_LINK_LIBS 一致),
#     经下方 PUBLIC link_directories (AGENTXX_INSTALL_DIR/lib) 传播解析;
#     链接顺序敏感 (sqlite → acp → mcp → async → llm → core)
#   - OpenSSL::SSL / OpenSSL::Crypto 为 imported target, config find_dependency
#     解析, include 路径随 INTERFACE 传递 (插件 include http_client.h 时
#     beast ssl 需要 openssl 头, 无需自行配置)
# - 注意: 链接项必须用 imported target (Iconv::Iconv / fmt::fmt ...) 而非
#   裸库名 (charset): 裸名导出后消费方需自行配置 link_directories, 插件项目
#   无此配置会链接失败; 裸库名仅用于 install/lib 内存在的库 (neograph 系/
#   yyjson/hs_runtime, 经 link_directories 解析)
# - iconv 目标名兼容: Iconv::Iconv 在两种来源下均存在 (libiconv-native
#   config 中为 Iconv::iconv 的 ALIAS, CMake 内置 Findiconv.cmake 模块为
#   原生目标); Iconv::charset 仅 libiconv-native config 提供 (Windows),
#   模块模式 (glibc 系统 iconv) 下不存在, 下方条件链接

# 裸库名依赖解析目录 (PUBLIC 传播): neograph 系/yyjson/hs_runtime 位于
# AGENTXX_INSTALL_DIR/lib, 插件消费本库后自动获得, 无需自行 link_directories
target_link_directories(agentxx_util PUBLIC "${AGENTXX_INSTALL_DIR}/lib/")
target_link_libraries(agentxx_util PUBLIC
  fmt::fmt
  SQLite::SQLite3
  uchardet::libuchardet
  Iconv::iconv
  Iconv::charset
  # === neograph vvv 链接顺序是必要的 (与 lib 自身一致) ===
  neograph_sqlite
  neograph_acp
  neograph_mcp
  neograph_async
  neograph_llm
  neograph_core
  # === neograph ^^^
  yyjson
  OpenSSL::SSL
  OpenSSL::Crypto
)

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

# ===== FFI 导出面控制 (仅作用于 agentxx_shared) =====
# - 目标: 共享库仅导出 ffi_api.h 的 C API 符号 (见 ffi_symbols.map), 供其他
#   语言 FFI 干净调用; 内部 C++ 符号与静态链接第三方库符号全部隐藏
#   (与 plugins/CMakeLists.txt 的导出控制同构, 降低库体积与符号冲突风险)
# - ELF (Linux/Android): version script 白名单 (global: agentxx_*; local: *)
# - macOS: -exported_symbols_list (符号带 _ 前缀)
# - Windows/MSVC: 默认不自动导出, 仅 AGENTXX_FFI_EXPORT/FFI_PLUGIN_EXPORT
#   (__declspec(dllexport)) 标记的符号导出, 无需额外处理
# - agentxx_static 不受影响 (cli/test/benchmark 及 C++ 嵌入方继续用静态库)
if (NOT WIN32)
  if (APPLE)
    set(_AGENTXX_FFI_EXPORT_SYMS "${CMAKE_CURRENT_SOURCE_DIR}/ffi_exported_symbols_darwin.txt")
    file(WRITE "${_AGENTXX_FFI_EXPORT_SYMS}" "\n")
    foreach(_sym agentxx_malloc agentxx_free agentxx_strdup_n
                 agentxx_ffi_api_version agentxx_ffi_library_version agentxx_ffi_strerror
                 agentxx_create agentxx_start agentxx_stop agentxx_destroy
                 agentxx_send_input agentxx_cancel agentxx_select_model agentxx_set_permission
                 agentxx_switch_session agentxx_get_model_info agentxx_get_context_messages
                 agentxx_list_sessions agentxx_interrupt_respond agentxx_drain_logs
                 agentxx_event_queue_create agentxx_event_queue_free
                 agentxx_event_queue_on_event agentxx_event_queue_pop
                 agentxx_get_builtin_plugins)
      file(APPEND "${_AGENTXX_FFI_EXPORT_SYMS}" "_${_sym}\n")
    endforeach()
    target_link_options(agentxx_shared PRIVATE "-Wl,-exported_symbols_list,${_AGENTXX_FFI_EXPORT_SYMS}")
  else()
    target_link_options(agentxx_shared PRIVATE
      "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/ffi_symbols.map")
  endif()
  unset(_AGENTXX_FFI_EXPORT_SYMS)
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
  # agentxx_static 不链接 hyperscan: 静态库的 PRIVATE 依赖导出为
  # $<LINK_ONLY:...> 且排在 INTERFACE 开头 (在 agentxx_util 之前), 而
  # regex.cpp.o 现在位于独立的 agentxx_util.a (在其后), GNU ld 单遍扫描
  # 归档无法回溯解析 hs_* 符号 → 静态库消费者 (client/test/benchmark)
  # 均已自行 PRIVATE 链接 hyperscan, 由它们按自身顺序解析
  # agentxx_util 的 regex.cpp 在 AGENTXX_ENABLE_HYPERSCAN=1 时用 hyperscan
  # 实现 (宏经 _AGENTXX_USAGE_TARGETS 的 PUBLIC 编译定义传播给本库);
  # PUBLIC 链接 (与 agentxx_shared 一致): 导出 INTERFACE 带
  # $<LINK_ONLY:PkgConfig::hyperscan> + hs_runtime 裸名, 消费方项目须
  # pkg_check_modules(libhs) 定义同名 target (见 plugins/CMakeLists.txt,
  # 与 client/test 一致); hs_runtime 裸名经 agentxx_util 的 PUBLIC
  # link_directories (install/lib) 解析
  target_link_libraries(agentxx_util PUBLIC
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
  Iconv::iconv
  Iconv::charset
  uchardet::libuchardet
)
# libagentxx 各目标链接 agentxx_util (先于 _AGENTXX_LINK_LIBS 声明, 保证链接行
# 顺序: 目标文件 → agentxx_util.a → 其依赖库; CMake 会把 agentxx_util 的
# INTERFACE 依赖 (fmt/sqlite3/uchardet/iconv) 紧跟在 agentxx_util 之后展开)
# - agentxx_obj PRIVATE: 仅编译期 usage requirements (头路径/宏)
# - agentxx_shared/static PUBLIC: 目标文件并入共享库; 静态库消费者连带链接
foreach(_agentxx_target ${_AGENTXX_OBJ_LINK_TARGETS})
  target_link_libraries(${_agentxx_target} PRIVATE agentxx_util)
endforeach()
target_link_libraries(agentxx_shared PUBLIC agentxx_util)
target_link_libraries(agentxx_static PUBLIC agentxx_util)
foreach(_agentxx_target ${_AGENTXX_OBJ_LINK_TARGETS})
  target_link_libraries(${_agentxx_target} PRIVATE ${_AGENTXX_LINK_LIBS})
endforeach()
target_link_libraries(agentxx_shared PUBLIC ${_AGENTXX_LINK_LIBS})
target_link_libraries(agentxx_static PUBLIC ${_AGENTXX_LINK_LIBS})
unset(_AGENTXX_LINK_LIBS)
if (AGENTXX_ENABLE_BOOST_PROCESS)
  find_package(Boost CONFIG REQUIRED COMPONENTS process)
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
  # 仅链接 agentxx 自身需要的 Windows 系统库:
  # - ws2_32: asio 网络栈 (http_client/ws_client/http_server/ws_server) 必需
  # - mswsock: AcceptEx/GetAcceptExSockaddrs (asio win_iocp accept) 必需，MinGW 需显式链接
  # - 插件专用库 (d3d11/dxgi/windowscodecs/mmdevapi/ole32/oleacc/user32/winhttp)
  #   由各插件 CMakeLists 声明: 独立模式经插件目标 PRIVATE 链接, 内置合并
  #   模式经 AGENTXX_BUILTIN_PLUGIN_LINK_LIBS 收集 (见各插件内置分支)
  target_link_libraries(agentxx_shared PUBLIC ws2_32 mswsock)
  target_link_libraries(agentxx_static PUBLIC ws2_32 mswsock)
  target_link_libraries(agentxx_util PUBLIC ws2_32 mswsock)
endif()

if (AGENTXX_LINUX_IO_URING_SUPPORTED)
  pkg_check_modules(uring REQUIRED IMPORTED_TARGET liburing)
  # - pkg-config 找到的库用 PRIVATE，否则 install 时会被写入依赖中，名称却是当前项目自定义的 PkgConfig::uring,
  # 导致难以被找到，所以这里用 PRIVATE 链接，依赖当前项目的库重新 Find 再链接 liburing.a 即可
  target_link_libraries(agentxx_shared PRIVATE PkgConfig::uring)
  # agentxx_util: asio 文件 I/O 在 -DASIO_HAS_IO_URING 下引用 io_uring 符号
  # (宏由顶层统一传入所有子项目), PUBLIC 链接使使用 util 重模块 (http_client
  # 等 asio 实现) 的插件直接获得; 消费方项目须 pkg_check_modules(liburing)
  # 定义同名 target (见 plugins/CMakeLists.txt, 与 client/test 一致)
  target_link_libraries(agentxx_util PUBLIC PkgConfig::uring)
endif()

install(TARGETS agentxx_shared agentxx_static agentxx_util
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
# agentxx_util 独立导出: 插件经 find_package(agentxx_util) 引用 (superbuild
# 中 agentxx_plugins_repo DEPENDS agentxx_lib_repo, 安装产物先于插件构建);
# 依赖链由 config 内 find_dependency 传递解析
# (注意: iconv 的 config 文件为 iconvConfig.cmake, find_dependency 须用小写
# 包名 iconv, 大写 Iconv 无法匹配该文件; 且必须显式 CONFIG 模式 ——
# CMake >= 3.28 内置 FindIconv.cmake 模块 (Windows 大小写不敏感会匹配到
# FindIconv.cmake) 中 FPHSA 仅设置大写 Iconv_FOUND, 与 find_dependency(iconv)
# 检查的小写 iconv_FOUND 不匹配, 导致依赖解析误判失败;
# 故下方 DEPENDENCIES 条目写 "iconv CONFIG" (含空格): 经 PackageProject 模板
# (Config.cmake.in 的 find_dependency(${args}) 展开) 生成
# find_dependency(iconv CONFIG), 强制匹配 libiconv-native 安装的
# iconvConfig.cmake, CONFIG 模式成功时 find_package 框架设置精确的小写
# iconv_FOUND=TRUE; 若仅写 "iconv" 则会退化为模块模式, 目标 Iconv::iconv/
# Iconv::charset 不存在, 消费方链接 agentxx_util 时直接报 target not found)
# - OpenSSL::SSL/Crypto 为 INTERFACE_LINK_LIBRARIES 中的 imported target,
#   须在 DEPENDENCIES 中声明以便消费方 find_dependency 解析
# - neograph 系/yyjson/hs_runtime 为裸库名, 经 PUBLIC link_directories 解析,
#   无需 find_dependency; PkgConfig::hyperscan/uring 由消费方项目自行
#   pkg_check_modules 定义 (见 plugins/CMakeLists.txt)
set(agentxx_util_DEPENDENCIES "fmt;SQLite3 CONFIG;uchardet;iconv CONFIG;OpenSSL")
packageProject(
  NAME agentxx_util
  VERSION ${PROJECT_VERSION}
  BINARY_DIR "${PROJECT_BINARY_DIR}"
  INCLUDE_DIR "${PROJECT_SOURCE_DIR}/include/"
  INCLUDE_DESTINATION include/
  TARGETS agentxx_util
  CPACK YES
  DISABLE_VERSION_SUFFIX YES
  DEPENDENCIES "${agentxx_util_DEPENDENCIES}"
)