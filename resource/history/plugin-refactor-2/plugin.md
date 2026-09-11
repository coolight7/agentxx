# 插件框架 Reset-v1 重构任务交接文档

> **文档状态：最终交接版；方案已定稿，产品代码重构尚未开始。**
>
> 本文供后续新会话直接执行。它合并了前期源码审查、独立探针结论和用户确认后的最终设计，不再保留互相冲突的旧阶段方案。
>
> **重要前提**：项目尚未对外推广，当前插件只有仓库内置开发插件。因此本轮允许同时修改宿主、C ABI、C++ SDK、内置插件和测试，完全放弃历史插件兼容；接口和线程语义可以彻底重置，但所有内置插件必须在同一提交序列中迁移完成。

---

## 0. 新会话执行须知

### 0.1 当前状态

- 本文是任务交接文档。**当前状态：Reset-v1 重构进行中（未完成）**；逐提交进度、
  验证结果与剩余待办以 `resource/history/plugin-refactor-2/work.md` 为准
  （最新提交 28；该文件**第 1 节=已实现任务内容、第 2 节=待实现任务内容**）。
- 已落地（阶段）：R1 Runtime/Operation；R2 加载事务与异步关闭（含注册事务回滚，
  工具/图/订阅/prompt/hook/能力/资源与客户端 UI 均有真实 DSO 用例）；R3 ABI v1 与
  SDK 统一 root adapter / 反例编译检查；R4 全部内置插件（含 3 个 JS 系插件）的
  start/stop 迁移、后台任务托管与 JS 引擎停/启语义定义；R5 Client 语义渲染/动作代次/
  依赖级联/prompt 贡献；R6 C17 ABI 检查、导出符号白名单、Debug+ASan 回归、
  插件框架定向 UBSan 探针、设计文档 Reset-v1 章节。
- 未完成（阻塞"重构完成"判定）：Windows 平台编译与专项（本机无 Windows 工具链，
  未验证不得声明）；work.md 2.3 记录的"依赖启用事务顺序"残余边界不影响契约
  （失败→Disabled 可重试），但需在结论中列出。
- TSan 定向回归已完成（work.md 3.4 节）：插件框架 7 模块 0 告警 / 1695 断言通过，
  期间修复 3 处数据竞争；扩展模块的告警全部落在非插件模块
  （FFI/HttpServer/测试脚手架）与未插桩三方库（liburing + boost asio io_uring）上，
  需另立任务 —— 即第 12 节"无本仓库代码的 TSan 告警"目前仅在**插件框架范围内**满足。
- 仓库中已有用户未提交修改，至少包括：
  - `agentxx-config.yaml`（用户模型配置改动，勿回退）
- 新会话开始时必须先执行 `git status --short --branch`，不得覆盖上述修改，也不得重置整个工作树。
- `resource/history/plugin-refactor-2/plugin.md` 是本任务的最终事实来源；
  `docs/zh-cn/design/plugins.md` 已按本文更新出 Reset-v1 章节（第 15 节）与相关修订，
  不得反过来覆盖本文的 Reset-v1 决策。

### 0.2 总目标

将当前插件框架从“多处各自管理异步操作和卸载计数”重构为一个有明确状态机的运行时：

1. 插件实例、动态库、插件上下文、注册项、排队任务、完成回调和已编译图节点有统一生命周期。
2. 工具、hook、capability、graph node、后台任务、sleep、offload、跨插件互调使用统一的 Operation 协议。
3. 所有跨线程完成结果先复制到宿主拥有的完成包，再在所属 IO 线程发布；不以原子变量顺序和裸指针猜测安全。
4. 卸载和退出是可等待的异步过程；未达到安全条件时不得 destroy 或 dlclose。
5. SDK 能够自然组合 `Task<T>`，拥有跨挂起点的输入，正确支持 `void`、异常和取消。
6. Client UI 只消费宿主拥有的语义模型，旧快照、旧动作和旧 renderer 不得调用已销毁插件代码。
7. 所有内置插件在 Linux Debug、Windows 编译/专项测试及多实例场景下遵守同一契约。

### 0.3 明确不做的事

- 不保留旧 API 表布局、旧字段顺序、旧线程保证或旧插件二进制兼容。
- 不在 C ABI 中传递 STL、Asio executor、C++ 异常、`std::atomic*`、`coroutine_handle` 或 C++ 对象布局。
- 不用增加几个 `shared_ptr`、删除 mutex、延长一个裸指针或增加轮询来掩盖生命周期问题。
- 不把“析构函数调用 shutdown”当作异步关闭；析构只能处理已经安全关闭的对象。
- 不把阻塞 IO 一律改成协程。纯 CPU、文件遍历、正则、向量计算可以使用受控线程池，但必须可取消、可限额、可等待。

---

## 1. 当前源码范围与事实基线

### 1.1 主要代码位置

