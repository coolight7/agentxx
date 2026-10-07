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
- 需要插件自己解释的配置 → 写在 `plugin.list[].args` / `plugin.list[].config`，由插件定义语义（宿主原样透传）。

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

详细实现与清单见 [roadmap.md](roadmap.md) 的 CFG-1 / CFG-9 条目。
