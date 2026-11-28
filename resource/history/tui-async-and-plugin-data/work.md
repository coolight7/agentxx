# 实施记录: TUI 分页滚动屏蔽 / async 工具行 / 插件数据目录

- 任务卡片: [index.md](index.md)

## 进度

| 项 | 状态 |
|---|---|
| 1 TUI 分页时屏蔽滚动 | 已完成 (测试 `tui_scroll` 1039 通过) |
| 2 async 工具行内容 | 已完成 (测试 `tui_tool_header` 179 通过) |
| 3 插件数据目录规范 + 迁移 | 已完成 (见下; 已提交) |

## 1 TUI 分页时屏蔽滚动

### 问题与做法

页请求在飞期间用户可以继续滚轮; 这期间攒下的行数都是在"前插区高度尚未补齐"
(新增条目的高度还是粗略估算、锚点以上高度和还没算上它们) 的状态下攒的。
页响应一到, 前插 + 锚点平移落实, 这些行数再落实一次就跨过刚加载出来的整段,
表现是"加载出来前面的消息后立刻跳到新加载消息的开头"。

做法: 请求在飞期间屏蔽滚轮, 页响应处理完再放开。

- `LazyScrollable::setScrollSuspended(bool)` / `scrollSuspended()`
    - 暂停期间滚轮事件被吞掉 (`OnEvent` 返回 true, 不累积 `pendingScrollRows_`)
    - 进入/退出暂停都把 `pendingScrollRows_` 清零 (暂停前攒的行数按旧内容算,
      恢复后内容已变)
    - 只影响滚轮: `scrollToIndex` / `scrollToBottom` / 吸附底部不受影响
- `MessageListComponent::setScrollSuspended` / `isScrollSuspended` 转发;
  `maybeRequestMoreHistory` 在暂停期间直接返回 (不重复发起分页)
- `TUIClientAgentIO`:
    - `requestOlderHistory` 受理成功后 `suspendHistoryScroll()` (经 UI 动作队列下发)
    - `onViewMessagesPage` 所有出口 (接受/空页/迟到/不连续) 都 `resumeHistoryScroll()`;
      接受路径里恢复动作排在前插动作之后 (队列 FIFO), 同一帧渲染时两者都已落实
    - `onSync` 整体替换、`switchToSession` 也会放开 (并作废看门狗)
    - 看门狗 `kHistoryPageTimeout` (8s): 页响应丢失 (断线等) 时放开滚动并
      `HistoryWindow::abortLoading`, 免得列表一直不响应滚轮; 回调只持弱引用 +
      代次 (`historyPageSeq_`), 旧定时器不会动新请求
- 新增用例 `testScrollSuspensionDuringHistoryLoad` (`tui_scroll`):
  ① 屏遮期间滚轮被吞、视口逐行不变、恢复后可继续滚;
  ② 屏遮期间不发起分页预取, 恢复后照常预取;
  ③ 屏遮期间进来一页 (前插 3 条) 后视口内容不变、偏移等量下移, 恢复后上滑一行的
  位移不超过 2 行 (估算校正允许 1 行偏差)

### 验证

- `agentxx` (client, Debug) 编译通过
- `agentxx_test tui_scroll --log-level warn`: passed=1039 failed=0

## 2 async 系列工具的工具行内容

### 问题

`agentxx_async_*` 是 lib 内置工具, 但 `registerBuiltinToolRows` 只给
`agentxx_share_store` / `agentxx_subagent` 注册了显示名与折叠内容。没内容的工具行
在运行中只能画默认文案 (`正在调用工具...`), 而后台任务的参数 (哪个工具、哪个任务 id、
取消哪几条) 恰恰是用户最想看到的信息。

### 做法

`agent/lib/src/plugins/builtin_tool_renderers.cpp` 新增五个内置折叠内容 (与 share_store /
subagent 同一个注册表、同一条渲染路径; 插件内容仍可覆盖内置):