| 层 | 主要文件 | 当前职责 |
|---|---|---|
| C ABI | `agent/lib/include/agentxx/plugin/api/plugin_api.h` | Agent 核心表、工具、hook、事件、capability、scheduler、tasks 等接口 |
| Client ABI | `agent/lib/include/agentxx/plugin/api/client_plugin_api.h` | Client 事件、UI 注册、命令、renderer、动作和 overlay 接口 |
| C++ SDK | `agent/lib/include/agentxx/plugin/api/plugin_kit.h`、`plugin_guard.h` | `PluginBase`、`Task`、awaiter、tool/hook/capability/spawn helper、Client helper |
| Agent 公共基建 | `plugin_common.*`、`plugin_manager_base.h` | manifest、依赖排序、IO 投递、内存工具、inflight、卸载等待 |
| Agent 管理器 | `plugin_manager.h`、`plugin_manager_lifecycle.cpp`、`plugin_manager_vtable.cpp`、`plugin_manager_adapters.cpp`、`plugin_manager_capability.cpp`、`plugin_manager_scheduler.cpp`、`plugin_manager_tasks.cpp` | 加载、注册、调度、互调、资源、提示词、卸载 |
| 异步驱动 | `agent/lib/include/agentxx/plugin/op_driver.h` | `OpCore`、start/done 等待、取消、超时后的后台收尾、句柄回收 |
| 图适配 | `plugin_graph_node.h/.cpp` | 将 C ABI graph node 适配为 NeoGraph 节点 |
| Client 管理器 | `client_plugin_manager.h`、`client_plugin_manager.cpp` | Client 插件加载、事件、命令、UI 注册表、renderer、动作和卸载 |
| Agent 生命周期 | `agent/context.cpp`、`agent/base_agent.cpp`、`agent/agent_host.cpp` | AgentContext/AgentHost 的插件装配和销毁 |
| 内置插件 | `agent/plugins/` | filesystem、command、web、RAG、monitor、planning、JS、平台插件和示例 |
| 测试 | `agent/test/plugin/`、相关 `agent/test/core/`、Client 测试 | 插件系统、资源、多实例、Client 和业务插件测试 |

### 1.2 当前执行链

普通插件工具目前大致走：

```text
Toolcall / PluginTool::execute_async
  → awaitPluginOp
  → execute_start(args/session/call_id/notify/error)
      ├─ fast_tool：done 后返回 nullptr
      ├─ tool：创建 SDK Task，resume 后在 sleep/call/offload 处挂起，返回 Job*
      └─ blocking_tool：创建 Job，调用 scheduler.offload
  → OpCore channel / cancellation_signal 等待
  → 插件调用 notify.done
  → 宿主恢复等待协程并回收句柄
```

这条链的主要问题不是“用了协程”，而是：

- `OpCore` 的状态、payload、通知和回调可能由不同线程修改/读取。
- 完成通知可能在调用方 DSO、provider DSO、插件上下文或管理器已经进入关闭状态后到达。
- `inflight` 只覆盖部分执行阶段，不能覆盖排队任务和 caller 回调。
- `waitInflightZero` 通过指数退避定时器轮询，不是一次性 idle 事件。
- `PluginManagerBase` 维护额外 `ioTasks_` 队列，入队 lambda 捕获裸 `this`。
- `AgentContext::~AgentContext()` 和 Client `shutdownAll()` 是同步路径，可能直接 destroy/dlclose。

---

## 2. 已确认问题清单

以下问题已经通过源码分析或独立 Debug/ASan 探针确认。后续实现必须为每类问题增加回归测试；编号用于交接，不要求保留旧实现中的行号。

### 2.1 P0：内存安全、死锁和退出安全

| 编号 | 问题 | 当前根因 | Reset-v1 要求 |
|---|---|---|---|
| F01 | 互调 start 失败时重复调用完成回调 | `nullptr + error` 失败出口仍同步调用 cb，SDK 再按空句柄处理 | 未接受请求绝不回调；所有登记失败回滚 |
| F02 | Agent unsubscribe 释放后仍写 `sub->inst` | vector/EventBus 移除最后引用后 vtable 继续访问裸指针 | 句柄有独立宿主控制块；失效标记先于释放，重复 unsubscribe 安全 |
| F03 | 旧 Client UI 快照调用已销毁 renderer | 快照保存函数指针和裸 `userData`，destroy 后仍可被 TUI 使用 | 快照只保存语义结果/renderer 代次；失效后回退通用展示 |
| F04 | 已编译 GraphNode 在 unload 后使用 plugin context | 节点强持有实例但 unload 仍 destroy，`enabled` 不能表示上下文存活 | 节点持 lifetime/代次；Closing 后拒绝执行，只返回插件已关闭 |
| F05 | `shutdownAll` 不等待后台 Task | 取消挂起协程后立即 destroy，后续恢复访问 ctx | 增加真正的异步 shutdown；destroy 只能发生在 idle 后 |
| F06 | worker 发起互调时 `start` 不在 IO 线程 | 只把 registry 查询投递到 IO，后续登记和 start 仍在调用线程 | ABI 接收入口先复制输入，完整 start/cancel/登记流程投递所属 IO |
| F07 | 跨插件操作只保护 provider 不保护 caller | caller 的完成函数和 `userData` 属于 caller DSO，caller 可先卸载 | 每个操作同时持 caller/provider lease，caller lease 覆盖 cb 返回 |
| F13 | SDK 挂起后继续读取 ABI 借用参数 | `string_view` 只复制地址，不保活 `args/session/call_id` | root operation 建立拥有的 Request，视图只在 Request 存活期间有效 |
| F14 | done 后到 reaper 前仍可调用失效 cancel_ud | 句柄还在 vector 中，cancelFn 未检查已完成状态 | 完成提交先使 cancel 失效；完成后 cancel 只返回 completed |
| F15 | 管理器销毁后排队 lambda 仍执行 | `postToIo`/`postToIoAsync` 捕获裸 `this` | 直接向 executor 投递拥有状态包；不捕获裸 manager |
| F16 | JS 顶层脚本失败遗留工具/事件/timer | 注册先发生，失败只销毁 JS binding | 脚本初始化是注册事务，失败先撤销全部副作用，再释放 JSContext |
| F17 | JS A 调 JS B 同一线程循环等待 | `call_tool_blocking` 阻塞 JS 线程，B 又排队同一线程 | JS API 始终返回 Promise，完成事件回 JS 线程 settle |
| P0-A | `OpCore` 先发布 notified 后写 payload | acquire/release 不能修复错误写入顺序；doneSignal 和回收通道竞态 | 任意线程只提交拥有完成包；IO 线程一次性提交状态、payload、回调和计数 |
| P0-B | `volatile int32_t` 跨线程取消是数据竞争 | IO 写、worker 读，volatile 不是同步原语 | 使用不透明 CancelToken + `is_cancelled`，SDK 不暴露 ABI 原子地址 |
| P0-C | done 不等于插件代码完全返回 | JS/自建线程可能在 done 后继续执行 DSO 代码 | 明确 done 为终止协议；插件必须在 done 后立即返回；宿主 lease 覆盖受管执行阶段 |

