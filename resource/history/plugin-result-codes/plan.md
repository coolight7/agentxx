# agentxx 插件业务表返回码重构（迁到 pluginxx 的应用自定义段）

## 目标

按 `pluginxx/api/result.h` 的设计建议，把 agentxx 各业务表的返回码收成一份表：成功 = 0，
失败一律落在约定给应用的 `-5000 ~ -9999` 段；删掉现有的 `AGENTXX_PLUGIN_ERR_NO_AGENT`（-2）、
`AGENTXX_PLUGIN_RUNTIME_*`（-1~-7）以及大量裸 `-1`/`0`，内置插件、SDK、测试、文档同步迁移。
版本还没发布，按用户要求**不做旧版本兼容**（直接改名改值，不保留别名）。

## 范围

- `agent/lib/include/agentxx/plugin/api/agentxx_result.h`（新增，唯一码表 + 名字表）
- `plugin_api.h` / `client_plugin_api.h`（引用新码表，更新每张表的返回码说明）
- 生产者：`plugin_manager_adapters.cpp`（`PluginMount::*` 注册/注销）、`plugin_manager_vtable.cpp`
  （agent 侧表入口）、`plugin_manager_runtime*.cpp`（运行时表）、`client_plugin_manager*.cpp`
- SDK 与插件：`plugin_kit.h` 的 runtime/其它便捷层、`agent/plugins/**`（chatbot / JS 引擎 / 示例）
- 测试：`agent/test/**` 里比较这些码的用例
- 文档：`docs/zh-cn/design/plugins.md`（§4 / §17 / §19 等处的返回码）、`AGENTS.md`

## 不在范围

- `AGENTXX_FFI_*`（`ffi_api.h`）：FFI 是给嵌入方程序的独立 C API，不走插件框架，码表本来就有名字
  与 `agentxx_ffi_strerror`，与本次 pluginxx 的建议无关，保持不变。
- 插件框架的通用表（broadcasts/capabilities/…）：它们已经用 `PLUGINXX_RESULT_*`。
- 句柄类返回值（channel id / trigger id / timer id / asyncId）：`-1` 表示"没拿到 id"，
  不是返回码，保持不动。

## 现状调查

| 位置 | 现状 |
|---|---|
| `plugin_api.h:58` | `AGENTXX_PLUGIN_ERR_NO_AGENT (-2)`，17 处使用（注册类接口引用无效） |
| `plugin_api.h:660~667` | `AGENTXX_PLUGIN_RUNTIME_OK(0)` + `RUNTIME_ERR_{ARG,STATE,NOT_FOUND,BUDGET,CONFLICT,UNSUPPORTED,FAILED}`（-1~-7），运行时表专用；在插件与测试里出现 200+ 次 |
| 各业务表入口 | 除运行时表外一律裸 `0` 成功 / `-1` 失败（`plugin_manager_vtable.cpp` 127 处、`client_plugin_manager.cpp` 71 处、`plugin_manager_adapters.cpp` 40 处） |
| `plugin_manager_runtime*.cpp` | 运行时表内部 78 处，混用 `AGENTXX_PLUGIN_RUNTIME_*` 与裸 0/-1 |

`pluginxx/api/result.h` 的分段约定：`0` 成功；`-1 ~ -99` 通用；`-100 ~ -199` 网络；
`-200 ~ -299` 广播；`-500`/`-999` 生命周期与内部；**`-5000 ~ -9999` 应用自定义**；
`-10000 ~ -19999` 插件自定义。用自有码时主程序按区间给出类别（`app_custom`），名字由应用解释。

## 方案

### 码表（`agentxx/plugin/api/agentxx_result.h`）

一份表，所有 agentxx 表共用（同义同值，和 pluginxx 通用段的做法一致）。全部落在
`-5000 ~ -5010`，每个码都必须有真实返回点：

| 码 | 值 | 含义 |
|---|---|---|
| `AGENTXX_RESULT_OK` | `PLUGINXX_RESULT_OK` (0) | 成功 |
| `AGENTXX_RESULT_ERR_ARG` | -5000 | 参数非法（空指针 / 空字符串 / 字段越界 / `struct_size` 低于基线） |
| `AGENTXX_RESULT_ERR_JSON` | -5001 | 参数里的 JSON 文本非法（解析失败 / 不是对象 / 版本不认识） |
| `AGENTXX_RESULT_ERR_STATE` | -5002 | 当前状态不允许（未就绪 / 关闭中 / 在 `mount` 里注册 / 已停止） |
| `AGENTXX_RESULT_ERR_NOT_FOUND` | -5003 | 目标不存在（工具 / 会话 / 资源 / 图 / 挂载 / 根 / 通道 / 触发器 / 定时器 / 任务 / 键位 / UI 句柄） |
| `AGENTXX_RESULT_ERR_NO_AGENT` | -5004 | agent 引用无效（原 `AGENTXX_PLUGIN_ERR_NO_AGENT`） |
| `AGENTXX_RESULT_ERR_NOT_OWNER` | -5005 | 对象不属于本实例（注销别人的工具/权限，替别人的工具声明权限） |
| `AGENTXX_RESULT_ERR_CONFLICT` | -5006 | 名字或 id 冲突（工具/节点类型重名，同一 sessionId 开两个通道，键位被占用） |
| `AGENTXX_RESULT_ERR_LIMIT` | -5007 | 超过上限（根数 / 通道数 / 触发器 / 定时器 / 键位 / UI 描述大小） |
| `AGENTXX_RESULT_ERR_UNSUPPORTED` | -5008 | 主程序没提供这项能力（权限中间件未装配 / 运行时容器未 attach / 客户端无该子能力） |
| `AGENTXX_RESULT_ERR_FAILED` | -5009 | 执行失败（落盘 / 投递 / 目标侧拒绝 / init 失败，原因看日志） |
| `AGENTXX_RESULT_ERR_INTERNAL` | -5010 | 实现内部错误（不该发生：异常逃逸 / 不变量被破坏） |

