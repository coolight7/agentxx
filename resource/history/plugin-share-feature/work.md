# 插件共享功能（功能点统一）— 实施记录

> 方案文档: [plan.md](plan.md)（7 个阶段，每阶段一个提交）
> 状态: **实施中** — 阶段 1~5 已完成

## 阶段进度总览

| 阶段 | 内容 | 状态 |
|---|---|---|
| 1 | 子系统骨架（`agentxx::feature` + 值缓存 + 开发者模式 + `feature_points` 测试） | ✅ 已完成 |
| 2 | 第一批核心点迁移（`countTokens` / `summarize`，行为不变） | ✅ 已完成 |
| 3 | C ABI 接口表 + kit（插件登记实现） | ✅ 已完成 |
| 4 | 对外开放调用与插件自定义点 | ✅ 已完成 |
| 5 | 钩子处理器清单与优先级 | ✅ 已完成 |
| 6 | 命令行与 FFI 接入 | ⬜ 待开始 |
| 7 | 第二批点与文档收尾 | ⬜ 待开始 |

---

## 阶段 1：子系统骨架（已完成）

### 已完成内容

**新增文件（lib 侧）**

| 文件 | 内容 |
|---|---|
| `agent/lib/include/agentxx/feature/feature.h` | 类型 / 选项 / 结果 / 错误码 / `ImplContext` / `ImplSpec` / `PointBase` / `ProvidePoint<TReq,TValue>` / `JsonProvidePoint` |
| `agent/lib/include/agentxx/feature/value_cache.h` | 值缓存：三策略（None / Latest / ByIdentity）+ 条数与字节双上限 + 按来源失效 |
| `agent/lib/include/agentxx/feature/registry.h` | `Registry`：点表 + 清单 + 插件面入口（`definePluginPoint` / `addImpl` / 按归属撤销） |
| `agent/lib/include/agentxx/feature/points.h` | 核心点常量 + `CountTokensRequest/Value` + `SummarizeRequest/Value` + `TokenEstimator` + `registerContextPoints` |
| `agent/lib/src/feature/feature.cpp` | 点基类实现：实现链（异常隔离 / 超时）、值缓存与置空落地、in-flight 去重、重入保护、统计、清单 |
| `agent/lib/src/feature/registry.cpp` | 点表与插件点命名空间校验、按归属批量撤销、清单 |
| `agent/lib/src/feature/points_context.cpp` | token 估算规则（一份实现）+ 请求/结果 JSON 编解码 + 上下文两点的核心实现 |
| `agent/test/include/agentxx-test/core/test_feature_points.h` + `agent/test/core/test_feature_points.cpp` | 测试模块 `feature_points`（114 项断言） |

**改动的既有文件**

- `agent/lib/include/agentxx/agent/context.h`：`AgentContext::features`（功能点注册表，随实例一份）
- `agent/lib/src/agent/base_agent.cpp`：新增 `feature_registry` 装配步（在 `event_bus` 之后、
  `middleware_context` 之前 —— 早于中间件装配，也早于插件装载），同时把配置里的开发者模式
  镜像到进程级只读标记
- `agent/lib/include/agentxx/agent/config_static.h`：`AgentConfigStatic::devMode`（启动期冻结）
- `agent/lib/include/agentxx/agent/config.h` + `agent/client/include/agentxx-client/config_loader.h`
  + `agent/client/src/config_loader.cpp` + `agent/client/main.cpp`：yaml `dev_mode`
- `agent/lib/src/ffi/ffi_runtime.cpp`：配置 JSON `devMode`
- `agent/test/core/test_config_keys.cpp` + `agent/schema/config-keys.json` +
  `docs/zh-cn/design/config-keys.md`：新键登记与生成物更新
- `agent/test/test.cpp`：注册 `feature_points` 模块

### 关键实现口径（照方案执行）

- **两层实现顺序**：`plugin` → `core`；层内 `(priority 升序, 登记顺序)`；
  默认优先级带 = 插件 `0` / FFI 宿主 `1000`；`priority` 越界裁剪到 `±100000`（覆盖默认带，
  保证声明方能显式排到宿主之后）并记警告，不拒绝登记。
- **没有 `allowPluginImpl`**：点天生接受外部实现；`callable` 只决定"能不能被外部调用"。
- **两个入口一份实现**：`ask()`（应用取值：写值缓存、记置空）与 `call()`（外部调用：
  只读值缓存、不记置空、不写调用方状态）。
