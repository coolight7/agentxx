# plugin-async-unify —— 插件侧接口统一异步（彻底重构）

> 计划见 [plan.md](plan.md)，任务卡片见 [index.md](index.md)。
> 本文件记"实际做了什么 + 怎么验证 + 与计划的偏差"，每完成一步就写。

## 进度总表

| 阶段 | 内容 | 状态 |
|---|---|---|
| A | 框架层：提交即可等的 ticket + `tableOpSubmit` + 顺序契约写进头文件 | 待做 |
| B | kit 改名与补形态：先同步 → `*Blocking`，主名让给 awaiter / submit / callback | 待做 |
| C | 迁移 25 个插件 + 删 4 个私有 `netCallWait` + 工具形态（`polled_tool` + `offload`） | 待做 |
| D | 生命周期异步完成（`finishLater` + 宿主超时配置） | 待做 |
| E | 删除全部阻塞形态 + `boundaries` 禁令 + 文档 | 待做 |
| F | （可选）client 侧三个回答型回调异步 + 删 150 ms 同步桥 | 待确认 |
| G | 收尾：文档、快照、全量回归、性能对比 | 待做 |

## 分步记录

（每完成一批补一节：改了哪些文件 / 怎么验证 / 遇到的问题）

## 与计划的偏差

（实施中随时补）

## 剩余事项

（收尾时补）
