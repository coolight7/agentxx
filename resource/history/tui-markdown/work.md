# TUI 消息列表渲染重构 实施记录 (work.md)

方案原文: [plan.md](plan.md) —— 本文件记录实施进度、已完成/待完成内容与注意事项。

## 进度总览

| # | 提交内容 | 状态 | 提交 |
|---|---|---|---|
| 1 | 方案 `plan.md` | 已完成 | `9931ff44` / `d4112e27` |
| 2 | B1 紧凑文本节点 (ftxui `Text`) | **已完成** | `ed99c480` |
| 3 | B2a `FlowText` (纯文本) + User 纯文本 | **已完成** | `c77aa97a` |
| 4 | B2b `FlowText` 行内样式 + `FlowCodeBlock` | **已完成** | `b603bf3b` |
| 5 | A/C 每帧成本与列表长度解耦 + 估算链路瘦身 | **已完成** | `bd6f531c` |
| 6 | A6 稳定块预算 + A7 去重 + B3 预算标定 | **已完成** | `0bd9c580` |
| 7 | 渲染性能基准模块 + 文档同步 | **已完成** | `9f523be9` |
| 8 | **阶段 5 完全体: 视口锚点即主状态** (本轮) | **已完成** | `ff9a1152` + 本提交 |

> 第 8 项是方案 §3.4/§3.5 的完整形态: 把"偏移从顶部计 + 前插多帧校正 + 估算容错带"
> 替换为"锚点 `(anchorIndex_, anchorRow_)` 即唯一滚动状态"。原先第 5 项保留旧滚动状态
> 只做每帧成本优化的折中做法已不再存在 (见下文"阶段 5 完全体")。

---

## 已完成内容 (按阶段)

### 阶段 2 (B1): 紧凑文本节点

- `agent/third_party/ftxui/src/ftxui/dom/text.cpp` (子模块 `72e11a66f`): `Text` 节点由
  "每字素一个 `std::string`" 改为 **原文 + 各行起始字节偏移**; 渲染按行解码直写单元格
  (全角占两格且第二格占位空串, 组合字符并入前一格, 控制字符不占列); `Select` 用同一套
  字节偏移, 列区间到文本的映射与旧实现逐条对齐; 单行文本不分配偏移数组。
- 新增测试模块 `ftxui_text`: 行数/列宽/尾随换行/宽字符/组合字符/控制字符/裁剪/选择取
  文本 + 内存上界断言 (`operator new` 计量存活字节)。

### 阶段 3 (B2a): FlowText + User 纯文本

- `markdown_ftxui` 子模块 `932d854`: 新增 `markdown::FlowText` (单节点承载整段文本;
  空格为词边界折行、超宽词按列硬拆、`'\n'` 硬换行; **折行结果按宽度缓存**; 逐格行内
  样式; `Select`; 链接可见区段登记; 迭代布局与 `ftxui::flexbox` 同口径) 与
  `text_utils.hpp` 的 `glyph_columns` / `is_zero_width` / `utf8_decode_at`。
- `markdown_block.{h,cpp}` 新增 `renderPlainText`; `MessageListComponent` 的 User 消息
  正文由 `ftxui::paragraph` (每词一节点) 改为单节点折行文本。
- 新增测试模块 `markdown_flow`。

### 阶段 4 (B2b): 段落/行内/代码块改用自绘节点

- `markdown_ftxui` 子模块 `6c73e8e`: `collect_inline_spans` (行内内容收集为带样式的
  片段; 软换行→空格, 硬换行→`'\n'`, 图片→`"[IMG: alt]"` 文本) + `build_wrapping_container`
  改用 `FlowText`; 新增 `FlowCodeBlock` (语言标签行 + 上下内边距 + 按盒宽折行);
  `Theme` 增加 `link_style` / `link_focus_style` / `code_inline_style`; 链接点击区段
  改由折行节点记录, `flat_link_boxes()` 按当前布局结果返回拷贝。