- **超时两处都实现、默认 0 = 不限**：点声明方 `implTimeoutMs`（到点取消当次实现并按
  "没意见"继续链）、调用方 `timeoutMs`（到点返回 `failed`，`message` 为
  `timeout after <n>ms`）；实现自报 `defaultTimeoutMs` 与点的值取较小非 0 者。
- **异常隔离**：实现抛异常 / 回答不是合法 JSON / 回答是空对象，都只等于"没意见"，
  记一条警告后继续问下一个（统一经 `catchErrorAsync` 处理同步抛与挂起点之后抛）。
- **重入保护**：只对 `call()` 生效；同一 `(点, 调用方)` 未结束再来一次返回 `busy`；
  guard 放在协程作用域析构上，协程被取消也会释放。
- **in-flight 去重**：同一身份并发只跑一次实现；搭顺风车的一方按其自身 `timeoutMs`
  取舍（超时返回 `failed`）；发起方无论正常结束还是被取消，都会发布结果唤醒等待者
  （作用域析构兜底发布"失败"）。
- **统计与成功日志只在开发者模式**：`asks/calls/errors/lastBy/lastCaller/lastMs`；
  关闭时清单里**没有** `stat` 段（不是全 0）。
- **插件点**：id 必须落在 `plugin.<本实例插件名>.*`；本轮只允许 `provide`；
  声明期间恒可调、固定不缓存（`None`）；`removePointsOwnedBy` 撤点时连带撤实现。
- **核心点清单视图**：`implViews()` 虚函数让基类统一按 `(层, 优先级, 登记顺序)` 排序，
  清单里看到的顺序就是实际询问顺序（`effectiveBy` 同一份算法）。
- **热路径优化**：点没有插件实现时不生成请求 JSON（`fillRequest` 只在 `implCount() > 0`
  时序列化），核心实现直接拿借用指针（`ChainEnv::typedRequest`）。

### 测试结果

```
agentxx_test feature_points
--- feature_points done: passed=114 failed=0 (392 ms) ---
```

覆盖：顺序与默认带 / 越界裁剪 / 不设数量上限 / 异常隔离 / 置空（ask 记、call 不记、
摘除还原）/ 值缓存（命中、换身份、refresh、条数上限、字节上限、None 不产生条目、
call 前后不变、按来源失效）/ in-flight 去重 / 三道保护（caller 与 viaCall 入载荷、
自己是调用方不问、重入 busy）/ 两处超时 + 实现自报超时 / 清单字段 / 开发者模式门控 /
`agentxx.context.countTokens` 真实点（插件实现替换核心实现、摘除后回落）/
插件点（声明、命名空间校验、撤销）。

回归：`config_keys` 因新增 yaml 键 `dev_mode` 失败 → 登记键并重新生成
`config-keys.json` / `config-keys.md`，现 163/163 通过。全量测试跑完无其他失败。

### 注意事项 / 留给后续阶段

1. **`AskOptions` / `CallOptions` 定义顺序**：两个结构体在头文件里定义于
   `ProvidePoint` 之后（模板方法体里用到），改用普通具名结构体参数以避免依赖顺序。
2. **`ChainEnv::typedRequest` 借用指针**：只在一次 `runResolve` 的 await 期间有效；
   核心实现不得把指针存下来跨次使用。
3. **阶段 3 待办**：`agentxx.agent.feature` 表 + `plugin_manager_feature.cpp` +
   kit 糖；接口表数量 19 → 20（`kInterfaceTableCount` / `plugins.md` §8 /
   根 `AGENTS.md` 三处一起改）。

---

## 阶段 2：第一批核心点迁移（已完成）

### 已完成内容

**`agent/lib/include/agentxx/middlewares/summarization.h`**

- 新增成员 `std::shared_ptr<feature::TokenEstimator> tokenEstimator` 与
  `feature::ContextPoints featurePoints`，并给出只读访问器 `points()` / `estimator()`
  （供测试与诊断按"一份规则两个入口"校验）。
- `doSummarizeWithLLM` 的语义改为"功能点 `agentxx.context.summarize` 的核心实现：
  只产出摘要文本，不写回会话"；新增编排方入口 `summarizeViaPoint`。
- 新增 `countTokensViaPoint`（预算计算经功能点取数；拿不到值时回退同步快路径）。

**`agent/lib/src/middlewares/summarization.cpp`**

- 构造函数：用本中间件的系数构造 `TokenEstimator`（一份实例），并
  `registerContextPoints` 声明两点 + 登记核心实现（`estimator` 副本 + 压缩核心实现 lambda）。
