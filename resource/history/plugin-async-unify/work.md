# plugin-async-unify —— 插件侧接口统一异步（彻底重构）

> 计划见 [plan.md](plan.md)，任务卡片见 [index.md](index.md)。
> 本文件记"实际做了什么 + 怎么验证 + 与计划的偏差"，每完成一步就写。

## 进度总表

| 阶段 | 内容 | 状态 |
|---|---|---|
| A | 框架层：提交即可等的 ticket + `tableOpSubmit` + 顺序契约写进头文件 | 待做 |
| B | kit 改名与补形态：先同步 → `*Blocking`，主名让给 awaiter / submit / callback | 待做 |
| C | 迁移 25 个插件（含 chatbot 10 个工具改 `polled_tool`）+ 删 4 个私有 `netCallWait` | 待做 |
| D | 生命周期异步完成（`finishLater`；不设时限、不加配置键） | 待做 |
| E | 删除全部阻塞形态 + `boundaries` 禁令 + 文档 | 待做 |
| G | 收尾：文档、全量回归、卸载路径 ASan 复核、性能对比 | 待做 |

不做：client 侧回答型回调与 150 ms 同步桥（留给 `plugin-api-async` 的阶段 D/E）。

## 分步记录

（每完成一批补一节：改了哪些文件 / 怎么验证 / 遇到的问题）

## 与计划的偏差

（实施中随时补）

## 剩余事项

（收尾时补）
