# TUI 消息列表渲染重构实施方案 (内存 / 性能)

> 范围: 阶段 A (宿主侧每帧成本) + 阶段 B (markdown/ftxui 渲染基元) + 阶段 C (锚点式视口模型),
> 外加 "User 消息按纯文本渲染" 与 "渲染性能测试模块"。
> 相关代码: `agent/client/src/io/tui/{lazy_scrollable,components/message_list,markdown_block}.cpp`,
> `agent/third_party/markdown_ftxui/`, `agent/third_party/ftxui/`。

---

## 0. 结论摘要 (先说做什么)

| 问题 | 现状 (实测) | 方案 |
|---|---|---|
| 渲染树内存放大 | 纯段落 ×60、富 markdown ×112 (FTXUI `Text` 每字符一个 `std::string`) | B1 紧凑文本节点 (单 `std::string` + 行偏移) |
| 节点数爆炸 | 320 节点/条 (1.4KB 消息), 253~467 B/节点, 每词/每行一个节点 | B2 自绘折行叶节点 (段落/代码块) |
| 每帧可见集重排 | 30 条消息 1.28~1.6 ms/帧 (box 随流式增长上移) | B2 折行结果按宽度缓存 → 重排 ≈ 0 |
| 预计算高度 | 首屏/宽度变化对**全部**消息跑估算; Tool 消息估算会 `measureItems` **真渲染**, Interrupt 会 `layoutForm` | C 取消"为全列表准备高度"; 估算降级为 O(1) 粗略值, 仅服务滚动条 |
| 每帧 O(n) 扫描 | `itemKey` 全量 (含插件缓存加锁查字符串)、高度求和、坐标数组全量重建、decor 可见性全量 | A/C 只处理窗口内条目 (O(可见)) |
| 前插校正 | `notifyPrepended` + `applyPrependAnchorCorrection` + `kEstimateSlack` + `corrected` 反复兜 | C 锚点模型下前插零校正, 上述机制整体删除 |
| 流式内存 | 增量渲染器永久持有全部稳定块元素 | A6 稳定块按预算 LRU 淘汰 |
| User 消息 | 走 FTXUI `paragraph` (每词一节点) | 用纯文本折行叶节点 (不解析 markdown) |

预期目标 (对照第 1 节基线):

- 一屏 30 条消息渲染树内存: **4.5 MB → 0.5~1.0 MB**;
- 每帧可见集重排: **1.3~1.6 ms → < 0.2 ms**;
- 首屏 / 终端 resize 的估算开销: **O(全部消息 × markdown 语义估算, 含真渲染) → O(1)/条 粗略值 + 窗口内实测**;
- 每帧遍历复杂度: **O(历史长度) → O(可见条目)**;
- 单条消息重建 (滚动回视口): **0.45 ms → 预计 0.1 ms 级**。

---

## 1. 基线实测 (实施前后对比用)

测量环境: Windows x86_64, MSVC `/O2`, 宽 100 列; 微基准源码与产物见本方案末尾"附录 A"。

| 场景 | 渲染树 | 构建 | 测量(迭代布局) | 重复布局 (box 变动) | 绘制 (40 行) |
|---|---|---|---|---|---|
| `ftxui::paragraph` 2.6 KB | 158,640 B (×60.5) | 0.36 ms | 0.24 ms | 0.070 ms | 0.057 ms |
| 逐词 flexbox (带样式段落) | 157,391 B (×60.0) | 0.21 ms | 0.14 ms | 0.061 ms | — |
| markdown 文档 1.4 KB | 157,166 B (×112) | 0.30 ms (解析另 0.14 ms) | 0.073 ms → 43 行 | 0.022 / 0.025 ms | 0.036 ms |
| 整屏 30 条 markdown | **4.48 MB**, 9600 节点 (320/条) | — | — | **1.28 ms/帧** | ~1.1 ms/帧 |
| 原型: 紧凑文本节点 | ×57.9 (−48%) | 持平 | 持平 | 持平 | 持平 |
| 原型: 紧凑文本 + 自绘折行段落 | ×54 (纯段落场景 ×60 → ×1.1) | 持平 | 持平 | **3.24 ms → ≈0** (纯段落) | 持平 |