- `countTokensForUtf8Str` → `tokenEstimator->estimateText(...)`（**转发**，删掉重复的估算循环）。
- `countTokens` → 系统消息按 `extraTokensPerMessage + estimateText`，消息交
  `tokenEstimator->estimateMessages`（逐项结果与原实现完全一致）。
- 自动压缩（`onModelcallRunFunc`）：上下文用量经 `countTokensViaPoint`；压缩经
  `summarizeViaPoint(sessionId, toSummarize, direct=false)`；压缩后超限兜底也改用同一取数入口。
- 手动压缩（`compactSessionContext`）：`oldTokens` / 新摘要请求 / `newTokens` 全部经功能点
  （`direct=true`）。
- **未改动**的编排：提示消息（append/更新 viewMessage）、`NodeInterrupt` 派生与 resume、
  写回形状（`[system] [user 压缩指令] [assistant 摘要]`）、硬截断兜底、冷却计数、
  落盘（`persistNow`）、上下文统计广播。
- `service.token.count` 同步服务保持 `countTokensForUtf8Str`（= 同一份规则），TPS 路径不受影响。

**`agentxx/feature/points.h` + `points_context.cpp`**

- `SummarizeRequest` 新增 `manual` 字段（进 JSON 编解码）：说明"算哪一份数据"之外的调用场景
  ——`manual=true` 表示 agent 空闲时的手动触发（核心实现直派子代理、不抛中断），
  `false` 为轮次内自动触发（走中断路径）。
- 身份计算改为**增量哈希**（`hash::fnv1a64` 逐段带种子链式计算），不再拼接可能几百 KB 的
  临时字符串（整段上下文每轮都要算身份，这一步在热路径上）。

### 测试结果

```
agentxx_test feature_points summarization   → passed=585 failed=0
agentxx_test summarization feature_points event_stream event_bridge agent plugins
             plugin_cleanup assembly_snapshot_io observability boundaries
             message_supplement persist_semantics → passed=1833 failed=0
```

新增守卫用例 `test_sync_path_matches_point`（`feature_points` 模块，断言 9 项）：
- 中间件 `countTokens`（同步快路径）与 `countTokensViaPoint`（功能点）结果必须相等；
- `countTokensForUtf8Str` 与 `tokenEstimator->estimateText` 结果一致；
- `AgentContext::features` 里两个核心点存在、来源为 `core`、`countTokens` 可调、`summarize` 不可调。

回归：`summarization` 462 项、`agent` 198 项、`event_stream` 34 项全部保持通过
（自动压缩 / 手动压缩 / 失败兜底 / 提示消息复用 / 写回形状行为不变）。

### 注意事项 / 留给后续阶段

1. **`SummarizeRequest::manual`**：这是唯一一处把"调用场景"放进请求的字段（不进身份）——
   核心实现的直派与中断两条路径必须由调用方说清，不能靠实现猜"当前是不是在轮次里"。
2. **插件实现的可见面**：预算计算（上下文用量、压缩触发、压缩后兜底、手动压缩前后 token 数）
   经功能点，因此插件 tokenizer / 压缩实现都会在这里生效；TPS 显示与内部裁剪
   （`fitSummaryMaxTokens` / `hardTruncate` / `splitRecentByTokenBudget`）仍是同步快路径，
   只用核心实现。
3. **阶段 3 待办**：`agentxx.agent.feature` 表 + `plugin_manager_feature.cpp` + kit 糖；
   接口表数量 19 → 20（`kInterfaceTableCount` / `plugins.md` §8 / 根 `AGENTS.md` 三处一起改）。

---

## 阶段 3：C ABI 接口表 + kit（已完成）

### 已完成内容

**接口表（`agentxx.agent.feature`，v1）**

- `agent/lib/include/agentxx/plugin/api/plugin_api.h`：新增
  `AGENTXX_PLUGIN_IFACE_AGENT_FEATURE`（IID = `agentxx.agent.feature`）、
  `AgentxxPluginFeaturePointSpec`（点声明：id / type / title / depict / args_doc /
  result_doc / impl_timeout_ms）、`AgentxxPluginFeatureImplSpec`（实现登记：point_id /
  priority / default_timeout_ms / `impl_start` / `impl_cancel` / user_data）、
  `AgentxxPluginFeatureIface`（7 个入口：`list_points` / `define_point` / `undefine_point` /
  `register_impl` / `unregister_impl` / `call_point_async` / `op_cancel`）；
  `plugin_interfaces.h` 加 `plugin_interfaces::AgentFeature` 常量。
