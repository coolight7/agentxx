# 实施记录

> 计划见 [plan.md](plan.md)，任务卡片见 [index.md](index.md)。

## 进度

| 步 | 内容 | 状态 |
|---|---|---|
| 1 | kit：client 表协程形态便捷入口 | 完成 |
| 2 | `example_plugin` client start 改异步事务 | 完成 |
| 3 | 其余内置 client 插件同上 | 完成（planning 无 start 期注册） |
| 4 | 回调体改提交即返回 | 待开始 |
| 5 | 收紧准入（去掉 `allowOnPluginThread`） | 待开始 |

## 步 1：kit 便捷入口

`agent/lib/include/agentxx/plugin/api/plugin_kit.h`：

- `clientTableOpAsync(ctx, start)` —— 插件 Ctx 形态的提交（转发 `pluginxx::tableOp`），
  返回 `TableOpTicket`，可 `co_await` / `.forget()` / `whenAll` 批量收；
- `clientHandleOf<Handle>(result)` —— 句柄类结果取句柄（失败/未受理 → nullptr）；
- `ClientIfaces` 增加 `tasks`（`pluginxx.tasks`）：“spawn 需要它登记取消”。

## 步 2：ClientPluginBase 接入协程驱动

`ClientPluginBase` 改为继承 `pluginxx::PluginBaseT<ClientIfaces>`：

- 得到 `bridge()` / `spawn(this Self&, Fn&&)` / `stopBridge()` 与统一的
  `host` / `iface` / `log` / `init()`（`init` 里查表 + 调 `onHostReady()`）；
- client 侧自己的便捷方法（`setStatusJson` / `setPanelItems` / `showToast` / 渲染器
  注册 / 定时器 / 快捷键…）保持原样，内部仍是同步助手（后续步 4 再改）。

## 步 2 附：example_plugin client start

`agent/plugins/example_plugin/example_plugin.cpp`：

- `exampleClientSetup` → `exampleClientSetupAsync(ClientCtx&) -> pluginxx::Task<void>`：
  状态栏项 / 面板 / Info 段落 / 2 个命令 / 3 条广播订阅，全部 `co_await clientTableOpAsync`，
  句柄由 `clientHandleOf<T>` 取；任何一条拒绝抛 `std::runtime_error`（带表返回正文）；
- `exampleClientStart`：`finishLater(notify)` + `ctx.spawn(...)` + `return later.provider()`；
  协程里 `later.ok()` / `later.failed(e.what())`；
- `stop` 不变（只交完成信号，主程序按记录撤销注册）。

验证：`client_plugins` 843 通过（start 事务异步后注册仍在 load 返回前生效——
`loadNativeAsync` 等 start 事务）；`plugin_executor` 679 通过。

## 步 3：其余内置 client 插件

- `agentxx_system_monitor`：`sysMonClientSetup` → `sysMonClientSetupAsync`（Info 段落 /
  用量频道订阅 / `sysinfo` 命令三条提交后 `co_await`；失败只记日志，与原行为一致），
  start 改 `finishLater` + `spawn`；
- `agentxx_codegraph`：`codegraphClientStart` → `codegraphClientSetupAsync`（Info 段落 +
  两条跨端订阅，失败抛错）；`ClientCtx` 改为继承 `ClientPluginBase`，create 改调 `init(host)`；
- `agentxx_chatbot`（client 侧）：`chatbotClientSetup` → `chatbotClientSetupAsync`（面板注册 +
  必需频道必需成功、READY / 连接状态两个频道失败只记日志）；
- `agentxx_planning`：**没有 start 期注册**——Info 段落是频道回调用到时才懒注册
  （`ensureSection`），留在回调体那一批（步 4）处理；
- `example_js`：只有 create 期一次 `get_own_info` 同步查询（loader 线程上，不是注册事务），
  保留。

## 遇到的编译问题
- `ClientPluginBase` 原来自己声明 `host/iface/log/init`，改成继承后要删掉这些重复成员，
  否则 `init` 会隐藏基类版本；
- `PluginBaseT` 的 `spawn` 需要 `iface.tasks`（`register_task`），`ClientIfaces` 原来
  没有它 —— 已补（client 侧本来就有 `pluginxx.tasks` 通用表）；
- `PluginStringView` + `std::string` 不能直接相加（`"..." + res.text()` 编译失败），
  统一改 `fmt::format("...: {}", res.text())`。

## 验证

- `example_plugin` / `agentxx_system_monitor` / `agentxx_codegraph` / `agentxx_chatbot`
  逐个重建通过；`ClientPluginBase` 头变更后全部 25 个插件重建也通过。
- `client_plugins` 843 / `plugin_executor` 679 / `plugin_runtime` 743 / `plugin_sdk` 141 /
  `plugin_bridge` 271 全部 0 失败。
- 全量 `agentxx_test --log-level warn` → **43820 通过 0 失败**。

## 剩余事项

1. 回调体（`setStatusJson` / `setPanelItems` / `setInfoSectionItems` / `clientState()`）
   改提交即返回，测试从"排空"改为轮询内容（`drainPlugin` 之后还要等内容落地）：
   `agentxx_planning` 的 `ensureSection` / `refreshPlanSection`、`agentxx_codegraph` 的
   `refreshSection`、`agentxx_system_monitor` 的 `refreshUsageDisplay` 等；
2. 收紧准入：client 侧去掉 `allowOnPluginThread`，补"插件线程上同步等 io 被拒绝"的断言；
3. JS 引擎的同步桥保留（QuickJS 的 C API 要求当场回答），不在这条任务线内。
