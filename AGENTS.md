# Agentxx
- C++ 23 实现的 AI Agent；编译器启用标准 c++26/c17

## 兼容性
- 跨系统支持:
    - 可编译为独立可执行程序/动态库/静态库，摆脱额外的动态库依赖，仅依赖基本的系统库
    - Linux x86_64 + WSL扩展功能
    - Windows 10+ x86_64
    - Android 5.0+

## 设计
- Agent 的设计支持:
    - 并发多会话，单线程/多协程交错执行会话，不需要线程锁
    - client 主要负责UI渲染展示、用户交互；agent (DeepAgent) 负责运行会话，调用 llm api、运行 toolcall 等
    - 支持 client+agent 在同一个进程内启动，此时两者使用线程间数据交互
    - 支持 client 通过网络连接 agent server，此时两者在不同进程，通过网络传输交互（已支持 websocket）
    - 应当尽量统一抽象接口，分层屏蔽细节，降低复杂度，让架构设计更清晰
    - `会话 Agent_IO`、`CancelToken`、`上下文统计`、`模型选择` 应当独立记录，按 `thread_id` 取值
- 编写代码时在需要注意的地方、设计描述应当有清晰的注释，请勿随意移除原代码中的注释
- 合适的情况下，尽量使用`std::string_view`替代`const std::string&`
- 应当使用 [XX_LOG](agent/lib/include/agentxx/util/log.h) 输出日志，而不是 std::cout/cerr，避免影响 TUI 显示
- 最终的代码实现目标要能稳定运行在生产环境，广泛服务于各种设备和用户，需要仔细思考实现方案、编写足量的常规使用方式测试+各种边界情况测试

## 代码结构
- `agent`: 
    - C++ 实现 Agent
- `agent/lib`: libagentxx
    - 核心库，包含了内置实现的 DeepAgent、toolcall、node、middleware 等，分离编译以便嵌入其他 app 开发使用
    - [util](agent/lib/include/agentxx/util/) 一些工具类和函数，包括 `http server/client`、`websocket server/client`、`log`、`lru cache`、`aho_corasick/regex`、[字符串工具](agent/lib/include/agentxx/util/string_util.h) (大小写转换、str转数值、自动检测字符编码并转utf8、计算utf8长度、移除空白符、base64、unix/windows/自动路径标准化、忽略大小写的判断包含/相等、split按char切割字符串)、[异常处理](agent/lib/include/agentxx/util/exception.h)
    - [DeepAgent](agent/lib/include/agentxx/agent/deepagent.h) agent 运行核心
- `agent/client`: 编译结果 {build}/exec/agentxx_cli
    - 命令行可执行程序，用于启动 agent 服务、实现命令行 cli/TUI 交互
    - `agent/client/main.cpp` 通过 --agent 启动时
    - IO交互继承于 [AgentIOBase](agent/lib/include/agentxx/agent/agent_io.h)
        - `agent/client/include/agentxx-client/io/stdio` 采用 stdin、stdout、stderr 作为输入输出 `agentxx_cli cli`
        - `agent/client/include/agentxx-client/io/tui` 采用 TUI 作为终端渲染界面交互
        - [RemoteClientAgentIO](agent/lib/include/agentxx/agent/remote/remote_client_io.h) 和 [RemoteServerAgentIO](agent/lib/include/agentxx/agent/remote/remote_server_io.h) 用于包装
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
    - [boost]
        - asio
        - beast
        - process
        - exception
    - [codegraph-cpp]
    - [curl]
    - [fmt]
    - [FTXUI]
    - [glob]
    - [html2md]
    - [hyperscan]
    - [iconv] | [libiconv-native]
    - [liburing]
    - [NeoGraph]
    - [OpenSSL]
    - [simdjson]
    - [sqlite3] | [sqlite3-cmake]
    - [uchardet]
    - [vectorscan]
    - [yaml-cpp]
    - [zlib] | [zlib-ng]

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

## 常见问题
- 如果遇到编译器崩溃 (ICE)，直接重新运行编译尝试即可，如果多次运行都崩溃，则可能确实代码有问题，需要重新检查一下。