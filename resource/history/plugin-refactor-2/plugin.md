# 插件框架架构审查与重构方案

> **文档状态：审查最终报告；重构尚未实施。**
>
> 本文由 Agentxx 根据当前源码、现有插件、测试及独立最小复现整理；只分析与提出方案，未修改产品实现或现有测试。
>
> - 范围：`agent/lib/include/agentxx/plugin/`、`agent/lib/src/plugins/`、`agent/plugins/`，以及 `agent/test/plugin/` 和相关 core/client 测试；补查 AgentContext 销毁、BaseAgent 装配、TUI 渲染调用链。
> - 验证平台：Linux x86_64 / GCC 16.1 / 现有 Debug + AddressSanitizer 构建。未声称 Windows、Android、不同 STL 组合已验证。
> - 下文路径均相对仓库根目录；行号以以上源码版本为准。测试使用现有 Debug 产物，独立探针优先包含源码头；未全量重建，不把已有产物通过当作全部当前源码的构建证明。

## 1. 核心结论

**方向正确，但当前实现不能认定为已满足全部设计要求，也不宜仅依据现有测试通过就认定卸载和异步边界安全。**

值得保留的基础设计是：纯 C 数据边界、`alloc/free/query_interface` 最小宿主表、按 IID 查询扩展表、实例上下文隔离、工具/钩子/能力的 `start + cancel + done` 协议、宿主 Asio 协程与插件自有 C++ coroutine 的回调桥接、UI 语义 JSON 与具体组件分离。

问题集中在协议实现一致性，而不是需要推翻 C ABI：

1. **生命周期保护不完整**：业务上下文、DSO 代码、调用方完成回调、排队的调度任务、旧 UI 快照及已编译图节点，没有统一的存活与关闭规则。
2. **异步协议存在互斥条件错误**：启动失败既返回空句柄又调用完成回调，SDK 会重复释放回调上下文。
3. **线程边界不一致**：部分宿主异步互调只把查询投递到 IO 线程，真正 `start` 仍在调用线程执行；完成通知先发布终态再写结果。
4. **“无轮询”只对部分路径成立**：普通操作等待确实是协程事件等待；卸载计数仍指数退避轮询；JS Promise 驱动仍含 1ms 轮询与递归执行队列。
5. **生产插件并未普遍使用同线程原生异步**：HTTP、命令、RAG、系统采样大量采用 `blocking_tool → 私有 io_context.run()`，每个等待占据 worker。
6. **多实例约束没有贯彻到底层**：系统监控的 GPU/PDH 可变静态缓存、文本选择监控的全局实例指针依然存在。
7. **SDK 不足以支撑自然的协程拆分**：`Task<T>` 不能 `co_await` 子 Task，`offload` 返回 void 编译失败，hook helper 会忽略协程返回值。

两轮合计通过独立 ASan 探针确认 **9 类 use-after-free 触发路径**，另确认 JS 跨脚本循环等待、协程 hook 被忽略、Client 退订后仍派发、提示词撤销串扰等问题，见 §4。这里的“9 类”按触发路径归类，不表示九个互不相关的根因。

本轮重跑现有四个框架模块 **740 条断言通过**，九个相关业务模块 **440 条断言通过**，合计 **1180 passed / 0 failed**。二者不矛盾：现有测试多数覆盖正常调用、直接实现或受限环境，未覆盖此次复现的交叉路径。

**建议先固定异步协议和退出安全条件，再统一运行时，最后迁移插件及整理 SDK；不要先把文件拆小、把 mutex 删除，或只在几个裸指针上补 shared_ptr。**

## 2. 当前架构

### 2.1 分层与职责

| 层 | 主要文件 | 实际职责 |
|---|---|---|
| C ABI | `plugin/api/plugin_api.h`、`client_plugin_api.h` | 基础类型、借用/拥有字符串、接口表、回调、opaque handle、入口 typedef |
| 插件 C++ SDK | `plugin/api/plugin_kit.h`（3511 行）、`plugin_guard.h` | 字符串封装、接口查询、Task/awaiter、取消注册表、工具/钩子/能力注册、UI helper、导出宏 |
| 宿主公共设施 | `plugin_common.*`、`plugin_manager_base.h` | manifest、路径、依赖排序、跨线程同步桥、inflight、卸载等待 |
| Agent 管理器 | `plugin_manager_lifecycle.cpp`、`*_vtable.cpp`、`*_adapters.cpp`、`*_capability.cpp`、`*_scheduler.cpp`、`*_tasks.cpp` | 动态库/内置加载、注册、互调、资源与提示词、调度、卸载 |
| 操作驱动 | `op_driver.h` | 调用插件 start、等待 done、取消/超时后的后台等待、句柄回收 |
| 引擎适配 | `PluginTool`、`PluginMiddlewareHandle`、`PluginGraphNode`、`ToolRegistry` | 插件 C 回调适配为宿主工具/中间件/图节点 |
| Client 管理器 | `client_plugin_manager.*`（cpp 3274 行） | UI 注册快照、命令、事件、动作、overlay、插件加载卸载、全部 client vtable |
| 插件业务 | `agent/plugins/`（20 个目录） | 文件/命令/网络/数学/RAG/监控/规划/图节点/JS 等 |

### 2.2 ABI 与扩展机制

- 核心宿主表是 `alloc(uint64_t) / free(void*) / query_interface(host*, iid*)`，Agent/Client 共用 `AgentxxPluginHost`。
- Agent 有 16 张标准扩展表，Client 有 7 张。结构体参数通过指针传递；字符串为 `{data, uint64_t size}`。
- 跨边界没有直接传 `std::string`、`std::function`、Asio executor 或 C++ coroutine handle；SDK 的 C++ 类型只在各自模块内部使用，这一点正确。
- opaque `void*` 可以指向插件私有 C++ 对象，只要宿主不解释对象布局、不析构它、只原样回传；这不等于把 C++ 对象按值暴露到 ABI。宿主与插件之间的隔离是 **ABI/所有权隔离，不是进程内安全沙箱**。
- `#pragma pack(push, 8)` 是最大成员对齐约束，并不等于对所有目标平台承诺所有结构都具有 8 字节自然对齐；兼容性应按目标架构验证 `sizeof/alignof/offsetof`。
- 当前接口表仅有 `version`，查询 helper 未验证版本，表没有长度字段；尚缺完整演进协议。
- 这是 **COM 风格接口发现**，不是完整 Windows COM/IUnknown 对象模型；目前无需为追求名称一致而引入通用 AddRef/Release 对象体系。

### 2.3 普通异步工具链

