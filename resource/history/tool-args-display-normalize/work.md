# 实施记录 — 工具特化渲染前的参数自动修正

## 进度

- [x] 抽出共享实现 `util/tool_args_fix.{h,cpp}`，`ToolcallWrapNode::autoFixArgsType` 改薄包装
- [x] `ToolRegistry` 增加静态工具参数 schema 登记 + `parametersOf` 查询，`BaseAgent::init` 填表
- [x] `EventBridge` 建工具展示行时按 schema 自愈参数文本
- [x] `SessionsManager` 历史尾窗加载 / 分页读取时自愈工具行参数
- [x] `agentxx_filesystem` 渲染器简化（去掉 string/单对象兼容）
- [x] 测试与文档
- [x] debug 构建 + 相关测试模块回归

## 每步改了什么 / 怎么验证

1. **共享参数自愈实现**
   - 新增 `agent/lib/include/agentxx/util/tool_args_fix.h`、`agent/lib/src/util/tool_args_fix.cpp`：
     `autoFixToolArgsTypes(parameters, args, toolName)`（原 `nodes/toolcall.cpp` 里的
     `autoFixArgsType` 主体与全部 helper 搬过来）+ 文本级 `autoFixedToolArgsJson(parameters, argsJson)`
     （解析 → 修正 → 有变化才 `dump()`，否则原样返回）
   - `agent/lib/src/nodes/toolcall.cpp`：删除搬走的 helper，`autoFixArgsType(def, args)` 改为
     转 `def.parameters` 后调共享实现（签名不变，测试与调用点不动）；顺带清掉不再使用的
     `<cerrno>/<charconv>/<cmath>/<cstdlib>/<type_traits>`
   - `agent/lib/include/agentxx/nodes/toolcall.h`：长文档注释改为指向共享实现
   - 验证：`agentxx_test toolcall_args`（新增 #76/#77 覆盖共享函数与文本入口）

2. **工具参数 schema 查询**
   - `agent/lib/include/agentxx/plugin/tool_registry.h` + `src/plugins/tool_registry.cpp`：
     `setStaticToolSchemas()`（整体替换）、`parametersOf(name, out)`（静态表 → 插件工具定义，
     插件工具定义按名字缓存，`registerTool`/`unregisterTool` 时清缓存）
   - `agent/lib/src/agent/base_agent.cpp` 的 `bind_tools` 步骤：登记名字的同时登记
     `get_definition().parameters`（`fromNeographJson` 成业务 Json）
   - 注意点：`bind_tools` 必须在 `engine->own_tools` 之前收集（沿用原有约束）

3. **实时展示行**
   - `agent/lib/src/event/event_stream.cpp`：新增 `toolRowArgsText()`，
     `handleChannelWrite` 展开 assistant `tool_calls` 建 Tool 行时用它写 `m.text`
   - 未命中 schema / 无 registry / 参数非对象 → 原样（与改动前逐字节一致）
   - 验证：`agentxx_test event_bridge` 新增 `test_eventbridge_tool_args_normalize`
     （登记 schema 的工具被自愈、未登记工具的原文不动、落进 `viewWindow` 的也是自愈后文本）；
     原有 `test_eventbridge_channel_write_messages`（无 registry）仍是原文，语义未变

4. **历史（加载 / 分页）**
   - `agent/lib/include/agentxx/agent/context.h`：`SessionsManager::toolArgsNormalizer`
     （`std::function`，默认空）+ 私有 `normalizeToolViewArgs()`
   - `agent/lib/src/agent/context.cpp`：`normalizeToolViewArgsIn()` 只处理 `Role::Tool` 且有参数的行；
     `getOrCreate` / `getOrCreateAsync` 在 `restoreWindow` 之前过一遍，
     `makeSessionStoreHooks` 多收一个 normalizer 并作用在 `onLoadViewPage` 结果上
   - `BaseAgent::init` 注入回调（捕获 `shared_ptr<ToolRegistry>`，无悬垂风险）
   - 验证：`agentxx_test session_persistence` 新增 `testToolViewArgsNormalizeOnLoad`
     （尾窗恢复自愈 / 分页读出的更早行自愈 / 未注入回调时保持原文）