- `agent/lib/src/plugins/plugin_manager_feature.cpp`（新文件）：接口表的宿主落地
  —— 实现登记（`pluginxx::OpCore` 驱动插件回调，与工具/钩子同一套操作协议）、
  点声明/撤销、清单、按名调用（在 IO 线程跑实现链并把结果 JSON 经完成协议回给调用方）。
- `plugin_manager_vtable.cpp`：新增 `g_ifaceFeature` 静态表 + 7 个 trampoline 入口 +
  `query_interface` 分支；`plugin_manager.h` 的 `kInterfaceTableCount` 19 → 20。
- 生命周期与记账：`PluginInstance` 新增 `featurePoints` / `featurePointImpls` 记录；
  `detachDomainRegistrations` 撤实现与点、`clearDomainRegistrations` 清记录；
  `registrationInventory` 统计生效中的 `featurePoints` / `featureImpls`（计入 `total()`）；
  `list()` 视图新增 `featurePointCount` / `featureImplCount`；
  装配快照新增插件条目 `feature_point_count` / `feature_impl_count` 与运行侧 `feature_points` 段。

**SDK（`agent/lib/include/agentxx/plugin/api/plugin_kit.h`）**

- `AgentIfaces::feature` + `query()` 查询。
- `FeaturePointSpec` / `FeatureImplOptions`；`defineFeaturePoint` / `undefineFeaturePoint`；
  `provideFeature`（**同步 `std::string` 与协程 `Task<T>` 两种形态**）/ `unprovideFeature`；
  `listFeaturePoints`；`callFeature`（协程 Awaiter，结果 JSON 原样回，失败读 `ok` 字段）；
  新增 `featureAnswer(...)`：协程实现返回值的包装糖（与同步形态同语义）。

**示例插件（`agent/plugins/example_feature`，内置/动态双模式）**

声明三个点（`plugin.example_feature.beat` 同步实现 / `.slow` 协程实现 /
`.tooslow` 点声明 `impl_timeout_ms=50` 而实现等 400ms），并为宿主点
`agentxx.context.countTokens` 登记实现（返回固定值 `{"tokens":424242}`，便于断言）。

**测试（`plugin_feature` 模块，128 项断言）**

覆盖：声明与登记（归属 / 可调 / 实现层序 / 实例视图与注册清单计数 / 清单 JSON）、
插件实现覆盖核心点与摘除后回落（含按归属失效值缓存）、按名调用的各条结果路径
（同步实现 / 协程实现 / 实现超时按没意见继续 / 同归属调用不问自己的实现 /
强类型点拒绝受理 / 点不存在 / 无实现 / 显式置空）、`call` 的零副作用契约
（不写值缓存、不记置空标记）、并发重入 `busy`、禁用/启用/卸载三态可逆。

**改动同步**

- 接口表数量三处一起改：`PluginManager::kInterfaceTableCount`、
  `docs/zh-cn/design/plugins.md` §8（新增 `agentxx.agent.feature` 行 + 数量 19 → 20）、
  根 `AGENTS.md`（19 张 → 20 张，并补功能点体系一节）；
  `agent/test/core/test_boundaries.cpp` 规则 10（领域表 9 → 10）与规则 8（19 → 20）。
- `agent/test/plugin/test_plugin_cleanup.cpp`：注册清单基线补功能点计数（装载 0 /
  禁用 0 / `list()` 视图与 `registrationInventory` 同源）。

### 本轮修掉的真实缺陷（都由新测试暴露）

1. **协程实现收到悬垂入参**：`provideFeature` 的异步分支把 `impl_start` 栈上的
   `callText` 以 `string_view` 交给延迟启动的协程，协程恢复时该字符串已析构
   （ASan 报 heap-use-after-free）。改为传 Job 拥有的 `RootRequest` 副本。
2. **失败/取消的回包被当成回答**：插件实现失败或取消时回包里是错误文本，原先直接
   当回答解析（错误文本恰好是 JSON 时会被当成值）。改为非 OK 状态按"没意见"继续。
3. **协程实现与同步实现的回答形状不一致**：同步形态由 kit 自动把裸数据包成
   `{"value": ...}`，协程形态原先要求自己包。现在插件边界统一规范化
   （`normalizePluginAnswer`），并给 SDK 补 `featureAnswer()` 糖，两种形态写法一致。

### 测试结果

```
agentxx_test boundaries config_validation plugin_bridge feature_points plugins
             plugin_cleanup plugin_feature summarization
→ passed=1609 failed=0
   boundaries 13 / config_validation 47 / plugin_bridge 193 / feature_points 123 /
   plugins 541 / plugin_cleanup 102 / plugin_feature 128 / summarization 462
```

