# 配置与设置边界

> 相关文档: [index.md](index.md)（配置示例与加载流程）· [config-keys.md](config-keys.md)
> （配置键目录, 生成物: 每个 yaml 键的类型与默认值）· [security.md](security.md) ·
> [roadmap.md](roadmap.md)

## 1. 两个存储、三种归属

agentxx 有两处持久化配置，职责必须分开，避免同一件事两处可配：

| 归属 | 存放位置 | 适用内容 | 例子 |
|---|---|---|---|
| **可分发配置** | YAML（`agentxx-config.yaml`，base + overlay 两层） | 团队/项目共享、需要进版本库、需要随部署走的配置 | 模型列表与参数、插件列表、权限模式与白/黑名单、技能与记忆文件列表、会话语言（`language`）、`data_dir` / `work_dir` |
| **本机偏好** | 设置库（`{data_dir}/sqlite/global.db` 的 `setting` 表） | 只对当前这台机器/这个用户有意义的界面与体验项 | `tui.theme`（主题）、`tui.animationLevel`（动画等级）、`tui.logLevel`（日志等级）、`tui.tailThinking`（末尾思考显示）、`tui.lang`（界面语言）、`tui.checkUpdateOnStartup`（启动检查更新） |
| **运行期安全状态** | 内存（不落盘） | 生命周期只到进程结束的状态 | 完全授权（用户在询问卡片勾选或客户端切换）、"记住本次选择"的权限规则、队列暂停状态 |

判定规则：

- 换一台机器还需要一样的 → YAML；换一台机器应该不一样（界面、显示习惯）→ 设置库；
- 涉及安全授权且应当每次运行都重新确认 → 内存，**不要**写进设置库，也**不要**写进 YAML 当作已授权；
- 需要插件自己解释的配置 → 写在 `plugin.list[].args` / `plugin.list[].config`，由插件定义语义（主程序原样透传）。

除"存在哪里"，还有第二个维度：**哪个进程读它**（跑 agent 的服务端，还是只做界面的
客户端）—— 见第 5 节。

## 2. 语言：`language` 与 `tui.lang` 的分工

两者同名但不是一件事，2026-10 起已明确分开：

| 键 | 位置 | 作用域 | 取值 |
|---|---|---|---|
| `language` | YAML | **会话与模型提示词语言**（agent 端），全进程/全会话默认值 | `zh-cn` / `en`（不支持 `auto`，空值或 `auto` 归一化为 `en`） |
| `tui.lang` | 设置库 `setting` 表 | **TUI 界面显示语言**（本机偏好） | `0=Auto`（跟随系统） / `1=ZhCn` / `2=EnUs` |

解析优先级（会话语言）：

1. YAML 显式配置了 `language` → 以它为准，客户端连接时携带的界面语言**不再覆盖**（`AgentConfig::languageExplicit`）；
2. YAML 未配置 → 客户端 `WireHello.language`（TUI 按 `tui.lang` 解析出的语言码）生效；
3. 都没有 → `en`。

同一规则对 FFI 生效：配置 JSON 里显式给出 `language` 时视为配置指定（`FfiAgentRuntime` 的 `set_language` 仍可在运行期显式改写）。

## 3. 合并与覆盖（base / overlay）

- overlay：工作目录（或 `--config` 指定）的 `agentxx-config.yaml` + 同目录 `.env`；
- base：overlay 的 `data_dir` 目录下的同名文件（未配置 `data_dir` 时为系统数据目录 `~/.agentxx/`）；
- 列表段统一为 `{overwrite: {mode: merge|replace, remove: [...]}, list: [...]}`：
  `merge`（默认）按键归并/追加去重，`replace` 整段不继承，`remove` 按身份剔除 base 项；
- 其余键为标量覆盖、映射逐键合并；`.env` 同名变量取 overlay 值；
- 旧键（`models` / `plugins` / `use_model` 等）会被告警并忽略，见 [index.md](index.md) 的配置章节。

**不修改用户文件**：加载阶段对结构变化只做告警与内存态适配（例如旧段名提示、非法值回退默认），
不写版本号、不迁移、不改写源文件也不生成副本。

## 4. 校验

配置校验的结果按"键路径 + 来源文件 + 严重级别（致命 / 警告）"给出：

- **致命**：会直接导致功能不可用且用户无法在运行时补救的项（例如显式指定了插件路径但文件不存在、显式指定了模型但没有可用的 API 端点、`--config` 指向的文件无法解析、配置要求持久化但 `data_dir` 不可写）；
- **警告**：可回退到默认值继续运行的项（未知键、非法枚举取默认值、目录不存在但功能可降级）。

详细实现与清单见 [roadmap.md](roadmap.md) 的配置校验与配置键目录条目。

## 5. 配置归属：服务端 / 客户端 / 两侧

同一份 `agentxx-config.yaml` 可能被两个进程读到：跑 agent 的**服务端**，和只做界面渲染的
**客户端**。原则只有一条：

> **每条配置只由真正使用它的那一侧读。** agent 行为（模型、工具、权限、资源、提示词）全部
> 归服务端；客户端只读自己的界面偏好。客户端不参与决定模型，也不要求本地配好模型才能连接。