```text
Toolcall / PluginTool::execute_async
  → awaitPluginOp
  → 插件 execute_start(user_data, args*, session*, call_id*, notify*, error*)
      ├─ fast_tool：同步计算，done，返回 nullptr
      ├─ tool：创建 Task 帧，resume，到 co_await 时挂起，返回 Job*
      └─ blocking_tool：保存输入，scheduler.offload，返回 Job*
  → 宿主 concurrent_channel.async_receive 挂起
  → 插件完成后 notify.done
  → 宿主解析状态并恢复等待协程
```

`while (!notified) co_await channel.receive()` 本身不是定时轮询；不能仅看见 while 就判定是假异步。正常的 sleep awaiter 也是定时器到期回调恢复，而不是周期查结果。

但“真实协程”和“正确协程”是两件事：任务取消、失败、卸载、任意线程 done 的实现仍有错误。

### 2.4 生命周期与 UI

- Agent 将实例放入 `plugins_` 后调用 create；工具注册后即能被查询；JS 壳在脚本未加载完成时就返回 create 成功。
- unload：设置 `unloadRequested` → 摘注册/取消部分操作 → 轮询 inflight → destroy → 从表移除；DSO 在实例最终析构时关闭。
- shutdown：不等待 inflight，直接 destroy；`AgentContext::~AgentContext()` 会走此路径，并非仅操作系统退出才调用。
- Client 的 dlopen/create 在单线程 pool 中运行，普通事件/命令在 client IO 线程；UI 数据采用 COW 快照。
- **工具 renderer 是例外**：快照包含插件函数指针和裸 userData，TUI 渲染直接调用，没有保护实例或参与 inflight。

## 3. 逐项对照设计要求

| 预期 | 当前判断 | 依据 |
|---|---|---|
| C API，基础变量/C 结构体指针/opaque handle 隔离 | 基本符合 | 两个 ABI 头 C17 编译通过；未发现公开表直接暴露 STL/Asio；Linux 16 个现有 DSO 导出表符合入口限制 |
| COM 风格查询，接口独立演进 | 发现机制符合，兼容策略不完整 | IID 查询存在；version 不检查、无长度、Agent manifest require 未执行检查 |
| 异常不能穿越边界 | 不完全符合 | SDK create 分配/init 在 try 外；部分 vtable、renderer 后处理与 agent get_info/destroy 缺完整守卫 |
| 默认同一线程、无业务锁、协程交错 | 部分符合 | tool/sleep 主路径符合；worker 互调 start 错位；内部多重 mutex/atomic 与私有 loop 普遍存在 |
| 非阻塞 | 不完全符合 | RAG create 同步网络；system_usage capability 直接 run 私有 loop；destroy join；pool 被长期等待占据 |
| 等待异步结果不轮询 | 不完全符合 | 普通 op 等待是事件式；waitInflightZero 与 JS drivePromise 仍轮询 |
| 真协程，便于函数组合 | 仅基础能力具备 | Task 无子任务 await 支持；void offload 编译错误；hook helper 不处理 Task 返回值 |
| 多实例隔离 | 上层较好，底层不符合 | GPU/PDH 可变 static、文本选择 instancePtr，详见 §5 |
| 安全卸载/失败回滚 | 不符合 | 9 类 ASan 触发路径，以及调用方无保护、排队任务不计数等 |

“无锁”应准确限定为 **宿主会话及默认插件状态由单 IO 线程串行管理，无需业务锁**，而不是要求 Asio 内部、操作系统、显式 worker、UI 跨线程快照绝对没有同步原语。不能为了删除锁而让真实跨线程访问发生数据竞争。

## 4. 已验证的问题与修复方向

证据等级：**R**＝本次运行/编译复现；**S**＝源码能确定的缺陷；**V**＝还需特定环境/压力测试验证具体表现。优先级 P0 为内存安全/死锁/核心协议，P1 为主要功能及设计约束，P2 为整理优化。

### F01 · P0 · 互调启动失败：回调与空返回同时发生，SDK 重复释放（R）

- 宿主：`plugin_manager_capability.cpp:371–384, 473–486`。
- SDK：`plugin_kit.h:1948–1951, 1976–1982`，capability 同构路径 `2071–2074, 2099–2105`。
- 被调用方按契约返回 `nullptr + error_out` 后，宿主同步执行 cb，再返回 nullptr。cb 先删除 `holder`；awaiter 在发现 nullptr 时再次删除同一对象。
- **无需恶意插件**，正常拒绝启动即可触发。独立复现触发 ASan heap-use-after-free，栈定位到 `plugin_kit.h:1977`。
- 失败前已插入的 outstandingOps 没有回滚，也形成残留。

**修复**：把“未接受请求”和“已接受后失败”严格分开：返回失败/空句柄时绝不派发 cb；接受请求后只能通过恰好一次、IO 线程异步派发的 cb 完成。这里指 **宿主的 call_tool_async / invoke_capability_async 返回协议**；不能误删 provider `execute_start` 既有的 `done + nullptr` 快同步完成形态。所有失败出口回滚登记。对 tools/capabilities 共用一个实现，避免两份错误保持同步。

### F02 · P0 · ABI unsubscribe 在释放订阅后写裸指针（R）

- `plugin_manager_vtable.cpp:164–179`，`plugin_manager_adapters.cpp:575–593`。
- manager unsubscribe 从 EventBus 和实例 vector 移除最后引用后，vtable 仍执行 `sub->inst = nullptr`。
- 通过真实 events 接口 `subscribe → unsubscribe` 即出现 ASan 已释放内存写入，定位 `vtable.cpp:178`。
- 现有测试多直接调用 manager unsubscribe，绕开了有问题的 vtable 尾部。

**修复**：关闭标记在释放前处理；整个反注册操作持强引用；明确句柄注销后的有效期。若要支持 destroy 中重复注销，使用宿主句柄表或存活至实例退出的轻量失效记录，不能凭悬空指针判空实现幂等。

### F03 · P0 · 旧 UI 快照仍可调用已销毁 renderer（R）

- `client_plugin_manager.h:85–116`，`client_plugin_manager.cpp:3132–3220`。
- TUI 调用：`agent/client/src/io/tui/components/message_list.cpp:768,1515`。
- 快照只有函数指针/userData；unload 摘除新快照不影响旧快照，但随后 destroy 会删除 shim。
- 复现：加载 filesystem client → 持有快照 → unload → 通过旧快照 renderClientTool，ASan 在 `plugin_kit.h:3140` 报 UAF。
- 测试仍持实例引用也不能保护 pluginCtx：**宿主实例存活不等于业务上下文未销毁**。

**修复首选**：UI 只读语义数据，不执行插件代码。renderer 在 client IO 线程按输入版本计算，输出宿主持有的不可变渲染模型；UI 发起请求后使用缓存/默认显示，不同步等待 IO。模板型 renderer 可继续由宿主直接计算。仅给快照补 DSO shared_ptr 不够，因为 ctx 已被 destroy。

### F04 · P0 · 已编译图节点在 unload 后访问已销毁上下文（R）

