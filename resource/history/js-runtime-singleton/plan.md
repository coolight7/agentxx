# JS 运行时单例重构设计

> 目标读者: 修改 `agentxx_javascript_engine` / 编写 JS 插件的开发者
> 相关文档: [plugins.md](../../../docs/zh-cn/design/plugins.md) (插件系统总览, §12 是当前 JS 插件说明) · [plugin-api-async plan](../plugin-api-async/plan.md) (插件接口表异步化, 本设计的前置项) · [index.md](../../../docs/zh-cn/design/index.md)
>
> 本文件是任务规划 (未实施), 落地后结论搬进 [plugins.md](../../../docs/zh-cn/design/plugins.md) §12 与 `AGENTS.md`。

## 1. 背景与现状

### 1.1 当前结构

```
AgentHost (每进程一份)
 └─ agentxx_javascript_engine (C++ 插件实例, 每 host 一份)
     ├─ JSRuntime + 专用 JS 线程 (每 host 一条, 已经是进程级)
     └─ 脚本实例 = 一份脚本 × 一个挂载作用域 → 一个 JSContext
```

- 实例键: `脚本名@<agentId>#<mountSeq>`；
- 一个脚本挂在 N 个 agent 上，就有 N 个 JSContext，**顶层代码执行 N 次、全局变量各一份**；
- 桥函数靠 `JS_GetContextOpaque(ctx)` 定位实例，因此"一个 context = 一个作用域"是整套实现的隐含前提；
- 一个脚本没有"进程级只执行一次"的入口，也没有跨 agent 共享的状态。

### 1.2 由此产生的四个问题

1. **状态无法共享**: 连接、缓存、采集结果这类"应该只有一份"的东西，被复制 N 份，各 agent 之间要靠广播互通。
2. **没有进程级初始化**: C++ 插件的 `start` 是"一次"，JS 只有"每挂载一次"；想在进程启动时做一次准备工作（预热、读配置、建目录）没有落点。
3. **每 agent 一整套的开销**: 每个 JSContext 有自己的内建对象与全局表，典型 3 脚本 × 8 agent = 24 份。
4. **登记时机不确定**: 脚本实例是挂载时投递到 JS 线程建立的，而主程序初始化顺序是 `plugin_mount` → `engine_build`，脚本顶层登记的执行图节点类型有概率晚于首次建图。

### 1.3 不能改的部分（先说清楚）

- **JS 线程必须存在**: JS 不能跑在 io 线程上（没有让出机制，长脚本会卡住 io）。本次不改这一点，只把"每 agent 一套"改掉，线程仍是"每 host 一条"。
- **不做跨 host 共享**: 一个进程里多个 `AgentHost` 各自一份引擎实例，与"AgentHost 单例"的说法一致。跨 host 共享会让"脚本属于哪个 host 的插件"无从定义。

## 2. 目标模型

```
AgentHost (每进程一份)
 └─ agentxx_javascript_engine (C++ 插件实例)
     ├─ JsRuntime: 1 个 JSRuntime + 1 条 JS 线程 + 任务队列 (含中断预算)
     ├─ ScriptCtx: 每脚本一个 JSContext —— 全局状态就在这里, 跨 agent 共享
     │    ├─ 顶层代码执行一次 (进程级 API)
     │    └─ 生命周期入口: onMount(mount) / onUnmount(mount) / onUnload()
     └─ MountCtx: 每 (脚本 × 挂载) 一个 —— scope + mount 对象 + 该挂载的注册记账
```

对照 C++ 插件: `ScriptCtx` ↔ 插件实例 (进程级状态)、`MountCtx` ↔ 挂载记录 (每 agent 一份，带 scope)。C++ 里"一个实例 + N 个挂载"的结构，在 JS 里变成"一个 context + N 个 mount 对象"。

## 3. API 分三层

### 3.0 脚本自身的生命周期函数（不是 API 成员）

脚本顶层可以声明，三个都可选:

```js
function onMount(mount) {}     // 每次挂载调一次; 返回 Promise 时引擎等它 resolve
function onUnmount(mount) {}   // 卸载该挂载之前调一次
function onUnload() {}         // 脚本整个卸下之前调一次
```

因为**一个脚本 = 一个 JSContext**，不需要注册机制（`agentxx.onMount(fn)` 那种），引擎直接调用全局函数。
一个脚本两端共用时在 `onMount` 里按 `agentxx.side` 分支即可。

### 3.1 进程级 `agentxx.*`（顶层可用，不需要 scope）

