# 任务卡片: 适配 cxx_pluginxx 0.2.0 的插件体系重构

## 目标

把 `agentxx` 主程序与它自带的 17 个内置插件 + 9 个示例插件，整体迁移到新版
`cxx_pluginxx`（0.2.0，代次 5），并按新框架的两个通用机制（**能力 capabilities** /
**广播 broadcasts**）重排插件扩展面；**不保留旧版本兼容**，最低支持代次直接抬到 5。
新框架在本项目里缺的通用能力（能力行带作用域、宿主发起调用、广播结束、功能埋点原语、
网络 markdown 形态）**直接改库补上**，供其它主程序复用。

## 范围

| 面 | 内容 |
|---|---|
| 库 | `cxx_pluginxx`：能力行带 scope、宿主发起调用、广播结束、功能埋点原语（L1~L4、L6、L7）|
| 库 | `cxx_utilxx`：`PluginNetHub` 补 `format:"markdown"` 与 `sslVerify`（L5）|
| 构建 | `PLUGINXX_APP_NAME=agentxx`、API 代次 4→5、`cxx_utilxx` → `cxx_pluginxx` 的构建顺序 |
| 主程序（agent 侧） | `DomainHooks`→`DomainServices`、删钩子/功能点/事件三套自有注册表、功能埋点派发、广播频道与总线桥、`agentxx.net` 接入 |
| 主程序（client 侧） | 删自有 json/log/events 三表、接通用表与广播、跨端插件数据改走广播、同样提供 `agentxx.net` |
| SDK | `plugin_kit.h` 按新框架重写：功能埋点 / 广播 / 能力 / 网络的便捷层 |
| 插件 | 17 个内置插件 + 9 个示例插件全部重编、改写注册面；**4 个网络插件改走 `agentxx.net`** |
| 接口表 | agent 侧 23→21 张（10 张框架通用 + `net` + 10 张业务）；client 侧 9→6 张（+ 11 张通用） |
| 测试 / 文档 | 新增 `plugin_point` / `plugin_broadcast` / `plugin_net` 三个模块，约 20 个既有模块改造，`boundaries` 台账、`plugins.md` / `index.md` / `AGENTS.md` |

## 关键决策（详见 plan.md §3、§4）

1. **钩子与功能点统一成"功能埋点"**：一个埋点 = 一个 id（能力 id + 同名广播频道）。
   插件既能**登记实现行**（影响结果），也能**订阅广播**（只想知道），不再有两套注册表：
   删 `agentxx.agent.hooks` / `agentxx.agent.feature`。原钩子 7 点 + 原功能点 3 点合并成 10 个埋点
   （`agentxx.agent.point.*`）。
2. **通用能力直接做进库**：能力行带作用域（`scopeId`）、宿主（无插件租约）发起调用、
   广播 `end` 语义、`declarePoint/emitPointAsync/endPoint` 原语都放进 `cxx_pluginxx`；
   `PluginNetHub` 补 `format:"markdown"`。库版本 0.2.0 → 0.3.0，表 capabilities v5→v6、
   broadcasts v2→v3，只追加成员。
3. **广播面统一 + 支持结束**：插件间通知、跨端插件数据、内建事件镜像、`plugin.<id>.net`
   全部走 `broadcasts`；属主可 `end`，订阅方收到 `on_end`，`mode=all` 的等待据此收尾
   （不再用 `{"$end":true}` 约定值）。删 `agentxx.agent.event` 与 `agentxx.client.events`。
4. **网络必须走 `agentxx.net`**：agent 侧与 client 侧各一个 `PluginNetService`（包
   `utilxx::PluginNetHub`）；`websearch` / `rag_search` / `chatbot`（HTTP + WS）/
   `text_selection_monitor`（CDP over WS）四处直接建连接的代码全部改写。
5. **插件自定义能力用前缀即可**：`plugin.<插件 id>.*`（能力 id、广播频道、埋点 id 同一规则），
   不需要额外的官方 id 机制；官方/应用侧统一 `agentxx.agent.*`。
6. **`agentxx.agent.mount` 表保留**（挂载查询仍走独立业务表，不并入能力）。
7. **不保留旧兼容**：`AGENTXX_PLUGIN_API_VERSION = 5`，代次 4 的插件被 loader 拒绝；
   删掉的表与结构体不留别名。

## 状态

- 方案设计：**已完成**（`plan.md`，§1~§8 + 附录 A/B）
- 代码实施：**进行中**（逐步骤进度见 `work.md` 顶部表格）
  - 已完成：步骤 0（库改动）、步骤 1（构建与接口表骨架）、步骤 2（agent 侧广播 + 中枢）、
    步骤 4（网络：agent/client 两张表 + SDK 便捷层 + 4 个插件全迁）
  - 进行中：步骤 3（7 个通知型埋点已接到派发；旧三表与取值型埋点待改）、
    步骤 5（client 侧 net 已提供，广播与 `DomainServices` 待做）、
    步骤 6（能力表 API 引起的必改点已迁）
  - 未开始：步骤 7（测试、文档、收尾）

## 结论

（全部完成后回填：实际改动面、与计划的偏差、遗留事项。本轮的结论先记在 `work.md` 各步骤末尾。）