- `plugin_graph_node.cpp:14–41,65`；`plugin_manager_lifecycle.cpp:333–382`。
- 节点持有 PluginInstance 强引用，但 unload 明确调用 destroy，且未将 enabled 置 false；node.run 只检查 enabled。
- 构造节点 → unload → run，ASan 确认插件 user_data UAF。
- `unregisterGraphNodeType` 只删除实例记录，GraphRegistry 类型还在，重载同类型会冲突；detachAll 本身也没有完整撤销图类型。

**修复**：引入明确生命周期状态；所有调用入口拒绝 Closing/Closed，不以 enabled 代替存活。为已编译图定义策略：默认停止新运行，等待已有运行结束，再销毁上下文；旧节点只返回“插件已卸载”。真正删除工厂或使用带代次的间接注册项，不能以“引擎不会再编译”解释残留无害。

### F05 · P0 · shutdownAll 不等待后台 Task 退出便销毁 ctx（R）

- `plugin_manager_lifecycle.cpp:131–180`；`agent/lib/src/agent/context.cpp:30–35`。
- `detachAll` 的取消会排队恢复挂起协程，紧接着 shutdown destroy ctx；协程恢复时引用已经失效。
- 独立复现：spawn 挂起 sleep → shutdownAll → IO 继续处理取消回调，ASan 确认 UAF。

**修复**：增加真正异步的 shutdown，AgentHost/FFI/客户端退出顺序必须在 executor 和 pool 停止前 await。destroy 只释放已停止的实例，不承担 join/异步停止。析构函数不能替代异步 shutdown；兜底宁可隔离保留未停止的模块，也不能强制 dlclose。

### F06 · P0 · worker 发起互调时 execute_start 不在 IO 线程（R）

- `plugin_manager_vtable.cpp:84–103,276–299`。
- `plugin_manager_capability.cpp:287–312,332–363,448–465`。
- 只把 registry 查询通过 ioCallSync 回 IO；后续实例检查、outstandingOps 写入和 drive.start 仍在调用线程。
- 复现输出 `execute_start_off_io=1`。JS 的 `call_tool_blocking` 正好使用这条路径，并非不可达。

**修复**：在 ABI 接收入口复制借用数据，整个 start/cancel/登记流程投递到所属 IO executor；异步入口不应先阻塞 worker 等查表。回调始终异步 IO 派发，SDK 才能用简单同线程状态机。

### F07 · P0 · 跨插件互调只保护 provider，不保护 caller（R）

- `plugin_manager_capability.cpp:332–341,448–459`；`op_driver.h:122–140`。
- caller 保存 outstandingOps 不等于 caller 有 inflight；done 回调函数属于 caller DSO，userData 也属于 caller。
- 复现：A 调 B 未完成操作，输出 `caller_inflight=0 provider_inflight=1`；unload A 已成功返回，之后 B done 仍派发 A 的回调。

**修复**：一次跨插件操作至少保护两个角色：provider 业务执行，caller 完成回调。caller 的保护一直持有到完成回调返回；provider 的保护覆盖其所有实际执行及退出协议。持有 caller_host 的第三方引擎也必须有对应租约/托管任务，不能只借用一个裸 host 指针。

### F08 · P1 · sleep 完成记录不回收，且独立 sleep/post 未完整登记（R/S）

- `plugin_manager_scheduler.cpp:22–51`：sleepTimers 插入后，正常到期无 erase。
- 复现 32 个一次性定时器后：`callbacks=32 retained_timers=32 inflight=0`。
- sleep 只在回调开始临时加 inflight；排队期间没有计数。`xx_post_to_io` 也只保存 fn/ud，没有持实例执行租约。

**修复**：timer/post 从接受到回调返回都由运行时登记；定时器完成时移除记录，取消与到期竞争统一终结一次。生命周期保护必须覆盖队列等待阶段，不能等到已开始执行才加计数。

### F09 · P1 · 三级依赖禁用不完整，恢复不对称（R）

- `plugin_manager_lifecycle.cpp:252–322`。
- A 被 B 依赖、B 被 C 依赖，disable A 得到 `A=0 B=0 C=1`；enable A 后 `A=1 B=0 C=1`。
- 订阅在 detachAll 中 clear，enable 只恢复 tools/hooks/capabilities，订阅和 spawn 不恢复，提示词恢复策略也不对称。

**修复**：明确“暂停”和“停止”区别；依赖禁用按完整图递归，维护用户显式禁用与依赖不可用两类原因。订阅记录保留、暂停派发；后台任务是否重启必须由 on_enable/on_disable 或生命周期接口定义，而不能尝试恢复已销毁的 coroutine frame。

### F10 · P1 · 静态工具冲突注册失败却返回成功（R）

- `plugin_manager_adapters.cpp:262–282`；`tool_registry.cpp:10–30,51–53`。
- contains 只看动态工具；registerTool 内部会检查 staticToolNames，但 manager 忽略它的返回值，仍登记 toolNames/tools 并返回 0。
- 复现：`register_rc=0 registry_contains=0 recorded_tools=1`。

**修复**：register 返回值是唯一成功依据；失败不修改实例记录；注销用 owner/注册凭证而非仅 name。SDK 注册 helper 应返回 `[[nodiscard]]` 结果，必要注册失败使初始化事务整体失败。

### F11 · P1 · Task 无法组合，void offload 编译失败（R）

- `plugin_kit.h:1292–1375`：没有 `operator co_await` 或 awaiter 接口。
- `plugin_kit.h:1775–1783`：始终声明 `optional<ResultType>`，ResultType=void 非法。
- 编译探针：`co_await child()` 报 `no member named await_ready`；`co_await offload(ctx, [](volatile int32_t*){})` 因 `optional<void>` 失败。

**修复**：区分 root operation 与普通子 Task；Task 支持 continuation、结果移动与异常传递，只有根任务对宿主 done；void 使用专门结果存储。hook/capability/graph 也复用同一 Task 适配器，不再各自造一套 Job。

### F12 · P1 · screen_capture 当前入口无法编译（R）

- `agent/plugins/agentxx_screen_capture/agentxx_screen_capture.cpp:29,91,118,143,224–246`。
- `ScreenCaptureScreenCapturePluginCtx` 未定义，辅助函数仍使用不存在的 PluginCtx；入口调用 captureAll/captureScreenUnderMouse/formatFramesJson 等与 `screen_capture.h` 不符，且把返回 ScreenFrame 当 optional 使用。
- Linux 对该入口做语法检查已确认非平台相关的类型错误。默认 Linux 构建跳过 Windows 插件，因而没有暴露这些错误。

**修复**：先统一上下文与真实 API 名称，补 Windows 编译任务和插件级 smoke test；不以删除 gate 或构造空实现来“让测试通过”。

