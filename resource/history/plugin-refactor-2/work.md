# 插件框架 Reset-v1 重构交接摘要（进度 / 提交边界 / 验证事实）

> 事实来源：设计定稿是 `resource/history/plugin-refactor-2/plugin.md`（Reset-v1 方案、R0-R6 阶段、F/P 问题编号、测试矩阵）。本文件只记录进度、提交边界、验证结果和待办；与 plugin.md 冲突时以 plugin.md 为准。
>
> 本文件更新时间：2026-09-11（R4-1 内置插件迁移提交）。**状态：Reset-v1 未完成。**
> 当前重构进度 = 十四个提交：`3a4497ba`（R1-1 Runtime / Operation）、`f861bcf9`（fix-build）、
> `b2b5114a`（Operation/Runtime 可靠性、加载事务与关闭、owner 顺序、ABI v1 / SDK 推进）、
> `c2869f07`（P0-1 宿主控制块 / 迟到调用安全失败 / 注册执行期复查，见第 3.4 节）、
> `8c717236`（P0-2 Operation 终态与取消线性化，见第 3.5 节）、
> `aa4b33ff`（P1-2 C17 ABI 编译期检查与接口表严格协商，见第 3.6 节）、
> P1-1 提交（SDK 拥有型 Request / 统一 root adapter / hook 同步异步分发，见第 3.7 节）、
> P1-4 提交（启用/禁用 start-stop 事务、prompt 贡献模型、依赖级联，见第 3.8 节）、
> P1-3 提交（Client 工具语义渲染缓存、动作代次、client 侧启停事务与依赖级联，见第 3.9 节）、
> P2-1a 提交（example_plugin 双端 start/stop 迁移 + SDK client 生命周期导出宏 + 导出符号白名单脚本，见第 3.10 节）、
> P2-1b 提交（JS Promise 终态映射 / 事件式等待 + 设计文档 Reset-v1 章节，见第 3.11 节）、
> P2-1c 提交（多实例可变静态审计 + 文档例外说明，见第 3.12 节）、
> R1-2 提交（plugin.md 11.2 用例 5/8/9 + GraphTypeSlot 代次用例 + enable/unload 竞态守卫，见第 3.13 节）、
> R4-1 提交（example_resources/example_graph_node 迁移、CancelToken 接入、codegraph 后台任务托管、
> system_monitor 双实例专项，见第 3.14 节）。
> 工作树在该提交后是**干净的**；本文档自身也已包含在最新提交中。
> 已完成/待完成对照见第 5、6 节，内容明细见第 3、4 节。

---

## 0. 新会话执行须知

### 0.1 第一步：确认状态

```bash
cd /home/coolight/program/agentxx
git status --short --branch
git log --oneline -6
git diff --stat
git diff --cached --stat
git diff --check
```

阅读顺序：

1. `plugin.md`（方案、R1-R6 验收标准、第 11 节测试矩阵）
2. 本文件第 1 节（总览）、第 3.4/3.5 节（P0-1 / P0-2 明细）、第 4 节（`b2b5114a` 明细）、
   第 5 节（已完成）、第 6 节（待完成）、第 9 节（下一步）
3. `git show 3a4497ba`、`git show f861bcf9`、`git show b2b5114a`、`git show c2869f07`、
   `git show 8c717236`、`git show HEAD`

预期看到的状态：`main` 比 `origin/main` 领先 4（`b2b5114a`、P0-1、P0-2、P1-2 均尚未推送），
工作树无修改、无 untracked 文件。

### 0.2 工作树保护规则（不得违反）

- 禁止 `git reset --hard`、`git checkout --`、清空 build 目录或批量删除测试。
- 当前基线提交是 P0-1 提交（见第 2 节）；不要重写、回退或压缩已存在的重构提交。
- `TODOS.md` 已在 `b2b5114a` 中入库（含 3 行用户改动），`agentxx-config.yaml`、
  `resource/history/plugin-refactor-2/index.md` 随 `3a4497ba` 入库：这些文件已成为历史的一部分，
  既不要回退，也不要在后续重构提交中顺手修改。
- 若新会话期间又出现用户新改动，保留它们并只提交本任务相关文件。

### 0.3 构建与测试基线

```bash
# 构建（高并行；GCC 16.1 偶发 ICE 时直接重试同一构建目录，不要清空 build）
cmake --build agent/build/linux-debug --target agentxx_test_repo -j12

# 插件专项
timeout 600s agent/build/linux-debug/exec/agentxx_test \
  plugin_runtime plugins plugin_resources plugin_multi_instance client_plugins --fail-fast

# 含 ffi / host / subagent 的扩展回归（ASan + LSan）
ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 timeout 1500s \
  agent/build/linux-debug/exec/agentxx_test \
  ffi_c_api agent_host subagent_tool subagent_bus plugin_runtime plugins \
  plugin_resources plugin_multi_instance client_plugins agent memgrowth --fail-fast
```

当前结果与日志见第 7 节（对应 P0-1 提交）；二进制 `agent/build/linux-debug/exec/agentxx_test`
比源码新，提交后复跑构建为 up-to-date。

---

## 1. 状态总览（对照 plugin.md 第 10 节的 R0-R6）

| 阶段 | 状态 | 事实依据 |
|---|---|---|
| R0 契约冻结 | 完成 | `plugin.md` 定稿；本文件只做进度记录 |
| R1 Runtime / Operation | 基本完成（P0-2 / R1-2 已落地） | `plugin_runtime.h`/`op_driver.h` 已重写并提交（`3a4497ba`）；`b2b5114a` 补齐完成端点、executor 停止重放、`ioCallSync` 快速失败、idle/lease 守卫；`c2869f07` 把 vtable 投递闭包纳入 admission lease；P0-2 提交把 cancel/done 的线性化协议写成显式约束（普通 mutex）、定义完成投递失败的可观察终态并补竞速/重放回归；R1-2 提交补齐 plugin.md 第 11.2 节 5/8/9 条独立用例与 GraphTypeSlot 代次用例（见第 3.13 节） |
| R2 加载事务 / 异步关闭 | 大部分完成（P0-1 / P1-3 / P1-4） | 名称预占、`Loading/Ready/Closing/CloseFailed`、`create/start/stop/destroy`、`shutdownAsync`、owner 顺序、`GraphTypeSlot` 已在 `b2b5114a` 落地；P0-1 补齐宿主控制块、迟到调用安全失败、注册执行期复查；P1-4 完成 agent 侧 start/stop 事务；P1-3 完成 Client 语义渲染缓存（插件 renderer 只在 client io 线程执行）与 client 侧启停事务；仍缺加载期 start 失败的真实 DSO 回滚用例 |
| R3 ABI v1 / SDK | 大部分完成 | 接口表 `struct_size` + SDK 严格校验、opaque CancelToken、scheduler v1（删 `pump_io`/`cancel_sleep`/`volatile`）、tasks handle 语义、SDK scheduler/offload 迁移已在 `b2b5114a` 落地；`c2869f07` 统一 `host.opaque` 令牌；P1-2 补 C17 ABI 编译期检查 + 接口表严格协商；P1-1 补 SDK 拥有型 `Request`、统一 root adapter、hook 同步/异步分发；P2-1a 补 client 生命周期导出宏；仍缺 capability 异步业务、graph node 纳入 SDK adapter、C++ 反例编译测试 |
| R4 内置插件 / JS / 平台 | 部分完成 | 4 个内置插件已迁到 CancelToken 新签名；`f861bcf9` 修平台插件构建；P2-1a 把 example_plugin 迁移为双端 start/stop 正例；P2-1b 完成 JS Promise 拒绝/超时/取消的终态映射与事件式等待；P2-1c 完成可变静态审计；R4-1 完成 example_resources/example_graph_node 迁移、websearch/rag/string CancelToken 接入、codegraph 后台任务托管与 system_monitor 双实例专项（第 3.14 节）；仍缺 JS `callTool` Promise 化、脚本顶层异常事务、其余内置插件 start/stop 迁移、Windows 平台 gate |
| R5 Client / 依赖 / prompt | 基本完成（P1-3 / P1-4） | 事件逐 callback 复查 alive、renderer lease、动作派发校验已落地；prompt contribution（F20）与 agent 侧依赖级联（F09）由 P1-4 落地；语义 renderer cache、动作代次、client 侧启停事务与依赖级联由 P1-3 落地；仍缺 UI 侧的"旧快照模块级"端到端用例（现有用例在 manager 层驱动） |
| R6 验证 / 文档 / 发布审查 | 大部分完成 | P1-2 完成 C17 ABI 编译期检查；P2-1a 完成导出符号白名单脚本（16 库全绿）；P2-1b 完成 `docs/zh-cn/design/plugins.md` Reset-v1 章节（第 15 节）与第 2/3/4/9 节修订；仍缺 UBSan/TSan 与 Windows 平台验证（本机无 Windows 工具链，未验证即不得声明） |

结论：不能把当前状态写成“Reset-v1 完成”。下一阶段建议见第 9 节。

本节与后面第 4-8 节的判定，对应的代码状态是最新提交（P0-1，见第 3.4 节）；
`b2b5114a` 之前的判定仍按第 3.1-3.3 节成立。

---

## 2. 提交边界与工作树状态

```text
基线（重构前）   a805f9cb  --
提交 1          3a4497ba  重构插件框架-R1-1 Runtime / Operation 部分实现   (2026-09-09 14:00 +0800)
提交 2          f861bcf9  重构插件框架-fix-build                          (2026-09-09 18:32 +0800)
提交 3          b2b5114a  重构插件框架-推进 Operation / Runtime 可靠性、加载事务 / 关闭 / owner 顺序、ABI v1 / SDK
                          (2026-09-11 02:15 +0800, 31 文件, +2779/-835)
提交 4（P0-1）  重构插件框架-P0-1 宿主控制块与迟到调用安全失败
                          `c2869f07` (2026-09-11, 16 文件；见第 3.4 节)
提交 5（P0-2）  重构插件框架-P0-2 Operation 终态与取消线性化
                          (2026-09-11, 5 文件；见第 3.5 节)
提交 6（P1-2）  重构插件框架-P1-2 C17 ABI 编译期检查与接口表严格协商
                          (2026-09-11, 4 文件；见第 3.6 节)
提交 7（P1-1）  重构插件框架-P1-1 SDK 拥有型 Request 与统一 root adapter
                          (2026-09-11, 6 文件；见第 3.7 节)
提交 8（P1-4）  重构插件框架-P1-4 启用/禁用事务与 prompt 贡献模型
                          (2026-09-11, 5 文件；见第 3.8 节)
提交 9（P1-3）  重构插件框架-P1-3 Client 语义渲染缓存与动作代次
                          (2026-09-11, 12 文件；见第 3.9 节)
提交 10（P2-1a）重构插件框架-P2-1a example_plugin 双端 start/stop 迁移与导出符号校验
                          (2026-09-11, 7 文件；见第 3.10 节)
提交 11（P2-1b）重构插件框架-P2-1b JS Promise 终态映射与设计文档更新
                          (2026-09-11, 5 文件；见第 3.11 节)
提交 12（P2-1c）重构插件框架-P2-1c 多实例可变静态审计
                          (2026-09-11, 3 文件；见第 3.12 节)
提交 12.1      重构插件框架-状态总览对齐 P2-1 进展 (`1561a11b`, 仅本文档)
提交 13（R1-2）重构插件框架-R1-2 生命周期回归补全与启停/卸载竞态修复
                          (2026-09-11, 3 文件；见第 3.13 节)
提交 14（R4-1）重构插件框架-R4-1 example 正例迁移、CancelToken 接入与后台任务托管
                          (2026-09-11, 8 文件；见第 3.14 节)
工作树          干净（无修改、无 untracked）；origin/main 停在 f861bcf9，提交 3-14 均未推送
```

