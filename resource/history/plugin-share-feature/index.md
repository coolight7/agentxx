# 插件共享功能：功能点统一（两类型 + 值缓存 + 自省 + 对外开放调用 + 钩子清单）

- 难度: S
- 类型: 架构设计
- 基于commit: 22301e5f456cf71cc2722eb9214a411364e813a7
- 时间: 2026-10-10 19:20（二稿修订：2026-10-10；三稿修订：2026-10-10）
- 需求:
```md
请参考 musicxx 的 插件框架 重构 D:\0Acoolight\Program\flutter\mymusic\resource\history\extern-plugin-share-feature\plan.md，
将 功能埋点 的处理统一起来，请仔细思考分析，agentxx 项目目前的架构设计，
以及 musicxx 的插件框架重构中有哪些设计可以参考同步优化重构，
让 agentxx 也统一设计思路和代码实现，整理出完善的方案写入到文件
D:\0Acoolight\Program\cpp\agentxx\resource\history\plugin-share-feature\plan.md
```
- 二稿反馈（已并入方案）:
```md
1. agentxx 只有一套插件框架（builtin 是同一框架内联编译），只区分 plugin / core；
   "值仓"改称"值缓存"；token 计数 64 条 / 256 KiB 可行；工具输出摘要不缓存、不设上限（它有自己的库管理）。
2. 埋点就是 core 主动调用的扩展点，天生希望外部注册处理器，去掉 allowPluginImpl。
3. 压缩暂不对外开放调用，加入路线图。
4. 功能点要暴露给 FFI 与命令行，让多方统一。
5. 钩子要做处理器清单和优先级，便于 core 或插件读取判断。
6. 暂不展示功能清单，加入路线图。
7. 插件点先固定不缓存，计划加入路线图。
```
- 三稿反馈（已并入方案）:
```md
1. priority 越界则修正到上下限并日志警告即可；
2. 不限制 处理器、功能实现 的注册数量，非必要不增加太多检查和限制；
3. ffi 本质跟直接修改或导入 agentxx 开发差不多，默认比 core 高优先级、比插件低；
4. timeout 有必要实现，但默认不限制，由调用者自行决定；
5. 命令行参数做成子命令 `agentxx_cli feature --list/--call/--dump-config`；
6. 增加一个 开发者模式，启用时启动才收集各种记录数据。
```
- 方案文档: [plan.md](plan.md)（功能点子系统：`provide` / `decide` 两类型、实现分层 `plugin → core`
  与默认优先级带（插件 0 / FFI 宿主 1000）、显式置空、值缓存三策略与身份、对外开放调用与三道保护、
  两处超时（默认不限）、插件自定义功能点、**钩子处理器清单与优先级**、
  跨边界接口（接口表 `agentxx.agent.feature` v1 + kit + FFI + 子命令 `feature`）、
  **开发者模式**（`dev_mode`，开启才收集记录数据）、自省与诊断、
  第一批功能点（token 计数 / 上下文压缩 / 工具输出摘要）、7 阶段迁移计划与测试点、
  与 musicxx 本轮及上一轮设计的逐条对照（26 条）、路线图）
