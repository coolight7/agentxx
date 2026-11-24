# 跟进 cxx_pluginxx_ui 0.2 的界面描述层重构

## 目标

`cxx_pluginxx_ui` 从 `fa4e323` 升到 `aaa1d511`（0.2.0，`uiApiVersion` 3 → 4），
界面词汇与 kit 都换了写法。把 agentxx 的 TUI 客户端、中断界面、扩展 kit、插件与用例
同步到新词汇，并把新能力按 TUI 的实际情况取舍（能画的上报、画不出的不声明）。

同时确认另外两个库的新提交（`cxx_pluginxx` 广播默认放开、`cxx_utilxx` 删网络段上限）
是否需要 agentxx 跟改。

## 范围

- `agent/lib/src/middlewares/interrupt_ui.cpp` / `interrupt_presets.cpp`（中断界面组装）
- `agent/lib/include/agentxx/plugin/api/client_plugin_kit.h`（插件侧便捷组件）
- `agent/client/src/io/tui/ui_components.cpp` + `components/*.cpp`（能力段与渲染）
- `agent/schema/agentxx-ui-kit.def.json` + 生成物（`agentxx_ui_kit.g.h` / `agentxx_ui_kit.js`）
- 内置插件里手写 `tone` 的地方（chatbot 面板 / planning / codegraph / system_monitor）
- 用例与文档（`ui-layer.md`、`plugins.md` §9、`AGENTS.md`）

## 结论

**已完成**（2026-11-24）。计划见 `plan.md`，逐步骤与验证见 `work.md`。

- **`cxx_pluginxx_ui` 0.2**：词汇全量跟进 —— `tone` → `color`（含 `kTone/vTone`、
  `statusTone` 一类派生名）、动作 kind 收敛为 `call/route/none`、按钮变体
  `normal/full/simple` + `highlight`/`size`、单内容容器的孩子改成 `child`（单个对象）。
  扩展 kit 定义升到 `schemaVersion` 2 / `kitVersion` 2 并重新生成三份产物；TUI 重写了
  按钮渲染（`buttonCaption` / `buttonWidth` / 新的 `buttonElement`），`ThemeScope` 与
  `UISlot` 按“终端没有对应语义”**不声明**（由库的 `adapt` 展开/跳过）。
- **`cxx_pluginxx`**：**需要跟改一处**。框架把插件声明 `plugin.<名>.*` 且没写 `publish`
  时的默认发布权改成 `anyone`，主程序在“订阅还没人声明的频道”时补的声明于是也要改成
  `anyone`（`PluginManager::ensureTopicBroadcast` 与 `ClientPluginManager::
  ensureClientChannelDeclared`），否则同一个频道的发布权取决于“谁先声明”。连带修了两处
  用例的断言口径：一处断言补声明是 `owner`，一处用“发布必然失败”证明订阅已撤销
  （那其实靠旧的 owner 规则挡住，现改为直接查 `infoJson()` 的 `subscribers == 0`）。
- **`cxx_utilxx`**：**不需要改代码**（删的是网络段数量上限，本项目没有依赖它们的断言）。
- 顺带两处：
  - `boundaries` 规则 15 命中库内注释里的停用词 `载体`，在库仓库改掉后单独提交，
    agentxx 侧只跟进子模块指针；
  - `plugin_example_chatbot` 有 3 处旧断言按 `附件未传给模型` 找说明行，而代码早已改成
    `[Unsupport media] …`，之前靠系统提示词里恰好注入的 `AGENTS.md` 正文“假通过”；
    改为锚定 `[Unsupport media] 1/视频|音频|图片`（265 → 268 条断言）。
- 全量回归 `agentxx_test --log-level warn`：**passed=43686 failed=0**
  （上轮基线 43682；差值 = `test_plugins` 回滚段换断言 +1、
  `plugin_example_chatbot` 媒体段新增 3 条）。定向模块条数见 `work.md` 的“验证”段。

## 相关提交

- 库仓库 `cxx_pluginxx_ui`：注释用词调整（`通用载体` → `通用扩展数据`）
- 主仓库：词汇跟进 + 广播默认发布权对齐 + 用例/文档更新
