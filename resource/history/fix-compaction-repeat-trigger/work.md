# 实施记录: 压缩完成后不再重复触发压缩

## 当前进度

- [x] 现场数据取证 (会话库 view_message / usage 两张表)
- [x] 方案与边界 (`plan.md`)
- [x] `summarization.cpp` 修复 (自动 + 手动压缩)
- [x] `middleware.h` 键说明
- [x] 新增 3 个用例 + 修正 T19 的旧行为断言
- [x] 构建 (windows-debug: lib 静态库 + test) 与测试
- [x] `docs/zh-cn/design/index.md` 的压缩段补规则

## 第 1 步 取证

读的是现场会话 `sess-cf62b88a1b24-19652-a55e364c-0000` 的 `session.db` 副本 (含 WAL),
只看 `view_message` 的 tip 行与 `usage` 的逐次调用记录。同一个 `oldTokens` 连出三条
"Summarized" 提示、且第二三条附近各对应一次额外的压缩子代理调用 (p≈48K), 确认是
"压缩完成后又被触发" 而不是"界面重复显示"。结论写进了 `index.md`。

## 第 2 步 代码改动

`agent/lib/src/middlewares/summarization.cpp` → `SummarizationMiddlewareHandle::onModelcallRunFunc`

- 顺序调整: 先拷上下文 + 确定性压缩 (去重/折叠/噪音清理), 再判冷却; 冷却是 `co_return`,
  不建提示消息、不重写会话、不动 `summarizationLastMsgCount` (原来的写法是"用原消息写回 +
  追加一条提示 + 把计数复位成 0")。
- 提示消息的创建/复用挪到冷却判定之后 —— 冷却命中时不再留下一条假的 "Summarized" 提示。
- `session->replaceMessages` / `persistNow("compaction")` 之后把
  `graphDataKey_LLMTokenUsage` 改写成 `newTokens`。

`compactSessionContext` (手动压缩) 同样在 `persistNow("manual-compaction")` 之后改写该键。

`agent/lib/include/agentxx/middlewares/middleware.h`: 给 `graphDataKey_LLMTokenUsage`
补了写入/改写/读取三方的说明。

## 第 3 步 测试

`agent/test/core/test_summarization.cpp`:

- `runModelcall` 增加 `keepExistingTokenUsage` 参数 (不改写图上已有用量, 模拟"压缩完成后
  紧接着的一次重试")。
- T21 压缩后用量被改写 + 同一轮重试不再触发 (不派生 subagent、不新增提示)。
- T22 冷却命中: 上下文原样、不派生 subagent、不新增提示、`lastMsgCount` 仍为 300、
  用量仍是 900。
- T23 手动压缩同样改写用量, 之后的第一轮 modelcall 不再自动压缩。
- T19 (同一会话反复压缩 5 轮) 原来靠"冷却分支也会写一条提示"凑出 5 条记录; 现在改为每轮
  都从同一份 9 条上下文出发 (上下文长回阈值以上才会再压), 仍断言 5 条且没有停留在
  "Summarizing..." 的残留。

## 验证

```pwsh
# 构造: 只编内层库 + 安装头 (避免重链接正在运行的 agentxx.exe)
cmake --build <build>/agentxx_lib_repo-prefix/src/agentxx_lib_repo-build --config Debug --target agentxx_static --parallel 10
cmake --install <build>/agentxx_lib_repo-prefix/src/agentxx_lib_repo-build --config Debug
cmake --build <build>/agentxx_test_repo-prefix/src/agentxx_test_repo-build --config Debug --target agentxx_test --parallel 10

# 运行
agentxx_test --log-level warn summarization feature_points subagent_tool graph_data session_persistence prompt_cache_prefix memgrowth
```

结果: `summarization` 482 条断言全通过; 连同相关 7 个模块共 1561 条断言全通过, 0 失败。

## 与计划不一致的地方

- 计划里写的"冷却命中时跳过"实现为直接 `co_return`, 连带把提示消息的创建挪到冷却判定之后
  (否则提示已经追加, 跳过后会残留一条假的 "Summarizing/Summarized" 提示)。
- 计划外改动: T19 必须改 (它断言的正是被修掉的旧行为)。

## 留给后续的事项

- 冷却分支没有按老注释说的"直接降级硬截断": 压缩后上下文仍 >= 75% 又确实没有增长时只是
  跳过本次。真要兜底可以在这里按 `countTokenUsage` 判断 >= 95% 再硬截断, 但估算偏高时
  会白丢历史, 需要先确认。
- 压缩后写回的是**估算值** (与提示消息里显示的 newTokens 同一份), 下一次成功调用立即用上游
  `usage.total_tokens` 覆盖; 若上游长期不可用, 触发判断就依赖这份估算。
