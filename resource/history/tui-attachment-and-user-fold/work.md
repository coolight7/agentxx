# 实施记录: TUI 消息列表附件显示与用户消息初始折叠

方案见同目录 [plan.md](plan.md)。本文记录**实际做了什么**与验证结果。

- 时间: 2026-11-08
- 分支: 当前工作树

## 进度

- [x] `tui_state.h`: 阈值常量 + `shouldCollapseUserMessage` (含 `utilxx_base/string_util.h`)
- [x] `message_list.cpp`: 附件段共用件 + 七个角色分支接入 + `quickHeight` 统一加附件行数
- [x] `message_list.h`: 声明 `appendAttachmentList`; 命中区注释由"附件卡片"改为"附件行"
- [x] `tui_i18n.cpp`: `msg.attachCount` / `msg.attachCountOne` 新增, `msg.attachOpen` 改文案
- [x] `agent_tui.cpp`: `foldLongUserMessage(s)` + 四条接收路径 + 两处本地行
- [x] `test_tui_scroll.cpp`: 新增 `testUserMessageAttachments` / `testRoleSharedAttachmentSection` /
      `testUserMessageFoldOnReceive`
- [x] `docs/zh-cn/design/tui.md`: §2.18 重写 (折叠阈值 + 所有角色共用的附件段), §2.19 补充例外说明,
      §3.x 命中区用词
- [x] 编译 (客户端 Debug + 测试 Debug) 与模块/全量测试
- [x] 后续: 用户删掉 User↔Event 兼容与折叠预览的 `theme.dim()` 后, 同步测试与文档 (见文末)

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

共用件 (匿名命名空间 + 成员函数):

```cpp
std::string attachmentCountTextOf(const TUIMessage&);      // 无附件 -> 空串
int         attachmentCountColumnsOf(const TUIMessage&);   // 预览预算要扣的列数 (+1 间隔)
void        appendAttachmentCount(Elements&, const TUIMessage&, const TUITheme&);
bool        isCollapsibleRole(TUIMessage::Role);           // 可折叠角色名单唯一来源
size_t      attachmentListLinesOf(const TUIMessage&);      // 折叠态 0 / 展开态 1 + 附件数

// 成员: 展开态列表 (头行 + 每条附件一行, 逐行登记 attachmentHits_)
void MessageListComponent::appendAttachmentList(Elements& out, const TUIMessage& msg, size_t msgIndex);
```

各角色分支接入:

| 角色 | 折叠态 | 展开态 |
|---|---|---|
| User | 头行末尾 `appendAttachmentCount` (正文为空但有附件时也画这一行) | `appendAttachmentList` |
| Event / System / Tip / Think | 同上 (预览预算加 `attachmentCountColumnsOf`) | 正文之后 `appendAttachmentList` |
| Tool | 同上 (三个 `collapsedPreviewBudget` 都含计数列数) | 参数/结果/装饰体之后 `appendAttachmentList` |
| Assistant | 不可折叠 | 永远列表 (`vbox({markdown, list...})`) |
| Interrupt | 不可折叠 | 中断块之后 `appendAttachmentList` |

- `OnRender` 的可折叠命中登记改用 `isCollapsibleRole(msg.role)` (与附件排版同一名单)
- `quickHeight`: 改成 `contentLines + attachmentListLinesOf(msg) + 1`, `contentLines` 保留各角色原估算
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

### 5. `agent/test/client/test_tui_scroll.cpp` (新增三个场景 + 工具函数)

- `trimRowEnd(row)`: 去掉屏幕行尾空白 (屏幕按整宽输出)
- `testUserMessageAttachments` (中文界面):
  - 展开: 正文行 +1 行是 `|- 2个附件（点击打开）`, 再往下依次 `photo.png` (12.1 KB)、
    `clip.mp4` (2.0 MB); 每条附件行里不再重复"点击打开"; `attachmentHitBoxes().size() == 2`
  - 折叠: `+ 预览` 行去尾后以 `2个附件` 结尾 (贴在最右侧), 附件列表不出现,
    `attachmentHitBoxes().size() == 0`
  - 单个附件: 右侧是 `1个附件` (单数文案)
