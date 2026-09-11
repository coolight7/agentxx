# 插件框架 Reset-v1 重构交接文档（已完成 / 待完成）

> 事实来源：`resource/history/plugin-refactor-2/plugin.md`（Reset-v1 方案、R0-R6 阶段、
F/P 问题编号、测试矩阵）。本文件记录进度、提交边界、验证事实与待办；与 plugin.md
冲突时以 plugin.md 为准。
>
> **状态：Reset-v1 重构完成（2026-09-11）。** R0～R6 全部完成，plugin.md 第 12 节的
> 6 项完成标准全部满足（逐项勾选见 1.1）。关键提交：`91b345a9`（R6-3 Windows 平台验证
> 与平台 gate 收敛），其余提交见第 4 节。
> **已验证平台**：Windows（MSVC 14.51 / VS18 Debug + ASan）、Linux（GCC 16.1 Debug +
> ASan/LSan、定向 UBSan、定向 TSan）。
>
> **阅读顺序**：**第 1 节 = 已完成任务内容；第 2 节 = 待完成任务内容**；
> 第 3 节 = 验证记录；第 4 节 = 提交边界；第 5 节 = 已知风险；
> 第 6 节 = 下一步执行清单；第 7 节 = 提交与文档维护规范。
>
> **不在本任务跟踪范围**（故不列为待办）：仓库中非插件模块的 TSan 告警
> （FFI / HttpServer / 测试脚手架 / 未插桩三方库，另行立项）、Android 平台验证。

---

## 0. 新会话执行须知

### 0.1 第一步：确认状态

```bash
# Windows 侧（提交 29 及之后的提交在这一份副本上产生）
cd /d D:\0Acoolight\Program\cpp\agentxx
git status --short --branch
git log --oneline -8
git diff --stat && git diff --check
```

阅读顺序：

1. `plugin.md`（方案、R1-R6 验收标准、第 11 节测试矩阵、第 12 节完成标准）；
2. 本文件第 1 节（已完成，重点是 1.8 缺陷清单、1.10 Windows 平台验证）、
   第 2 节（待完成，非阻塞）、第 5 节（已知风险）；
3. `git show <提交号>` 按需查看；最新提交号以 `git log -1` 为准。

预期状态：工作树无代码/文档修改，只有构建副产物与用户既有改动（见 0.2）。

> **两个工作副本（重要）**：Windows 侧 `D:\0Acoolight\Program\cpp\agentxx` 与
> WSL2 侧 `/home/coolight/program/agentxx` 是同源的两份独立 clone
> （`origin = https://github.com/coolight7/agentxx.git`），**互不共享文件**，且提交
> 尚未推送。在任一副本继续开发前先确认 `git log -1` 是否已包含第 4 节列出的最新提交；
> 不在同一份副本上时需先同步（push / fetch + 合并），不要在两份副本上分别提交同一改动。

### 0.2 工作树保护规则（不得违反）

- 禁止 `git reset --hard`、`git checkout --`、清空 build 目录或批量删除测试。
- `TODOS.md`、`agentxx-config.yaml`、`resource/history/plugin-refactor-2/index.md`
  已随历史提交入库（含用户改动）：不要回退，也不要在重构提交中顺手修改。
- 若又出现用户新改动，保留它们并只提交本任务相关文件。
- 构建副产物不属于本任务，勿清理/勿回退：`agent/third_party/fmt`（untracked）、
  `agent/third_party/libiconv-native`、`agent/third_party/liburing`（submodule 脏标记）。

### 0.3 构建 / 测试 / 校验命令速查

Linux 侧（WSL2）：

```bash
# 1) 测试二进制构建（高并行；GCC 16.1 偶发 ICE 时直接重试同一构建目录，不要清空 build）
cmake --build agent/build/linux-debug --target agentxx_test_repo -j12

# 2) 插件动态库全量构建（改动 agent/plugins/ 下插件后必须执行）
cmake --build agent/build/linux-debug -j12

# 3) 插件专项回归
ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 timeout 900s \
  agent/build/linux-debug/exec/agentxx_test \
  plugin_runtime plugin_sdk plugins plugin_resources plugin_multi_instance \
  client_plugins cpu_gpu --fail-fast

# 4) 扩展回归（含 ffi / host / subagent / agent / memgrowth / codegraph；ASan + LSan）
ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 timeout 1500s \
  agent/build/linux-debug/exec/agentxx_test \
  ffi_c_api agent_host subagent_tool subagent_bus plugin_sdk plugin_runtime plugins \
  plugin_resources plugin_multi_instance client_plugins cpu_gpu agent memgrowth \
  codegraph --fail-fast

# 5) 定向 UBSan 探针（只插桩插件框架源与测试 TU，不重编第三方）
cmake -B agent/build/linux-debug -S agent -DAGENTXX_PLUGIN_UBSAN_PROBE=ON
cmake --build agent/build/linux-debug --target agentxx_test_repo -j12
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 \
  timeout 1500s agent/build/linux-debug/exec/agentxx_test \
  plugin_runtime plugin_sdk plugins plugin_resources plugin_multi_instance client_plugins --fail-fast
# 验证完恢复基线（OFF 后重建）
cmake -B agent/build/linux-debug -S agent -DAGENTXX_PLUGIN_UBSAN_PROBE=OFF

# 6) TSan 定向回归（独立构建目录; 与 ASan 基线互斥, 不动 linux-debug）
cmake -B agent/build/linux-tsan -S agent \
  -DBOOST_ROOT="$(pwd)/agent/third_party/boost-linux-build-debug" \
  -DOPENSSL_ROOT_DIR="$(pwd)/agent/third_party/OpenSSL-linux-build" \
  -DAGENTXX_BUILD_CLIENT=ON -DAGENTXX_BUILD_TEST=ON -DAGENTXX_BUILD_BENCHMARK=OFF \
  -DAGENTXX_ENABLE_HYPERSCAN=ON -DAGENTXX_ENABLE_BOOST_PROCESS=ON -DAGENTXX_ENABLE_PCH=OFF \
  -DXX_IS_RELEASE_D=0 -DCMAKE_BUILD_TYPE=Debug \
  -DAGENTXX_ENABLE_ASAN=OFF -DAGENTXX_ENABLE_TSAN=ON
cmake --build agent/build/linux-tsan --target agentxx_test_repo -j12
TSAN_OPTIONS=halt_on_error=0 second_deadlock_stack=1 timeout 1200s \
  agent/build/linux-tsan/exec/agentxx_test \
  plugin_runtime plugin_sdk plugins plugin_resources plugin_multi_instance \
  client_plugins cpu_gpu --fail-fast

# 7) 导出符号白名单（要求仅入口符号）
bash agent/script/check_plugin_exports.sh          # OK: 16 plugin libraries

# 8) SDK 反例编译检查（错误签名必须编译失败）
bash agent/script/check_sdk_negative_compile.sh    # OK: 5 snippets behave as expected
```

Windows 侧（MSVC / VS18 + ASan）：

```powershell
# 1) 构建（脚本 = 配置 + 构建 + install + 复制到 build/windows-debug-output）
.\agent\script\windows_debug_build.bat
# 增量单目标构建（改动 lib/plugins/test 后常用）
cmake --build agent/build/windows-debug --target agentxx_test_repo --config Debug --parallel 8
cmake --build agent/build/windows-debug --config Debug --parallel 8      # 含全部插件

# 2) 插件专项回归（含平台插件 screen_capture / text_selection）
agent\build\windows-debug\exec\agentxx_test.exe `
  plugin_runtime plugin_sdk plugins plugin_resources plugin_multi_instance `
  client_plugins cpu_gpu screen_capture text_selection --fail-fast

# 3) 扩展回归（16 模块）
agent\build\windows-debug\exec\agentxx_test.exe `
  ffi_c_api plugin_runtime plugin_sdk plugins plugin_resources plugin_multi_instance `
  client_plugins screen_capture cpu_gpu text_selection agent_host subagent_tool `
  subagent_bus agent memgrowth codegraph --fail-fast

# 4) 工具模块回归（含 execute_command 的 Windows 命令分支）
agent\build\windows-debug\exec\agentxx_test.exe `
  command filesystem string_tools math_tools web_search codegraph datetime rag_search --fail-fast
```

Windows 运行注意（本次实测）：

- **必须让工作目录 = 可执行文件所在目录**（`agent/build/windows-debug/exec`）：
  `agentxx_test` 用 `GetModuleFileNameW` 的父目录 + `plugins/<name>` 定位插件 DSO，
  工作目录不对会加载不到插件（表现为插件用例失败）；Linux 等价机制是 `/proc/self/exe`。
- 长跑用例（扩展回归约 185s）应以 `Start-Process -NoNewWindow -RedirectStandardOutput <log>`
  后台运行 + 轮询日志，避免撞上工具会话超时。
- Windows 没有 WSL 的 `timeout`/`ASAN_OPTIONS` 包装；MSVC ASan 经
  `clang_rt.asan_dynamic-x86_64.dll`（构建自动部署到 `exec/`）。
- `cmake --build ... --parallel 8` 偶发 `MSB6006: CL.exe 已退出` + `MSB4166`
  （无 `error C####`，属编译器进程被资源压力终止）：重跑即通过（见 5.7）。

### 0.4 最终判定基线

