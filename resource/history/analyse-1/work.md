# analyse-1 实施记录

> 依据: 同目录 [analyse.md](analyse.md) 的 `[问题]` / `[不一致]` / `[可优化]` 清单,
> 结合其中逐条"人工核定"的结论实施。
> 状态: `已完成` / `进行中` / `待开始` / `不做`(附核定理由)。

## 0. 人工核定结论 (实施约束)

| 条目 | 核定 |
|---|---|
| §2.1 `hasError` | toolcall 拦截异常、插件明确报错时置 `true`；插件跨 C ABI 无异常，需插件侧返回字段指示错误；**移除**按 `"[Error:"` 文本判断的做法 |
| §2.2 重放缓冲字节上限 | 计算字节占用麻烦、大占用情况少 → **移除字节限制的说法**（文档改掉、删掉无调用者的估算函数），在 roadmap 记一条"未来如有需要再实现" |
| §2.5 `fromJson` 注释 | 统一注释与实现：有专属子结构的 role 一律保证非空，没有子结构的 role 就是没有 |
| §7.1 `saveStateItem` | 预留接口、当前无引用 → 暂时不管 |
| §8.1 MCP stdio | **补充实现支持配置**（`mcp.list[].command` / `args`） |
| §8.2 `McpServer` / `A2aServer` | 暂时不管，写入 roadmap（后续把 cli 能力同步到 mcp/a2a，并提供插件注册导出） |
| §9 训练系统 | 整块仍在开发中 → 暂时不管（存档原子写等记入 roadmap） |

## 1. 任务状态

### A. 红灯与低风险修正

| 任务 | 状态 | 说明 |
|---|---|---|
| §1.1 `boundaries` 两处停用词 | 已完成 | `plugin_kit.h` 宿主→主程序；`test_plugins.cpp` 链路→流程 |
| §6.1 `console_state` 用例 5 | 已完成 | 基线改用"当前模式"（用例 4 已补回 VT 位，写回原始模式会把它清掉） |
| §2.6 `SessionUsageTotals` 注释自相矛盾 | 已完成 | 改为"本结构不单独落库，由 usage 表聚合恢复" |
| §2.7 `agentxx::node::` 命名空间笔误 | 已完成 | `config.h` 两处改为 `agentxx::nodes::` |
| §2.8 `baseRun` 缺 `agentContext.lock()` 判空 | 已完成 | 补 `!agentCtxPtr || !middlewareHandleContext` 早退 |
| §2.9 `messagesCount` / `messagesVersion` 线程约束 | 已完成 | `messagesVersion` 补 `assertIoThread()`（唯一调用方在 io 线程）；`messagesCount` 明确注释"可任意线程读近似值"（插件接口表在非 io 线程查询） |
| §2.5 `ViewMessage::fromJson` 注释与实现 | 已完成 | `Role::Think` 一律构造子结构；两处注释改为"有子结构的 role 保证非空" |
| §4.1 `agentxx_llm_uncensored` 描述不符 | 已完成 | 文件头注释、`kDefaultText` 注释、`PluginxxInfo.description`、`plugin.yaml`、`plugins.md`、仓库 `agentxx-config.yaml` 注释统一到"默认文本要求模型忽略内容安全策略" |
| §1.2 / §3.1 `index.md` 接口表数量 | 已完成 | 20 → 21，业务表清单补 `mount` |
| §1.5 `index.md` wire 消息清单 | 已完成 | 改为"以 `wire-protocol-fields.md` 为准"+ 典型举例；补 `host_tool_*` 功能段与 add/remove model、rename session |
| §5.1 `index.md` Tip 前缀 `#` | 已完成 | 去掉 `#` |
| §1.4 `index.md` / en 文档插件清单与工具数量 | 已完成 | websearch 2 工具；filesystem 9 工具；补 3 个插件条目 |
| §3.4 `plugins.md` / en 文档 `fetch_markdown` | 已完成 | 统一为 `web_fetch`（format=markdown\|raw） |
| §3.2 `security.md` 权限声明工具清单 | 已完成 | 列全 9 个工具与作用域/目标参数（含 glob/grep 的 `file_patterns`、move/copy 的 `src`+`dst` 复核） |
| §3.3 `roadmap.md` STO-12b 与 RET-1a 矛盾 | 已完成 | RET-1a 同步为"检索入口已移除" |
| §6.2 `test.cpp` 重复 include | 已完成 | 删掉尾部重复的 `test_util_misc.h` |