- client `tui_theme.cpp` 为两套主题填充新的逐格样式; `markdown_block` 测试补充行内
  样式/链接区段/标题/列表/引用/图片/硬换行抽查。

### 阶段 5 (A/C): 每帧成本与列表长度解耦 + 估算链路瘦身

`LazyScrollable`:
- **尾部窗口发现** (吸附底部时从尾部向前累计到一屏, 边走边实测): 视口定位只依赖
  尾部一屏内条目的实测高度 + 视口上方高度和, 与视口上方未实测条目的估算无关。
- **扫描起点** (`scanStartIndex_` + `rowsAboveScanStart_`, 增量维护): 每帧从"第一个与
  视口相交的条目"开始向后扫描并随滚动推进, 不再每帧从头部重扫。
- **高度和增量维护** (`setItemHeight`): 去掉每帧 O(n) 求和; 宽度变化时按 O(1) 粗略值
  重算 (不触发渲染)。
- **key 惰性校验** (`keyFrames_` 帧标记): 只校验窗口 + 下方预取带内的条目; 视口外条目
  的失效延迟到重新进入窗口时 —— 去掉每帧 O(n) 的 `itemKey` 调用 (含插件缓存加锁)。
- **按上一帧记录复位** 可见盒/已确保保护标记 (不再 O(n) 全量填充)。
- `estimateHeight` 改名 `quickHeight` (语义: 必须 O(1) 的粗略高度)。

`MessageListComponent::quickHeight`: 不再查插件语义渲染 (`queryToolRender`)、不再
`measureItems` (真渲染装饰) / 中断表单 `layoutForm`; 展开的工具消息按参数/结果字节折算
行数, 中断消息给量级估计; 正文仍用不含渲染的文本扫描 (`estimateLines` /
`estimateMarkdownLines`), 保持既有精度。

新增测试模块 `tui_lazy_view`: 每帧构建/校验次数有界且与条数无关 (2000/5000 条对比)、
前插零跳变、实测高度精确、滚动边界、估算偏差 (高估/低估 10 倍) 不影响视口内容。

> 注: 本阶段的滚动状态 (`scanStartIndex_` / `rowsAboveScanStart_` / `pendingPrepend_` /
> `kEstimateSlack`) 已在**阶段 8** 被锚点模型替换 (见下文), 上述机制名仅作历史记录。

### 阶段 6 (A6/A7/B3)

- **A6 稳定块预算** (`markdown_ftxui` 子模块 `26dbfbd`): 稳定块元素改为"访问即构建 +
  LRU 保留" (`setElementBudget`, 默认 16 块 / 256KB 源字节), 超预算释放 element+builder
  (源码文本保留, 需要时按块重建); 主题/宽度变化整体释放。
- **A7 工具渲染请求去重前置**: 新增 `ClientToolRenderRequest::hashInputs` (string_view,
  零拷贝) 与 `ClientToolRenderCache::inFlight` / `ClientPluginManager::toolRenderInFlight`;
  `renderClientTool` 与 client 的 `queryToolRender` 先算特征 + 查缓存/在途表, 只有确实要
  投递时才拷贝 args/result 大文本。
- **B3 sourceBytes 标定**: 新增实测用例 (929B 源文档渲染树存活 32KB = ×35; 基线旧实现
  ×112; 纯文本节点 ×1.34), 折算由"源 ×64"改为"源 ×24 + 固定 2KB"; 缓存预算
  `maxBytes` 4MiB → 2MiB, 豁免阈值 64KB → 32KB。

### 阶段 7: 基准模块与文档

- 新增 `agent/benchmark/bench_render.h` 并在 `benchmark_main.cpp` 注册模块 `render`:
  单条消息解析+建树 (助手/用户/工具样本)、布局三种口径 (首次测量 / 同宽度重复布局 /
  流式盒位置上移)、整屏 N 条消息 (布局+绘制 / `Screen::ToString`)、
  `LazyScrollable` 在 100/1000/5000 条下的每帧成本与滚动一屏成本。
