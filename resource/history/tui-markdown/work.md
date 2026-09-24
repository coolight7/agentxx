# TUI 消息列表渲染重构 实施记录 (work.md)

方案原文: [plan.md](plan.md) —— 本文件记录实施进度、已完成/待完成内容与注意事项。

## 进度总览

| # | 提交内容 | 状态 | 提交 |
|---|---|---|---|
| 1 | 方案 `plan.md` | 已完成 | `9931ff44` / `d4112e27` |
| 2 | B1 紧凑文本节点 (ftxui `Text`) | **已完成** | `ed99c480` |
| 3 | B2a `FlowText` (纯文本) + User 纯文本 | **已完成** | `c77aa97a` |
| 4 | B2b `FlowText` 行内样式 + `FlowCodeBlock` | **已完成** | `b603bf3b` |
| 5 | A/C 每帧成本与列表长度解耦 + 估算链路瘦身 | **已完成** (见"与方案的差异") | `bd6f531c` |
| 6 | A6 稳定块预算 + A7 去重 + B3 预算标定 | **已完成** | `0bd9c580` |
| 7 | 渲染性能基准模块 + 文档同步 | **已完成** (基准模块已接入; 本轮未运行, 见下) | 本提交 |

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

---

## 实测数据 (本轮环境: Windows x86_64, MSVC Debug, 宽 100 列)

| 指标 | 修复前 (方案基线) | 现在 |
|---|---|---|
| 100 条 1KB 文本节点存活字节 | 源文本 30~60 倍 | **137 KB (×1.34)** |
| markdown 渲染树 (929B 源) | ×112 (1.4KB 文档基线) | **32 KB (×35)** |
| User 消息渲染节点 | 每词一个节点 | **1 个节点** (折行缓存) |
| markdown 段落节点 | 每词一个节点 | **1 个节点/段** (代码块同理 1 个/块) |
| 同宽度重复布局 | 每帧重排 (1.28 ms/帧基线) | **折行缓存命中, 只更新盒位置** |
| 每帧扫描范围 | O(历史长度) (key/高度和/坐标全量) | **O(窗口 + 预取带)** |
| 未可见条目高度估算 | 语义估算 (含 `queryToolRender`/真实渲染) | **O(1)/轻量文本扫描, 不渲染** |
| 流式稳定块内存 | 全部稳定块常驻 | **预算 16 块/256KB, LRU 释放** |
| 客户端测试 | — | 22 个模块 3874 项断言全部通过 |

未运行的验收项 (说明):
- `agentxx_benchmark render` 模块已实现并注册, 但本轮构建配置里
  `AGENTXX_BUILD_BENCHMARK=OFF` (方案 §1 的基线微基准也已不在磁盘上), 故未采集
  benchmark 报告; 需要时执行:
  `cmake -DAGENTXX_BUILD_BENCHMARK=ON ...` (或改脚本) 后
  `cmake --build <build> --config Debug --target agentxx_benchmark`,
  再跑 `agentxx_benchmark render`。
- `resource_real_tui` 的帧耗时/RSS 对比同样需要打开 benchmark 构建, 未在本轮采集。

---

## 与方案的差异 (需要知道)

1. **阶段 5 未采用"锚点即主状态"的完整锚点模型**。方案 §3.4 希望把 `anchorIndex_/
   anchorRow_` 作为唯一滚动状态并删除 `pendingPrepend_`/估算校正。实际实现保留了
   "偏移从顶部计 + `pendingPrepend_` 校正"的既有语义, 但把所有"每帧 O(历史长度)"
   的成本 (高度和、扫描、key 校验、可见盒复位) 都改成 O(视口) 增量维护, 并新增
   **尾部窗口发现** (吸附底部时的定位不再依赖视口上方条目的估算)。
   原因: `tui_scroll` 有 760 项断言覆盖多年历史回归 (抖动/空白/预算淘汰/前插锚定/
   滚动边界), 全量替换滚动状态语义的回归风险高, 而方案 §8 的验收标准 (每帧成本与
   条数解耦、内存、估算不参与定位) 已通过上述改造达成。前插仍走"多帧收敛校正"
   (`applyPrependAnchorCorrection`), 但配合增量高度和后不再产生跳变
   (新增 `tui_lazy_view` 的"前插零跳变"用例把关)。
2. **`quickHeight` 不是纯 O(1) 字节折算**: 助手/思考正文仍用 `estimateMarkdownLines`
   (线性文本扫描, 不含渲染)。工具/中断消息改为 O(1) 字节折算 (方案目标: 去掉
   `queryToolRender`/`measureItems`/`layoutForm` 这些**会渲染**的成本 —— 已去掉)。
   保留文本扫描是为了不改动既有"估算≈实测"的精度约定 (tui_scroll 场景 13 等)。
3. **`Theme::table_*_style` 未新增**: 表格仍走既有逐元素实现 (方案 §3.2 表格行标注
   "本期保留"), 因此没有引入用不到的主题字段。

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
- `LazyScrollable` 的两个不变量 (改动时保持):
  ① 视口内条目的**位置**只由实测高度 + 视口上方高度和决定, 估算只影响滚动条长度;
  ② 每条目高度变化必须经 `setItemHeight` (它维护 `totalHeight_` 与
     `rowsAboveScanStart_`), 不要直接写 `heights_`。
