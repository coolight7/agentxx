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

## 待办

- 步骤 2、3 的记录见下方（实施后补）