### F13 · P0 · SDK 协程参数仅借用，真实工具互调挂起后读到已释放输入（R）

- SDK：`plugin_kit.h:2405–2410` 将 ABI 的 `args_json` 直接转 `std::string_view` 交给 Task，没有保存请求副本。
- ABI 已声明入参“仅本次 start 调用有效”。协程参数为 string_view 按值，只复制地址和长度，并不延长底层字符串寿命。
- `callToolAsync` 的 `drive.start` 捕获 argsStr；返回后 drive 仅有 cancel 闭包/等待协程暂时保活。provider 挂起后，等待协程的 drive 生命周期并不是面向插件的输入保活契约；本轮真实 `SDK caller → call_tool → SDK provider → sleep → 读 args` 已触发 ASan UAF。
- 另有纯 ABI 探针：start 返回后释放调用方输入，再恢复 Task，同样 UAF。正常 `PluginTool::execute_async` 路径碰巧持有 argsJson 更久，不能据此认为 SDK 输入安全。

**修复**：root operation 在调用业务 Task 前建立拥有 args/session/callId 的 Request；直到帧与清理回调完成后才释放。便捷 SDK 默认给开发者安全视图，C ABI 用户仍遵守“挂起前复制”的原始契约。增加真实互调回归，不仅做手写 notifier 测试。

### F14 · P0 · Task 完成与句柄清理之间，detachAll 仍调用已释放的 cancel_ud（R）

- `plugin_manager_tasks.cpp:98–110` 的 `cancelFn` 没有捕获/检查 OpCore 完成态；`spawnHandleReaper` 要到后续 IO 执行才移除句柄。
- 探针通过 tasks ABI 注册 → `notify.done` → 释放任务私有 cancel_ud → **宿主直接 detachAll**。输出 `after done inflight=0 handles=1`，随后 ASan 报 cancel 回调读已释放对象。
- 不需要插件在完成后主动使用失效句柄；触发者是宿主卸载清理。完成协议允许任务在 done 后释放资源，宿主就不能继续把它当可取消任务。

**修复**：完成提交时先让取消入口失效，再通知等待者/降低业务计数；句柄登记和操作终态合并在一处。已完成 ID 的 cancel 只返回 already-completed，不再调用插件代码。

### F15 · P0 · 管理器排队 lambda 捕获裸 this，管理器销毁后仍执行（R）

- `plugin_manager_base.h:177–218` 的 postToIo/postToIoAsync 入队 `[this]`，后续写 ioThreadId_ 并运行私有 ioTasks_。
- 探针：构造 ClientPluginManager → postToIoAsync(noop) → 释放 manager → IO 继续运行。ASan 定位 `plugin_manager_base.h:203`。
- 即使业务 lambda 不引用插件、不操作任何状态，也会发生。因此只保护插件实例不能解决运行时自身的生命周期。

**修复**：去掉额外的 ioTasks_ 队列，把已拥有输入和运行时引用的闭包直接 post 到 executor；提交成功后，运行时至少存活到闭包执行/安全丢弃。弱引用可用于“允许丢弃”的宿主纯数据消息，不能静默丢弃需要完成通知的 coroutine resume。

### F16 · P0 · JS 顶层初始化失败后遗留已注册工具，调用即 UAF（R）

- `agentxx_javascript_engine.cpp:463–511, 927–933`。
- 脚本先 `agentxx.registerTool(...)`，再 `throw Error(...)`；doLoadScript 返回失败、局部 JsPluginCtx 及 binding 已释放，宿主注册仍在。
- 探针输出 `script_load_status=2`、`script_tool_retained=1`，调用残留工具时 ASan 在 `JsEngine::toolExecuteStart:933` 报 UAF。
- timer/事件/hook 在失败期间同样需要撤销，不仅是工具；C++ 壳当前不等待 load 的完成结果，扩大了问题暴露范围。

**修复**：脚本初始化使用宿主注册事务；失败时先停止、撤销全部注册与定时器，再释放 JSContext/binding，最后返回失败。不要仅在调用入口捕获错误：访问已释放 binding 时已经来不及。

### F17 · P0 · JS A 调用 JS B 在同一引擎内形成循环等待（R/S）

- `agentxx_javascript_engine.cpp:1224–1283` 只对 **当前脚本自己的 tools** 内联；不同脚本落到 call_tool_blocking。
- A 所在线程阻塞等待 B；B 的 execute 又排队到同一 JS 线程，永远无法执行。QuickJS interruptHandler 不能中断这里的 C++ condition_variable 等待。
- 本轮两份极短脚本均加载成功；A 调 B 时 12 秒独立进程截止退出 124，未得到结果。结合上述调用链可确认循环等待，而不是把任意测试超时直接归为死锁。

**修复**：callTool 始终创建 JS Promise 并立即返回；完成事件投递回 JS 线程 settle Promise。即便是同脚本，也复用异步路径，避免特殊内联路径产生不同异常、取消和 callId 语义。此变更涉及脚本 API 行为，提供明确的新 API 版本并迁移脚本为 `await agentxx.callTool(...)`。

### F18 · P1 · Client 当前轮派发没有复查 alive，退订不能阻止后续回调（R）

- `client_plugin_manager.cpp:1255–1296, 2725–2744`：构造快照时检查 alive，逐项派发时不再检查。
- 两个 READY handler，先执行的 handler 通过 ABI 退订后一个；本轮仍输出 `second_called_after_unsubscribe=1`。
- shared_ptr 仅保护 Subscription 对象，不保护开发者在退订后释放的 user_data。具体 UAF 取决于用户释放方式；本轮只运行了不释放 user_data 的功能探针，不把它计入 9 类 ASan。
- dispatchEvent 快照还保存裸 inst，缺 closing/generation 复查；UI action 仅携带名字、不带点击时的注册代次，重绑或同名重载可能把旧点击交给新实例。

**修复**：每个回调执行前在 IO 重查存活、订阅代次、实例状态；派发持实例强引用与执行保护。取消订阅后不再开始新的回调，已开始回调允许结束。动作请求携带实例/绑定代次，而不是比较“当前快照”和“当前注册”后假定等于点击时状态。

### F19 · P1 · hook 接受协程签名，却完全不执行协程（R）

- `plugin_kit.h:2786–2848` 的 hook 只调用 fn 并忽略返回值。
- 返回 `Task<void>` 可以编译；Task 初始挂起，临时 Task 随即析构，函数体未运行，宿主却收到成功 done。
- 本轮重跑探针：`coroutine_hook_entered=0 done_notifications=1`。

**修复**：新 SDK 统一识别同步 `void` 与异步 `Task<void>`，使用同一个 root adapter；迁移前对旧 hook 增加严格返回类型约束，宁可编译时报错，不能静默成功。

### F20 · P1 · 提示词“备份—恢复”在多插件叠加时撤销错误（R）

