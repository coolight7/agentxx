# 实施记录

> 计划见 [plan.md](plan.md)，任务卡片见 [index.md](index.md)。

## 进度

| 步 | 内容 | 状态 |
|---|---|---|
| 1 | 框架：同步桥回调 + 就地调用 + 策略参数 | 已完成 |
| 2 | 主程序：client 表 / 广播表放行插件线程 | 已完成 |
| 3 | client 管理器默认起线程、删开关 | 已完成 |
| 4 | 测试（同步桥用例 + client_plugins 排空） | 已完成 |
| 5 | 文档与全量回归 | 已完成 |

## 每一步改了什么、怎么验证

### 步 1 框架（`agent/third_party/cxx_pluginxx`）

- 新增 `include/pluginxx/api/sync_bridge.h`：`SyncBridgeCallback{magic, fn, ud}`、
  `invokeSyncBridge`（先释放包装块再调唤醒体）、`isSyncBridgeCallback`、
  `SyncWaitThreadPolicy{refuseOnPluginThread, allowOnPluginThread}`。
- `runtime/op_driver.h`：`OpCore::commit` 的派发条件加 `!isSyncBridgeCallback(ud)` ——
  同步桥回调在完成线程就地调用。
- `kit/kit.h`：`tableOpSyncWait` 改用同步桥；新增末参策略（ctx 形态同参数）；
  前置声明同步带上 policy（否则调用不明确）。

### 步 2 主程序（`agent/lib`）

- `plugin/api/plugin_kit.h`：`clientTableOp` 与 4 个广播 host 助手传
  `allowOnPluginThread`（工作体只改注册表，不派回插件线程）。
- `plugin/client_plugin_manager.h`：`ClientTimerImpl` 新增 `inFlight`（回调未返回时
  到期直接跳过，见下）。
- `src/plugins/client_plugin_manager.cpp`：
  - 定时器 `fireTimerTick` 读 `inFlight`：未返回就计一次 `dropped` 并跳过。
  - `invokeCommand` 不再捕 `ClientCommand*`（快照可能被替换）→ 捕 `execute` + `ud`。
  - 删掉 `postKeybindInvocation` 里重复的判定块、删掉没人用的 `copyView` 指针重载。
  - 删 `startPluginThread()`，改私有 `ensurePluginThread()`（`loadNativeAsync` 开头调用，
    起不来只记 WARN）。

### 步 3 测试

- `test/plugin/test_plugin_executor.cpp` 新增第 9 组：标记语义 / 普通完成回调去插件线程 /
  插件线程上 `startTableOp` + 同步等 → 唤醒体在 io 就地跑（679 通过）。
- `test/plugin/test_client_plugins.cpp`：
  - 首载后断言 `mgr->hasPluginExecutor()`（线程自动起）。
  - 新增 `drainPlugin(mgr, 名字)`：向该实例投一个 FIFO 探针，等它跑到 —— 插件回调
    异步化后，断言前必须排空（注册表断言、适配器计数、动作命中数）。
  - READY 段按主题查（两条上行先后不固定）→ `hasDataTopic`。
  - 第 33 组定时器用例重写：单周期定时器第一次回调阻塞 370ms，用"相邻两次回调开始
    间隔 < 20ms 的次数为 0"验证不追补（原写法量"阻塞结束后首次回调的间隔"，在回调正好
    跨过到期点时判据不成立，已淘汰）。
- `test/plugin/test_plugin_executor.cpp` 头注释补第 9 项。

### 步 4 文档

- `docs/zh-cn/design/plugins.md`：§9（client 表同步助手放行 + 同步桥）、§9.5（回调未返回
  时跳过到期）、§15.3（线程表、派发规则、同步桥与准入策略）、§15.5（start 里的异步登记）、
  多处"默认 = client io 线程"改为"首次加载时自动起"。
- `AGENTS.md`：同步等待原语一段改写（同步桥 + 策略）、插件执行线程一段（client 侧自动起、
  目标线程模型）、client 六张表一段（同步助手可用 + 借用视图/快照指针规则）。

## 遇到的问题

- **`example_plugin` enable 报 `registration transaction failed`（复现 3 次）**：重建该插件
  后消失，随后 5 次连跑不再出现。归因为插件 DLL 与 kit 头不同步（内联的
  `tableOpSyncWait` 策略是旧的）。结论：改 kit/框架头之后必须重建全部插件
  （`agent/script/build_plugins.ps1`）。
- **`boundaries` 规则 15** 拦下"载体"一词（`sync_bridge.h` / `kit.h` / 测试注释），
  已改为"包装块 / 包装回调"。
- **client_plugins 里多处断言过早**：插件回调改到插件线程后必须排空；`uiRegistrySnapshot()`
  之类的读取本身是加锁快照，不需要额外同步。

## 验证

- `agentxx_test --log-level warn client_plugins` → 843 通过（连跑 5 次稳定）。
- `agentxx_test --log-level warn plugin_executor client_plugins` → 1522 通过 0 失败。
- 相关回归：`plugin_runtime` / `plugins` / `plugin_sdk` / `plugin_bridge` / `plugin_cleanup` /
  `plugin_host_mount` / `plugin_multi_mount` / `plugin_multi_instance` / `plugin_broadcast` /
  `plugin_dispatch` / `plugin_point` / `plugin_feature` / `plugin_agent_runtime` /
  `plugin_example_runtime` / `plugin_example_chatbot` / `agent_host` / `chatbot_task` /
  `tui_widget` / `tui_form` / `view_replica` / `tui_ui_items` / `ui_items` 全部 0 失败。
- 全量：`agentxx_test --log-level warn` → **43820 通过 0 失败**。

## 剩余事项

- 内置 client 插件（`example_plugin` / `agentxx_planning` / `agentxx_codegraph` /
  `agentxx_system_monitor` / `chatbot_client` / `libexample_js`，共 22 处同步表调用）与 kit 的
  client 表助手改协程形态，之后 client 侧可以不再用 `allowOnPluginThread`（同步桥保留给
  JS 引擎这类确实无法协程化的边界）。