真实 TUI 基准 (`agentxx_benchmark resource_real_tui`, 708 条消息 / 1042 KB 文本):
平均帧 **0.143~0.165 ms** (仅组件树构建口径), 客户端 RSS 22.8 MB。

---

## 2. 设计总览

```
             渲染基元 (阶段 B)                    视口与调度 (阶段 A/C)
  ┌──────────────────────────────┐      ┌──────────────────────────────────┐
  │ ftxui::Text  (紧凑存储)       │      │ LazyScrollable (锚点式)           │
  │   └ markdown::FlowText        │◀────▶│   · anchorIndex_ + anchorRow_     │
  │        (折行缓存 + 行内样式)   │      │   · 底部优先窗口发现 (O(可见))     │
  │   └ markdown::FlowCodeBlock   │      │   · 实测优先, 粗略高度仅算滚动条   │
  │   └ (表格沿用现有布局实现)     │      │   · 每帧只做窗口内 key 比较        │
  └──────────────────────────────┘      │   · 前插 = 锚点平移 (零校正)       │
                                        └──────────────────────────────────┘
                        ▲                                    ▲
                        │                                    │
              message_list (构建/缓存/命中)          TUI 状态 (append-only 消息)
```

三条不变量 (实施中必须始终成立):

1. **定位只依赖实测高度**: 视口内每个条目的位置只由"它自己与它上方已实测条目的高度"决定;
   估算值只影响滚动条长度, 不影响任何渲染位置。
2. **测量即渲染**: 条目高度从它自己的布局结果得来 (自绘节点一次布局即得准确高度),
   不存在"第二套行数判定"。
3. **每帧成本与历史长度无关**: 每帧只遍历"窗口 + 预取带"内条目, 其余条目零成本。

---

## 3. 详细设计

### 3.1 B1 紧凑文本节点 (改 `agent/third_party/ftxui/src/ftxui/dom/text.cpp`)

现状: `class Text` 把文本拆成 `std::vector<std::string> glyphs_` (每字素 32 B) + `lines_offsets_`,
这是渲染树 ×30~×60 的主因。

改为:

- 成员: `std::string text_`(原文) + `std::vector<int> line_starts_`(各行起始字节偏移, 末尾哨兵 = size+1) + `int max_cols_` + 选择态 (`selection_first_line_` / `selection_rows_`)。
- 构造: 一次线性扫描 (`EatCodePoint`), 统计每行列数 (控制字符跳过、组合字符并入前一列、全角占 2 列)、记录行起点。
- `Render`: 按行解码 UTF-8 直写 `screen.CellAt(...)`; 全角写占位空串; 组合字符追加到前一格; 选择反色按 `selection_rows_` 判定。
- `Select`: 语义与原实现一致 (行区间 + 每行列区间 + `AddPart` 文本), 只把"字素下标"换成"字节偏移"(新增 `colToByte` 辅助)。
- `requirement_.min_x/min_y` 口径不变 (每行最大列数 / 行数)。
- `VText` 不动。

影响面: `ftxui::text()` 的所有使用者 (含 client 头部/预览文本) 自动受益; 无需改接口。
注意: ftxui 是 ExternalProject 构建, 改源码后需删除 `agent/build/<cfg>/ftxui_repo-prefix` 让其重编 (方案第 8 节)。

### 3.2 B2 自绘折行叶节点 (改 `agent/third_party/markdown_ftxui/`)

新增 `markdown/include/markdown/flow.hpp` + `markdown/src/flow.cpp`:

```cpp
namespace markdown {

/// 行内样式 (自绘节点逐单元格应用; 与 ftxui::Decorator 等价的表现层描述)
struct CellStyle {
    ftxui::Color fg = ftxui::Color::Default;
    ftxui::Color bg = ftxui::Color::Default;
    bool bold = false, dim = false, italic = false, underline = false;
};

/// 一段带样式的文本
struct Span {
    std::string text;      // 不含换行 (换行由 FlowText 的分段处理)
    CellStyle   style;
    int         link = -1; // >= 0: 链接序号 (点击区域登记用)
};

/// 自绘折行段落: 1 个节点承载整段文本
/// - 按 SetBox 得到的实际宽度折行, 结果按宽度缓存 (同宽度重排直接复用)
/// - ComputeRequirement 报告上次折行高度; SetBox 折行后高度变化则请求再迭代一次
///   (FTXUI 的标准 need_iteration 协议, 与 Flexbox 一致)
class FlowText : public ftxui::Node { ... };

/// 自绘代码块: 语言标签行 + 内边距 + 逐行折行 + 背景填充 (1 个节点)
class FlowCodeBlock : public ftxui::Node { ... };
}
```

要点:

1. **折行规则**: 以空格为词边界折行 (与现 `ftxui::paragraph` 一致); 超宽单词按列宽硬拆 (现行为是裁掉尾部,
   属允许的视觉变化, 且对 URL/路径更友好); `\n` 硬换行保留。
