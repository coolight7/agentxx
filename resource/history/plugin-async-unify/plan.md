# plan —— 插件侧接口彻底统一成异步（plugin-async-unify）

## 1. 目标

把插件与主程序之间"要跨线程拿结果"的调用统一成一种模型：**受理即返回、结果经完成回调交回**；
SDK 层不再保留"阻塞等结果"的入口。只剩两种形态：

| 形态 | 用途 | 是否阻塞 |
|---|---|---|
| `xxx(ctx, args)` → `TableOpTicket` | **调用即提交**；`co_await` 拿结果 / 存起来稍后再 await / `.forget()` 提交完不管 | 不阻塞 |
| `xxxCallback(ctx, args, done)` | 没有协程的上下文（`start` / `mount` / `stop` / 通知型回调）要结果 | 不阻塞 |

`co_await whenAll(t1, t2, …)` 等一批已提交的 ticket 全部完成（总耗时是最慢那条，不是相加）。

前置条件：`start` / `mount` / `stop` / `unmount` 允许**异步完成**（宿主早已支持 notify，见 §3.4），
析构路径不再调接口表（收尾统一搬到 `stop` / `unmount`）。

## 2. 现状调查（文件与函数级）

### 2.1 接口表层：已经全是异步契约

19 张表 + `agentxx.net`（v2）+ client 侧 16 张，每个成员都是
`(…, request_id_out?, cb, cb_ud, error_out) → PluginxxOperatorHandle*`：返回非 NULL = 已受理，
结果经 `PluginxxOperatorCallback` 恰好一次交回；返回 NULL = 未受理 + `error_out`。
**本轮不改任何表成员**（见 §3.6）。

### 2.2 主程序侧执行框架

`agentxx/third_party/cxx_pluginxx/include/pluginxx/host/table_op.h` 的 `startTableOp`：

- 工作体在管理器自己的 IO 线程执行；**调用方本来就在 IO 线程时内联执行**，`cb` 在本次调用返回前被调用
  （注册类调用"登记完立刻可见"的语义与老同步调用一致）；
- 其它线程调用时投递到 IO 线程，按受理顺序排队（FIFO）；
- `cb` 可为空 = "只受理不回程"；
- 句柄按 `pluginxx/api/abi.h`（约 162 行）"**仅用于取消；不可轮询/收尸**" —— 所以
  "提交后稍后再等"必须在 SDK 侧做（见 §3.2）。

### 2.3 kit 现状：已有三种形态（改造后只剩两种）+ 一批同步包装

框架 kit（`pluginxx/kit/kit.h`）已有：

- `tableOp(ctx, start)` —— awaiter（约 3975 行）
- `tableOpCallback(ctx, start, done)` —— 回调形态（约 3988 行）
- `tableOpBlocking(ctx, start, timeoutMs)` / `tableOpBlockingRaw(host, cancel, scheduler, start, timeoutMs)`
  —— 阻塞形态（约 4032 / 4036 行），**就是本轮要删的东西**
- `invoke_capability_blocking(...)`（约 4395 行）

主程序 kit（`agent/lib/include/agentxx/plugin/api/plugin_kit.h`）里建在它们之上的**同步包装**
（括号内是 `agent/plugins/**` 里的调用点数量，`git grep` 统计）：

- 广播：`publishPluginBroadcast`(15) / `declarePluginBroadcast`(5) /
  `subscribePluginBroadcast`(23) / `unsubscribePluginBroadcast`(6)
- 提示词与图：`promptJson`(3) / `setPromptJson`(7) / `graphJson`(2) / `setGraphJson`(2)
- 模型与自身信息：`modelConfigJson`(3) / `ownResourcesJson`(1)
- 资源登记：`registerSkillDir`(2) / `registerMemoryFile`(1) / `registerMcpServer`(1)
- 权限：`registerReadPathPermission`(5) / `registerWritePathPermission`(5) /
  `checkPathDecisions`(1) / `filterPathPermissions`(1)
