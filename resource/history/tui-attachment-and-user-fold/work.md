# 实施记录: TUI 消息列表附件显示与用户消息初始折叠

方案见同目录 [plan.md](plan.md)。本文记录**实际做了什么**与验证结果。

- 时间: 2026-11-08
- 分支: 当前工作树

## 进度

- [x] `tui_state.h`: 阈值常量 + `shouldCollapseUserMessage` (含 `utilxx_base/string_util.h`)
- [x] `message_list.cpp`: User 分支渲染 (折叠右侧计数 / 展开末尾列表) + `quickHeight`
- [x] `message_list.h`: 命中区注释由"附件卡片"改为"附件行"
- [x] `tui_i18n.cpp`: `msg.attachCount` / `msg.attachCountOne` 新增, `msg.attachOpen` 改文案
- [x] `agent_tui.cpp`: `foldLongUserMessage(s)` + 四条接收路径 + 两处本地行
- [x] `test_tui_scroll.cpp`: 新增 `testUserMessageAttachments` / `testUserMessageFoldOnReceive`
- [x] `docs/zh-cn/design/tui.md`: §2.18 重写 (折叠阈值 + 附件形态), §2.19 补充例外说明,
      §3.x 命中区用词
- [x] 编译 (客户端 Debug + 测试 Debug) 与模块/全量测试

## 改了哪些文件

### 1. `agent/client/include/agentxx-client/io/tui/framework/tui_state.h`

```cpp
inline constexpr size_t kUserMessageFoldMaxNewlines = 5;
inline constexpr size_t kUserMessageFoldMaxChars    = 200;

inline bool shouldCollapseUserMessage(std::string_view text) noexcept {
    size_t newlines = 0;
    for (const char c : text) {
        if (c == '\n' && ++newlines > kUserMessageFoldMaxNewlines) {
            return true;
        }
    }
    return utilxx_base::utf8GetLength(text) > kUserMessageFoldMaxChars;
}
```

- 先数 `\n` (命中即返回, 长文本不必全扫), 再按码点数字数
- 头文件新增 `utilxx_base/string_util.h` (码点计数) 与 `<string_view>`

### 2. `agent/client/src/io/tui/components/message_list.cpp`

- 新增 `attachmentCountText(n)`: `1` 用 `msg.attachCountOne`, 其余 `trf("msg.attachCount", n)`
- User 分支:
  - 展开: 先正文行, 再 `|- n个附件（点击打开）` 头行, 再每条附件一行
    `|- <类型图标>: <文件名> <大小>` (每行保留自己的 `reflect` 命中区)
  - 折叠: `hbox({"+ ", 预览|xflex_shrink, filler(), "n个附件"})`,
    预览列宽预算扣掉计数宽度 + 1 列间隔; 正文为空但有附件时该行仍显示 (只有标记 + 右侧计数)
  - 附件行去掉了原来每行重复的 `( 点击显示 )` (提示改到计数头行)
- `quickHeight` 的 User 分支同步:

  ```text
  折叠: (!textBlank || 有附件) ? 1 : 0
  展开: (正文可见或没有附件 ? estimateLines(text) : 0) + (有附件 ? 1 + 附件数 : 0)
  两种都 +1 (尾部空行)
  ```

- 注释里"附件卡片"统一改为"附件行"/"附件列表"

### 3. `agent/client/src/io/tui/tui_i18n.cpp`

| key | en | zh |
|---|---|---|
| `msg.attachCount` | `{} attachments` | `{}个附件` |
| `msg.attachCountOne` | `1 attachment` | `1个附件` |
| `msg.attachOpen` (改) | ` (click to open)` | `（点击打开）` |

### 4. `agent/client/src/io/tui/agent_tui.cpp`

- 匿名命名空间新增:

  ```cpp
  void foldLongUserMessage(TUIMessage& row);                              // 单行
  void foldLongUserMessages(const std::vector<std::shared_ptr<TUIMessage>>&, size_t begin, size_t end);
  ```

  只处理 `Role::User`, 只把 `collapsed` 置 `true` (不置 `false`),
  `local_` 前缀的本地行跳过
