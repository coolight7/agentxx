# 实施记录：工具特化渲染 = 显示名 + 折叠内容 + 展开内容

方案见 [plan.md](plan.md)，任务卡片见 [index.md](index.md)。

## 当前进度

- [x] 现状调查（C ABI / SDK / 注册表 / 渲染查询 / TUI 消费点 / 插件 / 测试 / 文档）
- [x] 方案定稿（显示名进注册；折叠与展开拆成两个回调、两份缓存；删 `summaryKey`/模版；
      头行骨架固定；老逻辑清单见 plan §4）
- [x] 步骤 1：C ABI + SDK（两个回调类型/输出、spec 字段、`ToolRenderers`、kit 封装）
- [x] 步骤 2：注册表 + 两部分渲染查询 + `toolDisplayNameFor` + 按部分缓存/指纹
- [x] 步骤 3：内置渲染器 + 4 个插件（模版 → 折叠内容 lambda，去掉 `" · "` 前导）
- [x] 步骤 4：TUI（固定骨架、两部分查询、itemKey、删预取与嗅探）
- [x] 步骤 5：测试（`plugin_runtime` / `client_plugins` / `tui_tool_header` / `tui_scroll`）
- [x] 步骤 6：文档（`plugins.md` §9、`tui.md` §2.2/§2.14/§4、`docs/en`、`AGENTS.md`）
- [x] 步骤 7：编译 + 插件重建 + 全量测试

## 每步改动与验证

### 步骤 1：C ABI + SDK

改动的文件：

- `agent/lib/include/agentxx/plugin/api/client_plugin_api.h`
  - 删除 `AgentxxToolRenderOutput` / `AgentxxToolRenderFn`，新增
    `AgentxxToolCollapsedOutput`（`text`）、`AgentxxToolExpandedOutput`（`items_json`）、
    `AgentxxToolCollapsedRenderFn`、`AgentxxToolExpandedRenderFn`；
  - `AgentxxToolRenderSpec` 改为 `{struct_size, version, tool_name, display_name,
    collapsed_fn, expanded_fn, user_data}`（64 字节）；删除 `template_json`；
  - `register_tool_renderer` 的注释按新语义重写。
- `agent/lib/include/agentxx/plugin/api/plugin_kit.h`
  - `ToolRenderOutput` → `ToolCollapsedOutput` / `ToolExpandedOutput` + `ToolRenderers`；
  - `registerToolRenderer` 签名改为 `(host, ui, toolName, displayName, ToolRenderers, shimStorage)`，
    shim 持有回调副本，两个 trampoline 分别调用；只注册给了回调的那一侧（另一侧传 NULL）；
  - 删除 `registerToolTemplate` 与 `ClientPluginBase::registerTemplate`；
    `ClientPluginBase::registerRenderer(tool, displayName, renderers)`。

验证：`agentxx_static` 目标编译通过。

### 步骤 2：注册表 + 两部分渲染查询

- `client_plugin_manager.h`
  - `ClientToolRenderReg`：`displayName`（归一后恒非空）+ `collapsedFn` / `expandedFn` /
    `userData`；删除 `renderFn` 与四个 `template*` 字段；
  - 新增 `ClientToolRenderPart`（`Collapsed` / `Expanded`）、`ClientToolCollapsedResult`、
    `ClientToolExpandedResult`；删除 `ClientToolRenderResult`；
  - `ClientToolRenderEntry`：删除 `displayName`/`summary`，改为 `matched` + `text` + `items`；
  - `ClientToolRenderRequest` 增加 `part`；`keyFor(toolCallId, toolName, part)` 追加
    `#c` / `#e` 后缀（两部分条目、版本号、请求去重全部按部分独立）；
  - `renderClientTool` → `renderClientToolCollapsed` / `renderClientToolExpanded`；
    新增 `toolDisplayNameFor(reg, toolCallId, toolName)`；
  - `registerBuiltinToolRenderer(toolName, displayName, collapsedFn, expandedFn, userData)`。