配套 `agentxx::plugin::resultName(code)`（`inline`，C++ 侧日志/JSON 用；名字与 pluginxx 的
风格一致：`invalid_argument` / `invalid_json` / `invalid_state` / `not_found` / `invalid_agent` /
`not_owner` / `conflict` / `limit_exceeded` / `unsupported` / `failed` / `internal`）。

### 各表的映射要点

- **注册类**（tools / permission / prompt / resources / graph node）：引用无效 → `ERR_NO_AGENT`；
  参数不全 → `ERR_ARG`；实例在关闭/禁用 → `ERR_STATE`；工具重名 → `ERR_CONFLICT`；
  注销/声明别人的对象 → `ERR_NOT_OWNER`；权限中间件没装配 → `ERR_UNSUPPORTED`。
- **读取类**（`get_messages` / `get_config` / `get_graph_*` / `list` / `get` …）：目标不存在
  → `ERR_NOT_FOUND`；参数非法 → `ERR_ARG`；异常 → `ERR_INTERNAL`。
- **写入类**（`set_prompt` / `set_graph_json`）：JSON 非法 → `ERR_JSON`；无占用者冲突 →
  `ERR_NOT_OWNER`（已被别的实例占用）。
- **运行时表**：`ARG/STATE/NOT_FOUND/CONFLICT/UNSUPPORTED/FAILED` 原样搬到同名新码；
  `BUDGET` → `ERR_LIMIT`；句柄类入口（`open_channel` / `register_trigger` / `register_timer`
  返回 id）失败仍是 `-1` / 0，不算返回码。
- **client 侧表**（ui/session/wire/self/timer/keybind）：未连接 → `ERR_STATE`；句柄/id 不存在
  → `ERR_NOT_FOUND`；JSON 非法 → `ERR_JSON`；键位占用 → `ERR_CONFLICT`；动画等级不允许 /
  主程序没接 → `ERR_UNSUPPORTED`；条数超限 → `ERR_LIMIT`。

### 其它决定

- `AGENTXX_PLUGIN_API_VERSION` / 各表 `_VERSION` **不抬**：当前 5 就是这次未发布的代次，
  用户明确说本次直接改、不做旧版本兼容。
- 旧宏名直接删除（不留 alias），全树改名，编译器会把漏改的地方全部报出来。
- 句柄类接口的 `-1`/`NULL` 语义保持不变。

## 分步实施

1. 新增 `agentxx_result.h`（码 + `resultName`），并在 `plugin_api.h` / `client_plugin_api.h` 里包含它
2. 生产者：`plugin_manager_adapters.cpp` → `plugin_manager_vtable.cpp` → `plugin_manager_runtime*.cpp`
3. client 侧：`client_plugin_manager.cpp`（ui/session/wire/self/timer/keybind 各入口）
4. SDK `plugin_kit.h`：runtime 便捷层与其它比较这些码的地方改成新宏；错误串拼接用 `resultName`
5. 内置插件与示例：chatbot / javascript_engine / example_* 等
6. 测试：把所有比较旧码的地方换新码（含 `AGENTXX_PLUGIN_RUNTIME_OK` 83 处）
7. 文档：plugins.md 的返回码段落、AGENTS.md 的 `AGENTXX_PLUGIN_ERR_NO_AGENT` 提法
8. 编译 + 跑全量测试

## 验证

```powershell
agent/script/windows_debug_build.bat
agent/build/windows-debug/exec/agentxx_test.exe --log-level warn
# 旧宏名必须清零
grep -rn 'AGENTXX_PLUGIN_ERR_NO_AGENT|AGENTXX_PLUGIN_RUNTIME_' agent/lib agent/client agent/plugins agent/test docs AGENTS.md
```

## 风险

- 漏改某处 `-1` 判断会让"成功也算失败"或反之：逐个入口按上下文改，并靠全量测试兜底。
- 插件比主程序先编译（头文件不一致）时会编译失败 —— 一次性全量构建可避免。
- `resultName` 只用于日志/JSON，不参与 C ABI（`inline`，各模块各一份字面量）。
