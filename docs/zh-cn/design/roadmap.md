# 实施状态清单

> 相关文档: [index.md](index.md) · [security.md](security.md) · [plugins.md](plugins.md) ·
> [tui.md](tui.md) · [benchmark.md](benchmark.md)
>
> 本文是**单一实施状态清单**：功能融合比较文档（`resource/history/compare-agent/*.md`）
> 中每条设计的当前状态、代码位置和验收位置都记在这里。新增能力时先在此登记，
> 不要在各处零散写 TODO。
>
> 状态取值：**已实施**（有代码 + 有测试）· **部分实施**（限定范围完成）·
> **待实施**（已核定可行，未做）· **不做**（已核定不采用）· **未来计划**（保留方向，不排期）。

## 1. 架构与装配

| 编号 | 内容 | 状态 | 代码位置 / 验收 |
|---|---|---|---|
| ARC-1 | 核心边界可执行检查 | 已实施 | `agent/test/core/test_boundaries.cpp`（测试模块 `boundaries`） |
| ARC-2 | 目录级规则文件 | 已实施 | `agent/{lib,client,plugins,test}/AGENTS.md` |
| ARC-3 | 装配清单和启动断言 | 已实施 | `agent/lib/src/agent/base_agent.cpp`（`InitStep` 清单 + `verifyStartupAssembly`） |
| ARC-5 | 分阶段关闭与后台任务收敛 | 待实施 | `BaseAgent::shutdownAsync` |
| ARC-6 | 生效装配快照（启动日志 + `--dump-config`） | 待实施 | `BaseAgent` 装配、`agent/client/main.cpp` |
| ARC-7 | 新能力不进入核心骨架 | 已实施（纪律） | `agent/lib/AGENTS.md` |
| ARC-8 | 消费者使用窄接口（试点） | 待实施 | 中间件 / subagent 工具 |
| ARC-4 / ARC-7b / ARC-9 | 子系统配置视图 / 千行文件拆分 / 领域与策略分离 | 不做 | 提高内聚、无功能收益 |

## 2. 轮次与输入投递

| 编号 | 内容 | 状态 | 代码位置 / 验收 |
|---|---|---|---|
| LOOP-1 | 持久化收件箱（两段状态） | 待实施 | 会话库 `session_input` 表；依赖 STO-1/STO-2（已实施） |
| LOOP-2 | `next-step` / `next-turn` / `inject` | 待实施 | `wire_protocol.h`、`session_server_agent_io.cpp`、`nodes/modelcall.cpp` |
| LOOP-3 | 投递结果显式化 | 待实施 | 同上（`started/queued/steered/rejected` + 结构化原因） |
| LOOP-4 | QueueState 状态机 | 待实施 | `idle/running/paused/draining` |
| LOOP-11 | `collect` 合并投递 | 待实施 | 同上（短暂静默窗口内合并连续输入） |
| LOOP-5 / LOOP-6 / LOOP-7 / LOOP-8 / LOOP-9 / LOOP-10 | 幂等键 / 轮次表 / 请求抢占 / 步骤快照 / 优雅取消 / 失败结构化留痕 | 不做 | 现有中断+插入+重跑、view 消息记录与错误提示已覆盖需求 |

## 3. 上下文、提示词、技能与记忆

| 编号 | 内容 | 状态 | 代码位置 / 验收 |
|---|---|---|---|
| PRM-1 | stablePrefix / dynamicSuffix | 待实施 | `nodes/modelcall.cpp`、`agent/prompt.cpp` |
| PRM-2 | 段落排序号（`order`） | 待实施 | 提示词贡献表 |
| PRM-4 | 记忆文件过大警告 | 待实施 | `middlewares/memory_file.cpp` |
| PRM-5 | 技能优先级与同名裁决 | 待实施 | 技能加载链路 |
| PRM-7 | 请求体结构断言 + 稳定段哈希断言 | 待实施 | 测试模块 `agent` / `modelcall` |
| CMP-3 | 结构化摘要与尾部原文 | 已实施 | `agent/lib/src/agent/prompt.cpp`（固定小节 Goal/Done/In progress/Blocked/Key facts/Next） |
| CTX-1 ~ CTX-9 | 来源化上下文 / 三态来源 / 消息来源 / 只读快照 / 历史替换记录 / 自定义条目 / 附件引用 / 工作上下文 / 会话重建入口 | 不做（CTX-7 待理解） | `flags`/`extra` 不进请求体、`history_contents` 保留旧版本等机制已覆盖需求 |

