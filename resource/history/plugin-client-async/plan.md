# client 插件接口协程化（第二阶段）

## 目标

内置 client 插件的注册事务与回调体从"同步表助手"改为协程/提交即返回，最终让 client 侧
不再依赖 `SyncWaitThreadPolicy::allowOnPluginThread`（同步桥只留给 JS 引擎这类确实无法
写成协程的边界）。

## 现状

- 第一阶段（[plugin-executor-client](../plugin-executor-client/index.md)）已让 client 插件跑在
  独立线程上，同步助手靠同步桥放行。
- 同步助手调用点（插件源码，共 22 处）：`example_plugin`(6) / `agentxx_system_monitor`(6) /
  `agentxx_planning`(4) / `agentxx_codegraph`(3) / `chatbot_client`(2) / `libexample_js`(1)。

## 分步

| 步 | 内容 | 状态 |
|---|---|---|
| 1 | kit：client 表的协程形态便捷入口（`clientTableOpAsync` / `clientHandleOf`） | 进行中 |
| 2 | `example_plugin` client start 改异步事务（协程 + `finishLater`） | 待开始 |
| 3 | `agentxx_system_monitor` / `agentxx_planning` / `agentxx_codegraph` / `chatbot_client` / `libexample_js` 同上 | 待开始 |
| 4 | 回调体（状态栏/面板/Info 更新）改提交即返回，测试从"排空"改为轮询内容 | 待开始 |
| 5 | 收紧准入：client 侧去掉 `allowOnPluginThread`，补反向断言 | 待开始 |

## 边界

- 不改 ABI 与表版本（协程形态只是 kit 的用法）。
- `stop` 仍然只交完成信号（主程序按记录撤销注册），不需要协程。
- JS 引擎侧的表调用同步桥保留（QuickJS 的 C API 要求当场回答）。
