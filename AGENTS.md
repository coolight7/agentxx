# Agentxx
- C++ 23 实现的 AI Agent；编译器启用标准 c++26/c17

## 兼容性
- 跨系统支持:
    - 可编译为独立可执行程序/动态库/静态库，摆脱额外的动态库依赖，仅依赖基本的系统库
    - Linux x86_64 + WSL扩展功能
    - Windows 10+ x86_64
    - Android 5.0+

## 设计 & 建议
- 详细架构设计见[design.md](docs/zh-cn/design.md)，当大幅修改代码时，请参考并更新
- Agent 的设计支持:
    - 并发多会话，单线程/多协程交错执行会话，不需要线程锁
    - client (tui/cli) 主要负责UI渲染展示、用户交互；agent (BaseAgent/CodeAgent) 负责运行会话、调用 llm api、运行 toolcall 等
    - client 应当仅做UI渲染，各种数据来源、消息插入应当尽量由 agent 实现并提供
    - 支持 client+agent 在同一个进程内启动，此时两者使用线程间数据交互
    - 支持 client 通过网络连接 agent server，此时两者在不同进程，通过网络传输交互（已支持 websocket）
    - 应当尽量统一抽象接口，分层屏蔽细节，降低复杂度，让架构设计更清晰
    - `会话 Agent_IO`、`CancelToken`、`上下文统计`、`模型选择` 应当独立记录，按 `thread_id` 取值
- 编写代码时在需要注意的地方、设计描述应当有清晰的注释，请勿随意移除原代码中的注释
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
- 为了尽量保持兼容性，主程序和插件之间的接口只能使用 C Api，不能使用 c++，插件将编译成动态库，然后按接口要求导出接口符号，由主程序运行时加载插件动态库后查找符号调用
- 主程序和插件编译时默认动态链接 c++ 标准库，减少体积；插件也可以自己静态链接c++标准库、libgcc_s
- 主程序和插件可以复用一些代码，比如一些工具函数，这部分复用代码需要静态链接进主程序和各自插件内，确保兼容不同版本的复用代码编译的主程序和插件可以加载运行
- 在主程序和插件的接口中不能传递标准库结构体，也不能传递复用代码里的结构体，这些都只能各自内部使用，且插件编译时应当只导出接口符号，其他符号全部隐藏

## 编译
- Linux:
    - 使用 shell 脚本编译: [linux_debug_build.sh](agent/script/linux_debug_build.sh) 或 [linux_release_build.sh](agent/script/linux_release_build.sh)
    - 运行测试 [linux_test_run.sh](agent/script/linux_test_run.sh)
- Windows:
    - 使用 bat 脚本编译: [windows_debug_build.bat](agent/script/windows_debug_build.bat) 或 [windows_release_build.bat](agent/script/windows_release_build.bat)
    - 运行测试 [windows_test_run.bat](agent/script/windows_test_run.bat)
- Android:
    - 在 Linux 上使用 shell 脚本交叉编译: [android_release_build.sh](agent/script/android_release_build.sh)
- 编译脚本创建的 build 目录一般为:
    - debug_build: `agent/build/linux-debug/` 或 `agent/build/windows-debug/`
    - release_build: `agent/build/linux-release/` 或 `agent/build/windows-release/`
    - android_release_build: `agent/build/android-release/`
    - 注意，修改文件时不建议修改 build 目录内的文件，编译时可能被覆盖
- 为了减少编译输出内容展示，只捕捉关键词，可以参考: `./path/to/linux_debug_build.sh 2>&1 | grep -E -i "Built target|error|warn" | tail -10`

## 常见问题
- 如果遇到编译器崩溃 (ICE)，直接重新运行编译尝试即可; 也可能是内存不足或内存中的缓存占用太多了，可以清理一下再编译试试; 如果多次运行都崩溃，则可能确实代码有问题，需要重新检查一下。
```sh
# 清理内存缓存
sudo echo 1 > /proc/sys/vm/drop_caches
sudo echo 2 > /proc/sys/vm/drop_caches
sudo echo 3 > /proc/sys/vm/drop_caches
```
- 编译如果警告`不应忽略函数返回值`时，如果函数返回值是协程值(比如asio::awaitable<>)则必须处理，需要 co_await，否则该协程函数没有启动执行，相当于没调用