## 4. 持久化与崩溃恢复

| 编号 | 内容 | 状态 | 代码位置 / 验收 |
|---|---|---|---|
| STO-1 | 会话目录内核级写租约 | 已实施 | `agent/lib/{include/agentxx/agent,src/agent}/writer_lease.*`；模块 `writer_lease` |
| STO-2 | schema 版本和相邻迁移链 | 已实施 | `session_store.cpp`（`kSchemaVersion` / `applyMigrationStep` / 备份）；模块 `session_schema` |
| STO-8 | 用量账本 | 已实施 | `session_store`（`usage` 表）+ `nodes/modelcall.cpp`；模块 `usage_ledger` |
| STO-11 | settings_db 乐观版本 | 已实施 | `util/settings_db.*`；模块 `settings_db` |
| STO-12 | 会话标题与检索（存储层） | 已实施 | `session_store`（`sessionTitle` / `setSessionTitle` / `searchSessions`）；模块 `session_schema` |
| STO-12b | 标题/检索的协议与 TUI 入口（RET-1a） | 待实施 | `wire_protocol.h`、TUI 会话弹窗 |
| STO-4 | `view_message.seq` + `hello.afterSeq` 增量补拉 | 待实施 | `session_store.cpp`、`session_server_agent_io.cpp` |
| STO-5 | 持久化语义分级（`persistNow` / `persistThrottled`） | 待实施 | 会话写入口 |
| STO-9 | 持久化降级可见（首次写失败推 `MessageTip`） | 待实施 | 会话写入口 |
| STO-13 | 会话导出与取证包 | 待实施 | 新导出入口 |
| STO-3 / STO-6 / STO-7 / STO-10 | 事件序列表 / turn-attempt 表 / 启动清账 / 大写入不阻塞 io 线程 | 不做 | 读模型已足够；`viewMessages` 可直接承载记录 |

## 5. 工具系统

| 编号 | 内容 | 状态 | 代码位置 / 验收 |
|---|---|---|---|
| TOOL-1 | 分阶段并行 prepare / dispatch / finalize | 已实施 | `nodes/toolcall.cpp`（prepare/run/finalize 三段）；模块 `toolcall_parallel` |
| TOOL-2 | 并发分类与上限 | 已实施 | `tools/tool.h`（`supportsParallel`）+ `AgentConfig::toolParallelMaxConcurrency`；插件 flags `AGENTXX_PLUGIN_TOOL_FLAG_PARALLEL_SAFE` |
| TOOL-3 | 并行取消与收尾 | 已实施 | `nodes/toolcall.cpp`（按声明顺序写回、已完成结果保留、未完成补 `[User canceled]`）；模块 `toolcall_parallel` |
| TOOL-16 | 按规范化文件路径排队执行 | 待实施 | 文件写工具 |
| TOOL-17 | 执行环境加固（`NO_COLOR` / `TERM` / `PAGER` / locale） | 已实施 | `plugins/agentxx_execute_command/execute_command_impl.h` |
| TOOL-12 | 工具可用性与授权分层（并入 ARC-6 快照） | 待实施 | 装配快照 |
| TOOL-4 / TOOL-5 / TOOL-6 / TOOL-7 / TOOL-8 / TOOL-9 / TOOL-10 / TOOL-11 / TOOL-13 / TOOL-15 | 分发层超时 / 结果守卫 / 聚合预算 / 结构化定位符 / 结构化结果 / 单调 guard / 错误状态 / 公告身份 / 审批身份绑定 / 后置上下文 | 不做 | 各工具自带超时、share_store 定位、异常分类已覆盖；插件为受信代码（见 [security.md](security.md)） |
| TOOL-14 | 延迟工具暴露级别 | 后续计划 | 现有延迟加载不完善，暂不启用 |