顺带修掉 `plugin_bridge` 两处时间敏感断言（本地 timer 到期 + 固定 `sleep_for` 后单次
驱动，在重负载下偶发失败）：改为按总时限重试（语义不变，只是不再依赖机器负载）。

### 注意事项 / 留给后续阶段

1. **实现超时不会取消插件侧的活**：`ImplSpec::cancel` 已存储但**当前无人调用**，
   点声明的 `implTimeoutMs` 到点只让调用方拿到 `no_impl` 并继续链，插件那边的操作
   仍会跑完（结果丢弃）。插件实现是 CPU 重活时这会白算一次 —— 阶段 4/6 接入
   命令行/FFI 调用时一并处理（要么在超时回调里调 `entry.cancel`，要么让
   `ImplOps` 强持 OpCore 后再取消），本轮先按现状记录，未列入阶段 3 范围。
2. **按名调用只支持通用 JSON 点**：强类型核心点（如 `agentxx.context.countTokens`）
   返回受理错误（需要类型化请求对象），插件改图/重写点后可经 `xx_*` 通道或用
   JSON 点中转；阶段 4 的对外调用入口按 JSON 请求解码补齐这条路径。
3. **调用方自己的实现不会被问到**（保护 ②）：同一插件内部要取值请直接调本地函数；
   SDK 文档已写明（`callFeature` 注释），否则会出现"自己的点调用返回 `no_impl`"的困惑。
4. **回答保留键**：`value` / `disable` / `verdict` 是回答的形状键 —— 业务数据里出现
   同名字段时会被当成回答解读（本轮示例数据就踩到过），SDK 文档已标注；
   阶段 7 收尾时把这条写进插件作者文档 (plugins.md 的"功能点"一节)。
5. **`struct_size` 守卫**：宿主侧目前未做 `struct_size` 校验（与既有表一致），
   新增字段扩版本时需一并补上（阶段 7 统一处理接口表版本升级约定时处理）。

---

## 阶段 4：对外开放调用与插件自定义点（已完成）

> 说明：本阶段的大部分内容在阶段 1/2（核心侧 `call()` / 三道保护 / 两处超时 /
> `callable` + `callDoc` / `countTokens` 声明为可调）与阶段 3（`define_point` /
> `undefine_point` / 插件点按名调用）已经落地；本轮补齐剩余缺口并补测试。

### 本轮改动

**`bad_args` 错误码真正产生（此前只是"声明了但没人产生"）**

- `agent/lib/src/feature/feature.cpp`：`runResolve` 新增第 1 步"参数形状"——
  `argsJson` 非空时必须能解析为 JSON，否则直接返回 `bad_args`
  （**不进实现链**，实现一次都不会被问到）；空串 = 不带参数，放行；
  形状不做限制（对象 / 数组 / 标量都由点的 `callDoc` / `argsDoc` 说明），
  强类型点的 `requestJson` 由 Codec 生成，恒为合法 JSON，不受影响。
- `agent/lib/src/plugins/plugin_manager_feature.cpp`：删掉"参数非法就清空 argsText"的
  旧处理（它让 `bad_args` 永远不可见，坏参数被静默降级成"没带参数"），
  改为原样交给点判定 —— 插件拿到的结果 JSON 里就是 `{"ok":false,"error":"bad_args"}`。

**清单进装配快照与 `--dump-diagnostics`**

- `agent/lib/src/agent/assembly_snapshot.cpp`：
  - 插件条目文本行补 `features=<生效点>/<生效实现>`；
  - 新增 `featurePoints[N] devMode=yes|no:` 段，逐条列出
    `id [origin] impls=N by=<生效实现> cache=<策略> callable=yes|no [disabledBy=…]`
    —— 排障时"哪个实现现在生效 / 被谁置空"一眼可见（`buildDiagnosticsText`
    复用同一份渲染，因此 `--dump-diagnostics` 与 `--dump-config` 都带这段）。

### 测试结果

```
agentxx_test boundaries observability plugin_bridge feature_points plugin_cleanup
             plugin_feature summarization
→ passed=1135 failed=0
   boundaries 13 / observability 99 / plugin_bridge 193 / feature_points 134 /
   plugin_cleanup 102 / plugin_feature 132 / summarization 462
```

新增断言：

- `feature_points`（+11）：`bad_args`（`call` 与 `ask` 两条入口、实现零调用）、
  合法 JSON 放行（对象 / 标量）、空串按"不带参数"处理；
- `plugin_feature`（+4）：插件按名调用传非法 JSON → 受理成功 + `bad_args`；
- `observability`（+3）：诊断包装配段含 `featurePoints[`、两个核心点 id 与
  `devMode` 标注。