| 项目 | 结果 | 节号 |
|---|---|---|
| Linux 插件专项（7 模块，ASan + LSan） | **1706 passed / 0 failed**，LSan 4480 字节 / 64 处与重构前基线逐项一致 | 3.1 |
| Linux 扩展回归（14 模块，ASan + LSan） | **2179 passed / 0 failed** | 3.2 |
| Windows 插件专项（9 模块，2 轮） | **1765 passed / 0 failed** | 3.1 |
| Windows 扩展回归（16 模块） | **2251 passed / 0 failed** | 3.1 |
| Windows 工具模块回归 | **360 passed / 0 failed** | 3.1 |
| 定向 UBSan 探针 | 1670 passed / 0 failed，0 处 `runtime error` | 3.4 |
| 定向 TSan 回归（插件框架 7 模块） | 1695 passed / 0 failed，**0 条告警**（连续两轮） | 3.5 |
| 导出符号 / ABI / 反例编译 | 16 库全绿 / C17 布局检查 / 5 snippets 符合预期 | 3.1 |
| 已验证平台 | Windows（MSVC Debug + ASan）、Linux（Debug + ASan/LSan、定向 UBSan/TSan） | 3.1 |

结论：plugin.md 第 12 节的 6 项完成标准全部满足，状态已更新为"Reset-v1 重构完成"；
非阻塞待完成任务见第 2 节。

---

## 1. 已完成任务内容

### 1.1 R0～R6 总览与完成标准

| 阶段 | 状态 | 关键提交 | 验证 |
|---|---|---|---|
| R0 契约冻结 | 完成 | `plugin.md` 定稿 | 本文件只做进度记录 |
| R1 Runtime / Operation | 完成 | `3a4497ba`、`b2b5114a`、`c2869f07`、`8c717236`、`cdbcc738` | `plugin_runtime` 623/0（11.2 全 10 条覆盖） |
| R2 加载事务 / 注册事务 / 异步关闭 | 完成 | `b2b5114a`、`c2869f07`、`9be9c735`、`a321267c`、`cdbcc738`、`0bab72e3`、`e96f8f1b`、`d6ae39cd`、`75b01a56` | `plugin_runtime` 623、`plugins` 359、`plugin_resources` 83、`plugin_multi_instance` 48、`client_plugins` 421 |
| R3 ABI v1 / SDK | 完成 | `b2b5114a`、`aa4b33ff`、`4f8d1fdf`、`3e76a143`、`0bab72e3` | `plugin_sdk` 71、C17 ABI 检查、反例编译 5/5 |
| R4 内置插件 / JS / 平台 | 完成（JS 三件提交 27；Windows 平台专项提交 29） | `f861bcf9`、`dfe04a6a`、`ff6fc990`、`3143ef92`、`460d35f5`、`2a53977f`、`3e76a143`、`10cb3ae2`、`d3dbd909`、提交 28、提交 29 | `plugins` 392、`plugin_multi_instance` 80、`codegraph` 23、`cpu_gpu` 25；JS 见 1.5；system_monitor 实例化（提交 28）；**Windows 平台见 1.10/3.1** |
| R5 Client / 依赖 / prompt | 完成 | `9be9c735`、`a321267c`、`e96f8f1b`、`43c93ff6` | `client_plugins` 421、`plugin_runtime` 623 |
| R6 验证 / 文档 / 发布审查 | 完成 | `aa4b33ff`、`dfe04a6a`、`ff6fc990`、`0bab72e3`、`7a44e76d`、提交 28、提交 29 | Linux ASan/LSan 2179/0、UBSan 1670/0、TSan 插件框架 0 告警（3.5）、导出符号 16 库、**Windows 全插件构建 + 1765/0 + 2251/0 + 360/0（3.1）** |

plugin.md 第 12 节完成标准（6 项，逐项对应）：

| # | 完成标准 | 状态 | 证据 |
|---|---|---|---|
| 1 | 所有内置插件迁移（§9 第 4-8 步）与专项生命周期测试通过 | 完成 | 1.5（16 个内置插件 + 3 个 JS 系 + Windows 平台插件）、3.1 |
| 2 | TSan 结论落档（插件框架范围） | 完成 | 3.5（0 告警，连续两轮；3 处竞争已修复，见 1.8） |
| 3 | Windows 平台验证完成并写入结果 | 完成 | 1.10、3.1 |
| 4 | 全模块扩展回归 + 导出符号 + 反例编译 + UBSan 探针 | 完成 | 3.1、3.2、3.4（Linux 2179/0、Windows 2251/0、16 库、5/5、1670/0） |
| 5 | 明确"已验证平台"范围 | 完成 | 0.4、1.10（Windows / Linux；不在范围内：Android） |
| 6 | 非阻塞残余项在结论中列出 | 完成 | 第 2 节（依赖启用事务顺序等 4 项） |

### 1.2 R1 Runtime / Operation（已完成）

**核心设计落地**

- 新增 `agent/lib/include/agentxx/plugin/plugin_runtime.h`：
  `PluginInstanceState`（Loading/Ready/Disabled/Closing/Closed/CloseFailed）、
  `InstanceLifetime`（原子状态 + admission 位 + lease 计数 + `waitIdleUntil` 事件式
  空闲等待）、`InstanceLease`、`PluginRuntime`（executor、IO 线程标识、Operation 表、
  待重放 `RuntimeAction` 队列）。
- 重写 `agent/lib/include/agentxx/plugin/op_driver.h`：`OpCore` 统一
  start/done/cancel/句柄回收；caller/provider 双 lease；完成 payload 先复制进宿主
  拥有的完成包、再在 IO 线程一次性提交（exactly-once）；完成端点 tombstone 保证
  迟到 `done` 安全丢弃；取消/完成线性化协议（普通 mutex + 持锁期间不调用插件/
  调用方代码）；终态后 cancel 为空操作。
- 删除 `ioTasks_` 二级队列、捕获裸 `this` 的投递、sentinel/reaper 重复终态通道；
  `waitInflightZero` 改为 idle 事件等待；post/sleep/offload 全部纳入 Operation
  并返回宿主托管句柄。
- 完成投递失败可观察：`OpCore::completionPending()` +
  `PluginRuntime::pendingOperationSummary()`（卸载超时日志输出阻塞的 Operation，
  含 `(completion-pending)` 标记）。

**提交**：`3a4497ba`（R1-1 主体）、`b2b5114a`（可靠性收尾）、`c2869f07`（P0-1
vtable 投递纳入 admission lease）、`8c717236`（P0-2 终态与取消线性化）、
`cdbcc738`（R1-2 回归补全 + 竞态修复）。

**验证**：`plugin_runtime` 模块 623 断言（Windows 修复后 634）；plugin.md 第 11.2 节
1-10 条全部覆盖（1-4/6/7 见 P0-2 竞速/重放用例；5/8/9 见 R1-2 用例；
10 见 R3-2/R2-4/R2-5/R4-3 的注册回滚用例）。

### 1.3 R2 加载事务 / 注册事务 / 异步关闭（已完成）

**实例生命周期**

- `Loading/Ready/Disabled/Closing/Closed/CloseFailed` 状态机；名称预占
  `reservePluginName/releasePluginName`（Loading 期重复加载被拒绝）。
- `create → start → Ready`、`Ready ⇄ Disabled`、`Closing → Closed`、
  `Closing → CloseFailed → Closing`（可重试）；`start` 失败返回 `NULL + error`
  由宿主回滚并撤销实例。
- `stop` 在停用/关闭时调用；stop 未完成或 lease 非零时同步析构/`shutdownAll`
  不 destroy/dlclose（保留 ctx 与 DSO、日志报错，安全兜底）。
- owner 顺序：`BaseAgent::shutdownAsync` → `AgentContext` 析构自检 →
  `AgentHost::destroyAgentAsync`（先子后父）→ `FfiAgentRuntime` 停 ioCtx 前 await →
  `mode_runners` 本地 CLI/TUI 退出前 `shutdownAgentPlugins()` + client `shutdownAsync`。
- `unloadAsyncUntil` 共享绝对 deadline；agent/client 两侧 `shutdownAsync`。

**宿主控制块（P0-1）**

- `PluginHostControl`：交给插件的 `AgentxxPluginHost` 视图使用进程级稳定地址
  （`host.opaque` = 控制块地址，作为一次性令牌），只持有 `weak_ptr` 实例；
  实例销毁后退休控制块，迟到调用全部安全失败（非 0 / NULL + error），且不
  转交同名重载的新实例。
- vtable 入口统一 `enterPluginHost()`：解析令牌 + 取实例/管理器强引用 +
  admission lease；注册类入口执行期复查（`acceptsRegistration`），关闭/停用期间
  排队到达的注册被拒绝且不产生残留。
- `ioCallSyncKeep` / `ioCallSyncVoidKeep`：投递闭包持有强引用 + lease，
  卸载等待覆盖"已排队未执行"阶段（agent 侧 82 处、client 侧 57 处调用点迁移）。

**注册事务（真实 DSO 双端验证）**

- agent 侧回滚范围：工具、图类型（GraphTypeSlot 失效）、事件订阅、prompt 贡献、
  hook（中间件句柄）、能力、资源（skill 目录所有权）。
- client 侧回滚范围：状态栏项、面板、Info 段落、命令、事件订阅。
- 测试 DSO：`agent/test/plugin/dso_plugins/test_start_fail/`（全注册后主动失败）与
  `.../test_client_start_fail/`（每步注册后句柄 update 自检，全成功后失败；
  环境变量 `AGENTXX_TEST_CLIENT_START_OK=1` 时第二次加载全量重建成功）。
- `enable/disable` start/stop 事务：agent 侧（P1-4）与 client 侧（P1-3）都已改为
  "停用投递 stop、启用先补 stop 再 start"，start 失败回滚本次注册并回到 Disabled；
  stop 成功后清空 start 重新声明的注册记录，重复 enable/disable 不累积。

**Graph / Client 句柄**