`b2b5114a` 就是此前工作树里的全部增量，代码与验证记录一一对应（未做任何额外改动）；
它同时包含 `TODOS.md`（+3）与本文档的重写（work.md 计 535 行变更）。详见第 3.3 节和第 4 节。

---

## 3. 已提交内容明细

### 3.1 `3a4497ba` 重构插件框架-R1-1 Runtime / Operation 部分实现

21 文件，+2193/-1121。核心是把插件异步执行收敛到统一的 Operation/Lifetime：

- 新增 `agent/lib/include/agentxx/plugin/plugin_runtime.h`：`PluginInstanceState`（Loading/Ready/Disabled/Closing/Closed/CloseFailed）、`InstanceLifetime`（原子 state + admission 位 + lease count + `waitIdleUntil` 事件式等待）、`InstanceLease`、`PluginRuntime`（executor、IO thread id、Operation 表）。
- 重写 `agent/lib/include/agentxx/plugin/op_driver.h`：`OpCore` 统一 start/done/cancel/句柄回收，provider/caller 双 lease，完成 payload 拥有化后再进 IO 线程发布。
- `plugin_manager_base.h` / `plugin_common.h`：删除 `ioTasks_` 二级队列与捕获裸 `this` 的投递，`waitInflightZero` 改为 idle 事件等待。
- `plugin_manager_scheduler.cpp`（post/sleep/offload 接入 OpCore）、`plugin_manager_tasks.cpp`（`register_task` 返回宿主托管 handle + `cancel_task`）、`plugin_manager_capability.cpp`（provider start 在 IO 线程、输入拥有化）、`plugin_manager_adapters.cpp`、`plugin_manager_lifecycle.cpp`（`pluginCreated/pluginDestroyed/destroyDeferred` + lease 守卫）、`plugin_manager_vtable.cpp`、`client_plugin_manager.{h,cpp}` 同步接入。
- 新测试 `agent/test/plugin/test_plugin_runtime.{h,cpp}`（472 行）并注册到 `agent/test/test.cpp`；`test_plugins.cpp` 两处宿主协程内同步 `engine->run()` 改为 `co_await engine->run_async()`。
- 同提交还包含既有用户文件改动：`TODOS.md`、`agentxx-config.yaml`、`resource/history/plugin-refactor-2/index.md`，以及本文件的初版（407 行）。

注意：R1-1 之后还有 `f861bcf9`（fix-build）与 `b2b5114a`（主要增量）；旧文档中“未创建提交”的说法已过期。

### 3.2 `f861bcf9` 重构插件框架-fix-build

8 文件，+100/-23，全部是构建/平台修复，不含框架语义变化：

- `plugin_kit.h`：`util::insertOrAssignHeterogeneous` 替代异构 map 直接下标赋值，并补 include。
- `util/json.h`：补 `<ostream>`。
- `agentxx_rag_search/CMakeLists.txt`：补 `html2md` 依赖（含 builtin 传递）。
- `agentxx_audio_stream.cpp`：`AudioDataSource source_` 改为实例成员，`start` 失败不再记录为运行中。
- `agentxx_computer_use/ui_control.cpp`、`agentxx_execute_command.cpp`（setup 抽函数）、`agentxx_screen_capture.cpp`：平台入口/编译修复。
- `agent/test/plugin/test_text_selection_monitor.cpp`：+33 行测试。

### 3.3 `b2b5114a` 重构插件框架-推进 Operation / Runtime 可靠性、加载事务 / 关闭 / owner 顺序、ABI v1 / SDK

31 文件，+2779/-835。这是当前进度的主要载体，跨越 R1/R2/R3 并触及 R4/R5：

- R1 收尾：Runtime action 队列与 executor 停止重放、完成端点 tombstone（迟到 `done` 安全丢弃）、
  `ioCallSync` 快速失败、`recursive_mutex` 之外的取消重放、exactly-once 提交与异常隔离。
- R2：`start/stop` 生命周期入口（两端 ABI + 内置插件 + SDK 导出宏）、名称预占、
  `unloadAsyncUntil` 共享截止时间、agent/client `shutdownAsync`、`CloseFailed` 可重试、
  同步析构不再绕过 stop/lease、owner 顺序（`BaseAgent`/`AgentHost`/`AgentContext`/
  `FfiAgentRuntime`/`mode_runners`）、`GraphTypeSlot` 与节点代次校验。
- R3：接口表 `struct_size` + SDK 严格校验、opaque CancelToken、scheduler v1
  （删除 `pump_io`/`cancel_sleep`/`volatile`，`sleep`/`offload` 返回 Operation handle）、
  tasks handle 语义、SDK scheduler/offload（含 `offload<void>`）迁移、`Task` continuation。
- R4/R5：4 个内置插件迁移 CancelToken；Client 事件逐 callback 复查、renderer lease、
  动作派发校验、`blockedByDependencies`。
- 文档与用户文件：本文档重写、`TODOS.md`（+3 行，用户改动）。

按阶段归类的文件级明细见第 4 节；完成/未完成对照见第 5、6 节；验证结果见第 7 节。

### 3.4 P0-1 提交：宿主控制块与迟到调用安全失败（R2/R3 交叉）

15 文件（见第 2 节；代码文件 12 个 + 测试 3 个 + 本文档）。核心是让"插件保存的
旧 `const AgentxxPluginHost*`"在实例卸载后安全失败，并让 vtable 投递的请求不会
跨过 `dlclose`：

- 新增宿主控制块（`plugin_manager_base.h`）：
  - `PluginHostControl` 持有真正交给插件的 `AgentxxPluginHost` 视图（**进程级稳定
    地址，永不释放**）；`host.opaque` = 控制块地址，作为一次性令牌（地址永不复用）；
  - 进程级注册表 `detail::pluginHostControlRegistry()` 保存控制块强引用（tombstone），
    `resolvePluginHostControl(host)` 按令牌解析；未注册令牌（垃圾值）返回空，
    已关闭实例返回控制块但 `instance()` 为空 -> 全部入口安全失败。
  - 控制块只保存 `weak_ptr<PluginInstanceBase>`，不阻止实例释放；实例析构后引用
    自然失效。
- 实例不再把 host 视图放在自己对象内部：`PluginInstance::host` /
  `ClientPluginInstance::host` 成员删除，改为 `hostView()`（交给插件的地址）+
  `retireHostControl()`（destroy 成功后退休）；`destroyPlugin()` 的所有成功出口
  都调用退休，destroy 失败/延后路径保持原状。
- vtable 入口统一前置 `enterPluginHost<InstanceT, ManagerT>()`（agent 侧
  `enterHost()`、client 侧 `enterClientHost()`）：解析控制块，取实例/管理器
  `shared_ptr` 与 admission lease；任一环节失败即按"实例不存在"返回失败值
  （非 0 / NULL + error），不再有 `instOf(host)` 裸指针强转。
  - `allowClosing=false`（默认）：注册、投递、Operation 创建等"开始新动作"的入口，
    Closing/Disabled 后直接拒绝；
  - `allowClosing=true`：只读查询入口（`get_plugin_args`/`get_language`/`list_plugins`/
    `json_*`/`is_io_thread` 等），关闭过程中仍可执行，由 lease 保证卸载等待其返回。
- 投递保活：新增 `ioCallSyncKeep` / `ioCallSyncVoidKeep`（`plugin_common.h`）。闭包
  按值捕获 vtable 上下文（实例/管理器强引用 + admission lease），卸载的
  `waitInflightZero` 因此覆盖"已排队但尚未在 IO 线程执行"的阶段；agent 侧 82 处、
  client 侧 57 处调用点全部改走该路径（`xx_unsubscribe`/`xx_cunsubscribe` 保持
  句柄语义，改为闭包持有管理器强引用，修掉原先 `.lock().get()` 悬垂）。
- 注册执行期复查：`PluginManagerBase::acceptsRegistration()` 统一检查
  `enabled` + `lifetime->acceptsRegistration()`；agent 侧 `registerTool`/`registerHook`/
  `registerCapability`/`registerCapabilityEx`/`registerGraphNodeType`/`registerTask`/
  `subscribe`/`registerSkillDir`/`registerMemoryFile`/`registerMcpServer`/`setPromptJson`，
  client 侧 `registerStatusItem`/`registerPanel`/`registerInfoSection`/`registerCommand`/
  `registerToolDecor`（update 路径）/`registerToolRenderer`/`bindActionHandler`/`subscribe`/
  `openOverlay` 全部在执行点复查，排队期间发生关闭时拒绝且不产生注册残留。
- 测试：`test_plugin_runtime.cpp` 新增 3 组用例（旧 host 指针安全失败且不转交同名新
  实例、注册执行期复查、工作线程注册 + 关闭的真实 vtable 路径）；`RuntimeFixture`
  改为装配真实宿主 vtable 的控制块；其余插件测试改用 `hostView()`。

未做（仍属 P0-1 之外或未覆盖）：prompt contribution、renderer 语义 cache、
Operation cancel/done 线性化（P0-2）、SDK `Request` 所有权（P1-1）。

### 3.5 P0-2 提交：Operation 终态与取消线性化（R1 收尾）

5 文件（`op_driver.h`、`plugin_runtime.h`、`plugin_manager_lifecycle.cpp`、
`test_plugin_runtime.cpp`、本文档）。要点：

- 取消/完成线性化协议显式化（`op_driver.h`）：`submitMutex_` 由
  `std::recursive_mutex` 改为普通 `std::mutex`，并把三条约束写成 `cancelOnIo`
  上方的契约注释：
    1. `completionSubmitted_` 是"done 已被接受"的唯一切换点，任意线程只在持锁时
       读取/置位，因此重复 done 与 done/cancel/reject 竞争只有一个赢家；
    2. 持锁期间绝不调用插件或调用方代码（插件可能在 cancel 里同步 `notify.done`，
       锁内进入插件就是自锁；这也是普通 mutex 足够的原因）；
    3. 终态（Completed/Rejected）与调用方回调只在 IO 线程的 `commit()`/`reject()`
       中生效，`cancelOnIo()` 只把状态推进到 Cancelling，不产生终态。
- 完成投递失败的可观察终态：新增 `OpCore::completionPending()`（完成包已产生、
  尚未在 IO 线程提交）与 `PluginRuntime::pendingOperationSummary()`（`label#id` 列表，
  定义放在 `plugin_manager_lifecycle.cpp`，因为 `OpCore` 在 `plugin_runtime.h`
  里只有前置声明）。`unloadAsyncUntil` 在等待 lease 归零超时（CloseFailed）时
  输出该摘要，明确"哪几个 Operation 阻塞了关闭"：完成包在 executor 停止期间保留
  在待重放队列，Operation 保持未终结且继续持有 caller/provider lease，实例因此
  进入 `CloseFailed` 并保留 ctx/DSO（可重试），而不是静默泄漏；executor 重新绑定后
  完成包重放并只提交一次。
- 回归：
    - 扩展"executor 停止期间完成包必须保留"用例：断言 `completionPending()`、两侧
      lease 仍持有（provider 1 + caller 1）、`pendingOperationSummary()` 含该操作
      标签；重放后断言完成一次、摘要为空、两侧 lease 归零。
    - 新增 32 轮 cancel/done 并发竞速用例（`std::barrier` 对齐两个线程）：只产生
      一个终态、回调恰好一次且状态为 OK、竞速中的插件 cancel 最多被调用一次、
      终态之后再 cancel 不再进入插件、`runtime()->operations` 与 lease 全部回收。

