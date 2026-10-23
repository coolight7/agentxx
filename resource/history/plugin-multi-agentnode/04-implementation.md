# 04 实施规划

## 1. 阶段划分

每个阶段结束必须：编译通过 + 相关测试模块全绿。阶段之间不叠批，各自独立提交。

```
阶段 A  契约基础与历史包袱清理（新类型/新表/新入口、版本与基线、删旧分支；不动现有签名）
阶段 B  宿主化与挂载（PluginManager → per-host、PluginMount、mount/unmount 入口、插件注册搬 mount）
阶段 C  agent 引用显式化（逐表加 `PluginxxScopeRef` 参数，删掉 B 的临时"正在挂载"状态）
阶段 D  宿主接线与配置（AgentHost、五处启动路径、plugin.mount、删 attachRootAgentHost）
阶段 E  SDK 与示例（AgentRef/MountStore、mount 表便捷方法、示例插件、端到端测试）
阶段 F  会话定位与诊断（存储根冲突拒绝、挂载矩阵、日志）
阶段 G  文档清理与最终核对
```

为什么这样排：签名改动（C）必须排在挂载存在（B）之后 —— 没有挂载就没有可传的引用；
而 B 又必须先有 `PluginMount` 这类基础设施（A）。分三步走，每步都能编译能跑。

### 阶段 A：契约基础与历史包袱清理

**目标**：把新机制先立起来、把历史分支删掉，但**不动任何现有签名**（插件照旧能编）。
契约版本号在 A 的最后一步才翻，避免开发期把旧插件全拒掉。

**改动**：

1. `pluginxx/api/abi.h`：新增引用结构体（`02` §3.2）：

   ```c
   typedef struct PluginxxScopeRef { uint32_t struct_size; uint32_t flags;
                                     PluginxxStringView id; uint64_t seq; } PluginxxScopeRef;
   #define PLUGINXX_SCOPE_REF_BASELINE_SIZE sizeof(PluginxxScopeRef)
   static inline int pluginxx_scope_ref_ok(const PluginxxScopeRef* r);
   ```

2. `pluginxx/kit/kit.h`：SDK 值对象 `pluginxx::AgentRef`（持有 id 字符串 + seq，
   提供 `id()` / `seq()` / `raw()`）。
3. `pluginxx/api/tables.h`：`pluginxx.cancel` 表尾追加 `op_cancel`（`VERSION` 1→2、
   `MIN_VERSION` 保持 1、`BASELINE` = `offsetof(..., op_cancel)`）；其余 9 张不动。
4. `pluginxx/api/iface_meta.h`：`IfaceMeta<PluginxxCancelIface>` 基线改 `offsetof`。
5. `agentxx/plugin/api/plugin_api.h`：
   - 新增 `agentxx.agent.mount` 表（`list` / `get`，第 1 代）；
   - 新增 `AgentxxPluginAgentMountSpec`（带 `struct_size`/`flags`/`mount_seq`）；
   - 新增 `AGENTXX_PLUGIN_ERR_NO_AGENT`（-2），删除 `ERR_NO_MOUNT`；
   - 新增入口符号名 `agentxx_plugin_agent_{mount,unmount}`（`entry.h`）。
6. `agentxx/plugin/api/plugin_iface_meta.h`：`mount` 表特化。
7. `agentxx/plugin/api/plugin_spec.h`：`MountSpec` 基线特化。
8. `plugin_kit.h`：宏统一为 8 参数（第 7/8 个可传 `nullptr`）；`AgentIfaces::mount`；
   引入 `AgentRef`。
9. 宿主 `PluginManager`：实现 `mountOf(ref)`（本阶段：ref 的 id 等于本管理器的 agentId 才算通过，
   否则 `ERR_NO_AGENT`），实现 `mount` 表的 `list`/`get`（本阶段只有一个挂载可报）。
10. 历史包袱清理（`01` §4.3）：删 `PromptBackup`/`restorePromptBackup`/`promptKeys_` 的
    base/applied 模型；钩子合并为句柄式一种；`tools`/`feature` 的 `op_cancel` 调用点改指
    `pluginxx.cancel`；删 `attachRootAgentHost` 之外的"宿主收养"分支；删 legacy 配置键告警。
11. 全部 20 个插件 + `plugin_kit.h` 示例：`op_cancel` 调用点换表、钩子换句柄式、
    导出宏补 `nullptr, nullptr`（挂载入口稍后再实现）。