- `docs/zh-cn/design/tui.md`: §1.5 与 §3.3 重写/扩写 (3.3.1 渲染基元, 3.3.2 懒构建列表
  每帧成本, 3.3.3 其它), §3.1 补充"滚动定位以实测高度为准"的要求。

### 阶段 8 (本轮): 阶段 5 完全体 —— 视口锚点即主状态

方案 §3.4/§3.5 的完整锚点模型 (`agent/client/{include,src}/io/tui/lazy_scrollable.{h,cpp}`):

**状态与不变量**

- 唯一滚动状态: `anchorIndex_` (视口顶行所在条目) + `anchorRow_` (视口顶行在该条目内
  的行偏移, `0 <= anchorRow_ < 高度(anchor)`); `scrollOffset()` 变成**派生值**
  (`rowsAboveAnchor_ + anchorRow_`, 夹取到 `[0, 总高 - 视口高]`), 只服务滚动条长度/
  位置与历史分页预取判定。
- ① **定位只用实测高度**: 每帧从锚点向后逐条 "测量 -> 定位", 锚点以上条目完全不参与
  本帧计算 (不构建/不测量/不读高度)。视口上方未实测条目只在 `rowsAboveAnchor_` 里按
  `quickHeight` 计入 —— 估算再离谱 (少报/多报 4~100 倍) 也只让滚动条长度偏旧。
- ② **测量即渲染**: 条目高度取自它自己的布局结果, 无第二套行数判定。
- ③ **每帧成本 = O(锚点到视口底部)**: key 校验只覆盖窗口 + 下方预取带; 上一帧的
  可见/已确保记录按列表复位; 宽度变化时全部高度回到粗略估算 (每条 O(1), 不渲染)。

**窗口发现与滚轮**

- 吸附底部 (默认): 每帧从尾部向前实测累计到一屏, 锚点 = 尾部窗口起点, 行偏移 =
  窗口内被视口遮住的行数 (`rowsAboveAnchor_` 由此刻总高反推, 派生偏移严格等于
  `总高 - 视口高`, 滚动条贴底)。
- 非吸附: 锚点即视口顶行, 只向后定位; 锚点条目本轮必测 (行偏移夹取到 `[0, 高度-1]`)。
- 滚轮每次 1 行, 事件只累积 `pendingScrollRows_`, **下一帧布局时落实**: 上滚时行偏移
  先减, 减到 0 就跨入上一条目 (先实测取真实高度, 顶行落在它最后一行); 下滚时行偏移
  先加, 到底进入下一条目首行, 内容末尾已落到视口内则恢复吸附底部。
  - **"是否已到底"取上一帧定位阶段的精确结论** (`contentEndsInViewport_`: 布局是否
    走到末尾条目且累计行数未超视口高), 不用估算高度和判断 —— 否则视口下方条目被
    低估时会在下滚中途误判到底, 把视口直接吸到内容末尾 (新增用例: 估算少报 4 倍时
    从顶部逐行下滚必须严格连续, 中途不得吸附)。一次性落实多行下滚跨过到底那一行时,
    由"锚点已在末尾条目行尾"兜底 (同样恢复吸附)。
  - **不在 OnEvent 里构建元素**: 构建会登记命中区 (decor 按钮/中断控件/附件卡片),
    而每帧 `OnRender` 会清空登记表, 事件期构建的元素命中区会丢 -> 按钮点击失效。

**删除的机制 (方案 §3.4 要求整体删除)**