| 工具 | 显示名 | 折叠内容 |
|---|---|---|
| `agentxx_async_run` | `AsyncRun` | `<目标工具>` / `subagent <名字>`, 有 `delayMs` 补 `+2s`, 完成后用结果里的 `asyncId` 补 `(async-3)` |
| `agentxx_async_query` | `AsyncQuery` | `id=async-3` 或 `group=running` (缺省 `all`) |
| `agentxx_async_await` | `AsyncAwait` | `async-3 (30s)` (只有显式给 `timeoutMs` 时才画时长) |
| `agentxx_async_manager` | `AsyncManager` | `cancel async-3, async-5` / `delay async-3 +5s` (id 超过 3 个补 `, ...`) |
| `agentxx_async_emit` | `AsyncEmit` | `<事件名>` / `<事件名> (end)` |

- 参数还没到、解析失败、必要字段缺失 → 返回空串, 界面照旧走默认渲染
  (参数流式接收期间仍是占位行, 这是真实状态)
- 结果里的任务 id 只在 `finished` 时取 (受理成功才有); 失败行本来就走错误回退, 不冲突

### 验证

- `test_tui_tool_header` 新增后台任务族断言 (运行中/受理完成/子代理+延时/查询两种形态/
  等待/取消+改延时/发布+end), 含一条“运行中不再出现 `receiving tool call`”的回归断言;
  `tui_tool_header` passed=179 failed=0
- 测试注册表 (`makeTestToolRegistry`) 同步补上五条内置行内容

## 3 插件数据目录规范

### 问题

插件各自拼自己理解的落盘位置, 结果一插件一个样:

- `agentxx_planning` 写 `{data_dir}/plans/`
- `agentxx_codegraph` 写 `{data_dir}/sqlite/codegraph/` (与主程序的 `{data_dir}/sqlite/` 混在一起)
- `agentxx_screen_capture` 写 `{data_dir}/captures/`
- `agentxx_chatbot` 写 `{data_dir}/chatbot/media/`
- `http.download` 写 `{data_dir}/<插件 id>/data/downloads/`

一是没人知道某个子目录归谁, 二是插件卸载/重装要手工找残留, 三是"安装式插件"
以后要落盘时没有统一位置。做法: 由主程序算好每个插件的数据目录并交给插件,
插件不再自己拼 `data_dir`。

### 目录布局

```text
{data_dir}/plugins/data/{插件 id}/     插件自己的持久化 (缓存/索引/截图/媒体/下载/状态)
{data_dir}/plugins/installed/          安装式插件落点 (预留, 当前没有写入方)
```

`data_dir` 为空 (没配持久化) 时一律返回空串 —— 插件据此降级为不落盘, **不**回退
`~/.agentxx` (与 `AgentConfig::dataDir` "空 = 不落盘" 的语义一致)。

> 与用户原话的差异: 原话说安装目录是 `{data_dir}/plugin/installed/` (单数)。实现
> 统一成复数 `plugins/installed/`, 让"插件数据根"与"插件安装根"同在一个 `plugins/`
> 段下 (`plugins/data` + `plugins/installed`), 避免单复数混用。只改一处常量即可换回。

### 唯一实现 (主程序侧)

新增 `agent/lib/include/agentxx/plugin/plugin_paths.h` + `agent/lib/src/plugins/plugin_paths.cpp`:

| 函数 | 作用 |
|---|---|
| `pluginDataRoot(dataDir)` | `{data_dir}/plugins/data` |
| `pluginInstallRoot(dataDir)` | `{data_dir}/plugins/installed` (预留) |
| `sanitizePluginId(id)` | 插件 id → 目录名 (安全化) |
| `pluginDataDirOf(dataDir, id)` | `{data_dir}/plugins/data/{目录名}` |
| `ensurePluginDataDir(dataDir, id, error)` | 上面那个路径 + 建目录 (幂等) |

- 安全化只保留 `[A-Za-z0-9._-]`, 其它字节换成 `_`; 空 id 给 `_`; 全点号 (`.` / `..`)
  加前缀 `_`。**被改过名的 id 追加原 id 的 8 位 FNV1a 短哈希** —— 保留可读前缀的同时
  让 `a/b` 与 `a_b` (两个不同插件) 不落进同一目录; 合法 id 的目录名不变
  (`agentxx_chatbot` 仍是 `agentxx_chatbot`)