2. **折行缓存**: `cached_width_` + `lines_`(每行 `[字节起点, 字节终点, 样式片段])`;宽度不变时
   `SetBox` 只做盒更新, 重排成本 ≈ 0。这是"每帧重排 1.3 ms → ≈0"的来源。
3. **行内样式**: 逐单元格写 `cell.foreground_color / background_color / bold / dim / italic / underlined`;
   外层装饰器 (`color(theme.assistantColor)` 等) 先于子节点写入, 因此子节点样式优先, 语义正确。
4. **选择/复制**: 实现 `Select()` (行区间 + 列区间 + `AddPart`), 与 `ftxui::Text` 同语义。
5. **链接点击区域**: `DomBuilder::register_link` 目前给"每个词元素"套 `reflect`;
   改为在 FlowText 折行时把每个链接的可见区段 (行, 列区间) 换算为 `Box` 写入
   `LinkTarget::boxes`。为保持 `_flat_boxes` 中 `Box const*` 指针稳定,
   构建期对每个链接 `boxes.reserve(上限)` (上限 = 该链接的词数), 布局期只 `resize`/写入不超容量。
6. **样式来源**: `Theme` 增加显式单元格样式字段, 供自绘节点使用 (块级仍沿用现有 Decorator):
   `link_style` / `code_inline_style` / `table_header_style` / `table_border_style`;
   client 的 `TUITheme::markdownTheme` 同时填 Decorator (兼容 viewer/editor) 与 `*_style`。

`dom_builder.cpp` 的改造点:

| 现实现 | 改为 |
|---|---|
| `build_wrapping_container` 纯文本快速路径 → `ftxui::paragraph` | `FlowText` (单 Span) |
| `collect_inline_words` + `words_to_element` (每词一元素 + flexbox) | `FlowText` (多 Span, 带样式) |
| `build_code_block` (每行 `hbox{text, filler}` + 上下留白 + 语言行) | `FlowCodeBlock` |
| `build_heading` / `build_list_item` / `build_blockquote` | 结构保留 (外层 Decorator/前缀), 内容改用 FlowText |
| `build_table` | 本期保留 (已自绘换行与边框, 只是把"每行 hbox 组装"留在原地), 见 §3.9 后续项 |
| HardBreak 分段、嵌套列表不变 | 不变 |

预期: 节点数 320/条 → 60~100/条; 富 markdown 放大 ×112 → 预计 ×15~30 (紧凑文本 −48% 之上再降节点开销)。

### 3.3 User 消息按纯文本渲染

- `message_list.cpp` 的 `Role::User` 分支: 正文由 `ftxui::paragraph(msg.text)` 改为
  `markdown::FlowText`(纯文本, 不解析 markdown, 保留 `\n` 硬换行, 空格/长词按 §3.2 规则折行);
  外层颜色仍用 `theme.userColor`。
- 高度不再依赖估算: 布局即测量 (§3.4)。
- 顺带把 User 正文的 `sourceBytes` 上报口径改为实测节点/字节口径 (§3.8)。

### 3.4 C 锚点式视口模型 (`lazy_scrollable.{h,cpp}` 重构)

**状态**: `anchorIndex_` (视口顶行所在条目) + `anchorRow_` (视口顶行在该条目内的行偏移, `0 <= anchorRow_ < height(anchor)`)
+ `stickToBottom_`; 派生出 `rowsAboveAnchor_`(锚点以上条目高度和) 与 `totalHeight_`(全部条目高度和)。

**每帧布局流程** (`prepareLayout`):

1. 同步条目数组 (增删/前插, 见下) 与宽度变化处理 (宽度变化 → 清缓存 + 粗略高度全部重算, 与今天一致但**不触发任何语义估算/渲染**)。
2. 求窗口:
   - `stickToBottom_`: 从**尾部往前**累计高度 (未测量的先测) 直到 ≥ 视口高度 → 得窗口起点 `anchorIndex_` 与 `anchorRow_ = Σ窗口高度 - 视口高度`; 内容不足一屏时退化为顶部对齐 (`anchorIndex_ = 0`, `anchorRow_ = 0`)。
   - 非 sticky: 以 `(anchorIndex_, anchorRow_)` 为起点向**后**填充视口 (O(可见)); 视口上方条目完全不参与计算。
3. 窗口内条目: 测量 (未测的做一次迭代布局, 得到准确高度) → 以 `anchorIndex_/anchorRow_` 为基准顺次定位到屏幕坐标。
4. 缓存: 构建/复用的 Element 进 LRU (同今天的条数 + 字节双预算), 窗口外条目按预算淘汰。
5. 每帧只对**窗口 ± 预取带**内条目做 key 比较 (key 变化 → 失效重测)。

**滚动** (`OnEvent` 直接实现, 不再走 `handleWheelScroll` 的"offset-from-top"口径):

- 上滚 1 行: `anchorRow_ > 0 ? --anchorRow_ : 跨到上一条目 (先测量它, `anchorRow_ = h-1`)`; 到首条且 `anchorRow_ == 0` 停住。
- 下滚 1 行: `anchorRow_ + 1 < h(anchor) ? ++anchorRow_ : 跨到下一条目`; 到达末尾 → `stickToBottom_ = true`。
- 解除吸附: 上滚即 `stickToBottom_ = false` (与原行为一致)。

**前插 (历史分页)**: `notifyPrepended(count)` = 并行数组头插 + `anchorIndex_ += count` + `rowsAboveAnchor_` 用新增区粗略高度累加;
**不再需要** `pendingPrepend_` / `applyPrependAnchorCorrection` (锚点未动, 视口内容天然稳定)。

**滚动条**: 用 `totalHeight_`(实测 + 粗略) 计算, 语义与原一致。

**对外只读接口** (为兼容测试与既有调用点保留):

- `scrollOffset()` = `rowsAboveAnchor_ + anchorRow_` (锚点以上全部已实测时为精确值, 与旧口径一致);
- `totalHeight()` / `viewportHeight()` / `contentWidth()` / `visibleBoxes()` 语义不变;
- **删除**: `estimateHeight` 回调 (见 §3.5)、`pendingPrepend_` 相关内部状态。

### 3.5 A 估算与 key 策略 (`lazy_scrollable.cpp` + `message_list.cpp`)

- 新回调 `quickHeight(index, width)`: **O(1)** 粗略行数 (message_list 用 `字节数/宽度 × 系数 + 2`),
  仅用于: (a) 未测量条目的 `totalHeight_` 贡献; (b) 首次布局前的占位。**不解析文本、不渲染、不加锁**。
- 删除 `estimateHeight` 语义估算链路对滚动定位的参与:
  - `MessageListComponent::estimateHeight` 中的 `queryToolRender(...)` + `measureItems(...)`、
    `interruptView_.estimate(...)`(会 `layoutForm` 真渲染) 一律不再参与高度计算;
  - `estimateMarkdownLines` 保留为诊断/测试工具 (不再被滚动路径调用), 其"与渲染同源"的职责由
    "布局即测量" 取代。
- key 只在窗口 ± 预取带内计算; 窗口外条目的失效延迟到它重新进入窗口时 (锚点模型下不会造成视觉跳动,
  只会让滚动条长度短暂偏旧)。

### 3.6 A 流式增量 (稳定块可淘汰)

`markdown::IncrementalRenderer` 增加"已构建稳定块"预算:

- `setElementBudget(size_t maxBlocks, size_t maxBytes)` + 访问时 LRU 标记;
- 超预算时释放最久未访问的 `element/builder` (源码文本保留, 需要时按块重解析重建 —— 单块解析成本与块大小同阶);
- `MessageListComponent` 每帧把窗口内稳定块序号告知渲染器 (`touchStable(i)` 已有访问即标记), 其余按预算淘汰;
- 流结束/切流时整体释放 (现状已做)。

### 3.7 A 工具渲染请求去重前置

`queryToolRender` 现在先构造 `ClientToolRenderRequest`(拷贝 `argsJson` / `resultText` 大字符串) 再判重。
改为: 先用 `string_view` 计算输入特征哈希 + 查缓存/查在途表, 只有确实需要投递时才拷贝;
`ClientToolRenderCache` 增加 `peek(key, inputHash)` 与 `hashInputs(...)` (静态, 接受 `string_view`)。

### 3.8 B3 缓存预算与 `sourceBytes` 标定

- `buildMessageItem` 的 `sourceBytes` 由 `源字节 × 64` 改为按**节点数 + 文本字节**折算
  (自绘节点后每条消息的节点数骤降, ×64 已明显偏离);
  自绘节点在构造时累加一个模块级"本次构建节点/字节计数", 构建完成后取差值上报 (精确且廉价)。
- 预算默认值随实测下调 (`maxBytes` 4 MiB → 2 MiB, 条数 64 不变), 保证"预算 = 真实驻留"。

### 3.9 本期不做 (留存后续)

- 表格自绘节点 (`build_table` 的每行 hbox 组装): 现有实现已自算换行, 节点数是"每行常数个", 收益小于段落/代码块; 待 B2 稳定后可增量替换。
- 语法高亮 (`highlight.hpp`) 与 mermaid 状态图: 不在本次范围 (其内部结构由各自实现决定)。

---

## 4. 接口与文件变更清单

| 文件 | 变更 |
|---|---|
| `third_party/ftxui/src/ftxui/dom/text.cpp` | `Text` 紧凑存储 + 直写渲染 + 选择态按字节偏移 (B1) |
| `third_party/markdown_ftxui/markdown/include/markdown/flow.hpp` (新) | `CellStyle` / `Span` / `FlowText` / `FlowCodeBlock` 声明 |
| `third_party/markdown_ftxui/markdown/src/flow.cpp` (新) | 折行 + 缓存 + 渲染 + 选择 + 链接区段 |
| `third_party/markdown_ftxui/markdown/include/markdown/theme.hpp` | 新增 `link_style` / `code_inline_style` / `table_*_style` 单元格样式字段 |
| `third_party/markdown_ftxui/markdown/src/dom_builder.cpp` | 段落/行内/代码块改用自绘节点; 链接区段登记适配 |
| `third_party/markdown_ftxui/markdown/include/markdown/incremental.hpp` + `src/incremental.cpp` | 稳定块元素预算与淘汰 (A6) |
| `client/include/agentxx-client/io/tui/lazy_scrollable.h` + `src/io/tui/lazy_scrollable.cpp` | 锚点模型 + 窗口发现 + 增量总高 + quickHeight + 窗口内 key (A/C) |
| `client/src/io/tui/components/message_list.cpp` | 估算链路瘦身、User 纯文本、sourceBytes 标定、流式稳定块 touch |
| `client/src/io/tui/markdown_block.{h,cpp}` | `estimateMarkdownLines` 降级为诊断工具; 新增 `renderPlainText` 帮助函数 |
| `client/include/agentxx-client/io/tui/tui_theme.h` + `src/io/tui/tui_theme.cpp` | 填充新的 markdown 单元格样式字段 |
| `agent/lib/src/plugins/client_plugin_manager.cpp` | `peek` / `hashInputs` (A7, 与 client 一起改) |
| `agent/test/client/test_tui_scroll.cpp` 等 | 适配新视口模型 + 新增断言 |
| `agent/benchmark/bench_render.cpp` (新) + `benchmark_main.cpp` | 渲染性能基准模块 |
| `docs/zh-cn/design/tui.md` (§3.3) 等 | 同步设计说明 |

---

## 5. 测试与基准

### 5.1 单元测试 (agentxx_test)

- 既有: `tui_scroll` / `tui_stream` / `tui_widget` / `tui_ui_items` / `tui_tool_header` / `tui_context_overlay` / `markdown_block` / `mermaid_state` 全部通过 (必要时按新模型更新断言)。
- 新增 `tui_lazy_view` (或并入 `tui_scroll`):
  1. 锚点模型: 上滚 1 行 / 下滚 1 行 / 跨条目 / 到顶到底的边界;
  2. 窗口成本: 在 2000 条消息下, 单帧布局只构建/测量窗口内条目 (以 `buildItem` 调用计数断言, 与历史长度无关);
  3. 前插稳定: 非 sticky 下前插 N 条, `scrollOffset()` 与屏幕内容均平移而不跳动 (旧 API `onHistoryPrepended` 语义保持);
  4. 精确性: 全部条目进入过视口后 `totalHeight()` == 各条目实测高度和;
  5. 估算不影响定位: 人为给出错误的 `quickHeight` 时, 视口内内容仍正确。
- 新增 `markdown_flow`:
  1. `FlowText` 折行行数与渲染高度一致 (同一宽度重复布局结果稳定);
  2. 宽度变化后重新折行且高度更新;
  3. 行内样式 (粗体/行内代码/链接) 落到正确单元格 (离屏断言配色);
  4. 长单词硬拆、宽字符 2 列、组合字符合并;
  5. 选择区间取文本正确 (与 `ftxui::Text` 行为对齐)。
- 新增 `ftxui_text` (或并入 `markdown_flow`): 紧凑 `Text` 节点与旧行为等价 (行数/列宽/宽字符/选择) + 内存断言 (构造 1KB ASCII 文本的分配字节数上界)。

### 5.2 性能基准 (`agentxx_benchmark render_tui`)

新模块 `render_tui` (不依赖 LLM, 纯渲染路径), 覆盖:

1. 单条消息: 解析 + 构建耗时与渲染树字节 (按进程堆差值), 覆盖用户/助手/工具/思考四类文本;
2. 布局: 首次测量、同宽度重复布局、box 每帧上移 (流式) 三种口径;
3. 整屏: N 条消息的构建总字节、每帧重排耗时、绘制耗时、`Screen::ToString` 耗时;
4. 消息列表组件: 在 100/1000/5000 条消息下渲染一帧 (断言每帧耗时与消息数解耦) + 滚动一屏的成本;
5. 缓存命中率与重建次数统计 (通过组件暴露的计数, 仅在 `enableBenchmark` 打开时采集)。

结果进入既有报告体系 (`bench_<时间戳>.json/.md`), 可与基线对比。

### 5.3 端到端回归

- `agentxx_benchmark resource_real_tui` (真实 TUI 帧耗时/渲染字节/RSS);
- `agentxx_test memgrowth` (内存增长回归);
- 手工: 长会话滚动、流式输出、鼠标拖选复制、链接/装饰按钮点击、终端 resize。

---

## 6. 实施顺序与提交计划

| # | 提交内容 | 验证 |
|---|---|---|
| 1 | 本方案 `resource/history/tui-markdown/plan.md` | — |
| 2 | B1 紧凑文本节点 (ftxui) | 编译 + `markdown_block`/`tui_*` 测试 + 微基准对比 |
| 3 | B2a `FlowText` (纯文本) + User 纯文本 | 编译 + `markdown_flow` 新测试 + 渲染基准 |
| 4 | B2b `FlowText` 行内样式 + `FlowCodeBlock` | 同上 + 视觉抽查 |
| 5 | C/A 锚点视口模型 + 估算/key 瘦身 (lazy_scrollable + message_list) | `tui_scroll`/`tui_stream`/新增 `tui_lazy_view` |
| 6 | A6 流式稳定块淘汰 + A7 工具渲染去重 + B3 预算标定 | 编译 + `tui_stream`/`plugin_sdk` + 基准 |
| 7 | 性能测试模块 + 文档同步 | 基准跑通 + 文档更新 |

每个阶段独立可编译、可回退; 提交信息说明改动范围与实测对比。

---

## 7. 风险与回退

| 风险 | 应对 |
|---|---|
| 自绘节点与 FTXUI 迭代布局协议不一致 (高度抖动) | 严格按 `need_iteration` 协议; 单测断言"同宽度重复布局高度稳定"; 保留 `layoutAndMeasure` 迭代上限 |
| 选择/复制、链接点击、装饰命中在自绘节点上失效 | 单测覆盖 `Select`/`AddPart` 文本、链接区段 Box; `visibleBoxes()` 命中口径不变 |
| 锚点模型下 `scrollOffset()` 语义变化影响既有调用点 | 保留派生实现; 逐个调用点核对 (`maybeRequestMoreHistory` / 测试) |
| ftxui 改动影响面大 (组件层也用它渲染文本) | 仅改 `Text` 内部存储与渲染实现, 不改接口; 全量测试 + 手工界面抽查 |
| 视觉变化 (长词硬拆 / 代码块绘制细节) | 已知并接受; 抽查窄终端、宽字符、表格、mermaid 场景 |
| ExternalProject 缓存导致第三方改动不生效 | 改动后删除 `agent/build/<cfg>/{ftxui_repo-prefix,markdown_ftxui_repo-prefix}` 再构建 |

---

## 8. 验收标准

1. `agentxx_test` 全量通过 (含新增模块);
2. `agentxx_benchmark render_tui` 相对本方案第 1 节基线:
   - 一屏 30 条渲染树内存 ≤ 1.0 MB (基线 4.48 MB);
   - 每帧可见集重排 ≤ 0.2 ms (基线 1.28 ms);
   - 单条消息构建 ≤ 0.2 ms (基线 0.30 ms 构建 + 0.14 ms 解析) 或明确说明未达标原因;
3. `render_tui` 的"每帧耗时 vs 消息条数"曲线在 100/1000/5000 条下基本持平 (±20%);
4. `resource_real_tui` 客户端 RSS 不高于基线 + 0.5 MB, 帧耗时不超过基线;
5. 文档同步: `docs/zh-cn/design/tui.md` §3.3 与 §1.5、本目录 `work.md` 记录实测对比。

---

## 附录 A: 基线微基准 (可重跑)

路径: `%TEMP%\agentxx\sess-24d5f6d4e0fe0-1568-ef2e6cfe-0000\mdbench`

- `bench.cpp` / `bench.exe`: FTXUI 段落级 (paragraph / 逐词 flexbox / 自绘折行原型) 内存与布局计时;
- `bench2.cpp` / `bench2.exe`: 真实 markdown 管线 (cmark-gfm + DomBuilder) 基线;
- `benchB.exe`: 紧凑文本节点变体; `benchC.exe`: 紧凑文本 + 自绘折行段落变体;
- `build_ctext2.bat` / `build_ctext_flow2.bat` / `build2.bat`: 对应构建脚本 (MSVC + 项目内 cmark 静态库)。