12. 最后一步：`PLUGINXX_API_VERSION` 1→2、`PLUGINXX_MIN_API_VERSION` 1→2；
    `pluginxx.cancel` 的 `MIN_VERSION` 维持 1（仍是纯追加）。

**验收**：全量编译通过；`plugin_runtime`（含"v1 插件被拒"）、`plugin_sdk`、`boundaries`
（台账加 `mount` 表与 `cancel` 追加段）、`plugins`、`plugin_hooks`、`plugin_feature`、
`plugin_resources`、`config_keys` 全绿；`cancel->op_cancel` 用 `ifaceHas` 可判。

**回滚**：独立提交；运行语义不变（还是 per-agent 管理器，注册还在 `start`）。

### 阶段 B：宿主化与挂载

**目标**：`PluginManager` 变进程级宿主；`PluginMount` 承载 agent 侧实体；插件注册搬进 `mount`。
本阶段为了让注册能定位目标，临时保留一个"正在挂载的 entry"（只有宿主内部可见），
阶段 C 用显式引用替换并删除。

**改动**：

1. `PluginManager` 构造只取 executor；新增 `createShared` / `createSelfHosted`；
   成员拆成"宿主部分"与"挂载部分"（`02` §2.1）。
2. 新增 `plugin_mount.h`：`PluginMount` / `Entry` / `mountSeq()` / `findEntry()`；
   成员从管理器搬过来（钩子表与派发器、轮次状态、提示词贡献、图定义占用、撤销记录）。
3. `AgentContext` 加 `pluginHost` / `pluginMount`；`pluginManager` 保留为宿主别名；
   删 `shutdownPluginsAsync`。
4. `BaseAgent::init`：`plugin_system` 取/建宿主；`plugins_load` 改名 `plugin_mount`，
   调 `mountAgentAsync(ctx)`；轮次钩子与关闭阶段③ 改指挂载。
5. `PluginInstance` 解析 `mount`/`unmount` 入口；`mountOneAsync` 分配 `seq`、填 `MountSpec`、
   驱动 mount 操作；失败只回滚该 agent。
6. 注册类入口：暂时用"正在挂载的 entry"定位（宿主在驱动 mount 期间设置，`mount` 返回后清），
   其余场景返回 `ERR_NO_AGENT`；B 结束时应只剩"插件在 start 里注册"这一处过渡失败路径。
7. 20 个插件：`start` 里的注册搬到 `mount`，`mount` 返回 per-agent 状态（`03` §7）；
   `probe`/`planning` 这类无 per-agent 状态的插件传 `nullptr, nullptr`。
8. `assembly_snapshot` 改两段（宿主 + 本挂载）。

**验收**：`plugin_host_mount` 用例 1~8、10~12 绿；`plugin_multi_instance` 改为"两宿主各一实例"；
20 个插件全部迁完，TUI 手工冒烟可用。

### 阶段 C：agent 引用显式化

**目标**：按 `02` §3 给每张表的 agent 级成员加 `const PluginxxScopeRef*`，宿主用 `mountOf()`
解析；删掉 B 的临时状态。

**改动**：

1. 逐表推进（每张表一个提交，改完即编译 + 跑对应测试模块）：

   | 顺序 | 表 | 改动 |
   |---|---|---|
   | 1 | `agentxx.agent.mount` | `get` 的 id 参数保持；`list` 不变 |
   | 2 | `agentxx.agent.model` / `prompt` / `resources` | 读取类加 `agent` |
   | 3 | `agentxx.agent.session` / `context` | 读取类加 `agent` |
   | 4 | `agentxx.agent.permission` | `register/unregister/check_paths` 加 `agent` |
   | 5 | `agentxx.agent.graph` | 注册 + 读取加 `agent` |
   | 6 | `agentxx.agent.feature` | 全部成员加 `agent` |
   | 7 | `agentxx.agent.tools` | 全部成员加 `agent` |
   | 8 | `agentxx.agent.hooks` | 全部成员加 `agent` |
   | 9 | `pluginxx.events` | `subscribe` 加 `agent`（可空） |
   | 10 | `pluginxx.capabilities` | `invoke_capability_async` 加 `agent`（可空） |
   | 11 | `pluginxx.config` | `get_session_work_dir` 加 `agent` |
   | 12 | `pluginxx.cancel` | `is_cancelled` 加 `agent` |

   每张表改完：`VERSION` → 2、`MIN_VERSION` → 2、`BASELINE` = 新 `sizeof`；
   `IfaceMeta` 基线同步；`boundaries` 台账更新。