- `client_plugin_manager.cpp`
  - `performToolRender` 按 `req.part` 只查/只调对应部分的回调，写对应字段；
  - 注册路径删除模版 JSON 解析与体积校验，`displayName` 空 → 归一为工具名；
  - 删除 `truncateToolSummary`（只服务模版）；
  - 查询流程：decor 有该部分内容 → 用它；否则注册项有该部分回调 → 缓存命中用条目
    （空 = 回调返回 null → 默认渲染）、未命中 `pending` + 沿用同调用同部分的上一份；
    否则没注册。没有 `toolCallId` 时不沿用上一份（键会退化成 `#工具名`）。
- `builtin_tool_renderers.{h,cpp}`：只注册显示名 + 折叠内容（`Store` / `Subagent`），
  文本去掉前导 `" · "`。

验证：`agentxx_static` 编译通过。

### 步骤 3：内置渲染器与插件

- `agentxx_filesystem`：`List`/`Write`/`Remove` 模版 → 折叠内容 lambda（取 `path`）；
  `Read`（区间 + 路径）、`Glob`、`Grep`、`Move`、`Copy` 只注册折叠内容；
  `Edit` 折叠内容（`±行数` + 路径）+ 展开内容（逐段 `Diff` items）。
- `agentxx_websearch`：`Search` 折叠内容取 `query`、`Fetch` 取 `url`。
- `agentxx_execute_command`：两条命令工具折叠内容取 `command` 首行（多行补 ` ...`）。
- `agentxx_planning`：`buildPlanningCollapsedText` / `buildPlanningExpandedItems` 两个薄封装
  （共用 `buildPlanDecorParts`），注册时给显示名 `Plan` + 两个回调。

验证：`build_plugins.ps1` 编译 4 个插件通过，`exec/plugins/*` 时间戳已更新。

### 步骤 4：TUI

- `message_list.cpp`
  - `queryToolRender` → `queryToolCollapsed` / `queryToolExpanded`（后者只在展开态调用，
    按部分键去重 + 投递）；
  - 折叠头固定骨架 `+ [工具] <名字> · <内容>`：失败结果首行 > 自定义折叠内容 >
    pending 且无上一份则**不画** ` · ` > 默认（运行中 `msg.genToolCalling` / 已完成结果首行）；
    删除 `starts_with(" ·")` 嗅探与"运行中显示参数预览"分支；
  - 展开头固定 `- [工具] <名字> <耗时>`；展开体：失败 → 默认（参数 + 结果）>
    自定义 items > pending 留空 > 默认（参数 + 运行中 `msg.genToolCalling` / 结果，结果为空不画）；
  - `toolDisplayNameOf` / `genPlaceholderToolDisplayName` 改为 `toolDisplayNameFor` 同步查表；
  - 占位行改为 `{符号} [工具] <名字> · 正在调用工具...`（名字为空时不画 ` · `）；
  - `itemKey`：折叠态计入折叠内容版本 + 显示名哈希，展开态再加展开内容版本；
    删除 `#工具名` 名称结果版本项。
- `message_list.h`：两处注释按新语义重写。
- `agent_tui.cpp`：删除 `prefetchInterruptToolNames` 及调用点。
- `interrupt_view.{h,cpp}`：`ToolDisplayNameFn` 去掉"返回空串 = 未就绪"的约定。
- `tui_i18n.cpp`：删除 `tool.running` 条目。

验证：`agentxx_client`（内层构建目录）编译通过。

### 步骤 5：测试

- `test_plugin_runtime.cpp`：`AgentxxToolRenderSpec` 布局断言 → `display_name` 24、
  `collapsed_fn` 40、`expanded_fn` 48、`user_data` 56、`sizeof` 64。