仍未覆盖（下一阶段）：plugin.md 第 11.2 节第 5/8/9 条（caller 卸载保护、
shutdown 中挂起后台 Task、超时后立即 unload 的独立用例）。

### 3.6 P1-2 提交：C17 ABI 编译期检查与接口表严格协商（R3/R6）

4 文件（新增 `agent/test/plugin/test_plugin_abi_c17.c`、修改 `agent/test/CMakeLists.txt`、
`test_plugin_runtime.cpp`、本文档）。要点：

- **纯 C17 翻译单元**（新文件）：用 C 编译器（`-std=c17 -pedantic-errors -Wall -Wextra`）
  包含 `plugin_api.h` + `client_plugin_api.h`，证明 ABI 头不依赖 C++ 语法；文件内用
  `_Static_assert` 固定：全局版本必须为 1、18 张跨边界结构体全部 8 字节对齐、
  `AgentxxPluginStringView`/`AgentxxPluginString` 的 `data`/`size` 偏移、
  `AgentxxPluginHost.vtable` 首字段、`AgentxxHostVtable` 三项顺序、各接口表
  `version`/`struct_size` 前两字段布局；另声明四个入口函数指针类型（create/start/
  stop/destroy）验证调用约定宏在 C 下可用。
- **C/C++ 布局对照**：C 侧 `agentxx_test_abi_value(id)` 暴露 17 项
  `sizeof`/`offsetof`/版本号，C++ 测试逐项比对（编号表写在 C 文件里，新增 ABI 字段时
  两边同步更新）；未知编号必须返回 `UINT64_MAX` 哨兵。
- **接口表严格协商回归**：新增伪装宿主（`FakeIfaceHost` + `g_fakeIfaceVtable`，
  `query_interface` 返回可控表），验证 SDK 拒绝 `version != 1`、`struct_size <
  sizeof(Iface)` 与 NULL 表，只接受完整且版本精确匹配的表（plugin.md 第 11.1 节）。
- **构建接线**：`agent/test/CMakeLists.txt` 增加 `plugin/*.c` glob 与按文件编译选项
  （GCC/Clang `-std=c17 -pedantic-errors -Wall -Wextra`；MSVC `/std:c17 /W4`）。

仍未做：C++ 反例编译测试（错误 hook 签名等必须编译失败）、导出符号白名单检查。

### 3.7 P1-1 提交：SDK 拥有型 Request 与统一 root adapter（R3）

6 文件（`plugin_kit.h`、新增 `agent/test/plugin/test_plugin_sdk.{h,cpp}`、`test.cpp`、
本文档）。要点：

- **拥有型 Request（F13）**：新增 `detail::RootRequest`（args/session/call_id/method +
  host + cancel token 视图），在业务代码之前把 ABI 借用视图复制成自己的字符串。
  `tool()` 的 Job、`hook()` 的 HookJob、`capability()` 的调用点都改为把视图指向
  Request；宿主借用缓冲区（`drive.start` 的局部字符串）失效后，插件协程继续读取
  仍然正确。
- **统一完成守卫**：新增 `detail::CompletionGuard`（exactly-once notify.done；
  显式 ok/failed/cancelled；`fromCurrentException()` 把异常映射为 FAILED/CANCELLED；
  作用域结束仍未完成时补 FAILED），`fast_tool`/`hook`/`capability` 的同步路径统一
  走它，去掉三处重复的 try/catch+done 代码块。
- **修复真实缺陷（测试暴露）**：`tool()` 与 `spawn` 把 `OpCtl` 放在 start/starter 的
  栈上并以引用传给业务协程，协程一挂起该引用即悬垂（原 example_plugin 的
  `ctl.throw_if_cancelled()` 就踩在这里）。现在 `OpCtl` 由 Job / `SpawnRecord` 持有，
  直到协程真正结束；ASan 的 `stack-use-after-return` 复现与修复都在本次提交内。
- **钩子同步/异步严格分发（F19）**：`hook()` 用
  `detail::invokeHook()`（按可调用性选择 fn(ctx,point,input)/fn(ctx,input)/fn(input)）
  的返回类型做 `if constexpr` 分派：
    - 返回 `void`：调用返回即完成（异常 → FAILED）；
    - 返回 `Task<T>`：start 返回 `HookJob*` 作为宿主可取消的 provider 句柄，完成由
      promise 在协程真正结束后收束；`hook_cancel` 置取消标志并取消嵌套 awaiter。
- **测试**：新增模块 `plugin_sdk`（`agent/test/test.cpp` 注册），用伪宿主接口表捕获
  SDK 注册出的 spec，直接按 ABI 驱动 `execute_start`/`hook_start`，可以精确控制
  借用缓冲区生命周期：覆盖工具输入所有权、同步钩子正常/异常、异步 Task 钩子
  不提前完成 + provider 句柄 + 借用缓冲区失效后仍能完成。

仍属 P1-1 未做：graph node / capability 的异步 Task 分发（capability 目前仍是同步
业务）、正反例编译测试（错误签名必须编译失败）。

### 3.8 P1-4 提交：启用/禁用事务与 prompt 贡献模型（R2/R5）

5 文件（`op_driver.h`、`plugin_manager.h`、`plugin_manager_lifecycle.cpp`、
`plugin_manager_vtable.cpp`、`test_plugin_runtime.cpp` + 本文档）。要点：

- **启用/禁用事务（agent 侧，F09/R5）**：`disable`/`enable` 拆成
  `disableImpl`/`enableImpl`（`userInitiated` 区分用户与级联）：
  - 禁用按直接依赖者递归（原实现只处理一层），三级与菱形依赖现在都能级联；
    级联禁用只置 `blockedByDependencies`，不改写 `userDisabled`；
  - 启用先置位再递归依赖链（父子顺序、循环依赖安全），随后级联恢复
    `blockedByDependencies` 的依赖者，用户显式禁用的插件不被恢复；
  - 关闭流程中（`lifetime->closeRequested()`）拒绝启用状态变化，避免与
    stop/destroy 交错。
- **start/stop 事务收尾（R2/R5）**：导出 `start`/`stop` 的插件在禁用时把 stop
  投递到 IO executor（`requestStopForDisable` → `stopForDisable`），启用时投递
  start（`requestStartForEnable` → `startForEnable`）：
  - `startForEnable` 先补齐仍欠着的 stop 再 start，保证"禁用后立刻启用"最终落到
    启用态且注册只声明一次；
  - stop 成功后清空由 start 重新声明的注册记录（`clearPluginOwnedRegistrations`），
    重复 enable/disable 不再累积重复工具/能力记录；
  - start 失败回到 Disabled、撤销本次已生效的部分注册（`detachInstanceRegistrations`
    + 清记录），实例保留可重试；卸载/下次启用仍会先 stop 清理；
  - legacy 插件（无 start/stop 导出）继续走宿主侧记录恢复
    （`restoreHostSideRegistrations`），行为与旧实现一致（回归用例 7、H4 通过）。
- **生命周期 Operation 的状态门禁修正**：`OpCore::create` 原先把"provider 已禁用"
  一律判为拒绝，导致停用中的实例收不到 stop。现在只有业务操作检查
  `enabled`/可注册状态；生命周期操作（`lifecycle=true`）由放行 Closing 的 lease
  把关（`Closed` 仍拒绝，见 `InstanceLifetime::tryAcquire`）。
- **prompt 贡献模型（F20/R5）**：`setPromptJson` 不再"备份后无条件写回"，改为按
  `(owner, key, sequence, value)` 记录贡献（键名 `system` / `append:<key>` /
  `tool:<toolName>`），有效值 = 首次贡献前的基础值 ⊕ 按 sequence 应用的全部存活贡献；
  宿主之外（用户/其他代码）改过该键时以当前值为新基础（`applied` 比对 + rebase），
  卸载/禁用只删除该 owner 的贡献并重新合成，不覆盖其他 owner，也不写回已卸载
  owner 的旧值。`restorePromptBackup` 保留为兼容入口。
- **回归（`plugin_runtime` 457 → 526 断言）**：
  - 生命周期事务用例：disable 同步摘除 + stop 异步撤销、enable 重新声明、
    连续 3 轮 enable/disable 记录数不增长、禁用后立刻启用的最终态、
    start 失败回滚（探针证明失败前确实登记过工具）、关闭拒绝状态变化、
    卸载补齐 stop 后 destroy/移除实例；
  - 依赖用例：三级 + 菱形级联禁用、按拓扑恢复、用户显式禁用不被级联恢复；
  - prompt 用例 6 组（多 owner 叠加、卸载顺序、外部修改保留、systemPrompt 回退、
    toolPrompt 部分覆盖）。

仍属 P1-4 未做：client 侧 enable/disable 的 start/stop 事务（client 目前只有
`userDisabled`/`blockedByDependencies` 级联，禁用不触发插件 stop，仍未做 client
侧依赖级联恢复）、加载期 start 失败的 DSO 端到端回滚用例（需要真实测试插件）、
graph/UI 注册记录在失败回滚中的显式断言。

### 3.9 P1-3 提交：Client 语义渲染缓存与动作代次（R2/R5）

12 文件（`client_plugin_manager.{h,cpp}` 为主，TUI 侧
`message_list.{h,cpp}`/`agent_tui.{h,cpp}`/`tui_sidebar_content.cpp`/`overlays.{h,cpp}`/
`tui_plugin_adapter.h`，测试 `test_client_plugins.cpp` + 本文档）。要点：

- **工具语义渲染缓存（F03 / plugin.md 第 8.2 节）**：新增
  `ClientToolRenderEntry` / `ClientToolRenderRequest` / `ClientToolRenderCache`
  （宿主拥有、写入后不可变、内部短锁）：
  - `renderClientTool(reg, cache, ...)` 不再调用插件：decor 与预设模版仍是纯宿主
    计算；命中自定义 renderer 时只读缓存，未命中返回 `matched=false +
    pendingRender=true`（UI 本次用通用回退）；
  - UI 侧把输入拷成 `ClientToolRenderRequest` 交给
    `ClientPluginManager::requestToolRender()`，由 client io 线程的
    `performToolRender()` 复查 lease/alive/enabled/可注册状态、持 lease 调用
    `render_fn`，把 displayName/summary/items 拷成宿主对象后写入缓存并
    `uiAdapter->onToolRenderUpdated()` 通知重绘；
  - 输入特征哈希（toolName/args/result/finished/error/maxWidth，FNV-1a）决定命中：
    输入变化必须重新渲染；同键同输入特征的在途请求在缓存内去重，未命中期间不会
    每帧重复投递；
  - 输出字段在成功/失败/异常路径统一由宿主 `hostMemoryFree` 释放；
  - 失效：禁用/卸载走 `invalidatePlugin`（版本号递增 → UI 重建为通用渲染），
    会话切换整体 `clear()`；条目记录 plugin + 实例代次便于诊断与断言。
- **动作代次（plugin.md 第 8.3 节）**：`ClientUiRegistry` 新增
  `instanceGenerations`（插件名 → 实例代次，加载时登记、卸载时移除），TUI 的三处
  按钮命中框（消息装饰、侧边栏/信息栏、面板）、overlay 按钮都记录渲染快照中的
  代次；`dispatchAction(..., generation)` 在 client io 线程复查，代次不匹配
  （同名插件重载）直接丢弃，不转交新实例。
- **TUI 接线**：消息块缓存 key 计入
  `ClientToolRenderCache::version(key)`，"通用回退 → 插件语义内容"由
  `onToolRenderUpdated → requestRedraw` 驱动；工具消息两处渲染入口统一走
  `queryToolRender()`。