- `GraphTypeSlot` + 节点代次：卸载/重载后旧节点返回插件已关闭/代次失效，
  不调用任何插件回调，同名 type 重载不转交新实例。
- 客户端旧 host 指针卸载后安全失败；订阅句柄退订幂等；同轮事件派发中
  前一个 handler 退订后一个后，后者不执行。

**提交**：`b2b5114a`、`c2869f07`、`9be9c735`、`a321267c`、`cdbcc738`、
`0bab72e3`、`e96f8f1b`、`d6ae39cd`、`75b01a56`。

**验证**：`plugin_runtime` 623、`plugins` 359、`plugin_resources` 83、
`plugin_multi_instance` 48、`client_plugins` 421。

### 1.4 R3 ABI v1 / SDK（已完成）

**C ABI v1**

- 全部接口表 `_reserved` 改为 `struct_size`；宿主填 `sizeof(表)`；SDK 严格校验
  `version == 1 && struct_size >= sizeof(Iface)`；插件 `api_version` 要求精确相等。
- opaque `AgentxxPluginCancelToken` + `agentxx_plugin_cancel_is_requested()`；
  删除 ABI 层 `volatile int32_t* cancel_flag`。
- scheduler v1：`post_to_io` 返回状态、`sleep` 返回 Operation handle、通用
  `op_cancel`、`offload` 工作函数接收 CancelToken；删除 `pump_io`/`cancel_sleep`。
- tasks v1：`register_task` 返回宿主托管 handle + `cancel_task`；SDK `spawn` 在
  无 tasks 表时明确失败（不再 unmanaged 降级）。
- C17 编译期检查：新增 `agent/test/plugin/test_plugin_abi_c17.c`
  （`-std=c17 -pedantic-errors`）固定 18 张跨边界结构体对齐/偏移/版本布局，
  并与 C++ 侧逐项对照（17 项编号表）。

**C++ SDK**

- 拥有型 `detail::RootRequest`（args/session/call_id/method + host + cancel token），
  工具/hook/能力/图节点四类入口统一 root adapter；借用视图失效后协程继续读取正确。
- `detail::CompletionGuard`：exactly-once `notify.done`、异常映射 FAILED/CANCELLED、
  作用域兜底 FAILED。
- hook 同步/异步严格分发（`if constexpr` 按返回类型）：`void` 同步完成；
  `Task<T>` 返回可取消 provider 句柄，完成在协程真正结束后发出。
- capability 支持 `Task<T>` 异步；`graph_node(ctx,type,schema,fn)` 与 tool/hook/
  capability 共用同一完成协议；`offload<void>` 由 `std::monostate` 支持。
- 修复 `OpCtl` 栈引用悬垂（stack-use-after-return，见 1.8）。

**提交**：`b2b5114a`（ABI/表/SDK 主体）、`aa4b33ff`（P1-2 C17 + 严格协商）、
`4f8d1fdf`（P1-1 Request/adapter/hook 分发）、`3e76a143`（capability 异步）、
`0bab72e3`（R3-2 graph adapter + 反例编译检查）。

**验证**：`plugin_sdk` 71；`check_sdk_negative_compile.sh` 5/5
（positive_control 必须成功；错误 hook/capability/tool 返回类型、跨边界 STL 参数
必须失败）。

### 1.5 R4 内置插件 / JS / 平台（已完成）

**已迁移为 start/stop 事务的内置插件**（16 个）

| 插件 | 迁移内容 | 提交 |
|---|---|---|
| `example_plugin` | 双端工具/hook/事件/能力/prompt + client UI 注册 | `dfe04a6a` |
| `example_resources` | 运行时资源注册移入 start | `460d35f5` |
| `example_graph_node` | 两个节点类型注册移入 start | `460d35f5` |
| `agentxx_math` / `agentxx_system` / `agentxx_string` / `agentxx_websearch` / `agentxx_rag_search` | create 只构造；配置读取与工具注册移入 start | `3e76a143` |
| `agentxx_codegraph` | 注册事务移入 start + 后台 warmup 托管化 | `460d35f5` |
| `agentxx_filesystem` | 6 工具 + client 模板/渲染器 | `10cb3ae2` |
| `agentxx_execute_command` | bash/windows 工具 + client 模板 | `10cb3ae2` |
| `agentxx_system_monitor` | 工具/能力/订阅 + 后台采样 spawn；client Info/订阅/命令 | `10cb3ae2` |
| `agentxx_planning` | prompt 贡献 + 规划工具 + client_attached 订阅；client 动作绑定 + 3 订阅 | `10cb3ae2` |
| `agentxx_javascript_engine` | 引擎 runtime/线程与能力注册移入 start；stop 停线程并释放 runtime | `d3dbd909` |
| `agentxx_execute_javascript` | create 只构造；start 校验能力 + 异步加载脚本；stop 卸载脚本 | `d3dbd909` |
| `example_js` | 同上（不再于 create 期经 PluginBase::init 注册轮次事件订阅） | `d3dbd909` |

**取消与后台任务**

- CancelToken 接入：`websearch`/`rag`/`string`（`460d35f5`）以及
  `filesystem`/`codegraph`/`execute_command`/`system_monitor`（`b2b5114a`）；
  HTTP/正则本体尚不可中断，取消在其返回后的边界生效。
- `codegraph` 后台 warmup 由 `std::thread + stop 标志` 改为
  `ctx.spawn(sleep → offload(updateIndex))`（卸载可取消、lease 覆盖索引代码）。
- `system_monitor` 采样任务迁入 start（disable→enable 会重启采样），
  双实例专项用例（`plugin_multi_instance` 29 → 48）。

**JS**

- `drivePromise` 终态映射：`Value → OK`、`Rejected/Timeout → FAILED`、
  `Cancelled → CANCELLED`；异常值不再当成功 payload（`ff6fc990`）。
- 删除 1ms 忙轮询：改用任务队列 / 最近定时器到期 / `timerEpoch_` 版本 / 绝对
  截止时间四者唤醒（`ff6fc990`）。
- `callTool` 始终返回 Promise：本引擎工具同线程 then 链，宿主工具经
  `call_tool_async` + 完成回调 post 回 JS 线程 settle；删除 `call_tool_blocking`
  自锁路径（`2a53977f`）。
- 脚本顶层异常事务：注册撤销动作逆序执行完后才释放 JSContext；脚本 `unload`
  不再残留工具/订阅（`2a53977f`）。
- `hookStart` / 能力 `unload` 的真实完成：JS 回调（含 Promise）结束后才 done
  （`2a53977f`）。
- 多实例可变静态审计：修复 `jsCapStart("load")` 活动 op 占位句柄的函数级
  static，改为实例成员（`3143ef92`）。

**JS 三件插件 start/stop 迁移（提交 27）**

`JsEngine` 生命周期（引擎层）：

- `create` 只构造并装配宿主句柄；`start()` 才 `JS_NewRuntime` + 启动专用 JS 线程
  并注册 `interpreter.js` 能力；`stop` 停止线程、释放 runtime —— disable→enable
  往返等价于一次 stop+start，引擎线程与脚本上下文按事务重建，不存在"线程已存在
  但实例已停用"的中间态（原设计把线程放在 create，disable 语义未定义）。
- stop 不在调用线程（IO 线程）`join`：JS 线程可能在宿主 vtable 调用（`ioCallSync`
  等 IO 线程）上阻塞，直接 join 会自锁；改由独立收尾线程完成 join +
  `JS_FreeRuntime`，完成通知从该线程上报（ABI 允许 done 来自任意线程）。
- JS 线程空闲（无在手中任务、队列为空）时走调用线程直接收尾的快路径
  （`busy_` 标记与取任务在同一临界区内置位，保证"观察为空闲"时线程只会走不调用
  宿主的退出清理路径）：省一次线程切换，并让 stop→start 紧邻的启用事务保持确定
  顺序（依赖方能力注册先于依赖者 start 完成）。
- 停止后不再执行插件 JS：`post` 拒绝新任务，队列任务在 JS 线程内按
  CANCELLED/FAILED 终结（能力 load/unload、工具执行、hook 各有前置检查），
  事件投递静默丢弃；已开始的执行由 `drivePromise` 观察停止标志后按取消完成。
- `stop_` 由普通 bool 改为 `std::atomic<bool>`（JS 线程读、宿主线程写，消除竞争）。
- 能力注册搬入 start 事务并加实例幂等标记（`capabilityRegistered_`），重复 start
  不再触发同名冲突。

脚本壳插件（`example_js` / `agentxx_execute_javascript`）：

- `create` 只构造（查询接口表 + 解析自身 `plugin.js` 路径），不注册、不起线程、
  不调用能力；
- `start` 校验 `interpreter.js` 能力 → 异步 `load` 脚本；脚本注册的工具/订阅挂在
  本实例，因此 start 的完成必须等加载结束（返回宿主托管句柄，完成回调里上报
  OK/FAILED），加载失败由宿主回滚本次加载；
- `stop` 通知引擎 `unload` 脚本（fire-and-forget；卸载 op 自身持有本实例 caller
  lease，宿主等到它完成才 destroy），引擎不可用时只记录；
- `destroy` 只释放本地状态，不再调用宿主接口（原实现于 destroy 内发起 unload）。

**平台**

- `f861bcf9` 修复平台插件构建（audio_stream 实例成员、computer_use/screen_capture
  编译、execute_command setup 抽函数、rag_search 依赖）。
- 平台 gate（提交 29 收敛）：`screen_capture`/`computer_use`/`text_selection_monitor`
  仅 Windows 编译；`agentxx_audio_stream` 因 WASAPI 实现未启用（`XX_IS_WIN_D && false`，
  只剩桩分支）改为**空平台列表 = 全平台跳过**，Windows/Linux 均不产出该 DSO。
  矩阵与验证状态见 `docs/zh-cn/design/plugins.md` 15.6 与本文 1.10。

