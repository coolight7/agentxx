# 实施记录

## 进度

- [x] `model/message_window.h`: 判定规则改写 (`planLocalMessageTrim` 两参数版本 +
      `kLocalMessageViewportMargin`)
- [x] `agent_tui.cpp` / `agent_tui.h`: `applyLocalTrimCheck` 改按视口窗口清理;
      不带底部判定; 锁内按最新条数重算; `insert_message` 与 `onIncrementalSync` 补触发点
- [x] `test_tui_model.cpp`: 判定用例改写 (底部 / 上滚 / 视口在顶部 / 锚点越界 / margin 边界)
- [x] `test_tui_scroll.cpp`: 场景 D 断言改为新规则, 新增"上滚到顶 -> 清理量为 0"用例
- [x] `docs/zh-cn/design/tui.md` (§2.13 表格 + §3.5) 与 `docs/zh-cn/design/index.md`
- [x] 判定函数验证 (独立小程序, 见下)
- [x] 子模块 `cxx_pluginxx` 切到 `fcb0834` 使源码树可编译 (按用户指示)
- [x] 模块测试 `tui_model` / `tui_scroll` / `tui_lazy_view`
- [x] 全量测试

## 每一步改了什么 / 怎么验证

### 1. 判定函数

`planLocalMessageTrim(messageCount, firstVisibleMessageIndex)`:

```text
visible      = min(视口顶行下标, 条数)
fromViewport = 条数 - visible
keep         = max(100, fromViewport + min(visible, 3))
清理条数     = 条数 <= keep ? 0 : min(条数 - keep, visible > 3 ? visible - 3 : 0)
```

验证: 临时程序 (`%TEMP%/check_trim.cpp`, 已用 VC 工具链 `/utf-8 /std:c++latest` 编译)
只包含该头文件, 覆盖用例 + 穷举 (`条数 0..400 × 视口下标 0..条数`) 三条不变量,
输出 `all checks passed`。用例里曾把"视口在倒数第 160 条"的期望写成 `160 - margin`,
实际应为 `visible - margin` (清理到视口上方 3 条为止), 已按实际规则修正。

### 2. 端点

`TUIClientAgentIO::applyLocalTrimCheck` (UI 线程帧首动作):

- 去掉 `messageList_->isStickToBottom()` 判定与 `planLocalMessageTrim` 的 `atBottom` 参数;
- 先取 `trimSafeHeadCount(快照条数)` 作为视口顶行下标, 进锁后用 `replica.rows().size()`
  重算一次清理条数 (排队期间列表可能被替换/变短), 为 0 时直接返回;
- 删除后照旧 `history.noteTrimmedHead(dropped)` + `messageList_->onHistoryTrimmed(dropped)`;
- 末尾 `left > kLocalMessageLimit` 时仍再投递一次检查 (下一帧锚点已在视口位置,
  判定结果 0 即自然停止)。

触发点补充: `Type::InsertMessage` 分支与 `onIncrementalSync` 在条数超过上限时投递检查
(原来只有本地行/瞬态提示会投递)。

### 3. 文档

`tui.md` §3.5 改为"保留窗口 = max(末尾上限, 视口及其上方几条)"并列出触发点;
§2.13 模型表格同步; `index.md` 的"客户端本地消息条数上限"条目同步。

## 编译与测试

环境: Windows debug (ASan/UBSan), 机器空载; 全量构建 `windows_debug_build.bat` 通过
(只有既有 warning)。为恢复可编译, `agent/third_party/cxx_pluginxx` 切换到 `fcb0834`
(接口重构之前的最后一个提交, 仍有通用 `events` 表), 并删掉
`agent/build/windows-debug/cxx_pluginxx_repo-prefix` 让 ExternalProject 重新 configure
(两个提交间删过源文件 `src/broadcast_registry.cpp`, 不重配置会编到不存在的文件)。

```text
# 本次直接相关的三个模块
agentxx_test --log-level warn tui_model tui_scroll tui_lazy_view
  -> tui_scroll passed=1004 / tui_lazy_view passed=3851 / tui_model passed=160
  -> 合计 passed=5015 failed=0

# 19 个 client/TUI 模块 (tui_widget tui_interrupt tui_input tui_settings tui_sidebar
# tui_context_overlay tui_tool_header tui_surface tui_theme tui_ui_items ui_items
# ui_snapshot view_replica interrupt_ui banner_art tui_form)
  -> passed=8795 failed=0

# 全量
agentxx_test --log-level warn
  -> passed=42721 failed=0
```

`tui_scroll` 场景 D 的关键断言 (新规则生效): 贴底时清理量 = `视口顶行 - 3`;
上滚到顶时清理量为 0; 头部删除后视口逐行一致、历史窗口起点右移。

## 未完成

- 主程序适配 `cxx_pluginxx` 新接口代次 (`events` -> `broadcasts`) 后, 把子模块切回
  `159f961` 或更新的提交再跑一次全量测试。
- 子模块现在是 detached HEAD (`fcb0834`), 与父仓库记录的 `159f961` 不同;
  `git status` 会显示 `m agent/third_party/cxx_pluginxx`, 属预期。

## 注意

- 上滚期间保留窗口随新消息一起变长 (视口距末尾的条数 + 3), 所以停在原地看历史时
  不会继续清理; 滚回底部后下一次新消息会把列表收回末尾 100 条。这是规则的直接结果,
  不是遗漏。
- 上滚期间若正好停在已加载窗口顶部, 分页拉回的一段在下一条新消息到达时可能又被清掉,
  再次上滚会重新拉取 (行为正确, 只是多一次分页请求)。
