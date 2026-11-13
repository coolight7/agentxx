# 实施记录

## 进度

- [x] 步骤 1：广播 ↔ 事件总线两向转发（`PluginManager`）
- [x] 步骤 2：`llm_error` 状态码按独立 token 匹配 + 切掉诊断后缀
- [x] 步骤 3：`BroadcastRegistry` 相同声明幂等（不打日志）
- [x] 步骤 4：测试与构建验证
- [ ] 步骤 5：文档同步（plugins.md / index.md）

## 步骤 1：广播 ↔ 事件总线两向转发

改动：

| 文件 | 内容 |
|---|---|
| `agent/lib/include/agentxx/plugin/plugin_manager.h` | 新增 `attachEventBus`（公开）、`mirrorBusValue` / `forwardBroadcastToBuses` / `publishOnBus` / `detachEventBuses` 与 `BusBridge` 成员 |
| `agent/lib/src/plugins/plugin_manager_broadcast_bridge.cpp`（新） | 两向转发实现 |
| `agent/lib/src/plugins/plugin_manager_lifecycle.cpp` | 构造函数挂主 agent 总线；`attachMountSync` 挂每个挂载 agent 的总线；析构撤订阅 |
| `agent/lib/src/plugins/plugin_manager_domain_services.cpp` | `publishBroadcastFrom` 成功后 `forwardBroadcastToBuses`；`publishBroadcastValue` 改为"有总线发总线，否则就地写广播" |
| `agent/test/plugin/test_plugin_broadcast.cpp` | 夹具加事件总线；新增 `test_bus_bridge`（两向各一组断言 + 未声明话题不建频道） |

验证：

```
agentxx_test --log-level warn plugin_broadcast
--- plugin_broadcast done: passed=86 failed=0 (233 ms) ---
```

回归（广播/埋点/挂载相关）：

```
agentxx_test --log-level warn plugin_point plugin_feature plugin_dispatch plugin_cleanup plugin_host_mount
Total: passed=450 failed=0
```

## 步骤 2：LLM 错误分类不再被字节数带偏

用户报的现象：

```
[提示] 错误
Node 'llm' failed after 1 attempt(s): SSE read-body timeout after 60000
recv=403684B events=565 idle=60013ms finish=none tools=0 args=0B name=
```

根因：`streamFailureSuffix` 拼进错误文本的 `recv=403684B` 里有 `403`，而
`classifyLlmError` 用裸子串匹配状态码 → 判成 `Auth`（不可重试）→ 重试循环第一次就退出
（`recv` 字节数里出现 400/401/402/403/413/429/500… 都会触发，所以"偶现"）。

改动：

| 文件 | 内容 |
|---|---|
| `agent/lib/src/nodes/llm_error.cpp` | 新增 `containsStatusCode`（前后不是 ASCII 字母数字才算命中）、`isAsciiAlnum`；六处状态码（413 / 401+403 / 402 / 400 / 429 / 500+502+503+504）改走它；判类前切掉 ` &#124; stream: ` 起的自有诊断后缀 |
| `agent/lib/include/agentxx/nodes/llm_error.h` | `classifyLlmError` 注释补上这两条规则 |
| `agent/test/core/test_llm_error.cpp` | 新增 6 条带诊断后缀的真实文本（字节数含 403/400/402/413/429/500）、4 条无后缀的裸字节数、7 条独立成词的状态码 |

验证：

```
agentxx_test --log-level warn llm_error
--- llm_error done: passed=148 failed=0 (1 ms) ---
agentxx_test --log-level warn agent
--- agent done: passed=225 failed=0 (18632 ms) ---
```

## 待办

- 步骤 3 的记录实施后补