### 注意事项 / 留给后续阶段

1. **参数形状只校验"是不是合法 JSON"**：形状（对象 / 数组 / 标量）不设限制，
   由点的 `argsDoc` 说明、实现自己判断 —— 否则插件点想收数组参数会被框架拦住。
2. **`bad_args` 与 `not_callable` 的顺序**：先判参数（`bad_args`）再判可调性
   （`not_callable`）—— 参数错是调用方最该先看到的问题。
3. **阶段 5（钩子清单与优先级）已完成** —— 见下方"阶段 5"章节；接口表新增一张
   （`agentxx.agent.hooks_ex`，agent 侧 20 → 21 张）。
4. **阶段 6（命令行 / FFI 接入）**：`feature` 子命令与 FFI 导出会走本阶段确立的
   `call()` 入口（按 JSON 请求解码），届时 `bad_args` / `not_callable` / 超时
   这些错误码会直接在命令行输出里出现。
5. **钩子清单也可作为排障入口**：`PluginManager::hooksJson()` / `handlersOf(point)`
   与 `list_feature_points` 同一层级，阶段 6 接命令行时可一并暴露
   （`--dump-diagnostics` 现在已经包含它）。

---

## 阶段 5：钩子处理器清单与优先级（已完成）

> 方案文档 §6（设计 D，586-684 行）+ §15 阶段 5（1320-1329 行）。
> 本轮**不改钩子语义**（载荷仍是 `{sessionId, point}`、结果仍丢弃、仍是 7 个固定点），
> 只补两件"能读、能排"的事。

### 与方案的一处偏离（重要）

方案 §6.2 主张在 `AgentxxPluginHooksIface` 表尾追加三项（`register_hook_ex` /
`unregister_hook_ex` / `list_hooks`），靠 `struct_size` 守卫兼容老插件。**这条在本仓库行不通**：

- SDK 的接口表校验是 `version != 1 || struct_size < sizeof(Iface)` 即**整表判为不可用**
  （`pluginxx/kit/kit.h` 的 `validateInterface`）；
- 表尾追加成员后，新插件编译出的 `sizeof(AgentxxPluginHooksIface)` 变大，老宿主返回的
  `struct_size` 更小 → `queryInterface` 返回 nullptr → 新插件在**老宿主上连基础的
  `register_hook` 一起丢掉**（能力"越用越少"）；
- 仓库已有明文约定：**新增能力一律走"新接口表或新能力名"，不在表尾追加成员**
  （`docs/zh-cn/design/plugins.md` §9 "版本约定" 与 `client_plugin_api.h` 的同类注释）。

**实际做法**：基础表 `agentxx.agent.hooks`（2 项）结构一字未动，扩展能力放进**新接口表**
`agentxx.agent.hooks_ex` v1（3 项）。插件把新表当可选能力：查不到就退回 `hook()`。
四个"接口表数量"位置由 20 同步为 21（`kInterfaceTableCount` / `plugins.md` §8 /
根 `AGENTS.md` / `boundaries` 规则 8+10）。

### 已完成内容

**C ABI / SDK**

- `agentxx/plugin/api/plugin_api.h`：
  - `AgentxxPluginHookSpecEx`（`struct_size` / `point` / `priority` / `flags` /
    `owner_tag` / `depict` / `hook_start` / `hook_cancel` / `user_data`）；
  - 新表 `AGENTXX_PLUGIN_IFACE_AGENT_HOOKS_EX = "agentxx.agent.hooks_ex"` v1：
    `register_hook_ex`（出参句柄）/ `unregister_hook_ex`（按句柄）/ `list_hooks`（清单 JSON）；
    表头注释写清"为什么不并在基础表里"。
- `agentxx/plugin/plugin_interfaces.h`：`plugin_interfaces::AgentHooksEx`（并说明它是
  **可选能力**：只用到基础两项的插件应声明 `AgentHooks`，否则会在只支持基础钩子的老宿主上
  被跳过加载）。
- `plugin_kit.h`：
  - `AgentIfaces::hooksEx`（一次查询，老宿主为 NULL）；
  - 把钩子执行体（`hookStart` / `hookCancel` 模板，含同步 void 与 `Task<T>` 两种形态、
    `RootRequest` 拥有型输入、`HookJob` 回收）抽成 `detail::` 共享模板 —— 基础 `hook()`
    与新增 `hookEx()` 只差"交给哪张表"，执行体只有一份；
  - 新增 `HookOptions{priority, ownerTag, depict}`、`hookEx()`（返回句柄，`0` = 宿主不支持
    或拒绝）、`unhookEx()`、`listHooks()`。

