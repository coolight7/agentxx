# 工具与测试的使用体验修正 (tool-ux-fixes) 实施记录

> 来源: 实际使用本 agent 跑任务后自己记下的 6 条问题 (搜索提示误导、测试输出噪声、
> 命令工具缺 `cwd`、插件编译要手敲内层目标、CodeGraph 空结果没有新鲜度提示等)。
> 本文件记录**改了哪些、为什么这么改、验证结果与踩过的坑**。

## 总体进度

| 项 | 内容 | 状态 |
|---|---|---|
| 1 | `grep` 空结果不再算失败 + 文案说清"路径命中但内容没命中" | 已完成 |
| 2 | `agentxx_test --log-level` + 日志前缀统一为 `[Warn]` / `[Error]` | 已完成 |
| 3 | 命令工具新增 `cwd` 参数 + 多行文本写法提示词 | 已完成 |
| 4 | 新增 `script/build_plugins.{ps1,sh}` + `AGENTS.md` 插件编译说明 | 已完成 |
| 5 | CodeGraph 工具结果附索引新鲜度, 空结果提示可能过期 | 已完成 |
| 6 | (确认后不动) `filesystem_read` 上限报错已含提示 / grep offload 阈值 / `[ExitCode]` 包装 | 已确认 |

## 1. grep 空结果不再算失败

`agent/plugins/agentxx_filesystem/filesystem_impl.h`:

- `file_patterns` 没展开出任何文件: 由 `throw std::runtime_error` 改为
  `detail::okText("No match `file_patterns`: the patterns expanded to no file, nothing was searched ...")`,
  并区分两种情况加括号说明 —— 到 `max_files` 提前停止 / 没停止时提示 `exclude_patterns`
  与权限规则也会过滤候选
- 文件扫过但内容没命中: 同样改为 `Ok`, 正文改成
  `No match: N file(s) matched `file_patterns` and were scanned, but none contained the <模式>`
  (模式按本次实际传的 `text_patterns` / `regex_patterns` 说成 "text pattern(s)" /
  "regular expression(s)" / 两者并集), 并删掉原来的 `Active modes: ...` 尾巴;
  正则模式下追加"检查未转义的元字符 / 改用 `text_patterns`"的提示;
  仍保留"跳过了 N 个超大文件"与"遍历因 `max_files` 提前停止"两段说明
- 为什么这不是"忽略错误": grep 本身的退出码 1 就表示"没找着", 不是故障; 之前
  `[Error: ...]` 会让模型以为工具坏了, 也让它误判"路径都不存在"。真正的参数错误
  (含正则编译失败) 仍然 `Failed`; `agentxx_filesystem_list` 的通配路径无匹配**仍按
  失败处理** (请求的那个路径确实不存在, 与"搜了一圈没搜到"是两件事)

用例: `test_filesystem_tools.cpp` 的 `test_scan_scale_limits` 里 ⑥ 号断言原来要求
"无匹配文本里出现 `[Error:`", 改为要求出现 `max_files` + `stopped early` 且**不含**
`[Error`; `test_grep_no_match_fail_fast` 仍按 `No match` 前缀判定, 无需改动。

## 2. `agentxx_test --log-level`

- `agent/test/include/agentxx-test/test_framework.h`: 新增 `g_logLevel` 与 `logEnabled()`,
  直接复用 `utilxx_base::LogLevel` (Trace/Debug/Info/Warn/Error) —— 测试侧输出与库侧
  `XX_LOGW` / `XX_LOGE` 用同一套级别, 只有一份含义。`passStream` / `infoStream` /
  `skipStream` 按 `Info` 过滤, `warnStream` 输出 `[Warn] ` 且按 `Warn` 过滤,
  `failStream` **始终输出** (失败不能被过滤掉)。被过滤的输出写进一个丢弃用的
  `std::streambuf` (`g_nullStream`), 调用点的 `TEST_INFO << ... << std::endl;` 写法不变
- `agent/test/test.cpp`: 解析 `--log-level <level>` 与 `--log-level=<level>`
  (`debug|info|warn|error`, 非法值打印提示并 `return 2`); 原来的 `TestWarnErrorLogSink`
  输出 `[lib:W]` / `[lib:E]`, 改成 `[Warn] ` / `[Error] ` 并受同一级别控制
  —— 这两个前缀就是以前测试输出里那些看不懂的行的来源 (库侧 warn/error 日志)
- `AGENTS.md`: 测试运行示例补 `--log-level warn`, 并写明只保留警告与错误时输出量小很多、
  不影响定位失败

实测: `agentxx_test string_util --log-level=error` 只打印段落标题与汇总; 默认级别
照旧逐条 `[PASS]` / `[INFO]`。

## 3. 命令工具 `cwd` 参数

`agent/plugins/agentxx_execute_command/`:

- `execute_command_env.h`: 新增 `kCwdArgDesc`, 并加进 `bashToolPrompt` / `windowsToolPrompt`
  的 `args` 表 (插件默认提示词与 `agentxx.agent.prompt` 注入共用一套文案)
- `execute_command_impl.h`: 新增 `detail::resolveCwdArg(arguments, workDir)` —— 未给时返回
  会话工作目录; 相对路径按会话工作目录展开; **目录必须已存在**, 否则抛
  `std::invalid_argument` (插件边界把它变成失败结果, 不会带着错目录启子进程再报一句
  看不懂的失败)。boost.process 路径用它作 `process_start_dir` 与 `PWD` 预设;
  popen 回退路径用 `cd /d "..."&& ` (Windows) / `cd '...' && ` (POSIX) 前缀, 且 `PWD` 跟着走