- 路径输出统一 UTF-8 + 正斜杠 (经 `utilxx_base::pathToUtf8Generic`), 插件直接拿去
  拼 `std::filesystem::path` 或写日志都不会出现反斜杠/本地代码页问题

### 交给插件的两条路

1. **`get_own_info` 的 `dataDir` 字段** (插件拿自己的)
   - agent 侧: `agentxx.plugins.get_own_info` 与 `get_plugin` (都经
     `PluginManager::getPluginJson`), 实例加载时 (`createInstance`) 就解析并建好目录
     (`PluginInstance::dataDir` + `dataDirResolved` 缓存, 只解析一次)
   - client 侧: `agentxx.client.self.get_own_info` (`getOwnInfoJson`),
     `ClientPluginInstance::dataDir` 同样在 `createInstance` 解析
   - SDK: `PluginBase::dataDirAsync()` / `ClientPluginBase::dataDirAsync()` (协程),
     共用解析函数 `dataDirOfOwnInfo(json)`
2. **进程级配置** (`agentxx.config.get_config` / client 的 config 表):
   追加 `dataDir` / `pluginDataRoot` / `pluginInstallRoot`, 给"要看整体布局"的场景
   (client 侧此前没有 `dataDir`, 这次 `ClientPluginManager::setDataDir` 由
   `mode_runners.cpp` 的 `setupClientPlugins` 从 `AgentConfig::dataDir` 传入)

`http.download` 的落盘根也换成插件数据根 (`PluginNetService::create(hooks,
pluginDataRoot(dataDir))` + NetHub 自拼属主 id), 于是下载落在
`{data_dir}/plugins/data/{插件 id}/data/downloads/`。

### 插件迁移

| 插件 | 旧位置 | 新位置 |
|---|---|---|
| `agentxx_planning` | `{data_dir}/plans/` | `<插件数据目录>/plans/` |
| `agentxx_codegraph` | `{data_dir}/sqlite/codegraph/` | `<插件数据目录>/sqlite/codegraph/` |
| `agentxx_screen_capture` | `{data_dir}/captures/` | `<插件数据目录>/captures/` |
| `agentxx_chatbot` | `{data_dir}/chatbot/media/{账号}/` | `<插件数据目录>/media/{账号}/` |
| `http.download` | `{data_dir}/<插件 id>/data/downloads/` | `<插件数据目录>/data/downloads/` |

四个插件原来都自己从 `get_config` 的 `dataDir` 里读主程序数据根再拼子目录,
现在统一 `co_await ctx.dataDirAsync()` 一次缓存 (coding 顺序注意: chatbot 里这个值
必须在填实例 `config` 成员之前取, 成员名会遮蔽基类方法)。

### 验证

- `plugin_runtime` 新增 `plugin_paths` 断言块 (拼接/安全化/建目录幂等/非法 id 不逃出
  数据根): passed=844 failed=0
- `agentxx` (client Debug) 编译通过; 四个插件 DLL 重编通过
- 回归: `plugin_sdk` 142 / `plugin_host_mount` 147 / `client_plugins` 945 /
  `boundaries` 20 / `config_keys` 220 / `codegraph` 28 / `screen_capture` 53 /
  `chatbot_media_io` 60 / `plugin_example_chatbot` 268 全绿;
  `tui_scroll` 1039 / `tui_tool_header` 179 仍绿

### 顺带

- `client_plugins` 里"内置工具行条数"的四处断言从 2 改成 7 (任务 2 加了 5 条后台任务
  工具行, 当时只跑了 `tui_tool_header`); 该模块现在 945 通过
- `plugin_broadcast` 有一个**既有的**偶发失败 (约 1/10): 用例在 `publish_broadcast`
  表调用返回后立刻断言订阅回调的计数, 而订阅回调是排队到插件执行线程执行的 ——
  插件线程慢一点就还没跑。与本次改动无关 (该路径没有被改动), 已单独修掉:
  `SubCounter` 改成带锁的记录器 (回调在插件线程写、断言在本线程读, 原本是数据竞争),
  断言前一律 `co_await waitSubValues/waitSubEnds` / “不应发生”的用 `settlePluginCallbacks()`
  等一小段再断言; 20 次顺序 + 4路×3轮并行 + CPU 占满下 6 次全部通过 (断言数 102 → 106)