- `test_client_plugins.cpp`
  - 新 helper `RenderedTool{displayName, summary, items, matched, collapsedPending,
    expandedPending}` + `queryExpandedAsync`，`renderToolAsync` 改为折叠/展开各查一次；
  - 断言去掉折叠文本的 `" · "` 前导；`hasRenderer` = 任一侧回调非空；
  - 重写 21b（折叠/展开分部分缓存、pending、回调返回空、装饰缺项回落、卸载失效）、
    重写 28.10（插件注册项优先于内置）；`entry.displayName` → `entry.text`。
- `test_tui_tool_header.cpp`
  - `makeTestToolRegistry()` 全部改为 `displayName` + `collapsedFn`/`expandedFn`；
  - `pushRenderEntry` → `pushCollapsedEntry(key, plugin, text)`，删 `pushToolNameResult`；
  - 新 helper `resolveFromRegistry(...)`：用镜像渲染器在测试线程算出内容并写入语义缓存
    （镜像只存在于 UI 注册表快照里，管理器执行请求时找不到它们 —— 见下面“已知坑”）；
  - `testTuiToolHeaderPendingRender` → `testTuiToolHeaderPendingContent`；
    占位行断言 `Read · receiving tool call...`、展开首行 `- [Tool] Read 1.2s` 等。

验证（`exec` 下实际运行）：

```text
plugin_runtime    passed=730 failed=0
tui_tool_header   passed=166 failed=0
client_plugins    passed=774 failed=0
plugin_sdk        passed=133 failed=0
tui_widget        passed=208 failed=0
tui_form          passed=84  failed=0
tui_ui_items      passed=277 failed=0
ui_items          passed=117 failed=0
boundaries        passed=19  failed=0
tui_scroll        passed=1004 failed=0
```

### 步骤 6：文档

- `docs/zh-cn/design/plugins.md` 的“工具特化渲染架构”第 1 条重写（显示名注册时确定、
  两个回调、两份缓存、折叠内容不带 `" · "`、模版机制已删 + 新 SDK 示例）、第 2 条
  （内置渲染器只给显示名 + 折叠内容）、第 6 条（语义渲染缓存：按部分 pending/沿用/版本,
  未就绪帧不画 ` · `、失败走默认、无语义缓存时同步调用）、第 4 条（显示名与展开状态无关）。
- `docs/zh-cn/design/tui.md`：§2.2 新增工具消息头骨架说明；§2.6 中断分段改为
  “显示名同步可得”（删预取与“留空”约定）；§2.14 占位行改为
  `- [工具] Read · 正在调用工具...` 并重写被取代帧的规则；§3.3 高度估算不得查两部分内容；
  §4 回归表那一行按 `pending` 新语义重写。
- `docs/en/design/plugins.md`：同步第 1 条（显示名 + 两个回调 + pending 帧）。
- `AGENTS.md`：“参数自愈”那条去掉“预设模版摘要”，新增一条“工具特化渲染的注册形状”
  （显示名注册时确定、折叠内容不带 ` · `、两部分各自缓存、旧插件 DLL 必须重建）。
- 顺手清理的旧注释：`client_plugin_manager.h` 的 `queryToolRender`、
  `test_client_plugins.cpp` 的两处“(模版)”、`test_tui_scroll.cpp` 场景 23 的注释与断言。

### 步骤 7：编译与全量测试

- `agentxx_static` / `agentxx_test` / `agentxx` 三个构建目录均编译通过（无新增警告）,
  三个 `cmake --install` 都已执行（`exec/` 下的 exe 均为最新）。
- 全量 `agentxx_test --log-level warn`：**passed=42786 failed=2**，两个失败都在
  `input_delivery`（line 1184 / 1203，inject 输入的分页提示与 share store 取回）。
  单跑两次：第一次同样 2 失败、第二次 139 通过 0 失败 —— 是既有的时序抖动
  （与该模块共享代码的只有 agent 侧输入投递，与工具渲染无关）。
- 插件：`build_plugins.ps1` 全量重建过一次，之后再未改插件源码（只改过 planning 的临时
  调试日志，已删并重建）。

## 验证清单