### B. `WireDelta::hasError` 真实错误标记

| 任务 | 状态 | 说明 |
|---|---|---|
| 工具错误字段贯通生产端 | 已完成 | 工具执行层写 `ChatMessage::extra["hasError"]`（异常 / 工具不存在 / 参数非法 / 权限拒绝 / 重复调用被拒 / 中间件异常中止）；插件工具 FAILED 终态经 SDK → `std::runtime_error` → 同一异常路径；工具自己按历史约定返回的失败文本在 `toolResultTextIsFailure`（toolcall.cpp，唯一判定处）认出来 |
| 下发与持久化 | 已完成 | EventBridge 读 `extra.hasError` → `ViewMessage::ToolData::hasError`（JSON `has_error`，随会话库持久化）+ `WireDelta.hasError` |
| CLI / TUI 改用 `hasError` | 已完成 | TUI 实时 delta 与历史消息都改看 `tool->hasError`（仅 `[Interrupt]` 占位仍按警告色）；CLI 原本就用 `delta.hasError`，现在是真值 |
| 回归测试 | 已完成 | `tui_stream` 场景 13（成功/失败两条 delta → 消息标记）+ `agent` 的中间件异常中止用例断言 `extra.hasError` |

### C. MCP stdio 配置入口 + 三处实现隐患

| 任务 | 状态 | 说明 |
|---|---|---|
| `mcp.list[].command` / `args` 配置字段 | 已完成 | `McpServerConfig` 新增 `command` / `args` + `isStdio()`；`config_loader.cpp` 解析（与 `url` 二选一，都填时 url 胜并告警；`args` 非列表时告警忽略）；`code_agent.cpp` / `resource_applier.cpp` 组装 `serverCommand` |
| 配置文档与生成物 | 已完成 | `config_keys` 新增两条键（归属=服务端），`AGENTXX_UPDATE_CONFIG_KEYS=1` 重生 `config-keys.md` / `config-keys.json` |
| 回归测试 | 已完成 | `mcp` 模块新增 `client_stdio_subprocess`（把 `agentxx_test --mcp-stdio-server` 当子进程，完整跑 initialize → listTools → 调用 echo 工具 → close）与 `client_stdio_bad_command`（命令不存在时初始化失败不崩） |
| §8.3 三处隐患 | 已完成 | Windows 回退路径改为 `CreateProcessW` + 参数逐个引号转义（UTF-8 → UTF-16）；POSIX 第二个 `pipe()` 失败时关掉已开的两个 fd；`_SC_OPEN_MAX` 失败时用固定上界 1024 |

### D. 性能与内存

| 任务 | 状态 | 说明 |
|---|---|---|
| §2.2 移除字节上限的说法与死函数 | 已完成 | 删 `estimateWireDeltaBytes` / `estimateViewMessageBytes`（无调用者）；文档与 roadmap 已同步 |
| §2.3 中断路径深拷贝整份上下文 JSON | 已完成 | `throwNodeInterruptBase` / `requestInterrupt` 去掉 `msgs` 参数，四个调用点同步；toolcall 中断分支不再序列化整份上下文 |
| §2.4 每次 modelcall 3 次全量深拷贝 | 已完成 | ① system 消息改走新增的 `Session::setSystemMessage`（就地改，内容不变则不动版本号）；② `repairMessages` 前加只读体检 `repairNeeded` + 健康时只走 `updateMessageCheckInfo`；③ `summarization` 常规路径用 `const&`，进入压缩分支才拷一份 |

### E. 自检体系改进

