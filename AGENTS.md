# Agentxx
- C++ 23 实现的 AI Agent；编译器启用标准 c++26/c17

## 兼容性
- 跨系统支持:
    - 可编译为独立可执行程序/动态库/静态库，摆脱额外的动态库依赖，仅依赖基本的系统库
    - Linux x86_64 + WSL扩展功能
    - Windows 10+ x86_64
    - Android 5.0+

## 设计 & 建议
- 详细主程序架构设计见[design](docs/zh-cn/design/index.md)，插件设计文档见[plugins.md](docs/zh-cn/design/plugins.md)，当大幅修改代码时，请参考并更新
- Agent 的设计支持:
    - 并发多会话，单线程/多协程交错执行会话，不需要线程锁
    - client (tui/cli) 主要负责UI渲染展示、用户交互；agent (BaseAgent/CodeAgent) 负责运行会话、调用 llm api、运行 toolcall 等
    - client 应当仅做UI渲染，各种数据来源、消息插入应当尽量由 agent 实现并提供
    - 支持 client+agent 在同一个进程内启动，此时两者使用线程间数据交互
    - 支持 client 通过网络连接 agent server，此时两者在不同进程，通过网络传输交互（已支持 websocket）
    - 应当尽量统一抽象接口，分层屏蔽细节，降低复杂度，让架构设计更清晰
    - `会话 Agent_IO`、`CancelToken`、`上下文统计`、`模型选择` 应当独立记录，按 `thread_id` 取值
- **编写代码注释**: 
    - 编写注释时，统一使用 markdown 风格，多行注释使用 `///` 开头, 示例(其中的 args 和 return 不必每一个都详细说明，尽量对需要注意、不容易从名称了解含义的进行说明):
```c++
/// 英文字母转小写
/// - 支持传入**unicode**，自动判断在 [A, Z] 转换为 [a, z]，非字母返回原值
/// - 相关: 转大写函数见 [charToUpper]
/// 
/// - `args`:
///     - [c] 单字符unicode码点，应当 >= 0
/// 
/// - `return` 字母将被转换为小写，非字母返回原值
int charToLower(int c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}
```
- 合适的情况下，尽量使用`std::string_view`替代`const std::string&`
- 应当使用 [XX_LOG](agent/third_party/cxx_utilxx_base/include/utilxx_base/log.h) 输出日志，而不是 std::cout/cerr，避免影响 TUI 显示
- 最终的代码实现目标要能稳定运行在生产环境，广泛服务于各种设备和用户，需要仔细思考实现方案、编写足量的常规使用方式测试+各种边界情况测试
- 非必要不应修改 `agent/third_party/` 内的代码，尽量修改本项目的代码实现功能。如果修改了的话应当删除 build 内对应的目录，让 cmake 重新编译，否则可能不生效
- 使用 grep、glob 等工具前参考以下代码结构缩小范围，非必要不应去搜索 `agent/**` 整个代码库，里面包含了 build、third_party 等文件夹太大
- 需要捕获异常时，建议优先考虑 [agentxx::util::catchError 系列](D:\0Acoolight\Program\cpp\agentxx\agent\lib\include\agentxx\util\exception.h)，尤其是协程异常，不应 try {} catch(...) 捕获全部异常，应当使用 `agentxx::util::catchErrorAsync` 放行 取消和中断
- 如果需要编译或运行测试，一般跑 debug 即可
- 修改代码后无需自动执行 `clang-format` 等格式化处理

## 代码结构
- `agent`: 
    - C++ 实现 Agent
- `agent/lib`: libagentxx
    - 核心库，包含了内置实现的 BaseAgent/CodeAgent、toolcall、node、middleware 等，分离编译以便嵌入其他 app 开发使用
    - [util](agent/lib/include/agentxx/util/) 与图引擎/宿主耦合的少量头: [exception.h](agent/lib/include/agentxx/util/exception.h) (异常分类 + 统一捕获, 基于 utilxx_base::catchError*)、neograph_json_bridge.h、cancel_adapter.h; 以及宿主专用的数据库工具 [sqlite.h](agent/lib/include/agentxx/util/sqlite.h) (轻量 RAII sqlite3 封装)、[settings_db.h](agent/lib/include/agentxx/util/settings_db.h) (全局设置 KV 库, 实现于 `agent/lib/src/util/`), 其余通用工具已拆为独立工程 (见 `agent/third_party/cxx_utilxx*`)
    - [BaseAgent](agent/lib/include/agentxx/agent/base_agent.h) agent 运行核心基类 (ReAct 循环 + 会话执行)
    - [CodeAgent](agent/lib/include/agentxx/agent/code_agent.h) 继承 BaseAgent, 添加编程工具/中间件