- `agentxx_execute_command.cpp`: 两个工具的 schema 都加 `.string("cwd", ..., false)`
- 提示词另补多行文本写法: PowerShell 用 Here-String (`@'` ... `'@`, 结尾标记必须顶格)、
  cmd.exe 没有 heredoc 也没有 Here-String (多行内容先落文件再执行); WSL 转发那条同样说明
  "外层 Linux shell 有 heredoc, 但交给 `-Command` 的 PowerShell 代码里要用 Here-String"
- **未动**输出限制与编码处理 (按用户要求: 输出没必要限制、编码问题不建议动)

用例: `test_command_tools.cpp` 新增 `test_exec_cwd_param` —— ① 不存在的 `cwd` 必须抛
`invalid_argument` 且文本含 `` `cwd` ``; ② 存在的 `cwd` 下执行 `Get-ChildItem -Name` /
`ls`, 要能看到只存在于该目录的 marker 文件 (证明真的切了目录)。

## 4. `build_plugins` 脚本

`plugins/` 是 lib 的子工程 (`add_subdirectory`), 顶层 `cmake --build <build>` 只编 `.obj`,
不产出 `exec/plugins/<插件名>/*.dll`。新增两个脚本把"点名内层目标"这条命令固定下来:

- `agent/script/build_plugins.ps1`: 自动定位
  `<build>/agentxx_lib_repo-prefix/src/agentxx_lib_repo-build`, 按
  `agentxx_plugins_builtin/<名字>/<名字>.vcxproj` 列出可编目标 (`-List`), 支持
  `-Config` / `-BuildDir` / `-Jobs` 与位置参数式目标名; 编完打印 `exec/plugins/<插件名>`
  下最新 DLL 的时间戳 (确认产物真被替换, 这一步最容易漏)
- `agent/script/build_plugins.sh`: 同样的流程, 目标表用
  `cmake --build <lib 构建目录> --target help` 探测 (Ninja / Makefiles 都适用),
  支持 `--build-dir` / `--config` / `--jobs` / `--list`
- `AGENTS.md`: 编译节新增"插件动态库不随顶层 ALL 构建产出"一段 + 两个脚本的用法

实测: `build_plugins.ps1 -List` 正常列出; `build_plugins.ps1 agentxx_filesystem
agentxx_execute_command agentxx_codegraph` 编出三个 DLL, 时间戳随之更新。
`build_plugins.sh` 只做了 `bash -n` 语法检查 (当前机器为 Windows)。

## 5. CodeGraph 结果带索引新鲜度

- `codegraph_manager.{h,cpp}`: 新增 `CodeGraphIndexInfo{available, indexing, updatedAtMs,
  files, symbols}` 与 `CodeGraphManager::getIndexInfo()`; `initialize()` 记下 `index_path_`,
  用索引库文件的 `last_write_time` 换算 "最近一次写入 ≈ 最近一次索引完成", 计数取
  `db_->get_all_files().size()` + `db_->count_nodes()`; 统计值 5 秒内走缓存
  (逐次统计对大索引库不划算, 新鲜度不需要实时)
- `agentxx_codegraph.cpp`: 新增 `humanAge()` / `formatLocalTimeMs()` / `indexFooter()`,
  `contextToText` / `impactToText` / `search` / `path` 四处统一调用它, 结果末尾给出
  `[index] N file(s) / M symbol(s), built <本地时间> (<多久以前>)`;
  结果为空时追加"索引可能是旧的, 用 `agentxx_codegraph_index` 重建或检查名字拼写";
  索引尚未建好 / 正在索引时给出对应警告

目的: 以前 callers/callees 结果为空时, 模型无法区分"代码里真的没有"与"索引没扫到"。

## 6. 确认后不动的三处

- `filesystem_read` 超过 2000 行 / 256 KiB 的报错**本来就**已带提示
  (用 `offset`/`limit` 分页、用 `agentxx_filesystem_grep` 定位、单行过长改用命令工具切),
  无需再改
- grep 结果被整体 offload 到 share store 的阈值与实现: 需要单独设计, 本次不动
- 命令工具输出里的 `[ExitCode]` / `[StdOut]` 包装: 按用户意见不动

## 验证

- 编译: `agentxx_test_repo` (Debug) 通过; 三个插件 DLL 经 `build_plugins.ps1` 重编
- 全量回归 (`agentxx_test --log-level warn`): **passed=41246 / failed=1**, 唯一的失败是
  `boundaries` 规则 15 (子模块 `agent/third_party/cxx_pluginxx_ui` 里 5 处 `载体`),
  与本次改动无关 (既存)
- 相关模块单独确认: `filesystem` 221/0、`command` 63/0、`codegraph` 28/0、
  `math_tools` 82/0、`toolcall_parallel` 58/0

## 踩过的坑

- **`agentxx::test::LogLevel` 会和测试文件里的 `using utilxx_base::LogLevel;` 冲突**
  (MSVC C2872 "不明确的符号", 因为两者在同一作用域): 不要在 `test_framework.h` 另立
  同名枚举, 直接复用 `utilxx_base::LogLevel`
- 改插件源码后只跑顶层 `cmake --build` 不会更新 `exec/plugins` 里的 DLL (见第 4 项),
  否则运行期会报"插件 API 版本过低"或加载被拒
- 用 `git diff` 复查时要留意**无意义的空白改动**: 这次 `agentxx_codegraph.cpp` 里
  `blocking_tool(` 的参数缩进被顺手改错一位、`codegraph_manager.cpp` 里一个空行与注释
  被并到上一行, 都在复查时改回来了 (`git diff -w` 对照确认剩下的都是真实改动)
