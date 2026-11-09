# 实施记录

## 进度

- [x] 1. `hub.h`：`contentLimitLength` / `contentText` / `renderEventValuesText` 声明
- [x] 2. `hub_text.cpp`：三段渲染 + 纯文本值渲染
- [x] 3. `hub.cpp`：`flushWait` / `contentText` 改造，删 `rawValueText` / `valueText`
- [x] 4. `config.*` / `base_agent.cpp`：阈值合成与注入
- [x] 5. `async_tools.cpp`：`agentxx_async_await` 同步换格式
- [x] 6. TUI：i18n 键、配色、` · ` 分隔符、`[Content]` 预览
- [x] 7. 测试更新与新增
- [x] 8. 文档（events.md / tui.md / config_loader 注释 / AGENTS.md）
- [x] 9. 编译 + 运行相关测试模块 + 全量回归

## 1. libagentxx（提交 `8de6bc5d`）

改动文件：

| 文件 | 改动 |
|---|---|
| `agent/lib/include/agentxx/event/hub.h` | `Config::valueBytes` → `Config::contentLimitLength`（utf8 字符数, 默认 2 K）；`valueText` → `contentText(sessionId, values)`；新增 `renderEventValuesText`；`renderEventText` 改收 `contentText` |
| `agent/lib/src/event/hub_text.cpp` | 三段渲染 + 纯文本值渲染（对象 `键: 值`、数组 `- 值`、多行字符串缩进、多值 `#n`、空值 `(no value)`、8 层深度上限退回 JSON） |
| `agent/lib/src/event/hub.cpp` | `flushWait` 先取整段内容再渲染；`contentText` 内按阈值调 `offloadShareStoreIfOversized`；删 `rawValueText` / `valueText` |
| `agent/lib/include/agentxx/agent/config.h` + `src/agent/config.cpp` | `eventValueMaxBytes` 注释改为"内容段"；新增 `eventContentLimitLength()`（与 `toolcallSummaryLimitOutputLength` 取小, 0 = 该侧不限制） |
| `agent/lib/src/agent/base_agent.cpp` | 填 `contentLimitLength`，日志打印它 |
| `agent/lib/src/tools/async_tools.cpp` | `agentxx_async_await` 用 `contentText` + 新 `renderEventText` |

验证：`agentxx_test event_hub event_delivery event_tools event_subagent async_tools plugin_event` 全通过；
`event_hub` 新增两个用例（格式 golden、长内容转 share store）。

过程中修掉的两个渲染细节：

- 数组里放对象时最初渲染成 `-\n    n: 1`（三行），改成把嵌套首行接在 `- ` 之后（`- n: 1`），
  与 YAML 写法一致、更省行。
- 长内容不能逐值卸载：默认阈值（2 K 字符）下逐值 32 KB 的卸载结果仍超阈值，会卸载两次并
  得到两个 share store id，因此改为整段只卸载一次。

## 2. TUI（提交 `4b06220a`）

| 文件 | 改动 |
|---|---|
| `agent/client/src/io/tui/tui_i18n.cpp` | 新增 `msg.eventPrefix`（` [Event] ` / ` [事件] `），删 `msg.eventSource` |
| `agent/client/src/io/tui/components/message_list.cpp` | `Role::Event` 分支重写：标记与前缀 `accentColor`，折叠态名字/分隔符/预览 `dim`；分隔符 ` · `；新增 `eventContentPreview`（取 `[Content]` 段首行，跳过 `#n`，找不到则退回正文首行） |
| `agent/test/client/test_tui_settings.cpp` | 补 `msg.eventPrefix` 的中英文断言 |

验证：`tui_scroll tui_stream tui_settings` 全通过；`tui_scroll` 的事件场景新增逐格断言
（标记高亮不 dim、名字 `accentColor + dim`、` · ` 列距 3 列、预览 dim、展开后名字不 dim）。

测试定位手段：`ScrollFixture::renderScreen()` 返回 `ftxui::Screen`，配合 `screenRowText`
（一格一字，宽字符只留首字节）用 ASCII 事件名定位行后再取列，逐格看 `foreground_color` / `dim`。