- `agent/client`: 编译结果 {build}/exec/agentxx_cli
    - 命令行可执行程序，用于启动 agent 服务、实现命令行 cli/TUI 交互
    - `agent/client/main.cpp` 通过 --agent 启动时
    - IO交互继承于 [AgentIOBase](agent/lib/include/agentxx/agent/io/agent_io.h) (端点基类)
        - `agent/client/include/agentxx-client/io/stdio` 采用 stdin、stdout、stderr 作为输入输出 `agentxx_cli cli`
        - `agent/client/include/agentxx-client/io/tui` 采用 TUI 作为终端渲染界面交互
        - client/server 为两个 AgentIOBase 端点, 经 transport 双向通信 (进程内 Channel / 远程 WS);
          发送经 `sendToPeer()`, 接收经 `onPeerMessage()` 分发到 protected 被动回调 (onDelta 等)
        - 服务端点为 [SessionServerAgentIO](agent/lib/include/agentxx/agent/io/session_server_agent_io.h)
          (被 BaseAgent 驱动, delta 缓冲/重连重放)
- `agent/test`: 编译结果 {build}/exec/agentxx_test
    - 测试
    - 头文件统一位于 `agent/test/include/agentxx-test/{,core/,plugin/,client/}` (与源码目录
      `core/` `plugin/` `client/` 同名对应), 该目录是测试唯一的 include 根; 源码一律写
      `#include "agentxx-test/core/test_xxx.h"` 这类完整路径, 避免与其它库同名头文件歧义
    - 运行测试示例:
```bash
# 运行所有测试模块，遇到错误也不终止继续运行
path/to/agentxx_test 

# 当任意模块测试存在错误时立即终止测试，未指定时默认无论模块是否存在错误，都完成运行所有测试模块
path/to/agentxx_test --fail-fast
path/to/agentxx_test -f

# - 指定仅运行测试模块 `string_util` `regex`, 其他不运行，默认未指定时运行所有模块
# - 测试模块名称定义见 `agent/test/test.cpp`
path/to/agentxx_test string_util regex
```
- `agent/benchmark`: 编译结果 {build}/exec/agentxx_benchmark
    - 性能测试（一般仅 release 启用编译该模块）
    - 资源基准 (resource_* 模块, 详见 docs/zh-cn/design/benchmark.md):
      同进程 CLI/TUI、真实两进程 (server/client 子进程)、真实运行的 TUI (FTXUI 界面线程,
      含帧耗时/渲染字节)、真实 server 单独运行 (空载漂移/WS 轮次/断连回收)、
      PTY 驱动的真实 TUI 子进程、插件逐项边际内存
    - 指标: RSS/PSS/私有脏页/匿名/峰值/线程/fd + glibc 堆在用与碎片 + malloc_trim
      可回收 + smaps 模块级分解 (可执行文件/项目库/各插件/系统库/堆/匿名) +
      逻辑内存 (会话消息/TUI 状态/工具 schema) + 分阶段增量 + CPU 用户/内核时间
    - 报告: {exec}/bench/bench_<时间戳>.json (机器对比) 与 .md (人工阅读);
      `--baseline <bench_*.json>` 输出与上次的 ΔRSS/ΔPSS/Δ堆 及模块级差异
    - 聚合模块 `resource` 每个场景用独立子进程执行 (避免前序场景内存污染基线);
      `AGENTXX_BENCH_NO_ISOLATE=1` 退回同进程顺序运行
    - 真实两进程场景负载缩放: `AGENTXX_BENCH_SCALE` (0.01~1.0, 默认 1.0)
    - 性能统计 (TUI 帧耗时等) 由 libagentxx 全局标记
      `agentxx::agent::AgentConfigStatic::enableBenchmark` 控制 (默认关闭):
      关闭时统计代码不执行 (热路径只多一次无等待原子读), 基准程序启动时打开
    - 注意: io_context 在 poll() 因"无工作"返回后会被标记 stopped, 导致后续 run()
      立即返回 (客户端收发全失效); 基准场景在任何 poll() 之前先持有 work_guard