### 1.6 R5 Client / 依赖 / prompt（已完成）

- **语义渲染缓存（F03）**：插件自定义 renderer 只在 client io 线程执行；UI 只读
  宿主语义快照（displayName/summary/items + plugin + generation + 输入特征哈希）；
  未命中返回通用回退并投递一次渲染请求；输出字段在所有路径由宿主释放；
  禁用/卸载按插件失效并递增版本号，会话切换清空；容量上限默认 512，
  条目与版本记录按写入顺序同时回收（`43c93ff6`）。
- **动作代次**：`ClientUiRegistry::instanceGenerations` + 点击携带
  plugin/owner/generation，io 线程复查，重载/卸载后旧点击丢弃不转交新实例。
- **事件派发**：快照后逐 callback 复查 `alive`/代次/实例状态，同轮退订生效。
- **依赖级联（F09）**：agent/client 双侧递归级联禁用/恢复；`userDisabled` 与
  `blockedByDependencies` 区分，用户显式禁用的插件不被级联恢复。
- **prompt 贡献（F20）**：owner + key + sequence 合成模型；卸载/禁用只移除本
  owner 贡献并 rebase 基础值，不覆盖其他 owner 与用户后续写入。
- 可选遗留（不影响验收，见 2.4）：`onToolRenderUpdated` 仍整表重绘，未做按消息块
  精确失效；TUI 组件级"旧快照渲染"用例未补（manager 层已覆盖语义路径）。

### 1.7 R6 验证 / 文档（已完成）

- **Linux ASan + LSan**：插件专项 7 模块 1706/0；扩展回归 14 模块 **2179 passed /
  0 failed**；LSan 报告与重构前基线逐项一致（4480 字节 / 64 处）→ 未新增泄漏。
- **Windows ASan**：插件专项 9 模块 1765/0（2 轮）、扩展 16 模块 2251/0、
  工具模块 360/0；详见 1.10 与 3.1。
- **TSan 定向回归（提交 28）**：独立构建目录 `agent/build/linux-tsan`
  （`AGENTXX_ENABLE_ASAN=OFF` + `AGENTXX_ENABLE_TSAN=ON`）；插件框架 7 模块
  **0 告警 / 1695 断言全通过**（连续两轮）。TSan 期间定位并修复 3 处插件相关数据竞争
  （log sink 成员顺序、测试顺序探针同步、system_monitor GPU/PDH/查询实例化），
  明细见 1.8 第 9-11 条与 3.5。
- **UBSan 定向探针（`7a44e76d` 建立；提交 28 复跑）**：构建选项
  `AGENTXX_PLUGIN_UBSAN_PROBE`（默认 OFF，开启时只对 `lib/src/plugins/*.cpp` 与
  6 个插件测试 TU 追加 `-fsanitize=undefined -fno-sanitize-recover=undefined`，
  链接参数由顶层注入）；首次 1592/0、提交 28 代码 1670/0，两次均无 `runtime error`，
  验证后已恢复基线。
- **导出符号白名单（`dfe04a6a`）**：`agent/script/check_plugin_exports.sh`；
  16 个插件库只导出
  `agentxx_plugin_{agent,client}_{get_info,create,start,stop,destroy}`。
- **SDK 反例编译（`0bab72e3`）**：`agent/script/check_sdk_negative_compile.sh`
  从真实 `compile_commands.json` 提取环境，5/5 符合预期。
- **设计文档**：`docs/zh-cn/design/plugins.md` 第 15 节 Reset-v1 生命周期与异步
  契约（15.1 入口与状态机 / 15.2 Operation 终态 / 15.3 线程与租约 /
  15.4 启用禁用事务 / 15.5 自管线程插件 / 15.6 平台支持矩阵与验证状态），
  并更新第 2/3/4/6/9/12/14 节；英文侧同步第 13/14 节。
- 本阶段无未完成项（非阻塞待完成见第 2 节）。

### 1.8 关键缺陷修复清单（交接重点）

以下均为重构过程中真实复现并修复的缺陷（含测试/回归方式）：

1. **`BaseAgent::shutdownAsync` 投递错 executor**：原投递到自身 `ioCtx`，
   而 engine 直跑的子代理其 `ioCtx` 从未 `run()` → `agent_host` 模块永久挂起。
   改为投递 `pluginManager->ioExecutor()`（`b2b5114a`）。
2. **`OpCtl` 栈引用悬垂**：`tool()`/`spawn` 把 `OpCtl` 放在 start 栈上按引用传给
   业务协程，协程挂起后引用悬垂（ASan `stack-use-after-return` 复现）。
   改为 Job/`SpawnRecord` 持有（`4f8d1fdf`）。
3. **enable/disable 事务落后于 unload**：`startForEnable`/`stopForDisable` 在实例
   Closed 后仍 `setState(Ready)`（触发断言）或覆盖 Closing。两处事务在进入与每个
   await 之后复查 `closeRequested()/Closed`；修复前可复现 SIGABRT（`cdbcc738`）。
4. **同步执行器停止期间的 `ioCallSync` 永久等待**：runtime 不可用时快速失败，
   完成/取消动作进待重放队列（`b2b5114a`）。
5. **JS 定时器执行后未立即泵 job**：定时器回调解决 Promise 产生的新 job 未运行，
   谓词等到下一个（可能 30s 后的）定时器 → 虚假超时。到期定时器执行后立即回到
   循环头重跑 QuickJS job（`ff6fc990`）。
6. **Client 端闭包 `.lock().get()` 悬垂**：退订/回调闭包持弱引用后取裸指针；
   改为闭包持有强引用（`c2869f07`）。
7. **`cpu_gpu` 用例在 Reset-v1 后必失败且会挂起**：能力调用 `nullptr` caller 被
   拒绝（现在要求 caller 实例做租约保护），失败后 `while(!done)` 无界等待。
   改为传 `inst.get()` + 有界等待（`10cb3ae2`）。该模块此前不在回归列表。
8. **JS `jsCapStart("load")` 可变静态**：函数级 static 占位句柄跨实例共享，
   改为实例成员（`3143ef92`）。
9. **`ThreadedLogSink` 成员初始化顺序竞争**（提交 28，TSan 发现）：日志线程在
   `thread_` 构造时启动并立刻读 `running_`/`idle_`，而这两个成员声明在 `thread_`
   之后（成员按声明顺序初始化）→ 读到未初始化值；严重时线程判定"已停止"提前退出、
   丢日志。改为把状态成员声明在 `thread_` 之前并注释原因。
10. **`test_plugin_runtime` 顺序探针裸 vector 竞争**（提交 28，TSan 发现）：
    11.2-8 用例的 `order` 由后台任务线程与主线程并发访问 → 新增互斥量保护的
    `OrderLog`。
11. **`agentxx_system_monitor` GPU/PDH 状态与并发查询**（提交 28，TSan 发现）：
    ① Linux GPU 枚举缓存、Windows DXGI 适配器缓存与 PDH 查询句柄原为函数级 static
    → 改为 `CpuGpuMonitor::Impl` 实例成员（PDH 句柄随之与实例生命周期配对释放）；
    ② 同一实例的并发查询（后台采样 offload 与工具/能力调用同在宿主阻塞池）并发读写
    CPU 采样基线 `_sample` → 新增实例成员 `queryMutex` 串行化 `querySync()`。
    修复后插件框架 TSan 告警归零。
12. **`test_plugin_runtime` 在 Windows 偶发失败/挂起**（提交 29，Windows 验证发现）：
    测试用轮询 `poll()` 驱动 IO 且主线程阻塞在 `future.get()`；win_iocp 的定时器
    到期由独立定时器线程投递完成包，可能晚于本次 `poll()`，导致
    `waitIdleUntil`/`unloadAsync` 完成事件被漏掉 → 断言失败或永久挂起
    （cdb 抓栈定位在 `test_plugin_runtime.cpp:424` 的 `future<bool>::get`）。
    改为有界推进 IO 的等待（`drainUntil`/`waitFutureReady`/`waitFutureValue`），
    并在 `drainAll` 中于"无就绪处理器"时先让出 CPU 再确认一次。
    修复后 Windows `plugin_runtime` 634/0 连续 10 轮稳定；Linux 复跑 634/0 不变。
    见 1.10 第 5 条第 1 项。
13. **`agentxx_audio_stream` 以桩实现进入产物**（提交 29，Windows 验证发现）：
    WASAPI 分支条件是 `XX_IS_WIN_D && false`，平台 gate 却声明 `windows` →
    Windows 上会产出一个只有"not implemented" 桩的插件 DSO。改为空平台列表
    （全平台跳过）并在 CMake/源码注明恢复条件。见 1.10 第 1 条。

### 1.9 新增测试 / 脚本 / 构建选项清单

