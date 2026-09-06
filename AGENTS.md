# Agentxx
- C++ 23 实现的 AI Agent；编译器启用标准 c++26/c17

## 兼容性
- 跨系统支持:
    - 可编译为独立可执行程序/动态库/静态库，摆脱额外的动态库依赖，仅依赖基本的系统库
    - Linux x86_64 + WSL扩展功能
    - Windows 10+ x86_64
    - Android 5.0+

## 设计 & 建议
- 详细主程序架构设计见[design.md](docs/zh-cn/design.md)，插件设计文档见[plugins.md](docs/zh-cn/plugins.md)，当大幅修改代码时，请参考并更新
- Agent 的设计支持:
    - 并发多会话，单线程/多协程交错执行会话，不需要线程锁
    - client (tui/cli) 主要负责UI渲染展示、用户交互；agent (BaseAgent/CodeAgent) 负责运行会话、调用 llm api、运行 toolcall 等
    - client 应当仅做UI渲染，各种数据来源、消息插入应当尽量由 agent 实现并提供
    - 支持 client+agent 在同一个进程内启动，此时两者使用线程间数据交互
    - 支持 client 通过网络连接 agent server，此时两者在不同进程，通过网络传输交互（已支持 websocket）
    - 应当尽量统一抽象接口，分层屏蔽细节，降低复杂度，让架构设计更清晰
    - `会话 Agent_IO`、`CancelToken`、`上下文统计`、`模型选择` 应当独立记录，按 `thread_id` 取值
- **注释**: 
    - 编写代码时在需要注意的地方、设计描述应当有清晰的注释，请勿随意移除原代码中的注释
    - 注释和功能名称尽量不要用黑话、新名或比喻类比，应当使用简单易懂的词语和语句，比如不要使用 `在途, 触达, 水位, 对标, 赋能, 抓手, 沉淀, 组合拳, 弹药, 倒逼, 脱节, 旗标, 旁路` 等词语
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
- 应当使用 [XX_LOG](agent/lib/include/agentxx/util/log.h) 输出日志，而不是 std::cout/cerr，避免影响 TUI 显示
- 最终的代码实现目标要能稳定运行在生产环境，广泛服务于各种设备和用户，需要仔细思考实现方案、编写足量的常规使用方式测试+各种边界情况测试
- 非必要不应修改 `agent/third_party/` 内的代码，尽量修改本项目的代码实现功能。如果修改了的话应当删除 build 内对应的目录，让 cmake 重新编译，否则可能不生效
- 使用 grep、glob 等工具前参考以下代码结构缩小范围，非必要不应去搜索 `agent/**` 整个代码库，里面包含了 build、third_party 等文件夹太大
- 需要捕获异常时，建议优先考虑 [agentxx::util::catchError 系列](D:\0Acoolight\Program\cpp\agentxx\agent\lib\include\agentxx\util\exception.h)，尤其是协程异常，不应 try {} catch(...) 捕获全部异常，应当使用 `agentxx::util::catchErrorAsync` 放行 取消和中断
- 如果需要编译或运行测试，一般跑 debug 即可

## 代码结构
- `agent`: 
    - C++ 实现 Agent
- `agent/lib`: libagentxx
    - 核心库，包含了内置实现的 BaseAgent/CodeAgent、toolcall、node、middleware 等，分离编译以便嵌入其他 app 开发使用
    - [util](agent/lib/include/agentxx/util/) 一些工具类和函数，包括 `http server/client`、`websocket server/client`、`log`、`lru cache`、`aho_corasick/regex`、[字符串工具](agent/lib/include/agentxx/util/string_util.h) (大小写转换、str转数值、自动检测字符编码并转utf8、计算utf8长度、移除空白符、base64、unix/windows/自动路径标准化、忽略大小写的判断包含/相等、split按char切割字符串)、[异常处理](agent/lib/include/agentxx/util/exception.h)
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
- `agent/third_party`: 第三方库依赖
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
    - [OpenSSL](agent/third_party/openssl-4.0.1/)
    - [simdjson](agent/third_party/simdjson/)
    - [sqlite3] | [sqlite3-cmake](agent/third_party/sqlite3-cmake/)
    - [uchardet](agent/third_party/uchardet/)
    - [yaml-cpp](agent/third_party/yaml-cpp/)
    - [zlib] | [zlib-ng](agent/third_party/zlib-ng/)