2. 宿主：`mountOf()` 成为唯一入口（`02` §3.3）；所有实现去掉"正在挂载的 entry"，
   失败返回 `ERR_NO_AGENT`（注册类）/ `-1`（读取类）+ WARN。
3. 工具对象 / 钩子处理器 / 功能点 shim / 图节点 / 事件源：创建时保存 `AgentRef`，
   调用插件前传给接口（不再需要压栈）。
4. 20 个插件：所有 agent 级调用补 `AgentRef`（`03` §7 第 3~4 条）；per-agent 状态存 `AgentRef`。
5. 删除：`mountStack_` / `MountScope` / `lastMountHint` / `resolveBySession` / `rootMount`
   （`02` §7.4）。
6. `pluginxx.config.get_language`/`set_language`、`scheduler`、`tasks`、`json`、`log`、`plugins`
   保持无 agent 参数（`02` §3.5）。

**验收**：`plugin_host_mount` 全 14 例；新增"传错 agentId / 过期 seq 被拒"用例；
`boundaries` 逐表校验版本四件套；20 个插件全部通过。

### 阶段 D：宿主接线与配置

1. `AgentHost::Config` 加 `pluginList`；`AgentHost` 加 `pluginHost()` / `loadPluginsAsync()`；
   删除 `attachRootAgentHost`（连同 4 处调用点改新顺序）。
2. 五处启动路径按 `02` §9.2 改造（`mode_runners.cpp`、`jsonl_mode.cpp`、`main.cpp`、
   `ffi_runtime.cpp`、benchmark）。
3. 配置：`AgentConfig::plugins` → `pluginLoadList`；新增 `pluginMountPolicy` / `pluginMounts`；
   `config_loader.cpp` 解析 `plugin.mount`；`makeSubagentConfig` 设 `None`；`validate()` 校验挂载项。
4. `destroyAgent/Async` 先拆挂载再移除节点；`AgentContext` 析构拆挂载 + 告警。

**验收**：TUI / CLI / jsonl / server / FFI 冒烟；`plugin_mount_policy` 用例（all / none / listed /
未知名 / 参数覆盖）；子代理不挂载断言。

### 阶段 E：SDK 与示例

1. `PluginBase` 便捷方法：`listMounts()` / `mountInfoOf()`（没有"当前挂载"版本）。
2. `MountStore<T>`（`03` §4.2）；`AgentRef` 与工具/钩子注册 helper 串起来
   （`tool(ctx, ref, name, ...)` 这类带引用的重载）。
3. 示例插件 `agent/plugins/example_multi_mount`（计数器），供测试与文档引用。
4. `plugin_multi_mount` 端到端用例（§3.2）。

**验收**：`plugin_multi_mount` 全绿；`plugin_sdk` 覆盖新 API。

### 阶段 F：会话定位与诊断

1. `AgentHost` 在挂载 agent 时比对规范化后的会话存储根，相同则 ERROR + 拒绝（`02` §7.3）。
2. `list()` 增加挂载字段；加载快照加 `plugins_agent` 段；`--dump-diagnostics` 加挂载矩阵。
3. 日志按 `02` §12 补齐（拒绝路径都带 agentId / seq）。

**验收**：两 agent 同存储根 → 第二个被拒 + ERROR；不同存储根同名会话 → 各自可用；
`--dump-diagnostics` 含挂载矩阵。

### 阶段 G：文档清理与核对

1. `docs/zh-cn/design/plugins.md`：
   - §2 “接口表独立演进/版本限制” → “ABI 换代：`MIN_API` 1→2 + 14 张表 `VERSION`/`MIN`→2、IID 不变；追加成员仍用 `ifaceHas`”；
   - §8 表一览：21 张，标出 14 张 v2 表与新增的 `mount`；
   - §17 "接口表版本与基线" → 改写成"怎么改接口（改了就升版本）"；
   - 新增 §18 "插件宿主与多 agent 挂载"（浓缩 `02`/`03`）；
   - §15 生命周期加 `mount`/`unmount`；§8 的权限段补"接口带 agent 引用"。
2. `docs/zh-cn/design/index.md` §6/§7：补"插件宿主归 AgentHost"。
3. 根 `AGENTS.md`：重写"接口表版本与基线"整段；插件段补"一次加载/多挂载"与
   "agent 级接口显式带 agent 引用"；接口表数量 21；删掉已不成立的历史叙述。