- 应用点:
  - `onDelta` / `InsertMessage`: `applyDelta` 后用 `rowById(delta.message->id)` 取回新行判定
  - `onSync` (整份): `replica.reset(payload)` 之后遍历全部行 (本地行被跳过)
  - `onIncrementalSync`: 只遍历 `messagesBefore` 之后新追加的行
  - `onViewMessagesPage`: 只遍历前插进来的 `prependedCount` 行
  - 中断输入回显 (`start()` 的 `onSend` 分支与 `sendPluginUserInput`): 建本地行时
    `localUser.collapsed = shouldCollapseUserMessage(text)`

### 5. `agent/test/client/test_tui_scroll.cpp` (新增两个场景 + 工具函数)

- `trimRowEnd(row)`: 去掉屏幕行尾空白 (屏幕按整宽输出)
- `testUserMessageAttachments` (中文界面):
  - 展开: 正文行 +1 行是 `|- 2个附件（点击打开）`, 再往下依次 `photo.png` (12.1 KB)、
    `clip.mp4` (2.0 MB); 每条附件行里不再重复"点击打开"; `attachmentHitBoxes().size() == 2`
  - 折叠: `+ 预览` 行去尾后以 `2个附件` 结尾 (贴在最右侧), 附件列表不出现,
    `attachmentHitBoxes().size() == 0`
  - 单个附件: 右侧是 `1个附件` (单数文案)
- `testUserMessageFoldOnReceive`:
  - 边界: 5 个 `\n` 展开 / 6 个折叠; 200 个 ASCII 展开 / 201 折叠;
    200 个中文 (600 字节) 展开 / 201 折叠
  - 接收路径: `insert_message` 落两条用户行 (短/长) → `collapsed` 分别 false/true, 正文不被改写
  - 整份 `onSync` 的两条历史用户行同样按规则定初始状态
  - 其它角色 (Think) 不受影响 (agent 给的 `collapsed` 原样保留)
  - 用测试子类 `FoldTUIClientIO` 暴露 `onDelta` / `onSync` (与 `test_tui_stream` 同一手法)

### 6. `docs/zh-cn/design/tui.md`

- §2.18 由"用户消息的展开/折叠"改写为"用户消息的展开/折叠与附件显示":
  加上阈值/判定时机/两种附件形态/命中区/高度估算/测试指引
- §2.19 "折叠默认值也在 agent 端定义" 后补一句: 用户自己的消息是唯一例外 (TUI 收到时判定,
  只影响展示)
- §3.x 命中区说明里的"附件卡片"改为"附件行"

## 验证

```
# 编译 (Debug)
cmake --build <build>/agentxx_client_repo-prefix/src/agentxx_client_repo-build --config Debug --target agentxx
cmake --build <build>/agentxx_test_repo-prefix/src/agentxx_test_repo-build     --config Debug --target agentxx_test
```

- 相关模块 (全部通过):

  ```
  tui_scroll 961 / tui_stream 192 / tui_tool_header 169 / tui_interrupt 303 / tui_input 124
  ```

- 全量 `agentxx_test --log-level warn`:

  ```
  Total: passed=42295 failed=1
  ```

  唯一失败是 `boundaries` 规则 15 (称呼与用词) 命中
  `agent/third_party/cxx_pluginxx_ui` 中既有的"载体"用词 (该目录本次未改动,
  属既有失败); 本次改动的文件通过了同一规则的检查。

- 抓帧确认排版 (调试打印后已删除):

  ```
  展开:
     - look at ATTACHPHOTO7
     |- 2个附件（点击打开）
     |- 📷 图像附件: photo.png 12.1 KB
     |- 🎬 视频附件: clip.mp4 2.0 MB

  折叠 (viewport 100 列, 内容区 94 列):
     + look at ATTACHPHOTO7                                        2个附件
  ```

## 注意事项 / 后续

- 阈值是代码内常量 (`tui_state.h`), 没有加配置键; 若需要让用户调整, 再加
  `tui.*` 配置项并同步 `config-keys.md`
- 整份 `onSync` 会重建所有行, 因此用户手动展开过的长消息在重连/切会话后会回到折叠;
  与其它角色的折叠状态行为一致, 未额外保存"用户点击过的行"的本地记忆
- 事件 `Role::Event` 与工具 `Role::Tool` 不会带附件, 因此附件段只在 User 分支实现;
  若将来其它角色也可能带附件, 需要把 `buildMessageBlock` 的附件段抽成共用函数
- 新增 i18n 键只有 `msg.attachCount` / `msg.attachCountOne`; `msg.attachOpen` 语义
  变成"点击打开的提示", 复用该键而不是新增, 免得同一句提示出现两份