- `agent/third_party`: 第三方库依赖 (含本项目自研、按独立工程维护的三个库)
    - [cxx_utilxx_base](agent/third_party/cxx_utilxx_base/) 无重依赖基础件 (日志/JSON/字符串/
      容器/环境/系统探测/取消令牌/异步卸载), 命名空间 `utilxx_base` + 跨库契约 `utilxx::CancelToken`;
      产物 `libcxx_utilxx_base(.so|_static.a)` (Debug 加 `d`)
    - [cxx_utilxx](agent/third_party/cxx_utilxx/) 重依赖工具 (HTTP/WS/正则/路由/差异/
      worktree/散列), 命名空间 `utilxx`, 依赖 cxx_utilxx_base; 产物 `libcxx_utilxx(.so|_static.a)`
      (不含数据库依赖: sqlite 封装与设置库属于宿主, 见 `agentxx/util/`)
    - [cxx_pluginxx](agent/third_party/cxx_pluginxx/) 插件框架内核 (纯 C ABI 基座 pluginxx/api/、
      插件 SDK kit/{kit.h,guard.h}、运行时 runtime/、宿主实现 host/), 依赖 cxx_utilxx_base +
      fmt + yaml-cpp (仅 Boost 头); 产物 `libcxx_pluginxx(.so|_static.a)`
        - SDK 分层: 通用 (跨边界字符串工具/通用表聚合 PluginIfaceCore/Task 锚定协程/锚定原语/
          CancelRegistry/OpCtl/ArgReader/PluginBaseT<IfacesT>/导出宏) 在内核,
          宿主领域 helper 在宿主侧 `agentxx/plugin/api/plugin_kit.h` (umbrella: 包含内核头 +
          逐条 `using` 引入 agentxx::plugin, 插件源码零改动)
        - 通用表实现 (十张表的**定义与实现**) 也在内核: `host/host_core.h`
          (`PluginHostCore<InstanceT>`: 能力/事件/调度/任务/取消投递的通用实现) +
          `host/tables_impl.h` (C ABI 入口 + `queryGenericPluginIface<I,M>`) +
          `host/domain_hooks.h` (`DomainHooks`: 通用表需要宿主数据的入口) +
          `host/event_bus.h` (事件后端 `EventSource` 与订阅句柄);
          agentxx 侧宿主实现见 `agent/lib/src/plugins/plugin_manager_domain_hooks.cpp`
          (事件后端包装 `agentxx::events::EventBus`) 与 `plugin_manager_vtable.cpp`
          (领域表 + `query_interface` 先查通用表)
        - 生命周期骨架也在内核: `host/lifecycle.h` 的 `PluginHostLifecycle<InstanceT>`
          (继承 `PluginHostCore`) 提供装载/启停/禁用启用/卸载/级联依赖/关闭超时重试
          的唯一实现, 宿主只提供接缝 (纯虚 `selfRef`/`createInstance`/`hostVtable`,
          可选 `detachDomainRegistrations`/`clearDomainRegistrations`/
          `applyDeclaredResources`/`releaseInstanceResources`/`onInstanceEnabledChanged`/
          `onInstanceLoaded`/`onInstanceUnloaded`/`cascadeUnloadEnabledOnly`/`logTag`);
          agent 侧 `PluginManager` 与 client 侧 `ClientPluginManager` 都已接入
          (client 侧仅"装载"保留自有实现: dlopen 卸载到内部线程池 + 接口协商 +
          双端入口探测); 实例侧 `destroyPlugin()` 在 `PluginInstanceBase` 共用,
          派生类只需给出 `pluginDestroySymbol()`
    - [boost](agent/third_party/boost/)
        - asio
        - beast
        - process
        - exception
    - [codegraph-cpp](agent/third_party/codegraph-cpp/)
    - [cmark-gfm](agent/third_party/cmark-gfm/)
    - [curl](agent/third_party/curl/)
    - [fmt](agent/third_party/fmt/)
    - [FTXUI](agent/third_party/ftxui/)
    - [glob](agent/third_party/glob/)
    - [html2md](agent/third_party/html2md/)
    - [hyperscan](agent/third_party/hyperscan/)
    - [iconv] | [libiconv-native](agent/third_party/libiconv-native/)
    - [liburing](agent/third_party/liburing/)
    - [NeoGraph](agent/third_party/neograph/)
    - [Markdown-ui](agent/third_party/markdown-ui/)
    - [mimalloc](agent/third_party/mimalloc/) 内存分配器 (Linux/Windows 默认启用; macOS/iOS 无覆盖机制, 自动关闭, 见"编译"节)
    - [OpenSSL](agent/third_party/openssl-4.0.1/)
    - [simdjson](agent/third_party/simdjson/)
    - [sqlite3] | [sqlite3-cmake](agent/third_party/sqlite3-cmake/)
    - [uchardet](agent/third_party/uchardet/)
    - [yaml-cpp](agent/third_party/yaml-cpp/)
    - [zlib] | [zlib-ng](agent/third_party/zlib-ng/)