## 3. 测试与文档

- 测试改动：`test_event_hub`（格式 golden + 长内容转存 + 相关断言）、`test_event_delivery`、
  `test_plugin_event`（`step: 2` 取代 `"step":2`）、`test_tui_scroll`、`test_tui_stream`、
  `test_tui_settings`。
- 文档改动：`docs/zh-cn/design/events.md` §5（三段文本 + 压缩规则 + 附录表）、
  `docs/zh-cn/design/tui.md` §2.17、`AGENTS.md` 事件段摘要、
  `agent/client/{include,src}` 的 `event.value_max_bytes` 注释。

## 4. 验证清单

| 项 | 结果 |
|---|---|
| `agentxx_test --log-level warn event_hub event_delivery event_tools event_subagent async_tools plugin_event` | 通过（`event_hub` 265 条含新增用例） |
| `agentxx_test --log-level warn tui_scroll tui_stream tui_settings config_keys` | 通过 |
| 全量 `agentxx_test --log-level warn` | 42699 passed / 5 failed，5 项全在 `boundaries`：规则 7/8/9/10（接口表数量/名集合/四件套/成员顺序各 4 条）与规则 15（用词 23 条，命中都在 `third_party/cxx_pluginxx` 的头注释里）。属工作区既有状态（子模块刚升到带 `capability_info` / `list_capabilities` 的版本，台账与用词未同步），本次未碰接口表与 `boundaries` 用例 |
| 构建 | 内层 lib（`agentxx_static`）+ install + 内层 client（`agentxx`）+ 内层 test（`agentxx_test`）均无错误 |

## 4.1 收尾补充

- 转存提示的首行前缀抽成公用常量 `agentxx::util::kOffloadHintPrefix`
  (`share_store_offload.h`), 提示文本本身仍由 `offloadShareStoreIfOversized` 排版
  (字节级输出未变, `share_store` 的逐字断言仍通过); 界面侧据此把提示行当成"非内容"跳过。
- 新增 `tui_scroll` 用例 `testEventOffloadPreview`: 内容段第一行是转存提示时, 折叠预览
  显示提示后面那行真实内容。

## 4.2 修正：`[Content]` 不再改写自定义 JSON

第一版把值改成了自己造的 `键: 值` / `- 项` / `缩进` 文本，问题：

- 值可能是**别人自定义的 JSON**（插件发布的值、模型经 `agentxx_async_emit` 发的值）：
  `"step":1` 被改成 `step: 1`、字符串 `"1"` 与数字 `1` 再也分不清、数组/对象结构被重排；
- 嵌套结构也可能是有意义的 JSON（工具参数/结果），重排就是改数据。

改后的规则（`hub_text.cpp`）：

- 字符串值 → 原文（它本身就是文本，没有 JSON 结构可保）
- 对象/数组 → **紧凑 JSON 原样**（`dump()`，键序/转义/数字格式与发布时一致）
- 多个值 → 整体 JSON 数组（原来自造的 `#1`/`#2` 分块删掉）
- **唯一例外**：`eventTextPayloadField(eventName)` 非空的框架事件
  （`agentxx.agent.async.*`）把 `content` 字段（工具输出 / 子代理回答，本身可能是
  JSON 文本）按原文单独给出（`content:` 一行 + 原文），其余字段仍是同一个 JSON；
  正文字段不是字符串时不动值

同步改动的测试：`event_hub` 格式 golden 重写（含“自定义 JSON 原样”“异步正文字段展开”
“声明字段非字符串不动值”三块）、`plugin_event`（`{"step":1}` 原样）、timer/bus 断言改回 JSON 形式、
`tui_scroll` 事件正文样例改用 JSON，预览断言随之调整。

## 4.3 调整：无值用 `[No Content]`，事件 inject 不再前置来源标记

用户提出两点：无值时把 `[Content]` 段标记改成 `[No Content]`；事件输入的来源标记
（`[event:<名字>]`）与 `[Info]` 段重复，删掉。

### `[No Content]`

