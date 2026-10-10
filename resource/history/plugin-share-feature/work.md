# 插件共享功能（功能点统一）— 实施记录

> 方案文档: [plan.md](plan.md)（7 个阶段，每阶段一个提交）
> 状态: **实施中** — 阶段 1 已完成

## 阶段进度总览

| 阶段 | 内容 | 状态 |
|---|---|---|
| 1 | 子系统骨架（`agentxx::feature` + 值缓存 + 开发者模式 + `feature_points` 测试） | ✅ 已完成 |
| 2 | 第一批核心点迁移（`countTokens` / `summarize`，行为不变） | ⬜ 待开始 |
| 3 | C ABI 接口表 + kit（插件登记实现） | ⬜ 待开始 |
| 4 | 对外开放调用与插件自定义点 | ⬜ 待开始 |
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
3. **阶段 2 待办**：把 `SummarizationMiddlewareHandle` 的估算规则改为转发
   `TokenEstimator`（删掉重复实现），`service.token.count` 同步服务直连同一份规则
   （TPS 不受影响）；压缩改为经点取摘要文本，编排（提示消息 / 写回 / 硬截断 / 冷却）
   留在中间件。
4. **阶段 3 待办**：`agentxx.agent.feature` 表 + `plugin_manager_feature.cpp` +
   kit 糖；接口表数量 19 → 20（`kInterfaceTableCount` / `plugins.md` §8 /
   根 `AGENTS.md` 三处一起改）。