- `plugin_manager_vtable.cpp:1650–1749` 按每个实例保存修改前的值，卸载时无条件写回。
- A 写 shared=A，B 再写 shared=B；禁用 A 后 shared 被删除，B 的有效贡献消失；再禁用 B，shared 又恢复为已禁用 A 的值。
- 本轮输出：`after_disable_A_shared=<missing>`、`after_disable_B_shared=A`。
- setPromptJson 还会在仅修改 append key 时备份整个 systemPrompt；用户或其他插件在其后改 systemPrompt，恢复同样可能覆盖新值。

**修复**：把基础用户配置与插件贡献分离，按 `(owner, key, revision)` 保存声明，以明确优先级合成有效提示词。卸载只删除该 owner 的贡献，不写回旧快照。图定义修改也需要事务/版本规则，不能全局覆盖后在失败路径遗留副作用。

### F21 · P1 · JS Promise rejection 被当作工具成功结果（R）

- `agentxx_javascript_engine.cpp:691–750` 将 rejected Promise 转成普通 JS string；pending job 错误、等待超时也返回字符串。
- doToolExecute 只对 JS_IsException(result) 标记 error；因此 `Promise.reject("expected rejection")` 本轮正常返回 `result=expected rejection`，没有操作失败。
- `120000` 是循环次数，不是精确的 120 秒截止时间；任务递归和 long timer 都能改变实际等待长度。

**修复**：Promise 状态映射到操作状态，rejected→FAILED，明确取消→CANCELLED；timeout 用单调时钟截止 timer。JSValue/异常值采用作用域所有权封装，包含 PromiseResult、新建 timer fn 及枚举 atom，防止新增分支泄漏。

## 5. 源码分析、边界约束与尚未运行的专项验证

### 5.1 P0：OpCore 完成发布与回收竞态（S，压力表现待 V）

`op_driver.h:95–142` 先 CAS `notified=true`，再写 status/payload，之后 send channel / emit doneSignal / reset guard。

- IO 等待者可能在 status/payload 尚未写完时看见 notified；release/acquire 不能发布发生在 release 之后的写入。
- `doneSignal.emit()` 来自任意线程，而 reaper 在 IO 线程绑定 slot；单个 Asio cancellation_signal 并不自动提供这类共享访问同步。
- 检查 notified 与绑定 slot 非原子事件序列，可能错过通知。
- 等待者会 move payload，onDone 又为 cb 复制 payload，相关顺序需要统一。
- registerTask 的 cancelFn 不看 core 完成态，done 后、reaper 尚未移除句柄的窗口调用已失效 cancel_ud；该子问题已单独复现，见 F14。
- `notified` CAS 只在 OpCore 仍存活时防止重复处理；对已经释放的裸 host_ud 再次 done，shared_from_this/bad_weak_ptr catch 并不能使解引用悬空对象变安全。不能把该 CAS 描述成可以容忍任意晚到的违约回调。

**修复**：分离“完成被认领”与“结果已就绪”。任意线程 done 只负责复制借用载荷并投递完成包，状态提交、取消失效、结果可见、回调派发及所有计数更新均在 IO 线程进行。完成前取消与完成后取消在同一状态机定义，移除单独 reaper/sentinel 的重复通知通道。

### 5.2 P0：volatile 不是跨线程同步（S）

`plugin_kit.h:1781,1794,2597,2769` 的 cancelFlag 在 IO 写、worker 读，是普通 volatile int32_t，按 C++ 内存模型构成数据竞争。

**修复**：不要直接把 `std::atomic*` 改成跨语言 ABI。推荐 opaque cancel token + 宿主 `is_cancelled(token*)` 基础类型返回，插件 SDK 内部包装；插件自己拥有、自己访问的 atomic 可作为私有状态。事件通知驱动阻塞 IO 中断，状态查询仅作为计算循环合作取消检查。

### 5.3 P0：SDK 导出与回调异常守卫不完整（S）

- 导出宏 `plugin_kit.h:3448–3450,3487–3489` 的构造/init 在 try 之外。
- coroutine tool / blocking_tool 的参数复制、分配、Job 建立不在完整 C 入口守卫内。
- `xx_call_tool_async`、`xx_log`、`xx_json_escape`、OpCore onDone 等仍有可抛分配操作未完整隔离。
- renderer 的 JSON dump 在业务 fn 的 catch 之外，且宿主只在 rc==0 时释放输出；错误返回/部分输出有泄漏风险。
- 手写 create 常把 unique_ptr 建在 guard 内层 lambda，却在外层 catch logger 中使用 raw 指针；异常展开后 raw 可能已悬空。
- 导出宏及部分手写入口缺 `AGENTXX_PLUGIN_CALL`，一些入口还用 int；在当前 x64 平台往往 ABI 等价，但没有落实声明的严格规约。

**修复**：统一 `noexcept` C trampoline，最外层包住构造/参数复制/结果编码/清理；错误日志不用可能已析构的 ctx；失败上报不能再次依赖可能抛出的格式化。宿主兜底 catch 不能替代插件侧隔离，跨编译器异常不应先越界再期望 catch。

### 5.4 P0/P1：初始化、失败回滚与重复加载没有事务闭环（S）

- Agent `plugins_[name]=inst` 会覆盖同名实例，没有 Client 的重复检查；工具与能力可能仍归旧实例。
- create 失败 detach 后立即 dlclose，没有 await 尚在启动/取消中的任务，也没有统一释放非空 pluginCtx。
- SDK create setup 失败会先析构 ctx，宿主回滚才尝试取消已经注册的 spawn，时间顺序相反。
- JS 壳先返回成功，脚本异步注册工具；依赖者可在未 Ready 时进入，资源冻结也早于 JS 初始化结束。
- Agent 没有调用 `checkInterfacesForSide`；manifest require 的约束只在 Client 真正实施。
- Agent unload 超时不复位 unloadRequested，之后无法重试；依赖卸载失败被忽略，仍继续销毁 provider。

**修复**：建立 Loading → Ready → Closing → Closed/CloseFailed 状态机；同步 create 仅分配/查询，增加异步 initialize/shutdown；初始化注册先暂存，成功才提交，失败先停任务再销毁。名字预占和 manifest/info 校验统一由公共 loader 完成。

### 5.5 P1：等待降级可能永久挂起（S；无 pool 路径已有定向运行）

- scheduler.offload 无 pool 时只记日志并返回 void，SDK 等不到 done。本轮显式清空 AgentContext 默认 threadPool 后，25ms 截止观察 `completed=0 inflight=1`；结合源码确认已接受请求没有终结路径。该探针单独退出，不声称已经等待到“永久”。正常 AgentContext 默认创建 pool，不能把“测试没有显式设置 pool”误当作 pool 缺失。
- sleep 返回 nullptr 没有安排回调时，SleepAwaiter 仍挂起。
- offload 缺接口时 await_ready=true，但结果 optional 未赋值。
- call_tool/invoke_cap 缺接口时可能成功返回空字符串；spawn 缺 tasks 继续不受托管运行。