| 分类 | 成员 |
|---|---|
| 日志与配置 | `log(level, msg)`、`getConfig()`、`getPluginArgs()`、`getPluginConfigPath()`、`getLanguage()`、`setLanguage(tag)` |
| 插件清单 | `listPlugins()`、`getPlugin(name)`、`getOwnInfo()` |
| 广播 | `declareBroadcast(decl)`、`publish(topic, value)`、`subscribe(topic, fn, opts?)`、`unsubscribe(token)`、`readBroadcast(id)`、`endBroadcast(id, reason?)` |
| 定时器 | `setTimeout(fn, ms)`、`clearTimeout(id)` |
| JSON | `jsonEscape(text)`、`jsonGetString(jsonText, key)` |
| 网络 | `net.*`（`netCall`/`netFetch`/`netWs*`/`netHttp*`/`netInfo`/`netCancel`），资源按脚本记账，一进程一份 |
| 查询 | `listCapabilities()`、`capabilityInfo(id)`、`invokeCapability(id, args?, opts?)`、`listMounts()`、`getMount(agentId?)` |

### 3.2 容器级 `agentxx.runtime.*`（看的是整个容器，不需要 scope）

`mountRoot(opts)`、`unmountRoot(rootId)`、`openChannel(opts)`、`closeChannel(id)`、`sendInput(id, text)`、`cancelChannel(id)`、`selectModel(id, name)`、`respondInterrupt(id, interruptId, values)`、`getChannelState(id)`、`getChannelMessages(id)`、`listRuntime()`、`registerTrigger(opts)`、`unregisterTrigger(id)`、`registerTimer(opts)`、`unregisterTimer(id)`、`emitEvent(topic, value)`

### 3.3 挂载级 `mount.*`（每个 agent 一个对象）

```
只读字段: mount.agentId / mount.mountSeq / mount.workDir / mount.language
工具:     registerTool(spec) / unregisterTool(name) / callTool(name, args)
权限:     registerToolPermission(spec) / unregisterToolPermission(name)
          registerReadPathPermission(tool, param) / registerWritePathPermission(tool, param)
          checkPaths(paths, scope?, sessionId?) / filterPaths(...)
资源:     addSkillDir / removeSkillDir / addMemoryFile / removeMemoryFile / addMcpServer / removeMcpServer
          getOwnResources()
提示词:   getPrompt() / setPrompt(objOrString) / getToolPrompt(toolName)
会话:     getSessionWorkDir(sessionId?) / isCanceled(sessionId?) / getMessages(sessionId)
          messagesCount(sessionId) / getShareStore(sessionId, offset?) / addShareStore(sessionId, text)
          emitMessageTip(sessionId, tip)
模型:     getModelConfig()
能力行:   provideCapability(spec) / providePoint(pointId, handler, opts?) / unprovideCapability(id, ownerTag?)
图:       registerNodeType(spec) / unregisterNodeType(type) / getGraphJson() / getGraphName() / setGraphJson(objOrString)
后台任务: submitAsyncWork(opts) / cancelAsyncWork(id) / getAsyncWork(id) / listAsyncWork(sessionId?)
订阅:     subscribe(topic, fn, opts?) —— 挂载级订阅, 卸载自动撤销, 回调带 scope
```

callTool 需要"在哪个 agent 上调"，因此放在 mount 上；`getOwnInfo()` 与挂载无关，留在进程级。

### 3.4 回调参数统一带 scope

| 回调 | 签名 |
|---|---|
| 工具 | `execute(args, ctx)`，`ctx = { sessionId, toolCallId, isCanceled(), scope: mount }` |
| 挂载级订阅 | `fn(payload, scope)` |
| 能力行 | `handler(args, scope)` |
| 功能埋点 | `handler(scope)` |
| 图节点 | `run(input)`，`input = { nodeName, type, config, state, threadId, scope }` |
| 挂载级定时器 | `fn(scope)` |
| client 侧各类回调 | 同上，`scope` 恒为 client 作用域对象 |

## 4. scope 传递机制（本次设计的关键）

### 4.1 不用"当前 scope"隐式状态

`mount` 是 JS 对象，C++ 侧对应 `MountCtx`（含 `PluginxxScopeRef`）。所有 `mount.*` 方法由同一个 bridge 入口分派，**scope 从 `this` 取**:

```cpp
// mountBridgeCall: 只处理 this 为 mount 对象的调用
int64_t    mountId = 0;
JSValue    idVal   = JS_GetPropertyStr(ctx, this_val, "__mountId"); // 不可写属性
if (JS_ToInt64(ctx, &mountId, idVal) != 0) { ... }
MountCtx*  mount   = engine->findMount(mountId);   // 已卸载 → 抛"挂载已卸载"
```

为什么这样够用:

- **没有隐式状态**：脚本把 `mount` 存进闭包、传给 `setTimeout`、在 Promise 续体里用，都指向同一个 scope，不会错位；
- **不需要包装回调**：C++ 侧投递任务时不再需要"设置/恢复当前 scope"，回调参数里带上 `mount` 即可；
- **失效可预期**：卸载后 `findMount` 返回空 → 抛明确错误，不会落到别的 agent 上。

### 4.2 桥入口拆成三个

| 入口 | 判据 | 覆盖 |
|---|---|---|
| `processBridgeCall` | `this` 是 `agentxx` 或全局函数 | 进程级成员 |
| `runtimeBridgeCall` | `this` 是 `agentxx.runtime` | 容器级成员 |
| `mountBridgeCall` | `this` 是 mount 对象 | 挂载级成员（agent 与 client 共用，client 的 scope 是固定 client 作用域） |

client 侧不再单独维护一套 `clientBridgeCall`：client 的 ui/session/wire/self/timer/keybind 成员挂到 client 的 mount 对象上，与 agent 侧同一套分派逻辑；差异只在"这个方法在当前作用域可不可用"。

### 4.3 与"插件 API 异步化"的关系

| 阶段 | 桥实现 |
|---|---|
| 前置（plugin-api-async 完成） | 所有重活表成员已是异步契约 → `mount.getMessages()` 直接返回 Promise，引擎只登记回调，**不需要任何 io→JS 同步桥** |
| 中间态（异步化未完成时） | 同步成员仍走现有 `ioCallSyncKeep`（含 150 ms 上限）。JS 重构可以先做，但会保留这段过渡代码 |

**建议顺序: 先做接口表异步化，再做本重构**，这样 mount bridge 的实现量少一半，也避免"先写同步桥再删"。

## 5. 生命周期契约

```
load(script)     → 建 ScriptCtx + JSContext → 执行顶层（此时只有进程级 API）
mount(agent)     → 建 MountCtx + mount 对象 → 调 onMount(mount) → **全部完成后才 ack 主程序**
unmount(agent)   → 调 onUnmount(mount) → 撤销该挂载的注册 → 销毁 MountCtx
unload(script)   → 逐个挂载走 onUnmount → 调 onUnload() → 销毁 ScriptCtx
disable/enable   → 与 load/unload 同一套（引擎实例 disable 时所有脚本一起卸）
```

### 5.1 mount ack 等待 onMount 完成（顺带解决登记时机）

- `onMount` 允许 async，引擎在它 resolve 后才调用 `notify->done(OK)`；
- 这一段**不会死锁**：mount ack 是异步的，io 线程没有被阻塞，`onMount` 里调 `mount.registerTool` 仍能被正常服务；
- 由于主程序的 `plugin_mount` 步骤要等所有 mount ack 才继续到 `engine_build`，**脚本在 `onMount` 里登记的执行图节点类型一定进入首次建图**（当前那条限制随之消失）；
- 超时: 单个 `onMount` 默认 5 s（可配 `script.mount_timeout_ms`），到点中断该脚本、按失败 ack，主程序回滚该插件的挂载并记日志；
- `onUnmount` 对称处理（默认 5 s），超时后强制销毁 MountCtx 并记日志。

### 5.2 错误与回滚

- `onMount` 抛异常 → 该挂载已完成的注册按记录回滚（沿用现有 `rollbackActions` 机制）→ ack FAILED；
- `onUnmount` 抛异常 → 记日志，继续撤销剩余注册（卸载不能被脚本阻断）；
- 脚本顶层抛异常 → load 失败（现有行为保持）。

## 6. 线程、中断与内存

- **一条 JS 线程串行执行所有脚本的所有回调**。这是共享线程的固有代价: 一个脚本死循环会拖住其它脚本。对策沿用并强化现有机制:
  - `JS_SetInterruptHandler` 已注册，现按 `kTaskTimeoutMs`(60 s) 打断单任务；本次把它明确成"每任务预算"，并在超时日志里写清"哪个脚本、哪个挂载、哪个回调"；
  - 超时只让**该次调用**失败（工具/能力行/图节点按失败收尾），不影响其它脚本继续。
- **内存**: JSContext 数从 `脚本数 × agent 数` 降到 `脚本数`。典型 3 脚本 × 8 agent: 24 → 3。
- **诊断**: 加载快照新增 `js_scripts` 段（每脚本: 名称、context 地址无关的序号、挂载列表、顶层耗时、最近一次回调耗时）；`KeyMetrics` 增加 `js.callback_timeout` 计数。

## 7. client 侧