- **client 侧启用/禁用事务与依赖级联（P1-4 遗留）**：
  `disableImpl` 在摘除 UI 注册后把 stop 投递到 client io 线程
  （`stopForDisable` → `clearPluginOwnedUiRegistrations`），`enableImpl` 对导出
  start 的插件走 `startForEnable`（先补齐欠着的 stop 再 start，失败回到
  Disabled 并撤销部分注册）；legacy 插件的 UI 记录恢复移入
  `restoreHostSideUiRegistrations()`；启用/禁用都增加关闭流程守卫，启用按依赖
  拓扑级联恢复被级联禁用的依赖者（用户显式禁用的不恢复）。
- **回归（`client_plugins` 309 → 374 断言）**：工具语义渲染缓存用例（首查不回退
  到插件、请求后命中缓存并带 plugin/generation、输入变化重新渲染、卸载后旧快照
  只回退且版本号递增）、动作代次用例（当前代次派发、重载后旧代次丢弃、新代次
  派发）、client 启停事务用例（disable 触发 stop、enable 触发 start、关闭拒绝
  状态变化、卸载补齐 stop）、client 三级/菱形依赖级联用例。

仍属 P1-3 未做：TUI 组件级的"旧快照渲染"端到端用例（现用例在 manager 层驱动
`renderClientTool`）；语义缓存目前按 (key, 输入特征) 无上限增长（会话切换会清空，
但仍可考虑按容量淘汰）；`onToolRenderUpdated` 每次重绘整表，未做按块精确失效。

### 3.10 P2-1a 提交：example_plugin 双端 start/stop 迁移与导出符号校验（R4/R6）

7 文件（`agent/plugins/example_plugin/example_plugin.cpp`、
`agent/lib/include/agentxx/plugin/api/plugin_kit.h`、新增
`agent/script/check_plugin_exports.sh`、`test_plugins.cpp`、`test_plugin_resources.cpp`、
`test_client_plugins.cpp` + 本文档）。要点：

- **example_plugin 迁移为 Reset-v1 正例（plugin.md 第 5.2 / 9 节）**：
  - agent 侧：`create` 只构造上下文 + 查询接口；新增 `exampleAgentSetup()`（原
    create 内的工具/hook/事件/能力/prompt 注册）、`exampleAgentStart()`（注册事务，
    失败返回 NULL + error 由宿主回滚）、`exampleAgentStop()`；
    用 `AGENTXX_PLUGIN_AGENT_LIFECYCLE_EXPORT` 生成 noexcept trampoline。
  - client 侧：`create` 只构造；`exampleClientSetup()`/`exampleClientStart()`/
    `exampleClientStop()` 承载状态栏项/面板/Info 段落/命令/事件订阅；`destroy`
    不再调用宿主注册接口（只记日志 + 释放内存），注册撤销统一由宿主在 stop 后
    执行。
  - 由此**真实 DSO 走通**：加载 create→start、disable→stop、enable→stop+start、
    unload→stop→destroy 四条路径；此前只有测试内伪实例覆盖这些事务。
- **SDK 补齐 client 生命周期导出宏**：新增
  `AGENTXX_PLUGIN_CLIENT_LIFECYCLE_EXPORT(CtxType, StartFn, StopFn)`（与 agent 侧
  对称，含最外层异常兜底），避免插件手写 `agentxx_plugin_client_start/stop`
  trampoline。
- **导出符号白名单脚本（R6）**：新增 `agent/script/check_plugin_exports.sh`，
  用 `nm -D --defined-only` 遍历插件动态库，要求只导出
  `agentxx_plugin_{agent,client}_{get_info,create,start,stop,destroy}`。当前 16 个
  插件库全部通过（每库 3/6/10 个符号，无第三方静态依赖符号泄漏）；脚本对普通
  动态库（含第三方库）会判失败，负路径已用系统库验证。
- **测试适配（启用改为异步 start 事务）**：`plugins` 第 7 项与 H4 用例、
  `plugin_resources` 资源恢复用例、`client_plugins` 第 6 项与动作绑定用例改为
  "enable 后等待注册恢复"（轮询上限 200×5ms），并在 client 动作用例中重新绑定
  测试自有的 action handler（它不属于插件 start 事务）。

仍属 P2-1 未做：example_resources / example_graph_node 迁移；string/math/system
签名校准；websearch/rag/planning 接入 CancelToken；system_monitor/codegraph 多实例
专项；JS（`callTool` Promise、删除 1ms 轮询、顶层异常事务、rejection/timeout 映射）；
Windows 平台 gate（本机无 Windows 工具链，未验证）。

### 3.11 P2-1b 提交：JS Promise 终态映射与设计文档更新（R4/R6）

5 文件（`agentxx_javascript_engine.cpp`、`example_js/plugin.js`、
`test_plugins.cpp`、`docs/zh-cn/design/plugins.md` + 本文档）。要点：

- **Promise 终态映射（F21）**：`drivePromise` 返回值由"任何情况都返回一个 JS 字符串"
  改为 `PromiseOutcome{Value | Rejected | Timeout | Cancelled}`：
  - JS 异常值统一归一为 `Rejected`（异常对象由驱动侧取出，不再留在 context 上）；
  - 工具执行据此映射终态：`Value → OK`、`Rejected/Timeout → FAILED`（错误文本带
    拒绝原因，`Error.message` 优先）、`Cancelled → CANCELLED`；
  - 拒绝不再被当成普通成功文本（原实现把拒绝原因塞进结果字符串直接返回）；
  - 定时器/钩子/事件回调中的拒绝改为记录警告日志；
  - JS 内 `agentxx.callTool` 命中本引擎工具时，拒绝改为抛回 JS。
- **删除 1ms 忙轮询**：等待点改为"任务队列 / 下一个定时器到期 / 定时器集合版本号变化 /
  绝对截止时间（`steady_clock`）"四者中最近的一个；定时器集合新增 `timerEpoch_`
  版本号并在注册/清除/执行时递增，`setTimeout` 注册会 `notify_all` 唤醒等待者。
  修复过程中发现并解决了一个真实缺陷：执行完到期定时器后必须立即回到循环头重跑
  QuickJS job（定时器回调解决 Promise 会产生新的 continuation job），否则谓词会
  一直等到下一个（可能 30s 后的）定时器，导致虚假超时。
- **正例与回归**：`example_js` 新增演示工具 `js_reject_demo`（返回 rejected Promise）；
  `plugins` 新增用例断言该工具调用**失败**且错误文本包含拒绝原因。
- **设计文档更新（plugin.md 第 12 节要求）**：`docs/zh-cn/design/plugins.md` 新增第 15 节
  “Reset-v1 实例生命周期与异步契约”（状态机、入口语义、Operation 终态、线程与租约、
  启用/禁用事务、prompt 贡献、动作代次），并更新第 2/4/9 节（线程约定新增第 15 节指引、
  入口符号集补 start/stop 与 `check_plugin_exports.sh`、工具渲染新增"语义渲染缓存"小节）。

仍属 P2-1 未做（本节未覆盖的 JS 项）：`callTool` 尚未改为“总是返回 Promise + 事件回投
JS 线程 settle”（当前仍是同步驱动，只是不再把拒绝当成功）；脚本顶层异常的事务化回滚；
`hookStart` 仍是投递即完成（不等 JS 执行结果）。以上需要重构 JS 执行模型，风险较高，
留给后续会话。

### 3.12 P2-1c 提交：多实例可变静态审计（R4）

3 文件（`agentxx_javascript_engine.cpp`、`docs/zh-cn/design/plugins.md` + 本文档）。
要点：

- 审计全部内置插件的可变 `static`（多实例三铁律第 1 条）：
  - 修复：`agentxx_javascript_engine` 的 `jsCapStart("load")` 活动 op 占位句柄原为
    函数级 `static int`，改为实例成员 `capOpToken_`（`capOpToken()` 访问器），
    同一动态库多实例并存时不再共享可变静态存储；
  - 保留并写入文档例外：`agentxx_filesystem` 的编辑临时文件名序号
    `static std::atomic<uint64_t> s_editTmpSeq`（只增不减、只用于唯一性，
    改为每实例计数反而会造成两实例同名临时文件冲突）；
  - 其余 `static` 均为无状态函数/常量或 `static constexpr`，符合铁律。
- 文档：`docs/zh-cn/design/plugins.md` 第 3 节补充"进程级单调计数器"唯一例外说明。

### 3.13 R1-2 提交：11.2 剩余用例与代次失效回归（R1/R2 收尾）

3 文件（`agent/test/plugin/test_plugin_runtime.cpp`、
`agent/lib/src/plugins/plugin_manager_lifecycle.cpp` + 本文档）。要点：

- **plugin.md 11.2 用例 5（caller 卸载与 provider 未完成互调）**：
  provider 未完成时卸载 caller，等 idle 超时进入 CloseFailed 并保留 ctx/destroy 未调用；
  provider 完成后 callback 仍持有 caller lease（`protectedDuringCallback`）并在 IO 线程
  返回；随后重试卸载成功，destroy 恰好一次。
- **11.2 用例 8（shutdown 中后台 Task 挂起）**：卸载先经 `detachAll → cancel`
  请求取消（不会提前 destroy），任务在自有线程恢复并提交 done；断言顺序为
  `cancel → resumed → doneSubmitted → callback(completion) → destroy`，
  callback 返回前 `pluginDestroyed == false`，lease 归零后才 destroy/Closed/移除。
- **11.2 用例 9（超时后立即重试卸载）**：0ms 超时进入 CloseFailed 后立即重试仍是拒绝
  （不跳过 lease、不 destroy）；插件执行真正退出并提交完成包后，第三次卸载才
  destroy 成功。
- **P1-C/F04 GraphTypeSlot 代次失效用例**：注册有效时节点正常执行；同一实例重新注册
  类型后旧节点返回 "generation is no longer active" 且不调用任何回调；卸载后新旧节点
  只返回插件已关闭；同名 type 重载到新实例后旧节点不转交新实例，仅新代次节点可执行。
- **修复真实竞态（测试暴露）**：`startForEnable` / `stopForDisable` 是投递到 IO 的
  异步事务，可能落后于 `unloadAsync`。原实现会在实例 Closed 后调用
  `setState(Ready)`（触发 `InstanceLifetime::setState` 断言）或在 Closing 期间改写状态。
  现在两处事务在进入与每个 await 之后复查 `closeRequested()/Closed`，关闭期间不启动
  start、不覆盖 Closing 状态；stop 由卸载路径补齐。复现方式：连续运行
  `plugin_runtime plugin_sdk plugins`（修复前一次复现 SIGABRT，修复后 3 次复跑全绿）。
- 回归（`plugin_runtime` 526 → 600 断言）：新增 74 项断言（用例 5/8/9 与 GraphTypeSlot）。

### 3.14 R4-1 提交：example 正例迁移、CancelToken 接入与 codegraph 后台任务托管（R4）

8 文件（`example_resources.cpp`、`example_graph_node.cpp`、`agentxx_websearch.cpp`、
`agentxx_rag_search.cpp`、`agentxx_string.cpp`、`agentxx_codegraph.cpp`、
`test_plugin_multi_instance.cpp` + 本文档）。要点：

- **example_resources**：`create` 只构造上下文 + 查询接口 + 保存 host；运行时 skill
  目录注册移入 `start`；`destroy` 不再调用宿主注册接口（资源撤销由宿主 stop 后统一
  执行）；用 `AGENTXX_PLUGIN_AGENT_LIFECYCLE_EXPORT` 生成 trampoline。
- **example_graph_node**：`create` 只构造 + 查询接口；两个节点类型注册与执行图修改
  移入 `start`（失败返回 NULL + error，宿主回滚已生效注册）；`stop` 只给出完成信号
  （GraphTypeSlot 由宿主在 stop 后失效，旧节点安全失败）。
