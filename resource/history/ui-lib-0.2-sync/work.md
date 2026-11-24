# 实施记录：跟进 cxx_pluginxx_ui 0.2

## 进度

- [x] 认库的变化（`cxx_pluginxx_ui` 0.2 词汇表、`cxx_pluginxx` 广播默认值、`cxx_utilxx` 网络段）
- [x] 扩展 kit 定义改词汇并重新生成（`schemaVersion` 2 / `kitVersion` 2）
- [x] `agent/lib` 侧改名与中断界面组装
- [x] `agent/client` 侧改名 + 按钮渲染重写 + 能力段
- [x] 内置插件与 `example_js` 手写 JSON 改名
- [x] 用例夹具与断言跟进
- [x] 文档（`ui-layer.md` / `plugins.md` 中英 / `tui.md` / `js/README.md` / `AGENTS.md`）
- [x] 广播默认发布权对齐框架新默认值 + 两处用例口径修正
- [x] 构建 + 定向测试 + 全量回归

## 每一步改了什么

### 1. 认库的变化

三个子模块被移动：

| 子模块 | 旧 → 新 | 实质 |
| --- | --- | --- |
| `cxx_pluginxx_ui` | `fa4e323` → `aaa1d511` | 0.2.0：`uiApiVersion` 3 → 4，语义色 `tone` → `color`，动作收敛 `call/route/none`，按钮变体 `normal/full/simple` + `highlight`/`size`，单块容器子节点改成 `child`，新增 `ThemeScope`/`UISlot`，基础 kit `kitVersion` 3 |
| `cxx_pluginxx` | `bfd8601` → `2ddfdfe8`（含 `5b1a37a`） | 插件频道默认 `publish: anyone`（声明里没写 `publish` 时）；`end` 不再核对属主 |
| `cxx_utilxx` | `ff9d6b86` → `361d112` | 删掉网络段七个数量上限（只改 `src/net_hub.cpp`） |

### 2. 扩展 kit 定义与生成物

`agent/schema/agentxx-ui-kit.def.json`：

- `schemaVersion` 1 → 2、`kitVersion` 1 → 2；
- 11 处 `tone` 参数与参数类型 `tone` → `color`，`statusTone` → `statusColor`；
- `card` 的参数 `children` → `child`；
- `interruptRow` 的按钮变体 `primary` → `normal`、`ghost` → `simple`。

用一次性脚本把**单内容容器**的 `children` 改成 `child`（多块时包一层 `Column`）后跑
`agent/script/gen_ui_kit.ps1` 重新生成 `agentxx_ui_kit.g.h` / `agentxx_ui_kit.js` /
`docs/zh-cn/design/agentxx-ui-kit.md`。生成器会校验模板字段是否在 schema 里声明，
`Block`/`Expanded` 一类若仍写 `children` 会直接报“未声明的字段”，这个校验当成了改对的依据。

### 3. 代码改名（机械部分）

按标识符边界批量替换：`.tone` → `.color`、`->tone` → `->color`、`kTone/vTone` →
`kColor/vColor`、`"tone"` → `"color"`、`statusTone` → `statusColor`、`todoTone` →
`todoColor`、独立的 `tone` 标识符 → `color`；`Action::Kind::Dispatch` → `Call`、
`build::dispatch(` → `build::call(`。

涉及：`ui_components.cpp/.h`、`plugin_ui_items.h`、`status_bar.cpp`、`overlays.cpp`、
`interrupt_ui.cpp`、`interrupt_presets.cpp`、`client_plugin_api.h`、`client_plugin_kit.h`，
插件 `chatbot_panel_ui.cpp` / `agentxx_codegraph.cpp` / `agentxx_planning.cpp` /
`agentxx_system_monitor.cpp`；用例 `test_chatbot_core` / `test_interrupt_ui` / `test_ui_items` /
`test_ui_kit` / `test_tui_tool_header` / `test_tui_ui_items` / `test_client_plugins` /
`test_plugin_sdk`。

排除项（同名但是英文散文/别的词）：`agent/lib/src/agent/training.cpp` 的 `tone`、
`client_plugin_manager.cpp` 的 `tombstone`。

### 4. 按钮渲染重写（`ui_components.cpp`）

- 新增 `buttonCaption(label, glyph, icon)` 与 `buttonWidth(caption)`：`label` 可省，
  只用 `glyph` 表示纯图标按钮；`icon` 在终端没有图标表，忽略；
- `buttonElement(..., glyph, highlight)`：`simple` 画成纯文字（`highlight` 时加粗），
  `full` 与 `highlight` 走 Accent role，其余按 `buttonRoleOf(color)`；
- Button 分支传 `item.glyph` / `item.highlight`，命中宽度按 `caption` 算；
- `buttons` 控件的选中项改用 `highlight` 参数；
- 顺手修掉两处变量名遮蔽 `ftxui::color`：`buttonElement` 形参 `color` → `roleColor`、
  Text 分支局部 `color` → `resolvedColor`（不修会编译不过）。