### 2.2 P1：协议完整性、可恢复性和功能正确性

| 编号 | 问题 | Reset-v1 要求 |
|---|---|---|
| F08 | sleep 记录不回收，post/sleep 排队阶段不计数 | timer/post 从接受到回调返回均是 Operation；完成/取消统一回收 |
| F09 | 多级依赖禁用/恢复不对称 | 区分 user-disabled 与 dependency-blocked，维护完整依赖图和可恢复状态 |
| F10 | 静态工具冲突时 manager 仍返回成功并记录工具 | `ToolRegistry::registerTool` 返回值是唯一成功依据，失败不得改实例记录 |
| F11 | Task 不能组合，`optional<void>` 编译失败 | Task 提供正确 promise 类型的 awaiter、continuation、void 结果和异常传递 |
| F12 | screen_capture 入口类型/API 不匹配 | 修正上下文和真实 capture API，增加 Windows 编译任务 |
| F18 | Client 同轮派发不复查 alive | 每个 callback 前复查订阅代次/实例状态；取消后不开始新的回调 |
| F19 | hook helper 接受 Task 却不执行 | 同一 root adapter 识别同步 void 和异步 `Task<void>`，错误签名必须编译失败 |
| F20 | prompt 备份恢复覆盖其他 owner 的贡献 | 基础用户配置与 owner contribution 分离，按 owner/key/revision 合成有效值 |
| F21 | JS rejection 被当成普通成功文本 | rejected→FAILED，取消→CANCELLED，timeout 使用真实单调截止时间 |
| P1-A | 接口表只有 version、未做严格协商 | Reset-v1 表使用明确版本/大小校验；当前版本必须精确匹配 |
| P1-B | create/loading/重复加载没有完整事务 | 名称预占，Loading 不可被调用；失败撤销全部注册并可重试 |
| P1-C | GraphRegistry 类型不能简单删除，重载会冲突 | 使用宿主 GraphTypeSlot/代次间接层，旧工厂永远不调用新/旧失效 ctx |

### 2.3 平台和业务插件问题

- `agentxx_system_monitor` 的 GPU/PDH 状态必须从可变 static 改为实例成员；采样任务和缓存快照按实例管理。
- `agentxx_text_selection_monitor` 的 `instancePtr()` 静态全局指针必须删除；使用实例绑定的事件线程/窗口路由；COM 初始化和释放必须在同一线程配对。
- `agentxx_screen_capture` 必须修正当前 Linux 语法检查暴露的入口错误；Windows WIC 只有在实际初始化成功时才 `CoUninitialize`。
- `agentxx_audio_stream` 当前是 stub，CMake 不得把未实现能力伪装为已支持；应明确跳过构建/发布，直到有真实实现。
- `agentxx_execute_command`、`agentxx_websearch`、`agentxx_rag_search` 等可先继续受控 offload，但应逐步迁移到宿主 HTTP/process 服务，避免每个请求占用一个长期阻塞 worker。
- JS 专用线程本身不是错误；必须修复的是同步跨脚本等待、Promise 轮询、失败事务和 stop 完成语义。

---

## 3. Reset-v1 的不可变原则

1. **纯 C ABI**：跨边界只允许定长整数、C 结构体指针、函数指针、字符串视图、宿主分配字符串和 opaque handle。
2. **严格版本**：Agent/Client 全局 API 版本均为 1；每张表带版本和结构大小；宿主要求精确匹配当前 Reset-v1 表，不保留旧兼容分支。
3. **实例隔离**：禁止可变全局和函数级 static 保存实例状态；所有状态在 plugin context 或宿主 lifetime 中；回调通过 user data 恢复实例。
4. **默认 IO 串行**：注册表、生命周期、Operation 状态和默认插件业务只在所属 IO 线程；worker/JS/平台线程是显式例外。
5. **所有已接受操作必须终结**：接受、取消、失败、异常、缺少设施、超时都必须进入一次且仅一次终态；不能“接受但永远不 done”。
6. **事件式等待**：卸载等待使用一次性 idle 事件或完成计数触发器；不使用固定间隔/指数退避检查 inflight。
7. **停止先于销毁**：先阻止新进入，再取消旧操作，再等待所有插件代码和回调返回，最后 stop/destroy/dlclose。
8. **异常不穿越 C ABI**：所有导出入口、宿主 vtable、插件回调 trampoline、完成回调都要有最外层 noexcept 守卫；内部业务协程不能吞掉取消和 NodeInterrupt。
9. **语义 UI**：Client UI 快照不保存可直接执行的插件函数指针；插件 renderer 在 Client IO 线程计算，UI 只读宿主拥有的模型。
10. **没有 unmanaged 降级**：tasks/offload/sleep/post 设施不可用时明确拒绝或生成失败，不允许静默运行无人托管的协程。

---

## 4. 目标运行时设计

