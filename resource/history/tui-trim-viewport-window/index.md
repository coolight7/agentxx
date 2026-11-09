# TUI 消息列表淘汰改按视口窗口

## 目标

客户端展示历史 (TUI) 的本地条数上限改为**按视口位置**决定保留窗口:

```text
保留窗口 = max(末尾 kLocalMessageLimit (100) 条,
               视口顶行及其后的全部消息 + 视口上方 kLocalMessageViewportMargin (3) 条)
```

比保留窗口更旧的头部消息一律清理; 不再要求"滚动位置在底部"才清理 —— 用户上滚看历史时
视口之前的更早消息也会被删掉 (服务端仍保留, 上滚到窗口顶部时按页拉回)。

例: 视口顶行是倒数第 160 条 -> 保留末尾 163 条, 更旧的清理掉。

## 范围

| 文件 | 改动 |
|------|------|
| `agent/client/include/agentxx-client/io/tui/model/message_window.h` | `planLocalMessageTrim` 改签名为 `(messageCount, firstVisibleMessageIndex)`, 新增 `kLocalMessageViewportMargin = 3` |
| `agent/client/src/io/tui/agent_tui.cpp` | `applyLocalTrimCheck` 去掉底部判定并按最新条数重算; `insert_message` 与增量补拉也投递清理检查 |
| `agent/client/include/agentxx-client/io/tui/agent_tui.h` | 成员注释同步; 删除未实现的 `pushLocalMessageLocked` 声明 |
| `agent/test/client/test_tui_model.cpp` | 判定函数用例改写 (含上滚不贴底的场景) |
| `agent/test/client/test_tui_scroll.cpp` | 场景 D 按新规则断言 + 上滚到顶的用例 |
| `docs/zh-cn/design/tui.md`, `docs/zh-cn/design/index.md` | 设计说明同步 |

## 状态

代码与文档已改完; 判定函数穷举验证通过; **编译与全量测试已通过**:

```text
agentxx_test --log-level warn                        -> passed=42721 failed=0
agentxx_test --log-level warn tui_model tui_scroll tui_lazy_view
                                                     -> passed=5015  failed=0
```

为恢复可编译, `agent/third_party/cxx_pluginxx` 按用户指示切到 `fcb0834`
(接口重构之前的最后一个提交, 仍有通用 `events` 表), 并删掉
`agent/build/windows-debug/cxx_pluginxx_repo-prefix` 让 ExternalProject 重新 configure
(两个提交间删过源文件 `src/broadcast_registry.cpp`, 不重配置会编到不存在的文件)。
主程序尚未适配 `cxx_pluginxx` 的新接口代次 (`events` -> `broadcasts`), 那是独立任务。