**宿主实现（新文件 `agent/lib/src/plugins/plugin_manager_hooks.cpp`）**

- **注册表**：`PluginManager::hookHandlers_`（`{句柄, 点, 优先级, 登记序号, 层, 归属,
  ownerTag, depict, load, 实例, 执行体}`），顺序 = `plugin` 层按 `(priority 升序, 登记序号)`
  → `core` 层同规则；`priority` 复用功能点的 `kPriorityMin/kPriorityMax`（±100000）裁剪并记警告；
  处理器数量不设上限。
- **单派发器** `PluginHookDispatchHandle`（中间件名 `plugin_hooks`）：首次登记时插入中间件链，
  没有处理器时移除（轮次执行中先置 `disabled` 跳过，轮末经 `flushPendingCleanup` 摘除 ——
  与旧的"每插件一个中间件句柄"同一套安全路径）。旧的按插件派发完全移除。
- **派发** `dispatchHook()`：开始派发时取一次排序快照（派发中登记/撤销不影响本次），
  逐个处理器执行前仍复核实例是否启用；插件层走操作协议（`awaitHostPluginOp`，失败只记日志），
  core 层在 io 线程同步调用（给一个"什么都不做"的完成通知器，返回操作句柄的按"core 层只支持
  同步处理器"记警告并请求取消）。
- **登记入口**：`registerHook`（覆盖式，`priority` = 插件默认带 0）/`unregisterHook`（按点，
  只影响基础登记）/`registerHookEx`（多处理器 + 句柄，带 `struct_size` 守卫）/
  `unregisterHookEx`（按句柄，校验归属）/`addCoreHookHandler`/`removeCoreHookHandler`。
- **清单** `hooksJson()`：`{devMode, count:7, handlers:N, points:[{point,name,count,handlers[]}]}`
  （7 个点都在，`stat` 段只在开发者模式出现）；`handlersOf(point)` 给 C++ 侧排序视图。
- **实例记录**：`PluginInstance::HookRegistration` 改成 `{point, handle, base}`
  （基础/扩展共用一份记录，`handle = 0` 表示已摘除），旧的函数指针与 `middleware` 字段删除。
- **生命周期**：`detachDomainRegistrations` 调 `detachHookHandlers`（摘注册表、记录留、句柄置 0）；
  `clearDomainRegistrations` 清记录；`detachDomainOwnedResources` 不再管理中间件句柄
  （派发器是宿主级单例）；`RegistrationInventory::middlewareAttached` 删除，
  `hooks` 改为"注册表里该实例的条数"，`PluginListView` 加 `hookPriorities`。

**接线与可观测性**

- `plugin_manager_vtable.cpp`：3 个 trampoline + `g_ifaceHooksEx` + `query_interface` 分支。
- `assembly_snapshot.cpp`：插件行加 `hook_priorities`；运行侧新增 `hooks` 段；
  文本渲染加插件行 `prio=[...]` 与 `hookHandlers[N] devMode=…:` 段
  （`点 (n): owner#handle[prio=…] -> … dispatches=… lastMs=… lastOrder=…`）。
- `plugin_manager.h`：`kInterfaceTableCount` 20 → 21（10 通用 + 11 领域）。
- 示例插件 `example_feature` 加一段 `hookEx` 用法（优先级 -10、`ownerTag`/`depict`，
  并注明老宿主降级路径）；`plugin.yaml` 把 `agentxx.agent.hooks_ex` 声明为 `optional`。

**文档**

- `docs/zh-cn/design/plugins.md`：§8 数量 20 → 21、架构图标注、新增
  `agentxx.agent.hooks_ex` 行。
- 根 `AGENTS.md`：接口表数量 21 + 新增"钩子处理器清单与优先级 (2026-10)"一节。

**测试**

- 新模块 `plugin_hooks`（`agent/test/plugin/test_plugin_hooks.cpp` +
  `include/agentxx-test/plugin/test_plugin_hooks.h`，`test.cpp` 注册；**156 项断言**）：
  1. 顺序：`plugin` 层按 `(priority, 登记序号)`、同优先级按登记顺序、`core` 层恒在后
     （负数优先级也越不过层）；
  2. 一个点多个处理器 + 按句柄精确撤销 + 别人的句柄被拒；
  3. 基础登记覆盖式、与扩展登记互相独立（撤基础不动扩展）；
  4. `priority` 越界裁剪到上下限（不拒绝登记）；
  5. 单派发器挂载/停用/摘除三态（含轮次中先停用、轮末 flush）；
  6. 实际派发顺序（插件层 → core 层）、单个处理器抛异常或返回操作句柄都不影响后续；
  7. 清单字段（`layer/owner/ownerTag/depict/priority/seq/enabled/load`）与
     开发者模式对 `stat` 段的开关；
  8. 禁用摘除 / 启用重登记 / 卸载回基线；
  9. 插件面：kit 的 `hookEx` / `listHooks` / `unhookEx` 经真实宿主视图走一遍。