- share store：`addShareStore`(4)
- 能力行：`capability`(2) / `providePoint`(6)
- 会话运行时与后台任务：`mountRoot`(2) / `openChannel`(3) / `sendInput`(5) /
  `addTrigger`(1) / `addRuntimeTimer`(1) / `submitAsyncWork`(1)
- 直接调阻塞原语：`tableOpBlocking`(3)
- 工具互调：`call_tool_blocking`(1)

合计约 110 个调用点（不含 `isCanceled` 这类本地查询）。

**注意**：`cancel->isCanceled()` / `shim->ctx->cancelRegistry.isCanceled(...)` 是**本地**查询
（`PluginxxCancelToken` 与 kit 的 `CancelRegistry`），不走接口表，**必须保持同步**。
统计里那 22 处 `isCanceled` 属这一类，不进入迁移清单。

### 2.4 生命周期：异步完成已支持（本轮不加超时）

`pluginxx/runtime/op_driver.h` 的 `awaitPluginLifecycle`：把 `start` / `stop` / `mount` / `unmount`
当一次操作跑（`OpCore` + notify），宿主 `co_await core->wait()` 之后才推进下一阶段。
`agentxx_javascript_engine` 的 `stop` 已经是"另起收尾线程，好了再 `notify->done`"。

缺的两点：

1. `core->wait()` **没有超时**（`OpCore` 的定时器是 `time_point::max()`）—— 插件不 done 就永久卡住加载/卸载；本轮**不加**超时（见 §3.4，限时由插件自己决定）；
2. kit 没有"稍后完成某次生命周期"的便捷对象，插件只能自己摸 `notify`，容易写成"既不 done 也不返回"。

### 2.5 kit 的协程基础设施（够用，不需要新建）

`PollOneBridge`（driver + 本地执行器）、`kit::spawn(ctx, fn)`、`kit::polled_tool`、
`kit::offload(ctx, work)`（工作体丢到主程序工作线程池，完成后回到 driver）、
`kit::sleep` / `kit::yield`、`kit::invoke_cap`、`netCall` / `netFetch`。
`polled_tool` 的业务体是 `awaitable`，由 driver 推进，因此**不能在里面直接做 CPU 重活**（用 `offload`）。

### 2.6 插件现状

`blocking_tool` / `polled_tool` 分布（`git grep` 统计）：

| 插件 | blocking_tool | polled_tool | 备注 |
|---|---|---|---|
| agentxx_chatbot | 10 | 0 | 工具体都走 `runToolOnIo` → `runOnIoBlocking` 等插件 io 线程（有 ASan 踩坑记录） |
| agentxx_filesystem | 10 | 3 | 纯文件 IO / CPU，保持 blocking |
| agentxx_codegraph | 5 | 0 | CPU，保持 blocking |
| agentxx_execute_command | 2 | 2 | 子进程等待，保持 blocking |
| agentxx_string / agentxx_math | 2 / - | 0 | CPU，保持 |
| agentxx_websearch | 2 | 0 | 体内等 `http.fetch` → 该改 coroutine |
| agentxx_rag_search | 1 | 0 | 体内等 embedding `http.fetch` → 该改 coroutine |
| agentxx_text_selection_monitor | 1 | 0 | 体内等 `http.fetch` + `ws.connect` → 该改 coroutine |
| agentxx_computer_use / screen_capture / audio_stream / system_monitor | 各 1 | 0 | OS 阻塞，保持 |

上一批（`plugin-api-async` 第四批）为了删掉 `netFetchBlocking`，在 4 个插件里各写了一份私有
`netCallWait`（回调 + 条件变量）：`agentxx_websearch`、`agentxx_rag_search`、
`agentxx_text_selection_monitor`、`agentxx_chatbot`（`chatbot_net.cpp`）。本轮的 C 批把它们一起删掉。

