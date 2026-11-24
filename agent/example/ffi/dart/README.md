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
│  FFI C API v4 (ffi_api.h)   │     纯 C ABI, 双 io 线程, 提交 + 回执
└─────────────────────────────┘
```

## 快速开始

```sh
# 1) 编译动态库 (若未编译)
agent/script/windows_release_build.bat     # Dart 侧请用 release 库: debug 库带 ASan,
                                          # 注入非 ASan 宿主进程 (dartvm) 会崩

# 2) 在 cmd 运行 (在 dart/ 目录)
dart run bin/agentxx_cli.dart --dll path/to/libagentxx.dll --base-url https://api.example.com/v1 --api-key sk-xxx --model gpt-4o-mini

# 或使用环境变量 AGENTXX_BASE_URL / AGENTXX_API_KEY / AGENTXX_MODEL_NAME
# 或直接传契约 JSON 文件:
dart run bin/agentxx_cli.dart --model-json model.json --config-json config.json
```

动态库默认按仓库布局自动查找 (`../agent/build/windows-debug/exec/libagentxxd.dll`、
`../../../build/windows-release/exec/libagentxx.dll` 等); 也可用 `--dll <path>` 或
环境变量 `AGENTXX_DLL` 显式指定。

## 功能

- 流式渲染: 文本 / 思考 (灰色) / 工具调用与结果 / 轮次统计 (耗时)
- HIL 人机协同: 🔐 权限确认 (`y/n/a`, `a` 记住路径权限) / ❓ 补充输入收集
- 会话管理: `/model` 切换模型、`/sessions` + `/switch` 会话持久化切换、`/rename`、
  `/compact` 压缩上下文、`/context` 查看 LLM 上下文、`/page` 分页拉展示历史
- 其他: `/cancel` 取消轮次、`/batch` 增量合批开关、`/auth` 完全授权、`/status`、
  `/caps` 能力清单、`/logs` 排障日志、Ctrl+C 优雅退出

## 架构要点

### 调用模型: 提交 + 回执 (FFI API v4)

所有业务操作都是 `agentxx_ffi_call({op,args,timeoutMs})`:

- 提交返回 `reqId`, 结果经 `EVT_RESULT {reqId, op, ok, result?|error?}` 到达
- `AgentClient` 维护 `reqId → Completer` 表, 把回执还原成 `Future`
- 每个受理的 reqId **恰好一条回执** (成功/失败/超时/取消/收尾都会回)
- `AgentxxCancelToken` 一次取消一组在飞调用 (`cancel` 只停等待, 不撤回服务端操作)
- 调用线程上没有跨线程等待, 所以**事件回调里也能继续调** `call` / `cancelCall`

### 事件接收为何用 `NativeCallable.listener`?

v4 起事件 payload **归回调所有**: 结构体与正文同在一块库分配的堆内存上, 取完正文后
`agentxx_ffi_free(payload)` 释放整块。结构体本身也能活到回调执行完, 因此可以直接用
listener 让回调在宿主 isolate 上执行, 不必再走 v3 的事件队列桥接
(`agentxx_ffi_event_queue_*` 已删除)。

### 顺序与超时

- 同时提交的多个调用完成顺序不确定; 需要"前一步生效"就 `await` 前一条回执
- `timeoutMs` 到点由原生侧回一条 `timeout` 回执并忘掉该 reqId (迟到应答被丢弃)
- `stop()` 是异步的: 等 `EVT_STOPPED` 之后才 `destroy` (`dispose()` 已封装)

## 目录结构

```
bin/agentxx_cli.dart      入口: 参数解析 / 配置加载 / 主循环
lib/src/agent_client.dart AgentClient —— C API 封装 (提交+回执/取消令牌/事件流/HIL)
lib/src/events.dart       事件 payload JSON → Dart 事件模型
lib/src/repl.dart         终端渲染 + REPL + 中断应答流程
lib/src/dll_loader.dart   动态库定位加载
lib/src/console_setup.dart Windows 控制台 UTF-8/ANSI 适配
example/smoke_check.dart  基本功能检查 (内置 mock LLM: 对话/HIL 权限/调用顺序/取消/生命周期)
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
cd ../../ffi/dart
dart pub get
dart run ffigen --config ffigen.yaml     # ffigen 19 从 --config 文件读配置
```