## 6. LLM provider 与成本

| 编号 | 内容 | 状态 | 代码位置 / 验收 |
|---|---|---|---|
| LLM-2 | 错误分类与重试策略 | 待实施 | `protocol/*provider.cpp`、`nodes/modelcall.cpp` |
| LLM-3 | 溢出一次性压缩重试 | 待实施 | 同上（与 LLM-2 共用入口） |
| LLM-5 | 假 provider | 待实施（测试已有模拟器） | `agent/test/core/test_agent.cpp` 的本地 LLM 模拟器 |
| LLM-7 | 消费端退出取消 | 待实施 | provider 流对象 |
| LLM-8 | 缓存断点与缓存用量 | 待实施 | Anthropic 请求装配 + 用量账本 |
| LLM-13 | HTTP 录制回放 | 待实施 | 测试夹具 |
| LLM-4 / LLM-6 | 静默看门狗 / 流式组装唯一实现 | 已实施 | provider 内组装 + 看门狗 |
| LLM-1 / LLM-9 / LLM-10 / LLM-11 / LLM-12 | 模型能力元数据 / 轮次局部回退 / 凭据分层 / 连接状态 / 结构化输出入口 | 不做（9、12 后续计划） | 不补价格与能力元数据；provider 全走 HTTP |

## 7. 上下文压缩与预算

| 编号 | 内容 | 状态 | 代码位置 / 验收 |
|---|---|---|---|
| CMP-1 / CMP-2 / CMP-4 / CMP-5 / CMP-6 / CMP-7 / CMP-8 / CMP-9 | 预算单一口径 / 剪枝-度量-摘要 / 原文可回取 / 压缩事务 / 恢复元数据 / 质量门 / 缓存前缀 / 记忆提醒 | 不做（CMP-8 保持现状） | 现状：确定性清理 → 子代理摘要 → `hardTruncate`；原文在 `viewMessages` 可查 |

## 8. 权限与安全

| 编号 | 内容 | 状态 | 代码位置 / 验收 |
|---|---|---|---|
| SEC-2 | 权限决定与理由（`decision/reason/rule/target`） | 待实施 | `middlewares/permission.cpp` |
| SEC-5 | 执行前目标复验（批准目标 = 实际执行目标） | 待实施 | `nodes/toolcall.cpp` + 权限中间件 |
| SEC-9 | 安全责任与边界文档 | 已实施 | [security.md](security.md) |
| TST-10 | 安全负面测试（门禁正确性） | 待实施 | 测试模块 `permission` / `boundaries` |
| SEC-1 / SEC-3 / SEC-4 / SEC-7 / SEC-8 / SEC-10 / SEC-11 / SEC-13 / SEC-14 / SEC-15 | 危险工具声明全覆盖 / 审批持久化 / 出网策略 / 审计 / 项目信任 / 规则定义分离 / 样例校验 / 先读后编辑 / 内容扫描 / 通用动作权限 | 不做 | 见 [security.md](security.md) §1 三条基本判断 |
| SEC-6 / SEC-12 | 符号链接真实路径 / 可选沙箱后端 | 未来计划 | 真隔离路径；实施前先定平台能力与文案 |

## 9. 客户端与 UI

| 编号 | 内容 | 状态 | 代码位置 / 验收 |
|---|---|---|---|
| UI-1 | 客户端模型层（分页窗口 / 队列镜像 / 重连 seq） | 待实施 | 无 FTXUI 依赖的模型类 |
| UI-2 | UI 快照夹具（固定尺寸文本 + 命中区基线） | 待实施 | 测试模块 `tui_ui_items` |
| UI-3 | 未知组件宽容降级（补两个测试） | 待实施 | `ui_components.cpp` |
| UI-4 | 渲染层边界测试 | 待实施 | 测试模块 `ui_items` / `boundaries` |
| UI-5 | 输入栏硬件光标（终端不支持时降级） | 待实施 | `components/input_bar.*` |
| UI-9 | 能力与体验级别声明 | 待实施 | `tuiUiCapabilities()` |
| UI-6 / UI-8 / UI-10 | Markdown offload / 进度卡 slot / 文案门禁 | 不做 | 现有渲染与测量共用实现足够 |
| UI-7 | 统一浮层管理器 | 未来计划 | 需要时再引入最小浮层模型 |

