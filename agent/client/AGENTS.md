# agent/client 局部约束

本文件只写本目录内的硬约束；跨目录规则见仓库根 `AGENTS.md`。界面层设计见
[docs/zh-cn/design/ui-layer.md](../../docs/zh-cn/design/ui-layer.md) 与
[docs/zh-cn/design/tui.md](../../docs/zh-cn/design/tui.md)。

## 目录职责

- 只做界面渲染与用户交互：解析输入、渲染消息/面板/弹窗、上报能力与命中区。
- 数据来源一律由 agent 提供（经 `AgentIOBase` + transport）：client 不直接读
  会话库、不自行推导业务状态。
- 进程内（Channel）与远程（WebSocket）是同一套端点的两种 transport，业务代码
  不要按 transport 分支写。

## 引用边界

- 只能包含 `agent/lib/include`、`agent/client/include` 下的公开头；不得包含
  `agent/lib/src/**`（边界检查见测试模块 `boundaries`）。
- 需要 lib 侧新数据时，优先在 lib 的公开头/协议里补接口，不要在 client 里复制
  一份实现。

## 渲染约定

- 组件描述（`agentxx.ui.item`）只有一份解析与渲染实现：`ui_components.cpp`；
  测量与真实布局共用同一函数（`measureItem` 内部渲染一次），改动折行/测量规则
  必须两处同步。
- 能力降级由库的 `adapt(caps)` 统一处理：渲染层不要写“我不支持谁”，能力表
  `tuiUiCapabilities()` 只如实上报。
- 命中区经 `OwnedReflect` / `UiHitRegistry` 登记；滚动容器用 `hitTestItem`，
  不要用子项 `reflect` 反射命中。
- 界面文案走 `tui_i18n` 字典，避免在渲染代码里散落面向用户的硬编码文本。

## 测试

- 渲染/命中/测量相关改动跑 `ui_items`、`tui_ui_items`、`tui_form`、
  `tui_widget`、`tui_tool_header` 模块；测试源码在 `agent/test/client`。
