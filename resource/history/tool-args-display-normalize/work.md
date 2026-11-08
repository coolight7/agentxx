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

## 后续调整（同日，按 review 意见）

1. **布尔字符串忽略首尾空白与大小写**（`agentxx::util::autoFixToolArgsTypes`）
   - 新建 helper `parseBoolText`（`agent/lib/src/util/tool_args_fix.cpp`）：只认
     `true`/`false`/`1`/`0`（忽略 ASCII 大小写与首尾空白），其它文本保持原样
   - 原因：以前只认精确的 `"true"`/`"false"`，模型写 `"True"` 时自愈不动，插件侧
     `ArgReader::get<bool>` 又会静默当成 `false`
   - 测试：`toolcall_args` 新增 #17b（True / " FALSE " / "1" / "0" / "ok" / 已是布尔）；
     文档同步（`docs/zh-cn/design/index.md` 参数自愈条目）
2. **`ArgReader` 字符串转数值改成严格解析**（`agent/third_party/cxx_pluginxx/include/pluginxx/kit/kit.h`）
   - 新增 `detail::parseWholeInteger` / `parseWholeDouble` / `parseBoolText` / `fitsInType`：
     字符串转整数/浮点要求**整个**字符串（允许首尾空白与一个前导 `+`）都是数值，
     `"100 lines"` 读不到而不是得 100；整数还要求落在目标类型范围内（不再回绕截断）；
     浮点拒绝 `0x` 写法与 inf/nan；字符串转布尔同样忽略大小写只认 true/false/1/0，
     读不到时返回 `nullopt`（`require` 报参数错误），不再静默 false
   - `get<std::string>` 的"非字符串按 `dump()` 返回"保留不变（用户确认第 3 点暂不修改）
   - 测试：`plugin_sdk` 新增 ArgReader 用例；`filesystem` 的 `editSegmentsArg` 单对象用例
     改为"先过自愈再渲染"（与客户端实际拿到的形态一致）
   - 注意：改的是子模块 `agent/third_party/cxx_pluginxx` 的**源码**，父仓库会显示该 submodule
     内容有修改；该库按独立仓库维护，需要在库仓库里另行提交。顶层构建的
     `cxx_pluginxx_repo` 带 `BUILD_ALWAYS`/`INSTALL_ALWAYS`，头文件会自动重新安装，
     依赖它的 lib / 插件 / 测试会重编（本次实测 `agentxx_filesystem` 重新编译并链接）
3. 验证：debug 构建 0 错误；`boundaries` 19、`toolcall_args` 224、`plugin_sdk` 133、
   `filesystem` 222、`plugins` 609、`event_bridge` 223、`client_plugins` 747 全绿；
   全量套件（`--log-level error`）除本次已修复的两个用例外无其它失败

## 后续调整 2（同日，按第二轮 review 意见）

1. **参数自愈不分工具归属**：确认前台工具调用 (`prepareToolInvocation` →
   `ToolcallWrapNode::autoFixArgsType`) 对内置/中间件/插件工具一视同仁（schema 取自
   `tool->get_definition()`）；补上唯一漏掉的入口 —— 后台工具任务
   `agent/lib/src/agent/async_work.cpp` 的 `runToolWork` 在执行体前也调
   `util::autoFixToolArgsTypes`（以前直接 `execute_async(spec.toolArgs)`，模型给的
   宽松参数原样进工具）
2. **布尔认词扩到常见肯定/否定词**（两个实现同步：`tool_args_fix.cpp` 的
   `parseBoolText` 与 kit.h 的 `detail::parseBoolText`）：
   真 = `true`/`yes`/`y`/`on`/`enable`/`enabled`/`1`，假 = `false`/`no`/`n`/`off`/`disable`/`disabled`/`0`
   （忽略首尾空白与大小写；`ok`/`maybe` 之类仍保持原样）
3. **浮点转整数改为“相近整数”取整**（`doubleToInt64Rounded` / kit.h 的
   `doubleToWholeInteger`）：与向上取整相差不超过 1e-6 时向上取整，否则向下取整
   —— `3.7` → 3、`4.999999999` → 5、`-3.7` → -4；越界与 NaN/Inf 保持原样。
   字符串 → integer 也走同一规则（`"3.5"` → 3、`"49.9999999"` → 50，
   `"3.5 files"` 仍不转）；数值收紧（整串才能转）保持不变
4. **ArgReader 跟着对齐**：字符串→整数的严格整串规则不变；JSON 浮点→整数改为同一套
   取整；整数目标补 `fitsInType` 范围检查（以前 `is_number_integer` 分支直接
   `static_cast` 会回绕）；`get<std::string>` 的 `dump()` 仍未改（用户确认第 3 点不动）
5. 验证：debug 构建 0 错误（全插件重编，因 kit.h 是 header-only）；
   `toolcall_args` 256、`plugin_sdk` 141、`async_tools` 99（新增
   `test_async_tool_args_normalize`：后台工具任务收到的 `"5"`/`123`/`"no"` 已按 schema
   改成 5/"123"/false）、`event_bridge` 223；全量套件（`--log-level error`）
   **passed=42651 failed=0**

## 注意事项（补充）
- `ArgReader` 与自愈各管一段：自愈按 tool schema 在**调用前 / 写展示历史前**改写参数，
  `ArgReader` 只负责按调用方要的类型读取（数值/布尔的严格规则见上）；
  `get<std::string>` 对非字符串仍 `dump()`（用户确认暂不改）

## 文档同步（同日）
- `docs/zh-cn/design/plugins.md`：`ArgReader` 条目由“宽容解析 / 智能自愈”改为“只做读取 +
  整串数值 + 布尔忽略大小写只认 true/false/1/0 + 参数形态修正归主程序
  (`util::autoFixToolArgsTypes`)”，`blocking_tool` 示例里的注释同步
- `docs/en/design/plugins.md`：同上（`Tolerant Parameter Reader` → `Typed Parameter Reader`），另补
  “参数已按 schema 自愈、渲染器不写类型兼容”一条，以及 DIFF 载荷两种形态都认的新理由
- `docs/zh-cn/design/index.md`：SDK 条目由“宽容类型解析与参数智能自愈”改为“强类型参数读取”
- `docs/en/design/index.md`：SDK 条目与 `Argument Self-Healing` 条目同步（补布尔大小写、
  单对象↔对象数组、展示侧同一份实现）
- `docs/zh-cn/develop.md` / `docs/en/develop.md`：速览条目里的“智能自愈 / error-tolerant”去掉
- 校验：`boundaries` 19 passed（含自研代码与文档的用词检查）