内嵌运行时类插件（`agentxx_chatbot`、`agentxx_text_selection_monitor`、`agentxx_filesystem` 的
`fsMount` 等）在 `stop` / `unmount` 里做收尾；只有 `text_selection_monitor` 的 `~TextSelectionPluginCtx`
会在析构里 `holder.stop()`（内部仍会碰网络表，见 §3.5）。

### 2.7 client 侧还没做的部分（原计划的阶段 D）

- 三个"回答型"回调仍要求同步返回：工具折叠内容、工具展开内容、斜杠命令 `execute`；
- 为此保留了 `JsEngine::invokeRenderSync` / `invokeCallbackSync` 与 `kClientSyncTimeoutMs`（150 ms）
  这条同步桥；`agentxx/client/.../ui_components.cpp` 的渲染路径依赖它。

## 3. 目标设计

### 3.1 一个操作两种形态、命名统一

- `xxx(ctx, args…)` → **调用即提交**，返回 `TableOpTicket`；`co_await` 拿结果，`.forget()` 放弃结果
- `xxxCallback(ctx, args…, done)` → 无协程上下文用（`done(TableOpResult)`，主程序 io 线程恰好一次）

不设 `xxxSubmit`：ticket 已经是"提交后返回的对象"，`co_await xxx(...)` 与
`auto t = xxx(...)` 是同一个入口的两种用法。

旧同步名 `xxx(...)`（阻塞实现）在迁移期改名为 `xxxBlocking(...)`（`[[deprecated]]`），
E 批连同框架的 `tableOpBlocking*` 一起删除。改名由编译器驱动找齐调用点。

**行为变化要写进文档**：

1. 调用了就一定会执行（哪怕随后提前 `return`）—— 结果丢弃而已；
2. `co_await` 只允许一次；重复 await 是 bug（运行期断言）；
3. `~TableOpTicket` 不取消（取消是显式动作 `cancel()`）；
4. 不要无界连发（提交进的是同一实例的 FIFO 队列）。

### 3.2 Ticket：提交即可等

```cpp
class [[nodiscard]] TableOpTicket {        // move-only
public:
    bool        accepted() const noexcept; // 受理成功?
    std::string startError() const;        // 未受理的原因
    void        cancel();                  // cancel.op_cancel (显式取消)
    void        forget();                  // 明确的"提交完不管"(丢弃结果)
    // co_await 它 → TableOpResult；已完成的立即恢复；重复 await 是 bug (断言)
};

// 等一批已提交的 ticket 全部完成 (按入参顺序返回结果)
asio::awaitable<std::vector<TableOpResult>> whenAll(std::vector<TableOpTicket>&& tickets);
```

实现要点：

- 构造时**就提交**（`startTableOp`），状态放 `shared_ptr`（沿用 awaiter 的状态机）；
- `co_await` 只是"等这个已提交的操作"，不再提交；调用方已在主程序 io 线程时工作体内联跑完，
  `await_ready()` 直接为 true（一次挂起都不发生）；
- `[[nodiscard]]` + `forget()`：防止"忘了 await"把失败原因默默吞掉；
- `whenAll` 只需"逐个 await"：ticket 提交时不阻塞，总耗时 = 最慢那条；
- 不取消即不取消：`~TableOpTicket` 不碰操作（取消是显式动作）。

网络侧同一套：`NetTicket`（复用 `detail::NetAsyncState`）。

### 3.3 顺序契约（写进头文件与文档）

1. 同一插件实例的表操作**按受理顺序 FIFO 执行**（宿主 IO 线程队列）；
2. 调用方已在宿主 IO 线程时**内联执行**，注册完立刻可见；
3. 把活交给别的线程的操作（网络服务、后台任务）**只保证入口按序**，完成顺序不保证 ——
   要完成顺序就 `co_await`（awaiter 或 ticket）。

结论：`xxx` 之后立刻 `xxx` 下一个，执行顺序仍然有保证；**不保证的是"何时完成"** ——
要完成顺序就 `co_await`（awaiter 或 ticket 都行）。

### 3.4 生命周期异步完成

kit 新增（`plugin_kit.h`，放在 `PluginBase` 附近）：

