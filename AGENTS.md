# Agentxx
- C++ 23 实现的 AI Agent；编译器启用标准 c++26/c17

## 兼容性
- 跨系统支持:
    - 可编译为独立可执行程序/动态库，摆脱额外的动态库依赖，仅依赖基本的系统库
    - Linux x86_64 + WSL扩展功能
    - Windows 10+ x86_64
    - Android 5.0+

## 代码结构
- `agent`: 
    - C++ 实现 Agent
- `agent/lib`: libagentxx
    - 核心库，包含了内置实现的 toolcall、node、middleware 等，分离编译以便嵌入其他 app 开发使用
- `agent/client`: 编译结果 {build}/exec/agentxx_cli
    - 命令行可执行程序，计划用于启动服务、实现命令行用户交互
- `agent/test`: 编译结果 {build}/exec/agentxx_test
    - 测试
    - 运行测试示例:
```bash
# 运行所有测试模块，遇到错误也不终止继续运行
path/to/agentxx_test 

# 当任意模块测试存在错误时立即终止测试，未指定时默认无论模块是否存在错误，都完成运行所有测试模块
path/to/agentxx_test --fail-fast

# 指定仅运行测试模块 `string_util` `regex`, 其他不运行，默认未指定时运行所有模块
# 测试模块名称定义见 `agent/test/test.cpp`
path/to/agentxx_test string_util regex
```
- `agent/benchmark`: 编译结果 {build}/exec/agentxx_benchmark
    - 性能测试（一般仅 release 启用编译该模块）
- `agent/third_party`: 第三方库依赖

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