4. 代码注释里的历史解释（"兼容旧契约""不再区分""重置前"）全部改写为当前事实。
5. `plugins.md` §15.7（注册可逆）与 §13（示例索引）同步。

**验收**：文档交叉引用可点开；`ERR_NO_MOUNT` / `mountStack_` / `.v2` IID 字串零命中；
`boundaries` 校验表数量 21 且四件套齐全。

## 2. 文件级改动清单

| 文件 | 阶段 | 改动 |
|---|---|---|
| `third_party/cxx_pluginxx/include/pluginxx/api/abi.h` | A | API/MIN → 2；新增 `PluginxxScopeRef` + 基线 + `pluginxx_scope_ref_ok` |
| `.../pluginxx/api/tables.h` | A/C | `cancel` 追加 `op_cancel`；4 张表加 `agent` 并升版本 |
| `.../pluginxx/api/iface_meta.h` | A/C | `cancel` 基线 offsetof；4 张变动表基线改新 `sizeof` |
| `.../pluginxx/kit/kit.h` | A/E | `validateInterface` 不变；新增 `AgentRef` |
| `.../pluginxx/host/lifecycle.h` | A | 判定不变（`>=`）；确认 `MIN=2` 生效（动态库 + builtin 两条路径） |
| `agentxx/plugin/api/plugin_api.h` | A/C | 10 张表加 `agent`；tools/feature 删 `op_cancel`；hooks 合并 3 成员；`mount` 表；`MountSpec`；`ERR_NO_AGENT` |
| `agentxx/plugin/api/plugin_iface_meta.h` | A/C | 各表 `IfaceMeta` 基线 |
| `agentxx/plugin/api/plugin_spec.h` | A | `MountSpec` 基线；`specSizeOk`/`specHas` 不变 |
| `agentxx/plugin/api/plugin_kit.h` | A/C/E | 8 参数宏、`AgentIfaces`、`AgentRef`、带引用的 helper |
| `agentxx/plugin/api/client_plugin_api.h` | A | 客户端 API 与 MIN 提到 2；表布局不动 |
| `agentxx/plugin/plugin_manager.h` | A/B | 删 PromptBackup；宿主成员；拆出 `plugin_mount.h` |
| `agentxx/plugin/plugin_mount.h`（新） | B | `PluginMount` / `Entry` / `mountSeq` |
| `src/plugins/plugin_manager_adapters.cpp` | B/C | 工具/权限/图：`mountOf(ref)` 定位后注册 |
| `src/plugins/plugin_manager_lifecycle.cpp` | A/B | 版本判定、禁用=全挂载卸载、删收养、`mount` 入口 |
| `src/plugins/plugin_manager_hooks.cpp` | A/B | 钩子表搬挂载、合并入口、带引用 |
| `src/plugins/plugin_manager_feature.cpp` | B/C | 挂载内登记 |
| `src/plugins/plugin_manager_vtable.cpp` | A~E | 各表实现、`g_ifaceMount`、`mountOf`、错误码 |
| `src/plugins/plugin_manager_capability.cpp` | A/C | `invoke_capability_async` 的 `agent` 归因 |
| `src/plugins/plugin_manager_domain_hooks.cpp` | A/B/F | 事件源按挂载 |
| `src/plugins/plugin_graph_node.{h,cpp}` | B/C | 节点持 `AgentRef` |
| `agent/lib/include/agentxx/agent/context.h` | B | `pluginHost` / `pluginMount` |
| `agent/lib/src/agent/base_agent.cpp` | B/D | `plugin_system` / `plugin_mount` / 轮次 / 关闭 |
| `agent/lib/src/agent/agent_host.{h,cpp}` | D/F | `pluginList`、`loadPluginsAsync`、删 `attachRootAgentHost`、存储根校验 |
| `agent/lib/src/agent/assembly_snapshot.cpp` | B/F | 两段快照 |
| `config.h` + `client/src/config_loader.cpp` | A/D | `pluginLoadList` / `plugin.mount`；删 legacy 告警 |
| `client/src/mode_runners.cpp`、`io/jsonl/jsonl_mode.cpp`、`main.cpp`、`ffi/ffi_runtime.cpp` | D | 启动顺序 |
| `agent/plugins/**`（20 个） | A/B/C | 宏 8 参数、hooks 句柄式、`op_cancel` 换表、注册搬 `mount`、调用补 `AgentRef` |
| `agent/plugins/example_multi_mount`（新） | E | 示例插件 |
| `agent/test/**` | A~F | 见 §3 |

