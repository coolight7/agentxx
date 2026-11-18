# client 插件接口协程化（并推广到插件线程不等 io）

## 目标

插件执行线程与主程序 io 线程彻底分开：插件线程上没有任何“同步阻塞等另一个线程结果”
的接口，跨线程等待一律协程/回调；client 侧不再依赖
`SyncWaitThreadPolicy::allowOnPluginThread`。

## 方案与分步

见 [plan.md](plan.md)，进度与验证见 [work.md](work.md)。

## 状态

完成（2026-11-18）。9 步全部落地：kit 协程入口 → 内置插件事务/回调改造 →
全局推广（业务表查询、广播、展示面）→ 收紧准入 + 边界规则 → chatbot start 协程化 →
JS SDK 全面 Promise 化。

## 验证

最终源码状态跑全量 `agentxx_test --log-level warn`：退出码 `0`（框架只在 `failed > 0` 时返回
`1`），通过数 43825。逐模块结果见 [work.md](work.md) 的“全量结果”与“步 8/9 验证”。

## 结论

- **插件执行线程不再阻塞**：kit 里所有表调用入口都用默认的
  `refuseOnPluginThread`，插件线程上同步等直接失败并带原因文本（操作已提交，但拿不到
  结果）；替代写法是 `co_await`（要结果）或 `cb = nullptr` / `*NoWait`（不要结果）。
- **主程序线程也不阻塞**：所有插件入口都是投递 + 协程等待；client 侧原本在 io 线程上
  内联跑 `dlopen`/`create`，现在与 agent 侧一致走加载线程池。
- **新增的 kit 形态**：`clientTableCall`（提交即返回）、`ensureInfoSection` /
  `infoSection` / `clearInfoSection`（懒注册 Info 段落）、`clientStateAsync` /
  `sendUserInputAsync` / `configAsync` / `argsJsonAsync` / `configPathAsync` /
  `languageAsync`、四个广播 `*NoWait`；工具注册不再回读提示词表。
- **插件侧**：`ctx.schema(...)` → `co_await schemaOf(ctx, ...)`（约 20 处）、广播发布/声明/
  退订全改 `*NoWait`、screen_capture 的 start 改协程、system_monitor 的 `sysinfo` 改成协程里
  `co_await clientStateAsync()`。
- **边界规则**：`boundaries` 新增“插件源码不得直接使用阻塞式 client 入口”
  （白名单只有 JS 引擎与 `example_js` 的 create 期清单查询）。
- **保留的同步等待**：`create` 期查询（加载线程池）、`blocking_tool` 一类线程池上的
  实现体 —— 理由写在 plugins.md §15.3/§15.5。JS 引擎已不再用它（步 9 把脚本侧表调用
  也改成了 Promise）。
- **JS 脚本侧也是异步的**：表成员一律返回 Promise（业务失败 resolve 成 `null`/`false`，
  永不 reject），脚本顶层可以写 `await`；只有纯本地函数（`log`/`jsonEscape`/`setTimeout`
  等）与“必须当场给内容”的回调（渲染器 `collapsed`/`expanded`、命令 `execute`）保留同步。
  引擎内部因此多了几条不变式（实例先登记再执行、桥由引擎持有并按“取消结算”收尾、
  上下文/实例延后到无 job 时释放、定时器只持实例 `weak_ptr`），见 plugins.md §15.5。
- **`tableOpCallback` 的 `user_data` 必须是真实内存**：库内原来直接 `new Done(...)`
  （空函子只 1 字节），而主程序会对 `user_data` 按指针读 8 字节标记 → ASan
  heap-buffer-overflow。已在库内改成带 `magic` 字段的 `Holder`。
- 遗留：`agentxx_execute_command` 的 `StoreFn` 两段式改造（与本次无关）。
