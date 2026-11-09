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
| 全量 `agentxx_test --log-level warn` | 42686 passed / 5 failed，失败全在 `boundaries`（接口表台账与 `third_party/cxx_pluginxx` 头文件不一致，规则 7/8/9/10/15），本次改动未触碰接口表与 `boundaries` 用例 |
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

## 5. 遗留与注意

- Windows 调试版测试必须从 `{build}/exec/` 目录运行（那里有 `clang_rt.asan_dynamic-x86_64.dll`）；
  直接跑内层 `agentxx_test_repo-build/Debug/agentxx_test.exe` 会因缺 DLL 卡住（无输出、不退出）。
  验证新测试时用 `Copy-Item` 把内层产物覆盖到 `exec/` 再运行。
- 改了 `agent/lib/include` 下的头后，内层 client/test 用的是安装前缀里的旧头，
  必须先 `cmake --install <lib-build>` 再编它们（本次踩过一次：`event_hub` golden 断言
  比对的是旧 `appendArray` 结果的输出）。
- `boundaries` 的 5 项失败属工作区既有状态（子模块 `cxx_pluginxx` 已带 `capability_info` /
  `list_capabilities` 等新成员，`boundaries` 台账还没同步），与本次改动无关。