- `pendingPrepend_` / `applyPrependAnchorCorrection` (前插多帧收敛校正)
- `kEstimateSlack` (估算容错带) 与"扫描起点"(`scanStartIndex_`/`rowsAboveScanStart_`)
- "先按估算定位、下一帧再修正"的两阶段布局 (阶段 1 构建测量 + 阶段 2 定位)
- `scroll_common.h::handleWheelScroll` (锚点模型与偏移模型语义不同, 无法共用;
  该头现在只保留共用的 `layoutAndMeasure` / `kTallHeight`)

**前插 (历史分页)**: `notifyPrepended` = 并行数组头插 + **锚点索引平移**, 视口内容
零跳变; 新增区高度记 `-1` (`unknownPrefix_`), 下一帧按新快照口径补齐 (位于锚点上方,
只影响滚动条)。`clearPrependAnchor` 改名 `resetAnchorState` (整体替换/会话切换时复位)。
帧间连续多次前插 (分页连发) 时未知区在前缀**累加** (`unknownPrefix_ += count`),
保证每一次前插的新增区都被补齐 (否则后一次的新增区高度一直是未知值)。

**宽度变化**: 锚点 (条目 + 行偏移) 保持不变, 只把行偏移夹取到新高度内 —— 终端 resize
后视口仍停在同一条内容上 (旧模型按偏移从顶部重定位, 内容会跳)。

**测试**: 新增 8 组锚点模型用例 (`agentxx_test tui_lazy_view` 单独 3814 项):

1. 顶行严格由锚点决定 —— 逐行滚动 (含跨条目) 顶行/末行逐行推进, 不跳行不重复;
2. 定位只用实测高度 —— 估算少报 4 倍时吸附底部与上滚后的顶/末行仍严格正确,
   总高仍按估算 (只影响滚动条);
3. 每帧成本与列表长度/视口上方条数无关 —— 5000 条列表上滚 100 行只构建 ~100 条,
   稳态帧零构建零估算, key 校验只覆盖窗口 ± 预取带;
4. 前插零校正 —— 新增区估算 40 行 vs 实测 3 行时单帧内画面逐行不变, 锚点索引平移,
   派生偏移只按估算增加; 连续两次前插的未知区都要补齐;
5. 估算只影响滚动条 —— 同内容两种离谱估算下视口画面逐行一致;
6. 下滚到底由实测布局判定 —— 估算低估 4 倍时从顶部逐行下滚严格连续, 中途不得吸附;
7. 终端宽度变化 —— 锚点条目不变 (行偏移按新高度夹取);
8. 既有用例外加: 实测高度精确、滚动边界、前插稳定、key 变化重建。

`agentxx_test` 全量 27091 项断言通过; `tui_scroll` 760 项历史回归 (抖动/前插/折叠/
预算/高视口/流式) 未改动全部通过 —— 说明可达滚动位置集合与旧模型一致 (逐行滚动、
滚到顶/底、吸附恢复的语义完全对齐)。

---

## 实测数据 (本轮环境: Windows x86_64, MSVC Debug, 宽 100 列)

| 指标 | 修复前 (方案基线) | 现在 |
|---|---|---|
| 100 条 1KB 文本节点存活字节 | 源文本 30~60 倍 | **137 KB (×1.34)** |
| markdown 渲染树 (929B 源) | ×112 (1.4KB 文档基线) | **32 KB (×35)** |
| User 消息渲染节点 | 每词一个节点 | **1 个节点** (折行缓存) |
| markdown 段落节点 | 每词一个节点 | **1 个节点/段** (代码块同理 1 个/块) |
| 同宽度重复布局 | 每帧重排 (1.28 ms/帧基线) | **折行缓存命中, 只更新盒位置** |
| 每帧处理范围 | O(历史长度) (key/高度和/坐标全量) | **O(锚点..视口底部 + 预取带)** |
| 未可见条目高度估算 | 语义估算 (含 `queryToolRender`/真实渲染) | **O(1)/轻量文本扫描, 不渲染** |
| 视口定位依据 | 偏移 (含上方估算) + 估算容错带 | **锚点 + 实测高度** (估算不影响内容) |
| 前插 | 多帧收敛校正 (`pendingPrepend_`) | **锚点索引平移, 零校正** |
| 流式稳定块内存 | 全部稳定块常驻 | **预算 16 块/256KB, LRU 释放** |
| 客户端测试 | — | 25 个模块 2827+ 项 (既有) + 锚点模型 8 组 |
| `agentxx_test` 全量 | — | **27091 项断言全部通过** |