- [x] 三个构建目录（lib/test/client）编译通过（无新增警告）
- [x] `build_plugins.ps1` 重建 `agentxx_filesystem` / `agentxx_planning` /
      `agentxx_execute_command` / `agentxx_websearch`，`exec/plugins/*` 时间戳已更新
- [x] `agentxx_test --log-level warn plugin_runtime client_plugins tui_tool_header`
- [x] `agentxx_test --log-level warn plugin_sdk tui_widget tui_form tui_ui_items ui_items boundaries tui_scroll`
- [x] 全量 `agentxx_test --log-level warn`（除 `input_delivery` 的 2 个抖动用例外全通过）
- [ ] TUI 手工冒烟（同上；本次只有自动化覆盖，未经真实终端交互）
- [x] 折叠行不再触发展开体渲染：`tui_tool_header` 的 pending 用例与 `client_plugins`
      的分部分缓存用例覆盖（折叠态只查 `#c` 键）

## 注意事项 / 已知坑

- 接口代次保持 5，但 `AgentxxToolRenderSpec` 布局变了（64 字节）：**旧插件 DLL 必须重建**，
  否则新主程序会按新布局读旧内存 —— 实测现象：旧测试可执行文件 + 新插件时，旧布局把
  `display_name`(偏移 24) 当函数指针调用，直接跳到字符串常量 `"Plan"` 上（ASan 报
  `access-violation`，PC 落在插件 `.rdata`）。
- 改完 `agent/lib` 与 `agent/test` **必须各跑一次 `cmake --install`**：测试/客户端用安装前缀里的
  头与库，`exec/agentxx_test.exe` 也只有 install 才更新；只 `cmake --build` 会留下旧可执行文件，
  与新建的插件 DLL 混跑（上面的崩溃即由此造成，排查耗时远大于一次 install）。
- `test_plugin_runtime.cpp` 的 `offsetof`/`sizeof` 断言要按 plan §2.2 改：
  `display_name` 24、`collapsed_fn` 40、`expanded_fn` 48、`user_data` 56、`sizeof` 64。
- 缓存键按部分分条（`#c` / `#e`）：`ClientToolRenderCache::version`、`beginRequest`、
  `lookupLatest` 都要按分条后的键调用；TUI 的 itemKey 在折叠态只计折叠键。
- 折叠内容必须**不带前导 `" · "`**，分隔符由 TUI 统一加；插件里所有
  `summary = " ·"` 的写法要一并清掉（filesystem/builtin/planning/测试夹具）。
- 展开体同样沿用上一份结果过渡（`lookupLatest`），没有上一份才留空；不用占位文字顶替。
- 失败（`Failed`）时折叠内容与展开体都走默认渲染，不吃自定义内容（沿用现有规则）。
- 占位行（`GenPhase::ToolCall`）单行、不可点击：`{符号} [工具] Read · 正在调用工具...`；
  工具名未解析时不画 ` · `。
- `tui_tool_header` 的占位断言要同步：`Read receiving tool call...` → `Read · receiving tool call...`；
  `tui_scroll` 场景 23（占位行显示名）同样要加 ` · `；
  `tool.running` 的 i18n 条目与使用一并删除（默认展开体的运行中行改用 `msg.genToolCalling`）。
- 夹具与真机的差异（只影响测试写法）：`tui_tool_header` 的镜像渲染器只进 UI 注册表快照,
  没有真实插件实例可注册到管理器 —— 所以“内容算出后上屏”的用例用
  `resolveFromRegistry()` 直接在测试线程跑同一份回调并写入语义缓存，而不是 `drainIo()` 驱动
  管理器。真实执行路径由 `client_plugins` 加载真插件 DLL 覆盖。
- 判定口径：`pending == false` 且内容为空 = 没注册或回调返回 null → 默认渲染；
  `pending == true` 且无上一份 → 折叠不画 ` · `、展开留空。
- 运行中的工具行只要特化内容就绪就照常用（不等完成）；`正在调用工具...`
  只出现在占位行与默认渲染的运行中行。