### 4.1 三个宿主内部职责

不把 C++ 类暴露到 ABI。实现可以先使用现有文件，最终建议拆出以下职责：

#### `PluginRuntime`

负责：

- 所属 IO executor 和 IO 线程识别。
- 名称预占、实例表、依赖图、注册事务。
- 直接向 executor 投递已拥有闭包，不维护 `ioTasks_ + mutex` 二级队列。
- Operation 表、关闭队列、一次性 idle 通知。
- 统一加载/启用/禁用/关闭状态变化。

#### `InstanceLifetime`

每个 Agent/Client 实例持有一个宿主 lifetime 控制块，至少包含：

```text
state: Loading | Ready | Disabled | Closing | Closed | CloseFailed
instance generation
module handle / plugin context ownership
lease count
close requested
idle notification
registered operation ids
registered resources and UI generation
```

lease 必须覆盖：排队等待、宿主调用 start/cancel、插件等待、done 完成包、宿主完成回调和清理。`enabled` 只表示逻辑是否启用，不能代替 `state`。

#### `PluginOperation`

每个操作持有：

```text
operation id / state
owned args, session id, call id, method and payload
caller lifetime lease (可空)
provider lifetime lease
cancel token
provider opaque handle
completion packet
waiting coroutine / callback
```

状态至少为：

```text
Accepted → Running → Cancelling → Completed
Rejected 仅用于 start 同步返回，不产生 done
```

完成包包含 `status + owned payload + error`，由任意线程产生、投递到 IO；只有 IO 线程修改 Operation 终态、调用 callback、失效 cancel、移除句柄和减少 lease。

### 4.2 完成协议

- 插件 `done` 的 payload 只在本次调用内借用；宿主回调入口第一步复制数据，之后不再保存插件指针。
- `done` 可从任意线程调用，但不得直接修改宿主 Operation 的业务字段，也不得直接恢复协程。
- IO 线程收到完成包后：
  1. 检查 Operation 是否已终结；重复 done 只记录诊断并丢弃。
  2. 提交 status/payload。
  3. 使 cancel 入口失效。
  4. 派发 caller callback（仍持 caller lease）。
  5. 释放 provider/caller lease 的对应阶段。
  6. 移除句柄和完成等待者。
- host-facing `call_tool_async`/`invoke_capability_async` 对“已接受但 provider 同步完成”的调用仍返回宿主托管句柄，完成 callback 异步经 IO 派发；只有真正拒绝才返回 NULL 且不回调。
- provider 的 `execute_start` 可以保持“同步 done 后返回 NULL”的内部形态，但宿主不能把它等同于拒绝；必须由 Operation 记录已接受/已完成。

### 4.3 跨插件调用

一次 A→B 调用必须同时保护：

- B provider lease：覆盖 B 的 start、执行、cancel、done 和受管清理。
- A caller lease：覆盖 A 的 callback、callback user data 和 callback 返回。

如果 caller 正在 Closing，新调用直接拒绝；已有调用仍由 runtime 托管到 callback 返回。`caller->outstandingOps` 只作为诊断/取消索引，不能被当作生命周期保护本身。

### 4.4 IO 投递

- 删除 `PluginManagerBase::ioTasks_`、`runPendingIoTasks` 业务路径和捕获裸 `this` 的投递 lambda。
- `postToIo` 直接 `asio::post(ioExecutor, ownedClosure)`；闭包捕获 `shared_ptr<RuntimeState>` 或独立的 completion state。
- 无需结果的消息如果 runtime 已失效可以安全丢弃；需要恢复协程、完成 callback 或释放 lease 的任务必须持有 completion state，不能静默丢弃。
- `pump_io` 从 Reset-v1 scheduler 表删除；插件不得主动驱动宿主事件循环。

### 4.5 事件式 idle

实例接受操作时增加 lease/operation count；操作最终清理时减少。计数从非零降为零时发布一次 idle event，关闭协程等待该事件与一个绝对截止 timer 的竞速：

- idle 先到：允许进入 stop/destroy。
- deadline 先到：进入 `CloseFailed`，保留实例、ctx 和 DSO，报告阻塞 Operation；可再次调用 close 重试。
- 不能使用 20ms→1s 等轮询观察计数。

---

## 5. Reset-v1 ABI 设计

### 5.1 通用边界

保持以下基础契约：

- `#pragma pack(push, 8)`，跨边界使用 `int32_t/int64_t/uint32_t/uint64_t`。
- `AGENTXX_PLUGIN_CALL` 出现在所有入口、函数指针和回调声明上。
- 核心宿主 vtable 只保留 `alloc/free/query_interface`。
- `AgentxxPluginStringView` 是借用视图；`AgentxxPluginString` 由宿主分配，接收方负责经 host free 释放。
- `AgentxxPluginHost` 的 `opaque` 对插件不透明；宿主不解释插件对象布局，也不跨边界析构插件对象。

所有接口表新增/重置为明确的 `version` 和 `struct_size` 字段；宿主查询后必须检查表非空、版本精确等于 1、大小覆盖所需成员，不能读取短表或接受未知版本。

### 5.2 生命周期入口

Agent 和 Client 的内置插件统一导出：

```text
get_info
create(host, &plugin_ctx)
start(plugin_ctx, notify, error_out)
stop(plugin_ctx, notify, error_out)
destroy(plugin_ctx)
```

