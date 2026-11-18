# 实施记录

> 计划见 [plan.md](plan.md)，任务卡片见 [index.md](index.md)。

## 进度

| 步 | 内容 | 状态 |
|---|---|---|
| 1 | kit：client 表协程形态便捷入口 | 完成 |
| 2 | `example_plugin` client start 改异步事务 | 完成 |
| 3 | 其余内置 client 插件同上 | 待开始 |
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

## 步 3：example_plugin client start

`agent/plugins/example_plugin/example_plugin.cpp`：

- `exampleClientSetup` → `exampleClientSetupAsync(ClientCtx&) -> pluginxx::Task<void>`：
  状态栏项 / 面板 / Info 段落 / 2 个命令 / 3 条广播订阅，全部 `co_await clientTableOpAsync`，
  句柄由 `clientHandleOf<T>` 取；任何一条拒绝抛 `std::runtime_error`（带表返回正文）；
- `exampleClientStart`：`finishLater(notify)` + `ctx.spawn(...)` + `return later.provider()`；
  协程里 `later.ok()` / `later.failed(e.what())`；
- `stop` 不变（只交完成信号，主程序按记录撤销注册）。

验证：`client_plugins` 843 通过（start 事务异步后注册仍在 load 返回前生效——
`loadNativeAsync` 等 start 事务）；`plugin_executor` 679 通过。

## 遇到的编译问题

- `ClientPluginBase` 原来自己声明 `host/iface/log/init`，改成继承后要删掉这些重复成员，
  否则 `init` 会隐藏基类版本；
- `PluginBaseT` 的 `spawn` 需要 `iface.tasks`（`register_task`），`ClientIfaces` 原来
  没有它 —— 已补（client 侧本来就有 `pluginxx.tasks` 通用表）；
- `PluginStringView` + `std::string` 不能直接相加（`"..." + res.text()` 编译失败），
  统一改 `fmt::format("...: {}", res.text())`。

## 验证

- 全部 25 个插件重新编译通过（`build_plugins.ps1 -KeepGoing`，487.9s）。
- `client_plugins` 843 / `plugin_executor` 679 / `plugin_runtime` 743 / `plugin_sdk` 141 /
  `plugin_bridge` 271 全部 0 失败。
- 全量 `agentxx_test --log-level warn` → **43820 通过 0 失败**。

## 剩余事项

1. `agentxx_system_monitor`(6) / `agentxx_planning`(4) / `agentxx_codegraph`(3) /
   `chatbot_client`(2) / `libexample_js`(1) 的 client start 同样改成协程事务；
2. 回调体（`setStatusJson` / `setPanelItems` / `setInfoSectionItems`）改提交即返回，
   测试从"排空"改为轮询内容（`drainPlugin` 之后还要等内容落地）；
3. 收紧准入：client 侧去掉 `allowOnPluginThread`，补"插件线程上同步等 io 被拒绝"的断言；
4. JS 引擎的同步桥保留（QuickJS 的 C API 要求当场回答），不在这条任务线内。