```cpp
class LifecycleFinisher {                  // move-only, 每个 notify 恰好一次
public:
    void ok();
    void fail(std::string_view reason);
};

LifecycleFinisher finishLater(Ctx& ctx, const PluginxxOperatorNotify* notify);
```

用法：

```cpp
void* myStart(Ctx& ctx, const PluginxxOperatorNotify* n, PluginxxString* err) {
    auto fin = agentxx::plugin::finishLater(ctx, n);
    agentxx::plugin::spawn(ctx, [&ctx, fin = std::move(fin)]() mutable -> asio::awaitable<void> {
        const auto r = co_await agentxx::plugin::tableOp(ctx, /* ... */);
        if (!r.ok()) { fin.fail(r.payload); co_return; }
        fin.ok();
        co_return;
    });
    return &ctx;                            // 实例先交回去, 完成信号稍后到
}
```

宿主侧（`pluginxx`）：`awaitPluginLifecycle` 保持现状（已经是"等 notify"，不改）。

**不做生命周期超时**（刻意）：不同插件"正常需要多久"差别太大（建索引、连平台、起子进程），
定一个数只会逼插件去适配它。谁发起异步操作，谁负责给它加上自己需要的限时。
代价也要接受：插件如果永不 `notify->done()`，本次加载/卸载就一直等着 —— 那是插件的 bug，
宿主不为它设定时限。

### 3.5 析构不调接口表

- 收尾统一放 `stop` / `unmount`（可异步完成）；
- 析构只释放本地资源，不投递表操作、不订阅/退订；
- `boundaries` 增加一条静态检查：插件源码里"析构函数体 30 行内出现表调用包装"直接报错；
- 迁移点：`text_selection_monitor` 的 `~TextSelectionPluginCtx`（`holder.stop()` → 搬到 `stop` 回调）。

### 3.6 ABI 与版本

- **不改任何表成员** → 不抬全局代次（停在 7）；`*Blocking` 全是头文件 inline，删除属源码级破坏；
- 插件必须按新头重编（`plugin_kit.h` 变化），但宿主与插件的二进制契约没变；
- 如果实施中发现必须新增表成员（例如给 `scheduler` 加 `lifecycle` 相关），再单独抬代次。

### 3.7 工具形态的选择规则（写进文档）

| 工具体内容 | 形态 |
|---|---|
| 只做 CPU / 阻塞式系统调用（文件、子进程、屏幕、音频） | `blocking_tool` |
| 要等接口表 / 网络 / 定时器 | `polled_tool` + `co_await`；CPU 重活套 `offload` |
| 只是查本地状态、几乎不耗时 | `fast_tool` |

### 3.8 数据兼容与重编要求

- 不动任何落盘格式（会话库、用量记录、share store 均不变）；
- 版本与二进制契约见 §3.6（不抬代次，停在 7）；
- `plugin_kit.h` 的源码级破坏会强迫全部插件重编（头文件 inline 函数改名/删除），
  旧插件库与新宿主仍能互相加载。

### 3.9 与"插件独立线程"重构的关系（为什么本轮先做）

现状（`plugins.md` §16）：插件协程的**每一步**都由主程序 io 线程经 `request_driver` →
`poll_one` 推进，插件代码与主程序协程**在同一个 io 执行序列里交错**；`blocking_tool` 体与
`offload` 工作体在主程序工作线程池；JS 插件在自己的 JS 线程；少数插件（chatbot、
text_selection_monitor）另有自己的线程，但它们调接口表仍然要回到主程序 io 线程。

`resource/history/plugin-executor/` 计划把插件代码整体搬到"插件线程"。**必须先做完本轮**：

1. 现在 `*Blocking` 之所以"看起来不慢"，是因为调用方常常就在主程序 io 线程上，
   表操作**内联**执行、一次线程切换都没有；插件搬到另一条线程后这条快路径消失，
   每次表调用都会变成"插件线程 → io 线程 → 插件线程"的真实往返；