| 资产 | 位置 | 用途 |
|---|---|---|
| `plugin_runtime` 测试模块 | `agent/test/plugin/test_plugin_runtime.{h,cpp}` | Operation/lifetime/租约/关闭/句柄/竞速回归（634 断言，含 Windows 有界等待） |
| `plugin_sdk` 测试模块 | `agent/test/plugin/test_plugin_sdk.{h,cpp}` | SDK Request/hook 分发/capability/图节点（71 断言） |
| C17 ABI 检查 TU | `agent/test/plugin/test_plugin_abi_c17.c` | 纯 C 编译期布局断言 + C/C++ 对照 |
| 测试插件 DSO（agent 侧） | `agent/test/plugin/dso_plugins/test_start_fail/` | start 全注册后失败 → 回滚 |
| 测试插件 DSO（client 侧） | `agent/test/plugin/dso_plugins/test_client_start_fail/` | UI 全注册后失败 → 回滚 → 二次加载重建 |
| SDK 反例片段 | `agent/test/plugin/negative_compile/*.cpp` | 错误签名必须编译失败 |
| 导出符号脚本 | `agent/script/check_plugin_exports.sh` | 白名单校验（16 库） |
| 反例编译脚本 | `agent/script/check_sdk_negative_compile.sh` | 5 片段行为断言 |
| UBSan 探针选项 | `AGENTXX_PLUGIN_UBSAN_PROBE`（顶层/lib/test CMake） | 定向 UBSan（不重编第三方） |

### 1.10 Windows 平台验证（提交 29 完成，R6 最后一项）

**背景**：本任务此前在 Linux/WSL2 完成，唯一阻塞"重构完成"判定的是 Windows 平台
（plugin.md §0.1）。提交 29 改在 Windows 本机（MSVC 14.51 / VS18、12 核）执行
完整构建与专项回归。

**1) 平台 gate 与产物面（plugin.md §2.3 要求）**

- 核对结论：`screen_capture` / `computer_use`（COM 配对）/ `text_selection_monitor`
  （UIAutomation）在 Windows 上**真实编译并进入产物**；
  `agentxx_audio_stream` 的 WASAPI 实现是 `#if XX_IS_WIN_D && false` 的**未启用代码**，
  实际只会编译"not implemented" 桩 —— 按 §2.3"不得把未实现能力伪装为已支持"，
  本次把它的平台声明改为**空列表（全平台跳过）**，不再产出该插件 DSO，
  并在源码与 CMake 处写明恢复条件（实现可用后声明 `windows` 并同步文档矩阵）。
  改动后 Windows 构建产物为 **19 个插件 DSO**（此前的 20 个去掉 audio_stream）。
- 增量构建目录中可能残留此前的 `agentxx_audio_stream.dll`（CMake/install 不回收
  已移除目标的旧文件）；全新构建不产出，产物统计以构建日志的
  `<target>.vcxproj -> ...` 行为准（本次已清理残留并复核）。

**2) 构建**

- 命令：`.\agent\script\windows_debug_build.bat`（配置 + 构建 + install + 复制到
  `build/windows-debug-output`），随后增量：
  `cmake --build agent/build/windows-debug --config Debug --parallel 8`。
- 结果：成功。插件目标逐个链接（日志含 19 个 `lib...dll` 输出行，无
  `agentxx_audio_stream.vcxproj`），`libagentxxd.dll` / `libagentxx_staticd.lib` /
  `agentxx_test.exe` / `agentxx_cli.exe` 全部产出；配置期打印
  `Skip plugin 'agentxx_audio_stream': no implementation for platform 'windows' (supported: )`。
- 注（既有经验，非本次引入）：命令行 `cmake --build ... --parallel 8` 在
  MSBuild 下偶发 `error MSB6006: CL.exe 已退出，代码为 1` + `MSB4166 子节点过早退出`
  （无 `error C####` 与之对应，属编译器进程被资源压力终止）；重跑即通过。
  构建脚本方式（含 `/Z7`、固定并行度）更稳定。

**3) 运行前置条件（Windows 专属）**

- 必须把工作目录设为 `agent/build/windows-debug/exec`：`agentxx_test.exe` 用
  `GetModuleFileNameW` 的父目录定位 `plugins/<name>`，工作目录不对时插件加载不到，
  用例会失败（Linux 侧的 `/proc/self/exe` 推导等价）。
- 长跑用例（完整扩展回归约 185s）应以后台进程 + 重定向日志方式运行，避免撞上
  工具会话超时。

**4) 平台专项与回归结果**（明细见 3.1）

| 项目 | 结果 |
|---|---|
| 插件专项回归（9 模块，连续 2 轮） | **1765 passed / 0 failed**（`plugin_runtime` 634） |
| 平台专项 | `screen_capture` 46 / `cpu_gpu` 25 / `text_selection` 13，全 0 failed |
| 扩展回归（16 模块） | **2251 passed / 0 failed** |
| 工具模块回归（含 `command` Windows 分支） | **360 passed / 0 failed** |
| 多轮重复运行（去 flaky） | `plugin_runtime` × 10 全绿；插件专项 × 2、扩展 × 1 全绿 |

**5) 本次顺手修复的两处 Windows 专属问题（都在测试/构建侧，产品代码未改语义）**

1. **`test_plugin_runtime` 在 Windows 上偶发失败/挂起**（见 1.8 第 12 条）：
   测试用轮询 `poll()` 驱动 IO，同时主线程阻塞在 `future.get()`；Windows（win_iocp）
   的定时器到期由**独立定时器线程**投递完成包，可能晚于本次 `poll()`，于是
   `waitIdleUntil` / `unloadAsync` 的完成被漏掉 → 断言失败或永久挂起。
   生产路径有常驻 `run()` 线程，不受影响；修复方式是把这类等待改为**有界推进
   IO 的等待**（`drainUntil` / `waitFutureReady` / `waitFutureValue`），超时即断言失败
   而不是挂起。修复后 Windows `plugin_runtime` 由"偶发失败/挂起"变为
   **634/0 连续 10 轮稳定**（断言数 623 → 634，多出的 11 条来自这些有界等待的
   显式断言）。
2. **`agentxx_audio_stream` 平台 gate**（见 1.8 第 13 条）：改为全平台跳过，
   避免"桩实现进入发布产物"。

### 1.11 plugin.md 第 11 节测试矩阵覆盖对照

| 矩阵条目 | 状态 | 覆盖位置 / 缺口 |
|---|---|---|
| 11.1 C17 ABI 编译测试 | 完成 | `test_plugin_abi_c17.c` + `plugin_runtime` 对照用例；SDK 正反例 5/5；未知/短表、NULL 表、版本不匹配均被拒绝 |
| 11.2 用例 1-10 | 完成 | 1-4/6/7：P0-2 与早期用例；5/8/9：R1-2；10：R3-2/R2-4/R2-5/R4-3（双端真实 DSO） |
| 11.3 Client/Graph/JS | 完成 | 同轮退订（R2-3）、旧快照/旧动作（P1-3）、GraphTypeSlot 代次（R1-2/R3-2）、JS callTool/顶层异常/hook 真实完成（R4-2）、Promise 终态映射（P2-1b）、JS 引擎 start/stop 往返与双实例隔离（提交 27） |
| 11.4 多实例 | 完成 | `plugin_multi_instance` 80（system_monitor 双实例采样隔离 + JS 引擎/脚本插件双实例：停用/启用/卸载互不影响）；另一组同进程不同 executor 用例在 `test_plugins` 多实例段；Windows 侧同模块 80/0 复跑通过 |
| 11.4 Windows 编译 | 完成（提交 29） | Windows Debug 全插件构建（19 DSO）+ `screen_capture` 46 / `text_selection` 13 / `cpu_gpu` 25 / `command` 28 全绿，见 1.10、3.1 |
| 11.4 导出符号 | 完成 | `check_plugin_exports.sh` 16 库全绿（Linux ELF）；MSVC 侧不自动导出，仅 `AGENTXX_PLUGIN_EXPORT` 入口导出（构建配置已注释说明） |
| 11.4 audio_stream 不进入支持矩阵 | 完成（提交 29） | CMake 平台 gate 改为全平台跳过（实现未启用），Windows/Linux 均不产出该 DSO |
| 11.5 旧基线 | 仅比较 | 旧基线 1180/0 已不作为验收依据 |

---

## 2. 待完成任务内容

> 以下 4 项均为**非阻塞**事项，不影响"Reset-v1 重构完成"判定（plugin.md 第 12 节
> 6 项完成标准已全部满足）。

### 2.1 依赖插件启用事务串行化（plugin.md 已知残余顺序边界）

**现象**：`disable(X)` 与 `enable(X)` 在同一轮同步调用（enable 紧邻 disable、中间
无 await）时，宿主按依赖拓扑投递 start 事务，但 `startForEnable` 在依赖方"stop 仍欠着"
时会先 `await stop`，于是依赖者的 start 可能先于依赖方的 start 完成：

- 引擎 stop 走收尾线程（JS 线程忙时）→ 完成通知晚于依赖者的 stop 完成通知；
- 依赖者（脚本壳）的 start 观察到 `interpreter.js` 能力暂缺 → 按契约失败并回到
  Disabled（无残留注册），需要再 `enable` 一次才能恢复。

**已做的缓解（提交 27）**：JS 引擎空闲时 stop 在调用线程直接收尾（`JsEngine::requestStop`
的空闲快路径），使常见场景（无在途 JS 执行）的 stop→start 顺序确定；`plugins`
25b 段对该路径有回归断言（disable 紧邻 enable 后工具/能力仍可恢复）。

**彻底修复方向**：`enableImpl` 把依赖者的 start 事务串到各依赖的启用事务完成之后
（per-instance 启用事务状态 + 依赖者 await），并在依赖 start 失败时不启动依赖者。
属于宿主调度改进，失败→Disabled 可重试的现行契约不受影响。

### 2.2 业务插件迁移到宿主 HTTP/process 服务（plugin.md §2.3 / §9 第 5 步"随后"部分）

- 现状：`agentxx_execute_command`、`agentxx_websearch`、`agentxx_rag_search` 已接入
  CancelToken 并受控 offload，但每个请求仍占用一个长期阻塞 worker（HTTP 本体、
  进程等待期间不可中断，取消只在返回后边界生效）。
