# 实施记录：工具特化渲染 = 显示名 + 折叠内容 + 展开内容

方案见 [plan.md](plan.md)，任务卡片见 [index.md](index.md)。

## 当前进度

- [x] 现状调查（C ABI / SDK / 注册表 / 渲染查询 / TUI 消费点 / 插件 / 测试 / 文档）
- [x] 方案定稿（显示名进注册；折叠与展开拆成两个回调、两份缓存；删 `summaryKey`/模版；
      头行骨架固定；老逻辑清单见 plan §4）
- [ ] 步骤 1：C ABI + SDK（两个回调类型/输出、spec 字段、`ToolRenderers`、kit 封装）
- [ ] 步骤 2：注册表 + 两部分渲染查询 + `toolDisplayNameFor` + 按部分缓存/指纹
- [ ] 步骤 3：内置渲染器 + 4 个插件（模版 → 折叠内容 lambda，去掉 `" · "` 前导）
- [ ] 步骤 4：TUI（固定骨架、两部分查询、itemKey、删预取与嗅探）
- [ ] 步骤 5：测试（`plugin_runtime` / `client_plugins` / `tui_tool_header`）
- [ ] 步骤 6：文档（`plugins.md` §9、`tui.md` §2.2/§2.14、`docs/en`、`AGENTS.md`）
- [ ] 步骤 7：编译 + 插件重建 + 跑测试

## 每步改动与验证

（实施时逐条填写：改了哪些文件、怎么验证的、验证结果。）

## 验证清单

- [ ] `windows_debug_build.bat` 编译通过（无新增警告）
- [ ] `build_plugins.ps1` 重建 `agentxx_filesystem` / `agentxx_planning` /
      `agentxx_execute_command` / `agentxx_websearch`，`exec/plugins/*` 时间戳已更新
- [ ] `agentxx_test --log-level warn plugin_runtime client_plugins tui_tool_header`
- [ ] `agentxx_test --log-level warn plugin_sdk tui_widget tui_form tui_ui_items ui_items`
- [ ] TUI 手工冒烟：占位行（符号 + 名字 + ` · ` + 提示，不可点击）、工具行各分支
      （失败 / 自定义内容 / pending 留空 / 无自定义走默认 / 已有结果）、展开首行显示名+耗时、
      历史回溯一致、插件禁用后回退默认渲染
- [ ] 折叠行不再触发展开体渲染（日志或计数器确认只有展开态才请求 expanded）

## 注意事项 / 已知坑

- 接口代次保持 5，但 `AgentxxToolRenderSpec` 布局变了（64 字节）：**旧插件 DLL 必须重建**，
  否则新主程序会按新布局读旧内存。
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
  `tool.running` 的 i18n 条目与使用一并删除（默认展开体的运行中行改用 `msg.genToolCalling`）。
- 判定口径：`pending == false` 且内容为空 = 没注册或回调返回 null → 默认渲染；
  `pending == true` 且无上一份 → 折叠不画 ` · `、展开留空。
- 运行中的工具行只要特化内容就绪就照常用（不等完成）；`正在调用工具...`
  只出现在占位行与默认渲染的运行中行。
- `ClientToolRenderEntry::displayName` 与 `renderClientTool` 删除后，
  `message_list.cpp` 的 `queryToolRender` 要拆分并去掉对旧结果字段的引用。
