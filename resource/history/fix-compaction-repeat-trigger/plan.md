# 方案: 压缩完成后不再重复触发压缩

## 1. 背景与现象

用户报障: 会话 `sess-cf62b88a1b24-19652-a55e364c-0000` (模型 `opencode-ds-flash`,
`model_context_max_token: 500000`, 触发阈值 75% = 375000) 连着触发好几次压缩; 中间压缩
已经完成、也发出了新请求, 只是被网络中断重试, 但压缩完成之后不应再压一次。

会话库 (`view_message` 表的 Tip 行, `usage` 表的逐次调用记录) 给出的现场:

| 时间 | 事件 |
|---|---|
| 16:30:14 | 模型调用失败 (timeout) |
| 16:31:21 | 模型调用失败 (SSE read-header timeout) |
| 16:32:27 | 成功调用 p=377104 c=5217 → 这次是**压缩 subagent** 的调用 |
| 16:32:27 | Tip `Summarized LLM Context 375936->38101/500000 · 1.7s` |
| 16:32:40 | 模型调用失败 (DNS) |
| 16:32:43 | Tip `Summarized LLM Context 375936->38109/500000 · 23ms` (没有模型调用) |
| 16:32:59 | 模型调用失败 (timeout) |
| 16:33:37 | 成功调用 p=49021 c=4479 → **又一次压缩 subagent 调用** |
| 16:33:37 | Tip `Summarized LLM Context 375936->37319/500000 · 40ms` |
| 16:33:41 | 真正的父会话请求才发出 (p=47850 c=58) |

第二条三联记录 (22:58~23:03) 形态完全相同: `375272->36702` (2.9s, 真实压缩)、
`375272->36710` (29ms)、`375272->36269` (40ms, 对应一次 p=48119 c=3185 的
额外压缩调用)。三行提示的 `oldTokens` 都是同一个压缩前的值, 说明压缩后判断依据没变。

## 2. 现状调查 (文件 / 函数级)

### 2.1 触发判定

`agent/lib/src/middlewares/summarization.cpp` → `SummarizationMiddlewareHandle::onModelcallRunFunc`:

- `messages` 取自会话 (唯一权威);
- `apiTokenUsage` = `MiddlewareContext::graphDataKey_LLMTokenUsage` 的值;
- 该键唯一的写者是 `agent/lib/src/nodes/modelcall.cpp` 的 `callLLM` 成功分支
  (`completion.usage.total_tokens`), 失败分支不写;
- `tokenUsage = (apiTokenUsage > 0) ? apiTokenUsage : countTokenUsage;`
- `if (tokenUsage >= modelContextMaxToken * 0.75)` → 进入压缩块。

### 2.2 谁会重复执行这个函数

`agent/lib/src/nodes/modelcall.cpp` → `ModelCallWrapNode::baseRun` 的重试循环:

```cpp
do {
    for (auto& handle : handles) { co_await handle->onModelcallRunFunc(in); }
    ...
    try { result = co_await callLLM(in); co_return; }
    catch (...) { /* 失败分类 + 退避, 继续下一轮 */ }
} while (true);
```

注释里已写明"重试时 messages 可能已经发生更改, 因此允许在重试时再次执行"。因此:

- 每一次重试都会重新跑一遍压缩判定;
- 自动压缩经 `requestInterrupt` (见 `middlewares/middleware.cpp::requestInterrupt` 与
  `tools/subagent.cpp::execute_async`) 抛 `NodeInterrupt` 中断父轮次, resume 后整个节点
  从头重新执行, 同样会再跑一遍判定 (`interruptResult` 是取一次就删的缓存, 第二次命中后
  不会重复派生 subagent)。

### 2.3 缺陷

1. **压缩写完会话后没有更新 `graphDataKey_LLMTokenUsage`**。压缩把上下文从 375K 降到
   37K, 但该键仍是 375272; 失败的重试不会更新它 (只有成功调用才写) → 之后的每次重试都
   读到压缩前的旧值 → 条件恒成立 → 反复触发压缩、反复重写上下文、反复 `persistNow`。
