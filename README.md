# Agentxx
[Github agentxx](https://github.com/coolight7/agentxx)

- C++ 实现的 AI Agent，减少内存占用、程序包体积、摆脱复杂依赖，为普通性能的手机、电脑等设备上运行设计
> - 早期开发中...

## 兼容性
- 跨系统支持:
    - ✅可编译为独立可执行程序/动态库，摆脱额外的动态库依赖，仅依赖基本的系统库
    - 系统支持:

| Status | System | TIP |
|---|---|---|
| ✅ | Windows 10+ | - |
| ✅ | Linux | 在WSL时额外支持扩展功能 |
| ✅ | Android 5.0+ | Linux 交叉编译 |
| ⬜ | Macos | 待测试兼容 |
| ⬜ | IOS | 待测试兼容 |
- `libagentxx` Lang Binding:
    - ✅C++ (自身开发语言)
    - ⬜Flutter/Dart
- 生成库链接方式:
    - ✅动态链接库`libagentxx`; Debug编译时末尾添加d`libagentxxd`，统一多平台名称，仅后缀区别`.so/.dll/.dylib`.
    - ✅静态链接库`libagentxx_static`; Debug编译时末尾添加d`libagentxx_staticd`，统一多平台名称，仅后缀区别`.a/.lib`. 支持静态链接所有依赖库，合并生成独立可运行的 `agentxx_cli`, 已在 linux/win 验证. 同理可静态链接`libagentxx_static`及其静态依赖库，即可得到让自己的程序也摆脱动态库依赖
    - 可修改[CMakeLists.txt](/agent/CMakeLists.txt)实现静态链接 `C++标准库 libstdc++`和`编译器运行时库 msvcrt/libgcc`, 但静态链接标准库和编译器运行时库有很大风险，谨慎考虑!
    - 默认编译提供 动态库`libagentxx`、静态库`libagentxx_static`, 且统一动态链接 libstdc++/libgcc/msvcrt(/MD|/MDd)

### 编译后的体积和依赖库
- Agentxx 编译后输出的 可执行程序`agentxx_cli`、动态库`libagentxx` 都会尽量静态链接依赖库，保持编译结果对动态库的依赖尽量少
- ⬜编译优化，控制导出符号，裁剪体积
- 以下是`仅编译agentxx，移除大部分不必要的扩展依赖库`时的体积和运行时内存占用，如果需要进一步裁剪体积，可以移除 VectorScan/Hyperscan/codegraph/Boost.process 等可选库、采用 -Os 体积编译优化

| System | agentxx_cli | agentxx_cli RAM | libagentxx | compiler | TIP |
|---|---|---|---|---|---|
| **Windows** | 8.89 M | 任务管理器 2.2 M | 1.21 M | MSVC 19.51.36247.0/Visual Studio 18 2026 · x86_64 · -O2 | 打包时建议带上msvc运行时 |
| **Linux** | 11 M | top/RES 10.5 M | 2.4 M | GCC 16.1.0 · x86_64 · -O3 · --strip-all | 打包时建议带上 libstdc++.so.6,libgcc_s.so.1 |
| **Android (-deps)** | - | - | 1.9 M | NDK-r29 · Clang 21.0.0 · android-21-arm64-v8a · -O3 · --strip-all | 打包建议带上 libc++.so |

## 计划实现
### 基础模块
- **Toolcall**
    - ✅返回值自动转换字符编码到 utf8
    - ✅拦截输出，超过限制长度时自动压缩、截取摘要存储到 share_store
    - ✅filesystem (支持 `同步`/`asio io_uring/IOCP 协程异步` 文件读写)
        - ls (file/dir/recursive-dir/limit)
        - read_text (full / offset-limit)
        - read_binary (full / byte-offset-limit)
        - write (text/binary)
        - edit_text
        - glob
        - grep (multi text/regex + multi-filepath)
        - 读取文件内容时自动转换字符编码到 utf8
        - ⬜写入文件内容时保持文件原有字符编码
    - ✅execute_command (支持 `同步`/`Boost.process 协程异步`执行)
        - ✅execute_linux_command
        - ✅execute_windows_command (检测到 WSL 环境时，允许在 linux/wsl 直接执行 windows 命令)
        - ⬜execute_python_command
        - ⬜execute_javascript_command
        - ⬜自动超时检查和警告
        - ✅区分 stdout、stderr，自动转换输出字符编码到 Utf8
    - ✅web_search (支持 asio 协程异步网络请求)
        - web_search (内置 HTML 转 markdown, 支持直接使用普通网页搜索api)
        - web_fetch_url_markdown (html to markdown)
        - web_fetch_url (raw resp body)
        - ⬜subagent 对接外部 llm agent 实现搜索
    - ✅planning
        - 分为两层规划
        - mermaid/stateDiagram-v2 状态图描述大方向的任务规划
        - todo_list 描述近期需要实现的任务细节步骤
    - ✅Sub-Agent (支持协程并发执行，并保证返回顺序正确)
    - ✅RAGSearch
    - ⬜tool_skill_search (延迟加载 tool/skill)
    - ✅ui_control (windows 系统上控制鼠标键盘)
    - ✅get_system_core_info 获取系统 CPU占用、内存、GPU占用、显存
    - ✅get_current_datetime 获取系统时间戳、本地时间、UTC时间
- ✅**Tree-Messages**
    - share_store (允许存取变量，在 llm-messages、skill、tool 之间传递数据)
        - 支持 `line_offset`/`line_limit` 文本分页读取
        - 压缩上下文时会将部分内容存储到 `share_store`
        - 自动拦截 tool/subagent 返回值，太长时存储原始内容到 `share_store`, 并留下摘要和 id
    - 消息分支，支持修改历史消息/模型重新生成消息
    - 多会话和历史会话
- ✅**中断恢复**
    - 支持在 Node或toolcall 发起中断，等待用户响应，然后恢复执行，会重复执行 node 中断前的代码，但不会重复执行成功完成的 toolcall
    - 支持多个 toolcall 同时发起中断，允许一轮中反复 `中断-用户响应`
    - 支持HITL，中断处理可以自定义实现，内置实现支持用户确认信息、输入内容等
    - 支持用户取消执行
- ✅**异常处理和自动重试**
    - Toolcall/LLM 节点支持自动重试，支持自定义重试次数
    - Toolcall/LLM 节点异常时自动添加消息到上下文，保持角色消息顺序正确
    - 保持 Middleware 拦截执行的顺序和异常处理正确
    - 触发异常时，保持上下文角色顺序正确、内容完整
    - 轮次开始时，自动检查和修复消息上下文角色顺序和内容
- ✅**Sub-Agent**
    - 借由 Tool 实现, 允许 llm/代码 异步启动 SubAgent
    - Toolcall 支持并发，因此支持同时启动运行多个 Subagent
    - 内置实现:
        - subagent_task (仅隔离上下文)
        - tool_skill_search
- ✅**Middleware**
    - 支持层次化栈式拦截 (层层执行 start，压栈对应的 end，再逐层向外退栈执行 end) `agentCallStart`、`agentCallEnd`、`modelCallStart`、`modelCallEnd`、`toolCallStart`、`toolCallEnd`
- ✅**PlanningMiddleware**
    - 分为两层规划
    - mermaid/stateDiagram-v2 状态图描述大方向的任务规划
    - todo_list 描述近期需要实现的任务细节步骤
- ✅**压缩上下文** `SummarizationMiddleware`
    - Api TokenUsage / 自动估算 tokens，达到阈值时自动启动压缩
    - toolcall 各自实现压缩处理
        - 裁剪历史消息中过时的 (filesystem)文件读写、(planning)任务规划、(share_store)变量读写消息
    - 将部分重要的长消息内容暂存到 `share_store`，而不压缩，模型需要时可以提取
    - LLM 总结压缩
    - 保留最近消息
- ⬜**Memory**
    - 持久记忆
    - 总结共享记忆
    - 自定义加载记忆消息
- ✅**权限限制** `PermissionMiddleware`
    - ✅允许指定 tool 调用前拦截，决定 允许、拒绝 或 中断提示询问
    - ✅预设文件读写权限限制
    - ⬜沙盒执行 Shell/File RW
- ⬜**事件通知**
    - ✅支持注册事件功能/订阅事件通知，事件触发时通知订阅者
    - 预设功能:
        - 定时通知
        - 定长延时循环通知
    - llm启动任务后，异步得到结果
    - 使任务结果支持分块流式输出
    - 接入外部程序的消息通知、数据添加
- ✅**Skill支持** `SkillMiddleware`
    - 文件夹扫描/metadata读取收集 + `filesystem`文件内容读取 + `execute_command`执行
- ❌**MCP支持** (Neograph已实现，但暂时使用有问题)
    - MCP client
    - Mcp Server
        - CodeGraph
        - Websearch
- ⬜**Self-upgrade**
    - 自动循环调整系统提示词、工具提示词等，评估效果
    - 自动测试
    - 空闲时自动优化 skill、prompt
- ⬜**LLM Api**
    - ✅Openai API
    - Anthropic API
- ✅**自定义配置**
    - LLM Api (BaseUrl/ApiKey/ModelName/ExtraArgs)
    - System Prompt
    - 分离 System/Tool Prompt 到独立配置，以便支持自定义和`Self-upgrade`自动调整适配

### 扩展
- ✅支持读取windows系统的 CPU占用、内存占用、GPU占用、显存占用
- ✅支持 DXGI/DGI 捕获屏幕帧
- ⬜支持捕获系统输出音频、指定程序输出音频、麦克风
- ✅支持接收各种程序、浏览器的选择文本事件
- ✅CodeGraph
    - 分析代码符号、查找定位
    - 保存分析结果到 sqlite
    - ⬜根据 .gitignore/.gitmodules 等排序分析优先级，把 third_party/test 等目录排后
- ✅RAG
    - ✅文本分割分块 + 默认20%相邻分块重叠
    - 文本分割方式:
        - ✅定长分割
        - ✅字符分割
        - ✅结构分割 (较长的再进行 字符分割/定长分割)
        - ⬜语义分割
- PaddleOCR (图片转文本)
- SD.cpp 图片视频生成
- FunASR 语音识别
- Qwen3-TTS 文本转语音

### Server
- Agent2App Api Server
- ACP Server
- A2A Server

### 测试
- Agent 整体稳定性测试
    - ✅在 llm/toolcall 等各种节点触发中断/异常时，保持上下文角色顺序正确、内容完整
    - 程序突然终止、重启，启动后的自动修复

### 功能
- ✅**操作键鼠**
    - Tool/ui_control
- ⬜**翻译/划词翻译**
    - 截图识别屏幕文本，允许复制、分析、翻译
- ⬜**根据图片内容，提取文本和提示并指定文本在图片上的位置**
    - 实现类似游戏中图片内容中的多个提示点，点击扩展到文本内容或提示信息
- ⬜**根据文本/音视频，生成评论/弹幕**
- ⬜**图片/视频生成**
    - 通过 llm 优化提示词后生成返回，可自动检查生成结果，调整提示词重新生成
- ⬜**ASR/TTS**
- ⬜**匹配歌词**
- ⬜**操作live2d/3d模型动作**
- ⬜部分扩展功能独立编译为 exe，以便支持 WSL 连接扩展获取数据

## 目录结构
- `agent`:
    - C++ 实现 Agent
    - 大部分手写实现，基础框架实现完善后由AI模块化添加功能和检查、补充测试
- `agent/script`:
    - 编译脚本，存放已经验证支持的系统上的编译脚本，使用前可以先参考 [对应的编译文档](/docs/build/)
- `agent/lib`: libagentxx
    - 核心库，包含了内置实现的 toolcall、node、middleware 等，分离编译以便嵌入其他 app 开发使用
- `agent/client`: agentxx_cli
    - 命令行可执行程序，计划用于启动服务、实现命令行用户交互
- `agent/test`: agentxx_test
    - 测试
- `agent/third_party`:
    - `neo-graph`: 图执行核心
        - [原项目](https://github.com/fox1245/NeoGraph)
        - [Fork 修改](https://github.com/coolight7/NeoGraph)
            - Toolcall
                - 调整 Tool 默认为异步执行
                - 支持并发启动多个 Tool 同时开始执行
                - toolcall 增加参数 thread_id
                - McpTool 增加异步操作
            - NodeInput 允许修改 state，以便支持修改 messages 消息上下文
            - ChatMessage 支持记录修改历史，以实现记录模型重新生成消息、修改用户消息
            - LLMCallNode 当 messages 中存在 system message 时不再额外添加
            - GraphState 增加 overwrite 函数以支持强制覆盖变量
    - `codegraph-cpp`: 分析代码/md文件关系. 
        - [原项目](https://github.com/plutoaac/codegraph-cpp)
        - [Fork 修改](https://github.com/coolight7/codegraph-cpp):
            - 从仅支持 c++/python 解析，扩展到支持 js/ts/dart/rust/go/java/kotlin/bash/markdown 等 20+ 种编程语言和文件格式结构
            - 扩展 Windows 编译运行支持
    - `正则表达式库支持`: 可根据编译选项自定义选择支持
        - Vectorscan: 优先选择，但仅在 Linux 支持
        - Hyperscan: 次选，兼容 Windows/Linux
        - std::regex: 兜底

## 编译 
- C++ Standard: Requires C++26 +.
- 编译器推荐
    - Linux/gcc 16.1. 此前使用 gcc 13.2 编译时，部分协程函数会导致编译器自身崩溃
    - Windows/msvc/visual studio 2026. 也可尝试 vs2022 等旧版本, 未验证是否支持
- 国内网络环境推荐先挂VPN代理，部分步骤需要手动或自动下载 Github 仓库
- 拉取项目源码和依赖库
```sh
git clone https://github.com/coolight7/agentxx
cd agentxx
git submodule update --init
```
- 安装 codegraph-cpp 依赖
```sh
cd {项目根目录}/agent/third_party/codegraph-cpp
npm install --legacy-peer-deps
```
- 接下来按希望输出的目标系统选择:
    - [Linux/WSL 可执行程序 / 动态库编译 .so / 静态库 .a](/docs/build/linux.md)
    - [Android 动态库编译 .so / 静态库 .a](/docs/build/android.md)
    - [Windows 可执行程序 .exe / 动态库编译 .dll / 静态库 .lib](/docs/build/windows.md)

## LICENSE
- [MIT License](LICENSE)
- 根据 动态链接、静态链接 库的不同，可能会携带他们的开源协议