> 新增源文件不需要改 `agent/lib/CMakeLists.txt`（`GLOB_RECURSE CONFIGURE_DEPENDS`）。

## 3. 测试计划

### 3.1 新增 `plugin_host_mount`（协程组）

| # | 用例 | 断言 |
|---|---|---|
| 1 | 一宿主两 agent | `create/start` 各 1 次；实例 1 个；两 agent 工具都在 |
| 2 | 工具状态隔离 | A 的调用只影响 A 的状态 |
| 3 | 提示词只影响本 agent | 两 agent 各自合成，基础值不互覆盖 |
| 4 | 钩子只在本 agent 派发 | 只挂 A 的处理器，B 的轮次不触发 |
| 5 | 卸载一个 agent | A 的工具消失、B 正常；宿主未 `stop` |
| 6 | 宿主关闭 | `stop/destroy` 各 1；挂载表空 |
| 7 | mount 失败回滚 | 该 agent 无该插件，其他 agent 正常，ERROR 日志 |
| 8 | 事件按挂载隔离 | 只订 A 的插件不收 B 的事件 |
| 9 | **`start` 里注册被拒** | 返回 `ERR_NO_AGENT`，无副作用 |
| 10 | 重复挂载幂等 | entry 仍 1 条，不重复注册 |
| 11 | 工具名冲突 | 该 agent 跳过 + WARN，其他 agent 正常 |
| 12 | 三挂载 | 3 个 agent 各一份状态 |
| 13 | 版本不符 | 构造 `api_version=1` 的 `PluginxxInfo`，加载被拒 + ERROR |
| 14 | **引用校验** | 传 `NULL` / 未知 id / 过期 seq → 注册类 `ERR_NO_AGENT`、读取类 `-1`，无副作用 |
| 15 | 表查询 | 21 张表 IID 都能查到；`mount.list` / `mount.get` 内容正确 |

### 3.2 新增 `plugin_multi_mount`（端到端）

用 `example_multi_mount`：两 agent 各 `1`；A 连 3 次后 A=4/B=1；卸载 A 后 A 不可用 B 正常；
重新挂载 A 计数从 0，且用旧 `AgentRef`（旧 seq）调用被拒；
`plugin.mount[].args` 覆盖生效（不同 agent 拿到不同 `config_json`）。

### 3.3 改造现有模块

| 模块 | 改动 |
|---|---|
| `plugins` | `makeAgentCtx` 改用宿主 + 挂载 |
| `plugin_multi_instance` | 改为"两个宿主各一实例"（仍验证三铁律） |
| `plugin_cleanup` | 注册清单拆宿主/挂载两份 |
| `plugin_feature` / `plugin_hooks` / `plugin_resources` | 注册点迁到 mount；断言"挂载后可用、卸载后不可用" |
| `plugin_sdk` | 新宏、`AgentRef`、`MountStore`、`listMounts()` |
| `plugin_runtime` | 留 IID 查询、表数量 21、`mount` 表；补"v1 插件被拒"；17 处长度/版本用例按 v2 基线更新 |
| `boundaries` | 逐表校验四件套 + 成员顺序台账（含 14 张 v2 表与 `mount`）；表数量 21 |
| `config_keys` | `plugin.mount` 新键名；确认 legacy 键不再被识别 |

### 3.4 手工验证

- [ ] TUI：启动 → 插件工具可用 → 调用 → 退出无 "plugins still closing"
- [ ] `--server`：客户端连上、工具调用正常
- [ ] jsonl：一行输入正常回包
- [ ] FFI 两个句柄：各自插件互不影响
- [ ] 用 v1 头编译的旧插件：加载被拒并给出重编译提示

## 4. 迁移

| 对象 | 处理 |
|---|---|
| 仓库内 20 个插件 | 阶段 A/B/C 一起改完并重编 |
| 外部预编译插件 | 必须用 v2 头重编；加载时明确报错 |
| 用户配置 | `plugin.list` 语义不变；新增可选 `plugin.mount`；legacy 键不再被识别（不报错也不生效） |
| 客户端插件 | 跟随 v2 头重编（成员未变） |

没有"旧版本继续能跑"的目标：本次的验收标准是"全部同版本构建后可运行"。

## 5. 风险与对策