`agentxx_benchmark render` (Debug + ASan, 40 行视口, 每条 2 行; 本轮补采):

| 场景 | 100 条 | 1000 条 | 5000 条 |
|---|---|---|---|
| 每帧 (吸附底部, 无滚动) | 27.0 ms | 27.0 ms | 30.1 ms |
| 每帧 (视口停在列表中段) | 25.9 ms | 27.7 ms | 27.8 ms |
| 滚动一屏 (40 行, 每行一帧) | 1.09 s | 1.07 s | 1.20 s |

帧成本与条数基本持平 (±10%, Debug+ASan 下绝对值由 `Screen::ToString` 主导;
Release 基线见方案 §1: 平均帧 0.143~0.165 ms)。"视口停在列表中段"与"吸附底部"
同量级, 直接印证"帧成本与视口上方条数无关"。

未运行的验收项 (说明):
- 方案 §1 的微基准 (`%TEMP%\...\mdbench`) 已不在磁盘上, 无法重跑逐项对比;
  内存量级由 `ftxui_text`/`markdown_block` 的存活字节用例覆盖 (见上表)。
- `resource_real_tui` 的帧耗时/RSS 对比本轮仍未采集 (真实 TUI 基准需要交互终端与
  长时间会话样本)。需要时执行:
  `cmake -DAGENTXX_BUILD_BENCHMARK=ON ...` 后
  `cmake --build <build> --config Debug --target agentxx_benchmark`,
  再跑 `agentxx_benchmark resource_real_tui`。

---

## 与方案的差异 (需要知道)

1. **阶段 5 分两步落地**: 第一次 (`bd6f531c`) 只做了"每帧成本与列表长度解耦 + 估算
   链路瘦身", 保留了"偏移从顶部计 + 前插多帧校正"的旧滚动语义; 本轮 (`ff9a1152`)
   才按方案 §3.4 换成**锚点即主状态**并删除 `pendingPrepend_`/估算容错带/扫描起点。
   现在方案 §3.4 与 §3.5 的全部条目都已落地 (含"估算不参与定位"与"前插零校正")。
2. **`quickHeight` 不是纯 O(1) 字节折算**: 助手/思考正文仍用 `estimateMarkdownLines`
   (线性文本扫描, 不含渲染)。工具/中断消息改为 O(1) 字节折算 (方案目标: 去掉
   `queryToolRender`/`measureItems`/`layoutForm` 这些**会渲染**的成本 —— 已去掉)。
   保留文本扫描是为了不改动既有"估算≈实测"的精度约定 (tui_scroll 场景 13 断言
   `totalHeight()` 精确值)。锚点模型下估算已不参与视口定位, 该精度只影响滚动条长度。
3. **`Theme::table_*_style` 未新增**: 表格仍走既有逐元素实现 (方案 §3.2 表格行标注
   "本期保留"), 因此没有引入用不到的主题字段。
4. **`scrollOffset()` 语义收窄**: 现在是锚点派生值 (视口上方未实测条目按估算计入),
   只保证滚动条与预取判定够用, 不代表视口内容对应的高度; 吸附底部时严格等于
   `totalHeight() - viewportHeight()`。既有调用点 (`maybeRequestMoreHistory` 的
   距顶判定、测试断言) 已逐一核对, 语义等价。

## 注意事项 (后续维护)