- 目标：逐步改为经宿主提供的 HTTP / process 服务执行，避免长占阻塞线程池、
  让取消能真正中断底层 IO。
- 验收建议：迁移后的插件在取消/超时时能立即释放 worker，并有对应回归。

### 2.3 平台交互人工回归（Windows 桌面插件）

- `computer_use`（鼠标键盘注入）与 `text_selection_monitor`（UIA 选择事件）目前只有
  接口级用例（`cpu_gpu` 25 / `screen_capture` 46 / `text_selection` 13 全绿），
  未做真实桌面场景的人工验证。
- 建议在发布桌面交互功能前补一次人工回归：多显示器捕获、坐标缩放（DPI）、
  前台/后台窗口键鼠事件、选择事件流。
- `agentxx_audio_stream` 无实现（已按平台 gate 全平台跳过，见 1.5/1.10），
  如需要该能力需另行实现并恢复 gate。

### 2.4 可选收尾项（按价值排序）

1. **create 失败回滚的独立用例**：现有真实 DSO 只覆盖 start 失败；补一个
   create 中途失败的 DSO（create 返回非 0），断言无注册残留且可再次加载同名插件。
2. **TUI 组件级旧快照端到端用例**：现有 renderer/动作用例在 manager 层驱动
   `renderClientTool`/`dispatchAction`；可补 message_list 组件级"旧快照 + 卸载"
   用例（需要 TUI 测试脚手架）。
3. **`onToolRenderUpdated` 按消息块精确失效**：目前整表重绘，长会话上有优化空间。
4. **`PluginHostControl` tombstone 内存评估**：控制块按实例累计、永不回收
   （每实例约 100 字节）。若出现"进程内加载/卸载上万次"场景，在不复用地址的
   前提下评估池化/压缩。
5. **关闭超时取证增强**：`pendingOperationSummary()` 可补充每个 Operation 的
   等待时长。

---

## 3. 验证记录

### 3.1 最终验证（提交 29，2026-09-11）

**平台与结论**：Windows（MSVC 14.51 / VS18 Debug + ASan）与 Linux（GCC 16.1 Debug +
ASan/LSan）双侧通过；这是"重构完成"判定的最终依据。

**Windows**

```powershell
# 构建
.\agent\script\windows_debug_build.bat
cmake --build agent/build/windows-debug --config Debug --parallel 8
# 运行（工作目录必须是 exec）
cd agent\build\windows-debug\exec
.\agentxx_test.exe plugin_runtime plugin_sdk plugins plugin_resources `
  plugin_multi_instance client_plugins cpu_gpu screen_capture text_selection --fail-fast
```

```text
plugin_runtime        634 passed / 0 failed
plugin_sdk             71 passed / 0 failed
plugins               392 passed / 0 failed
plugin_resources       83 passed / 0 failed
plugin_multi_instance  80 passed / 0 failed
client_plugins        421 passed / 0 failed
cpu_gpu                25 passed / 0 failed
screen_capture         46 passed / 0 failed
text_selection         13 passed / 0 failed
合计                 1765 passed / 0 failed（exit=0；连续 2 轮一致）
```

```text
扩展回归（16 模块）：
ffi_c_api 117 / plugin_runtime 634 / plugin_sdk 71 / subagent_bus 21 / subagent_tool 122 /
agent_host 95 / codegraph 23 / screen_capture 46 / cpu_gpu 25 / text_selection 13 /
plugins 392 / plugin_resources 83 / plugin_multi_instance 80 / client_plugins 421 /
agent 93 / memgrowth 15
合计 2251 passed / 0 failed（exit=0，耗时约 184s）

工具模块回归：
command 28 / filesystem 101 / string_tools 17 / math_tools 82 / web_search 19 /
codegraph 23 / datetime 5 / rag_search 85
合计 360 passed / 0 failed（exit=0）

稳定性：plugin_runtime 单模块 × 10 轮全部 exit=0、634/0；插件专项 9 模块 × 2 轮 1765/0；
扩展 16 模块 × 1 轮 2251/0

构建产物：19 个插件 DSO（agentxx_codegraph / computer_use / execute_command /
execute_javascript / filesystem / javascript_engine / math / planning / rag_search /
screen_capture / string / system / system_monitor / text_selection_monitor / websearch /
example_graph_node / example_js / example_plugin / example_resources）；
配置期输出 `Skip plugin 'agentxx_audio_stream': no implementation for platform 'windows'`
```

**Linux（交叉验证测试改动无回归）**

```bash
cmake --build agent/build/linux-debug -j12
ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 timeout 1200s \
  agent/build/linux-debug/exec/agentxx_test plugin_runtime plugin_sdk plugins \
  plugin_resources plugin_multi_instance client_plugins cpu_gpu --fail-fast
```

```text
合计 1706 passed / 0 failed（exit=0）
  plugin_runtime 634 / plugin_sdk 71 / plugins 392 / plugin_resources 83 /
  plugin_multi_instance 80 / client_plugins 421 / cpu_gpu 25
LSan: 4480 byte(s) leaked in 64 allocation(s) —— 与重构前基线逐项一致（未新增泄漏）
bash agent/script/check_plugin_exports.sh  → OK: 16 plugin libraries
```

### 3.2 提交 27/28 定向与扩展回归（ASan + LSan）

```bash
cmake --build agent/build/linux-debug --target agentxx_test_repo -j12
# 增量迭代时的手工同步（顶层 target 也会复制，二者等价）：
# cp agent/build/linux-debug/agentxx_test_repo-prefix/src/agentxx_test_repo-build/agentxx_test \
#    agent/build/linux-debug/exec/agentxx_test
ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 timeout 900s \
  agent/build/linux-debug/exec/agentxx_test \
  plugin_runtime plugin_sdk plugins plugin_resources plugin_multi_instance \
  client_plugins cpu_gpu --fail-fast
```

```text
plugin_runtime        623 passed / 0 failed
plugin_sdk             71 passed / 0 failed
plugins               392 passed / 0 failed   （基线 359；25b/26 段新增 33 断言）
plugin_resources       83 passed / 0 failed
plugin_multi_instance  80 passed / 0 failed   （基线 48；新增 JS 双实例段 32 断言）
client_plugins        421 passed / 0 failed
cpu_gpu                25 passed / 0 failed
合计                 1695 passed / 0 failed   （exit=0）
LSan                 4480 byte(s) leaked in 64 allocation(s) —— 与重构前基线逐项一致
```

```text
扩展回归（14 模块，ASan + LSan）：
ffi_c_api 117 / agent_host 95 / subagent_tool 122 / subagent_bus 21 / plugin_sdk 71 /
plugin_runtime 623 / plugins 392 / plugin_resources 83 / plugin_multi_instance 80 /
client_plugins 421 / cpu_gpu 25 / agent 91 / memgrowth 15 / codegraph 23
合计 2179 passed / 0 failed（exit=0）
日志：/tmp/asan-ext-final.log、/tmp/js-after1.log（1695/0）、/tmp/js-plugins-leak.log
```

补充校验：`check_plugin_exports.sh` 16 库全绿、`check_sdk_negative_compile.sh` 5/5、
`git diff --check` 无空白错误；`plugins` 模块单独复跑 3 次（含 LSan）均 392/0（无 flaky）。

### 3.3 各阶段验证汇总（断言数演进）

| 提交 | plugin_runtime | plugin_sdk | plugins | client_plugins | 其他（合计） |
|---|---|---|---|---|---|
| `b2b5114a`（基线） | 128 | — | 328 | 309 | multi_instance 29 |
| `c2869f07`（P0-1） | 151 | — | 328 | 309 | 合计 1361/0 |
| `8c717236`（P0-2） | 415 | — | 328 | 309 | 合计 1625/0 |
| `aa4b33ff`（P1-2） | 439 | — | 328 | 309 | 合计 1649/0 |
| `4f8d1fdf`（P1-1） | 439 | 29 | 328 | 309 | 合计 1678/0 |
| `9be9c735`（P1-4） | 526 | 29 | 328 | 309 | 合计 1765/0 |
| `a321267c`（P1-3） | 526 | 29 | 328 | 374 | 合计 1830/0 |
| `dfe04a6a`（P2-1a） | 526 | 29 | 328 | 375 | 合计 1831/0；导出符号 16 库 |
| `ff6fc990`（P2-1b） | 526 | 29 | 331 | 375 | 合计 1834/0 |
| `3143ef92`（P2-1c） | — | — | — | — | 静态审计提交（回归沿用 1834/0） |
| `cdbcc738`（R1-2） | 600 | 29 | 331 | 375 | 合计 1908/0 |
| `460d35f5`（R4-1） | 600 | 29 | 331 | 375 | multi_instance 48、codegraph 23；专项 860/0 |
| `2a53977f`（R4-2） | 600 | 29 | 340 | 375 | 扩展回归 1959/0 |
| `3e76a143`（R4-3） | 600 | 44 | 340 | 375 | ffi 117、codegraph 23 |
| `0bab72e3`（R3-2） | 622 | 71 | 340 | 375 | 扩展回归 2023/0 |
| `e96f8f1b`（R2-3） | 623 | 71 | 340 | 390 | 扩展回归 2039/0 |
| `d6ae39cd`（R2-4） | 623 | 71 | 354 | 390 | 扩展回归 2053/0 |
| `75b01a56`（R2-5） | 623 | 71 | 359 | 408 | 扩展回归 2076/0 |
| `7a44e76d`（R6-1） | 623 | 71 | 359 | 408 | UBSan 探针 1592/0 |
| `43c93ff6`（R5-1） | 623 | 71 | 359 | 421 | 扩展回归 2089/0 |
| `10cb3ae2`（R4-4） | 623 | 71 | 359 | 421 | cpu_gpu 25；扩展回归 2114/0 |
| `d3dbd909`（R4-5） | 623 | 71 | **392** | 421 | JS 三件 start/stop 迁移；multi_instance 80；专项回归 1695/0 |
| 提交 28（R6-2） | 623 | 71 | 392 | 421 | TSan 插件框架 0 告警/1695 断言 + 3 处竞争修复；扩展回归 2179/0；UBSan 复跑 1670/0 |
| 提交 29（R6-3） | **634** | 71 | 392 | 421 | Windows 平台验证；Windows 专项 1765/0、扩展 2251/0、工具模块 360/0；Linux 复跑 1706/0 |

历史详细记录（每个模块完整列表与日志路径）见 git 历史中的本文件旧版本
（`git show <旧提交>:resource/history/plugin-refactor-2/work.md`）。

### 3.4 定向 UBSan 探针（`7a44e76d` 建立；提交 28 复跑确认）

```bash
cmake -B agent/build/linux-debug -S agent -DAGENTXX_PLUGIN_UBSAN_PROBE=ON
cmake --build agent/build/linux-debug --target agentxx_test_repo -j12
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 \
  timeout 1800s agent/build/linux-debug/exec/agentxx_test \
  plugin_runtime plugin_sdk plugins plugin_resources plugin_multi_instance client_plugins --fail-fast
