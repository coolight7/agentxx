cmake_minimum_required(VERSION 3.10)
# find_package 使用 <PackageName>_ROOT 变量
cmake_policy(SET CMP0074 NEW)
# MSVC 运行时库标志由抽象层选择
cmake_policy(SET CMP0091 NEW)
# find_package 使用大写的 _ROOT 变量
cmake_policy(SET CMP0144 NEW)
cmake_policy(SET CMP0167 OLD)

project(agentxx VERSION ${AGENTXX_VERSION} LANGUAGES C CXX)
include(GNUInstallDirs) # CMAKE_INSTALL_*DIR

# 全局版本宏注入
add_compile_definitions(
  AGENTXX_VERSION_STRING="${AGENTXX_VERSION}"
  AGENTXX_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
  AGENTXX_VERSION_MINOR=${PROJECT_VERSION_MINOR}
  AGENTXX_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

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

# 定向 UBSan 探针 (AGENTXX_PLUGIN_UBSAN_PROBE, 见顶层选项说明): 仅对插件框架
# 源文件追加 -fsanitize=undefined; 其余源码保持基线 (通常 ASan) 插桩不变。
if (AGENTXX_PLUGIN_UBSAN_PROBE AND NOT XX_IS_MSVC_D)
  file(GLOB _agentxx_plugin_probe_sources CONFIGURE_DEPENDS "src/plugins/*.cpp")
  set_source_files_properties(${_agentxx_plugin_probe_sources} PROPERTIES
    COMPILE_OPTIONS "-fsanitize=undefined;-fno-sanitize-recover=undefined"
  )
  unset(_agentxx_plugin_probe_sources)
endif()

# ===== 依赖查找 (顺序: 先依赖库的依赖, 再依赖库本体) =====
# 1) 工具库的条件依赖 (io_uring / HyperScan)
# - 工具库 (cxx_utilxx_base / cxx_utilxx) 的导出接口只**声明依赖库名称**
#   (PkgConfig::uring / PkgConfig::hyperscan + 裸库名 hs_runtime), 不含库文件路径;
#   静态库不携带依赖二进制, 谁链接谁解析, 故先在本机查找出这些目标, 再
#   find_package 工具库 (导出目标会校验其 INTERFACE 引用的目标是否存在)
# - 开关与工具库构建时同源 (由 superbuild 下发): io_uring ← 顶层
#   AGENTXX_LINUX_IO_URING_SUPPORTED; HyperScan ← 顶层 AGENTXX_ENABLE_HYPERSCAN
# - 缺查找时 configure 报 "PkgConfig::uring ... target not found", 或运行期 dlopen
#   报 `undefined symbol: io_uring_queue_init`
find_package(PkgConfig REQUIRED)
if (AGENTXX_LINUX_IO_URING_SUPPORTED)
  # io_uring: cxx_utilxx_base 的 asio 文件异步 I/O 探测引用其符号
  # (静态优先: 上方已把 .a 排在 CMAKE_FIND_LIBRARY_SUFFIXES 之前)
  pkg_check_modules(uring REQUIRED IMPORTED_TARGET liburing)
endif ()
if (AGENTXX_ENABLE_HYPERSCAN)
  # HyperScan: cxx_utilxx 的正则实现引用其符号 (hs_runtime 按裸库名随其接口解析)
  pkg_check_modules(hyperscan REQUIRED IMPORTED_TARGET libhs)
endif ()

# 2) 工具库本体: cxx_utilxx_base / cxx_utilxx / cxx_pluginxx
# - 拆分说明: 原 `agentxx_util` 静态库已拆为三个独立 CMake 工程
#   (`agent/third_party/cxx_utilxx_base` 基础件 + `agent/third_party/cxx_utilxx`
#   重依赖工具 + `agent/third_party/cxx_pluginxx` 插件框架内核), 由 superbuild
#   先构建安装, 本处经 find_package 引用其**静态变体** (libagentxx 与各插件各
#   链接一份, 符号互不冲突; 见 AGENTS.md 插件设计约束)
# - 依赖 (fmt/simdjson/Boost/SQLite3/OpenSSL/html2md/iconv/uchardet) 由各包 config
#   的 find_dependency 链解析; 条件依赖目标已由上方按开关查好, 不需要本处重复声明
# - 包含路径/平台宏 (XX_IS_*_D) 随 INTERFACE 传播, 调用方无需自行配置
find_package(cxx_utilxx_base REQUIRED)
find_package(cxx_utilxx REQUIRED)
find_package(cxx_pluginxx REQUIRED)

# ===== 内置插件合并编译 =====
# - add_subdirectory(../plugins) 以内置模式收集启用的插件源文件/依赖库/
#   入口符号 (见 plugins/CMakeLists.txt 内置分支; 不创建任何插件动态库目标)
# - 生成的清单 builtin_plugins.cpp (改名后的入口符号静态表) 一并编译进
#   libagentxx, PluginManager 经 agentxx_plugin_get_builtin_plugins() 查询加载
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../plugins" agentxx_plugins_builtin)
list(APPEND agentxx_SOURCES ${AGENTXX_BUILTIN_PLUGIN_MANIFEST_SOURCE})

# ===== 目标创建 =====
# - GCC/Clang: 通过 OBJECT 库把 62 个源文件只编译一次，共享/静态库复用同一批 .o，
#   编译时间近乎减半。AGENTXX_EXPORTS 等宏在当前代码中已无实际引用，故可安全共用。
#   注意: 不能用 target_link_libraries(shared PRIVATE obj) 关联 OBJECT 库，
#   否则 PackageProject 生成的 install(EXPORT) 会因 obj 不在导出集中而报错；
#   改为把 usage requirements (宏/头文件路径) 统一设置到 obj 与共享/静态库上。
#   对共享/静态库用 PUBLIC 即可: 它们无源文件可编，编译部分静默忽略，
#   INTERFACE 部分照常传播给 client/test；obj 的 INTERFACE 部分无人处理，均无害。
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
  # 注意: 禁止在此添加 /FS /MP 等参数 ——
  # VS18/MSVC 14.51 的 FileTracker 在命令行出现重复 /FS 时会失效 (子编译进程
  # 不写 per-file 跟踪记录)，导致每次构建都判定全部源文件过期而全量重编;
  # 且参数经 superbuild 多层传递极易重复叠加，故统一不使用这两个选项
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

    XX_IS_MINGW_D=${XX_IS_MINGW_D}
    XX_IS_MSVC_D=${XX_IS_MSVC_D}
    XX_IS_GCC_D=${XX_IS_GCC_D}
    XX_IS_CLANG_D=${XX_IS_CLANG_D}

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

# 添加依赖库后应当同步更新 [packageProject:DEPENDENCIES]
# (cxx_utilxx_base/cxx_utilxx/cxx_pluginxx 为 libagentxx 的 PUBLIC 依赖:
#  处理方 find_package agentxx_shared/agentxx_static 时经 find_dependency 传递解析)
set (agentxx_DEPENDENCIES "iconv CONFIG;uchardet;fmt;simdjson;ZLIB CONFIG;OpenSSL;Boost CONFIG;yaml-cpp;CURL CONFIG;SQLite3 CONFIG;html2md;Glob;cxx_utilxx_base;cxx_utilxx;cxx_pluginxx")
# ===== 3) 第三方库 (本项目自编译/自带, 均安装于 AGENTXX_INSTALL_DIR 或 BOOST_ROOT,
# 经 CMAKE_PREFIX_PATH 命中) =====
# - iconv/ZLIB/SQLite3/CURL/Boost 必须显式 CONFIG 模式: 避免 CMake 内置
#   Findiconv/FindZLIB/FindSQLite3/FindCURL/FindBoost 模块 fallback 到系统库
#   (libiconv-native/zlib-ng/sqlite3-cmake/curl/boost 均安装了 config 文件)
# - OpenSSL 保持模块模式 (FindOpenSSL): 本项目 OpenSSL 为预构建目录, 无
#   OpenSSLConfig.cmake, 模块模式经 OPENSSL_ROOT_DIR 精确指向项目预构建目录
find_package(iconv CONFIG REQUIRED)
find_package(uchardet REQUIRED)
find_package(fmt REQUIRED)
find_package(simdjson REQUIRED)
find_package(ZLIB CONFIG REQUIRED)
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
    # neograph 自身头 (cancel/graph 等) 经 install/include 提供, 其内部
    # json.h 依赖的 yyjson.h 经 neograph 源码 deps 目录补齐 (仅构建期,
    # 不再向插件/下游传播; Json 自主化后业务代码不再直引 yyjson)
    "$<BUILD_INTERFACE:${neograph_SOURCE_DIR}/deps/yyjson/>"
  )
  target_include_directories(${_agentxx_target} SYSTEM PUBLIC 
    "${Boost_INCLUDE_DIRS}"
    "${Boost_INCLUDE_DIRS}/boost/"
  )
  target_link_directories(${_agentxx_target} PUBLIC 
    "${BOOST_LIBRARY_DIRS}"
  )