## 10. 协议与服务形态

| 编号 | 内容 | 状态 | 代码位置 / 验收 |
|---|---|---|---|
| PRO-1 | 消息往返测试 | 已实施 | 模块 `wire_roundtrip`（全消息类型往返 + 幂等 + 未知字段/类型兼容）；`remote_agent` 亦有协议往返段 |
| PRO-3 | 协议版本与能力握手 | 待实施 | `WireHello` / `WireHelloAck` |
| PRO-4 | 生成 `wire-schema.json` 与字段文档 | 待实施 | 脚本 + CI 新鲜度检查 |
| PRO-5 | 连接阶段与错误分类 | 待实施 | 端点状态机 |
| PRO-7 | 会话 ID 校验统一化 | 已实施 | `SessionServerAgentIO::acceptSessionScope`（入口统一校验，不匹配回 `WireError`）；`remote_agent` 的 `session scope validation` 用例 |
| PRO-8 | stdio JSONL 一次性运行 | 待实施 | `agent/client`（复用 Wire 结构） |
| PRO-11 | 统一错误对象与 wire 错误码 | 部分实施 | `WireErrorCode`（`agent_io_transport.h`，未知码按 `Internal`）+ 会话校验/错误回执已用；工具与节点边界的统一错误对象待补 |
| PRO-2 / PRO-6 / PRO-9 / PRO-10 / PRO-12 | 幂等键 / durable after 游标 / daemon / 开放 SDK / 结果状态元数据 | 不做（6 并入 STO-4） | 已支持常驻 server、同进程合并启动、FFI |

## 11. 插件与扩展机制

| 编号 | 内容 | 状态 | 代码位置 / 验收 |
|---|---|---|---|
| PLG-1 | 注册可逆与清理审计 | 待实施 | 插件生命周期测试 |
| PLG-2 | 声明式贡献集合与重算 | 待实施 | 贡献表 |
| PLG-4 | 独占能力 slot | 待实施 | 压缩器 / 记忆提供者 |
| PLG-6 | 装配树与域视图查询 | 待实施（并入 ARC-6） | `--dump-config` |
| PLG-7 | 教学式错误与信任声明 | 待实施 | 插件 SDK 错误文案 + 文档 |
| PLG-8 | 文档分页与接口表数字校验 | 待实施 | `plugins.md` |
| PLG-10 | 插件加载耗时与注册计数诊断 | 待实施 | 插件管理器日志 + ARC-6 快照 |
| PLG-3 / PLG-5 / PLG-9 / PLG-11 | 细粒度变更事件 / manifest schema / 作用域过滤 / 加载许可 | 不做 | 无派生缓存；`args` 原样透传；子代理白名单已实现 schema 与执行同时不可见 |

## 12. 配置

| 编号 | 内容 | 状态 | 代码位置 / 验收 |
|---|---|---|---|
| CFG-1 | 配置回填与统一校验（键路径 + 来源 + 致命/警告） | 待实施 | `agent/client/src/config_loader.cpp` |
| CFG-8 | 设置与配置边界（YAML / settings_db / 安全状态） | 待实施 | `docs/zh-cn/design/index.md` + 语言接线 |
| CFG-9 | 生成式配置键目录 | 待实施 | 脚本 + CI 新鲜度检查 |
| CFG-2 / CFG-3 / CFG-4 / CFG-5 / CFG-6 / CFG-7 | 配置版本迁移 / 来源清单 / 模型元数据 / 世代快照 / 凭据分层 / 项目信任 | 不做（3 并入 ARC-6） | 结构变化用告警 + 内存态适配；不写版本、不改源文件 |

## 13. 检索、诊断与可观测性