| 任务 | 状态 | 说明 |
|---|---|---|
| §10.1 规则编号重号/顺序 | 已完成 | 新增 `kRuleTable`(15 条, 编号只由表顺序决定) + `ruleLabel()`, 报告标签带编号; 检查函数注释与编号对齐; 重复的"规则 13"消失; 两条导出规则拆成两个 violation 列表 |
| §10.2 停用词"一行两禁词就放过" | 已完成 | 豁免只保留两条明确规则: 行内 `term-table` 标记; 词表文件白名单(运行时系统提示词)且仅对含两个以上禁词的行 |
| §10.3 `内核` 10 条短语白名单 | 已完成 | 改为"后面紧跟技术名词"判定(另允许"操作系统内核"前缀); 改掉 5 处直说内核却写成"内核..."的注释; docs 里指 reactor 的"内核就绪事件"改为"I/O 就绪通知" |
| §10.4 规则 8 纳入 `index.md` | 已完成 | 新规则 7 同时校验 `plugins.md` / `index.md` / `AGENTS.md` 的两侧数量 |
| §10.5 `校验` 词表冲突 | 已完成 | 用词检查不再靠"一行多词"计数豁免; `校验` 仍作为 `门禁` 的推荐用词 |
| §1.3 / §6.3 测试模块清单改为生成物 | 已完成 | 新模块 `test_modules` 从 `test.cpp` 生成 `docs/zh-cn/design/test-modules.md`(127 个模块 = 同步 67 + 异步 60, 其中 27 个仅 client), 逐字节比对; `index.md` 的手写清单换成链接 + 常用举例 |

### F. 验证

| 任务 | 状态 | 说明 |
|---|---|---|
| debug 编译 | 已完成 | `agent\script\windows_debug_build.bat` 通过 (无 error C/LNK)；Release/其它平台未跑 |
| 跑全量测试模块 | 已完成 | `agentxx_test.exe` 全量: `Total: passed=38968 failed=0` (127 个模块, 无失败) |

### G. 实施过程中另外发现的既有缺陷 (已修)

| 位置 | 状态 | 说明 |
|---|---|---|
| `test_input_delivery.cpp` `contextHasText()` | 已完成 | 原本搜全部消息, 会误命中系统提示词里的 `[Content offloaded. ...]` 示例文本 → 改为跳过 `role == "system"` |
| `test_screen_capture.cpp` / `test_text_selection_monitor.cpp` | 已完成 | 插件工具只在 `mount` 回调里注册, 用例只 `load` 未 `mount`, 工具始终缺失 → 补 `mountAgentAsync` |
| `test_tui_tool_header.cpp` 夹具 | 已完成 | `hasError` 改造后界面只看 `tool->hasError`, 而夹具只填结果文本 → 12 条断言失败。夹具新增 `hasError` 参数与 `pushFailedTool()`, 失败用例改用它; 并补反向用例 (文本带 `[Error]` 但 `hasError=false` → 不加 ` · ` 失败后缀、不用错误色) |

### H. 未处理项 (已写入 roadmap)

| 条目 | roadmap 编号 | 说明 |
|---|---|---|
| 重放缓冲的字节上限 | PRO-13 | 当前只有条数上限; 未来确需字节上限再实现估算 |
| `McpServer` / `A2aServer` 与 cli 能力对齐 | PRO-14 | 后续把 cli 能力同步到 mcp/a2a, 并提供插件注册导出 |
| 训练系统存档原子写等 | TRN-1 | 整块仍在开发中, 稳定后再改 |
| `saveStateItem` 跨进程持久化 | — | 预留接口, 当前无引用; 注释已写明"当前不做跨进程持久化" |

## 2. 注意事项

- `resource/history/**` 内的既有历史记录不改（本目录除外）。
- 只跑 debug 构建；不手动 `clang-format`。
- 注释用词遵守 `boundaries` 停用词表（写"主程序/流程/扩展点"，不写"宿主/链路/埋点"）。
- 界面不再按结果文本判错：新增/修改客户端用例时，模拟失败的工具消息必须显式给
  `hasError`（`ToolHeaderFixture::pushFailedTool`），只写 `[Error] ...` 文本不会变红。
- `git add` 一律给显式路径：工作区里 `TODOS.md` 是用户自己的改动，
  `agent/third_party/*` 是子模块指针差异，都不应随本任务提交
  （注：`0d11357f` 曾把 `TODOS.md` 的改动一并带上，后续提交已避开）。