| 文件 | 改动 |
|---|---|
| `agent/lib/include/agentxx/event/text_sections.h`（新） | `kEventContentSection = "[Content]"` / `kEventNoContentSection = "[No Content]"` |
| `agent/lib/include/agentxx/event/hub.h` | include 新头（原来这里定义的 `[Content]` 常量删掉）；`renderEventText` 文档改为“无值时写 `[No Content]`” |
| `agent/lib/src/event/hub_text.cpp` | 末尾按 `contentText.empty()` 选段标记，不再写 `(no value)` 占位 |
| `agent/client/src/io/tui/components/message_list.cpp` | 预览用两个常量；正文含 `[No Content]` 时返回空预览（不画 ` · `） |

常量单独放一个头，是为了让展示端不必为一个字符串包含整个 `hub.h`（`hub.h` 带 asio）。

### 事件 inject 的来源标记

`drainPendingSessionInputs` 只对**非事件来源**加 `[<来源>]` 前缀
（判定用 `agentxx::agent::eventNameOfInputSource(input.source).empty()`）。

原本的目的（查过 `7ffa773d` 与 `resource/history/compare-agent/work.md` 的 LOOP-2）：
`inject` 的内容只进本次请求、不进权威上下文，模型在历史里找不到这段内容的来历，
所以把来源写在正文前。事件投递的正文首行就是 `[Event] <名字>`、`[Info]` 里还写着
`mode`/`wait`/`status`，来源已经说明过，再叠一层只是重复。非事件来源仍保留前缀。

### 测试与文档

- `test_event_hub`：格式 golden 与 `test_hub_watch_ended_event` 的结尾断言改成 `[No Content]`。
- `test_tui_scroll`：`testEventOffloadPreview` 增加一条 `[No Content]` 消息，断言只有折叠头、
  没有 `quiet.evt · ` 预览。
- `test_input_delivery`：新增 `test_event_inject_has_no_source_prefix` —— 直接登记一条
  `source = event:chat.message` 的 inject，断言请求里有 `[Event] chat.message`、
  没有 `[event:chat.message]`，且权威上下文里仍没有这段内容（inject 语义未变）。
- 文档：`events.md`（`[No Content]` + inject 来源标记说明）、`tui.md` §2.17（`[No Content]`
  不画预览）、`AGENTS.md` 事件段摘要。

验证：`event_hub event_delivery event_tools event_subagent async_tools plugin_event
share_store input_delivery tui_scroll tui_stream tui_settings` 全通过（2645 passed / 0 failed）。

两处收尾：

- `hub.h` 里旧 `[Content]` 常量的注释残了一半（注释下面没有声明）→ 删掉，常量统一由
  `text_sections.h` 说明；`renderEventValuesText` 的 `[values]` 说明从“补占位”改成“写成 `[No Content]`”。
- TUI 判定从 `text.find("[No Content]")` 改成 `text.ends_with(...)`：无值时它就是最后一段，
  用 `find` 会被值里恰好出现的同名字样误伤（改为结尾判定后重编 client/test 并复跑了上面那批模块）。

## 5. 遗留与注意

- Windows 调试版测试必须从 `{build}/exec/` 目录运行（那里有 `clang_rt.asan_dynamic-x86_64.dll`）；
  直接跑内层 `agentxx_test_repo-build/Debug/agentxx_test.exe` 会因缺 DLL 卡住（无输出、不退出）。
  验证新测试时用 `Copy-Item` 把内层产物覆盖到 `exec/` 再运行。
- 改了 `agent/lib/include` 下的头后，内层 client/test 用的是安装前缀里的旧头，
  必须先 `cmake --install <lib-build>` 再编它们（本次踩过一次：`event_hub` golden 断言
  比对的是旧 `appendArray` 结果的输出）。
- `boundaries` 的 5 项失败属工作区既有状态（子模块 `cxx_pluginxx` 已带 `capability_info` /
  `list_capabilities` 等新成员，台账没同步；其头注释里还留着“埋点/宿主/应用侧”等停用词），
  与本次改动无关。
