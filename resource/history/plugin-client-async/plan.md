# client 插件接口协程化 —— 并推广到“插件执行线程不等 io”

## 目标

插件执行线程与主程序 io 线程是两条线程，插件线程上**不允许**任何“同步阻塞等另一个
线程结果”的接口；跨线程等待一律写成协程/回调形态，等待期间线程可以继续做别的事。
本任务先做 client 侧（注册事务 + 回调体），再推广到 agent 侧的全部插件调用点。

## 现状

- 第一阶段（[plugin-executor-client](../plugin-executor-client/index.md)）已让 client 插件跑在
  独立线程上，同步助手靠 `SyncWaitThreadPolicy::allowOnPluginThread` 放行。
- 推广到全局后要处理的东西：client 表同步助手、业务表同步助手（`ctx.schema` /
  `modelConfigJson` / `promptJson` / `addShareStore` 等）、广播发布/订阅/声明/退订。

## 分步

| 步 | 内容 | 状态 |
|---|---|---|
| 1 | kit：client 表的协程形态便捷入口（`clientTableOpAsync` / `clientHandleOf`） | 完成 |
| 2 | `example_plugin` client start 改异步事务（协程 + `finishLater`） | 完成 |
| 3 | `agentxx_system_monitor` / `agentxx_planning` / `agentxx_codegraph` / `chatbot_client` / `libexample_js` 同上 | 完成 |
| 4 | 回调体改提交即返回（client 展示面 + 广播 + 业务表查询），插件线程彻底不等 io | 完成 |
| 5 | 收紧准入：kit 去掉全部 `allowOnPluginThread`，`boundaries` 拦阻塞式 client 入口 | 完成 |
| 6 | 主程序 → 插件方向：client io 线程不再内联跑 `dlopen`/`create` | 完成 |
| 7 | 实例级查询的协程形态（`configAsync` 等）+ `screen_capture` 改协程 start | 完成 |

## 边界与例外

- 不改 ABI 与表版本（协程形态只是 kit 的用法）。
- `stop` 仍然只交完成信号（主程序按记录撤销注册），不需要协程。
- **仅存的同步等待**（都是“必须当场拿到结果”的边界，已写进 plugins.md §15.3）：
  - `create` 期的实例级查询（参数 / 配置路径 / 清单 / 配置）—— `create` 在加载线程池上
    执行（不是 io、也不是插件执行线程），与 `dlopen` 同类；新增代码用
    `configAsync()` / `argsJsonAsync()` / `configPathAsync()` / `languageAsync()` 在
    `start` 里取。目前只剩 `agentxx_chatbot`（参数/配置路径）与 `example_js`（定位脚本）。
  - `blocking_tool` / `offload` 一类**已经在线程池上**的实现体（阻塞是它的语义）；
  - JS 引擎的脚本线程：QuickJS 是同步 C API，脚本回调里必须当场回答
    （`boundaries` 允许清单里只有它和 `example_js` 的 create 期清单查询）。
- 主程序 → 插件方向：所有入口都是投递（`dispatchToPlugin`）或协程等待
  （`awaitPluginLifecycle`）；`get_info` 只是静态元数据，`destroy` 只释放本地对象。
- 已知遗留：`agentxx_execute_command` 的 polled 实现体里 `StoreFn`（`addShareStore`）
  仍是同步形态；polled 协程跑在插件执行线程上，所以它现在拿不到 id（输出超限时退回
  截断）。正确做法是两段式：实现体只交原文，回到 `asio::awaitable` 里用
  asio 形态的表调等待写入结果。这条与本次改动无关（执行线程改造时就已如此），
  单独排期。