- 同样只有一份脚本实例（client 没有多 agent），`onMount` 调一次，拿到 client 作用域对象；
- 该对象上挂 client 拥有的成员: `ui.*`（状态栏/面板/Info/命令/动作/渲染器/overlay/toast/装饰）、`session.*`、`wire.*`、`self.*`、`timer.*`、`keybind.*`，以及 `net.*`、广播、`getConfig`、`listPlugins`、`provideCapability` 等；
- 好处: **同一份 `plugin.js` 两端写法一致**，只需要按 `agentxx.side` 或 `typeof mount.registerTool === "function"` 判断能力。

## 8. 迁移清单

| 对象 | 改动 |
|---|---|
| `agentxx_javascript_engine.cpp` | 实例模型、三个桥入口、mount 表、生命周期、诊断 |
| `agent/plugins/example_js/plugin.js` | 按新 API 重写（双端骨架 + `onMount` 注册 + 进程级共享状态示例） |
| `agent/test/plugin/test_plugins.cpp` | JS 相关断言（约 60 处）：工具/能力行/图节点/订阅都改为"在 onMount 里注册" |
| `agent/test/plugin/test_client_plugins.cpp` | JS 段（约 40 处）：client 侧注册改到 onMount；同步回调限制相关断言删除或改写 |
| `docs/zh-cn/design/plugins.md` §12 | 整节重写 |
| `AGENTS.md` | "脚本 SDK 与 C++ 插件 SDK 的对齐面"一段重写为三层 API 说明 |
| `resource/history/js-runtime-singleton/work.md` | 实施记录 |

不兼容旧脚本（明确不做兼容层，留兼容层等于保留隐式 scope，等于没改）。

## 9. 分阶段实施

| 阶段 | 内容 | 验收 |
|---|---|---|
| A 结构 | `ScriptCtx` / `MountCtx` 拆分、实例键改为脚本名、单实例创建与销毁、诊断输出 | 引擎能编译通过；加载 1 个脚本在 2 个 agent 上只建 1 个 context（诊断断言） |
| B 桥 | 三个桥入口 + mount 对象 + 所有成员搬位；回调参数带 scope | `plugins` 里 JS 段改写后全绿 |
| C 生命周期 | `onMount`/`onUnmount`/`onUnload` + 异步 ack + 超时 + 回滚 | 新增用例：onMount 抛异常 → 挂载失败且注册回滚；图节点类型在首次建图前可见 |
| D client | client 侧并入同一套桥 + `example_js` client 段改写 | `client_plugins` 全绿 |
| E 收尾 | 删除 `ioCallSyncKeep` 与 150 ms 同步桥（若异步化已完成）、文档、全量回归 | 全量测试 + 文档更新 |

## 10. 风险与对策

| 风险 | 对策 |
|---|---|
| 单线程串行，一个脚本拖住所有 | 每任务预算 + 中断；诊断里点名脚本/挂载 |
| `onMount` 卡死导致挂载挂住 | 5 s 超时按失败 ack，主程序回滚并记日志 |
| 脚本把 `mount` 用错（A 的回调里用 B 的 mount）| 显式对象本就不会错位；若脚本自己跨挂载保存，落到的就是那个挂载（符合语义）。卸载后使用 → 明确报错 |
| 全局状态共享带来的耦合（一个脚本的全局变量被多个 agent 影响）| 这是本次要的能力；文档给出"按 `mount.agentId` 分桶"的写法 |
| 重构面大、旧插件不兼容 | 未发布；一次性切换 + 示例/测试同步改写；`plugins.md` §12 与 `AGENTS.md` 一并更新 |
| 与接口表异步化互相等待 | 先做异步化（阶段 A~C），再做本重构；中间态用现有同步桥过渡 |

## 11. 验收标准

1. 一份脚本挂 2 个 agent：进程级变量只初始化一次，两个 agent 的工具/能力行/图节点都注册成功，`mount.agentId` 各自正确。
2. `onMount` 里登记的图节点类型能被首次建图使用（不需要重建图）。
3. 卸载一个 agent：只有该挂载的注册被撤销，另一个 agent 的脚本状态不受影响。
4. `plugins` / `client_plugins` / `plugin_cleanup` / `boundaries` / `plugin_host_mount` / `plugin_multi_mount` 全绿。
5. 加载快照能看到每脚本的挂载列表与耗时。

## 12. 不做的事

- 不做跨 host 共享（每 `AgentHost` 一份运行时）。
- 不做"每 agent 一份全局状态"的开关（需要隔离就自己 `new Map()` 按 `mount.agentId` 分桶）。
- 不保留旧脚本 API 的兼容层。
- 不把 JS 线程合并到 io 线程（JS 没有让出机制，会卡住 io）。