- **CancelToken 接入**：`agentxx_web_fetch`/`agentxx_web_fetch_markdown`/`agentxx_web_search`
  与 `agentxx_rag_search`、`agentxx_string`（html2markdown/regexp）的 offload 工作函数
  改收 `const AgentxxPluginCancelToken*`，在开始与阶段性边界检查取消并抛
  `CancelledException`（映射为 CANCELLED）。HTTP/正则本体暂不可中断（HttpClient 未暴露
  cancellation slot），取消在其返回后的边界生效。
- **codegraph 后台 warmup 托管化（多实例/后台采样专项）**：原 `std::thread + stop 原子标志`
  在 `create` 启动、`destroy` join（会阻塞 IO 线程且卸载前不可取消）；改为 `start`
  事务内 `ctx.spawn` 宿主托管任务（`sleep(2s) → offload(updateIndex)`），卸载可取消、
  offload 期间持实例 lease，destroy/dlclose 前必然等待索引代码返回。同时 `create`
  不再注册工具/prompt/订阅，注册事务整体移入 `start`。
- **system_monitor 双实例专项**（`plugin_multi_instance` 29 → 48 断言）：同一插件在
  两个 AgentContext 并存加载；采样任务句柄按实例登记且互异；调用
  `agentxx_get_system_core_info` 得到结果；卸载 A 后 A 的采样任务取消回收、B 的实例与
  工具不受影响。
- 回归：`plugins` 331 / `plugin_resources` 83 / `plugin_multi_instance` 48 /
  `client_plugins` 375 / `codegraph` 23 全部通过（第 7.12 节）。

---

## 4. `b2b5114a` 内容明细（已提交，按阶段归类）

以下全部内容已在 `b2b5114a` 中；工作树在该提交后没有额外代码改动。
（P0-1 提交的明细见第 3.4 节。）

### 4.1 R1 收尾（Operation / Runtime 可靠性）

- `plugin_runtime.h`：新增 `RuntimeAction` 待办队列与 `enqueueRuntimeAction()`；executor 停止期间的完成/取消/idle 动作不丢失，executor 重绑后重放；`runtimeExecutorStopped()` 直接识别 `asio::io_context::executor_type`（不能用 `dynamic_cast`，Boost Asio 的 `execution_context` 不是多态类型）。
- `op_driver.h`：新增 `AgentxxPluginOperationCompletionEndpoint`（宿主 tombstone，迟到 `done` 只丢弃不 UAF）；完成提交 exactly-once；callback 与内部 completion handler 异常隔离；取消请求在 executor 停止时可重放。
- `plugin_manager_base.h`：`isIoThread()` 在 direct io_context 已停止时返回 false；`ioCallSync()` 在 runtime 不可用时快速失败（不再永久等 future）；新增 `ioExecutor()`、`hasPendingClose()`、`lifecycleStopPending()`。
- `plugin_manager_vtable.cpp` / `plugin_manager_scheduler.cpp` / `plugin_manager_adapters.cpp`：适配新 scheduler/tasks ABI、填充 `struct_size`、GraphTypeSlot 注册与冲突拒绝。
- 测试：`agent/test/plugin/test_plugin_runtime.cpp` +249 行（1000 并发完成、done/cancel 竞速、executor 停止重放、exactly-once、`ioCallSync` 快速失败、stop 未完成时禁止 destroy/dlclose、client 侧镜像守卫）。

### 4.2 R2 加载事务 / 关闭 / owner 顺序

- 生命周期入口：`AgentxxPluginStartFn/StopFn`、`agentxx_plugin_agent_start/stop`、`agentxx_plugin_client_start/stop`、内置插件 `start/stop` 字段；`plugin_manager_lifecycle.cpp` 与 `client_plugin_manager.cpp` 在 create 后调 start、Closing 时调 stop；`lifecycleStarted` 语义为“实例已激活”（无 start 导出的 legacy 插件在 Ready 前置位）。
- 名称预占：`reservePluginName()/releasePluginName()/isPluginNameLoading()`（`plugin_manager_base.h`），重复加载在 Loading 阶段被拒绝。
- 关闭：`unloadAsyncUntil()` 共享绝对 deadline；agent/client `shutdownAsync()`；失败置 `CloseFailed` 且可重试；同步 `shutdownAll()`/idle cleanup/实例析构在 stop 未完成或 lease 非零时不再 destroy/dlclose（保留 ctx 与 DSO 并报错）。
- owner 顺序：`BaseAgent::shutdownAsync()`（`base_agent.cpp:1112`，投递到插件管理器自己的 executor）、`AgentContext::shutdownPluginsAsync()` 与析构自检、`AgentHost::destroyAgentAsync()`（`agent_host.cpp:975`，先子后父）与同步 `destroyAgent` 告警、`FfiAgentRuntime::stopInternal()` 在停止 agent ioCtx 前 await 关闭（`ffi_runtime.cpp:540`）、`mode_runners.cpp` 本地 CLI/TUI 退出前 `shutdownAgentPlugins()` + client `shutdownAsync()`。
- Graph：`GraphTypeSlot` + `PluginGraphNode` 代次校验（`plugin_graph_node.{h,cpp}`），旧节点在卸载/重载后返回“插件已关闭/代次失效”，不调用插件回调。
- Client：`unloadAsyncUntil`/`shutdownAsync`/renderer lease/`dispatchEvent` 逐 callback 复查。

本轮发现的真实缺陷并已修复（见 `b2b5114a`）：`BaseAgent::shutdownAsync` 原先投递到 `ioCtx`，而宿主持有调用方 executor 直跑的子代理（engine 直跑）其自身 `ioCtx` 从未 `run()`，导致 `agent_host` 模块永久挂起；改为投递 `pluginManager->ioExecutor()` 后 `agent_host` 95/0 通过。

### 4.3 R3 ABI v1 / SDK（推进最多的一块）

- 接口表：全部 `_reserved` 改为 `struct_size`（`plugin_api.h`、`client_plugin_api.h`），宿主 vtable 填充 `sizeof(表)`，SDK 侧严格校验 `version == 1 && struct_size >= sizeof(Iface)`（`plugin_kit.h:356`）；插件 `get_info().api_version` 要求精确相等（agent/client 加载路径）。
- 取消：新增 opaque `AgentxxPluginCancelToken` + `agentxx_plugin_cancel_is_requested()`；删除 ABI 层的 `volatile int32_t* cancel_flag`。
- Scheduler v1：`post_to_io` 返回状态、`sleep` 返回 Operation handle、通用 `op_cancel`、`offload` 工作函数接收 CancelToken，删除 `pump_io` / `cancel_sleep`。
- Tasks：`register_task` 返回宿主托管 handle + `cancel_task`（handle 语义在 `3a4497ba` 已入库）；SDK `spawn` 在无 tasks 表时明确失败，不再 unmanaged 降级。
- SDK：scheduler/offload/sleep/yield/call/cap 迁移到新 ABI；`OffloadAwaiter` 用 `std::monostate` 支持 `offload<void>`；`Task` 子任务 continuation 已接通。

### 4.4 R4 内置插件 / 平台

- 已迁移到 CancelToken/新 offload 签名：`agentxx_filesystem`、`agentxx_codegraph`、`agentxx_execute_command`、`agentxx_system_monitor`。
- 平台构建修复已入库（见 3.2 节）；JS 系列、example_*、websearch/rag/planning 等尚未迁移。

### 4.5 R5 Client / 依赖

- `dispatchEvent`：快照后逐 callback 复查 `alive`/`enabled`/lifetime 并持 lease（同轮退订安全）。
- renderer：`ClientToolRendererLease`（alive + weak instance），UI 路径调用前复查 lease/实例状态。
- 动作派发：命中绑定前复查插件存在/启用/绑定快照一致。
- `blockedByDependencies` 字段落地，disable 级联标记、enable 清除。

### 4.6 测试

- `test_plugin_runtime.cpp`：109 → 128（`b2b5114a` 最新）
- `test_client_plugins.cpp`：300 → 309
- 其余模块数量见第 7 节。

---

## 5. 已完成任务对照（plugin.md R1-R6 / F、P 编号）

