

# Agentxx
[Github agentxx](https://github.com/coolight7/agentxx)

- C++ 协程异步实现的 AI Agent，可编译为`单程序、动态库`直接启动使用。降低内存占用、程序包体积、摆脱庞大的 动态库、python、js 等依赖，为普通性能的手机、电脑等设备上运行设计
- 目标支持嵌入App实现高性能的Agent功能，顺带实现 cli/TUI 的 Code Agent; GUI客户端计划将由[Lumenxx](https://github.com/coolight7/lumenxx-docx)支持，并实现 音视频处理、自动化控制 等 Agent 
- 已实测过最长单轮任务自动运行5小时完成，本项目已由 Agentxx 自身介入开发

> 初步完成 agent核心及服务、TUI、插件接口、FFI接口，但仍可能大幅度重构，接口可能大改动.

- [特点](#特点)
- [兼容性](#兼容性)
    - [跨系统支持](#跨系统支持)
    - [编译后的体积和依赖库](#编译后的体积和依赖库)
- [计划实现](#计划实现)
    - [基础模块](#基础模块)
    - [提示词训练](#提示词训练)
    - [插件化支持](#插件化支持)
    - [FFI动态库接口](#FFI动态库接口)
    - [功能](#功能)
    - [测试](#测试)
- [目录结构](#目录结构)
- [编译](#编译)
- [运行&配置文件](#配置文件和运行)

## 特点
- **C++协程异步实现**; 程序体积和内存占用少且性能高，可选添加 硬件加速Hyperscan 等扩展库
- **数据安全**; Agentxx 不会上传你的数据，如果使用局域网内的 LLM Api Server，完全可以实现全程断网运行; Agentxx 无法确认 LLM Api、MCP、Skill 的数据安全，如果导入需要自行确认
- **跨系统支持**; 优化 windows 兼容，可在 WSL 中直接执行 windows 命令、打开 windows 程序、自动转换文件路径
- **丰富的 tool**; 内置 文件读写、命令行执行、任务规划 等，编译时可选自由组合，支持自动纠正 LLM 的参数类型、字符编码
- **C++插件/js插件支持**; 已实现 codegraph、系统CPU/GPU/RAM等信息、屏幕截取、鼠标选择文本事件流 等效果显著的功能，通过 C++ Quickjs 插件可以加载实现 js 插件
- **UI与Agent可分离**; 内置支持 TUI、cli、接入GUI、Websocket API、FFI调用、动态库/静态库嵌入App; 支持单进程、多进程分别启动 UI 和 Agent Websocket Server服务
- **中断、错误自动处理**; 长时间稳定运行、网络重试、动态超时限制、消息上下文角色顺序检查和修正、自动检查和修正字符编码、空响应自动重试、Tool连续重复调用检查

## 兼容性
### 跨系统支持
- ✅可编译为独立可执行程序/动态库，摆脱额外的动态库依赖，仅依赖基本的系统库
- 系统支持:

| Status | System | TIP |
|---|---|---|
| ✅ | Windows 10+ | Win编译/Linux 交叉编译 |
| ✅ | Linux | 在WSL运行时额外支持直接执行 windows 程序和命令 |
| ✅ | Android 5.0+ | Linux 交叉编译 |
| ⬜ | Macos | 待测试兼容 |
| ⬜ | IOS | 待测试兼容 |

- `libagentxx` Lang Binding:
    - ✅C++ (自身开发语言)
    - ✅[FFI/C-Api动态库符号导出](#FFI动态库接口) 以便支持 flutter/dart/Javascript 等语言调用动态库
- 生成库链接方式:
    - ✅动态链接库`libagentxx`; Debug编译时末尾添加d`libagentxxd`，统一多平台名称，仅后缀区别`.so/.dll/.dylib`.
    - ✅静态链接库`libagentxx_static`; Debug编译时末尾添加d`libagentxx_staticd`，统一多平台名称，仅后缀区别`.a/.lib`. 支持静态链接所有依赖库，合并生成独立可运行的 `agentxx_cli`, 已在 linux/win 验证. 同理可静态链接`libagentxx_static`及其静态依赖库，即可得到让自己的程序也摆脱动态库依赖
    - 可修改[CMakeLists.txt](/agent/CMakeLists.txt)实现静态链接 `C++标准库 libstdc++`和`编译器运行时库 msvcrt/libgcc`, 但静态链接标准库和编译器运行时库有很大风险，谨慎考虑!
    - 默认编译提供 动态库`libagentxx`、静态库`libagentxx_static`, 且统一动态链接 libstdc++/libgcc/msvcrt(/MD|/MDd)

### 编译后的体积和依赖库
- Agentxx 编译后输出的 可执行程序`agentxx_cli`、动态库`libagentxx` 都会尽量静态链接依赖库，保持编译结果对动态库的依赖尽量少；编译优化 控制导出符号，裁剪无用符号
- 以下是`仅编译agentxx，移除大部分不必要的依赖库`时的体积和运行时内存占用, 测试于 `时间: 2026/08/22, commit: b35b226399062dc1196bced06d2c4f209de9e0fa`
- 可执行文件/动态库文件体积:

| System | agentxx_cli | libagentxx | compiler | TIP |
|---|---|---|---|---|
| **Windows** | 18.3M | 12.6M | MSVC 19.51.36247.0/Visual Studio 18 2026 · x86_64 · -O2 | 打包时建议带上msvc运行时 |
| **Linux** | 13.3M | 17.8M | GCC 16.1.0 · x86_64 · -O3 · --strip-all | 打包时建议带上 libstdc++.so.6,libgcc_s.so.1 |
| **Android** | - | 14.0M | NDK-r29 · Clang 21.0.0 · android-21-arm64-v8a · -O3 · --strip-all | 打包建议带上 libc++.so |

- 内置插件动态库文件体积 (横线 - 表示该插件不支持此系统):

| Plugin | Windows/.dll | Linux/.so | Androi/.so | TIP |
|---|---|---|---|---|
| agentxx_codegraph | 36.9M | 38.2M | 37.6M | - |
| agentxx_computer_use | 379K | - | - | - |
| agentxx_javascript_engine | 1.1M | 1.2M | 1.1M | - |
| agentxx_screen_capture | 383K | - | - | - |
| agentxx_system_monitor | 428K | 629K | - | - |
| agentxx_text_selection_monitor | 399K | - | - | - |

- 内存占用:

| agentxx_cli Target | 初始化 RAM | 100K 上下文 | 200K上下文 | TIP |
|---|---|---|---|---|
| **Win/TUI** | 3.1M | 12.2M | 19.6M | 任务管理器查看内存占用 |
| **Linux/TUI** | 1.9M | 8.9M | 14.1M | top命令查看RES-SHR, agentxx_cli 仅依赖系统库，不需要其他动态库，因此仅计算独占内存大小 |

- 默认的编译优化倾向于追求性能，如果需要裁剪体积，可以移除 Hyperscan/Boost.process 等可选库、采用 -Os/-Oz 体积编译优化

## 计划实现
### 基础模块
- **Toolcall**
    - ✅返回值自动转换字符编码到 utf8
    - ✅部分Tool支持连续重复调用检查
    - ✅拦截输出，超过限制长度时自动压缩、截取摘要存储到 agentxx_share_store
    - ✅自动转换参数类型（String、Array、Number互转），提高兼容性
    - ⬜支持依托`事件流`实现异步获取结果、分块获取结果
    - ✅filesystem (`同步`/`asio io_uring/IOCP 协程异步` 文件读写、超时限制)
        - list (file/dir/recursive-dir/limit)
        - read (full / offset-limit)
        - write
        - edit
        - glob
        - grep (multi text/regex + multi-filepath)
        - WSL 系统环境下自动转换 windows 文件路径
        - 读取文件内容时自动转换字符编码到 utf8
        - ⬜写入文件内容时保持文件原有字符编码
    - ✅execute_command (`同步`/`Boost.process 协程异步`执行、超时限制)
        - execute_bash_command
        - execute_windows_command (检测到 WSL 环境时，允许在 linux/wsl 直接执行 windows 命令)
        - 超时限制
        - 区分 stdout、stderr，自动转换输出字符编码到 Utf8
    - ✅web_search (asio 协程异步网络请求)
        - web_search (内置 HTML 转 markdown, 支持直接使用普通网页搜索api)
        - web_fetch_url_markdown (html to markdown)
        - web_fetch_url (raw resp body)
        - ⬜支持 subagent 对接外部 llm api 实现搜索
    - ✅planning
        - 目标规划 + 渐进任务细节 两层任务规划 + 备忘录
        - mermaid/stateDiagram-v2 状态图描述大方向的任务规划
        - todos 描述近期需要实现的任务细节步骤
    - ✅RAG
        - ✅文本分割分块 + 默认20%相邻分块重叠
        - ✅文本分割方式:
            - 定长分割
            - 字符分割
            - 结构分割 (较长的再进行 字符分割/定长分割)
            - ⬜语义分割
    - ✅Sub-Agent (支持协程并发执行，并保证返回顺序正确)
    - ⬜tool_skill_search (延迟加载 tool/skill)
    - ✅get_current_datetime 获取系统时间戳、本地时间、UTC时间
- ✅**Tree-Messages**
    - agentxx_share_store (允许存取变量，在 llm-messages、skill、tool 之间传递数据)
        - 支持 `line_offset`/`line_limit` 文本分页读取
        - 压缩上下文时会将部分内容存储到 `agentxx_share_store`
        - 自动拦截 tool/subagent 返回值，太长时存储原始内容到 `agentxx_share_store`, 并留下摘要和 id
    - 消息分支，支持修改历史消息/模型重新生成消息
    - 多会话和历史会话
- ✅**EventBus 事件流**
    - 支持注册事件功能/订阅事件通知，事件触发时通知订阅者
    - 预设功能:
        - 中断处理
        - 定时通知
        - 定长延时循环通知
    - ⬜llm启动任务后，异步得到结果
    - ⬜使任务结果支持分块流式输出
    - ⬜接入外部程序的消息通知、数据添加
- ✅**中断恢复**
    - 依托`事件流`实现，支持在 Node 或 toolcall 发起中断，等待用户响应，然后恢复执行，会重复执行 node 中断前的代码，但不会重复执行成功完成的 toolcall
    - 支持多个 toolcall 同时发起中断，允许一轮中反复 `中断-用户响应`
    - 支持HITL，中断处理可以自定义实现，内置实现支持用户确认信息、输入内容等
    - 支持用户取消执行
- ✅**权限限制** `PermissionMiddleware`
    - 依托`中断恢复`实现，允许指定 tool 调用前拦截，决定 允许、拒绝 或 中断提示询问
    - 预设文件读写权限限制
    - ⬜沙盒执行 Shell/File RW
- ✅**异常处理和自动重试**
    - Toolcall 自动转换参数类型（String、Array、Number互转），提高兼容性
    - Toolcall/LLM 节点支持自动重试，支持自定义重试次数
    - Toolcall/LLM 节点异常时 自动判断保留已生成的消息、补充添加消息到上下文，保持角色消息顺序正确
    - 轮次开始时，自动检查和修复消息上下文角色顺序和内容
    - 保持 Middleware 拦截执行的顺序和异常处理正确
- ✅**Sub-Agent**
    - 依托 `Toolcall` 实现, 允许 llm/代码 异步启动 SubAgent
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
    - 将部分重要的长消息内容暂存到 `agentxx_share_store`，而不压缩，模型需要时可以提取
    - LLM 总结压缩
    - 保留最近消息
- ✅**Memory记忆与上下文**
    - ✅自定义 yaml 配置加载 Memory 文件
    - ✅sqlite 保存 session 上下文和程序重启恢复
    - 总结共享记忆
- ✅**Skill支持** `SkillMiddleware`
    - 文件夹扫描/metadata读取收集 + `filesystem`文件内容读取 + `execute_command`执行
- ✅**MCP支持**
    - 宽泛协议支持 (2024-11-05 ~ 2026-07-28)
    - MCP client
        - 命名空间隔离
        - 默认超时限制
        - http/stdio
    - Mcp Server
        - ⬜CodeGraph
        - ⬜Websearch
- ⬜**A2UI支持**
    - 统一 client 插件使用 A2UI 数据结构
- ⬜**Self-upgrade**
    - 自动循环调整系统提示词、工具提示词等，评估效果
    - 自动测试
    - 空闲时自动优化 skill、prompt
- ✅**LLM Api**
    - Openai API
    - Openai Response API
    - Anthropic API
    - 支持捕获思考内容，回传 Thinking 内容，加密 Thinking 消息提示和回传
    - 自定义 (BaseUrl/ApiKey/ModelName/ExtraConfig)
    - 广泛的字段兼容、空响应检查和自动重试
- ✅**自定义配置**
    - 支持启动时从 agentxx-config.yaml、.env 加载配置文件
    - yaml 配置支持添加 Memory、MCP、Skill、plugins
    - 分离 System/Tool Prompt 到独立配置，以便支持自定义和`Self-upgrade`自动调整适配
- ✅**网络超时与SSL验证**
    - 支持配置连接超时、动态超时限制 (自动根据请求体大小动态计算发送超时、流式接收间隔超时)
    - 支持关闭SSL验证
- ✅**队列等待输入**
    - agent-io 用户输入队列，等待当前会话执行完成时自动插入继续执行; UI 端支持控制直接中断插入消息

### UI
- ✅Cli: `agentxx_cli cli`
- ✅TUI: `agentxx_cli tui`
- ⬜GUI

### Server
- ✅MCP server
- ✅ACP Server
- ✅A2A Server

### 提示词训练
- 系统提示词、工具提示词 对 LLM 的运行效果有重要影响，尤其是希望用于本地运行的小模型，为此设计了 `提示词训练`，希望实现对不同 LLM 模型针对性的提示词设计
- 通过循环多轮 生成->评分->调整, 对常用的每一个模型针对性训练，各自得到较优的提示词，在运行时根据 ModelName 动态加载使用的提示词
- ✅实现循环训练提示词流程
- ⬜训练一套通用提示词+适配一些常见模型
- ⬜根据 ModelName 动态加载，没有匹配的则取用默认提示词

### 插件化支持
- ✅c/c++插件支持，可对 agent、client-ui 插件化修改；详见[插件开发文档](docs/agent/plugins.md); [内置插件代码实现](/agent/plugins/); [插件示例](/agent/plugins/example_plugin/)
- ✅可选外置编译插件为动态库，或是内嵌编译进 libagentxx
- 其他编程语言插件: 
    - 仿照`agentxx_javascript_engine`实现编程语言的执行引擎, 然后新建插件项目, 指定依赖它, 运行时把代码片段发给执行引擎执行即可; 实际上不一定需要由 执行引擎插件 本身来执行代码, 也可以接收代码片段后调用系统安装的 `nodejs、python3` 等直接执行也是可以的
    - ✅`agentxx_javascript_engine`由 c++插件实现 js 扩展插件开发支持; [JS插件示例](/agent/plugins/example_js/)
    - ⬜`agentxx_python_engine`
- ✅`agentxx_codegraph`
    - 分析代码符号、查找定位
    - 保存分析结果到 sqlite
    - 可配置加载路径/忽略路径
    - 默认忽略 .gitignore 规则与 .gitmodules 子模块目录 (可配置关闭 use_gitignore)
- ✅`agentxx_system_monitor`支持读取 windows/linux 的 CPU占用、内存占用、GPU占用、显存占用
    - tool/get_system_core_info 获取系统信息
- ✅`agentxx_screen_capture`支持 DXGI/DGI 捕获屏幕帧
- ⬜`agentxx_audio_stream`支持捕获系统输出音频、指定程序输出音频、麦克风
- ✅`agentxx_text_selection_monitor`支持接收各种程序、浏览器的选择文本事件
- ✅`agentxx_computer_use`windows 系统上控制鼠标键盘
- ⬜PaddleOCR (图片转文本)
- ⬜SD.cpp 图片视频生成
- ⬜FunASR 语音识别
- ⬜Qwen3-TTS 文本转语音

### FFI动态库接口
- ✅[FFI动态库C-Api符号导出](/agent/ffi/); [设计文档](/docs/agent/ffi.md); [示例](/agent/example/ffi/)
- 通过SDK, 其他编程语言可以便捷地调用libagentxx动态库创建 agent、执行会话等, 本质上SDK就是将动态库符号套一层, 方便其他编程语言调用, 在其他编程语言里直接加载动态库, 然后搜索函数符号调用也是一样的
- 编程语言SDK:
    - ✅Flutter/Dart; [SDK](/agent/ffi/dart/); [示例](/agent/example/ffi/dart/)
    - ⬜Javascript
    - ⬜Python
- 插件与FFI的区别在于, agentxx 加载插件，agentxx 视作主体，在一些事件点时调用插件; 而 FFI 是其他编程语言写的程序为主体, agentxx 被作为一个工具一样创建 agent、执行会话, 更多用于将 agentxx 嵌入到已有的 App 中辅助实现 AI 功能时使用

### 功能
- ✅**操作键鼠**
    - 插件实现`agentxx_computer_use`
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

### 测试
- Agent 整体稳定性测试
    - ✅在 llm/toolcall 等各种节点触发中断/异常时，保持上下文角色顺序正确、内容完整
    - ✅UTF8检查和自动转换
    - 程序突然终止、重启，启动后:
        - ✅自动恢复中断处理、从中断节点继续执行
        - ✅自动修复上下文角色顺序

## 目录结构
- 详细代码结构和功能见[design.md](docs/zh-cn/design.md)
- `agent`:
    - C++ 实现 Agent
    - 大部分手写实现实现基础框架后，由AI模块化添加功能和检查、补充测试
- `agent/script`:
    - 编译脚本，存放已经验证支持的系统上的编译脚本，使用前可以先参考 [对应的编译文档](/docs/zh-cn/build/)
- `agent/lib`: libagentxx
    - 核心库，包含了内置实现的 toolcall、node、middleware 等，分离编译以便嵌入其他 app 开发使用
- `agent/client`: agentxx_cli
    - 命令行可执行程序，计划用于启动服务、实现命令行用户交互、TUI
    - `agent/client/include/agentxx-client/io` 实现了 stdio、TUI 方式的 Agent 调用
    - `agent/client/include/agentxx-client/train` 提示词训练
- `agent/plugins`: 
    - 插件
- `agent/test`: agentxx_test
    - 测试
- `agent/third_party`:
    - `neograph`: 图执行核心
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
        - Hyperscan: 兼容 x86 Windows/Linux
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
- 执行后在 `{项目根目录}/agent/third_party/` 可以看到很多依赖库目录，没有的话需要重新执行 `git submodule update --init`
- 安装 codegraph-cpp 依赖
```sh
cd {项目根目录}/agent/third_party/codegraph-cpp
npm install --legacy-peer-deps
```
- 接下来按希望输出的目标系统选择:
    - [Linux/WSL 可执行程序 / 动态库编译 .so / 静态库 .a](/docs/zh-cn/build/linux.md)
    - [Android 动态库编译 .so / 静态库 .a](/docs/zh-cn/build/android.md)
    - [Windows 可执行程序 .exe / 动态库编译 .dll / 静态库 .lib](/docs/zh-cn/build/windows.md)

## 配置文件和运行
- 参考 `{项目根目录}` 下的 `agentxx-config.yaml`，修改它在里面配置你的模型 llm api，然后 cd 到 `agentxx-config.yaml` 所在目录，运行 agentxx_cli 即可
- （可选）`agentxx-config.yaml` 内配置 llm api key 时，建议放到同目录的 `.env` 中，模版参考 `.env.example`, 复制并重命名为 `.env` 然后添加环境变量即可
```sh
cd {项目根目录}
# 修改 agentxx-config.yaml 
# 可选: 
#       cp .env.example .env
#       修改 .env 添加环境变量 api key

# client 负责UI渲染和输入输出交互
# agent-io 运行 agent-loop 的 server 端，负责执行会话、调用 llm api 等实际操作
# agentxx_cli 启动时可以选择 `启动UI+server` 或 `仅启动UI，网络连接server` 或 `仅启动 server`

# client + agent-io 在同一个进程内启动, agentxx_cli 内的 client 包含两种UI:
agentxx_cli tui # 一个进程启动 TUI 界面 + agent-io
agentxx_cli cli # 一个进程启动 cli + agent-io

# - client 和 agent-io 分离为两个进程，两者使用 websocket 网络连接
# - 自己开发 GUI 连接 server 可使用该方式连接
agentxx_cli server --host 0.0.0.0 --port 7007 --token passwd # 启动 agent-io
agentxx_cli tui --agent ws://127.0.0.1:7007/agent --token passwd # 启动 TUI界面，并连接 agent-io
```

## LICENSE & THIRD_PARTY
- [MIT License](LICENSE)
- 根据 动态链接、静态链接 库的不同，可能会携带他们的开源协议
- 感谢这些依赖库的支持:
    - [boost](https://github.com/boostorg/boost)
        - asio
        - beast
        - process
        - exception
    - [codegraph-cpp](https://github.com/plutoaac/codegraph-cpp)
    - [curl](https://github.com/curl/curl)
    - [fmt](https://github.com/fmtlib/fmt)
    - [FTXUI](https://github.com/ArthurSonzogni/FTXUI)
    - [glob](https://github.com/p-ranav/glob)
    - [html2md](https://github.com/tim-gromeyer/html2md)
    - [hyperscan](https://github.com/intel/hyperscan)
    - [iconv](https://www.gnu.org/software/libiconv/) | [libiconv-native](https://github.com/hesphoros/libiconv-native)
    - [liburing](https://github.com/axboe/liburing)
    - [MarkdownFTXUI](https://github.com/coolight7/MarkdownFTXUI)
    - [NeoGraph](https://github.com/fox1245/NeoGraph)
    - [OpenSSL](https://www.openssl.org/)
    - [simdjson](https://github.com/simdjson/simdjson/)
    - [sqlite3](https://github.com/sqlite/sqlite) | [sqlite3-cmake](https://github.com/sjinks/sqlite3-cmake)
    - [uchardet](https://www.freedesktop.org/wiki/Software/uchardet/)
    - [yaml-cpp](https://github.com/jbeder/yaml-cpp)
    - [zlib](https://github.com/madler/zlib) | [zlib-ng](https://github.com/zlib-ng/zlib-ng)
