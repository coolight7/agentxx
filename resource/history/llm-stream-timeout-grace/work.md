# 实施记录: 长工具调用被判为读取超时

进度: 0 / 8

## 任务列表

- [ ] 1. 建任务目录（index/plan/work）
- [ ] 2. cxx_utilxx: `RequestConfig.readChunkTimeoutPerRead` + `read-body` 循环取样
- [ ] 3. `provider_common.h`: `StreamProgress` / `streamFinishGraceTimeout` /
      `llmSendTimeout` / `streamFailureSuffix`
- [ ] 4. `openai_provider`: 统计出参 + 宽限钩子 + `finishSeen` 收尾 + 诊断 + 发送期限
- [ ] 5. `anthropic_provider`（含内联解析器）: 同上 + `stop_reason` 记录
- [ ] 6. 测试（http / openai_provider / anthropic_provider）
- [ ] 7. 文档 `docs/zh-cn/design/index.md`
- [ ] 8. 构建 + 相关模块回归

## 步骤记录

（每步完成后补：改了哪些文件、怎么验证的、遇到的问题）

## 验证清单

- [ ] `agent/script/windows_debug_build.bat` 通过
- [ ] `agentxx_test --log-level warn http openai_provider anthropic_provider` 通过
- [ ] 宽限取值表 / 发送期限公式单测通过
- [ ] `finish_reason` + 断连按成功收尾；无 `finish_reason` 静默仍报错并带诊断

## 剩余事项

- P4：按静默阶段拆分阈值（等本轮结论）