| 编号 / 条目 | 状态 | 位置 / 证据 |
|---|---|---|
| F01 互调 start 拒绝不回调、登记回滚 | 完成 | `op_driver.h` + `test_plugin_runtime` 拒绝用例 |
| F05 `shutdownAll` 不等待后台任务 | 完成 | `InstanceLifetime` lease + idle 事件 + `shutdownAsync`；R1-2 用例 8 覆盖 shutdown 中挂起后台 Task 的 cancel→resume→done→destroy 顺序（第 3.13 节） |
| F06 互调 start 在 IO 线程 | 完成 | `plugin_manager_capability.cpp` + `postToIo`/`ioCallSync` |
| F07 caller/provider 双 lease | 完成 | `OpCore` 的 provider_/caller_ guard |
| F08 sleep/post/offload 纳入 Operation 与回收 | 完成 | `plugin_manager_scheduler.cpp` + 测试 |
| F10 工具冲突不得写入实例记录 | 完成 | `plugin_manager_adapters.cpp` registerTool 返回值为唯一依据 |
| F13 SDK 借用参数跨挂起 | 完成 | `detail::RootRequest` 拥有 args/session/call_id/method；tool/hook/capability 统一使用；SDK 模块用例用失效借用缓冲区验证（第 3.7 节） |
| F19 hook helper 返回类型分发 | 完成 | `detail::invokeHook` + `if constexpr` 区分同步 void 与 `Task<T>`；异步钩子返回 provider 句柄并可取消（第 3.7 节） |
| F14 完成后再 cancel 不调用插件 | 完成 | `handle->completed` + `cancelFn` 失效 |
| F15 管理器销毁后队列 lambda 不访问裸 this | 完成 | 业务投递与 vtable 投递都持有 `shared_ptr<Instance/Manager>` + admission lease（P0-1 落地），不再捕获裸 `inst`/`mgr` |
| F18 Client 同轮派发复查 alive | 完成 | `client_plugin_manager.cpp:1511` dispatchEvent |
| P0 完成协议（拥有完成包、IO 线程一次性提交） | 完成 | `op_driver.h` commit 路径 + 完成端点 tombstone |
| P0 opaque CancelToken | 完成 | `plugin_api.h` + SDK 调用方 |
| P1-A 接口表严格协商 | agent/client 接口表完成 | `struct_size` 填充 + SDK 校验 + `api_version` 精确匹配 |
| P1-B 名称预占 / Loading 不可调用 | 完成 | `reservePluginName` + state 门禁；P0-1 补齐注册类入口的执行期复查 |
| P1-C GraphTypeSlot | 完成 | `plugin_graph_node.h` + `plugin_manager_adapters.cpp`；R1-2 用例覆盖旧节点代次失效、卸载后安全失败与同名重载不转交新实例（第 3.13 节） |
| P0-1 宿主控制块 / 迟到调用安全失败 / 注册执行期复查 | 完成 | `PluginHostControl`（`plugin_manager_base.h`）+ `hostView()`/`retireHostControl()` + `enterPluginHost()` + `ioCallSyncKeep`；回归见 `test_plugin_runtime` 新增 3 组用例（第 3.4、7 节） |
| P0-2 cancel/done 线性化协议 | 完成 | `op_driver.h` 普通 mutex + 契约注释；32 轮并发竞速用例（第 3.5 节） |
| P0-2 完成投递失败的可观察终态 | 完成 | `OpCore::completionPending()` + `PluginRuntime::pendingOperationSummary()`；CloseFailed 日志输出阻塞操作（第 3.5 节） |
| P0-2 裸 handle 失效语义 | 完成 | `AgentxxPluginOperatorHandle` tombstone（`completed` 置位后 cancel 空操作）+ 终态后重复 cancel 用例 |
| P1-2 C17 ABI 编译期检查 | 完成 | 新增 `test_plugin_abi_c17.c`（`-std=c17 -pedantic-errors`）+ C/C++ 17 项布局对照（第 3.6 节） |
| P1-2 接口表严格协商回归 | 完成 | 伪装宿主用例：version/struct_size/NULL 表必须被 SDK 拒绝（第 3.6 节） |
| P1-4 enable/disable start-stop 事务（agent 侧） | 完成 | `disableImpl`/`enableImpl` + `stopForDisable`/`startForEnable` + start 失败回滚；回归见第 3.8 节 |
| P1-4 start/stop 事务不累积重复注册 | 完成 | stop 成功后 `clearPluginOwnedRegistrations`；3 轮 enable/disable 断言记录数不变（第 3.8 节） |
| F09 多级依赖禁用/恢复不对称 | agent 侧完成 | 递归级联禁用/恢复 + `userDisabled`/`blockedByDependencies` 区分；三级 + 菱形用例（第 3.8 节） |
| F20 prompt 备份恢复覆盖其他 owner | 完成 | owner+sequence 贡献模型 + 基础值 rebase（第 3.8 节） |
| P1-4 生命周期 Operation 状态门禁 | 完成 | `OpCore::create` 只对业务操作检查 `enabled`/可注册状态，生命周期操作放行 Closing、拒绝 Closed |
| R2 owner 顺序（BaseAgent / AgentHost / Client runner） | 完成 | 见 4.2 节 |
| R2 Client semantic renderer cache | 完成 | `ClientToolRenderCache` + `requestToolRender`/`performToolRender`：插件 renderer 只在 client io 线程执行，UI 只读宿主语义快照（第 3.9 节） |
| F21 JS rejection 当成成功文本 | 完成（终态映射） | `PromiseOutcome` + 工具终态映射（第 3.11 节）；`callTool` 的 Promise 化仍未做 |
| R6 设计文档更新 | 完成 | `docs/zh-cn/design/plugins.md` 第 15 节 Reset-v1 契约 + 第 2/4/9 节修订（第 3.11 节） |
| P2-1a example_plugin 双端 start/stop 迁移 | 完成 | 第 3.10 节（真实 DSO 走通 create/start/stop/destroy 四条路径） |
| P2-1a SDK client 生命周期导出宏 | 完成 | `AGENTXX_PLUGIN_CLIENT_LIFECYCLE_EXPORT`（第 3.10 节） |
| R4-1 example_resources / example_graph_node 迁移 | 完成 | create/start 拆分 + 注册事务 + destroy 不调用宿主注册接口（第 3.14 节） |
| R4-1 websearch/rag/string CancelToken 接入 | 完成 | offload 工作函数接收 CancelToken，边界取消映射 CANCELLED（第 3.14 节） |
| R4-1 codegraph 后台 warmup 托管化 | 完成 | `std::thread` → `ctx.spawn(sleep → offload(updateIndex))`，卸载可取消且 lease 覆盖索引代码（第 3.14 节） |
| R4-1 system_monitor 双实例专项 | 完成 | `plugin_multi_instance` 新增采样任务隔离/卸载互不影响用例（第 3.14 节） |
| R6 导出符号白名单检查 | 完成 | `agent/script/check_plugin_exports.sh`；16 个插件库只导出入口符号（第 3.10、7.7 节） |
| R2/R5 client 侧 enable/disable 事务与依赖级联 | 完成 | `stopForDisable`/`startForEnable`/`restoreHostSideUiRegistrations` + 递归级联（第 3.9 节） |
| F03 旧 Client UI 快照调用已销毁 renderer | 完成 | 缓存条目按插件失效 + 版本号递增；旧快照只能回退通用渲染（第 3.9 节用例） |
| R5 动作代次 | 完成 | `ClientUiRegistry::instanceGenerations` + `dispatchAction(..., generation)` 复查（第 3.9 节用例） |
| F02/F03/F04/F16/F17/F21、P0-C | 未完成或仅部分 | 见第 6、8 节（F19/F20 已完成，见第 3.7、3.8 节） |
| R6 验证 / 文档 | 部分完成 | 导出符号白名单脚本已落地并全绿（第 3.10 / 7.7 节）；仍缺全模块 UBSan/TSan/Windows 验证与 `docs/zh-cn/design/plugins.md` 更新 |

---

## 6. 待完成任务（按优先级，含验收要求）

### P0-1 宿主控制块：让迟到插件调用安全失败（R2/R3 交叉）—— 已完成

已于第 3.4 节的提交落地（详见该节）。遗留的收尾项（下一阶段可做）：

- `PluginHostControl` 目前按实例累计、永不回收（tombstone 语义所需）。若将来出现
  "进程内反复加载/卸载上万次"的真实场景，需要在**保证地址不复用**的前提下评估
  池化或压缩（不要为了省内存改为复用地址）。
- `host.opaque` 现在是控制块地址；SDK 侧（`plugin_kit.h`）仍把它当作不透明值透传，
  没有额外断言。若后续给 ABI 加"令牌合法性"自检，需同步更新该处文档。

### P0-2 Operation 终态与取消线性化（R1 收尾）—— 已完成

已于第 3.5 节的提交落地（契约注释、可观察终态、竞速回归）。遗留：

- 验收要求 plugin.md 第 11.2 节 1-10 全部自动化。R1-2 提交已补齐 5（caller 卸载与
  provider 未完成互调）、8（shutdown 中挂起后台 Task 的
  cancel→resume→done→destroy 顺序）、9（timeout 后立即重试 unload）的独立用例（第 3.13 节）；
  1-4/6/7/10 的既有覆盖见第 3.5 节。
- 关闭超时目前只输出未终结 Operation 摘要（`label#id`）。若要更强的取证，可在
  摘要里带上每个 Operation 的 `completionPending()` 标记与等待时长。

### P1-1 SDK Request 与统一 root adapter（R3）—— 主体完成

已于第 3.7 节的提交落地：拥有型 `RootRequest`（F13）、`CompletionGuard`
（exactly-once），hook 的同步/异步严格分发（F19），并顺带修掉 `OpCtl` 栈引用悬垂。

遗留：

- capability 目前仍是同步业务（返回 `Task<T>` 的能力尚未支持）；graph node 的
  run_start 由各插件自行实现，未纳入 SDK root adapter。
- `Task<T>`/`Task<void>`/`offload<void>` 组合、异常与取消传播的**正反例编译测试**
  （错误签名必须编译失败）需要"预期编译失败"机制，尚未建立。

### P1-2 C ABI v1 编译期与运行期检查（R3/R6）—— 大体完成

已于第 3.6 节的提交落地：

- C17 `-pedantic-errors` 包含两个 ABI 头：`sizeof`/`offsetof`/对齐/调用约定断言（完成）。
- 未知/短接口表、NULL 表的安全拒绝（完成）；API 版本不匹配的加载期拒绝已有用例
  （`b2b5114a` 的“api_version 精确相等”路径）。

遗留：

- C++ 反例编译测试（错误 hook 返回类型、跨边界传 STL/协程句柄必须编译失败）——
  需要“预期编译失败”的测试机制（脚本或 CMake 试验性编译）。
- 导出符号白名单检查（第三方静态依赖符号必须隐藏）——需要 `nm`/`objdump` 脚本。

### P1-3 Client semantic renderer cache 与动作代次（R2/R5）—— 已完成

已于第 3.9 节的提交落地：

- renderer 结果改为 client io 线程计算的宿主语义快照（displayName/summary/items +
  plugin/generation + 输入特征），UI 只读 cache；旧 snapshot 失效直接通用回退（F03）。
- 动作点击携带 plugin/owner/generation，io 线程复查后决定丢弃或派发（同名重载后
  旧点击不转交新实例）。
- 回归：旧 renderer snapshot、旧 action 点击、重载同名插件、输入变化重渲染。

遗留：

- 语义缓存无容量上限（会话切换清空，但同一会话内按 tool_call_id 持续增长）。
- `onToolRenderUpdated` 触发整表重绘，未做按消息块精确失效。
- 旧快照用例在 manager 层驱动 `renderClientTool`，未覆盖 TUI 组件级路径。

### P1-4 注册事务与启停事务（R2/R5）—— 已完成（agent 侧）

已于第 3.8 节的提交落地：

- `enable/disable` 改为 start/stop 事务（agent 侧）；legacy 插件保留宿主侧记录恢复。
- prompt contribution 改为 owner + sequence + key 合成（F20）；卸载/禁用只删除本 owner
  的贡献并 rebase 基础值，不覆盖其他 owner 与用户后续写入。
- 依赖：三级/菱形禁用-恢复、userDisabled 与 blockedByDependencies 区分（F09）。
- start 失败回滚本次已生效的部分注册；关闭流程拒绝启用状态变化。

遗留：

- client 侧 enable/disable 仍是"重新注册 UI 记录"的旧模型（不触发插件 stop，
  也没有依赖级联恢复）——归入 P1-3 同一文件处理。
- 加载期 start 失败的端到端回滚（真实 DSO：工具/hook/能力/资源/prompt/graph/UI
  残留都要断言为空）需要新增测试用插件动态库，尚未建立。
- `create` 失败与 `start` 失败目前共用同一回滚出口；`plugin.md` 第 7.2 节要求
  的回滚顺序（先取消已接受 Operation → 再撤销注册 → stop → destroy → 摘除 → dlclose）
  已有实现，但缺少对"回滚后能再次加载同名插件"的显式用例。

### P2-1 R4 插件迁移收尾

按 plugin.md 第 9 节顺序：example_plugin / example_resources / example_graph_node
（已完成）→ string/math/system（CancelToken 校准进行中）→ websearch/rag/planning
（CancelToken 已完成）→ system_monitor/codegraph 多实例
（system_monitor 双实例与 codegraph 后台任务托管已完成）→ JS 剩余（`callTool` Promise、
脚本顶层异常事务、`hookStart` 真实完成）→ Windows 平台 gate（需 Windows 工具链）。
其余内置插件（filesystem/execute_command/planning/system_monitor 等）尚未迁移为
start/stop 导出，属 R4 收尾剩余。

### P2-2 R6 验证与文档

- 全模块回归、ASan/UBSan/TSan 定向、Windows 编译与专项、多实例矩阵。
- 更新 `docs/zh-cn/design/plugins.md` 与测试说明。
- 明确“已验证平台”，不得用 Linux 结果替代 Windows/Android。

---

## 7. 验证记录（最新：P0-2 提交；P0-1 见 7.1，`b2b5114a` 见 7.3）

构建：

```bash
cmake --build agent/build/linux-debug --target agentxx_test_repo -j12
# 成功（GCC 16.1.0，Linux x86_64 / WSL）；二进制 agent/build/linux-debug/exec/agentxx_test
# 比最新源码更新，提交后复跑为 up-to-date
```

### 7.1 P0-1 提交的回归（ASan + LSan 开启，`--fail-fast`）

```bash
ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 timeout 1500s \
  agent/build/linux-debug/exec/agentxx_test \
  ffi_c_api agent_host subagent_tool subagent_bus plugin_runtime plugins \
  plugin_resources plugin_multi_instance client_plugins agent memgrowth --fail-fast
```

```text
ffi_c_api             117 passed / 0 failed
plugin_runtime        151 passed / 0 failed     # 128 -> 151（P0-1 新增 23 项断言用例）
subagent_bus           21 passed / 0 failed
subagent_tool         122 passed / 0 failed
agent_host             95 passed / 0 failed
plugins               328 passed / 0 failed
plugin_resources       83 passed / 0 failed
plugin_multi_instance  29 passed / 0 failed
client_plugins        309 passed / 0 failed
agent                  91 passed / 0 failed
memgrowth              15 passed / 0 failed
合计                 1361 passed / 0 failed
```

