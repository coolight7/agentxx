# 插件框架 v1 重构设计（锚定协程模型）

> 状态: 设计定稿，待实施（2026-08 评审修订：线程契约勘误 / 完成协议钉死 / 句柄生命周期 / 卸载取消闭环 / 实施顺序重排 / websearch 默认姿势反转）
> 关联: [plugins.md](../../../docs/agent/plugins.md)（旧设计存档）、[design.md](../../../docs/agent/design.md)
> 本文是插件系统的**最终重构蓝图**。不保留任何历史版本兼容；`AGENTXX_PLUGIN_API_VERSION = 1` 重新定义。

---

## 目录

- [1. 目标与不变量](#1-目标与不变量)
- [2. 核心模型：锚定协程](#2-核心模型锚定协程)
- [3. 线程、交错与取消语义](#3-线程交错与取消语义)
- [4. C ABI 契约 (v1)](#4-c-abi-契约-v1)
- [5. plugin_kit.h —— 插件开发 SDK](#5-plugin_kith--插件开发-sdk)
- [6. 宿主侧实现要点](#6-宿主侧实现要点)
- [7. 内置插件迁移矩阵](#7-内置插件迁移矩阵)
- [8. 实施计划](#8-实施计划)
- [9. 测试计划](#9-测试计划)
- [10. 风险与对策](#10-风险与对策)
- [11. 与旧设计的差异对照](#11-与旧设计的差异对照)

---

## 1. 目标与不变量

### 1.1 目标

| # | 要求 | 本方案的落点 |
|---|------|-------------|
| G1 | 默认协程异步接口；插件代码默认跑在与主程序相同的线程上 | 锚定协程：插件协程段物理执行于宿主 io 线程 |
| G2 | 插件协程与主程序 asio 协程互相交错切换、互不阻塞 | 挂起=让出 io 线程，恢复=完成回调在 io 线程派发 |
| G3 | 调用异步函数**零轮询**、真协程切换 | 全链路事件驱动：channel kick / 完成回调 / cancellation slot |
| G4 | 边界保持 COM 式查询 + 纯 C API，不传 C++ 对象 | 核心 vtable 冻结四成员 + 接口表；跨边界仅 函数指针/void*/字符串视图 |
| G5 | 兼容不同编译器 / 依赖库版本 / 标准库版本 | 插件本地 C++（协程帧、awaiter、asio 版本）永不跨界传递 |
| G6 | 降低插件开发复杂度与整体复杂度 | 三件套收缩为 start/cancel 两件套；删除寄生轮询层；SDK 一套 API |

### 1.2 不变量（重构中不动的东西）

- **核心 vtable 四成员冻结**：`alloc` / `free` / `strdup` / `query_interface`；
- **宿主无锁会话模型**：会话可变状态仅在宿主 io 线程读写（`assertIoThread`），一切插件注册/注销/会话访问串行于该线程；
- **卸载安全语义**：inflight 计数贯穿操作全程，`unloadAsync` 等 inflight 归零才 destroy/dlclose；
- client 侧同步回调模型不动（UI 回调天然快速返回）；
- JS 引擎"自有线程 + notify"形态不动（本就是正确的事件驱动形态）。

---

## 2. 核心模型：锚定协程

### 2.1 原理

C++20 协程帧是插件自己的堆内存；**`coroutine_handle::resume()` 从哪里被调用，协程段就运行在哪个线程**。因此：

> 让插件协程的所有挂起/恢复都经由「宿主保证在宿主 io 线程派发的 C 回调」完成，
> 协程段就天然与内置工具的 asio 协程在同一线程上原生交错——
> 无需共享 executor 对象（跨编译器不可能）、无需私有循环、无需任何轮询。

```
宿主 io 线程                                     时间 →
───────────────────────────────────────────────────────────────►
 toolcall 协程 ──co_await──► op_driver park(channel)      （让出，其他协程可跑）
                                 ▲
 plugin start():                 │ kick
   创建 Task 协程帧              │
   resume() #1                   │
     ├─ 段1 执行(解析参数…)       │
     ├─ co_await c.sleep(50) ──┐  │
     │     挂起(登记宿主定时器)  │  │
     ▼                        │  │
 … 其他工具/LLM 协程交错执行…    │  │
                                 │  │
 宿主 sleep 到点(精确唤醒,0轮询) ─┘  │
   cb(blk) → h.resume()             │
     ├─ 段2 执行                    │
     ├─ co_await c.offload(f) ─┐    │ （下阻塞池，io 线程继续让出）
     ▼                        │    │
 … 交错 …                     │    │
 阻塞池 work 完成 → done 投递回 io 线程
   cb → h.resume()
     ├─ 段3 执行
     └─ co_return 结果 → notify.done(OK, payload) ──► channel kick
 op_driver 醒来取结果 → toolcall 协程恢复
```

### 2.2 为什么不能用 `asio::awaitable` 承载锚定协程

| 障碍 | 说明 |
|---|---|
| 启动需要真实执行器 | `awaitable` 必须经 `co_spawn(executor,…)` 启动并绑定执行器；宿主 executor 是 C++ 对象，跨 ABI 共享即 UB（不同编译器/asio 版本布局不同） |
| 完成机制假设有运行中的 scheduler | asio 定时器/socket 的完成 handler 由其关联 reactor 事件循环驱动；锚定模型里唯一的循环是宿主线程，asio 无法把 handler 投进去 |
| 手动 resume 不被支持 | 锚定模型的引擎是「宿主回调里 `resume()`」；`awaitable_frame` 生命周期由 `co_spawn` 私有管理 |

结论：自定义极简 `Task<T>`（约 150 行 header-only）作为锚定载体；两者分层共存，中间地带全部删除。

### 2.3 操作协议：从三件套到两件套

旧协议 `start/poll/cancel` 中，`poll` 的唯一消费者是寄生轮询层。仓库内迁移后不再有任何 poll 型操作，故 **ABI 直接删除 poll**：

```c
/// 被调方操作 = start(启动, 可内联完成) + cancel(协作式取消请求);
/// 终结经 AgentxxOpNotify.done(status, payload) 恰好一次上报。
///
/// start 的三种合法实现:
///   1. 内联完成型 (快同步 <~1ms): 算完 → done(OK) → 返回 NULL;
///   2. 锚定协程型 (推荐): 创建 Task 帧 → resume() 到首挂起点 → 返回 job 句柄
///      (poll 不存在; 完全由完成回调驱动);
///   3. 自管线程型: 登记工作到自有线程 → 返回句柄 (JS 引擎模式);
///   (慢同步委托 offload 属 1/3 的组合: 登记 offload 后返回句柄或内联等待)
/// 启动失败: 返回 NULL 且 *error_out 输出错误 (host->alloc)。
/// 违约检测: 返回 NULL、无 error 且未 done → 宿主按协议错误处理。
```

被删除的机制（随 poll 一起消失）：hint 解释器（0/≥1/DONE 三态）、`kMaxPollHintMs`、DONE-but-not-notified 缓冲、收割协程推进循环、`plugin_poll_loop.h` 全文件、看门狗 poll 阶段。

poll 的表达力缺口由 scheduler 原语补齐：纯 C 状态机插件**不需要** kit 协程——
`sleep(ms, cb)` + `post(fn)` 的回调组合即可表达「等 N 毫秒后继续」（旧手写三件套
sleep_poll 型的等价形态，cb 即推进点）；C++ 插件则直接用 kit Task（见 §5）。
删除 poll 不损失任何表达能力，只删除「由宿主反复询问」这一种驱动方式。

---

## 3. 线程、交错与取消语义

### 3.1 线程契约

| 上下文 | 约定 |
|---|---|
| `start` / `cancel` 回调 | 仅宿主 io 线程调用；非阻塞快速返回（单次 ≤~1ms，看门狗 >100ms WARN） |
| 宿主→插件的原语回调（`sleep` cb / `offload` done / `call_tool` cb…） | 宿主保证在【宿主 io 线程】派发，且**一律经 `asio::post` 入队、禁止同步重入**（含内联完成型目标，见 §4.3）→ 在其中 `resume()` 即为锚定交错；post 派发同时保证恢复不发生在调用方栈上（嵌套深度恒定） |
| 插件→宿主的 `AgentxxOpNotify.done` | **插件任意线程可调**（JS 引擎等自有线程形态依赖此点，宿主不做线程约束）：OpCore 以 CAS 保证恰好一次、`concurrent_channel.try_send` 为线程安全 kick；等待方在 io 线程被唤醒取结果。「io 线程派发」只承诺唤醒/消费侧，不约束 done 调用侧 |
| 插件内多协程 | 同实例多个锚定协程交错 = 单线程协作式并发：共享状态只在检查点之间一致，跨 `co_await` 不持有不变量（与内置工具同纪律） |
| 插件协程段 | 物理执行于宿主 io 线程；两次挂起之间的同步代码 ≤~100ms（看门狗阈值）；禁止阻塞调用 |
| 需要阻塞/CPU 密集段 | `co_await c.offload(fn)` 下池，完成后回 io 线程续跑 |
| 需要真异步 IO (socket/子进程管道) | 自备专用线程 + 私有 io_context（reactor_tool），或自管线程 + notify |
| 恢复嵌套深度 | 完成回调均经 asio 事件循环派发（非同步链式调用），原生栈在两段之间展开，无累积 |

### 3.2 取消传播（零轮询）

两条通道，均为事件驱动：

1. **宿主侧精确中断（主通道）**：
   会话取消 → `CancelToken.emit()` → asio cancellation signal 沿引擎绑定的 `co_await` 链向下传播
   → op_driver 停靠中的 `chan.async_receive` 以 `operation_aborted` 立即返回
   → 调用插件 `cancel` 回调（置 flag / 取消挂起的宿主原语）→ 插件协程在检查点退出。
   预期延迟：微秒~毫秒级。
   *依据*：`neograph/src/core/graph_engine.cpp` `run_async` 入口以
   `asio::co_spawn(..., bind_cancellation_slot(operation->slot(), use_awaitable))`
   启动整条图执行链；嵌套 `co_await` 共享取消状态直达具体异步操作
   （`CancelToken` 文档与 ConnPool 实践均印证）；`asyncWithTimeout` 的
   `make_parallel_group` 亦支持向未完成子操作转发外部取消。
   Phase A 保留注入测试回归此链路（人为断开 slot 的用例验证兜底生效）。
2. **防御性有界停靠（兜底）**：带取消令牌的等待形如「channel.receive ‖ 100ms 定时器」竞速——
   即使 slot 链路在个别路径失效，最坏 100ms 也能把取消送达；无令牌的操作（钩子等）
   直接无限停靠 channel（完成即醒，无定时器）。这不是轮询：定时器只在单次停靠期间存在一次。

3. **插件内主动查询（保留）**：`agentxx.agent.cancel.is_cancelled` 接口表保留（advisory 定位），
   供长任务切片内主动查询以提前终止子任务（execute_command / filesystem 在用）；
   权威通知始终是 `cancel` 回调。

### 3.3 卸载安全

- inflight guard 挂在 **OpCore 的 shared_ptr** 上：core 存活 ⇔ 操作未终结 ⇔ 代码段保活；
  done 触发即释放。
- **放弃路径的收尾哨兵（重要正确性约束）**：等待方因超时/取消提前退出时，awaiter 帧随之
  销毁，但插件的终态 `done(host_ud=core*)` 迟早会到达——**必须有人持有 core 直到那一刻，
  否则迟到回调构成 use-after-free**。因此放弃路径 spawn 一个极简哨兵协程：
  `while(!core->notified) co_await core->chan.async_receive();` 然后释放 guard/core 并释放
  payload。它不做任何"推进"（事件驱动的操作无需被推进），是纯停靠等待者而非旧版轮询
  收割器；仅在放弃路径存在，正常路径零开销。（若插件违约永不通知，哨兵永久等待 →
  unload 30s 超时放弃可重试——与旧模型相同的活性取舍，但可观测性更好，见 §9-3。）
- **挂起原语的可取消性**：job 记录当前唯一 outstanding 的可取消原语句柄（协程同一时刻
  只挂在一个 await 上——sleep 定时器句柄或 call_tool/invoke op 句柄，awaiter 挂起时登记、
  恢复时清空）；`cancel` 回调除置 flag 外对当前 outstanding 句柄调用 `cancel_sleep` /
  `op_cancel` 提前唤醒。注意 sleep 回调签名不带状态参数：提前唤醒后协程在下一个检查点经
  `ctl.cancelled()` 观察到取消并退出（**检查点模式**，而非回调携带状态）。这保证后台任务
  （spawn）与长 sleep 工具的取消延迟不劣于一个检查点间隔。
- **卸载主动取消（unload 闭环）**：宿主按插件实例维护 outstanding-op 登记表（驱动侧
  per-instance）；`unloadAsync`/`detachAll` 在等待 inflight 归零**之前**对本实例全部在途 op
  逐个发出 cancel（→ cancel_sleep 提前唤醒 / 后台任务置取消）——否则一个挂起中的 60s sleep
  会让卸载一直等到 30s 超时放弃。登记表随实例销毁清空（同时是 §4.3 句柄退休机制）。
- 后台任务（`base.spawn`）同样持 guard；`detachAll` 遍历实例的后台任务记录逐个触发取消；
  插件必须在任务里响应取消（检查点），否则 unload 等 30s 超时后放弃（契约写明）。
- **spawn 的 disable/enable 生命周期**（对齐旧 add_timer 语义）：disable → 取消并停止任务
  （记录保留，含任务 lambda 与捕获参数）；enable → 按记录重新 spawn；实例析构时释放记录。
  任务记录与工具注册信息同级存放于 PluginBase。
- poll 型收割循环不存在了；op_driver 仅剩「停靠-唤醒-取结果」+ 放弃路径的哨兵等待。

---

## 4. C ABI 契约 (v1)

### 4.1 入口符号与全局版本

```c
#define AGENTXX_PLUGIN_API_VERSION 1   // 重新定义; 宿主精确匹配门禁, 无历史兼容
入口符号不变: agentxx_plugin_get_info / _create(host, void** ctx) / _destroy(ctx)
client 侧对称三符号不变 (client_plugin_api.h 同步小改, 见 4.4)
```

多实例三铁律不变：禁可变全局；状态只存 `*plugin_ctx`；接口表查询结果入实例上下文。

### 4.2 接口表清单与变更明细

所有接口表首字段 `int version`，本次重置统一为 `1`，此后独立演进。

| IID | 结构体 | 相对旧版变更 |
|-----|--------|--------------|
| `agentxx.agent.tools` | `AgentxxToolsIface` | register/unregister 不变；`call_tool_async` 改为**完成回调形**（见下）；**删除**阻塞版 `call_tool`（kit 提供）；**删除** `AgentxxHostOp` 句柄族 |
| `agentxx.agent.hooks` | `AgentxxHooksIface` | HookSpec 删除 `hook_poll`（start/cancel 两件套），其余不变 |
| `agentxx.agent.events` | `AgentxxEventsIface` | 不变 |
| `agentxx.agent.capabilities` | `AgentxxCapabilitiesIface` | `register_capability_ex` 的方法处理器同两件套化；`invoke_capability_async` 改完成回调形；**删除**阻塞版 `invoke_capability` |
| `agentxx.agent.scheduler` | `AgentxxSchedulerIface` | 成员重排为 `{version, is_io_thread, post, sleep, cancel_sleep, offload}`；**新增一次性 `sleep`**；**删除周期 `add_timer/cancel_timer`**（由 `spawn`+sleep 循环取代，见 5.5） |
| `agentxx.agent.session` | `AgentxxSessionIface` | 不变 |
| `agentxx.agent.plugins` | `AgentxxPluginsIface` | 不变 |
| `agentxx.agent.config` | `AgentxxConfigIface` | 不变（v3 形状定为 v1 基线） |
| `agentxx.agent.model` | `AgentxxModelIface` | 不变 |
| `agentxx.agent.cancel` | `AgentxxCancelIface` | **保留**（用户决策）；头文件标注 advisory 定位 |
| `agentxx.agent.planning` | `AgentxxPlanningIface` | 不变 |
| `agentxx.agent.prompt` | `AgentxxPromptIface` | 不变 |
| `agentxx.agent.json` | `AgentxxJsonIface` | 不变 |
| `agentxx.agent.log` | `AgentxxLogIface` | 不变 |
| `agentxx.agent.resources` | `AgentxxResourcesIface` | 不变 |

### 4.3 关键新签名

```c
/* ---- 完成回调 (统一形态; 宿主保证在宿主 io 线程派发) ---- */
typedef void (*AgentxxOpCb)(void* ud, int status, char* payload); // payload host->alloc,
                                                                  // 所有权归回调方
/* ---- 异步调用句柄: 仅用于取消 (不可轮询/收尸) ---- */
typedef struct AgentxxOpHandle AgentxxOpHandle;

/* tools 表 */
AgentxxOpHandle* (*call_tool_async)(const AgentxxHost* host,
                                    AgentxxPluginStringView name,
                                    AgentxxPluginStringView args_json,
                                    AgentxxPluginStringView thread_id,
                                    AgentxxOpCb cb, void* ud,
                                    char** error_out);
void (*op_cancel)(AgentxxOpHandle* op);        // 幂等; 任意线程可调; 完成后调用无害
                                               // (tools/capabilities 两表各一份;
                                               //  句柄所有权/生命周期见下方设计说明)

/* capabilities 表: invoke_capability_async 同构 (含各自的 op_cancel) */

/* scheduler 表 */
void* (*sleep)(const AgentxxHost* host, long ms, void (*cb)(void* ud), void* ud);
void  (*cancel_sleep)(const AgentxxHost* host, void* timer);
/* post/offload/is_io_thread 语义沿用旧版 (offload 的 done 回 io 线程) */
```

设计说明：

- **回调形取代句柄轮询形**：协程 awaiter 只需「挂起时登记 cb，cb 里 resume」，天然零轮询；
  线程型插件同样可用（cb 里自行 marshal 到自己线程）。
  `AgentxxOpCb` 与 `AgentxxOpNotify.done` 同形（`void(void*, int, char*)`）——ABI 中
  复用同一 typedef，宿主内部驱动器对两种调用方向共用同一 OpCore。
- 阻塞便捷调用不再是 ABI 成员：kit 用「cb + condvar」实现（offload 工作线程场景），
  io 线程调用 fail-fast 的保护移入 kit。
- **`AgentxxOpHandle` 生命周期**：句柄内存归宿主所有，挂入**调用方插件实例**的
  outstanding-op 登记表，随实例 detach/析构统一退休——因此 `op_cancel` 任意线程可调
  （宿主内部投递 io 线程转发）、幂等、完成/退休后调用无害（无 UAF 面）；插件侧不需要
  （也没有）释放句柄的 API。「不可轮询/收尸」与「完成后调用无害」同时成立的前提就是
  这条所有权规则。
- **完成回调禁止同步重入**：宿主派发 `AgentxxOpCb`（及 sleep/offload 的完成回调）一律经
  `asio::post` 入队，绝不在 `call_tool_async` 返回前同步回调——否则内联完成型目标会在
  调用方 awaiter 挂起完成前触发回调（挂起前完成竞态）。kit awaiter 侧再以「原子完成
  标志 + 未挂起则 post 延迟 resume」双保险（见 §5.3）。
- **payload 释放义务**：`AgentxxOpCb` 收到的 payload（host->alloc）由回调接收方负责
  `host->free`；CANCELLED 时 payload 可为 NULL（同旧 take 语义）。
- `ToolSpec` 字符串字段、flags、timeout 语义沿用；**删除字段**：`execute_poll`；
  `HookSpec` 删除 `hook_poll`；`register_capability_ex` 删除 poll 参数
  （`AgentxxCapStartFn` 签名不变）。全局版本门禁使布局变更无需过渡。

### 4.4 client_plugin_api.h

client 侧维持纯同步回调模型，本次仅做对齐性修订：全局版本号重置为 1、各表 version 统一为 1、
文档措辞与新模型一致。**无结构变化**。

---

## 5. plugin_kit.h —— 插件开发 SDK

header-only（`agent/lib/include/agentxx/plugin/plugin_kit.h`），编译进插件本体，仅依赖 plugin_api.h。
文件处置：**plugin_poll_loop.h 删除**；plugin_tool_sync.h 的 inline/sync 包装并入注册族
（保留薄层供纯 C 作者直用两件套）；plugin_iface_helper.h 的 `AgentIfaces/ClientIfaces`
成为 `PluginBase::iface` 成员类型（头保留，被 kit 包含）；plugin_guard.h 保留
（entry 边界守卫仍需要）。

### 5.1 PluginBase —— 每实例上下文基类

```cpp
struct PluginBase {
    const AgentxxHost*           host  = nullptr;
    agentxx::plugin::AgentIfaces iface {};      // entry 时 query 一次
    Logger                       log {};        // 成员 sink (非全局!) → impl 头显式传引用
    // ---- 助手 (集中原 13 处重复实现) ----
    std::string workDir(AgentxxPluginStringView tid = {}) const; // session_work_dir 优先回退 work_dir
    ToolPromptText toolPrompt(std::string_view tool) const;
    std::string argsJson() const;
    bool sessionCancelled(AgentxxPluginStringView tid) const;
    char* strdup(const char* s) const;  void free(char* p) const;
    // ---- 注册 RAII: 记录 spec/shim, destroy 时随实例释放 ----
protected:
    std::vector<std::string> storage_;
    std::vector<std::unique_ptr<void, void(*)(void*)>> shims_;  // 类型擦除 shim 存储
};
```

`Logger` 为实例成员闭包对象（host+log iface 捕获），impl 头函数签名统一接收
`Logger&` 或 `Ctx&` —— **根除 g_log_sink / g_mgr_log_sink 进程级全局**（多实例日志串扰缺陷修复）。

### 5.2 Task<T> —— 锚定协程类型（插件本地，约 150 行）

```cpp
template<class T> struct Task {          // 惰性启动: initial_suspend suspended
    // promise: 无执行器依赖; 异常在包装层捕获映射; 终态自动 notify.done
};
```

- 生命周期：start 适配器创建帧 → `resume()` 到首挂起点 → job 句柄返回宿主；
  终结时报告 done 并销毁帧（final_suspend + 显式 destroy）。
- **完成协议（实现正确性的关键，kit 单点实现）**：promise 的 `final_suspend` 恒为
  `suspend_always`；**每个** resume 入口（start 适配器与全部原语 trampoline）在
  `h.resume()` 返回后检查 `h.done()` → 命中则调用统一的 `finishIfDone(h)`：提取
  结果/异常 → **先销毁协程帧** → 再 `notify.done(...)`。「帧先于 done 销毁」保证
  OpCore 在 done 触发时释放 guard 的瞬间，帧内局部对象的析构（插件代码段）仍被
  inflight 覆盖——不存在「guard 已释放、析构仍在执行可能已卸载的代码」的窗口。
- 异常：**每个** resume trampoline 的 `h.resume()` 调用均以 try/catch 全收（协程体内
  未捕获异常会从发起本次 resume 的调用处抛出）→ 经 `finishIfDone` 映射
  `done(FAILED, e.what())`。铁律「resume 边界不得抛异常」由此机制保证，而非口头约定
  （否则异常会沿宿主 timer/post handler 逃逸直接 terminate）。

### 5.3 锚定原语（awaiter 族）

| 原语 | 实现 | 备注 |
|------|------|------|
| `co_await c.sleep(ms)` | scheduler.sleep 登记 cb，cb resume；**job 登记 outstanding 句柄，cancel 时 cancel_sleep 立即唤醒（cb 不携带状态——提前唤醒后协程在检查点观察取消）** | 精确唤醒，替代一切定时等待；可取消性保证后台任务/长 sleep 的即时退出 |
| `co_await c.yield()` | post(resume) 到 io | 公平让出检查点 |
| `co_await c.offload(fn)` | scheduler.offload(work=fn, done=resume) | fn 在池线程跑，结果带回 io 线程；fn 内可查 cancel flag |
| `co_await c.call_tool(name,args,tid)` | tools.call_tool_async(cb=resume)+句柄登记 | 取消联动 ctl；FAILED/CANCELLED 映射为异常/取消 |
| `co_await c.invoke_cap(cap,meth,args)` | 同上 | |
| `ctl.cancelled()` / `ctl.throw_if_cancelled()` | 读 job flag (+可选 is_cancelled) | 阶段边界检查点 |

awaiter 通用纪律（挂起前完成竞态）：挂起动作就绪（登记恢复入口 + 完成标志）之后才发起
异步调用；完成回调可能在挂起完成前到达（内联完成型目标、极短 sleep）——trampoline 见
「未挂起」标志则 `post` 延迟 resume，绝不同步重入。与 §4.3 宿主侧 post 派发规则互为
双保险（任一层单独存在即安全）。

### 5.4 注册族（显式命名，不做类型魔术）

```cpp
kit::tool(base, name, depict, schema, &my_task_fn);      // Task<T>(*)(Ctx&,ArgsView,OpCtl)
                                                          // → start/cancel 两件套自动包装
kit::fast_tool(base, spec, &my_sync_fn);                  // 内联完成 (<~1ms)
kit::blocking_tool(base, spec, &my_blocking_fn);          // offload 委托 (带 cancel_flag)
kit::hook(base, point, &my_hook_fn);                      // 快钩子内联 / Task 钩子可选
kit::capability(base, name, &my_cap_method);              // 能力方法 (Task 形)
```

### 5.5 后台任务 —— 取代周期定时器

```cpp
base.spawn([](Ctx& c, OpCtl ctl) -> Task<void> {          // create 时经 post_to_io 启动
    while (!ctl.cancelled()) {
        co_await c.sleep(kIntervalMs);                     // 周期 = 循环内的 sleep
        if (auto r = co_await c.offload(collect); ok(r)) publish(c, r);
    }
});
```

system_monitor 采集器、text_selection 监听等全部迁到此形态；`add_timer/cancel_timer`
自 ABI 删除，"定时器"概念统一为「任务里的 sleep 循环」。
后台任务持有 inflight guard；卸载时置取消并等终结（30s 超时放弃，契约注明任务必须可取消）。

---

## 6. 宿主侧实现要点

### 6.1 op_driver 重写（净减约一半）

```
OpCore {
  atomic<bool> notified; int status; string payload;
  concurrent_channel<void(ec)> chan{ex, 4};           // 容量 1024→4 (kick 幂等)
  shared_ptr<InflightGuard> guard;                    // core 存活 ⇔ 代码段保活
}
onDone(ud,st,payload): CAS 恰好一次 → 存状态/移交 payload → chan.try_send(kick)

awaitPluginOp(args):
  core = make_shared<OpCore>(ex, guard(inst))
  op = drive.start(&core->notify(), &err)             // io 线程
  失败/违约 → throw runtime_error
  while (!notified):
      if (token && token->is_cancelled()) requestCancel
      if (cancelRequested.exchange) safeCancelOnce(drive.cancel, op)
      if (notified) break
      // 有令牌: receive ‖ 100ms 定时器竞速 (兜底); 无令牌: receive 直停
      co_await park(core, token)
  按 status 映射: OK→payload / CANCELLED→CancelledException / FAILED→runtime_error

放弃路径 (等待方被取消/超时中断, catch 后 rethrow 前):
  safeCancelOnce(drive.cancel, op)
  co_spawn(ex, sentinel(core, drive, op), detached)
  // sentinel: while(!notified) co_await chan.receive(); 然后释放 payload+guard+core
  // ——纯停靠等终态, 不推进任何东西; 保证迟到的 done(ud=core*) 不悬垂 (见 §3.3)
```

- 取消主通道信任引擎已绑定的 asio cancellation slot 链路（源码已证实，§3.2）；
  有界停靠仅为兜底；
- detached 驱动（call_tool_async 内部）同样按 tid 取会话令牌接入取消联动（改进），
  其驱动协程天然就是自己的哨兵（等终态后才派发 cb）；
- 派发给插件的完成回调（AgentxxOpCb / sleep cb / offload done）一律 `asio::post` 入队
  （§4.3 禁止同步重入）；异步句柄挂入调用方实例的 outstanding-op 登记表托管
  （unload 时逐项 cancel，见 §3.3 卸载主动取消）。

### 6.2 其余宿主改动

- **call_tool_async 内部驱动**：target start → OpCore；cb 完成即向调用方 cb 派发（均在 io 线程）；
  op_cancel 置标志转发 target cancel；
- **kit 阻塞便捷助手**：call_tool/invoke_capability 阻塞版改为「cb + condvar」实现，
  isIoThread fail-fast 保护移入 kit；
- **生命周期**：detachAll/waitInflightZero/级联/pendingCleanup/prompt 备份逻辑不变，
  但 detachAll 增加「置取消所有后台任务记录 + 对 outstanding-op 登记表逐项发 cancel」
  两步（§3.3 卸载主动取消）；
- **plugin_manager.cpp 拆分**（不改行为）：vtable trampolines / PluginTool+Hook 适配 /
  生命周期 / scheduler(post+sleep+offload) / capability registry 五个编译单元；
- 看门狗补异常路径 exit；`asyncWithTimeout` 包裹关系不变（超时→slot abort→立即 cancel）。

### 6.3 ioCallSync

保留（offload 工作线程访问 io 线程约束 API 的同步通道）；io 线程内联直执行语义不变——
锚定协程段因物理运行于 io 线程，调用这些 API 天然零开销直执行。

---

## 7. 内置插件迁移矩阵

| 插件 | 旧姿势 | 新姿势 | 备注 |
|------|--------|--------|------|
| string / system / planning / rag_search | offload线程池适配异步接口 | `blocking_tool` | 语义不变，样板消失 |
| **filesystem** glob/grep/list 等 CPU 型 | offload | `blocking_tool` | 不变 |
| **filesystem** read/write/edit（真异步 stream_file） | polled (asio stream_file 私有 loop) | **默认** `blocking_tool`(offload 阻塞 IO) | impl 已是 asio 协程 → 升级路径几乎零改动（仅换 loop 宿主为专用线程）。触发升级条件：大文件高并发吞吐导致池线程饥饿。默认取简单优先 |
| example_plugin echo 等 | inline | `fast_tool` | |
| example_plugin sleeper（手写三件套 sleep_poll） | 手写三件套 | `tool` + `co_await c.sleep()` | 成为锚定模型教学样本 |
| **websearch** | polled (asio http 私有 loop) | `blocking_tool`+curl easy 为将来可选简化 | 与 filesystem 同理：impl 已是 asio 协程，reactor 迁移几乎零改动；curl 重写自带编码/压缩/代理行为差异风险（§10），不应作为默认；reactor 形态天然支持高并发，无需预留升级触发条件 |
| **execute_command** | polled (bp::v2) + popen 双路径 | popen 回退并入 `blocking_tool` | 保留精确唤醒/超时击杀语义；双路径样板消失 |
| **system_monitor** 能力采样 | polled (100ms 定时等待) | `tool` + `c.sleep(100)` | 教科书式受益者 |
| system_monitor 周期采集 | add_timer + offload | `spawn` + sleep 循环 + offload | add_timer 消费者清零 |
| text_selection_monitor delayMs | offload（delayMs 在执行函数内） | `blocking_tool` | 机械替换 |
| audio_stream / screen_capture / computer_use | offload | `blocking_tool` | 机械替换 |
| codegraph | offload + 全局日志 sink | `blocking_tool` + Logger 成员化 | 多实例日志串扰修复 |
| javascript_engine | 自有线程 + 手写两件套 | 不动（仅删 poll 字段置 NULL 处） | 已是新模型的自管线程形态；其 JS 线程内 `iface.tools->call_tool` 改用 kit condvar 助手 |
| example_js / example_plugin 互调 | 阻塞 call_tool / invoke_capability | kit condvar 助手（签名同形） | offload 工作线程场景专用 |
| planning client 段 / 双端 UI | 同步回调 | 不动 | client 侧模型不变 |

### 7.1 被删除 ABI 的消费面清单（核查结论：影响范围封闭）

| 被删项 | 消费者 | 迁移动作 |
|--------|--------|----------|
| `AgentxxHostOp` / `makeHostOp` / hop_* | lib 内部（op_driver.h、plugin_manager.*）+ **test_plugins.cpp §31 起 4 组用例**（op->poll / op->take / 恰一次 / 句柄语义断言）——并非零消费者 | 宿主侧重写时移除；测试用例同步改写为回调形断言（cb 状态/payload/恰一次/free 义务），验证目标不变 |
| 阻塞版 `call_tool` / `invoke_capability` | javascript_engine ×1、example_js ×2、example_plugin ×1 | kit 提供 condvar 助手，签名同形，机械替换 |
| `add_timer` / `cancel_timer` | 仅 system_monitor | spawn+sleep 循环重写采集器 |
| `execute_poll` / `hook_poll` 字段 | javascript_engine（置 NULL 处）、example_plugin sleeper、test_plugins.cpp 3 个 poll 用例 | 字段删除 + 用例按两件套协议重写（取消语义测试目标不变） |
| `plugin_poll_loop.h` | execute_command / websearch / filesystem(read/write/edit) / system_monitor 能力 | 随迁移删除 |
| `register_sync_tool` 等旧offload线程池适配异步接口 API | 10 个插件 | 并入 kit 注册族后原头文件退役 |

---

## 8. 实施计划

```
Phase A  lib 内核 (纯增量, 旧 ABI 面不动): scheduler 追加 sleep/cancel_sleep
         (add_timer 暂留) + op_driver 重写 (OpCore/有界停靠/放弃路径哨兵; 对旧
         三件套照常驱动 poll —— 未迁移的 polled 插件仍依赖它) + 回调形
         call_tool_async / invoke_capability_async 以新成员追加 (旧 HostOp/阻塞版
         暂留; 过渡期接口表成员追加仅限仓内同步重编译——项目无外部二进制消费者)
         └─ 验收: 现有 plugins 测试模块全绿 (旧路径行为不变) + 取消延迟专项测试
Phase B  plugin_kit.h: PluginBase / Task<T> (含完成协议, 见 5.2) / 原语族 /
         注册族 / spawn / Logger / 阻塞助手 (condvar)
Phase C  14 个内置插件逐个迁移到 kit 注册族 (新旧姿势并存, 每迁一个跑一次测试);
         test_plugins.cpp 的 3 组 poll 用例 + 4 组 HostOp 用例随迁移改写为回调形
Phase C2 ABI 收尾 (最后一个旧姿势消费者迁完后一次性执行):
         删除 execute_poll/hook_poll 字段、AgentxxHostOp 族、阻塞版
         call_tool/invoke_capability、add_timer/cancel_timer;
         删除 plugin_poll_loop.h; plugin_tool_sync.h 并入 kit (纯 C 薄层保留);
         全局版本号正式重置冻结为 1
Phase D  宿主拆分 plugin_manager.cpp 五件套; 文档同步 (plugins.md 换为本文件为准)
Phase E  测试补充 + windows/linux debug 全量构建回归
```

每阶段完成后编译 + 测试通过再进入下一阶段；Windows 环境优先验证（当前开发机），
Linux 交叉检查（脚本构建）。

ABI 破坏性删除集中在 C2 一次完成：版本重置门禁使布局变更无需兼容过渡，但工程顺序上
「先并存迁移、后删旧面」保证**任意中间提交均可编译、可测试、可回退**（原方案的
Phase A 即删 poll/HostOp，会使 4 个 polled 插件与 7 组测试用例当场编译失败，
「Phase A 全绿」不可达）。

## 9. 测试计划

新增/改造断言（plugins 测试模块）：

1. **唤醒精度**：锚定工具 `sleep(50)` 实测完成延迟 ∈ [50, 70]ms（证明零轮询粒度；
   上界放宽容纳 Windows 默认定时器粒度 ~15.6ms 与 io 线程调度抖动，Linux 可收紧到 55ms）；
2. **取消延迟**：会话 cancel → `cancel` 回调观测 <10ms（slot 主通道，源码链路已证实）；
   人为断开 slot 路径的注入测试下 ≤120ms（有界兜底生效）;
3. **迟到回调安全（哨兵）**：超时放弃等待后，插件延迟 done 不崩溃、guard 正常归零、
   payload 无泄漏；随后 unload 成功；
4. **sleep 可取消**：cancel 触发挂起中的 sleep 立即唤醒（cb 无状态参数），协程在检查点
   观察取消并以 CANCELLED 终结（后台任务卸载延迟 < 100ms 断言）；
5. **交错公平**：20 个并发锚定工具与内置慢工具混跑，io 线程无饿死、无忙等（CPU 占用断言）；
6. **卸载时序**：放弃等待（超时）→ 操作仍终结 → inflight 归零 → unload 成功；
7. **后台任务**：spawn 任务随 disable/卸载取消退出；不可取消任务触发 30s 超时路径；
8. **多实例**：同库双实例日志路由隔离（P0 回归）；双实例并发工具互不串扰；
9. **三姿势等价**：fast/blocking/task 三种注册的同名行为一致性；
10. **回调契约**：sleep/offload/call_tool_cb 的回调线程 == 宿主 io 线程（线程 id 断言）；
11. **JS 引擎回归**：interpreter.js load/unload 经新回调形能力表正常工作；
12. **既有 poll 用例重写**：test_plugins.cpp 中 3 处 `execute_poll` 型用例按两件套协议重写
    （验证目标不变：协议违约合成失败、取消联动、慢操作终结）。
13. **挂起前完成竞态**：`co_await c.call_tool(...)` 目标为内联完成型 fast_tool——
    回调早于挂起完成时不丢结果、不重入、不崩溃（awaiter 原子完成标志路径，
    与宿主 post 派发双保险）。
14. **spawn 生命周期**：disable → 任务在下一检查点退出且不再产生输出；enable →
    按记录重新 spawn 并恢复周期行为；unload → 取消后 inflight 归零。
15. **卸载主动取消**：挂起中的 60s sleep 工具 op + unload → 卸载延迟 < 200ms
    （detachAll 对 outstanding-op 登记表发 cancel 生效，而非等 30s 超时放弃）。
16. **HostOp 用例改写回归**：原 4 组句柄语义用例改写为回调形后，状态/payload/
    恰好一次/free 义务断言全部保留（等价迁移，不许静默删断言）。
17. **同实例多协程交错**：同插件两个锚定工具并发挂起/恢复，共享状态无串扰
    （协作式并发纪律回归）。

## 10. 风险与对策

| 风险 | 对策 |
|------|------|
| slot 链路个别路径未绑定导致取消失灵 | 有界 100ms 停靠兜底 + 注入测试覆盖两条路径 |
| Task resume 边界抛异常 | 包装层全捕获 → FAILED；铁律写入 kit 注释与文档 |
| execute_command 常驻线程成本 | 每实例 1 线程、空闲 park 零 CPU；destroy join；与 JS 引擎同款成熟模式 |
| 第三方手写两件套的学习曲线 | kit 默认姿势只需写一个协程函数；两件套仅框架作者接触；纯 C 作者可用 sleep+post+cb 组合（§2.3） |
| 删除 add_timer 影响潜在第三方 | 项目未推广、无外部消费者；且 sleep 循环表达力覆盖其全部用例 |
| 完成回调早于 awaiter 挂起（内联完成型目标） | 宿主侧一律 post 派发 + kit awaiter 原子完成标志双保险（§4.3/§5.3）；测试 #13 覆盖 |
| 卸载撞上挂起中的长 sleep / 不可取消后台任务 | detachAll 对 outstanding-op 登记表逐项发 cancel（§3.3）；仍不可取消者走既有 30s 超时放弃路径 |
| Task 帧销毁与 done/guard 释放时序错位 → 卸载后跑析构 | finishIfDone 单点收尾：先销毁帧再 done（§5.2）；测试 #3/#6 覆盖 |
| op_cancel 迟到调用（完成后/退休后） | 句柄归宿主、挂实例登记表、随 detach 退休（§4.3 生命周期），迟到调用幂等无害 |

## 11. 与旧设计的差异对照

| 维度 | 旧 | 新 |
|------|----|----|
| 操作协议 | start/**poll**/cancel 三件套 | start/cancel 两件套（notify 不变） |
| 插件异步姿势 | inline/sync/polled/手写 四选一，样板 ×13 | task(默认)/fast/blocking/reactor 四命名注册，一套 SDK |
| 事件唤醒 | 15ms 轮询步进（寄生 loop） | 完成回调精确唤醒（零轮询） |
| 取消传播 | 每 op 一个 20ms watcher 轮询 | asio slot 链精确中断 + 100ms 有界兜底 |
| 反向调用 | AgentxxHostOp 轮询句柄 + 阻塞版 2ms 自旋 | 完成回调 + 可取消句柄；阻塞助手 kit 内 condvar |
| 周期任务 | add_timer 接口 | spawn 任务 + sleep 循环 |
| 收割机制 | reapUntilDone 低频推进循环 | 放弃路径的收尾哨兵（纯停靠等终态，不推进） |
| 插件上下文 | 13 份手写 PluginCtx + 全局日志 sink | PluginBase 基类 + 成员 Logger |
| 宿主文件 | plugin_manager.cpp ~3.7k 行单体 | 五个编译单元 |
