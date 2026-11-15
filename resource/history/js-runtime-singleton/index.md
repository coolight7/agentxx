# JS 运行时单例重构

## 目标

把 JS 插件的实例模型从"一份脚本 × 一个 agent 挂载 = 一个 JSContext"改成
**"一份脚本 = 一个 JSContext（进程级）+ 每挂载一个 mount 对象"**，
对齐 C++ 插件"一个实例 + N 个挂载"的结构：

- 全局状态可以跨 agent 共享（连接、缓存、采集结果不再复制 N 份）；
- 有真正的进程级初始化落点（脚本顶层，等价 C++ 插件的 `start`）；
- JSContext 数从 `脚本数 × agent 数` 降到 `脚本数`；
- 挂载 ack 等 `onMount` 完成，脚本登记的执行图节点类型一定进入首次建图。

## 方案

见 [plan.md](plan.md)。要点：

- 目标结构 `JsRuntime`（1 线程）/ `ScriptCtx`（每脚本一个 JSContext）/ `MountCtx`
  （每 脚本×挂载 一个）；
- API 三层：进程级 `agentxx.*`、容器级 `agentxx.runtime.*`、挂载级 `mount.*`；
- **scope 通过 `this` → `__mountId` 取**，不用"当前 scope"隐式状态，因此回调不需要
  包装、Promise 续体不会错位；
- 三个桥入口 `processBridgeCall` / `runtimeBridgeCall` / `mountBridgeCall`，
  client 侧并入 mount 桥（不再单独维护 `clientBridgeCall`）；
- 脚本顶层声明 `onMount(mount)` / `onUnmount(mount)` / `onUnload()`，`onMount` 允许
  async，引擎等它 resolve 后才 ack。

## 范围

做:
- `agentxx_javascript_engine.cpp`：实例模型、三个桥入口、mount 对象、生命周期、诊断；
- `agent/plugins/example_js/plugin.js`：按新 API 重写（双端骨架 + `onMount` 注册 +
  跨 agent 共享状态示例）；
- `agent/test/plugin/test_plugins.cpp`（JS 段约 60 处）与 `test_client_plugins.cpp`
  （JS 段约 40 处）；
- `docs/zh-cn/design/plugins.md` §12 整节重写、`AGENTS.md` 的 JS 段重写；
- 加载快照新增 `js_scripts` 段，`KeyMetrics` 增加 `js.callback_timeout`。

不做:
- 不做跨 host 共享（每 `AgentHost` 一份运行时）；
- 不把 JS 线程合并到 io 线程（JS 没有让出机制）；
- 不保留旧脚本 API 的兼容层（留兼容层等于保留隐式 scope，等于没改）；
- 不做"每 agent 一份全局状态"的开关（需要隔离就自己按 `mount.agentId` 分桶）。

## 状态

规划中（2026-11）。方案已写入 [plan.md](plan.md)，实施未开始。

前置: [plugin-api-async](../plugin-api-async/index.md)（先做接口表异步化，
则 mount 桥不需要 io→JS 同步桥，实现量少一半）。**若前置未做也可以开工**，
中间态保留现有 `ioCallSyncKeep` 过渡，阶段 E 再删。

## 关键决策

1. **JS 线程保留**：JS 不能跑在 io 线程上，本次只改"每 agent 一套"，线程仍是每 host 一条。
2. **scope 显式化**：不给回调包实例标记，而是把 scope 做成 `mount` 对象，
   桥按 `this` 反查 —— 无隐式状态、无需包装、卸载后使用报明确错误。
3. **`onMount` 允许 async 并阻塞 ack**：mount ack 是异步通知，io 线程没被阻塞，
   不会死锁；顺带解决"脚本登记的图节点类型可能晚于首次建图"的老限制。
4. **client 侧也有 `onMount`**：client 只有一份实例，调一次，拿到 client 作用域对象，
   同一份 `plugin.js` 两端写法一致。
5. **不兼容旧脚本**：未发布，一次性切换，示例与测试同步改写。

## 风险

- 单线程串行，一个脚本死循环拖住所有脚本：沿用 `JS_SetInterruptHandler` 的每任务预算，
  超时日志点名"哪个脚本 / 哪个挂载 / 哪个回调"。
- `onMount` 卡死导致挂载挂住：默认 5 s 超时按失败 ack，主程序回滚并记日志。
- 重构面大：分 5 阶段（结构 / 桥 / 生命周期 / client / 收尾），每阶段可编译可测。

## 结论

待实施后补充。