插件专项（同一提交，另一次运行）：

```text
plugin_runtime plugins plugin_resources plugin_multi_instance client_plugins
合计 900 passed / 0 failed（改动后第三次复跑，含 sed 清理行内空格后的重编译）
```

无 ASan/LSan 报告；`git diff --check` 通过。日志：

```text
/tmp/agentxx-p01-sweep-1.log   # 1361/0（P0-1 第一次全量扩展回归）
/tmp/agentxx-p01-sweep-2.log   # 1361/0（脚本重建源码后复跑，结果一致）
/tmp/agentxx-p01-sweep-3.log   # 900/0（插件专项）
/tmp/agentxx-build-baseline.log  # 基线构建（up-to-date）
```

### 7.2 P0-2 提交的回归

```bash
ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 timeout 1500s \
  agent/build/linux-debug/exec/agentxx_test \
  ffi_c_api agent_host subagent_tool subagent_bus plugin_runtime plugins \
  plugin_resources plugin_multi_instance client_plugins agent memgrowth --fail-fast
```

```text
ffi_c_api             117 passed / 0 failed
plugin_runtime        415 passed / 0 failed     # 151 -> 415（P0-2 竞速/重放断言）
subagent_bus           21 passed / 0 failed
subagent_tool         122 passed / 0 failed
agent_host             95 passed / 0 failed
plugins               328 passed / 0 failed
plugin_resources       83 passed / 0 failed
plugin_multi_instance  29 passed / 0 failed
client_plugins        309 passed / 0 failed
agent                  91 passed / 0 failed
memgrowth              15 passed / 0 failed
合计                 1625 passed / 0 failed
```

插件专项（同提交）：`plugin_runtime plugins plugin_resources plugin_multi_instance
client_plugins` 合计 1164 passed / 0 failed。日志：

```text
/tmp/agentxx-p02-sweep-1.log   # 413/1（发现 provider/caller lease 计数断言写错）
/tmp/agentxx-p02-sweep-2.log   # 1164/0（修正断言后插件专项）
/tmp/agentxx-p02-sweep-3.log   # 1625/0（扩展回归）
```

构建期间遇到一次 GCC 16.1 ICE（`agent/client/src/io/tui/tui_log_sink.cpp`），
按 AGENTS.md 的约定重跑同一构建即通过（未改动该文件）。

### 7.3 P1-2 提交的回归

```bash
# 构建（C17 检查翻译单元按 test/CMakeLists.txt 的按文件选项编译）：
#   /usr/local/bin/cc ... -std=c17 -pedantic-errors -Wall -Wextra -c test_plugin_abi_c17.c
cmake --build agent/build/linux-debug --target agentxx_test_repo -j12
ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 timeout 1500s \
  agent/build/linux-debug/exec/agentxx_test \
  ffi_c_api agent_host subagent_tool subagent_bus plugin_runtime plugins \
  plugin_resources plugin_multi_instance client_plugins agent memgrowth --fail-fast
```

```text
ffi_c_api             117 passed / 0 failed
plugin_runtime        439 passed / 0 failed     # 415 -> 439（ABI 对照 19 + 协商 5）
subagent_bus           21 passed / 0 failed
subagent_tool         122 passed / 0 failed
agent_host             95 passed / 0 failed
plugins               328 passed / 0 failed
plugin_resources       83 passed / 0 failed
plugin_multi_instance  29 passed / 0 failed
client_plugins        309 passed / 0 failed
agent                  91 passed / 0 failed
memgrowth              15 passed / 0 failed
合计                 1649 passed / 0 failed
```

插件专项（同提交）：`plugin_runtime plugins plugin_resources plugin_multi_instance
client_plugins` 合计 1188 passed / 0 failed。日志：

```text
/tmp/agentxx-p12-sweep-1.log   # 1649/0（扩展回归）
```

### 7.4 P1-1 提交的回归

```bash
cmake --build agent/build/linux-debug --target agentxx_test_repo -j12
ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 timeout 1500s \
  agent/build/linux-debug/exec/agentxx_test \
  ffi_c_api agent_host subagent_tool subagent_bus plugin_sdk plugin_runtime plugins \
  plugin_resources plugin_multi_instance client_plugins agent memgrowth --fail-fast
```

```text
ffi_c_api             117 passed / 0 failed
plugin_sdk             29 passed / 0 failed     # 新增模块（F13/F19/完成协议）
plugin_runtime        439 passed / 0 failed
subagent_bus           21 passed / 0 failed
subagent_tool         122 passed / 0 failed
agent_host             95 passed / 0 failed
plugins               328 passed / 0 failed
plugin_resources       83 passed / 0 failed
plugin_multi_instance  29 passed / 0 failed
client_plugins        309 passed / 0 failed
agent                  91 passed / 0 failed
memgrowth              15 passed / 0 failed
合计                 1678 passed / 0 failed
```

日志：`/tmp/agentxx-p11-sweep-1.log`。本阶段还暴露并修复了两个环境问题：

1. 编译期 GCC ICE 会在 build 目录留下**残缺目标文件**（本次是
   `agentxx_obj.dir/src/agent/agent_runner.cpp.o` 的 `.debug_info` 重定位损坏），
   之后 mold/lld 一律在链接 `agentxx_cli` 时 SIGSEGV，只有 `-fuse-ld=bfd` 会报出
   “reloc against `.debug_str': error 4”。删掉该 .o 重新编译即恢复（已记录到第 8 节）。
2. `libneograph_llm.a` / `libneograph_mcp_types.a` 中存在 0 字节成员
   （`openai_provider.cpp.o`、`types.cpp.o`，来自更早的构建）。当前链接不需要这些
   成员，暂未处理；若将来出现 `neither ET_REL nor LLVM bitcode` 警告，需重建该库。

### 7.5 P1-4 提交的回归

```bash
cmake --build agent/build/linux-debug --target agentxx_test_repo -j12
# 构建期间出现一次 mold 链接 SIGSEGV（GCC 16.1 ICE 后的残缺 .o / 链接器偶发），
# 直接重跑同一构建目录即成功；未清理 build、未改动第三方目录。
ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 timeout 1500s \
  agent/build/linux-debug/exec/agentxx_test \
  ffi_c_api agent_host subagent_tool subagent_bus plugin_sdk plugin_runtime plugins \
  plugin_resources plugin_multi_instance client_plugins agent memgrowth --fail-fast
```

```text
ffi_c_api             117 passed / 0 failed
plugin_runtime        526 passed / 0 failed     # P1-4 新增 69 断言（原 457）
plugin_sdk             29 passed / 0 failed
subagent_bus           21 passed / 0 failed
subagent_tool         122 passed / 0 failed
agent_host             95 passed / 0 failed
plugins               328 passed / 0 failed
plugin_resources       83 passed / 0 failed
plugin_multi_instance  29 passed / 0 failed
client_plugins        309 passed / 0 failed
agent                  91 passed / 0 failed
memgrowth              15 passed / 0 failed
合计                 1765 passed / 0 failed   （exit=0）
```

插件专项（同提交）：`plugin_runtime plugin_sdk plugins plugin_resources
plugin_multi_instance client_plugins --fail-fast` = 526+29+328+83+29+309
= 1304 passed / 0 failed。日志：

```text
/tmp/p14-sweep-1.log     # 1759/0（新增用例最后一次编辑前）
/tmp/p14-sweep-2.log     # 1765/0（当前代码，文档记录以此为准）
/tmp/p14-run1.log        # 插件专项（无 ASAN_OPTIONS 覆盖；末尾 LSan 报
                         #  2800 bytes in 40 allocations，与 P1-1 提交完全一致，
                         #  来源是插件 DSO 内部（<unknown module>）的间接泄漏，非本次回归引入）
```

### 7.6 P1-3 提交的回归

```bash
cmake --build agent/build/linux-debug --target agentxx_test_repo -j12
cmake --build agent/build/linux-debug --target agentxx_client_repo -j12   # TUI 接线
ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 timeout 1500s \
  agent/build/linux-debug/exec/agentxx_test \
  ffi_c_api agent_host subagent_tool subagent_bus plugin_sdk plugin_runtime plugins \
  plugin_resources plugin_multi_instance client_plugins agent memgrowth --fail-fast
```

```text
ffi_c_api             117 passed / 0 failed
plugin_runtime        526 passed / 0 failed
plugin_sdk             29 passed / 0 failed
subagent_bus           21 passed / 0 failed
subagent_tool         122 passed / 0 failed
agent_host             95 passed / 0 failed
plugins               328 passed / 0 failed
plugin_resources       83 passed / 0 failed
plugin_multi_instance  29 passed / 0 failed
client_plugins        374 passed / 0 failed     # P1-3 新增 65 断言（原 309）
agent                  91 passed / 0 failed
memgrowth              15 passed / 0 failed
合计                 1830 passed / 0 failed   （exit=0）
```

插件/客户端专项：`plugin_runtime plugin_sdk plugins plugin_resources
plugin_multi_instance client_plugins --fail-fast` = 526+29+328+83+29+374
= 1369 passed / 0 failed。TUI 接线通过 `agentxx_cli` 全量重链接验证（无编译/链接错误）；
未做人工 TUI 会话走查（无显示环境），语义渲染路径由 `client_plugins` 用例覆盖。
日志：`/tmp/p13-sweep-1.log`。

### 7.7 P2-1a 提交的回归

```bash
cmake --build agent/build/linux-debug --target agentxx_test_repo -j12
cmake --build agent/build/linux-debug --target agentxx_client_repo -j12
./agent/script/check_plugin_exports.sh        # 新增: 导出符号白名单
ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 timeout 1500s \
  agent/build/linux-debug/exec/agentxx_test \
  ffi_c_api agent_host subagent_tool subagent_bus plugin_sdk plugin_runtime plugins \
  plugin_resources plugin_multi_instance client_plugins agent memgrowth --fail-fast
```

```text
ffi_c_api             117 passed / 0 failed
plugin_runtime        526 passed / 0 failed
plugin_sdk             29 passed / 0 failed
subagent_bus           21 passed / 0 failed
subagent_tool         122 passed / 0 failed
agent_host             95 passed / 0 failed
plugins               328 passed / 0 failed
plugin_resources       83 passed / 0 failed
plugin_multi_instance  29 passed / 0 failed
client_plugins        375 passed / 0 failed
agent                  91 passed / 0 failed
memgrowth              15 passed / 0 failed
合计                 1831 passed / 0 failed   （exit=0）
```

导出符号检查：

```text
[check_plugin_exports] OK: 16 plugin libraries export only entry symbols
```

日志：`/tmp/p21-sweep-1.log`。

### 7.8 P2-1b 提交的回归

```bash
cmake --build agent/build/linux-debug --target agentxx_test_repo -j12
# 注意: builtin 模式下 example_js/plugin.js 的拷贝挂在动态库 POST_BUILD 上,
# 只改脚本不会触发拷贝; 本次用 touch 源码 + 重建 example_js 强制刷新资源
ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 timeout 1500s \
  agent/build/linux-debug/exec/agentxx_test \
  ffi_c_api agent_host subagent_tool subagent_bus plugin_sdk plugin_runtime plugins \
  plugin_resources plugin_multi_instance client_plugins agent memgrowth --fail-fast