- `create` 只分配上下文、查询接口和初始化纯本地字段，不提交工具/UI/事件等运行时注册，不启动不可托管线程。
- `start` 在 IO 线程调用，执行初始化注册事务；可以同步 done，也可以返回宿主托管 Operation 并异步 done。只有 start done 成功后状态才变为 Ready。
- `stop` 在 Closing 状态调用，取消和等待插件自己的线程/定时器/JS job，撤销当前注册；stop 完成后才能 destroy。start/stop 必须支持重复尝试或明确返回 CloseFailed。
- `destroy` 不得创建异步工作、调用宿主注册接口或访问已失效 Operation；宿主只有在 idle 且 stop 已完成后调用。
- 所有入口由 SDK 生成统一 noexcept trampoline；手写入口也必须遵守同一规则。

### 5.3 统一 Operation 和取消

保留统一终态常量：`OK / CANCELLED / FAILED`，新增/重置：

```c
struct AgentxxPluginOperationHandle; /* opaque */
struct AgentxxPluginCancelToken;     /* opaque */

int32_t agentxx_plugin_cancel_is_requested(
    const AgentxxPluginCancelToken* token
);
```

工具、hook、capability、graph node、tasks 的 start/cancel/done 使用同一语义：

- start 必须由所属 IO 线程调用。
- 入参为借用，只在 start 调用期间有效；SDK root adapter 负责复制。
- start 返回 NULL + error 只表示拒绝，不得调用 done。
- 接受后必须 exactly-once done；同步 done 也属于接受。
- cancel 只能由宿主通过 opaque handle 调用；已完成操作 cancel 无操作。
- 插件不得保存宿主传入的 `notify*`、输入视图或 CancelToken 地址超过协议允许的 Operation 生命周期。

### 5.4 Scheduler v1

删除 `pump_io`、`volatile int32_t* cancel_flag` 和 `cancel_sleep`。scheduler 表的目标语义为：

- `is_io_thread`：只读查询。
- `post_to_io`：投递一个受 runtime 管理的 callback，返回明确状态；不允许在当前线程同步重入。
- `sleep`：返回 Operation handle，完成通过统一 notify；取消使用通用 `op_cancel`。
- `offload`：工作函数接收 `const AgentxxPluginCancelToken*`，返回结果/错误；返回 Operation handle，worker 返回前 token 和 provider lease 保持有效。
- 缺少 thread pool、工作函数异常、排队失败或设施关闭都要生成失败终态，不能让 awaiter 永久挂起。

`offload` 的工作函数可以继续是同步函数；SDK 对外提供 `offload<T>` 和 `offload<void>` 两条正确类型路径，不能实例化 `std::optional<void>`。插件内部自有 `std::atomic<bool>` 可以使用，但不得把它作为 ABI 参数。

### 5.5 Tasks v1

`register_task` 必须返回宿主托管 Operation handle；无 tasks 表时 `spawn` 直接失败。任务取消时：

1. IO 线程使 Operation 进入 Cancelling 并调用插件 cancel_fn。
2. 插件唤醒/退出协程。
3. 任务帧完成后 exactly-once notify.done。
4. IO 线程提交完成、移除句柄、释放 cancel_ud 的保护。

不再支持 unmanaged spawn，不再允许宿主在 done 后继续对已失效 `cancel_ud` 调用插件代码。

---

## 6. C++ SDK 目标设计

### 6.1 Task

`Task<T>` 必须拆分“普通子 Task”和“宿主 root operation”：

- 普通 Task 提供 `operator co_await`，由正确的 `promise_type` 设置 continuation；子任务完成后恢复父任务，结果移动给父任务，异常传递给父任务。
- `Task<void>` 使用专门的 void promise/result，不使用 `optional<void>`。
- root adapter 保存类型擦除的 `resume/destroy/cancel` 函数，但只在创建 root 时针对真实 promise 类型生成；禁止把任意协程帧强制转换为另一种 promise 的 `coroutine_handle`。
- root adapter 统一完成通知、Operation handle、lifetime lease 和清理；hook/capability/graph/tool/spawn 不再各自实现一套 Job。
- 取消、`CancelledException`、`NodeInterrupt` 等内部控制流必须继续传播；只有 C ABI trampoline 把真正的异常转为 FAILED。

### 6.2 Request 和输入所有权

SDK 在每个 root tool/capability/graph/hook 操作开始时建立：

```text
Request {
  owned args_json;
  owned session_id;
  owned tool_call_id / method;
  cancel token view;
  plugin context lifetime;
}
```

业务 lambda 可拿 `std::string_view`，但这些视图只在 Request 和 root Task 完成前有效。跨 `co_await` 的数据必须由 Request 或业务自己拥有；不能依赖宿主当前恰好延长了原始参数的生命周期。

### 6.3 注册 helper

- `register_tool`、`register_hook`、`register_capability`、renderer/UI 注册函数返回 `[[nodiscard]]` 的状态。
- 任意注册失败必须能让 start 事务失败并自动撤销之前的注册。
- hook helper 根据 callable 返回类型严格区分同步 `void` 与异步 `Task<void>`；不匹配时编译失败。
- `PluginBase` 析构只释放本地状态；不在析构中尝试跨线程等待宿主。
- SDK 导出宏将 setup 放入 `start` 事务，将 stop 清理放入 `stop`；`create` 只构造 ctx。

### 6.4 SDK 文件组织

可以先保留 `plugin_kit.h` 聚合入口，最终建议拆成：

```text
plugin/api/             纯 C ABI
plugin/sdk/abi.h        字符串、接口查询、边界守卫
plugin/sdk/lifetime.h   Request、CancelToken、Operation adapter
plugin/sdk/task.h       可组合 Task
plugin/sdk/async.h      sleep/yield/offload/call/invoke
plugin/sdk/plugin.h     PluginBase、注册事务、导出宏
plugin/sdk/client.h     Client 语义模型和动作 helper
plugin/sdk/json.h       可选 Json/Schema/ArgReader
```

