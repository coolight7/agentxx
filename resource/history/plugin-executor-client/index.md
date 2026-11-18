# client 插件独立线程（同步桥 + 删除开关）

## 目标

client 插件代码不再跑在 client io 线程上：`ClientPluginManager` **首次加载插件时就起**
插件执行线程，`startPluginThread()` 这个可选开关删除。目标线程模型：

`agent io` + `agent 插件` + `client io` + `client UI` + `client 插件` + 后台任务线程池。

## 方案与分步

见 [plan.md](plan.md)，实施记录见 [work.md](work.md)。

## 状态

已完成（2026-11-18），全量测试 43820 通过 0 失败。

## 结论

- 卡点是"插件线程上同步等 io"：kit 的同步表助手把完成回调投回插件执行器，而那条线程
  正阻塞在等待上 → 自己等自己。解法是**同步桥**：`tableOpSyncWait` 把回调包成带标记的
  `SyncBridgeCallback`，主程序 `OpCore::commit` 识别标记后**在完成线程（io）就地调用**，
  就地跑的只有唤醒体，插件业务代码仍在插件线程继续。
- 准入策略按表分级：agent 侧跨插件调用可能把工作体派回同一条线程，默认
  `refuseOnPluginThread`；client 表与广播表工作体只碰 client 注册表，显式
  `allowOnPluginThread`。
- 顺带修掉三处真实缺陷：`invokeCommand` 捕注册表快照指针的 use-after-free（派发到
  插件线程后快照可能已被替换）、重复的键位判定块、client 定时器慢回调返回后"追补积压
  触发"（新增 `ClientTimerImpl::inFlight`）。
- 后续工作（另一条任务线）：把内置 client 插件的 `start`/`stop` 与回调体改成协程形态，
  届时 client 侧可以不再依赖同步桥。
