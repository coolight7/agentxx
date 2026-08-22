# agentxx_dart_cli

经 **Dart FFI** 驱动 [libagentxx](../../../lib/) 内置 agent 会话的命令行客户端。

```
┌─────────────────────────────┐
│ dart CLI (bin/agentxx_cli)  │
│  ├─ CliRepl   渲染/交互/HIL  │
│  └─ AgentClient (FFI 封装)  │
├─────────────────────────────┤
│ agentxx_ffi_bindings        │  ← agent/ffi/dart (ffigen 由 ffi_api.h 生成)
├─────────────────────────────┤
│ libagentxx 动态库            │  ← agent/build/*/exec/libagentxxd.dll
│  FFI C API (ffi_api.h)      │     纯 C ABI, 双线程 io 模型
└─────────────────────────────┘
```

## 快速开始

```sh
# 1) 编译动态库 (若未编译)
agent/script/windows_debug_build.bat

# 2) 在 cmd 运行 (在 dart/ 目录)
dart run bin/agentxx_cli.dart --dll path/to/libagentxx.dll --base-url https://api.example.com/v1 --api-key sk-xxx --model gpt-4o-mini

# 或使用环境变量 AGENTXX_BASE_URL / AGENTXX_API_KEY / AGENTXX_MODEL_NAME
# 或直接传契约 JSON 文件:
dart run bin/agentxx_cli.dart --model-json model.json --config-json config.json
```

动态库默认按仓库布局自动查找 (`../agent/build/windows-debug/exec/libagentxxd.dll`
等); 也可用 `--dll <path>` 或环境变量 `AGENTXX_DLL` 显式指定。

## 功能

- 流式渲染: 文本 / 思考 (灰色) / 工具调用与结果 / 轮次统计 (耗时·tps)
- HIL 人机协同: 🔐 权限确认 (`y/n/a`, `a` 记住路径权限) / ❓ 补充输入收集
- 会话管理: `/model` 切换模型、`/sessions` + `/switch` 会话持久化切换、`/context`
- 其他: `/cancel` 取消轮次、`/status`、`/logs` 排障日志、Ctrl+C 优雅退出

## 架构要点

### 事件接收为何走队列桥接?

`AgentxxCallbacks.on_event` 在原生 client-io 线程同步回调, payload
**仅回调期间有效**。Dart 的 `NativeCallable.listener` 异步投递事件,
指针所指内存可能已释放 —— 直接使用存在悬垂风险。

因此 C++ FFI 层提供了异步安全桥接 (`agentxx_event_queue_*` 系列,
见 `docs/agent/ffi.md` 4.2 节): 原生侧在回调内同步拷贝 payload 入队,
Dart 侧轮询取出。`AgentClient` 已完整封装该流程。

### 同步查询约束

`get_model_info / get_context_messages / list_sessions` 为阻塞式同步查询,
同一句柄同一时刻仅允许一个在途 (C API 契约)。

## 目录结构

```
bin/agentxx_cli.dart      入口: 参数解析 / 配置装配 / 主循环
lib/src/agent_client.dart AgentClient —— C API 封装 (事件流/交互/HIL)
lib/src/events.dart       wire 协议 JSON → Dart 事件模型
lib/src/repl.dart         终端渲染 + REPL + 中断应答流程
lib/src/dll_loader.dart   动态库定位加载
lib/src/console_setup.dart Windows 控制台 UTF-8/ANSI 适配
example/smoke_check.dart  冒烟检查 (内置 mock LLM, 覆盖对话+HIL 权限链路)
example/mock_llm.dart     独立 mock LLM 服务器 (端到端联调用)
```

## 测试

```sh
dart analyze                    # 静态检查
dart run example/smoke_check.dart
```

## 绑定再生成

头文件变更后重新生成 Dart 符号定义 (输出到 `agent/ffi/dart/lib/`):

```sh
cd ../agent/ffi/dart
dart pub get
dart run ffigen --config ffigen.yaml
```