endforeach()
unset(_agentxx_target)

# 工具库的链接依赖: 全部随各包 config 的 find_dependency 链解析 (fmt/simdjson/
# uchardet/iconv/SQLite3/OpenSSL/html2md + Boost 头), include 路径与平台宏经
# INTERFACE 传播, 本处只需链接静态变体:
# - 静态库 (archive) 按目标文件提取: 只用到的模块对应 .cpp.o 会被并入
#   libagentxx, 未使用的模块不产生任何依赖 (DT_NEEDED 实测仅系统库)
# - regex.cpp 的 hyperscan 与 asio 文件 I/O 的 io_uring 依赖由
#   cxx_utilxx(_base) 的 INTERFACE **以目标名声明** (PkgConfig::hyperscan /
#   PkgConfig::uring + 裸库名 hs_runtime), 对应目标已由上方依赖查找段按开关查好

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
if (NOT XX_IS_WIN_D)
    target_link_options(agentxx_shared PRIVATE
      "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/ffi_symbols.map")
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

# 正则 (hyperscan / std::regex) 实现已随工具库拆分迁往 cxx_utilxx:
# 其 INTERFACE 以目标名 (PkgConfig::hyperscan + 裸库名 hs_runtime) 携带该依赖与
# UTILXX_ENABLE_HYPERSCAN 宏, 对应目标由该包 config 解析;
# libagentxx 自身不再直接引用 hs_* 符号, 故此处无需 pkg_check_modules
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
# libagentxx 各目标链接基础库静态变体 (先于 _AGENTXX_LINK_LIBS 声明, 保证链接行
# 顺序: 目标文件 → cxx_utilxx*.a → 其依赖库)
# - agentxx_obj PRIVATE: 仅编译期 usage requirements (头路径/宏)
# - agentxx_shared/static PUBLIC: 目标文件并入共享库; 静态库处理者连带链接
# - cxx_pluginxx_static 加入后 (插件框架内核), 插件宿主实现经其获得通用运行时
foreach(_agentxx_target ${_AGENTXX_OBJ_LINK_TARGETS})
  target_link_libraries(${_agentxx_target} PRIVATE cxx_utilxx_base_static cxx_utilxx_static cxx_pluginxx_static)