| 编号 | 内容 | 状态 | 代码位置 / 验收 |
|---|---|---|---|
| RET-1a | 会话搜索/改名的协议与 TUI 入口 | 待实施 | 与 STO-12b 同批 |
| OBS-3 | 关键指标（首 token / 轮次耗时 / 压缩次数等） | 待实施 | 复用 benchmark 基础设施 |
| OBS-4 | 诊断包导出 | 待实施 | 与 STO-13 合并 |
| OBS-5 | 模块级日志开关 | 待实施 | 日志前缀过滤 |
| RET-1b / RET-2 / RET-3 / RET-4 / RET-5 / OBS-1 / OBS-2 | FTS5 索引 / 标题元数据独立 / 附件校验元数据 / 结构化定位符 / opId / `get_diagnostics` / telemetry 边界 | 不做（RET-2 已并入 STO-12、OBS-2 保留为设计约束） | 逐会话子串扫描够用；share_store 已有行定位提示 |

## 14. 测试与门禁

| 编号 | 内容 | 状态 | 代码位置 / 验收 |
|---|---|---|---|
| TST-1 | 假 provider（固定流 / 错误 / 延迟 / tool call） | 待实施 | 测试夹具 |
| TST-2 | Wire 往返与 schema 一致性 | 部分实施 | 模块 `wire_roundtrip`（往返 + 幂等 + 兼容）；schema 生成一致性待补 |
| TST-3 | 持久化迁移/恢复测试 | 部分实施 | 模块 `session_schema`（老库迁移、幂等、高版本拒绝）；崩溃未闭合轮次待补 |
| TST-4 | 并发与竞态清单 | 部分实施 | 模块 `toolcall_parallel`（并行完成顺序 vs 提交顺序、屏障、并发上限、取消收尾）；其余待补 |
| TST-6 | 一致性测试骨架 | 待实施 | 存储替身 |
| TST-7 | 边界/导出/清理门禁 | 待实施 | 模块 `boundaries` 扩展 |
| TST-8 | CI 一键门禁 | 待实施 | 构建脚本 + 测试模块集合 |
| TST-9 | 测试隔离、耗时和脱敏 | 部分实施 | 独立临时目录与模块耗时已实现；脱敏与凭据环境清理待补 |
| TST-10 | 安全负面测试 | 待实施 | 见 §8 |
| TST-5 | UI 快照夹具 | 待实施（并入 UI-2） | — |
| TST-11 / TST-12 / TST-13 / TST-14 / TST-15 | 产物级 e2e / 覆盖率门禁 / 状态清单 / 守卫有效性用例 / 测试目录分组 | 部分实施 | 本文即 TST-13；TST-14 为 `agent/lib/AGENTS.md` 评审约定；benchmark 已覆盖真实两进程与 PTY |

## 15. 未来计划

| 编号 | 内容 | 触发条件 |
|---|---|---|
| SEC-12 | 可选沙箱执行后端（bubblewrap / Landlock / Seatbelt） | 需要在不可信内容或多用户环境部署时 |
| SEC-6 | 符号链接真实路径判定（先覆盖写操作） | 与 SEC-12 同批评估 |
| SEC-8 / CFG-7 | 项目信任（首次在项目路径启动时询问一次） | 需要加载不可信项目配置时 |
| UI-7 | 统一浮层管理器 | 浮层数量继续增长、锚点与层级规则开始重复时 |
| LLM-9 | 轮次局部模型回退 | 先定触发条件、候选模型来源与能力兼容校验 |
| LLM-12 | 结构化输出统一入口 | 出现第一个需要强制 JSON 输出的场景（标题生成 / 计划校验 / 抽取） |
| TOOL-14 | 延迟工具暴露级别 | 延迟加载实现完善后 |
| JOB-1 ~ JOB-12 | 作业表、进度、后台命令、子代理记录、配额车道、fork 上下文、可继续子代理、定时任务、跨会话观察者 | 子代理改造排期时整体设计 |
| OBS-2 | telemetry 内容边界（设计约束） | 出现 telemetry 出口时 |

## 16. 修订记录

- 2026-10-05：建立本清单（计划 TST-13），按 §1~§15 汇总六篇比较文档的裁定结果与代码位置。
- 2026-10-05：TOOL-1 / TOOL-2 / TOOL-3 / TOOL-17 标记为已实施（工具三段式执行 + 受限并行 + 命令执行环境加固）。
