# 插件共享功能（功能点统一）— 实施记录

> 方案文档: [plan.md](plan.md)（7 个阶段，每阶段一个提交）
> 状态: **实施中** — 阶段 1~4 已完成

## 阶段进度总览

| 阶段 | 内容 | 状态 |
|---|---|---|
| 1 | 子系统骨架（`agentxx::feature` + 值缓存 + 开发者模式 + `feature_points` 测试） | ✅ 已完成 |
| 2 | 第一批核心点迁移（`countTokens` / `summarize`，行为不变） | ✅ 已完成 |
| 3 | C ABI 接口表 + kit（插件登记实现） | ✅ 已完成 |
| 4 | 对外开放调用与插件自定义点 | ✅ 已完成 |
| 5 | 钩子处理器清单与优先级 | ⬜ 待开始 |
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
3. **阶段 5（钩子清单与优先级）尚独立未做**：`register_hook_ex` / `list_hooks` /
   单一派发器 + `plugin_hooks` 测试模块；与功能点体系共用"默认优先级带 + 越界裁剪 +
   不设数量上限 + 开发者模式记录"这套口径。
4. **阶段 6（命令行 / FFI 接入）**：`feature` 子命令与 FFI 导出会走本阶段确立的
   `call()` 入口（按 JSON 请求解码），届时 `bad_args` / `not_callable` / 超时
   这些错误码会直接在命令行输出里出现。