## C++插件开发
- 插件接口为 **API v1**: 入口为 `agentxx_plugin_agent_create` /
  `agentxx_plugin_agent_destroy` 实例对 (client 侧 `agentxx_plugin_client_create` / `_destroy`)。
  【API v1 规范】全局 API 版本及全部接口表版本均重置为 1；明确 8 字节结构体对齐与定长基础类型 (`int32_t/int64_t/uint64_t`)；跨边界函数统一 `AGENTXX_PLUGIN_CALL` 调用约定；结构体参数一律传递指针 (`const Struct*`)，结构体返回值一律改为指针出参 (`Struct* out`) 并返回 `int32_t` 状态码；核心 vtable 精简为 `alloc/free` 两件套 (去除了 `strdup`，采用头文件内联 `agentxx_plugin_strdup`)。
  【多实例契约】同一动态库可被同进程内不同 agent 宿主各自创建多个并存实例:
  ① 禁止可变全局/函数级 static 缓存; ② 实例状态只能放 `*plugin_ctx` 堆块,
  回调经 `spec.user_data` 恢复; ③ 接口表查询结果存实例上下文。
  offload线程池适配异步接口 (`plugin_tool_sync.h`) 适配器为调用方内嵌存储 (PluginCtx 成员),
  随实例销毁释放。详见 docs/zh-cn/plugins.md 4.2 节"多实例三铁律"
- 为了尽量保持兼容性，主程序和插件之间的接口只能使用 C Api，不能使用 c++，插件将编译成动态库，然后按接口要求导出接口符号，由主程序运行时加载插件动态库后查找符号调用
- 主程序和插件编译时默认动态链接 c++ 标准库，减少体积；插件也可以自己静态链接c++标准库、libgcc_s
- 主程序和插件可以复用一些代码，比如一些工具函数，这部分复用代码需要静态链接进主程序和各自插件内，确保兼容不同版本的复用代码编译的主程序和插件可以加载运行
- 在主程序和插件的接口中不能传递标准库结构体，也不能传递复用代码里的结构体，这些都只能各自内部使用，且插件编译时应当只导出接口符号，其他符号全部隐藏

已实现的插件设计约束 (2026-08)：
- 导出符号控制: 插件动态库仅导出宿主按名查找的入口符号
  (`agentxx_plugin_agent_get_info/create/destroy` + client 侧 `agentxx_plugin_client_*`),
  由 `AGENTXX_PLUGIN_EXPORT` 宏标记入口函数 (见 `plugin_api.h`);
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
  system_monitor 无 macOS; 跨平台插件默认放行; 见 docs/zh-cn/plugins.md 9.3.1
- 工具函数复用: 插件复用 `agent/lib/include/agentxx/util` 的全部工具函数经独立
  静态库 `agentxx_util` (src/util/ 全部源文件: http_client/http_server/ws_client/
  string_util/util/sqlite/settings_db/log/regex/http_header),
  libagentxx 与插件各自静态链接一份 (符号经导出控制隐藏, 互不冲突);
  插件 CMakeLists: `find_package(agentxx_util)` + `target_link_libraries(PRIVATE agentxx_util)`;
  依赖全部 PUBLIC 传递 (fmt/sqlite3/uchardet/iconv + neograph 系/yyjson/OpenSSL/
  hyperscan/uring 的链接与 include), 插件链接后直接可用全部 util;
  定位为内置插件便捷库 (与主程序同一 superbuild 构建、依赖齐全),
  第三方插件不需要它 (纯 C ABI 头即可, 甚至不用 C++);
  未引用模块按目标文件提取自动裁剪 (9 插件 DT_NEEDED 仅系统库);
  详见 `docs/zh-cn/plugins.md` 4.5/4.6 节

## 编译
- Linux:
    - 使用 shell 脚本编译: [linux_debug_build.sh](agent/script/linux_debug_build.sh) 或 [linux_release_build.sh](agent/script/linux_release_build.sh)
- Windows:
    - 使用 bat 脚本编译: [windows_debug_build.bat](agent/script/windows_debug_build.bat) 或 [windows_release_build.bat](agent/script/windows_release_build.bat)
- Android:
    - 在 Linux 上使用 shell 脚本交叉编译: [cross_android_release_build.sh](agent/script/cross_android_release_build.sh)
- 编译脚本创建的 build 目录一般为:
    - debug_build: `agent/build/linux-debug/` 或 `agent/build/windows-debug/`
    - release_build: `agent/build/linux-release/` 或 `agent/build/windows-release/`
    - cross_android_release_build: `agent/build/android-release/`
    - 注意，修改文件时不建议修改 build 目录内的文件，编译时可能被覆盖
- 为了减少编译输出内容展示，只捕捉关键词，可以参考: `./path/to/linux_debug_build.sh 2>&1 | grep -E -i "Built target|error|warn" | tail -10`

## 常见问题
- Windows/MSVC 禁止添加 `/FS` `/MP` 编译选项 (2026-08): 命令行出现重复 `/FS` 时
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