endforeach()
target_link_libraries(agentxx_shared PUBLIC cxx_utilxx_base_static cxx_utilxx_static cxx_pluginxx_static)
target_link_libraries(agentxx_static PUBLIC cxx_utilxx_base_static cxx_utilxx_static cxx_pluginxx_static)
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
#   PUBLIC 保证静态库处理者连带链接)
# 内置插件 OBJECT 库目标文件合并进共享/静态库 (各插件 OBJECT 库自带
# per-plugin 编译定义/包含路径, 与 lib 自身源码互不影响)
foreach(_plugin_target ${AGENTXX_BUILTIN_PLUGIN_OBJECT_TARGETS})
  target_sources(agentxx_shared PRIVATE $<TARGET_OBJECTS:${_plugin_target}>)
  target_sources(agentxx_static PRIVATE $<TARGET_OBJECTS:${_plugin_target}>)
endforeach()
# 内置插件依赖库 (quickjs/codegraph 系/平台库; 收集自 plugins 内置分支,
# 与 _AGENTXX_LINK_LIBS 相同链接策略: obj PRIVATE 供编译; 共享/静态库
# PUBLIC 保证静态库处理者连带链接)
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
endif()

if (AGENTXX_LINUX_IO_URING_SUPPORTED)
  # - libagentxx 自身 (asio 文件异步 I/O) 也引用 io_uring 符号, 用 PRIVATE 链接;
  #   静态变体的处理方 (插件/可执行程序) 由 cxx_utilxx_base 包 config 解析出的
  #   同名目标 (PkgConfig::uring) 经其 INTERFACE 连带链接
  target_link_libraries(agentxx_shared PRIVATE PkgConfig::uring)
endif()

# ===== 运行期动态库搜索路径: 优先从动态库自身所在目录搜索 =====
# release 分发会把 libstdc++.so.6 / libgcc_s.so.1 等运行库复制到 exec (与
# libagentxx.so 同目录), 不带该搜索项时加载器不会看自身目录, 目标机器上的旧版
# 系统 libstdc++ 会被优先加载 (详见 cmake/agentxx_runtime_search_path.cmake)
# - agentxx_shared: 安装到 exec, 供其他程序 (含 FFI/dlopen) 使用时优先复用
#   同目录携带的运行库
# - agentxx_static: 静态库无运行期依赖, 调用后直接返回
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/agentxx_runtime_search_path.cmake")
agentxx_set_own_dir_rpath(agentxx_shared)

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
  TARGETS agentxx_static
  CPACK YES
  DISABLE_VERSION_SUFFIX YES
  DEPENDENCIES "${agentxx_DEPENDENCIES}"
)
# 工具库 (cxx_utilxx_base/cxx_utilxx) 与插件框架 (cxx_pluginxx) 已拆为独立
# CMake 工程 (agent/third_party/ 下), 各自手写 config 导出 (不经 PackageProject /
# CPM, 无网络依赖): 其 config 内含完整 find_dependency 链, 处理方
# find_package(agentxx_shared/agentxx_static) 时经上方 agentxx_DEPENDENCIES
# 声明自动解析 (iconv 仍必须 "iconv CONFIG", 理由见 libiconv 相关注释)