完整便利 SDK 可以依赖项目 util；仅使用 `plugin_api.h` 的第三方 C 插件不应依赖宿主 C++ 库。

---

## 7. Agent/Client 生命周期和注册事务

### 7.1 实例状态

两侧实例统一使用：

```text
Loading → Ready
Ready → Disabled → Ready
Ready/Disabled → Closing → Closed
Closing → CloseFailed → Closing（可重试）
```

- Loading 不出现在可调用注册表中，或所有查询均拒绝。
- Disabled 保留实例和声明性配置，但不接受新操作；当前操作按策略取消并等待完成。
- Closing 停止所有新 start、post、renderer 请求和事件回调；已开始的 callback 由 lease 保护到返回。
- Closed 的 host opaque 只能用于安全失败；不得再次调用插件函数。

### 7.2 加载流程

```text
解析 manifest
  → 名称预占（拒绝重复加载）
  → dlopen / builtin 查找
  → get_info + API/接口精确校验
  → create（纯构造）
  → start（注册事务，可异步）
  → commit registrations
  → state = Ready
  → 依赖者允许加载/调用
```

失败回滚顺序固定为：

```text
停止接受新工作
  → 取消并等待已接受 Operation
  → 回滚工具/hook/capability/event/resource/prompt/graph/UI 注册
  → stop（若已经开始）
  → destroy
  → 从实例表移除
  → dlclose
```

失败路径不得留下工具名、能力名、EventBus 订阅、图类型、prompt 修改、资源记录或 UI 句柄。

### 7.3 卸载和 AgentContext/AgentHost 退出

新增 `PluginManager::shutdownAsync()` 和 Client 对应异步关闭接口。所有 owner 必须在 IO executor 和 blocking pool 停止前 await：

- `BaseAgent` 增加明确的异步 shutdown/stop 调用点。
- `AgentHost::destroyAgent` 不能同步释放仍可能运行的 AgentContext；改为在所属 IO 上先 await plugin shutdown，再移除 AgentNode。
- Client mode runner 在停止 transport/UI pool 前 await ClientPluginManager shutdown。
- `AgentContext::~AgentContext()` 不能直接调用会 destroy/dlclose 的同步 shutdown。析构只处理已关闭状态；若违反调用顺序，必须保留 runtime/module state 并报错，不能强制卸载。
- `PluginManager`/`ClientPluginManager` 析构必须能证明实例均 Closed；不能把“进程退出”当成所有后台线程已停止的证明。

### 7.4 禁用和依赖恢复

维护两个不同原因集合：

- `userDisabled`：用户明确关闭，依赖恢复不能绕过。
- `blockedByDependencies`：依赖不可用导致的级联关闭，依赖恢复后可自动解除。

禁用按反向依赖递归处理；启用按依赖拓扑处理。每个插件的 start/stop 事务必须可重复执行，订阅、spawn、prompt、resources、UI 注册不能靠“只重新 register 一部分”伪恢复。

### 7.5 Prompt 贡献

移除“每个插件备份旧值、卸载时无条件写回”的模型。维护：

```text
base user prompt
plugin contributions: owner + generation + key + sequence + value
```

有效 prompt 由宿主按明确优先级合成；卸载/禁用只删除或屏蔽对应 owner 的 contribution，不覆盖其他插件或用户在之后写入的内容。Graph definition 修改也必须走 owner/generation 事务，失败时回滚。

---

## 8. Graph、Client UI 和 JS 目标设计

### 8.1 Graph node

`GraphRegistry` 不能直接让已注册 factory 永久捕获裸 plugin ctx。使用宿主 `GraphTypeSlot`：

- factory 捕获 slot，而不是 plugin context。
- slot 保存当前 instance lifetime、generation、run_start/run_cancel 和 user data。
- 创建 node 时复制 generation/lifetime，node.run 每次检查状态。
- Closing/Closed 时 node.run 返回插件已关闭错误，不调用任何 plugin callback。
- 重新加载同名 type 时更新 slot 只能服务新编译节点；旧 node 的 generation 不匹配，不能转而调用新实例。
- 如果第三方 GraphRegistry 无删除接口，不要直接修改为全局可变工厂；使用 slot/代次间接层解决重载和旧节点问题。

### 8.2 Client renderer

当前 `ClientUiRegistry` 快照中的 renderer 函数指针和 `userData` 必须移除或改为仅宿主内部使用。目标链路：

```text
UI/TUI 提交 render request（拷贝 tool 输入）
  → Client IO 线程检查 plugin/generation/state
  → 持 renderer lease 调 plugin render callback
  → 复制 displayName/summary/items 为宿主 JSON
  → 更新不可变 semantic render cache/snapshot
  → UI 下一帧读取 cache；未命中或失效则通用回退
```

- 模板 renderer 可在 UI 侧纯宿主计算。
- 自定义 renderer 不在 UI 线程同步调用。
- 旧 snapshot 只含 cache、plugin name、generation 和 renderer id；generation 失效时不执行 DSO 函数。
- 输出 JSON 的每个分配字段必须在成功、失败、异常路径释放。

### 8.3 Client 事件和动作

- dispatchEvent 构造快照后，逐个 callback 前复查 `alive`、订阅代次、实例状态；前一个 handler 退订后，后一个尚未开始的 handler 不得执行。
- subscription handle 使用独立宿主控制块，插件释放 user data 后仍不能被后续 callback 使用。
- UI action 携带点击时的 plugin/generation/owner；IO 线程复查当前绑定是否仍匹配。重绑、禁用、卸载后旧点击只能丢弃，不能转交同名新实例。
- Client 命令和 overlay 同样经过 instance lease；卸载时 adapter 只接收宿主已复制的数据。

