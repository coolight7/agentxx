# 插件回调预算告警噪声 + LLM 流"只剩余心跳"永不超时

## 目标

1. 偶现的 `Plugin `agentxx_system_monitor`: callback `broadcast plugin.agentxx_system_monitor.usage`
   blocked the plugin thread for N ms (budget 100 ms)` 警告不再按偶发抖动刷出来。
2. 排查 `ModelCallWrapNode` 的超时判定：一次卡在 llm 节点十几分钟、既没有超时重试也没有
   任何日志（手动取消可正常结束）的问题，补上缺失的超时判定与可观察信息。

## 范围

- `agent/third_party/cxx_pluginxx`（自研库）：回调耗时统计与超预算告警策略。
- `agent/lib`：流式请求的"无有效输出"看门狗（`provider_common.h` + 三个 provider）、
  模型配置（`output_idle_timeout`）、诊断快照。
- 文档：`docs/zh-cn/design/index.md` / `configuration.md` / 配置键生成物。

## 状态

- [x] 现状调查（告警来源、LLM 超时链路）
- [x] 方案设计
- [x] 实施：插件超预算告警限频
- [x] 实施：LLM 流无输出看门狗
- [x] 编译 + 测试 + 文档

## 关键结论

- **插件侧没有 100 ms 级的工作**：`agentxx_system_monitor` 客户端实例的 `usage` 广播回调只做
  `std::string` 赋值、一次小 JSON 组装与一次"提交即返回"的表调用（`clientUpdateInfoSection`），
  实测级耗时在几十微秒。超过 100 ms 只能是**插件线程被系统调度挂起**；它是整棵树里唯一以
  5 秒周期派发的回调，所以只有它会统计到这种抖动。原策略"单次超预算即 WARN"把调度抖动
  写成了插件问题 → 改为同 (插件, 回调名) 限频：单次只记 DEBUG 与计数，重复出现才 WARN。
- **LLM 请求确实存在"永不超时"的缺口**：现有唯一超时是 HTTP 层的**块间超时**
  （`RequestConfig::readChunkTimeout`，默认 60 s），它按"收到任意字节"重置。上游/网关在等待
  生成时持续发保活心跳（SSE 注释 `: ping`、Anthropic 的 `event: ping`、空 delta 事件），
  这些字节让计时器一直重置 —— 于是一次已经没有产出的请求可以无限期挂着：没有超时、没有重试、
  也没有日志。节点层重试循环只在**拿到异常**时才动作，没有整体/语义超时。
- 修复取"语义空闲"上限：只有解析出**有效输出**（正文/思考/工具参数分片）才算进展，
  超过上限就把下一次读的超时压到最小，由 HTTP 层既有机制关闭连接，节点按普通传输失败重试；
  等待期间每 60 s 打一条 WARN 说明还在等（解决"日志里什么都没有"）。

## 偏差

- 没有加"单次尝试总时长"硬顶（会误砍合法的长生成）；语义空闲上限默认 600 s，可经
  `model.list[].output_idle_timeout` 调整，0 关闭。