### 5. 用例夹具跟新词汇

`"variant":"primary"` → `"normal"`（`test_tui_form` / `test_tui_ui_items` / `test_ui_snapshot`）、
`"kind":"dispatch"` → `"call"`（`test_tui_tool_header` / `test_tui_ui_items` / `test_ui_snapshot` /
`client_plugin_api.h` 文档 / `example_js/plugin.js`）、`Block`/`Collapse`/`Expanded`/`SizedBox`/`Align`
的 `children` → `child`（`test_tui_form` / `test_tui_ui_items` / `test_tui_widget`）、
`card` 的 kit 参数 `children` → `child`（`test_ui_kit`）。

### 6. 广播默认发布权对齐（`cxx_pluginxx` 5b1a37a 的连带改动）

框架把“插件声明 `plugin.<名>.*` 时没写 `publish`”的默认值从 `owner` 改成 `anyone`。
主程序在“订阅一条还没人声明的频道”时会代补声明，原来补的是 `owner`：

- `PluginManager::ensureTopicBroadcast`（`plugin_manager_domain_services.cpp`）
- `ClientPluginManager::ensureClientChannelDeclared`（`client_plugin_manager_domain_services.cpp`）

都改成 `anyone`。原因：频道权限由**先声明的一方**决定（应用声明优先、插件声明后到被忽略），
主程序若补成 `owner`，“先订阅后声明”与“插件先自己声明”会得出不同权限，同一个频道行为
取决于调用顺序。属主（`decl.owner`）仍按名字推断，频道按实例回收不受影响。

连带修两处用例口径：

- `agent/test/plugin/test_client_plugins.cpp`（8.2 段）断言补声明是 `Owner`，改成 `Anyone`；
- `agent/test/plugin/test_plugins.cpp`（`test_start_fail_plugin` 回滚段）原来用
  “发布 `watch` 频道必然失败”来证明订阅已撤销 —— 那其实是靠旧的 `owner` 规则挡住的
  （频道声明在挂载失败回滚时**不会**删除，只有禁用/卸载才 `removeChannelsOwnedBy`）。
  改成直接查频道自省：`infoJson()` 里 `subscribers == 0`、`owner == test_start_fail_plugin`，
  这才是“回滚按挂载摘掉了订阅”的直接证据。

### 7. 文档

- `docs/zh-cn/design/ui-layer.md`：`apiVersion` 4、`color`/`child` 写法、`ThemeScope`/`UISlot`
  本端不声明、kit 表补新组件、动作 kind 只有 `call/route/none`；
- `docs/zh-cn/design/plugins.md` §9 词汇段、§20.1 `publish` 省略时的默认值、§21.1 共用一段
  的两点代价（含“默认 `anyone`、主程序补声明用同一份默认值”）；`docs/en/design/plugins.md` 对应两处；
- `docs/zh-cn/design/tui.md`：`tone` → `color`；
- `agent/js/README.md`：`statusTone` → `statusColor`；
- `AGENTS.md`：新增“词汇代次 (库 0.2)”条目。

### 8. 用词检查（`boundaries` 规则 15）

全量回归时 `boundaries` 报一条：`cxx_pluginxx_ui/include/pluginxx/ui/item.h:467` 的注释
用了停用词 `载体`（规则 15 的扫描目录包含四个 `cxx_*` 自研库）。属于库内注释用词，
在库仓库改掉并单独提交（`**通用扩展数据**：含义由客户端定义…，库不解释它`），
agentxx 侧只跟进子模块指针。

顺带修了 `docs/zh-cn/design/plugins.md` §20.3 里过时的 SDK 名字（`declarePluginBroadcast`
/ `publishPluginBroadcast` → `*NoWait`，并去掉“内部走阻塞形态”的说法 —— 阻塞形态已删），
以及 `plugin_kit.h` 里一行重复的文档注释与“`publish: owner` 时归属就是本插件”的旧说法。

## 与计划不一致的地方

- 计划里写“单块容器最多一项的孩子仍走 `Item::children`”，实际库改成了 `Item::child`
  （单个对象），所以改的不只是 kit 定义，所有 JSON 夹具都要把 `children` 换成 `child`
  （多块包 `Column`）。
- 计划里“`full` 与 `normal` 在终端同渲染”——实现上 `full` 与 `highlight` 一起走 Accent role，
  `normal` 仍按 `color` 取 role，这样 `full` 在终端有可辨识的强调效果。
- 计划里指望“全量 grep `tone` 双保险”，实际发现两处**不该改**的同名标识符（见上文排除项），
  所以按“编译错误 + 逐处人工确认”推进，没有做全局无脑替换。

## 另外两个库的结论

- `cxx_pluginxx`：**需要跟改**（上面第 6 步）。只跟“默认发布权”这一处；
  `end` 不看属主对本项目没有影响（`PluginManager::endBroadcastValue` 一直以应用身份调用）。