### 8.4 JS

保留专用 JS 线程用于执行任意 JS，但重构：

1. `callTool` 总是创建 JS Promise，完成/失败/取消事件投递回 JS 线程 settle；禁止 condition_variable 等同步等待。
2. 删除 `drivePromise` 的 1ms 轮询；使用明确的任务队列事件、timer deadline 和 Promise 状态。
3. 每个脚本的 tool/hook/event/timer/能力注册属于脚本初始化事务，顶层异常先完整撤销再释放 binding。
4. `hookStart` 和 `jsCapStart(stop)` 的 done 只在真正业务完成/停止后触发。
5. rejected Promise 映射为 FAILED，异常值不再当作成功 payload；timeout 使用 `steady_clock` 的绝对截止时间。
6. 每个 JS 脚本和 engine instance 独立保存状态；不使用跨宿主可变 static。

---

## 9. 内置插件迁移顺序

不要一开始同时修改所有业务插件。按以下顺序迁移，每一步保持可编译：

1. `example_plugin`：作为 SDK 正例，覆盖 fast/tool/sleep/call/offload/hook/capability/spawn 和失败/取消。
2. `example_resources`：验证 create/start 事务、资源回滚和 Ready 状态。
3. `example_graph_node`：验证 GraphTypeSlot、旧节点和重载。
4. `agentxx_filesystem`、`agentxx_string`、`agentxx_math`、`agentxx_system`：验证同步/CPU/offload 的新 SDK 签名。
5. `agentxx_execute_command`、`agentxx_websearch`、`agentxx_rag_search`：接入 CancelToken；随后迁移宿主 process/HTTP 服务。
6. `agentxx_system_monitor`、`agentxx_codegraph`、`agentxx_planning`：验证多实例、后台采样、资源、prompt、Client 语义模型。
7. `agentxx_javascript_engine`、`example_js`、`agentxx_execute_javascript`：最后迁移脚本事务和 Promise 调度。
8. Windows 插件：`screen_capture`、`computer_use`、`text_selection_monitor`、`audio_stream` 按平台 gate 单独修复和编译。

每个插件必须满足：

- no mutable global/static instance state；
- start/stop 可重复或明确拒绝重复；
- 所有注册返回值检查；
- 所有跨挂起输入由 SDK Request 或插件对象拥有；
- cancel 使用新 CancelToken/Operation，不读取 volatile ABI 地址；
- destroy 前没有插件线程、timer、callback 或操作残留。

---

## 10. 实施阶段、提交边界和验收

### R0：交接和契约冻结（本文）

**内容**：确认问题、Reset-v1 ABI、状态机、测试矩阵和迁移顺序。

**验收**：新会话只以本文为方案依据；无旧兼容目标；保留用户工作树修改。

### R1：宿主 Runtime 和 Operation

**内容**：

- 新增/改造 `InstanceLifetime`、Operation 控制块和完成包。
- 重写 `OpCore`，移除跨线程直接修改 status/payload 的路径。
- caller/provider 双 lease。
- 删除 `ioTasks_`、裸 this 投递和 sentinel/reaper 的重复终态通道。
- sleep/offload/post/timer 从接受到回调返回纳入 Operation。
- 事件式 idle 和 close deadline。

**必须通过**：F01/F05/F06/F07/F08/F14/F15，以及 OpCore 完成发布压力测试。

### R2：Agent/Client 加载、注册事务和关闭

**内容**：

- 名称预占、Loading/Ready/Closing 状态。
- create/start/stop/destroy 新入口和 SDK 导出宏。
- 注册事务覆盖工具、hook、capability、事件、资源、prompt、graph 和 Client UI。
- `shutdownAsync` 接入 BaseAgent、AgentHost、Client runner。
- GraphTypeSlot 和 Client semantic renderer cache。

**必须通过**：失败 create 无残留、旧 graph node 安全失败、旧 UI snapshot 安全回退、CloseFailed 可重试、重复加载拒绝。

### R3：ABI v1 和 SDK

**内容**：

- 重写 `plugin_api.h`/`client_plugin_api.h` 的表版本/大小和 scheduler/tasks/cancel。
- 删除 `pump_io`、volatile cancel、unmanaged spawn 和旧 `cancel_sleep` 语义。
- 实现可组合 Task、void offload、Request、统一 root adapter。
- hook/cap/graph/tool 复用同一 adapter。

**必须通过**：C17/C++ ABI layout 检查、nested Task、void offload、错误 hook 签名、输入释放后挂起、取消/异常/超时交错测试。

### R4：内置插件和 JS/平台迁移

**内容**：按 §9 顺序迁移所有内置插件；修复静态实例状态、screen_capture 编译、COM 配对和 JS Promise。

**必须通过**：Linux Debug 全插件构建和插件测试；Windows 编译/双实例专项；JS 跨脚本调用和 rejection/timeout 测试。

### R5：Client、依赖和 prompt 收敛

**内容**：事件 alive 复查、动作代次、prompt contribution、依赖 blocked/userDisabled、enable/disable 事务。

**必须通过**：退订同轮不再派发、旧动作丢弃、prompt 多 owner 叠加/卸载顺序正确、三级和菱形依赖恢复正确。

### R6：验证、文档和发布前审查

**内容**：Debug、ASan/UBSan 定向探针、导出符号、CMake 平台 gate、文档更新和最终 diff 审查。

