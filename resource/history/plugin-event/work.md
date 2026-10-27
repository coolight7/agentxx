# 插件内驱动 agent 会话：实施记录

> 设计来源：[index.md](index.md) · [01-analysis.md](01-analysis.md) · [02-runtime-design.md](02-runtime-design.md) · [03-event-driven.md](03-event-driven.md) · [04-prompt-and-impl.md](04-prompt-and-impl.md)
> 前置：`../plugin-multi-agentnode/`（进程一份插件宿主 + 每 agent 挂载 + agent 引用，已完成）

## 状态总览

| 阶段 | 内容 | 状态 |
|---|---|---|
| 一 | 统一启动壳 `AgentSessionRuntime`（FFI 先接，CLI 第二步） + `parseUserInputJson` + `EmbeddedClientAgentIO` | **已完成** |
| 二 | `AgentHost` 多根（内置根 + 挂载根）+ 会话占用表 | **已完成** |
| 三 | 运行时接口表 `agentxx.agent.runtime`（挂根 / 通道 / 输入 / 事件 / 中断） | 进行中 |
| 四 | 事件驱动：触发器（QUEUE/COALESCE/DROP_IF_BUSY） + 定时器 | 未开始 |
| 五 | 会话级提示词来源（`PromptSectionSlot::Base`，可选） | 未开始 |
| 六 | 文档与示例插件 `agent/plugins/example_runtime` | 未开始 |

## 阶段一：统一启动壳（已完成）

| 文件 | 动作 | 状态 |
|---|---|---|
| `agent/lib/include/agentxx/agent/io/user_input_json.h` + `agent/lib/src/agent/io/user_input_json.cpp` | 新增：输入 JSON 解析（从 FFI 提取；`std::expected` + `UserInputError{badJson, message}` 区分语法错误与参数错误） | 完成 |
| `agent/lib/include/agentxx/agent/io/embedded_client_agent_io.h` + `agent/lib/src/agent/io/embedded_client_agent_io.cpp` | 新增：嵌入式 client 端点基类（中断登记/等待/过期/失败清理、增量合批、线程判定、`onRuntimeStopping`） | 完成 |
| `agent/lib/include/agentxx/agent/io/agent_io.h` | 新增空实现钩子 `onRuntimeStopping()`（关闭流程开始时通知客户端端点） | 完成 |
| `agent/lib/include/agentxx/agent/session_runtime.h` + `agent/lib/src/agent/session_runtime.cpp` | 新增：装配（makePair/setTransport/建服务端点）、启动顺序（两侧接收循环 → init → 登记根 → 就绪 → 驱动循环 → hello）、关闭顺序、控制命令 | 完成 |
| `agent/lib/src/ffi/ffi_client_io.{h,cpp}` | 改为继承 `EmbeddedClientAgentIO`，只保留事件转 C 回调与 FFI 专有消息 | 完成 |
| `agent/lib/src/ffi/ffi_runtime.{h,cpp}` | 改用 `AgentSessionRuntime` + `parseUserInputJson`；FFI 用 `loadPluginsInHost=false` 保持“主程序不代劳加载插件” | 完成 |
| `agent/client/src/mode_runners.cpp` | 本地直连改用 runtime（约 130 行重复装配删除），差异只剩端点类型/尾窗/语言/首个查询 | 完成 |
| `agent/test/core/test_session_runtime.cpp` + `agent/test/include/agentxx-test/core/test_session_runtime.h` | 新增模块 `session_runtime`（55 条断言: 解析 8 类边界、装配拒绝、幂等 start、init 失败上报、skipInit 下一轮完整对话） | 完成 |

验收结果（Debug）：

| 模块 | 结果 |
|---|---|
| `session_runtime` | passed=55 failed=0 |
| `ffi_c_api` | passed=208 failed=0 |
| `boundaries` / `config_keys` | 19 / 178 全绿（新增文件用词符合规则 15） |
| `input_delivery` / `agent` / `agent_host` / `subagent_bus` / `subagent_tool` | 全绿 |
| `agentxx_cli`（本地直连 TUI/CLI） | 编译链接通过，行为不变（装配顺序与原实现一致） |