- **ftxui / markdown_ftxui 是 ExternalProject + git 子模块**: 改源码或改其
  `CMakeLists.txt` 后外层构建不会自动重编 —— 需删除
  `agent/build/<cfg>/<name>_repo-prefix/src/<name>_repo-stamp/Debug/` 下的
  `*-configure` / `*-build` / `*-install` / `*-done` stamp (或整个 prefix 目录)。
  只改 `.cpp` 时删 build/install/done 三个即可; 改了头文件 (如 `incremental.hpp`)
  必须让 install 重新执行, 否则测试编译用的是旧头文件。
- 子模块改动需**先在子模块仓库内提交**, 再在外层仓库提交指针更新。
- ftxui 用 `/W3 /WX` 编译, 改动不得引入任何警告; markdown_ftxui 侧为 `/W4` (不升级为错误)。
- 源文件里写中文字面量注意十六进制转义贪婪匹配: `"a\x01b"` 会被解析成 `0x1B`, 应写 `"a\x01" "b"`。
- 测试断言用局部期望值 (曾把 "汉字" 的期望误写成 "好" 造成假失败); 逐行比较屏幕内容时
  注意滚动条列是多字节字符, 不能用 `pop_back()` 删一整列 (要按 UTF-8 码点删)。
- Windows 构建脚本末尾 "copy exec → windows-debug-output" 失败属预期 (运行中的
  agentxx_cli 占用); 跑测试用 `agent/build/windows-debug/exec/agentxx_test.exe`。
  并行过高时偶发 `MSBUILD : error MSB4166` (子节点早退), 重跑或降低
  `AGENTXX_BUILD_PARALLEL` 即可。
- 新增测试模块: `test/client/test_xxx.cpp` + `test/include/agentxx-test/client/test_xxx.h`
  + `test.cpp` 注册 (`runSync`); 源文件由 CMake glob 收集, 需重新 configure。
- `LazyScrollable` 的不变量 (改动时必须保持):
  ① **锚点即唯一滚动状态**: 视口顶行 = `(anchorIndex_, anchorRow_)`; 视口内条目的
     位置只由锚点 + 各条目实测高度决定, `rowsAboveAnchor_`/`quickHeight` 的估算
     **不得**参与任何定位计算 (只用于 `scrollOffset()`/滚动条与预取判定);
  ② 每条目高度变化必须经 `setItemHeight` (它维护 `totalHeight_` 与
     `rowsAboveAnchor_`), 不要直接写 `heights_`;
  ③ 移动锚点前必须先 `measureItem` 目标条目 (它即将进入视口, 高度必须真实),
     移动锚点时同步 `rowsAboveAnchor_` (跨条目: 进/出该条目的高度);
  ④ 滚轮只累积 `pendingScrollRows_`, 在 `prepareLayout` 里落实 —— **不要在
     `OnEvent` 里构建 Element** (构建会登记命中区, 每帧 `OnRender` 会清空登记表,
     事件期构建的元素命中区会丢, 表现为按钮点不动);
  ⑤ 吸附底部 (`stickToBottom_`) 每帧重算锚点, 其 `rowsAboveAnchor_` 由
     `totalHeight_ - 窗口行数` 反推 (覆盖 while 里 `setItemHeight` 的增量修正),
     保证派生偏移严格等于 `总高 - 视口高`;
  ⑥ 头部前插只做 `锚点索引 += count` + `unknownPrefix_` 标记, 不加任何偏移校正。
- 调试锚点/滚动问题时的抓手: `anchorIndex()` / `anchorRow()` (测试用访问器)、
  `tui_lazy_view` 的"顶行严格由锚点决定"用例 (逐行滚动断言屏幕顶行内容)。
- `tui_lazy_view` 夹具的条目**逐行渲染行号** (`item <id> mark=<id>[ row <r>]`),
  可用 `topPos()`/`bottomPos()` 断言视口首/末行内容 —— 新增定位类用例直接用这套断言。
