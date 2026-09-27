# 不同 Agent 项目对比和借鉴
- 难度: SSS
- 类型: 重构优化
- 基于commit: 24416ee099b6761c57dbd9ab2dd91d1ae08249c1
- 时间: 2026-09-27 17:42
- 需求:
```md
- 对比当前项目 agentxx 和 codex D:\0Acoolight\Program\js\codex 的架构设计，仔细思考分析两者的一些设计上的对比优缺点，以及有哪些设计适合迁移到 agentxx 实现的；应当分模块仔细通读对比两个项目源码、结合测试和文档，了解整体的架构设计，分模块逐步更新写入到文档
resource\history\compare-agent\compare-codex.md，并在最后整理一下整个 compare-codex.md 文档

- 对比当前项目 agentxx 和 opencode D:\0Acoolight\Program\js\opencode 的架构设计，仔细思考分析两者的一些设计上的对比优缺点，以及有哪些设计适合迁移到 agentxx 实现的；应当分模块仔细通读对比两个项目源码、结合测试和文档，了解整体的架构设计，分模块逐步更新写入到文档
resource\history\compare-agent\compare-opencode.md，并在最后整理一下整个 compare-opencode.md 文档

- 对比当前项目 agentxx 和 pi D:\0Acoolight\Program\js\pi 的架构设计，仔细思考分析两者的一些设计上的对比优缺点，以及有哪些设计适合迁移到 agentxx 实现的；应当分模块仔细通读对比两个项目源码、结合测试和文档，了解整体的架构设计，分模块逐步更新写入到文档
resource\history\compare-agent\compare-pi.md，并在最后整理一下整个 compare-pi.md 文档

- 对比当前项目 agentxx 和 dsh D:\0Acoolight\Program\js\deepseek-harness 的架构设计，仔细思考分析两者的一些设计上的对比优缺点，以及有哪些设计适合迁移到 agentxx 实现的；应当分模块仔细通读对比两个项目源码、结合测试和文档，了解整体的架构设计，分模块逐步更新写入到文档
resource\history\compare-agent\compare-dsh.md，并在最后整理一下整个 compare-dsh.md 文档

- 对比当前项目 agentxx 和 openclaw D:\0Acoolight\Program\js\deepseek-harness 的架构设计，仔细思考分析两者的一些设计上的对比优缺点，以及有哪些设计适合迁移到 agentxx 实现的；应当分模块仔细通读对比两个项目源码、结合测试和文档，了解整体的架构设计，分模块逐步更新写入到文档
resource\history\compare-agent\compare-openclaw.md，并在最后整理一下整个 compare-openclaw.md 文档

- 对比当前项目 agentxx 和 harness D:\0Acoolight\Program\js\deepseek-harness 的架构设计，仔细思考分析两者的一些设计上的对比优缺点，以及有哪些设计适合迁移到 agentxx 实现的；应当分模块仔细通读对比两个项目源码、结合测试和文档，了解整体的架构设计，分模块逐步更新写入到文档
resource\history\compare-agent\compare-harness.md，并在最后整理一下整个 compare-harness.md 文档

- 结合 compare-*.md，最终整理出每个模块中较好的、适合迁移到 agentxx 实现的设计
```