## 阶段二：AgentHost 多根

| 文件 | 动作 | 状态 |
|---|---|---|
| `agent/lib/include/agentxx/agent/agent_host.h` | `MountSpec` / `MountedRoot` / `mountRoot` / `unmountRoot` / `unmountRootsByOwner` / `findRoot` / `roots` / `maxRoots` / `claimSession` / `releaseSession` | 未开始 |
| `agent/lib/src/agent/agent_host.cpp` | 每根一个子代理服务（闭包捕获该根 ctx）；`spawnOneTask` 用发起方 ctx；`publishProgress` 走 hostBus；`AgentRegistry` 重复 id 拒绝；`mountRoot` 强注入 `pluginHost_` | 未开始 |
| `agent/test/core/test_agent_host.cpp` | 多根用例（节点/委派/记账/卸载/预算/存储根） | 未开始 |

验收（04 §4.3 的 8 条）：三根节点并存、挂载根委派、用量记账、卸载后释放、超预算拒绝、现有子代理测试全绿、`pluginMountPolicy` 生效、`(存储根, sessionId)` 冲突。

## 阶段二：AgentHost 多根（已完成）

| 文件 | 动作 | 状态 |
|---|---|---|
| `agent/lib/include/agentxx/agent/agent_host.h` | 新增 `MountedRoot` / `MountSpec`（agentId/owner/primary/registerSubagentService）、`mountRoot` / `unmountRoot` / `unmountRootsByOwner` / `findRoot` / `roots()` / `rootCount()`、`Config::maxRoots`（默认 8）、`claimSession` / `releaseSession` / `sessionOccupancyJson`；`injectShared` / `registerSubagentService` / `unregisterSubagentService` 私有辅助 | 完成 |
| `agent/lib/src/agent/agent_host.cpp` | `attachRoot` 转调 `mountRoot({agentId="root", primary=true})`；每根一个子代理服务（闭包捕获该根上下文，`spawnBatch(req, rootCtx)`）；`spawnOneTask` 的父配置/工具继承/用量记账/share_store/HIL 全改用发起方上下文；`publishProgress` 改走 hostBus 直接 co_spawn；`AgentRegistry::insert` 重复 id 拒绝；`destroyAgent`/`destroyAgentAsync` 一并摘根与子代理服务；会话占用表（`存储根 + sessionId`，带锁，跨插件实例） | 完成 |
| `agent/test/core/test_agent_host.cpp` | 新增 `test_host_multi_root`（三根并存/节点结构/重复 id/超预算/委派/卸载/归属者批量卸载）与 `test_host_session_claim`（同根同名互斥、不同根同名共存、非占用者不能释放） | 完成 |

验收结果（04 §4.3 的 8 条）：

| 编号 | 用例 | 结果 |
|---|---|---|
| 1 | 内置根 + 两个挂载根：`roots().size()==3`，节点 `parentAgentId` 空 / `depth=0` | 通过 |
| 2 | 挂载根里的子代理委派：节点父 id 指向该根，结果正常回收 | 通过 |
| 3 | 用量记账：`spawnOneTask` 用发起方上下文（`usageAccountSessions` 取该根的 sessions） | 通过 |
| 4 | `unmountRoot` 后节点与服务消失 | 通过 |
| 5 | 超 `maxRoots` 拒绝（无节点/无根表残留） | 通过 |
| 6 | 现有子代理测试全绿（`subagent_bus` 21、`subagent_tool` 143、`agent` 199、`summarization` 454） | 通过 |
| 7 | `pluginMountPolicy` 生效（`injectShared` 按策略注管理器；None 不注） | 通过（插件挂载组全绿） |
| 8 | `claimSession` 同存储根同名失败 / 不同存储根同名成功 / 非占用者不能释放 | 通过 |