2. 那时阻塞形态还会带来互等风险（`stop` 时 io 等插件收尾，插件收尾里等 io 的表调用）；
3. 本轮把等待全部换成"提交 + 挂起"，插件线程重构就只剩"谁来 driver 推进"这件事
   （`PollOneBridge` 已经抽象好了），**调用点不用再改第二遍**。

反过来，本轮也不引用任何"我是否在 io 线程"的判断 —— 插件线程重构完成后
`scheduler.is_io_thread` / `ioCallSync` 这类机制要删掉，新代码不该再依赖它。

## 4. 边界（本轮不做）

- 不改表成员签名、不改完成回调类型、不新增表；
- 不动"插件提供实现、主程序调用"那一侧（工具 / 能力行 / 图节点 / 生命周期回调本身的形态）；
- 不把 `blocking_tool` 删掉（它是工具体形态，不是表等待）；
- client 侧三个回答型回调与 150 ms 同步桥：不在本轮（上一个任务完成后再继续）；
- 不改 `isCanceled` 等本地查询。

## 5. 分阶段实施

### 批 A：框架层 ticket 与顺序契约

- `pluginxx/kit/kit.h`：`tableOp(ctx, start)` 改成**调用即提交**并返回 `TableOpTicket`
  （`[[nodiscard]]` + `forget()` + `cancel()`；内部仍是现在那套状态机）；新增
  `whenAll(std::vector<TableOpTicket>&&)`；`tableOpBlocking*` 加 `[[deprecated]]`（暂留）；
- `pluginxx/host/table_op.h`：补顺序契约注释；确认 `cb = NULL` 路径无泄漏（已有测试覆盖）；
- 测试：`plugin_sdk` 新增 ticket 用例（调用即提交 / 稍后 await / 已完成立即恢复 / `forget()` 仍执行 /
  `whenAll` 全部完成后返回 / 未受理时 `accepted()==false` / 重复 await 断言）；
  `plugin_runtime` 新增 FIFO 用例（A→B 提交顺序 = 执行顺序）；
- 提交：子模块 + 父仓库指针。

### 批 B：kit 补形态并改名（主程序侧）

按区块推进，每块都"改头 → 编译器驱动改调用点 → 编译通过"：

1. 广播 4 个（`publishPluginBroadcast` 顺带去掉内部隐式 declare：声明改到 `mount` 做一次，
   少一次往返）；
2. 提示词 / 图 / 模型 / 自身信息 / 资源登记 8 个；
3. 权限 4 个 + share store 1 个 + 能力行与埋点 2 个；
4. 会话运行时与后台任务 6 个；
5. 网络：`netCall` / `netFetch` 也改成"调用即提交 + 返回 `NetTicket`"；删掉 `netSubmit`
   （它的语义就是 `netCall(...).forget()`）。

每块提交前跑 `plugin_sdk` / `plugin_host_mount` / `boundaries`。

### 批 C：插件迁移（编译器驱动）

1. 25 个插件按报错改调用点（原来是同步调用的 → `co_await xxx(` / `xxx(` + `.forget()` /
   `xxxCallback(`；原来就是 `co_await` 的形状不用改）；
2. 删 4 个私有 `netCallWait`；`websearch` / `rag_search` / `text_selection_monitor` 的工具体改
   `polled_tool`：网络 `co_await netFetch`，CPU 重活 `co_await offload(ctx, …)`；
   `rag_search_impl.h` 的 `EmbedFn`、`websearch_impl.h` 的 `HttpFn` 改成返回 `awaitable`，
   并同步改测试里的注入实现；
3. `chatbot`：10 个工具全部改 `polled_tool`（去掉 `runToolOnIo` / `runOnIoBlocking` 那条"等插件
   io 线程"的阻塞路径），平台调用改成 `co_await` 插件 io 线程上的协程（经 `asio::co_spawn`）；
   这一项单独一个提交，方便单独回归；
4. 全量插件重编 + 插件相关测试全绿。

### 批 D：生命周期异步