## C++插件开发
- 插件接口为 **API v1**: 入口为 `agentxx_plugin_agent_create` /
  `agentxx_plugin_agent_destroy` 实例对 (client 侧 `agentxx_plugin_client_create` / `_destroy`)。
  【API v1 规范】全局 API 版本及全部接口表版本均重置为 1；明确 8 字节结构体对齐与定长基础类型 (`int32_t/int64_t/uint64_t`)；跨边界函数统一 `PLUGINXX_CALL` 调用约定；结构体参数一律传递指针 (`const Struct*`)，结构体返回值一律改为指针出参 (`Struct* out`) 并返回 `int32_t` 状态码；核心 vtable 精简为 `alloc/free` 操作 (去除了 `strdup`，采用头文件内联 `pluginxx_strdup`)。
  【生命周期契约 (必备)】所有插件必须导出 `agentxx_plugin_{agent,client}_{start,stop}`:
  `create` 只构造上下文 (`new Ctx + init(host)`, 不注册/不起线程), `start` 是注册事务
  (在宿主 IO 线程执行, 失败返回 `NULL + error` 由宿主回滚), `stop` 撤销自管资源
  (线程/定时器/订阅, 可重复), `destroy` 只释放本地对象。缺失 start/stop 的插件
  宿主拒绝加载 (不再支持 create 期注册的旧形态)。
  SDK 侧用 `AGENTXX_PLUGIN_AGENT_EXPORT(Ctx, Name, Ver, Desc, StartFn, StopFn)`
  (client 侧同名参数) 一次生成五个入口; 手写 create/destroy 的插件用
  `AGENTXX_PLUGIN_{AGENT,CLIENT}_LIFECYCLE_EXPORT(Ctx, StartFn, StopFn)` 只生成 start/stop。
  【多实例契约】同一动态库可被同进程内不同 agent 宿主各自创建多个并存实例:
  ① 禁止可变全局/函数级 static 缓存; ② 实例状态只能放 `*plugin_ctx` 堆块,
  回调经 `spec.user_data` 恢复; ③ 接口表查询结果存实例上下文。
  offload线程池适配异步接口 (`plugin_tool_sync.h`) 适配器为调用方内嵌存储 (PluginCtx 成员),
  随实例销毁释放。详见 docs/zh-cn/design/plugins.md §4 节"多实例三铁律"
- 为了尽量保持兼容性，主程序和插件之间的接口只能使用 C Api，不能使用 c++，插件将编译成动态库，然后按接口要求导出接口符号，由主程序运行时加载插件动态库后查找符号调用
- 主程序和插件编译时默认动态链接 c++ 标准库，减少体积；插件也可以自己静态链接c++标准库、libgcc_s
- 主程序和插件可以复用一些代码，比如一些工具函数，这部分复用代码需要静态链接进主程序和各自插件内，确保兼容不同版本的复用代码编译的主程序和插件可以加载运行
- 在主程序和插件的接口中不能传递标准库结构体，也不能传递复用代码里的结构体，这些都只能各自内部使用，且插件编译时应当只导出接口符号，其他符号全部隐藏

已实现的插件设计约束 (2026-08)：
- 导出符号控制: 插件动态库仅导出宿主按名查找的入口符号
  (`agentxx_plugin_agent_get_info/create/destroy` + client 侧 `agentxx_plugin_client_*`),
  由 `PLUGINXX_EXPORT` 宏标记入口函数 (见 `plugin_api.h`);
  构建侧统一配置: ELF `-fvisibility=hidden` + version script 白名单
  (隐藏第三方静态库符号; 白名单用通配符 `agentxx_plugin_agent_*`/`agentxx_plugin_client_*`,
  兼容单端插件在 Android lld --fatal-warnings 下链接),
  macOS `-exported_symbols_list`, MSVC 不自动导出 (仅 dllexport);
  见 `agent/plugins/CMakeLists.txt` 与各插件 CMakeLists