# 验证后恢复基线
cmake -B agent/build/linux-debug -S agent -DAGENTXX_PLUGIN_UBSAN_PROBE=OFF
```

```text
提交 28 代码复跑：1670 passed / 0 failed（exit=0）；`runtime error` 匹配 0 处
首次建立时：      1592 passed / 0 failed（exit=0，无 runtime error 报告）
插桩范围校核（compile_commands.json）：
  lib/src/plugins/*.cpp   11/11 带 -fsanitize=undefined
  test/plugin/*.TU         6/13 带（其余为 DSO 测试插件/平台模块/ABI C 检查 TU, 设计如此）
日志：/tmp/ubsan-run-commit28.log、/tmp/ubsan-run.log、/tmp/baseline-restore.log
```

### 3.5 定向 TSan 回归（提交 28）

**构建**（独立目录，与 ASan 基线互斥；不动 `linux-debug`/`linux-release`）：

```bash
cmake -B agent/build/linux-tsan -S agent \
  -DBOOST_ROOT="$(pwd)/agent/third_party/boost-linux-build-debug" \
  -DOPENSSL_ROOT_DIR="$(pwd)/agent/third_party/OpenSSL-linux-build" \
  -DAGENTXX_BUILD_CLIENT=ON -DAGENTXX_BUILD_TEST=ON -DAGENTXX_BUILD_BENCHMARK=OFF \
  -DAGENTXX_ENABLE_HYPERSCAN=ON -DAGENTXX_ENABLE_BOOST_PROCESS=ON -DAGENTXX_ENABLE_PCH=OFF \
  -DXX_IS_RELEASE_D=0 -DCMAKE_BUILD_TYPE=Debug \
  -DAGENTXX_ENABLE_ASAN=OFF -DAGENTXX_ENABLE_TSAN=ON
cmake --build agent/build/linux-tsan --target agentxx_test_repo -j12
```

构建耗时（12 核 / 35G）：首次因工具会话超时被中断两次，实际累计约 15 分钟（含全部
ExternalProject）；GCC 16.1 在 ftxui `border.cpp` 处出现一次 ICE，按既有经验直接重跑
同命令即通过。

> 中断善后（重要经验）：编译期被杀会留下**残缺 `.o`/`.a`**，表现为 mold 报
> `undefined symbol`（本次为 yaml-cpp/ftxui）。按时间窗清理
> （`find . -name '*.o' -newermt <中断时刻±40s> -delete`，`.a` 同理）后，还需删除
> `*_repo-prefix/src/<name>-stamp/<name>-{build,install,done}` 让 ExternalProject 重建，
> 否则会停在 `No rule to make target '.../libsqlite3.a'`。不要清空整个 build 目录。

**结果（插件框架范围，连续两轮）**：

```text
TSAN_OPTIONS=halt_on_error=0 second_deadlock_stack=1 agentxx_test \
  plugin_runtime plugin_sdk plugins plugin_resources plugin_multi_instance \
  client_plugins cpu_gpu --fail-fast
→ 1695 passed / 0 failed，exit=0，0 条 ThreadSanitizer 告警   （两轮一致）
日志：/tmp/tsan-focused-1.log、/tmp/tsan-focused-2.log
```

**TSan 发现并修复的数据竞争（3 处，均已复跑 ASan 全量确认无回归）**：
`ThreadedLogSink` 成员初始化顺序（`log.h`）、`test_plugin_runtime` 顺序探针裸 vector、
`agentxx_system_monitor` GPU/PDH 状态与并发查询 —— 明细见 1.8 第 9-11 条。

**范围说明**：同一次扩展运行（16 模块）另有告警，全部落在**非插件模块**
（FFI 运行时、HttpServer、测试脚手架）与**未插桩三方库**（liburing + boost asio
io_uring），与 Reset-v1 重构无关，不属于本任务范围（如需处理请另行立项）。
本轮工作的结论是：`lib/src/plugins/*`、`lib/include/agentxx/plugin/*`、
`agent/plugins/*`、`agent/test/plugin/*` 在 TSan 下 **0 告警**。

### 3.6 历史基线（对应提交 `10cb3ae2`，2026-09-11）

```bash
ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 timeout 1500s \
  agent/build/linux-debug/exec/agentxx_test \
  ffi_c_api agent_host subagent_tool subagent_bus plugin_sdk plugin_runtime plugins \
  plugin_resources plugin_multi_instance client_plugins cpu_gpu agent memgrowth \
  codegraph --fail-fast
```

```text
ffi_c_api 117 / plugin_runtime 623 / plugin_sdk 71 / subagent_bus 21 / subagent_tool 122 /
agent_host 95 / codegraph 23 / cpu_gpu 25 / plugins 359 / plugin_resources 83 /
plugin_multi_instance 48 / client_plugins 421 / agent 91 / memgrowth 15
合计 2114 passed / 0 failed（exit=0，ASan + LSan）
日志：/tmp/r44-sweep2.log（另有 /tmp/r51-sweep.log、/tmp/r26-sweep.log、
/tmp/p4-sweep3.log 等历史日志，/tmp 清理后需重跑）
```

### 3.7 复现命令

见第 0.3 节（Linux 构建/专项/扩展/UBSan/TSan/导出符号/反例编译 + Windows 构建/回归）。
所有 ASan 测试须带 `ASAN_OPTIONS=detect_leaks=1:halt_on_error=0` 与 `--fail-fast`；
TSan 构建用独立目录 `agent/build/linux-tsan`（TSan 与 ASan 运行时互斥）；
Windows 运行 `agentxx_test.exe` 必须把工作目录设为 `exec/`。

---

## 4. 提交边界与工作树状态

```text
基线（重构前）   a805f9cb  --
提交 1          3a4497ba  重构插件框架-R1-1 Runtime / Operation 部分实现   (2026-09-09 14:00 +0800)
提交 2          f861bcf9  重构插件框架-fix-build                          (2026-09-09 18:32 +0800)
提交 3          b2b5114a  重构插件框架-推进 Operation / Runtime 可靠性、加载事务 / 关闭 / owner 顺序、ABI v1 / SDK
                          (2026-09-11 02:15 +0800, 31 文件, +2779/-835)
提交 4（P0-1）  c2869f07  重构插件框架-P0-1 宿主控制块与迟到调用安全失败            (16 文件)
提交 5（P0-2）  8c717236  重构插件框架-P0-2 Operation 终态与取消线性化              (5 文件)
提交 6（P1-2）  aa4b33ff  重构插件框架-P1-2 C17 ABI 编译期检查与接口表严格协商       (4 文件)
提交 7（P1-1）  4f8d1fdf  重构插件框架-P1-1 SDK 拥有型 Request 与统一 root adapter  (6 文件)
提交 8（P1-4）  9be9c735  重构插件框架-P1-4 启用/禁用事务与 prompt 贡献模型        (5 文件)
提交 9（P1-3）  a321267c  重构插件框架-P1-3 Client 语义渲染缓存与动作代次           (12 文件)
提交 10（P2-1a）dfe04a6a  重构插件框架-P2-1a example_plugin 双端 start/stop 迁移与导出符号校验 (7 文件)
提交 11（P2-1b）ff6fc990  重构插件框架-P2-1b JS Promise 终态映射与设计文档更新      (5 文件)
提交 12（P2-1c）3143ef92  重构插件框架-P2-1c 多实例可变静态审计                     (3 文件)
提交 12.1       1561a11b  重构插件框架-状态总览对齐 P2-1 进展                     (仅本文档)
提交 13（R1-2） cdbcc738  重构插件框架-R1-2 生命周期回归补全与启停/卸载竞态修复    (3 文件)
提交 14（R4-1） 460d35f5  重构插件框架-R4-1 example 正例迁移、CancelToken 接入与后台任务托管 (8 文件)
提交 15（R4-2） 2a53977f  重构插件框架-R4-2 JS callTool Promise 化、顶层异常事务与 hook 真实完成 (4 文件)
提交 16（R4-3） 3e76a143  重构插件框架-R4-3 capability 异步 Task 与批量 start/stop 迁移 (8 文件)
提交 16.1       82096e58  重构插件框架-更新交接文档至 R4-3 状态与最终回归结果      (仅本文档)
提交 17（R3-2） 0bab72e3  重构插件框架-R3-2 graph node 统一 root adapter、SDK 反例编译检查与句柄回归 (8 文件)
提交 18（R2-3） e96f8f1b  重构插件框架-R2-3 客户端句柄安全、事件退订复查与关闭取证补全 (4 文件)
提交 19（R2-4） d6ae39cd  重构插件框架-R2-4 加载期 start 失败的真实 DSO 回滚用例    (5 文件)
提交 20（R2-5） 75b01a56  重构插件框架-R2-5 注册事务完成度：hook/能力/资源与客户端 UI 回滚 (6 文件)
提交 21（R6-1） 7a44e76d  重构插件框架-R6-1 插件框架定向 UBSan 探针               (5 文件)
提交 22（R5-1） 43c93ff6  重构插件框架-R5-1 Client 语义渲染缓存容量上限             (5 文件)
提交 23（状态同步）88166943 重构插件框架-实施状态对齐提交 22 进展                    (仅文档)
提交 24（R4-4） 10cb3ae2  重构插件框架-R4-4 四个内置插件 start/stop 迁移与 cpu_gpu 用例修复 (7 文件)
提交 25（设计补记）c58ce848 重构插件框架-补记 JS 引擎迁移设计要点                    (仅本文档)
提交 25.1      d507d90b  重构插件框架-交接文档提交计数与边界表校正                  (仅本文档)
提交 26（整理版）d507d90b+  重构插件框架-交接文档按已完成/待实现重组                    (仅本文档)
提交 27（R4-5） d3dbd909  重构插件框架-R4-5 JS 引擎与脚本壳插件 start/stop 迁移       (5 文件, 2026-09-11)
                          —— 本提交前 `plugin.md`/`work.md` 的整理版改动由本任务文档提交一并入库
提交 28（R6-2） fc3d2c9b  重构插件框架-R6-2 定向 TSan 回归与数据竞争修复            (7 文件, 2026-09-11)
                          —— 修复 log sink 成员顺序、测试顺序探针同步、
                             system_monitor GPU/PDH/查询实例化；work.md/plugin.md 同步
提交 28.1       a5f8a6e1  重构插件框架-R6-2b 提交 28 验证结果补记                    (仅本文档)
提交 --/--      11658fd7  --（用户提交: TODOS.md + agentxx-config.yaml）
提交 --/--      e2a7c247  fix windows build（用户提交: exception.h / test CMake / 测试跨平台化）
提交 29（R6-3） 91b345a9  重构插件框架-R6-3 Windows 平台验证与平台 gate 收敛        (10 文件, 2026-09-11)
                          —— Windows 全插件构建 + 插件专项 1765/0 + 扩展 2251/0 +
                             工具模块 360/0；test_plugin_runtime 有界等待修复（Windows
                             IOCP 定时器投递时序）；audio_stream 改为全平台跳过；
                             work.md/plugin.md 更新为"Reset-v1 重构完成"
提交 29.1       0667426b  重构插件框架-R6-3b 提交 29 记录补记                     (仅本文档)
提交 29.2       9ac97ca5  重构插件框架-R6-3c 提交边界表补入记录补记提交           (仅本文档)
提交 29.3       7fbbf3ba  重构插件框架-R6-3d 补记两个工作副本的同步注意事项        (仅本文档)
提交 29.4       c08d8c6c  重构插件框架-R6-3e 提交边界表补入提交 29.3              (仅本文档)
提交 30（整理） [本次提交] 重构插件框架-交接文档按已完成/待完成重组                  (仅本文档, 2026-09-11)
                          —— 第 1 节只放已完成（R0-R6 + 证据）、第 2 节只放 4 项非阻塞待完成；
                             非插件模块 TSan 告警与 Android 平台验证移出待办范围
工作树          提交 30 后：代码/文档修改均已入库；仅剩构建副产物与用户既有改动
                （`agent/third_party/fmt`(untracked)、`libiconv-native`/`liburing`
                 (submodule 脏标记)），勿回退/勿清理
```

说明：提交 3 及以后全部建立在 `b2b5114a` 之上；`b2b5114a` 包含当时的用户改动
（`TODOS.md` +3、本文件重写），`3a4497ba` 包含 `agentxx-config.yaml` 与 `index.md`。
新会话不要重写/压缩这些提交，也不要回退用户文件；提交 29 建立在提交 28 与两个
用户提交（`11658fd7`、`e2a7c247`）之后。

---

## 5. 已知风险与遗留缺陷（开工前必读）

1. **控制块 tombstone 固定内存代价**：`PluginHostControl` 按实例累计、永不回收
   （地址不复用是安全前提，每实例约 100 字节）。仅在"反复加载卸载上万次"场景需要
   评估池化/压缩，不能改为复用地址（见 2.4 第 4 项）。
2. **同步析构兜底保留 DSO**：stop 未完成或 lease 非零时实例保持 `CloseFailed`
   并保留 ctx/DSO（日志明确报错）。这是安全兜底而非最终形态，owner 必须先 await
   `shutdownAsync`；`AgentHost::destroyAgent`（同步）只告警不阻断，新代码应使用
   `destroyAgentAsync`。
3. **JS 引擎空闲判定依赖 `busy_` 不变式**：`JsEngine::requestStop` 在"无在手中任务、
   队列为空"时才在调用线程直接 join JS 线程；该不变式要求 JS 线程取任务/执行定时器
   与置忙在同一临界区内完成（代码中已注释）。若后续改动 JS 线程循环，必须保持该
   不变式，否则会重新引入"IO 线程 join 与 JS 线程回调互等"的自锁面。
4. **依赖插件启用事务顺序边界**：见 2.1（失败→Disabled 可重试，不影响契约）。
5. **Windows 桌面交互未做人工回归**：`computer_use` / `text_selection_monitor` 只有
   接口级用例（见 2.3）；`agentxx_audio_stream` 无实现（已按 gate 全平台跳过）。
6. **构建环境脆弱点（Linux）**：GCC 16.1 偶发 ICE 后 build 目录可能残留残缺 `.o`，
   链接器（mold/lld）会直接 SIGSEGV 而非报错。排查手法：把 `<build>/.../link.txt`
   里的链接器换成 `-fuse-ld=bfd` 重跑，bfd 会指出坏目标文件；删除该 `.o` 重编即可。
   不要为此清空整个 build 目录。
7. **构建环境脆弱点（Windows）**：命令行 `cmake --build ... --parallel 8` 偶发
   `MSB6006: CL.exe 已退出，代码为 1` + `MSB4166 子节点过早退出`（无 `error C####`，
   属编译器进程被资源压力终止）；重跑即通过。运行 `agentxx_test.exe` 必须把工作目录
   设为 `exec/`（见 0.3、1.10 第 3 条）。
8. **可选遗留**：`onToolRenderUpdated` 整表重绘；TUI 组件级旧快照用例、
   create 失败回滚独立用例缺失（均见 2.4，不阻塞验收）。

---

## 6. 下一步执行清单

```text
[x] 1. 复跑基线确认环境（提交 27 前为 1630/0，7 模块）
[x] 2. JS 三件 start/stop 迁移（提交 27：引擎层 start/stop + 三个壳插件，语义 B）
[x] 3. JS 迁移后回归：plugins 392 / plugin_multi_instance 80 / client_plugins 421 / cpu_gpu 25
       —— 新增 disable→enable 往返、紧邻 disable+enable、双实例隔离、shutdownAsync 收尾
[x] 4. TSan 定向回归（agent/build/linux-tsan）：插件框架 0 告警，3 处竞争已修复（3.5 节）
[x] 5. 扩展回归（14 模块）复跑：2179/0（ASan+LSan）
[x] 6. Windows 平台验证（提交 29）：全插件构建 + 专项 1765/0 + 扩展 2251/0 +
       工具模块 360/0；audio_stream 收敛为全平台跳过；test_plugin_runtime 有界等待修复
[x] 7. 完成标准满足：plugin.md 状态更新为"Reset-v1 重构完成"，本文件第 1/2 节按
       已完成/待完成重组（提交 30）
[ ] 8. 后续（非阻塞，按价值取舍；详见第 2 节）
       - 2.1 依赖插件启用事务串行化（依赖者 start 串到依赖启用事务之后）
       - 2.2 业务插件迁移到宿主 HTTP/process 服务
       - 2.3 平台交互人工回归（computer_use / text_selection_monitor）
       - 2.4 可选收尾项（create 失败 DSO、TUI 组件级用例、按消息块失效、
              tombstone 内存评估、关闭超时取证）
```

每一步完成后：跑对应模块回归、更新本文件第 1/2/3 节、执行 `git diff --check` 与
`git status` 检查，再提交。

---

## 7. 提交与文档维护规范

- 提交信息格式：`重构插件框架-<内容总结>`。**本任务（Reset-v1 重构）已完成**：
  后续提交不再标注"（Reset-v1 未完成）"；如再开子任务，沿用
  "重构插件框架-<子任务>" 前缀，并在提交说明中写清是否影响"重构完成"判定。
- 新提交一律建立在**最新提交**之上；不要重写、回退或压缩已有重构提交；
  不修改或回退 `TODOS.md`、`agentxx-config.yaml`、`index.md` 等已入库的用户改动。
- 提交前必做：`git diff --check`、构建、相关模块回归、`git status` 确认只包含
  本任务文件。
- 文档维护：每完成一个阶段，更新本文件第 1 节（已完成）与第 2 节（待完成），
  以及第 3 节验证记录与第 4 节提交边界；不要只改代码不改文档。
- 事实来源优先级：plugin.md > 本文件；两者冲突时以 plugin.md 为准，并回写本文件。