回归：`plugin_host_mount` 146 / `plugin_multi_mount` 47 / `plugin_multi_instance` 80 / `plugin_cleanup` 99 / `plugin_runtime` 736 / `boundaries` 19 / `session_runtime` 55，全部 failed=0。

## 阶段三：运行时接口表

| 文件 | 动作 | 状态 |
|---|---|---|
| `agent/lib/include/agentxx/plugin/api/plugin_api.h` | `agentxx.agent.runtime` 表 + 错误码 + 通道事件 + spec | 未开始 |
| `agent/lib/include/agentxx/plugin/api/plugin_iface_meta.h` | `IfaceMeta<AgentxxPluginRuntimeIface>` | 未开始 |
| `agent/lib/include/agentxx/plugin/api/plugin_kit.h` | `AgentIfaces.runtime` + SDK（mountRoot/openChannel/sendInput/…） | 未开始 |
| `agent/lib/include/agentxx/plugin/runtime_state.h` | 新增：根 / 通道 / 触发器记账 | 未开始 |
| `agent/lib/src/plugins/plugin_manager_runtime.cpp` | 挂根 / 开关通道 / 输入 / 查询 / 事件汇 | 未开始 |
| `agent/lib/src/plugins/plugin_manager_runtime_config.cpp` | `config_json` → `AgentConfig` 派生 + `mount_plugins` 解析 | 未开始 |
| `agent/lib/src/plugins/plugin_manager_vtable.cpp` | 16 个入口 + `query_interface` 分支 | 未开始 |
| `agent/lib/src/plugins/plugin_manager_lifecycle.cpp` | 禁用/卸载摘运行时 | 未开始 |
| `agent/test/core/test_boundaries.cpp` | 表数量 21 → 22 + 成员顺序台账 | 未开始 |
| `agent/test/plugin/test_plugin_agent_runtime.cpp` | 新增模块（04 §4.4 的 12 条） | 未开始 |

## 阶段四：事件驱动

| 文件 | 动作 | 状态 |
|---|---|---|
| `agent/lib/src/plugins/plugin_manager_runtime_trigger.cpp` | 触发器（队列/合并/丢弃）+ 定时器 | 未开始 |
| `agent/test/plugin/test_plugin_agent_trigger.cpp` | 策略、窗口、定时器、禁用撤销 | 未开始 |

## 阶段五：会话级提示词来源（可选）

| 文件 | 动作 | 状态 |
|---|---|---|
| `agent/lib/.../prompt_sources.{h,cpp}` | `PromptSectionSlot::Base` + 按会话基础段 | 未开始 |
| prompt 接口表 | 表尾追加 `register_source` / `unregister_source`（v2 → v3） | 未开始 |

## 阶段六：文档与示例

| 对象 | 状态 |
|---|---|
| `docs/zh-cn/design/plugins.md`、`index.md`、`ffi.md`、`agent/plugins/AGENTS.md`、根 `AGENTS.md` | 未开始 |
| `agent/plugins/example_runtime/` | 未开始 |

## 实施注意

- 插件实例永远只有一份（`create`/`start` 各一次）；根只是"再挂载一次"，`mountRoot` 必须强注入宿主 `pluginHost_`。
- 建根只能在 `start`；`mount` 回调期间调 `mount_root` → `ERR_STATE`；`include_creator` 默认 false。
- 入口函数体不能 `co_await`（`ioCallSyncKeep` 在 io 线程直接执行），建/停一律"发起协程后返回"，结果走事件。
- `(存储根, sessionId)` 独占是进程级：占用表挂 `AgentHost`，不能按插件实例建。
- 通道 delta 重放缓冲设 0（`deltaBufferCap`），transport 队列容量保持默认。
- FFI 侧保持"不让主程序代劳加载插件"的现有行为（`loadPluginsInHost=false`），避免启动顺序回归。
