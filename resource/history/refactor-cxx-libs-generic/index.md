- 难度: B
- 类型: 重构 (通用库去项目特化)
- 基于commit: cbfa9004
- 时间: 2026-10-26
- 需求:
```md
cxx_* 依赖库是我们开发的用于多个不同项目的通用库，请检查里面是否包含了 agentxx 的
特化内容，尽量将从中剥离出来在 agentxx 这边实现，避免污染其他项目
```

## 盘点 (三个库里的 agentxx 专有内容)

### 行为层面的特化 (不只是注释)

| 位置 | 内容 | 处理 |
|---|---|---|
| `cxx_utilxx/include/utilxx/worktree.h` (整个头, 26 KB) | git 工作树封装: `.agentxx/agent/worktrees`、分支前缀 `agentxx/wt-`、`.git/info/exclude` 写 `# agentxx worktrees (auto)`, 注释直接写"为 `agentxx_git_worktree` 工具服务" | 整份迁到 `agent/lib/include/agentxx/tools/worktree.h`, 命名空间 `utilxx::worktree` → `agentxx::tools::worktree` |
| `cxx_utilxx_base/src/system.cpp` + `system.h` | `defaultDataDir()` 里写死 `.agentxx`, 取 `~/.agentxx/` | 删除该函数 (agentxx 侧本来就有 `AgentConfigStatic::defaultDataDir/systemDataDir`, 且全仓没有任何地方调用库版本) |
| `cxx_utilxx/include/utilxx/http_server.h` | `Server` 头写死 `"agentxx/" + 版本` | 改成 `utilxx_base::productToken()` |
| `cxx_utilxx/src/http_client.cpp` | 默认 `User-Agent` 写死 `"Agentxx/" + 版本` | 同上 (缓存随产品名刷新) |
| `cxx_utilxx/src/ws_client.cpp` | 默认 `User-Agent` 写死 `"agentxx/1.0.0"` | 同上 |
| `cxx_utilxx/src/crypto.cpp` | 读不到机器特征时兜底串 `"agentxx_default_device"` | 改 `"unknown_device"` |
| `cxx_utilxx_base/include/utilxx_base/exception.h` | 中断类异常的消息前缀写死 `"NodeInterrupt"` (agentxx 图引擎的异常名) | 改 `"Interrupt"` |
| `cxx_utilxx_base/include/utilxx_base/asio_error.h` | 兼容另一个项目的开关 `NEOGRAPH_USE_BOOST_ASIO` | 只认本库开关 `UTILXX_USE_BOOST_ASIO` (链接本库即自动带上) |
| `cxx_utilxx/CMakeLists.txt` | 选项 `CXX_UTILXX_ENABLE_BOOST_PROCESS` ("Use Boost.process for worktree commands") 只为 worktree 存在 | 随 worktree 一起删; `cmake/cxx_utilxxConfig.cmake.in` 里的 process 组件查找也删 |

### 注释/文档层面的特化

`agentxx` / `libagentxx` / `AGENTXX_*` / `AgentConfig` / `BaseAgent` / `NodeInterrupt` /
`neograph` / `musicxx` / `.agentxx` / 仓库路径 / 具体工具名 (`agentxx_share_store`) ——
全部改成通用措辞 ("主程序" / "业务引擎" / "取消类异常" / `<主程序前缀>_plugin_agent_*`),
涉及三个库的 20 个头文件/实现文件的注释、3 份 README、2 份 CMakeLists 注释。
`cxx_pluginxx` 只有这类文档改动 (代码零改动)。

## 顺带修掉的 bug (worktree 功能一直是坏的)

`utilxx/worktree.h` 用 `#if defined(UTILXX_ENABLE_BOOST_PROCESS)` 决定是否编译 git
子进程执行, 但**这个宏从来没被定义过** (库的 CMake 选项叫
`CXX_UTILXX_ENABLE_BOOST_PROCESS`, 且没有把它变成编译定义), 于是 `runGit` 恒走降级分支,
返回 `"git execution unavailable (built without boost.process)"`:

- `agentxx_git_worktree` 工具的 create/info/status/remove 全部失败;
- 测试模块 `worktree` 的前置检查 `runGit({"--version"})` 失败 → 打一行
  "git not available in PATH, worktree tests skipped" 就返回, **看起来是通过的**。

迁到 agentxx 后门控改为 `AGENTXX_ENABLE_BOOST_PROCESS` (libagentxx 在开关打开时
PUBLIC 定义它), 执行实现恢复生效。实测 `worktree` 模块从"静默跳过"变成
`passed=25 failed=0`, 真实跑了 init/add/list/exclude/status/remove 保护全流程。

## agentxx 侧改动

- 新增 `agent/lib/include/agentxx/tools/worktree.h` (上述迁移后的头, header-only);
  `git_worktree.cpp`、`test_worktree.cpp` 的包含与命名空间同步。
- 对外标识产品名: 工具库新增 `utilxx_base::setProductName/productName/productToken`
  (默认 `cxx_utilxx/<版本>`), agentxx 在**两份 utilxx 静态副本**里各设一次
  `"Agentxx"` —— `BaseAgent` 构造 (agent 侧的 HTTP/WS 服务与模型请求) 与
  `client/main.cpp` (客户端自身 WS/更新检查; 远端模式不建 agent)。两处幂等。
- `agent/lib/CMakeLists.txt`: 导出依赖按开关补 `Boost CONFIG COMPONENTS process`
  (以前是经 cxx_utilxx 的 config 顺带解析到 `Boost::process`, 该库不再声明后必须自己写)。
- `agent/CMakeLists.txt`: 去掉下发给 cxx_utilxx 的 `-DCXX_UTILXX_ENABLE_BOOST_PROCESS`。
- 测试: `util_misc` 增 `test_product_identity` (默认值/空白回退/设置后标识串/默认请求头
  跟随产品名), +6 条断言。
- 文档: `docs/{zh-cn,en}/design/index.md` 的 worktree 底层封装路径、
  `docs/{zh-cn,en}/design/plugins.md` 与 `index.md` 里 cxx_utilxx 的能力清单 (去掉 worktree)。

## 验证

- Windows debug 全量构建通过 (仅既有 C4251/C4275 DLL 接口告警)。
- `agentxx_test boundaries util_misc worktree http websocket a2a acp plugin_sdk plugin_cleanup`
  → `passed=1319 failed=0`。
- 全量 `agentxx_test` → `passed=38780 failed=8`, 8 条均为改动前就存在的环境/用例问题:
  - `console_state` 2 条: 无真实控制台会话时 `SetConsoleMode` 不生效, 自检判为"被改动";
  - `screen_capture` / `text_selection` 5 条: 这两个用例只用 `loadPluginAsync` 加载插件,
    没有 `mountAgentAsync` (多 agent 挂载改造后注册只发生在 `mount` 回调里), 因此断言
    "工具已注册" 必然失败 —— 同目录其它插件用例都已改成先 `mountAgentAsync`。

## 注意

- `utilxx_base` 的全局状态 (产品名/日志级别等) 是**按静态副本**各自一份: 主程序与插件/
  客户端各自静态链入时互不共享, 需要在"用到它的那个模块"里各设置一次。
- 三个库都是独立 git 子模块, 本次改动只在工作区, 未提交。