5. **渲染器简化**
   - `agent/plugins/agentxx_filesystem/agentxx_fs_plugin.h`：`stringListArg` 去掉 string 分支、
     `editSegmentsArg` 去掉 `edits` 单对象分支（保留 `old_str`/`new_str` 这种工具本身支持的形态）
   - `agent/lib/src/plugins/builtin_tool_renderers.cpp`：行为不变，文件头补"参数已按 schema 自愈"说明
   - 验证：`agentxx_test client_plugins`、`tui_tool_header`

6. **测试调整（旧断言反映的是被删掉的兼容行为）**
   - `agent/test/plugin/test_client_plugins.cpp`：15.3h 的"`edits` 单对象与数组渲染一致"改为只喂数组
     形态（并注明自愈由 agent 侧完成）；15b 的 glob/grep "单字符串形态" 改为"单元素数组形态"
   - `agent/test/client/test_tui_tool_header.cpp`：同上，删除 `edits` 单对象用例的等价断言

7. **文档**
   - `docs/zh-cn/design/index.md` 的"参数自愈"条目：说明执行前与展示侧各用一次同一份实现
   - `docs/zh-cn/design/plugins.md`：工具特化渲染架构第 1 条新增"参数已按工具 schema 自愈，
     渲染器不需要再写类型兼容"；DIFF 载荷"两种形态都认"的理由改为"界面描述层是公开契约"，
     并说明工具参数本身已自愈

## 验证清单

- 构建：`cmake --build agent/build/windows-debug --config Debug`（含 lib / client / test / 插件）0 错误
- 插件：`agentxx_lib_repo-build --target agentxx_filesystem` 成功
- 测试（`--log-level warn`，实测全绿）：
  - `toolcall_args` 212、`event_bridge` 223、`session_persistence` 735
  - `tui_tool_header` 168、`client_plugins` 747
  - 相关面：`agent` 225、`input_delivery` 134、`remote_agent` 602、`view_replica` 114、
    `storage_consistency` 260、`persist_semantics` 35、`toolcall_parallel` 58、`subagent_tool` 143、
    `assembly_snapshot` 38、`observability` 114、`tui_stream` 193、`tui_surface` 836、`tui_theme` 22、
    `event_delivery` 48、`async_tools` 90、`subagent_bus` 21、`boundaries` 19、`json` 197、
    `share_store` 63、`invisible_text` 35、`plugins` 609、`plugin_sdk` 115、`plugin_bridge` 193、
    `ui_items` 117、`ui_kit` 167、`tui_form` 84、`tui_ui_items` 277、`tui_widget` 194
- 未做：真机 TUI 手工观察旧会话摘要（依赖已保存在库里的老数据；逻辑与用例覆盖一致）

## 遇到的问题 / 注意

- `agent/lib/src/nodes/toolcall.cpp` 的批量删改用 PowerShell 改行区间时一度写坏（把新段插到文件头），
  已 `git checkout` 还原后重做；**改这类大段代码别用「先算行号、下一命令再用」的脚本**
  （变量不跨命令保留），要么一条脚本内完成，要么用精确字符串替换
- 仓库里 `agent/third_party/cxx_pluginxx_ui`、`fmt`、`libiconv-native`、`liburing` 的
  submodule 工作区状态与父仓库记录不一致，属本次改动之前就存在的状态，未触碰
- `ToolRegistry::parametersOf` 每取一次会拷一份 schema（几百字节到几 KB）；只在建工具行 /
  加载历史行时调用，热路径（每轮多次工具调用）可接受
- 自带 `ArgReader` 的宽容读取（third_party `cxx_pluginxx`）保留未动：它属于插件框架的通用工具，
  与"展示侧参数已自愈"不冲突