- `test_plugins` 第 5/7/8/24 节与 DSO 回滚用例改写为新语义（按注册表与 `plugin_hooks`
  句柄断言）；`test_plugin_cleanup` 去掉 `middlewareAttached` 断言。
- `assembly_snapshot`（+11：`hooks` 段 7 点 / 空处理器 / 渲染行）、
  `observability`（+1：诊断包含 `hookHandlers[`）。

### 验证结果

- 主验证：`agentxx_test plugin_hooks plugin_runtime plugin_cleanup plugin_multi_instance
  boundaries` → **1016 通过 / 0 失败**（`plugin_hooks` 首跑 152 项，补 core 异步用例后 156）。
- 钩子相关回归：`plugins`(551) + `plugin_bridge`(193) + `plugin_sdk`(115) +
  `plugin_feature`(132) + `plugin_resources`(89) + `client_plugins`(657) +
  `assembly_snapshot`(37) + `assembly_snapshot_io`(41) + `observability`(100) +
  `config_keys`(163) → **2078 通过 / 0 失败**。
- 宽回归：`agent`(198) + `summarization`(462) + `shutdown_stages`(17) +
  `session_admin`(38) + `session_sync`(30) + `prompt_stability_io`(38) +
  `race_guards`(68) + `cancel`(45) + `toolcall_parallel`(48) + `checkpoint_store`(52) +
  `usage_ledger`(21) + `memgrowth`(15) + `feature_points`(134) + `config_validation`(47) →
  **1554 通过 / 0 失败**（`agent` 与 `memgrowth` 各要 2~3 分钟，与钩子无关，只是确认没被牵连）。
- 清单人工核对（临时断言，校验后已删）：`hookHandlers[2] devMode=yes:
  AGENT_START (2): plugin:example_feature#2[prio=-20] -> plugin:example_feature#1[prio=-10]
  dispatches=1 lastMs=1 lastOrder=plugin:example_feature#2, plugin:example_feature#1`。

### 注意事项 / 留给后续阶段

1. **规范偏离已落地并写成注释**：新能力走新接口表（不用表尾追加 + `struct_size` 守卫）。
   后续要给已有表加能力时照此处理；`struct_size` 守卫只在"入参结构体"上用
   （`AgentxxPluginHookSpecEx` 自己带了，非 0 且过小时拒绝登记）。
2. **core 层处理器只支持同步**：`addCoreHookHandler` 是库内自用入口，派发时不走操作协议
   （给一个空实现的完成通知器）；返回操作句柄会被记警告并请求取消。库内当前无使用者 ——
   将来真要用异步 core 处理器，需要像插件层那样接 `OpCore` 驱动。
3. **`hooksJson()` 的线程约定**：读注册表当前状态，应在宿主 io 线程调用（`list_hooks`
   入口已由 vtable 投递；装配快照与诊断也在同一生命周期阶段取数）。装配快照若哪天改到
   别的线程构建，需要给注册表加锁或改成快照缓存。
4. **清单只是只读视图**：不参与派发判定（派发只看注册表当前状态，且每次派发取一次快照）。
5. **顺序调整 = 重新登记**：没有"运行时改优先级"的接口（
   `unregister_hook_ex` + `register_hook_ex`），与方案 §6.5 一致。
6. **`example_feature` 现在同时示范功能点与有序钩子**：如果后续想拆成两个示例插件
   （一个只讲功能点、一个只讲钩子）更清晰，注意 `plugin.yaml` 的 `interfaces.require`
   别把 `hooks_ex` 写成 require（老宿主会整插件跳过加载）。
7. **阶段 6 待做**：命令行 / FFI 接入（`feature` 子命令 + FFI 导出 + wire 消息 +
   `ffi_symbols.map` + ffi.md + Dart 绑定）；钩子清单可顺带在同一入口暴露。
8. **阶段 7 待做**：第二批功能点、`docs/zh-cn/design/feature-points.md`、
   插件作者文档补"钩子优先级与默认带 / 超时口径 / `provide` 写法"。