- 插件平台支持矩阵: 各插件并非全平台适配, 源码无对应平台真实实现时跳过编译;
  支持平台声明于各插件自身 CMakeLists.txt 开头 (经
  `agent/plugins/cmake/plugin_platform_support.cmake` 的 gate 函数判定,
  复用顶层传入的 XX_IS_*_D 变量), screen_capture/computer_use/
  text_selection_monitor 仅 Windows, audio_stream 全平台未实现,
  system_monitor 覆盖 windows/linux/android/macos (macOS 经 mach
  host_statistics + IOKit IOAccelerator 读取 CPU/内存/GPU/显存,
  Apple Silicon 为统一内存: 无独立显存, 用共享内存口径);
  跨平台插件默认放行; 见 docs/zh-cn/design/plugins.md §14
- 工具函数复用: 插件复用 `cxx_utilxx_base` / `cxx_utilxx` 两个独立静态库
  (拆分自原 `agentxx_util`; 基础件 log/json/json_view/string_util/env/system/
  container_util/hash/lru_cache/path_sanitize/stream/async_mutex/asio_error +
  契约 utilxx/cancel.h、utilxx/async_offload.h; 重依赖 http_client/http_server/
  ws_client/router/regex/aho_corasick/diff_util/worktree/crypto ——
  数据库封装 sqlite 与设置库 settings_db 已随本次调整迁回宿主 `agentxx/util/`,
  不进入工具库);
  libagentxx 与插件各自静态链接一份 (符号经导出控制隐藏, 互不冲突);
  插件 CMakeLists: `find_package(cxx_utilxx_base|cxx_utilxx)` +
  `target_link_libraries(PRIVATE cxx_utilxx_base_static|cxx_utilxx_static)`
  (cxx_utilxx 依赖 cxx_utilxx_base, 只链后者时经 INTERFACE 自动带上);
  依赖全部 PUBLIC 传递 (fmt/simdjson/uchardet/iconv + OpenSSL/html2md/Boost 头;
  条件依赖 hyperscan (cxx_utilxx) / io_uring (cxx_utilxx_base: system.cpp 的 asio 文件
  异步 I/O 探测) 在导出接口里**只声明库名**(`PkgConfig::hyperscan` + `hs_runtime` /
  `PkgConfig::uring`), 不含库文件路径; 静态库不携带依赖二进制, 谁链接谁解析 ——
  使用方须**先按开关 `pkg_check_modules` 出这些目标, 再 `find_package` 工具库 /
  agentxx_static** (导出目标会校验 INTERFACE 引用的目标是否已存在): lib/client/test/
  benchmark 按顶层 `AGENTXX_LINUX_IO_URING_SUPPORTED` / `AGENTXX_ENABLE_HYPERSCAN`
  开关判断, plugins 目录查找一次即覆盖全部插件目标 (插件侧无需任何配置);
  两个工具库自身构建按中性开关 `XX_LINUX_IO_URING_SUPPORTED` 判断
  (superbuild 由顶层 AGENTXX_LINUX_IO_URING_SUPPORTED 下发),
  插件链接后直接可用全部工具 (含 `utilxx_base/json.h`/`json_view.h` 自主 Json/JsonView);
  定位为内置插件便捷库 (与主程序同一 superbuild 构建、依赖齐全),
  第三方插件不需要它 (纯 C ABI 头即可, 甚至不用 C++);
  未引用模块按目标文件提取自动裁剪 (9 插件 DT_NEEDED 仅系统库);
  详见 `docs/zh-cn/design/plugins.md` §5 节
- 工具权限限制由插件声明 (2026-09): 插件在注册工具后经 `agentxx.agent.permission`
  接口表 (`register_tool_permission`/`unregister_tool_permission`, SDK 便捷层
  `registerReadPathPermission`/`registerWritePathPermission`/`registerToolPermission`)
  声明自身工具的权限限制 (作用域 读/写、目标来源 无/路径/文本、目标参数名 —— 目标值
  按参数实际 JSON 类型处理: 字符串为单目标、数组自动逐项、可选分类文本);
  权限中间件不再硬编码任何工具名 —— 未声明权限的工具不参与
  权限判定 (直接放行), 声明解析目标后仍由宿主统一按规则判定 (白/黑名单、
  permission.mode、记住的选择、工作区隔离、完全授权)。声明随工具注销/插件禁用卸载
  自动撤销; 非本实例工具名或非法枚举取值会被拒绝并记日志。
  模式/前缀参数工具 (glob/grep/list) 另用同表的 `check_paths` 批量查询实际产生路径的
  三态判定 (DENY/ALLOW/ASK, 不发起询问; SDK: `filterPathPermissions` 等), 逐项丢弃
  被拒或未获批准的路径 —— 声明目标只决定"是否询问一次", 逐路径复核才保证子目录
  拒绝规则不被 `**` 模式绕过; 详见 `docs/zh-cn/design/plugins.md` §8 节

## 编译
- 平台/编译器宏: 顶层 `agent/CMakeLists.txt` 统一判定并经 `_AGENTXX_COMMON_CMAKE_ARGS`
  传入嵌套构建, 代码中一律使用 `XX_IS_*_D` (勿使用编译器内置平台宏):
  - 平台: `XX_IS_LINUX_D` / `XX_IS_WIN_D` / `XX_IS_MACOS_D` / `XX_IS_ANDROID_D` / `XX_IS_IOS_D`
  - 工具链: `XX_IS_MSVC_D` / `XX_IS_GCC_D` / `XX_IS_CLANG_D` /
    `XX_IS_MINGW_D` (Windows 目标 + 非 MSVC, 取代 `__MINGW32__`; 独立构建
    plugins 目录时由 `plugins/cmake/plugin_platform_support.cmake` 本地推导)
- 工具库 (cxx_utilxx_base/cxx_utilxx/cxx_pluginxx) 的条件依赖 (io_uring/hyperscan)
  不在导出接口里写死路径, 只声明库名 (`PkgConfig::uring` /
  `PkgConfig::hyperscan` + 裸库名 `hs_runtime`); 各构建目录在自己的 CMakeLists
  依赖查找段直接 `pkg_check_modules` (顺序: 先查依赖库的依赖, 再 find_package
  工具库本体 / agentxx_static): lib/client/test/benchmark/plugins 按顶层
  `AGENTXX_LINUX_IO_URING_SUPPORTED` / `AGENTXX_ENABLE_HYPERSCAN` 开关判断,
  cxx_utilxx/cxx_pluginxx 自身构建按中性 `XX_LINUX_IO_URING_SUPPORTED` 判断
  (superbuild 由顶层 AGENTXX_LINUX_IO_URING_SUPPORTED 下发; cxx_utilxx_base 的
  构建开关 CXX_UTILXX_BASE_LINUX_IO_URING_SUPPORTED 与之同源, 一并下发)
- 三个自研工具库 (cxx_utilxx_base/cxx_utilxx/cxx_pluginxx) 按独立发布维护:
  库内不使用宿主专名, 构建变量统一 `XX_*` 前缀 (`XX_INSTALL_DIR` /
  `XX_EXEC_INSTALL_PREFIX` / `XX_LINUX_IO_URING_SUPPORTED` + `XX_IS_*_D` 平台宏),
  未传入时各自回退默认值; superbuild 在公共参数里下发 XX_* 取值
  (见 agent/CMakeLists.txt 的 _AGENTXX_COMMON_CMAKE_ARGS)
- Linux:
    - 使用 shell 脚本编译: [linux_debug_build.sh](agent/script/linux_debug_build.sh) 或 [linux_release_build.sh](agent/script/linux_release_build.sh)
- Windows:
    - 使用 bat 脚本编译: [windows_debug_build.bat](agent/script/windows_debug_build.bat) 或 [windows_release_build.bat](agent/script/windows_release_build.bat)
- macOS:
    - 使用 shell 脚本编译: [macos_debug_build.sh](agent/script/macos_debug_build.sh) 或 [macos_release_build.sh](agent/script/macos_release_build.sh)
      (依赖自构建 Boost/OpenSSL; arm64 默认关闭 hyperscan; release strip 后自动
      ad-hoc 重签名; mimalloc 在 macOS 自动关闭, 见上)
- Android:
    - 在 Linux 上使用 shell 脚本交叉编译: [cross_android_release_build.sh](agent/script/cross_android_release_build.sh)
- 编译脚本创建的 build 目录一般为:
    - debug_build: `agent/build/linux-debug/` 或 `agent/build/windows-debug/` 或 `agent/build/macos-debug/`
    - release_build: `agent/build/linux-release/` 或 `agent/build/windows-release/` 或 `agent/build/macos-release/`
    - cross_android_release_build: `agent/build/android-release/`
    - 注意，修改文件时不建议修改 build 目录内的文件，编译时可能被覆盖
- LTO (Release 默认开启, 开关 `AGENTXX_ENABLE_LTO`): MSVC 用 `/GL`(编译)+`/LTCG`(链接),
  GCC/Clang 用 `-flto=auto`/`-flto=thin`; **全部产物都参与 LTO** (无例外项):
  - 三库 (`cxx_utilxx_base`/`cxx_utilxx`/`cxx_pluginxx`) 动态库改为**显式导出**:
    头文件用 `UTILXX_BASE_API`/`UTILXX_API`/`PLUGINXX_API` 标注 (dllexport /
    visibility default), 默认不导出任何符号 —— 因为 `WINDOWS_EXPORT_ALL_SYMBOLS`
    要解析 `.obj` 符号表生成 `.def`, 而 `/GL` 产物无符号表 (dumpbin:
    `File Type: ANONYMOUS OBJECT`), 二者互斥。
    实测导出数: base 3144→208, utilxx 46163→119, pluginxx 1273→27 (无公开 API 缺失);
    静态使用方由目标接口定义 `CXX_*_STATIC` → 宏为空 (非 dllimport)
  - hyperscan: fat runtime (运行期多微架构分发, 仅 Linux) **固定禁用** (与 LTO 互斥),
    改用编译期基线 ISA (按目标架构自动选择: x86_64 → `x86-64-v2`, x86 → `core2`),
    从而 hyperscan 也参与 LTO
- 链接期体积优化 (Release, GCC/Clang, 非交叉, 仅 Linux/Android):
  - ICF 相同代码合并 `-Wl,--icf=all` (开关 `AGENTXX_ENABLE_ICF`, 默认 ON):
    仅 mold/gold/lld 支持, 用默认 bfd 时自动跳过。实测体积收益 (已 strip 产物):
    `agentxx_cli` -3.2%, `libagentxx.so` -2.7% (未 strip 产物因符号表合并看起来更多)
  - 符号表裁剪仍由发布脚本完成 (`script/*_build.sh` 的 `strip`), 不在链接期做
    (直接 `cmake --build` 的产物会保留符号表, 属预期)
- 内存分配器 (mimalloc, 默认启用, 见 docs/zh-cn/design/index.md "内存占用与分配器调整"):
  - 开关在顶层 `agent/CMakeLists.txt`: `AGENTXX_ENABLE_MIMALLOC` (默认 ON) 与
    `AGENTXX_MIMALLOC_LINK=STATIC|SHARED` (默认 STATIC, 静态并入产物);
    源码是 `agent/third_party/mimalloc` 子模块, 经 ExternalProject 构建安装
    (`MI_INSTALL_TOPLEVEL` / `MI_OPT_ARCH=OFF` 通用 CPU 基线 / `MI_ALLOW_THP=OFF`)
  - 只作用于**最终程序** (`agentxx_cli`/`agentxx_test`/`agentxx_benchmark`), 接入逻辑
    在 `agent/cmake/agentxx_mimalloc.cmake`: STATIC 用 `-Wl,-u,malloc` 保证从静态库
    取出定义 malloc/free 的目标文件 (程序自身定义进入动态符号表 → 同进程的
    libstdc++/dlopen 插件也走 mimalloc, 不会跨模块 free 不匹配); SHARED 用
    `-u mi_version` 保留 `libmimalloc.so` 依赖, 并把 SONAME 文件装到 exec
  - 插件 (agent/plugins) 不接入 (内置合并进 libagentxx 的随最终程序用 mimalloc,
    独立编译的插件动态库跟随宿主进程); `libagentxx.so` 自身也不链接分配器
  - THP 必须关: 上游 Linux 默认 `MI_ALLOW_THP=FULL` 时按 2 MB 大页保留内存,
    实测 RSS 翻倍 (19.6 → 37.4 MB); 需要 THP 时运行时用 `MIMALLOC_ALLOW_THP=1`
  - sanitizer 与 mimalloc 互斥 (ASan 需独占 malloc, 同开会启动崩溃): 非 Release 且
    `AGENTXX_ENABLE_SANITIZER=ON` 时顶层自动关闭 mimalloc
  - Windows/MSVC + 动态 CRT (`/MD`) 下静态覆盖不生效 (上游以 `_DLL` 判定), 需真正
    接管分配器要用 `-DAGENTXX_MIMALLOC_LINK=SHARED`
  - macOS/iOS 无可用覆盖机制, 顶层**自动关闭** (`XX_IS_MACOS_D`/`XX_IS_IOS_D`
    时强制 `AGENTXX_ENABLE_MIMALLOC=OFF`): Mach-O 采用两层次命名空间, libc++/
    系统框架对 `malloc/free` 的引用在链接期已绑定 libSystem, 主可执行文件的静态
    覆盖只作用于自身 (且 `_malloc` 不在导出符号表中), 跨模块释放会在 `mi_free`
    崩溃 (实测 Release `agentxx_cli` 启动即崩); 这与 ELF 不同 (GNU ld 把被共享库
    引用的 `malloc` 放入 `.dynsym` 从而全进程插入)。强制接管只能用
    `DYLD_INSERT_LIBRARIES` 预加载 mimalloc 动态库 (非自包含分发)
  - 实测 (Release, 对比调优后的 glibc): 200K 上下文服务端 RSS +5.5~6.6 MB, 但 user
    -27% / sys -67% / wall -25%; 数据见 docs/zh-cn/design/benchmark.md 第 9 节
- 内存占用优化 (与体积无关, 见 docs/zh-cn/design/index.md "内存占用与分配器调整"):
  - `agentxx::util::tuneProcessAllocator()` (BaseAgent 构造时调用; 客户端 main 也调用):
    glibc 下 `mallopt(M_ARENA_MAX, 1)` —— 线程多时 glibc 默认每个线程建一个 arena
    (每个预留 64 MB 地址空间), 实测 VmSize 610 MB → 290 MB / 1285 MB → 387 MB;
    Windows 无对应参数 (空实现)
  - `releaseFreeHeapPages()` (轮末调用): glibc `malloc_trim(0)` / Windows `_heapmin`,
    归还堆内空闲页 (实测服务端 RSS -1.7 ~ -2.8 MB)
  - 未固定 `M_MMAP_THRESHOLD`/`M_TRIM_THRESHOLD`: 固定后 mmap/munmap 系统调用激增
    (服务端系统态时间 +60%, 场景耗时 +15%), 收益与轮末主动归还重合
- Debug 插桩: 单一开关 `AGENTXX_ENABLE_SANITIZER` (默认 ON) 同时启用 ASan + UBSan
  (GCC/Clang: `-fsanitize=address` + `-fsanitize=undefined -fno-sanitize-recover=undefined`)
  与插件框架定向探针 (`lib/src/plugins/*.cpp`、`test/plugin/*.cpp`); MSVC 只有 ASan
  (`/fsanitize=address`), UBSan/探针自动忽略; 已移除 TSAN 支持。
  插桩与探针**都只在非 Release 配置生效** (`XX_IS_RELEASE_D=0`): Release 无 sanitizer
  编译/链接参数, 探针若仍插桩会因缺少 `__ubsan_handle_*` 运行库符号而链接失败
- 为了减少编译输出内容展示，只捕捉关键词，可以参考: `./path/to/linux_debug_build.sh 2>&1 | grep -E -i "Built target|error|warn" | tail -10`

## 常见问题
- Windows/MSVC 禁止添加 `/FS` `/MP` 编译选项: 命令行出现重复 `/FS` 时
  VS18/MSVC 14.51 的 FileTracker 会失效 (子编译进程不写 per-file 跟踪记录),
  导致每次构建都全量重编 (增量编译完全失效)。参数经 superbuild 多层 CMake
  传递极易重复叠加, 故项目统一不使用这两个选项, 各 CMakeLists 中已有注释标记
- 如果遇到编译器崩溃 (ICE)，直接重新运行编译尝试即可; 也可能是内存不足或内存中的缓存占用太多了，可以清理一下再编译试试; 如果多次运行都崩溃，则可能确实代码有问题，需要重新检查一下。
```sh
# 清理内存缓存
sudo echo 1 > /proc/sys/vm/drop_caches
sudo echo 2 > /proc/sys/vm/drop_caches
sudo echo 3 > /proc/sys/vm/drop_caches
```
- 编译如果警告`不应忽略函数返回值`时，如果函数返回值是协程值(比如asio::awaitable<>)则必须处理，需要 co_await，否则该协程函数没有启动执行，相当于没调用