**修复**：必需异步设施缺失必须明确拒绝，不能“成功但不执行”。post/sleep/offload 接受结果统一可观测；所有已接受操作保证一次终结。spawn 没有任务托管时默认禁止启动。

### 5.6 P0/P1：强制转换 coroutine promise 类型不具备可移植保证（S）

`plugin_kit.h:1685–1688, 2248–2254, 2455–2460` 等将协程地址恢复为 `coroutine_handle<PromiseBase<void>>`，即使实际帧来自 `Task<std::string>::promise_type` 或 `Task<void>::promise_type`。C++ 不保证不同 promise 类型的帧偏移与继承布局可这样互换；“基类字段在本编译器上碰巧同位置”不是协程 ABI 契约。

**修复**：root 创建时保存针对真实 Promise 的 `resume/destroy/cancel` 类型擦除函数；只通过正确 promise_type 访问 promise。普通子 Task 不再包含宿主 notifier，continuation 也在模块内完成，不跨 ABI 传 coroutine_handle。

此外 spawn 的 SpawnRecord/starter 完成后仍保存在 spawns_ 中，只把 coroAddr 清空；大量一次性 spawn 会持续保留闭包和捕获对象。应由任务终态主动移除，保留有界诊断历史即可。

### 5.7 P1：执行适配、超时与取消语义不一致（S）

- 普通 PluginTool::execute_async 使用 default_timeout_ms；互调 callToolAsync 直接调用 spec.execute_start，绕过这条超时逻辑，也不走同一输出处理入口。
- 互调给 args JSON 注入 `tool_call_id`，但给 execute_start 的独立 tool_call_id 参数传空视图；JS `ctx.toolCallId` 与 JSON 中 ID 可能不一致。
- hooks 的 dispatch 传 `cancelToken=nullptr`，catch std::exception/catch(...) 将宿主取消或中断也当普通 hook 失败记录。宿主 C++ 内部应使用 catchErrorAsync 或显式重抛取消；**只有真正 C ABI 边界才把异常转状态，不能把“异常不越 ABI”推广为“所有内部协程吞掉取消”。**
- awaitPluginOp 的直接工具、hook、graph 操作没有像互调/tasks 那样加入实例的可枚举取消登记。卸载能看到 inflight 却未必能主动取消这些工作；需要把所有入口接入统一 provider operation 表。
- 单个工具 timeout 会调用 `cancelRegistry.cancel(sessionId)`，影响同会话其他工具；新轮次 clearCancelled 又只有会话名，没有轮次代次。取消应区分实例、会话轮次、单操作、子操作，单操作取消不应默认污染整轮。

### 5.8 P1：Client Loading 可见性与并发加载仍不完整（S，交错运行待验证）

- Client 的重复名字检查在 create 的 offload 之前，成功后才插 plugins_；两个交错 load 可以都通过检查，注册动作已经执行，最后一次 insert 返回结果未核对。与 Agent 的直接覆盖不同，但同样需要“预占名字 + Loading 记录”。
- create 在 worker 执行，UI/命令注册通过 ioCallSync 立即进入共享快照；实例却尚未 Ready。renderer 可能在 create 未结束时就执行，与 setup/shim 容器操作并发。
- Client unload 只递归 enabled 的依赖者。已禁用但上下文仍存在的依赖者也可能持 provider 资源，不能默认忽略。
- load 使用宿主 util::offloadAsync，若调用方取消而 worker 已进入 create，必须明确 worker/实例的保留与回滚顺序；目前 loader 没有完整的 Loading 关闭事务。该交错未做专项运行，不列为已复现 UAF。

### 5.9 P0：done 与“插件代码已经完全退出”不是同一个事实（S）

- SDK finishIfDone 的顺序是帧销毁 → notify.done → opCleanup；阻塞工具也是 notify.done 后 delete Job。
- 默认同 IO 回调若由宿主执行保护包住，代码返回之前 IO 不会交错执行另一个回调，通常可以安全收尾；**不能把这一性质推广到任意自建线程上报 done。**
- JS 工作线程在 done 后还会执行闭包析构/释放 op；单凭 inflight 在 onDone 中归零，IO 可能提前 destroy/dlclose。
- `InflightGuard` 只持实例裸指针，不拥有实例/模块；callToolAsync 和 capability 的等待状态也未完整拥有 provider 强引用。

**修复**：区分结果已就绪、所有插件执行已返回、模块可卸载。宿主调用的 start/cancel/timer/offload/done 回调必须由宿主侧执行范围保活；自建线程必须进入受管理的线程退出协议。给插件一个“release 最后引用”函数，插件调用后仍要返回插件代码，也不能自动证明安全 dlclose；详见 §7.4。

### 5.10 P1：数据长度、分配与内存释放协议需要补齐（S）

- `PluginString::from/strdup`、hostMemoryCreateString 等未验证 `size + 1` 溢出、uint64_t→size_t 截断或 `data==nullptr && size>0`。这不意味着能防御恶意原生插件，但应防止正常边界参数误用和跨架构错误。
- 多处先拿 AgentxxPluginString，再构造 std::string，最后手动 free；中间分配抛异常时丢失释放。已有 PluginString RAII 应在接收后立即接管。
- error_out 是否允许为空、成功时是否清空、失败返回但部分 out 非空的释放责任，应逐表统一。renderer 错误返回时的输出也必须释放。
- `get_info` 可选且没有完整协商时，未知未来 API 版本被 `>=` 放行并不安全。字符串生命周期、接口表大小、opaque handle 有效期不能靠全局版本检查替代。

## 6. 各插件现状与定向方案

