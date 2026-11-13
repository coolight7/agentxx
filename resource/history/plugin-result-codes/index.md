# 插件业务表返回码重构

## 目标

把 agentxx 各业务表的返回码收成一份表，落到 `pluginxx/api/result.h` 约定给应用的
`-5000 ~ -9999` 段：成功 = 0（= `PLUGINXX_RESULT_OK`），失败用有名字的具体码。删掉现有的
`AGENTXX_PLUGIN_ERR_NO_AGENT`(-2)、`AGENTXX_PLUGIN_RUNTIME_*`(-1~-7)，以及各入口大量裸
`0`/`-1`。内置插件、SDK、测试、文档一起迁移；版本未发布，不留旧名别名。

## 范围

- 新增 `agent/lib/include/agentxx/plugin/api/agentxx_result.h`（码表 + `resultName`）
- `plugin_manager_adapters.cpp` / `plugin_manager_vtable.cpp` / `plugin_manager_runtime*.cpp`
- `client_plugin_manager.cpp`（client 六张表）
- `plugin_kit.h`、`agent/plugins/**`、`agent/test/**`、`docs/zh-cn/design/plugins.md`、`AGENTS.md`

## 结论（事后来看这一页就够）

| 旧 | 新 |
|---|---|
| `AGENTXX_PLUGIN_ERR_NO_AGENT` (-2) | `AGENTXX_RESULT_ERR_NO_AGENT` (-5004) |
| `AGENTXX_PLUGIN_RUNTIME_OK` (0) | `AGENTXX_RESULT_OK` (= `PLUGINXX_RESULT_OK`) |
| `RUNTIME_ERR_ARG/STATE/NOT_FOUND/CONFLICT/UNSUPPORTED/FAILED` (-1~-7) | `RESULT_ERR_` 同名码（-5000/-5002/-5003/-5006/-5008/-5009） |
| `RUNTIME_ERR_BUDGET` (-4) | `RESULT_ERR_LIMIT` (-5007) |
| 各表裸 `0`/`-1` | 按原因给具体码（`ARG` / `JSON` / `STATE` / `NOT_FOUND` / `NOT_OWNER` / `CONFLICT` / `LIMIT` / `UNSUPPORTED` / `FAILED` / `INTERNAL`） |

细节见 `plan.md`，实施与验证记录见 `work.md`。

### 收尾事实

- 码表只有一份：`agentxx_result.h`（`AGENTXX_RESULT_OK` + 11 个 `AGENTXX_RESULT_ERR_*`，
  `-5000 ~ -5010`），名字统一经 `agentxx::plugin::resultName(code)` 输出；
- 旧宏全删（`AGENTXX_PLUGIN_ERR_NO_AGENT` / `AGENTXX_PLUGIN_RUNTIME_*`），不留别名：
  版本未发布，插件与主程序同一代头文件重编；
- 生产者逐个按原因分类：`plugin_manager_adapters.cpp`（`PluginMount::*`）、
  `plugin_manager_vtable.cpp`（表入口与 helper）、`plugin_manager_runtime*.cpp`、
  `client_plugin_manager.cpp`、`builtin_tool_renderers.cpp`、`plugin_kit.h`；
- 文档：`docs/zh-cn/design/plugins.md` 新增 §8.3 返回码表与规则，`AGENTS.md` 增加一条硬规则；
  `plugin_api.h` / `client_plugin_api.h` 的 `return` 注释改成具体码名；
- 验证：全量构建 exit=0（含 25 个插件 DLL），全量测试 **42775 通过 0 失败**；
  `boundaries` 的用词规则、插件四件套与成员顺序清单仍全绿；
- 未做：FFI 的 `AGENTXX_FFI_ERR_*` 未动（独立嵌入 API）；插件 API 代次仍为 5。