**必须通过**：所有 P0 回归通过；无已知 UAF、永久挂起、卸载死锁和 unmanaged 操作；更新 `docs/zh-cn/design/plugins.md` 与测试说明；不修改用户已有无关文件。

### 提交建议

建议按以下独立提交，便于新会话或后续开发者回退：

1. `plugin: reset-v1 runtime operation and lifetime`
2. `plugin: transactional loading and async shutdown`
3. `plugin: reset scheduler tasks cancel ABI`
4. `plugin: composable sdk task and request ownership`
5. `plugin: migrate built-in plugins`
6. `plugin: client semantic renderer and generation checks`
7. `plugin: lifecycle regression tests and docs`

不要在一个提交中同时拆文件、改 ABI、迁移所有插件和修改 TUI；每个提交都应有最小构建/测试结果。

---

## 11. 测试和探针矩阵

### 11.1 C ABI 和 SDK 编译测试

- C17 `-pedantic-errors` 包含两个 ABI 头，检查结构体大小、对齐、offsetof、调用约定声明。
- C++ 正例：`Task<string>` 嵌套、`Task<void>`、offload void、async hook、capability、graph。
- C++ 反例：错误 hook 返回类型、错误 CancelToken 类型、跨边界传 STL/协程句柄必须失败或不可编译。
- 检查未知/短 interface table、NULL 函数指针和 API 版本错误均安全拒绝。

### 11.2 Operation/lifetime 回归

使用可控 fake scheduler/fake plugin，不以 `sleepMs(2/5/50)` 反复探测结果：

1. start 拒绝：NULL + error，无 callback，无残留 lease/handle。
2. provider 同步 done + NULL：视为成功，宿主 callback exactly once。
3. done 和 cancel 竞速：只产生一个终态，完成后 cancel 不调用 plugin cancel。
4. worker 任意线程 done：输入 payload 释放后宿主仍取得完整副本。
5. caller unload 与 provider 未完成互调：caller DSO 不提前释放，callback 返回后才 close。
6. queued post/sleep/offload 后销毁 manager：不访问裸 this，不恢复悬挂 coroutine。
7. 32/1000 个 timer/operation：完成后记录全部回收，idle 事件只触发一次。
8. shutdown 中后台 Task 挂起：取消、恢复、done、ctx destroy、dlclose 顺序正确。
9. timeout 后立即 unload：等待实际 plugin execution 完全退出，不跳过 lease。
10. create/start 中途失败：所有注册、资源、prompt、图类型、UI 项回滚。

### 11.3 Client/Graph/JS 回归

- 两个同事件 handler，第一个退订第二个：第二个不执行。
- 保留旧 renderer snapshot，卸载插件后只返回通用模型，不调用 DSO。
- 保留旧 action 点击，卸载/重绑后丢弃，不投递给新代次。
- 编译 graph node，卸载后 run 返回 plugin closed；重载同 type 后旧 node 不调用新实例。
- A/B JS 脚本相互 callTool，不阻塞 JS 线程。
- JS top-level 注册后抛异常：工具、hook、event、timer 全部撤销。
- Promise reject/cancel/timeout 分别映射 FAILED/CANCELLED/FAILED。

### 11.4 多实例和平台回归

- 两个 AgentContext 同进程、不同 IO executor 加载同插件，状态、取消、timer、GPU/PDH、text selection 互不串扰。
- Windows 编译 `screen_capture`、`computer_use`、`text_selection_monitor`；Linux 不应因平台专属插件入口错误而误报全局通过。
- 导出符号只包含规定入口；第三方静态依赖符号保持隐藏。
- audio_stream 未实现时不进入支持矩阵和发布产物。

### 11.5 当前旧基线（仅供比较）

此前使用已有 Debug 产物观察到：

```text
plugins                         328 passed / 0 failed
client_plugins                  300 passed / 0 failed
plugin_resources                 83 passed / 0 failed
plugin_multi_instance            29 passed / 0 failed
合计                            740 passed / 0 failed
相关业务模块合计                 440 passed / 0 failed
总计                           1180 passed / 0 failed
```

这只是旧实现的正常路径基线，不代表生命周期问题已经解决，也不代表当前源码重构后的测试结果。重构后应重新构建，不能直接复用旧二进制作为验收依据。

---

## 12. 交接完成标准

新会话在结束本任务前必须：

- [x] 按 R1～R6 更新本文“实施状态”，未把计划写成已完成（见第 0.1 节；明细在 work.md）。
- [x] 记录每个阶段实际修改的文件、构建命令、测试命令、通过/失败结果（work.md 第 3/7 节）。
- [x] 失败或超时必须记录原因和是否留下 CloseFailed/临时资源（work.md 第 7/8 节）。
- [x] 每次修改后查看 `git diff --check` 和 `git status`，保留用户无关修改。
- [x] 更新 `docs/zh-cn/design/plugins.md`，使公开设计文档与 Reset-v1 实现一致
      （第 15 节 Reset-v1 章节 + 第 2/3/4/6/9/12/14 节修订）。
- [x] 明确已验证平台：Linux（Debug + ASan/LSan、定向 UBSan 探针、定向 TSan）；
      Windows/Android 未验证，不以 Linux 结果代替。
- [ ] 只有在所有内置插件迁移（§9 第 4-8 步）与专项生命周期测试通过、且 work.md 2.6 的
      全部前置条件满足后，才能将本文状态改为“Reset-v1 重构完成”；当前状态仍是
      “重构进行中（未完成）”（剩余：Windows 平台验证）。

**交接给后续会话的第一步**：读取本文，检查工作树，然后从 R1 建立宿主 Operation/Lifetime 基础；不要先修改业务插件，也不要先删除现有测试。