| 插件 | 当前模式 | 主要问题/方案 |
|---|---|---|
| example_plugin | fast echo、Task caller/sleep、同步 hook、双端 UI | 保留示例作用；统一导出与守卫；增加失败、取消、卸载示例而不是手写大量样板 |
| example_graph_node | C start 同步节点、修改图 | 图生命周期见 F04；意图移除代码 `214–229` 实际删除第一条 assistant 而非最后一条；增加多历史消息测试 |
| example_resources | manifest + create 资源注册 | create `108–162` 未交付 `*plugin_ctx`，局部 ctx 析构却返回成功；修复示例契约 |
| example_js / execute_javascript | C++ 壳异步请求 JS 引擎 | Ready/失败/卸载异步确认缺失；壳代码重复，应抽取 ScriptModule adapter 或声明式脚本资源 |
| javascript_engine | 专用线程 + mutex/CV + drivePromise | Promise 轮询、递归泵队列、跨脚本 callTool 阻塞互等；应返回 JS Promise 并在完成事件 settle，而不是同步等待 |
| execute_command | blocking_tool 内私有 io_context | worker 长时间被进程/管道等待占据；迁移宿主 C process/pipe 异步服务，保留整树取消、独立 stdout/stderr 输出策略 |
| websearch | blocking_tool 内私有 io_context | 缺外部取消；schema header=array<object> 但实现支持 object/string/string-array，不支持 object-array；统一 schema 并迁移 C HTTP 异步服务 |
| filesystem | 6 个 blocking_tool；另留一套未注册的异步实现 | 同步/异步代码重复；glob 遍历用恒 false 取消标记；大量 sessionCancelled 可能每项跨线程 fut.get；原子写/no-overwrite/权限及并发需加强 |
| string | blocking regex/html 转换 | 放 worker 合理，但要定义大小/计算预算与取消边界；不必为追求“协程”强行把纯 CPU 算法塞 IO |
| math | fast_tool | 组合/排列循环随输入可极大，IO 线程可长时间阻塞；数值转 int64 范围未验、gcd/lcm 溢出；增加预算与 checked conversion |
| system | fast 时间查询 | 基本适合内联；保留 tzdb 缺失回退，清理重复空上下文/日志 helper |
| planning | fast_tool 同步读写磁盘 + 事件 | ctx 未调用 init 导致 Logger 未装配；无 dataDir 仍返回 success 却不能 read；事件不带 sessionId；多宿主同文件 .tmp 冲突；增加内存态与异步持久化 |
| rag_search | create 扫文件/HTTP embedding；查询 blocking | create 会阻塞 IO；按 Ready 状态异步索引；查询可异步 HTTP + worker 向量计算，索引不可变快照 |
| codegraph | worker 查询、warmup 线程、SQLite/缓存 | 2s sleep 后 join 可阻塞卸载；初始 DB 打开/重试在 create；索引应托管取消；语法提取与框架边界解耦 |
| system_monitor | spawn/offload、工具 offload、能力同步 querySync | 同一 monitor 可被多 worker/IO 并发访问；GPU/PDH static 违反多实例；能力直接阻塞 IO；改单一采样任务与缓存快照 |
| screen_capture | Windows DXGI/GDI + 流线程 | 入口编译错误；工具并发与流线程共享 D3D/GDI 可变状态；WIC RPC_E_CHANGED_MODE 后无条件 CoUninitialize 不配对 |
| computer_use | blocking_tool + SendInput | schema actions 与底层读取 commands/动作名不一致；输入批次需串行，取消需释放已按下键；当前无取消参数 |
| text_selection_monitor | Windows hooks/UIA + 多线程 | `instancePtr()` 可变 static 串实例；COM 初始化/释放线程不固定；失败把 running=false 后 stop 跳过 join；需专用事件线程与实例路由 |
| audio_stream | CMake 标称 Windows、实际全平台 stub | `audio_stream.cpp:33` 是 `#if XX_IS_WIN_D && false`；CMake 却放行 Windows；应明确不支持/暂不发布，不能把历史注释当真实能力 |

### JS 需要单独重构，不要把专用线程一概判为错误

运行任意 JS 可能长时间纯计算，留专用线程是合理隔离。必须修的是：

1. `drivePromise:699–739` 不应 while 等某个 Promise；无 timer 时仍 `sleep_for(1ms)`，有 timer 时 wait predicate 只看 stop，不能及时响应新任务。
2. `B_CALL_TOOL:1224–1283` 只内联当前脚本自己的 tools；另一脚本仍要排入同一 JS 线程，但本线程阻塞在 call_tool_blocking，产生循环等待。
3. `doEventFire:659–684` 对任意订阅事件遍历该脚本全部 handler，未按触发的 subscription/topic 路由。
4. `hookStart` 投递后就 done，宿主 hook 完成不等于 JS hook 完成，时序与原生 hook 不同。
5. `doLoadScript` 顶层已注册部分工具后抛异常，局部 pctx 析构，而宿主注册未撤销，遗留 user_data。
6. `jsCapStart(unload)` 只投递就 done，不能证明 script 已停止；脚本宿主裸指针仍可能被 JS 队列使用。

## 7. 推荐目标设计与明确的协议选择

### 7.1 不变原则

- C ABI 保持纯数据与 opaque handle，**不传 Asio executor、STL、exception_ptr、coroutine_handle**。
- 普通业务与状态更新只在所属 IO 线程；明确 offload/JS/平台专用线程是例外。
- 完成结果和取消由同一个运行时状态机管理；不再让每个接口自行做 reaper/sentinel/计数。
- 所有异步请求从接受开始计入所属实例；安全条件是没有活跃或排队的插件执行，而不只是“某工具已输出结果”。
- 无异步设施即拒绝，不降级为 unmanaged 或在当前线程直接 resume。

### 7.2 运行时收敛

拟引入内部 `PluginRuntime / Operation / InstanceLifetime` 三个职责，而非新的跨 ABI C++ 类：

- Runtime：IO executor、模块与实例登记、调度、注册事务、退出。
- Operation：输入所有权、状态、取消、结果、等待者、caller/provider 租约；所有状态在 IO 修改。
- InstanceLifetime：Loading/Ready/Disabled/Closing/Closed，计数与一次性 idle 通知。

现有 `OpCore`、callToolAsync/invokeCapabilityAsync/registerTask、sleep/offload/post 的重复路径迁到此处。移除 `pump_io` 的业务使用和额外 ioTasks 队列；旧 ABI 槽保留兼容，但不作为正常协程驱动。

### 7.3 异步生命周期

```text
解析 manifest / 确认兼容 / 名称预占
  → 打开模块
  → create（只建立实例）
  → initialize_async（暂存注册，允许 await）
  → 提交注册，Ready，依赖者才可运行
  → Closing：停止接受新工作
  → 取消所有既有操作与背景任务
  → shutdown_async（等待外部线程/平台资源退出）
  → idle 事件，所有回调返回
  → destroy（不得再创建异步操作）
  → 释放模块
```

- idle 等待使用一次性事件/完成列表，减计数到零主动通知；timeout 仅一个截止 timer，不定期查 inflight。
- 停止超时则保留上下文/模块并报告阻塞项，允许重试；不能跳过失败依赖继续销毁 provider。
- 禁用保留可恢复注册元数据；如取消了后台 Task，重新启用走明确的 on_enable，而不是假装只需重新 register_tool。

### 7.4 SDK 与插件开发简化

拆分 SDK，但保留 `plugin_kit.h` 作为聚合入口：

```text
plugin/api/          # 纯 C ABI
plugin/sdk/abi.h     # 字符串、查询、noexcept 边界
plugin/sdk/task.h    # 可组合 Task
plugin/sdk/async.h   # sleep/yield/offload/call/invoke
plugin/sdk/plugin.h  # 实例、注册事务、声明式入口
plugin/sdk/client.h  # UI 模型/动作
plugin/sdk/json.h    # 可选 Json/Schema/ArgReader（有 util 依赖）
```