```

```text
ffi_c_api             117 passed / 0 failed
plugin_runtime        526 passed / 0 failed
plugin_sdk             29 passed / 0 failed
subagent_bus           21 passed / 0 failed
subagent_tool         122 passed / 0 failed
agent_host             95 passed / 0 failed
plugins               331 passed / 0 failed     # P2-1b 新增 3 断言（拒绝→FAILED）
plugin_resources       83 passed / 0 failed
plugin_multi_instance  29 passed / 0 failed
client_plugins        375 passed / 0 failed
agent                  91 passed / 0 failed
memgrowth              15 passed / 0 failed
合计                 1834 passed / 0 failed   （exit=0）
```

调试过程中确认的失败模式（保留供后续参考）：`drivePromise` 执行定时器后若不立即重跑
QuickJS job，等待谓词会落到下一个定时器（本机实测为 30s 的超时守卫），表现为
`promise not settled within 120000ms`；修复点是 `fireDueTimersInline()` 返回已执行数量并
在 > 0 时 `continue`。日志：`/tmp/p21b-sweep-1.log`（失败排查过程在
`/tmp/js-fail*.log`）。

### 7.9 P2-1c 提交的回归

```bash
cmake --build agent/build/linux-debug --target agentxx_test_repo -j12
agent/build/linux-debug/exec/agentxx_test plugins plugin_multi_instance ffi_c_api --fail-fast
ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 timeout 1500s \
  agent/build/linux-debug/exec/agentxx_test \
  ffi_c_api agent_host subagent_tool subagent_bus plugin_sdk plugin_runtime plugins \
  plugin_resources plugin_multi_instance client_plugins agent memgrowth --fail-fast
./agent/script/check_plugin_exports.sh
```

```text
合计 1834 passed / 0 failed（exit=0；同上表逐模块结果）
[check_plugin_exports] OK: 16 plugin libraries export only entry symbols
```

日志：`/tmp/p21c-sweep-1.log`。

### 7.11 R1-2 提交的回归

```bash
cmake --build agent/build/linux-debug --target agentxx_test_repo -j12
ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 timeout 1500s \
  agent/build/linux-debug/exec/agentxx_test \
  ffi_c_api agent_host subagent_tool subagent_bus plugin_sdk plugin_runtime plugins \
  plugin_resources plugin_multi_instance client_plugins agent memgrowth --fail-fast
```

```text
ffi_c_api             117 passed / 0 failed
plugin_runtime        600 passed / 0 failed     # R1-2 新增 74 断言（用例 5/8/9 + GraphTypeSlot）
plugin_sdk             29 passed / 0 failed
subagent_bus           21 passed / 0 failed
subagent_tool         122 passed / 0 failed
agent_host             95 passed / 0 failed
plugins               331 passed / 0 failed
plugin_resources       83 passed / 0 failed
plugin_multi_instance  29 passed / 0 failed
client_plugins        375 passed / 0 failed
agent                  91 passed / 0 failed
memgrowth              15 passed / 0 failed
合计                 1908 passed / 0 failed   （exit=0）
```

竞态修复复跑（ASan 关闭泄漏检测以缩短时间）：
`plugin_runtime plugin_sdk plugins plugin_resources plugin_multi_instance
client_plugins --fail-fast` 连续 3 次均为 1447 passed / 0 failed /
exit=0（修复前同一组合出现一次 `InstanceLifetime::setState` 断言 SIGABRT；
日志 `/tmp/p3-repro1.log`、`/tmp/p3-rerun{1,2,3}.log`）。扩展回归日志
`/tmp/p3-r1b-sweep.log`。

### 7.12 R4-1 提交的回归

```bash
cmake --build agent/build/linux-debug -j12          # 全量（含插件动态库）
ASAN_OPTIONS=detect_leaks=0 timeout 900s \
  agent/build/linux-debug/exec/agentxx_test \
  plugins plugin_resources plugin_multi_instance client_plugins codegraph --fail-fast
```

```text
plugins               331 passed / 0 failed
plugin_resources       83 passed / 0 failed
plugin_multi_instance  48 passed / 0 failed   # R4-1 新增 19 断言（system_monitor 双实例）
client_plugins        375 passed / 0 failed
codegraph              23 passed / 0 failed   # codegraph warmup 托管化后复跑
普通合计              860 passed / 0 failed   （exit=0）

另有扩展回归（ASan+LSan，R1-2 提交同批命令）：
合计                 1908 passed / 0 failed
```

日志：`/tmp/p3-r4c-tests.log`、`/tmp/p3-codegraph2.log`、`/tmp/p3-r1b-sweep.log`。

### 7.10 `b2b5114a` 的历史验证结果

以下结果测自 `b2b5114a`（同样为 ASan + LSan、`--fail-fast` 的扩展回归）：

```text
ffi_c_api 117 / plugin_runtime 128 / subagent_bus 21 / subagent_tool 122 /
agent_host 95 / plugins 328 / plugin_resources 83 / plugin_multi_instance 29 /
client_plugins 309 / agent 91 / memgrowth 15   合计 1338 passed / 0 failed
```

日志：

```text
/tmp/agentxx-reset-v1-sweep-2.log        # 上述 1338/0 总回归（交接前最后一次复跑）
/tmp/agentxx-reset-v1-sweep-1.log        # 同命令的一次更早运行，结果相同
/tmp/agentxx-reset-v1-dtor-asan-1.log    # 插件专项 ASan（877/0，BaseAgent 修复前）
/tmp/agentxx-reset-v1-dtor-client-1.log  # client 守卫回归
/tmp/agentxx-reset-v1-agenthost-2.log    # agent_host 95/0（BaseAgent executor 修复后）
```

未验证（不得当作已通过）：

- 全模块 `agentxx_test` 全量运行；UBSan/TSan；Windows/Android 构建与专项；
- C17 ABI layout、短表/NULL 表、导出符号检查；
- Client semantic renderer / 动作代次 / prompt contribution / 依赖恢复；
- `docs/zh-cn/design/plugins.md` 未更新。

---

## 8. 已知风险与遗留缺陷（开工前必读）

1. ~~host opaque / vtable 裸指针~~：P0-1 已解决（控制块 + 令牌 + 租约 + 注册执行期复查）。
   仍存在的是"控制块 tombstone 永不回收"的固定内存代价（每实例约 100 字节，见第 3.4 节）。
2. 同步析构兜底会保留 DSO：stop 未完成或 lease 非零时实例保持 `CloseFailed` 并保留 ctx/DSO（日志明确报错）。这是安全兜底而非最终形态，依赖 owner 先 await `shutdownAsync`。`AgentHost::destroyAgent`（同步）只告警不阻断，新代码应使用 `destroyAgentAsync`。
3. ~~renderer 在调用线程同步执行~~：P1-3 已改为 client io 线程计算语义快照 + UI 只读缓存
   （第 3.9 节）。仍存在的是缓存条目无容量上限（会话切换清空）。
4. ~~Operation 取消/终态未收敛~~：P0-2 已收敛（普通 mutex + 契约注释、`completionPending()`/
   `pendingOperationSummary()` 可观察终态、终态后 cancel 空操作）。仍缺的是 11.2 节
   第 5/8/9 条独立用例（见第 3.5、6 节）。
5. 注册/启停事务仍不完整（P1-4 已推进两步）：agent 侧 enable/disable 已改为 start/stop
   事务、prompt 已改为贡献模型、agent 侧依赖级联已递归化。仍缺：client 侧
   enable/disable 事务与依赖级联、加载期 start 失败的真实 DSO 回滚用例、
   UI/graph 注册记录的回滚断言（见第 3.8、6 节）。
6. ~~无 ABI 编译期检查~~：P1-2 已加入 C17 `-pedantic-errors` 编译期断言与 C/C++
   布局对照。仍未做的是 C++ 反例编译测试与导出符号白名单检查（见第 3.6、6 节）。
7. 构建环境脆弱点（本阶段实测）：GCC 16.1 偶发 ICE 后 build 目录可能残留残缺
   `.o`，链接器（mold/lld）会直接 SIGSEGV 而不是报错。排查手法：
   `bash <build>/.../link.txt` 换成 `-fuse-ld=bfd` 重跑，bfd 会给出具体坏目标文件；
   删掉该 `.o` 重新编译即可。不要为此清空整个 build 目录。

---

## 9. 下一步执行顺序（建议）

1. ~~基线复跑~~：确认 1834/0（P2-1c 提交状态）。
2. ~~P0-1 宿主控制块~~、~~P0-2 Operation 终态~~：已完成（第 3.4 / 3.5 节）。
   可选收尾：client 侧"旧 host 指针在卸载后安全失败"的专项用例；订阅句柄与
   GraphTypeSlot 旧节点的"代次失效 + 迟到调用"独立用例（机制已具备）：
   GraphTypeSlot 用例已在 R1-2 提交补齐（第 3.13 节），client 侧旧 host / 订阅句柄
   用例仍可选。
3. ~~plugin.md 第 11.2 节剩余用例 5/8/9~~：已在 R1-2 提交补齐（第 3.13 节）。
4. ~~P1-1 SDK Request + 统一 root adapter（F13/F19）~~：主体已完成（第 3.7 节）。
   遗留 capability 异步业务、graph node 纳入 adapter、正反例编译测试。
5. ~~P1-4 启用/禁用事务与 prompt 贡献模型~~：agent 侧已完成（第 3.8 节）。
6. ~~P1-3 Client 语义模型（renderer cache / 动作代次）+ client 侧 enable/disable 事务
   与依赖级联~~：已完成（第 3.9 节）。
7. P2-1a（example_plugin 双端 start/stop 迁移 + SDK client 生命周期宏 + 导出符号脚本）：
   已完成（第 3.10 节）。
8. ~~P2-1b（JS Promise 终态映射 + 设计文档 Reset-v1 章节）~~：已完成（第 3.11 节）。
9. P2-1 剩余 / P2-2 收尾：
   - ~~example_resources / example_graph_node 迁移为 start/stop 事务正例~~：
     已在 R4-1 提交落地（细节随 R4-1 提交补入本文档）；
   - string/math/system 校准 SDK 签名（string CancelToken 已完成，math/system 为
     fast_tool 无需改动）；~~websearch/rag/planning 接入 CancelToken~~（已完成）；
     ~~system_monitor/codegraph 多实例与后台采样专项~~（已完成：双实例用例 +
     codegraph warmup 托管化）；
   - JS 剩余：`callTool` 改为始终返回 Promise（完成/失败/取消事件回投 JS 线程 settle）、
     删除同步 `call_tool_blocking` 路径、脚本顶层异常的事务化回滚、`hookStart` 等真实完成；
   - Windows 平台 gate（screen_capture/computer_use/text_selection_monitor）：需 Windows 工具链；
   - 全模块 UBSan/TSan 定向回归（本轮已验证 ASan + LSan）。

每一步完成后：跑对应模块回归，更新本文件第 1、5、6、7 节，再提交。

---

## 10. 提交要求

- 提交信息使用 `重构插件框架-<修改内容总结>`。
- 阶段提交说明必须写明“Reset-v1 未完成”，不要把阶段成果写成整体完成。
- 新提交一律建立在 `b2b5114a` 之上；不要修改或回退 `TODOS.md`、`agentxx-config.yaml`、`index.md`
  这些已入库的既有改动，除非用户明确要求。
- 提交前：`git diff --check`、构建、相关模块回归；`git status` 确认只包含本任务文件。
- 建议的下一步提交边界（对应 plugin.md 第 10 节）：

```text
1. 重构插件框架-R1-2 Operation 终态与取消收敛
2. 重构插件框架-R2-1 宿主控制块与安全失败
3. 重构插件框架-R2-2 注册事务与异步关闭补全
4. 重构插件框架-R3-1 SDK Request 与统一 root adapter
5. 重构插件框架-R4-1 内置插件迁移
```
