# 修复: 压缩完成后同一轮内重复触发压缩

## 目标

上下文自动压缩 (75% 上限触发) 在压缩已经完成、上下文已回落到很小之后, 不应再被
同一次模型调用的重试触发; 压缩触发依据必须描述"当前这次请求要发的上下文"。

## 范围

- `agent/lib/src/middlewares/summarization.cpp` (自动压缩 `onModelcallRunFunc` 与手动压缩
  `compactSessionContext`)
- `agent/lib/include/agentxx/middlewares/middleware.h` (token 用量键的说明)
- `agent/test/core/test_summarization.cpp` (新增用例)

## 状态

已完成。

## 关键决策

- 压缩写完会话后, 把 `graphDataKey_LLMTokenUsage` 改写成压缩后的 token 数: 该键的语义
  变成"当前上下文大小", 不再只是"上一次成功调用的用量"。下一次成功调用照旧覆盖成真实
  上游用量。
- 冷却 (`coolDownActive`) 命中时整段跳过: 不追加/更新提示消息、不重写会话、不动
  `summarizationLastMsgCount`。原来"跳过但复位计数"会让再下一个触发升级成一次真实的
  第二次 LLM 压缩。
- 冷却判据 (上次压缩后消息数增长不足) 保持不变: 修好触发依据后它只是兜底, 不再承担
  "挡住重复压缩"的主要职责。

## 结论

- 现场会话 `sess-cf62b88a1b24-19652-a55e364c-0000` 里连续三条
  `Summarized LLM Context 375272->...` 提示就是本例: 第一条是真实压缩, 后两条由重试重新
  触发 (其中一条还真实派生了第二次压缩 subagent, 浪费一次 ~48K prompt 的调用)。
- 根因有两条: ① 触发依据 `graphDataKey_LLMTokenUsage` 只在**成功**调用时写入, 压缩换掉整份
  载荷后没有更新, 于是同一轮的每次重试都按压缩前的旧值判定"仍超 75%"; ② 冷却分支把
  `summarizationLastMsgCount` 复位成 0, 让再下一次触发升级成又一次真实压缩。
- 修复后: 压缩写完会话即改写该键 (自动 + 手动), 冷却命中时不改写任何东西、只跳过。
- 验证: `summarization` 482 条断言通过; 连同 `feature_points` `subagent_tool` `graph_data`
  `session_persistence` `prompt_cache_prefix` `memgrowth` 共 1561 条通过。
- 偏差: 测试 T19 原来断言的是"冷却分支也会写提示" (即被修掉的行为), 已改为每轮给足 9 条
  上下文以触发真实压缩。