- 框架：宿主侧不改（`awaitPluginLifecycle` 已支持 notify，且不加超时）；
- 主程序：不加任何时限配置键；
- kit：`LifecycleFinisher` / `finishLater`；
- 插件：至少改一个真实用例（`text_selection_monitor` 析构收尾 → `stop` 异步；`chatbot` 的
  `stop` 里等 io 线程收尾）；
- 测试：`plugin_host_mount` 新增"start 延时 done / start 失败 / stop 异步收尾 / 完成调两次"用例。

### 批 E：删除阻塞形态

- 删 `tableOpBlocking` / `tableOpBlockingRaw` / `invoke_capability_blocking` / `call_tool_blocking`；
- 删 `plugin_kit.h` 里全部 `*Blocking` 包装；
- `boundaries` 增加禁令：`agent/plugins/**` 与 `plugin_kit.h` 不得出现 `*Blocking(` 的**表调用**用法
  （`blocking_tool` 形态不在此列）；
- 文档：`plugins.md` §8.0 调用模型、`AGENTS.md`、`docs/zh-cn/design/plugins.md` §22。

### 批 F（不做）

client 侧三个回答型回调与 150 ms 同步桥不在本轮：上一个任务（`plugin-api-async`）剩下的
阶段 D/E 在本轮完成后再继续实施。

### 批 G：收尾

- 全量回归 + 一次"插件卸载/禁用"路径的 ASan 复核（析构不再调表）；
- 文档补"两种形态怎么选"（`xxx` 返回 ticket / `xxxCallback`）与顺序契约；
- 本任务三个文件补结论，`AGENTS.md` 只留规则。

## 6. 验证计划

每批：

1. 重建顺序：fw（build + install）→ lib（`agentxx_static` + install）→ test（编完把
   `Debug/agentxx_test.exe` 拷到 `exec/`）→ 点名插件目标重编；
2. 目标模块：`plugin_sdk` / `plugin_runtime` / `plugin_host_mount` / `plugin_multi_mount` /
   `plugin_bridge` / `plugin_dispatch` / `plugin_net` / `plugins` / `client_plugins` /
   `chatbot_media_io` / `async_tools` / `event_subagent`；
3. 全量回归（约 43k 条）：预期只剩已知的 `ui_snapshot` `diff_json` 基线失败；
4. 性能对比：同一份 benchmark（`agentxx_benchmark` 的 resource 模块）跑一次改动前后，
   确认"提交 + 回调"没有把插件工具链的耗时抬上去。

## 7. 风险与应对

| 风险 | 应对 |
|---|---|
| 插件 `start` 永不 done → 加载卡死 | 接受（不做超时）；文档要求插件给自己发起的异步操作限时，并保证 `finishLater` 恰好完成一次 |
| 异步完成漏调 / 调两次 | `LifecycleFinisher` 内部原子标记；测试覆盖"调两次" |
| 大批量改名把树改坏 | 每区块编译+测试后才继续；每批独立提交 |
| `polled_tool` 体内跑 CPU 重活占住宿主 IO 线程 | 规则写进文档；`websearch` / `rag_search` 用 `offload` |
| 析构里仍有表调用（漏改） | `boundaries` 静态检查 + 卸载路径 ASan 复核 |
| ticket 被重复 await / 跨协程 await | 运行期断言 + 文档说明"单次 await" |

## 8. 决策点（已定）

- 只保留 `xxx`（调用即提交、返回 ticket）与 `xxxCallback` 两种形态，**不设 `xxxSubmit`**；
- ticket 类型 `[[nodiscard]]`，fire-and-forget 写 `.forget()`；
- 支持并发多条 ticket 与 `whenAll`（逐个 await，总耗时 = 最慢那条）；
- 旧同步名迁移期叫 `xxxBlocking`，E 批删除；
- 不做生命周期超时；`chatbot` 10 个工具一并改；不做 client 侧回答型回调；
- 不改表成员、不抬 API 代次（停在 7）。
