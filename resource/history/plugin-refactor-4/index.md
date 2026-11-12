# 任务卡片: 适配 cxx_pluginxx 0.3.0 的插件体系重构

## 目标

把 `agentxx` 主程序与它自带的 17 个内置插件 + 9 个示例插件，整体迁移到新版
`cxx_pluginxx`（0.2.0，代次 5），并按新框架的两个通用机制（**能力 capabilities** /
**广播 broadcasts**）重排插件扩展面；**不保留旧版本兼容**，最低支持代次直接抬到 5。
新框架在本项目里缺的通用能力（能力行带作用域、宿主发起调用、广播结束、扩展点原语、
网络 markdown 形态）**直接改库补上**，供其它主程序复用。

## 范围

| 面 | 内容 |
|---|---|
| 库 | `cxx_pluginxx`：能力行带 scope、宿主发起调用、广播结束、扩展点原语（L1~L4、L6、L7）|
| 库 | `cxx_utilxx`：`PluginNetHub` 补 `format:"markdown"` 与 `sslVerify`（L5）|
| 构建 | `PLUGINXX_APP_NAME=agentxx`、API 代次 4→5、`cxx_utilxx` → `cxx_pluginxx` 的构建顺序 |
| 主程序（agent 侧） | `DomainHooks`→`DomainServices`、删钩子/功能点/事件三套自有注册表、扩展点派发、广播频道与总线桥、`agentxx.net` 接入 |
| 主程序（client 侧） | 删自有 json/log/events 三表、接通用表与广播、跨端插件数据改走广播、同样提供 `agentxx.net` |
| SDK | `plugin_kit.h` 按新框架重写：扩展点 / 广播 / 能力 / 网络的便捷层 |
| 插件 | 17 个内置插件 + 9 个示例插件全部重编、改写注册面；**4 个网络插件改走 `agentxx.net`** |
| 接口表 | agent 侧 20 张（10 张框架通用 + 10 张业务）+ 单独的 `agentxx.net`；client 侧 17 张（10 张框架通用 + `net` + 6 张自有） |
| 测试 / 文档 | 新增 `plugin_point` / `plugin_broadcast` / `plugin_net` 三个模块，约 20 个既有模块改造，`boundaries` 台账、`plugins.md` / `index.md` / `AGENTS.md` |

## 关键决策（详见 plan.md §3、§4）

1. **钩子与功能点统一成"扩展点"**：一个扩展点 = 一个 id（能力 id + 同名广播频道）。
   插件既能**登记实现行**（影响结果），也能**订阅广播**（只想知道），不再有两套注册表：
   删 `agentxx.agent.hooks` / `agentxx.agent.feature`。原钩子 7 点 + 原功能点 3 点合成 10 个扩展点
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
5. **插件自定义能力用前缀即可**：`plugin.<插件 id>.*`（能力 id、广播频道、扩展点 id 同一规则），
   不需要额外的官方 id 机制；官方/应用侧统一 `agentxx.agent.*`。
6. **`agentxx.agent.mount` 表保留**（挂载查询仍走独立业务表，不并入能力）。
7. **不保留旧兼容**：`AGENTXX_PLUGIN_API_VERSION = 5`，代次 4 的插件被 loader 拒绝；
   删掉的表与结构体不留别名。

## 状态

- 方案设计：**已完成**（`plan.md`，§1~§8 + 附录 A/B）
- 代码实施：**已完成**（步骤 0~7，逐步进度与逐条改动见 `work.md`）
- 收尾：**已完成**（全量构建 / 全模块测试 / 文档同步 / 静态自检；手工验收项留给人工）

## 结论

**实际改动面**（细节见 `work.md` 各步骤）：

- 库：`cxx_pluginxx` 0.3.0（能力行带 scope 与实现链、`broadcasts` v3 加 `end`、扩展点原语
  `declarePoint`/`emitPointAsync`、`DomainServices` 取代 `DomainHooks`、`PluginHostLifecycle`
  骨头）；`cxx_utilxx` 的 `PluginNetHub` 改名并泛化为 `utilxx::NetHub`（按 owner 记账）
- 主程序：agent 侧 10 张业务表 + 10 张框架通用表 + `agentxx.net`；client 侧 17 张表；
  删除 `agentxx.agent.hooks` / `agentxx.agent.feature` / `agentxx.agent.event` /
  `agentxx.client.events|json|log` 与对应实现（约 600 行）
- 插件：26 个全部重编，通知面与网络全部改走广播表与 `agentxx.net`

**与计划的偏差**

1. 表数量：计划写 agent 侧 21 张，实际常量 20 —— `agentxx.net` 在 agent 侧不计入
   `kInterfaceTableCount`（头文件与文档都写明“单独一张”），client 侧 17 张里包含 net
2. 计划里的“功能埋点”落地时统一叫**扩展点**（`boundaries` 规则 15 拦停用词）；
   `plugin_hooks` 合并进 `plugin_point`，`plugin_feature` 保留为插件面用例
3. 计划里的 `PluginNetHub` 拆成两层：库里的 `utilxx::NetHub` + 主程序的
   `agentxx::plugin::PluginNetService`（适配接口表、事件、租约）
4. `interpreter.js` 能力的“方法”从能力 method 挪进 args 的 `op` 字段（库侧取消 method
   后的唯一可行迁移，插件间协议因此有一处不兼容变更）

**遗留**

- 手工验收（空列表启动 26 插件全加载、禁用→启用账目归零、chatbot 断网重连、客户端重连
  后插件面板/装饰重建）需要人工开 TUI 或接真实平台；自动化用例已覆盖等价语义
- ASan 构建下有一批模块因搜狗输入法 TSF DLL 的 heap-use-after-free 中断（无 agentxx 帧），
  与本次改动无关，排查时勿当代码缺陷

