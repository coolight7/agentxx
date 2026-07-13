# Agentxx
- C++ 实现的 AI Agent

## 兼容性
- 跨系统支持:
    - 可编译为独立可执行程序/动态库，摆脱额外的动态库依赖，仅依赖基本的系统库
    - Linux x86_64 + WSL扩展功能
    - Windows 10+ x86_64

## 代码结构
- `agent`: 
    - C++ 实现 Agent
- `agent/lib`: libagentxx
    - 核心库，包含了内置实现的 toolcall、node、middleware 等，分离编译以便嵌入其他 app 开发使用
- `agent/client`: 编译结果 {build}/exec/agentxx_cli
    - 命令行可执行程序，计划用于启动服务、实现命令行用户交互
- `agent/test`: 编译结果 {build}/exec/agentxx_test
    - 测试
- `agent/third_party`: 第三方库依赖

## 编译
- Linux:
    - 使用 shell 脚本编译: [debug_build.sh](agent/script/linux_debug_build.sh) 或 [release_build.sh](agent/script/linux_release_build.sh)
    - 运行测试 [test_run.sh](agent/script/linux_test_run.sh)
- Windows:
    - 使用 bat 脚本编译: [debug_build.bat](agent/script/windows_debug_build.bat) 或 [release_build.bat](agent/script/windows_release_build.bat)
    - 运行测试 [test_run.bat](agent/script/windows_test_run.bat)
- 编译脚本创建的 build 目录一般为:
    - debug_build: `agent/build/linux-debug/` 或 `agent/build/windows-debug/`
    - release_build: `agent/build/linux-release/` 或 `agent/build/windows-release/`