2. **冷却分支自毁**: `coolDownActive` 命中时 (`messages.size() <= lastMsgCount + 2`) 只把
   `compressedMessages = messages` 原样写回, 但仍然:
   - 追加了一条 `Summarized LLM Context ...` 提示消息 (数字是旧的 `oldTokens` + 当前条数);
   - `session->replaceMessages` + `persistNow` (无意义的磁盘写);
   - 末尾把 `summarizationLastMsgCount` 复位成 0 → 再下一个触发不再处于冷却 → 直接派生出
     第二次真实的压缩 subagent (现场那两次 p≈48K 的额外调用就是它)。
3. 手动压缩 `compactSessionContext` 同样不更新该键 → 手动压缩后第一轮请求又会自动压缩一次。

## 3. 方案

### 3.1 压缩结果写回时同步改写上下文大小

`onModelcallRunFunc` 在 `session->replaceMessages(...)` / `persistNow` 之后, 用
`newTokens` (刚算出的压缩后 token 数) 覆盖 `graphDataKey_LLMTokenUsage`; 手动压缩
`compactSessionContext` 同样处理。语义: 该键表示"下一次请求要发的上下文大小", 下一次
成功调用由 `modelcall` 用上游真实用量覆盖。

### 3.2 冷却命中时整段跳过

把冷却判据与前后的副作用重排:

1. 复制 + 确定性压缩 (去重/折叠/噪音清理) 后先判定冷却;
2. 冷却命中 → 记一条 `XX_LOGD` 并 `co_return`: 不建提示消息、不重写会话、不复位计数;
3. 未命中 → 才创建 (或复用) 提示消息, 走原有 LLM 压缩 / 失败计数 / 硬截断回退。

判据本身保持不变 (消息条数增长不足是"摘要降不下来的"兜底信号), 但由于 3.1 已经让重复
触发不再发生, 它回到兜底角色的定位。

## 4. 边界

- 压缩 subagent 自身的模型用量由子 AgentContext 记账, 不会写父会话的该键 (同上下文模式
  只是共享 sessionId 与模型), 因此不影响本次判定。
- 中断 resume 后的第二次执行仍会进入压缩块 (该键此时还没被改写) 并命中 `interruptResult`
  缓存, 这是设计路径 (摘要正是在这次执行里写回), 保持不动。
- 上下文在压缩后同一轮内又真的涨回 75% 以上: 冷却可能挡住这一次, 但消息数一涨 (工具结果
  本身是消息) 就退出冷却; 不做额外限制。

## 5. 分步实施

1. `middleware.h`: 补 `graphDataKey_LLMTokenUsage` 的注释 (谁写、压缩后怎么变)。
2. `summarization.cpp`: 重排 `onModelcallRunFunc` 的压缩块; 写回后更新该键;
   `compactSessionContext` 同样更新。
3. `test_summarization.cpp`: 新增 3 个用例 (压缩后键被改写且不再触发 / 冷却不改写不复位 /
   手动压缩后不再触发)。
4. 构建 + 跑 `summarization` 测试模块。

## 6. 验证

- `agentxx_test --log-level warn summarization` 全部通过;
- 现有 `onModelcallRunFunc` 相关用例 (T16/T18/T20/T23/T24/T25) 不回归;
- 手工核对: 用例 1 第二次运行后会话仍是 3 条消息、`viewWindow` 不再新增 Tip。

## 7. 风险

- 触发依据改成压缩后的估算值, 若估算偏低会推迟下一次压缩 —— 下一次成功调用立即用上游
  真实用量覆盖, 窗口很短。
- 冷却跳过不再重写会话: 若某插件依赖"每次 modelcall 都 rewrite" 的行为会察觉差异, 实测
  没有这种依赖 (会话语义不变)。