| 风险 | 表现 | 对策 |
|---|---|---|
| 阶段 A 触及所有插件 | 大面积编译错误 | 用编译错误驱动，先改头文件再逐个插件；每改完 5 个跑一次构建 |
| 表布局变更时忘改版本/基线 | 新插件在旧主程序上按旧布局调用 | `boundaries` 台账（逐表成员顺序 + 版本四件套）+ "v1 插件被拒"用例 |
| 阶段 B 与 C 同时动宿主与插件 | 难以定位回归 | B（宿主化，单 agent 等价）与 C（逐表加参数）分开提交，C 内每表一个提交 |
| B 的临时"正在挂载"忘记删 | 后续代码又依赖它 | C 的验收项含 `grep -rn 'mountStack_\|MountScope' agent/` 零命中 |
| 漏带 agent 引用 | 运行期 `ERR_NO_AGENT` | 用例 14 + `03` §7.2 自查清单 + 插件逐个冒烟 |
| 钩子顺序变化 | 用户可感知 | `seq` 保持宿主级单调；加"跨挂载顺序"断言 |
| 关闭顺序 | `dlclose` 时仍有回调 | 先拆全部挂载 → 等 inflight → stop/destroy（C1） |
| `boundaries`/`config_keys` 漏改 | 自检红 | 阶段 A/C/D 各跑一次 |
| 文档仍写旧机制 | 误导 | 阶段 G 用 `grep` 断言零命中 |

## 6. 工作量估算

| 阶段 | 代码（行，量级） | 测试 | 相对耗时 |
|---|---|---|---|
| A 契约基础与清理 | 删 ~500 / 增 ~400 | 更新 `plugin_runtime`/`boundaries` | 3 |
| B 宿主化与挂载 | ~800 | 新增 12 用例 | 4 |
| C 引用显式化 | ~600（12 张表 + 宿主 + 插件） | 新增 3 用例 | 4 |
| D 宿主接线与配置 | ~450 | 4 用例 + 冒烟 | 2 |
| E SDK 与示例 | ~350 | 5 用例 | 1 |
| F 会话定位与诊断 | ~200 | 2 用例 | 1 |
| G 文档清理 | 文档 ~1200 行 | 1 次全量核对 | 2 |
| 合计 | 净减少约 200 行（删 1900 / 增 1700） | 25+ 用例 | 17（相对单位） |

约 7~9 个工作时段；阶段 B 与 C 占一半。

## 7. 开工检查清单

1. 先跑基线并记录通过数：
   ```bash
   ./path/to/agentxx_test plugins plugin_runtime plugin_sdk plugin_cleanup \
       plugin_feature plugin_hooks plugin_resources boundaries config_keys
   ```
2. 阶段 A **不动现有签名**，只加新东西并删历史分支，保证可回滚。
3. 每阶段结束执行一次检查：
   ```bash
   # 被删除的概念应零命中
   grep -rn 'ERR_NO_MOUNT\|mountStack_\|MountScope\|lastMountHint\|resolveBySession' \
        agent/lib agent/client agent/plugins agent/test
   # 演进机制必须继续存在（不要删）
   grep -rn 'MIN_VERSION\|BASELINE_SIZE\|ifaceHas\|specHas\|validateInterface' \
        agent/lib/src/plugins | head
   ```
4. 不动 `agent/third_party/` 里的**非插件框架**代码；`cxx_pluginxx` 的改动只限本篇列出的项。
5. 新测试模块名：`plugin_host_mount`、`plugin_multi_mount`（`plugin_runtime` 已占用）。
6. 阶段 B / C 各自可编译可跑；C 内按表逐个提交。
7. 每改完一个内置插件，跑一次 `plugins` 模块。
8. 阶段 G 之后核对：文档里不再出现"旧插件仍可运行"；“版本/基线/追加成员”的说明必须保留并与代码一致。

## 8. 明确不做

- 不做"旧插件继续可用"的任何兼容层（也不换 IID：`02` §10.3）。
- 不做挂载级工具裁剪语法（插件在 `mount` 里自行决定）。
- 不把 MCP 连接上提为宿主级共享。
- 不改客户端插件的成员结构（只跟随 API/MIN 换代）。
- 不做 `agentId/sessionId` 显式寻址语法（显式 agent 引用已经解决定位问题，`02` §7.3）。
- **不做多根**（`AgentHost` 本期仍单根）：插件建根、会话通道、事件触发是下一篇 `../plugin-event/`；本篇只把 `02` §16 的 S1~S8 约定固定下来。
