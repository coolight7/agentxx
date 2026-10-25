- 难度: C
- 类型: bug修复
- 基于commit: 3aaa8038
- 时间: 2026-10-25
- 需求:
```md
TUI 里一部分 tool 消息的边距非常大, 前面的消息都正常, 唯独最近的消息开始有问题。
（与 fix-tui-console-codepage 同批排查: 控制台代码页/画面漂移问题先修完之后,
 边距问题仍在。）
```

## 现象

消息列表里 tool 消息之间出现整行空白, 且集中出现在会话末尾:

```
+ [工具] Read · [420, 465] agent/lib/src/ffi/ffi_runtime.cpp
                                      <- 空白
                                      <- 空白
+ [工具] Edit · [+22 -3] agent/lib/src/ffi/ffi_runtime.cpp
+ [思考] 1.3s Careful: `catchErrorAsync<bool>` ...
```

每条 tool 消息本身只有 1 行, 但相邻两条之间能空出 2 行, 看起来像"边距被撑大"。
把画面拖选复制成文本时, 这些行表现为"只含一个零宽字符的行"(`​`)。

## 根因

模型 (opencode-ds-flash) 在发起工具调用时**把 U+200B (零宽空格) 当成正文输出**,
`EventBridge` 只判断 `content.empty()` 就把它当成一条正常的 assistant 消息写进展示历史:

- 会话 `sess-364e215a8ea30` 的 1584 条展示消息里有 **19 条 `role=assistant`、正文只有
  一个 U+200B** 的消息 (`seq=1494, 1496, 1498, 1500, 1503, ...`, 从 `seq 1494` 起几乎
  每次工具调用前后各一条);
- 这种消息在消息列表里渲染成 **1 行看不见内容 + 1 行尾部空行 = 2 行纯空白**
  (子项间距统一约定: 每条消息自带一行尾部空行, 见 tui.md "子项间距的统一约定");
- 于是相邻两条 tool 消息之间的间距从 1 行变成 2~3 行 —— 这就是用户看到的"边距非常大"。
  "前面的消息正常" 是因为前段会话没有这类空消息。

## 排查过程 (先排除渲染层)

1. 逐条测量渲染高度: 把该会话 1584 条 `view_message` 全部导出, 逐条单独放进消息列表
   (97 列宽, 6 行视口迫使首条被实测), 读 `LazyScrollable::totalHeight()`:
   - 1021 条 tool 消息**全部 2 行**, 500 条 thinking 全部 2 行, 6 条 tip 全部 2 行;
   - assistant 44 条 2 行、5 条 3 行、4 条含 markdown 表格 61~66 行;
   - 结论: 单条高度都符合设计 (内容 + 1 行尾部空行), 多出来的空白只能来自"多出来的消息"。
2. 按角色统计正文: 512 条 tool 结果带 2 个尾部换行但渲染只留 1 行 (尾部空行被归一),
   内容里没有 3 行以上连续空行 —— 排除"tool 结果本身带大段空白"。
3. 扫不可见码点: 发现 19 条 assistant 消息正文只有一个 U+200B, 且都在会话末尾,
   与"唯独最近的消息有问题"完全吻合。
4. 现场抓屏 (AttachConsole + 读 `srWindow` 的屏幕缓冲) 也印证: 这些行复制出来是
   单个零宽字符, 而它在终端上占一格 (看起来就是一行空白)。

## 修改

判据函数 (共享实现):

- 新增 `agent/lib/include/agentxx/util/invisible_text.h`
  - `isInvisibleCodePoint(cp)` / `isVisuallyBlank(text)`: 逐码点判断"这段文本看起来是不是空的"。
    覆盖 ASCII/C1 控制符与空格、U+2000..U+200A 各种宽度空格、U+200B..U+200F 零宽与双向
    控制符、U+2028/U+2029、U+2060..U+2064、U+2066..U+206F、BOM、U+2800 盲文空白、
    U+3000 全角空格、韩文填充符、U+FFF9..U+FFFB 注释符。
  - 非法 UTF-8 字节按"有内容"处理: 宁可多显示一条, 也不要静默吞掉真实内容。

源头 (不再产生新数据):

- `agent/lib/src/event/event_stream.cpp` (`EventBridge::handleChannelWrite`):
  `content` 从"非空"改为"非空且视觉不空"才写展示历史的一条 assistant 消息。
  只影响展示历史, **不影响 LLM 上下文** (上下文由会话消息独立维护)。

读取 (老数据也不再出现):

- `agent/lib/src/agent/session_store.cpp`: `loadSession` 与 `loadViewMessagesAfter`
  都跳过视觉为空的消息 (新增 `isDisplayableViewMessage`)。
  - 思考消息例外: `durationMs > 0` (折叠头展示耗时) 或带加密思考/token 统计时保留;
  - 工具/中断消息恒保留 (头部与控件始终有可见内容);
  - `msgIdCounter` 兜底改用**解析成功的行数**而不是保留条数 —— 被过滤的消息也占过一个
    msg id, 用保留条数会让 id 计数倒退, 后续新消息 id 与库中已有 id 重复。

渲染 (兜底, 覆盖已加载进内存的消息):

- `agent/client/src/io/tui/components/message_list.cpp`: 新增 `isBlankMessage`,
  `buildMessageItem` 对视觉为空的消息不再追加尾部空行 (只占 1 行)。

## 测试

- 新模块 `invisible_text` (35 断言): 空白/零宽/BOM/全角空格判为空; 中英文/数字/标点/emoji
  判为非空; **零宽字符后跟可见字符时为非空**; 非法 UTF-8 字节为非空; 单码点边界取值。
- `session_persistence` (639 断言) 新增: 写入 ZWSP assistant / 只有空格的 think 后重新加载,
  两者都不出现; 带耗时的 think 保留; `loadViewMessagesAfter` 走同一过滤;
  `msgIdCounter` 不因过滤而倒退。
- `tui_scroll` (901 断言) 新增: 视觉空消息总高度 1 行 (普通消息 2 行; 只有耗时的 think 1 行)。
- 回归: `agent 198`、`jsonl_mode 69`、`tui_lazy_view 3851`、`client_plugins 714`、
  `boundaries 14`、`wire_roundtrip 237` 全绿。

## 后续注意

- 展示历史与 LLM 上下文是两套数据: 这类过滤只作用于展示层, 上下文里的 assistant 内容
  保持模型原样 (压缩/前缀命中等逻辑不受影响)。
- 如果将来需要"真的什么都不显示"而不是"只占 1 行", 要在客户端子项列表层面跳过消息;
  那会动到 `LazyScrollable` 的子项下标 (历史分页窗口、本地条数上限清理、命中区都按下标
  索引), 风险明显高于现在的两层过滤 + 一行兜底, 暂不做。
- 判据只认"看不见的码点", 不去猜 "模型想不想说话"; 空字符串正文本来就不写展示历史。
