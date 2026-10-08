# 配置键目录

> 本文是**生成物**, 请勿手工编辑。机器可读版本见 `agent/schema/config-keys.json`。
>
> 重新生成: 设 `AGENTXX_UPDATE_CONFIG_KEYS=1` 运行测试模块 `config_keys`
> (生成后请人工 review diff 再提交)。
>
> 权威实现是 `agent/client/src/config_loader.cpp`; 本目录的每个键都由
> `config_keys` 真实加载校验 (默认值与样例值各一次), 并且加载器里出现的新键
> 若没登记进来, 该模块会直接失败。
>
> 相关文档: [index.md](index.md) · [配置与设置边界](configuration.md)

键数: 51

| 键路径 | 类型 | 默认值 |
|---|---|---|
| `data_dir` | string | 空 (不持久化: 数据仅存内存) |
| `work_dir` | string | 空 (用进程当前工作目录) |
| `language` | string (zh-cn | en) | 空 (由客户端界面语言决定) |
| `subagent.enable` | bool | true |
| `worktree.enable` | bool | false |
| `permission.mode` | enum (ask | all_ask | pass | deny) | ask |
| `permission.whitelist.list` | list<string> | 空 |
| `permission.blacklist.list` | list<string> | 空 |
| `skill.list` | list<string> | 空 |
| `memory.list` | list<string> | 空 |
| `model.use.default` | string | 空 |
| `model.use.subagent` | string | 空 |
| `model.use.web_search` | string | 空 |
| `model.use.acp` | string | 空 |
| `model.use.train` | string | 空 |
| `model.use.train_scorer` | string | 空 |
| `model.use.train_optimizer` | string | 空 |
| `model.list[].name` | string (必填) | 无 (缺 name 的条目被跳过) |
| `model.list[].type` | string (openai | anthropic | openai-responses) | openai |
| `model.list[].base_url` | string | 空 (用 provider 默认官方地址) |
| `model.list[].api_key` | string (支持 ${VAR} 展开) | 空 |
| `model.list[].model_name` | string | 空 |
| `model.list[].api_path` | string | 空 (按 type 用默认路径) |
| `model.list[].send_thinking` | bool | false |
| `model.list[].request_reasoning_summary` | bool | true |
| `model.list[].cache_control` | bool (仅 anthropic 生效) | false |
| `model.list[].connect_timeout` | int (秒) | 16 |
| `model.list[].read_chunk_timeout` | int (秒) | 60 |
| `model.list[].ssl_verify` | bool (省略 = 未指定, 用全局默认策略) | 未指定 (nullopt) |
| `model.list[].max_concurrent_connections` | int (0 = 不限制) | 5 |
| `model.list[].model_context_max_token` | int (0 = 未指定) | 0 |
| `model.list[].image_input` | bool | false |
| `model.list[].audio_input` | bool | false |
| `model.list[].video_input` | bool | false |
| `model.list[].extra_headers` | map<string, string> | 空 |
| `model.list[].extra_api_config` | map<string, any> (合并进请求体) | 空 |
| `model.overwrite.mode` | enum (merge | replace) | merge (继承并叠加 base 层) |
| `model.overwrite.remove` | list<string> (按 name 剔除 base 项) | 空 (不剔除) |
| `plugin.list[].path` | string (必填; 支持 builtin://<名字>) | 无 (缺 path/name 的条目被跳过) |
| `plugin.list[].name` | string (内置插件简写, 等价 builtin://<name>) | 空 (缺 path 时按内置名补齐) |
| `plugin.list[].enabled` | bool | true |
| `plugin.list[].sides` | enum (auto | agent | client) | auto |
| `plugin.list[].args` | map<string, any> (原样传给插件, 标量递归展开 ${VAR}) | 空 |
| `plugin.list[].config` | string (插件配置文件/目录; 支持 ~ 与 ${VAR}) | 空 |
| `mcp.list[].namespace` | string (必填; 命名空间) | 无 (缺 namespace/url 的条目被跳过) |
| `mcp.list[].url` | string | 空 (缺 url 的条目被跳过) |
| `mcp.list[].timeout` | int (秒; 0 = 不限制) | 120 (秒) |
| `mcp.list[].headers` | map<string, string> (HTTP 请求头; 支持 ${VAR} 展开) | 空 (不附加自定义请求头) |
| `models` | 已废弃 (改名为 `model.list`) | 不生效 (整段忽略) |
| `plugins` | 已废弃 (改名为 `plugin.list`) | 不生效 (整段忽略) |
| `use_model` | 已废弃 (移到 `model.use`) | 不生效 (整段忽略) |

## 样例

每个键的最小覆盖写法 (示例值仅用于说明形态):

### `data_dir`

```yaml
data_dir: cfg-keys-data
```

### `work_dir`

```yaml
work_dir: cfg-keys-work
```

### `language`

```yaml
language: ZH-CN
```

### `subagent.enable`

```yaml
subagent:
  enable: false
```

### `worktree.enable`

```yaml
worktree:
  enable: true
```

### `permission.mode`

```yaml
permission:
  mode: deny
```

### `permission.whitelist.list`

```yaml
permission:
  whitelist:
    list: ["/data/a", "./b"]
```

### `permission.blacklist.list`

```yaml
permission:
  blacklist:
    list: ["./secret"]
```

### `skill.list`

```yaml
skill:
  list: ["./skills", "./more"]
```

### `memory.list`

```yaml
memory:
  list: ["./AGENTS.md"]
```

### `model.use.default`

```yaml
model:
  use:
    default: m1
```

### `model.use.subagent`

```yaml
model:
  use:
    subagent: m1
```

### `model.use.web_search`

```yaml
model:
  use:
    web_search: m1
```

### `model.use.acp`

```yaml
model:
  use:
    acp: m1
```

### `model.use.train`

```yaml
model:
  use:
    train: m1
```

### `model.use.train_scorer`

```yaml
model:
  use:
    train_scorer: m1
```

### `model.use.train_optimizer`

```yaml
model:
  use:
    train_optimizer: m1
```

### `model.list[].name`

```yaml
model:
  list:
    - name: m9
      type: "openai"
```

### `model.list[].type`

```yaml
model:
  list:
    - name: m1
      type: "anthropic"
```

### `model.list[].base_url`

```yaml
model:
  list:
    - name: m1
      base_url: "https://api.example.com"
```

### `model.list[].api_key`

```yaml
model:
  list:
    - name: m1
      api_key: "EMPTY"
```

### `model.list[].model_name`

```yaml
model:
  list:
    - name: m1
      model_name: "gpt-4"
```

### `model.list[].api_path`

```yaml
model:
  list:
    - name: m1
      api_path: "/v1/chat/completions"
```

### `model.list[].send_thinking`

```yaml
model:
  list:
    - name: m1
      send_thinking: true
```

### `model.list[].request_reasoning_summary`

```yaml
model:
  list:
    - name: m1
      request_reasoning_summary: false
```

### `model.list[].cache_control`

```yaml
model:
  list:
    - name: m1
      cache_control: true
```

### `model.list[].connect_timeout`

```yaml
model:
  list:
    - name: m1
      connect_timeout: 3
```

### `model.list[].read_chunk_timeout`

```yaml
model:
  list:
    - name: m1
      read_chunk_timeout: 7
```

### `model.list[].ssl_verify`

```yaml
model:
  list:
    - name: m1
      ssl_verify: false
```

### `model.list[].max_concurrent_connections`

```yaml
model:
  list:
    - name: m1
      max_concurrent_connections: 2
```

### `model.list[].model_context_max_token`

```yaml
model:
  list:
    - name: m1
      model_context_max_token: 128000
```

### `model.list[].image_input`

```yaml
model:
  list:
    - name: m1
      image_input: true
```

### `model.list[].audio_input`

```yaml
model:
  list:
    - name: m1
      audio_input: true
```

### `model.list[].video_input`

```yaml
model:
  list:
    - name: m1
      video_input: true
```

### `model.list[].extra_headers`

```yaml
model:
  list:
    - name: m1
      extra_headers:
        x-custom: v
```

### `model.list[].extra_api_config`

```yaml
model:
  list:
    - name: m1
      extra_api_config:
        temperature: 0.7
```

### `model.overwrite.mode`

```yaml
model:
  overwrite:
    mode: replace
  list:
    - name: m3
      type: "openai"
```

### `model.overwrite.remove`

```yaml
model:
  overwrite:
    remove: [m1]
```

### `plugin.list[].path`

```yaml
plugin:
  list:
    - path: "builtin://agentxx_filesystem"
```

### `plugin.list[].name`

```yaml
plugin:
  list:
    - name: agentxx_filesystem
```

### `plugin.list[].enabled`

```yaml
plugin:
  list:
    - path: "builtin://agentxx_filesystem"
      enabled: false
```

### `plugin.list[].sides`

```yaml
plugin:
  list:
    - path: "builtin://agentxx_filesystem"
      sides: agent
```

### `plugin.list[].args`

```yaml
plugin:
  list:
    - path: "builtin://agentxx_filesystem"
      args:
        index_root: "/repo"
        depth: 0.5
```

### `plugin.list[].config`

```yaml
plugin:
  list:
    - path: "builtin://agentxx_filesystem"
      config: "./conf"
```

### `mcp.list[].namespace`

```yaml
mcp:
  list:
    - namespace: "tools"
      url: "http://127.0.0.1:9/sse"
```

### `mcp.list[].url`

```yaml
mcp:
  list:
    - namespace: "tools"
      url: "http://127.0.0.1:9/sse"
```

### `mcp.list[].timeout`

```yaml
mcp:
  list:
    - namespace: "tools"
      url: "http://127.0.0.1:9/sse"
      timeout: 5
```

### `mcp.list[].headers`

```yaml
mcp:
  list:
    - namespace: "tools"
      url: "http://127.0.0.1:9/sse"
      headers:
        Authorization: "Bearer token"
```

### `models`

```yaml
models:
  - name: legacy
    type: "openai"
```

### `plugins`

```yaml
plugins:
  - path: "builtin://agentxx_filesystem"
```

### `use_model`

```yaml
use_model:
  default: legacy-model
```

## 说明

- 列表段 (`model` / `plugin` / `mcp` / `skill` / `memory` / `permission.whitelist` /
  `permission.blacklist`) 统一是 `list:` + 可选 `overwrite:` 两键结构;
  旧写法 (段值直接给列表/字符串) 已不再支持, 会记警告并忽略该段。
- `overwrite` 只在**上层** (overlay) 生效: `mode: merge` (默认, 继承并叠加 base)
  或 `replace` (整段只用本层); `remove` 按身份剔除 base 项 (model 按 name,
  plugin 按 path/name, mcp 按 namespace, 名单与路径列表按字符串本身)。
- 已废弃的旧键 (不再生效, 只记一次迁移提示): `models` -> `model.list`,
  `plugins` -> `plugin.list`, `use_model` -> `model.use`。
- 键值都支持 `${VAR}` 展开 (查找顺序: 程序内置变量 > `--env` > `.env` >
  系统环境变量); 路径类键的 `~` 展开与相对路径绝对化由装配侧完成。