- `cxx_utilxx`：**不需要改代码**。删掉的是网络段的数量上限，本项目没有依赖这些上限的
  断言或兜底逻辑；`plugin_net`（38 条）与 `client_plugins`（842 条）复跑均绿。

## 验证

构建：顶层 `cmake --build agent/build/windows-debug --config Debug` `EXIT=0`；
插件 25/25 重链（`agent/script/build_plugins.ps1`）。

全量回归：`agentxx_test --log-level warn` → **passed=43686 failed=0**
（上轮基线 43682，差值来自 `test_plugins` 回滚段换了断言 +1、`plugin_example_chatbot`
媒体段新增 3 条）；插件 DLL 重链后同样跑完无失败模块。

定向用例（全部 `failed=0`）：

| 模块 | 条数 |
| --- | --- |
| `ui_kit` | 167 |
| `ui_items` | 117 |
| `tui_ui_items` | 277 |
| `tui_form` | 84 |
| `tui_widget` | 208 |
| `tui_tool_header` | 166 |
| `tui_surface` | 836 |
| `tui_theme` | 22 |
| `interrupt_ui` | 338 |
| `client_plugins` | 842 |
| `plugin_sdk` | 148 |
| `chatbot_core` | 874 |
| `plugin_broadcast` | 102 |
| `plugin_net` | 38 |
| `plugins` | 708 |
| `plugin_runtime` | 774 |
| `plugin_executor` | 663 |
| `plugin_cleanup` | 95 |
| `plugin_host_mount` | 148 |
| `plugin_multi_mount` | 48 |
| `event_hub` | 288 |
| `event_delivery` | 62 |
| `event_tools` | 97 |
| `plugin_example_chatbot` | 268 |
| `boundaries` | 20 |

全量回归见 `index.md` 的结论段。

### 用词检查（`boundaries` 规则 15）

首轮全量回归时 `boundaries` 报一条：`cxx_pluginxx_ui/include/pluginxx/ui/item.h:467`
的注释用了停用词 `载体`（规则 15 的扫描目录包含四个 `cxx_*` 自研库）。这属于库内的注释用词，
在库仓库里改掉并单独提交（`**通用扩展数据**：含义由客户端定义…，库不解释它`），
agentxx 侧只跟进子模块指针。

顺带修了 `docs/zh-cn/design/plugins.md` §20.3 里过时的 SDK 名字（`declarePluginBroadcast` /
`publishPluginBroadcast` → `*NoWait`，并去掉“内部走阻塞形态”的说法），以及
`plugin_kit.h` 一行重复的文档注释和“`publish: owner` 时归属就是本插件”的旧说法。

### 用例 `plugin_example_chatbot` 的一处假通过

全量回归顺带抖出一条旧用例的假通过：7e/7f/7g 用 `request.find("附件未传给模型")` 验证
“没声明能力时补的那行说明”，但 `describeSkippedMedia` 的输出已经改成英文
`[Unsupport media] 1/视频 count(agentxx config video_input = false)`（提交 `1fd98ea0`，早于本轮）。
它能“通过”是因为测试 agent 的系统提示词偶尔会把 `AGENTS.md`（里面恰好写有这个中文短语）
当成 Memory 注入请求，命中与用例想验的东西无关。

修法：三个段落都改按“计数 + 类型”锚定（`[Unsupport media] 1/视频` / `1/音频` / `1/图片`），
保留配置键名断言；另把紧跟着的注释里的旧短语一并改掉。共 265 → 268 条断言，连跑两次
全绿（各 145 s / 155 s，模块本身带多个 60 s 等待）。

同时观察到一次偶发：“取消后台任务”那段（`chatbot_task` `op:cancel`）的回执既不是“已取消”
也不是“已经结束”，同一次跑里只有它失败；后续两次都能过，属模块自身的时序抽动（后台任务可能
在查询与取消之间收尾，回执落在第三条分支上），本次未改。

## 踩过的坑 / 注意事项

- `Select-String` 偶尔输出 `StdOut conversion to utf8 failed, discard`，中文断言文本会丢；
  需要看原文时改用 `[IO.File]::ReadAllLines` 读日志再写文件。
- PowerShell 批量替换不要用“字符串数组当 pair”的写法，曾把 `test_tui_form.cpp` 写坏
  （`git checkout` 恢复后重做）。
- `test_plugins.cpp` 里 `pluginManager->broadcasts()` 返回 `std::shared_ptr`，写 `auto*` 编译不过。
- 生成物（`agentxx_ui_kit.g.h` / `agentxx_ui_kit.js` / `agentxx-ui-kit.md`）是脚本产物，
  定义改了要重跑 `gen_ui_kit.ps1`，不要手改生成物。
- 提交时只应带三个子模块指针（`cxx_pluginxx_ui` / `cxx_pluginxx` / `cxx_utilxx` 的新提交）；
  `agent/third_party/liburing` 的符号链接 typechange 与 `fmt` 的未跟踪脚本是 Windows 侧
  历史残留，不属于本次改动。