- `testRoleSharedAttachmentSection`:
  - 工具消息 (折叠, 2 附件): 折叠头行首字符为 `+`, 去尾后以 `2个附件` 结尾, 附件名不出现,
    且没有附件命中区 (全局命中区只有正文消息那 1 行)
  - 正文消息 (Assistant, 不可折叠, 1 附件): 正文下一行就是 `|- 1个附件（点击打开）`,
    再下一行是 `asst.png`
  - 事件消息 (默认折叠, 1 附件): 折叠头行包含事件名, 且 `1个附件` 明显靠右
    (该行可能与右侧跳转按钮列同高, 因此不按行尾判定)
  - 工具消息展开: 结果行之后才是 `|- 2个附件（点击打开）` 与附件行; 命中区 3 个
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
  tui_scroll 979 / tui_stream 192 / tui_tool_header 169 / tui_interrupt 303 /
  tui_input 124 / tui_widget 194 (另有 tui_lazy_view 3851 / ui_items 117 / ui_snapshot 22 / view_replica 114)
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
  用户消息展开:
     - look at ATTACHPHOTO7
     |- 2个附件（点击打开）
     |- 📷 图像附件: photo.png 12.1 KB
     |- 🎬 视频附件: clip.mp4 2.0 MB

  用户消息折叠 (viewport 100 列, 内容区 94 列):
     + look at ATTACHPHOTO7                                        2个附件

  工具消息折叠 / 展开:
     + [工具] my_tool tool result body                             2个附件
     - [工具] my_tool
         参数: {
                 "path": "a.txt"
               }
         结果: tool result body
     |- 2个附件（点击打开）

  正文消息 (Assistant, 不可折叠):
     asst body ASSTBODY2
     |- 1个附件（点击打开）
     |- 📷 图像附件: asst.png 2.0 KB
  ```

## 注意事项 / 后续

- 阈值是代码内常量 (`tui_state.h`), 没有加配置键; 若需要让用户调整, 再加
  `tui.*` 配置项并同步 `config-keys.md`
- 整份 `onSync` 会重建所有行, 因此用户手动展开过的长消息在重连/切会话后会回到折叠;
  与其它角色的折叠状态行为一致, 未额外保存"用户点击过的行"的本地记忆
- 事件 `Role::Event` / 工具 `Role::Tool` / 思考 / 提示 / 正文 / 中断都走同一段附件实现; 若将来
  新增角色, 只需在自己的分支里调 `appendAttachmentList` (展开) 与 `appendAttachmentCount`
  (折叠), 并确认 `isCollapsibleRole` 与 `attachmentListLinesOf` 的判定
- 新角色若不可折叠, 要确认 `isCollapsibleRole` 返回 false 且渲染分支按展开态接入附件列表
  (否则附件会只在折叠态丢得看不到, 与 `quickHeight` 也会不一致)
- 新增 i18n 键只有 `msg.attachCount` / `msg.attachCountOne`; `msg.attachOpen` 语义
  变成"点击打开的提示", 复用该键而不是新增, 免得同一句提示出现两份

## 后续 (同日): 去掉 User↔Event 兼容与折叠预览的 dim

用户自己改的 (按现状修测试, 不改回):

- `message_list.cpp` 的 User 分支: 不再画事件来源头 (`msg.eventSource` 只留给 `Role::Event`),
  折叠态单行预览去掉 `theme.dim()` (与正文同色); Event 分支的 dim 保留
- `conversation_types.h`: `ViewMessage::fromJson` 删除把 `role=user` + `source=event:*`
  归一化成 `Role::Event` 的兼容代码 (也不再据此补 `collapsed=true`) —— 角色按 JSON 原样解析

我改的:

| 文件 | 改动 |
|---|---|
| `agent/test/client/test_tui_stream.cpp` | `testTuiStreamEventSource` 尾部的"老数据兼容"段: 断言解析后是 User、不折叠、`source` 原样保留 |
| `agent/test/core/test_wire_schema.cpp` | sync 示例改用 `Role::Event` + `source` (注释写明角色为 event、默认折叠) |
| `agent/schema/wire-schema.json`、`docs/zh-cn/design/wire-protocol-fields.md` | `AGENTXX_UPDATE_WIRE_SCHEMA=1` 重新生成: sync 的 `messages` 示例值改成事件消息, 字段表补 `collapsed:boolean` |
| `docs/zh-cn/design/tui.md` | §2.17 兼容条目改为"只认 `Role::Event`, user + source 按普通用户消息展示"; §2.18 折叠预览写明"与正文同色, 不叠加 `theme.dim()`" |
| `AGENTS.md` | 常见问题新增: 改 `agent/lib/include` 的头之后要刷新安装前缀, 否则内层 test/client 构建仍用旧头 |

验证:

```
# 安装前缀的头要跟着更新 (agentxx_lib_repo 的 install 步骤)
cmake --build <build>/agentxx_lib_repo-prefix/src/agentxx_lib_repo-build --config Debug --target agentxx_static
cmake --install <build>/agentxx_lib_repo-prefix/src/agentxx_lib_repo-build --config Debug
# 再重编 test / client
```

- `tui_stream tui_scroll wire_schema wire_roundtrip view_replica event_delivery event_bridge
  session_persistence` → `passed=3777 failed=0`
- `tui_tool_header tui_interrupt tui_input tui_widget tui_lazy_view ui_items ui_snapshot
  interrupt_ui` → `passed=5118 failed=0`
- 两个 exe 已复制到 `agent/build/windows-debug/exec/`

坑: 先前 `tui_stream` 那两条失败是**编译用了旧的安装前缀头**造成的假失败 (源码里兼容代码已删),
不是测试写错。定位方法: 看 `agent/build/windows-debug/agentxx-project-install/include/...`
的修改时间是否落后于源码。