| 运行形态 | 谁在跑 | 读哪些配置 |
|---|---|---|
| `agentxx_cli server` | 服务端 | 全部（见 5.1） |
| `agentxx_cli tui` / `cli`（无 `--agent`） | 同一个进程同时是两端 | 全部；界面偏好另取本机设置库 |
| `agentxx_cli jsonl` | 服务端 | 全部（模型 + 插件 + 运行时资源） |
| `agentxx_cli acp` | 服务端 | 全部 |
| `agentxx_cli train` | 服务端 | 只用模型段（`model.use.train*`） |
| `agentxx_cli tui` / `cli --agent <url>` | 客户端 | **只有** `data_dir`（本机界面偏好 `{data_dir}/sqlite/global.db`）与 `plugin.list` 中 `sides` 允许 client 的条目（`cli` 不读设置库，`data_dir` 填了也不用） |

### 5.1 按运行侧划分的键

| 归属 | 键 | 说明 |
|---|---|---|
| 服务端 | `model.list`、`model.use.*`、`model.overwrite.*` | 模型属于 agent；客户端不知道也不发送配置里的模型 |
| 服务端 | `mcp.list` | MCP 连接由 agent 建立 |
| 服务端 | `skill.list`、`memory.list` | 提示词资源，相对路径按服务端工作目录解析 |
| 服务端 | `permission.mode`、`permission.whitelist`、`permission.blacklist` | 权限判定在 agent 侧；客户端只展示询问并镜像"完全授权" |
| 服务端 | `subagent.enable`、`worktree.enable` | 工具装配 |
| 服务端 | `dev_mode` | 统计与诊断采集，启动期冻结 |
| 服务端 | `plugin.mount.policy` / `.list` / `.args` | 本会话挂哪些插件 |
| 服务端 | `work_dir` | 工具相对路径、子进程工作目录、权限名单的基准；远程客户端显示与选文件用服务端回报的 `hello_ack.workDir` |
| 服务端 | `language` | 会话与提示词语言；远程客户端的界面语言是设置库的 `tui.lang` |
| 两侧 | `data_dir` | 各进程读自己那份：服务端存会话库与索引，客户端只存界面偏好 `sqlite/global.db` |
| 两侧 | `plugin.list`（含 `sides` / `args` / `config`） | 同一份清单，各进程按条目的 `sides` 决定加载谁 |

`.env` 与 `${VAR}` 由各进程用**自己那份**文件展开（内置变量 `AGENTXX_WORK_DIR` /
`AGENTXX_EXEC_DIR` 也按各自进程计算），所以同一个键在两侧可以解析出不同值；真正在两侧都
生效的只有 `data_dir` 与 `plugin.list`。

这张表与 [config-keys.md](config-keys.md) 的「归属」列同源：列由 `config_keys` 模块生成并
校验（`agent/test/core/test_config_keys.cpp` 的 `keyOwnerOf`），新增键没归类会让该模块失败。

### 5.2 客户端显示的东西全部来自服务端

远程模式（`--agent`）下，界面上与服务端能力相关的显示项一律取自 Wire 消息，不读本地 yaml：

| 显示项 | 来源消息 |
|---|---|
| 模型清单 / 当前模型名 | `hello_ack.models`、`model_info`（`get_model` 的应答） |
| 会话工作目录 / 设备 id / 对端插件清单 | `hello_ack` 的 `workDir` / `deviceId` / `plugins` |
| MCP / 技能 / 记忆文件清单 | `append_component_info`（`get_append_component_info` 的应答） |
| 完全授权状态 | `permission_state`（`get_permission_state` / `set_full_auth`） |
| 上下文用量 | `context_stats` |

切换模型只能由用户显式动作触发：选择弹窗经 `select_model`，或 `--model` 记到
`setPendingModel` 后随下一条 `user_input.model` 携带；接不接受由服务端决定。添加模型配置
（弹窗）也是请求服务端写入**服务端**的 yaml（`add_model`），客户端不落盘。

### 5.3 新增配置项该放哪

1. 影响 agent 行为（模型、工具、提示词、权限、资源路径）→ 只归服务端：写进 yaml，由
   `agent/client/main.cpp` 的 `applyServerRuntimeConfig` 装配，客户端代码不许读；
2. 只影响界面观感（主题、动画、日志等级、界面语言、检查更新）→ 只归客户端：写进设置库
   `{data_dir}/sqlite/global.db`，键名 `tui.*`（见第 1 节）；
3. 两侧都要 → 要么按显式分流字段处理（`plugin.list[].sides`），要么拆成两个键（会话语言
   `language` 与界面语言 `tui.lang`），**不要**一个键两处解释。

远程客户端的实现约束（`agent/client/main.cpp` 的 `clientOnlyMode`）：

- 不装配模型 / MCP / 技能 / 记忆 / 权限 / 子代理 / worktree，也不设置 `work_dir`；
- 不调 `validateStartupConfig` —— 没有服务端配置可校验，因此远程客户端不需要本地模型配置；
- `--dump-config` / `--dump-diagnostics` 也只导出本进程真正读的那部分，并说明"其余在服务端看"。