统一工具、hook、capability、graph 的 operation adapter。必需注册返回结果；同步 hook 显式要求 void，异步 hook 显式接受 Task<void>。输入在首次挂起前必须拥有，SDK 传入持有数据的 Request，不让开发者反复猜 string_view 是否还活着。

**SDK header-only 不等于零链接依赖**：当前 kit 直接包含 util::Json/fmt，第三方使用 kit 通常仍需它们的实现；应区分“仅 C 头可独立开发”与“完整便利层需要支持库”。

### 7.5 为已有异步 IO 提供纯 C 服务，不传宿主 executor

HTTP、process/pipe、必要的文件 IO 可新增独立接口表，内部复用宿主现有实现；跨边界仍是请求结构体指针、借用/拥有数据、cancel handle、完成回调。

推荐先迁移 websearch 和 execute_command，解决每个网络/进程等待占 worker 的问题。文件遍历/正则/解析/向量计算等不可避免的阻塞或 CPU 工作可继续 offload，但要有取消、预算和受控并发。

## 8. 分阶段实施方案与验收

| 阶段 | 工作 | 必须通过的验收 |
|---|---|---|
| P0-A | 修复 F01/F02/F03/F04/F05；补完整异常守卫；修正 call_start 线程 | 5 个 ASan 复现不再报错；失败空返回无 cb；start/cancel/resume 均在所属 IO |
| P0-B | 统一 Operation/caller-provider 保护、queued callback 计数、终态发布、事件式 idle | 不轮询卸载；caller/provider 任一关闭安全；完成取消交错不丢通知、不重复调用 |
| P1-A | 异步 initialize/shutdown、注册事务、版本与 manifest 协商、重复加载/依赖恢复 | 未 Ready 不可被依赖；失败注册全部撤销；CloseFailed 可重试；三级/菱形依赖正确 |
| P1-B | SDK 可组合 Task、void offload、统一 hook/cap/graph adapter、输入所有权 | SDK 正例编译运行；错误签名编译失败；await 缺接口明确错误；无 unmanaged spawn |
| P1-C | HTTP/process C 服务；迁移 web/command/RAG/system monitor；重构 JS Promise 调度 | 大量等待不按请求数占 worker；JS 跨脚本/原生回调 JS 无死锁；cancel 可终止真实工作 |
| P1-D | 修复平台入口、全局缓存/实例指针、COM 与设备线程约束 | Windows 编译和双实例测试；Android 最低支持环境验证；音频 stub 不发布 |
| P2 | Client 文件拆分、统一注册容器/manifest loader、CMake helper、清理旧注释/重复实现 | 行为不变的重构独立提交；示例、文档、构建和测试共用一套契约 |

每一阶段保持旧 ABI 可用或显式拒绝不兼容版本，不应在同一个 v1 表中静默改变结构布局和线程语义。新增表使用独立 IID/版本；若需要变更核心入口契约，明确发布不兼容版本而不是再次“重置全部版本为 1”。

## 9. 本次测试与复现记录

### 9.1 现有测试

使用已有 Debug 产物，未重新全量构建工程：

```bash
agent/build/linux-debug/exec/agentxx_test plugins
agent/build/linux-debug/exec/agentxx_test client_plugins plugin_resources plugin_multi_instance
```

| 模块 | passed | failed | 进程退出 |
|---|---:|---:|---|
| plugins | 328 | 0 | 0（独立限时重跑确认） |
| client_plugins | 300 | 0 | 与下两项同进程，0 |
| plugin_resources | 83 | 0 | 0 |
| plugin_multi_instance | 29 | 0 | 0 |
| 合计 | **740** | **0** | 正常退出 |

首次把 tests 与阅读命令放同一工具调用时触发外层 60s 超时；之后独立限时重跑确认退出码 0。因此不把首次工具超时归因于产品死锁。

### 9.2 独立探针

临时文件位于 `/tmp/agentxx-plugin-refactor-2-audit/`，不属于产品代码变更。探针使用源码头优先 include、现有 Debug 的编译/链接参数及已构建静态库。ABI C17 探针只包含两个 ABI 头，并检查当前 x64 的 StringView/HostVtable 大小。

| 探针 | 结果 |
|---|---|
| abi_c | `cc -std=c17 -pedantic-errors -fsyntax-only` 通过 |
| start_failure | ASan UAF，CallToolAwaiter::await_suspend `plugin_kit.h:1977` |
| unsubscribe | ASan UAF，`plugin_manager_vtable.cpp:178` |
| graph | ASan UAF，unload 后 node.run 调用已释放 user_data |
| shutdown | ASan UAF，取消 sleep 恢复的 Task 访问被 shutdown destroy 的 ctx |
| stale_ui | ASan UAF，旧快照 renderer 访问已释放 RenderShim |
| thread | `execute_start_off_io=1` |
| caller_lifetime | caller 已卸载成功，随后仍收到完成回调 |
| sleep | `callbacks=32 retained_timers=32 inflight=0` |
| disable | `A=0 B=0 C=1`，enable A 后 B 仍为 0 |
| registration | `register_rc=0 registry_contains=0 recorded_tools=1` |
| kit_nested | 编译失败：Task 不可 co_await |
| kit_void | 编译失败：optional<void> |
| screen-syntax | 编译失败：错误的上下文类型/接口调用；非 Windows 全构建结果 |

抽查 example_plugin、execute_command、system_monitor 的 Linux DSO 动态导出表，均只出现预期 agent/client 入口。不能据此推断全部平台/全部插件符号隔离都已验证。

### 9.3 测试覆盖缺口

- ABI 函数入口与 manager 直调用须分开测，不能认为调用同一底层就完全等价。
- `test_plugins.cpp:1901–1935` 的“事务回滚”实际仅 fakeInst + detachAll，未执行真实失败 create，更未启动后台任务后失败。
- `test_plugins.cpp:1937–1970` 的 1000 次并发主要是同 IO 的成功 echo，未验证 worker 发起调用与 start_failure。
- `test_plugin_multi_instance` 双宿主同 executor、主要 example_plugin，不覆盖 Windows 全局指针、GPU 静态缓存和两 IO 线程并发。
- 老 UI 快照、保留旧工具/图节点、create 失败、callback 期间注销/卸载，是独立的生命周期维度。
- 业务测试大量直测 *_impl.h，并有手写 schema；例如 web 测试 header=object 不能发现真实入口 header=array<object>。
- 需要有截止时间的事件式测试等待与可控 fake scheduler；不要依赖 `sleepMs(2/5/50)` 反复探测来证明实现“无轮询”。


