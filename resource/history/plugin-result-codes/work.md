# 实施记录

## 进度

- [x] 调查现有码表与裸 `0`/`-1` 的分布
- [x] 新增 `agentxx_result.h`（码表 + `resultName`）
- [x] `plugin_manager_adapters.cpp`（注册/注销的真实生产者）
- [x] `plugin_manager_vtable.cpp`（agent 侧表入口）
- [x] `plugin_manager_runtime*.cpp`（运行时表）
- [x] `client_plugin_manager.cpp`（client 六张表）
- [x] `plugin_kit.h` + 内置渲染器
- [x] 内置插件 + 示例
- [x] 测试
- [x] 文档（plugins.md §8.3 / AGENTS.md）
- [x] 编译 + 全量测试

## 第 1 步：调查

（命令与结论见 `plan.md` 的"现状调查"）

## 第 2 步：码表

新增 `agent/lib/include/agentxx/plugin/api/agentxx_result.h`：

- `AGENTXX_RESULT_OK` = `PLUGINXX_RESULT_OK`（另有 `static_assert` 兜住两边一致）；
- 失败段 `-5000 ~ -5010`：`ERR_ARG` / `ERR_JSON` / `ERR_STATE` / `ERR_NOT_FOUND` /
  `ERR_NO_AGENT` / `ERR_NOT_OWNER` / `ERR_CONFLICT` / `ERR_LIMIT` / `ERR_UNSUPPORTED` /
  `ERR_FAILED` / `ERR_INTERNAL`；
- C++ 侧 `agentxx::plugin::resultName(code)`（名字与 `pluginxx::result::nameOf` 同一套用词）；
- `plugin_api.h` / `client_plugin_api.h` 都 include 它，插件只看任一业务表头就拿到码表。

同时删掉 `plugin_api.h` 里旧的 `AGENTXX_PLUGIN_ERR_NO_AGENT`(-2) 与
`AGENTXX_PLUGIN_RUNTIME_*`(-1~-7) 两块宏（改为指向新码表的说明注释）。

## 第 3 步：全树改名

`AGENTXX_PLUGIN_ERR_NO_AGENT` → `AGENTXX_RESULT_ERR_NO_AGENT`；
`AGENTXX_PLUGIN_RUNTIME_ERR_BUDGET` → `AGENTXX_RESULT_ERR_LIMIT`；
`AGENTXX_PLUGIN_RUNTIME_OK/ERR_*` → `AGENTXX_RESULT_OK/ERR_*`。

涉及：`plugin_kit.h`、`plugin_manager.h`、`plugin_mount.h`、`plugin_manager_runtime*.cpp`、
`plugin_manager_vtable.cpp`、`agentxx_chatbot.cpp`、`example_runtime.cpp`、
`test_plugin_agent_runtime.cpp`、`test_plugin_host_mount.cpp`、`plugins.md`、`AGENTS.md`。

## 第 4 步：生产者逐个分类（真做判断的地方）

| 文件 | 改了什么 |
|------|----------|
| `plugin_manager_adapters.cpp` | `PluginMount::*` 注册/注销/权限/图/资源成员：按"引用无效 / 别人家的对象 / 重名 / 不存在 / 参数错 / 落盘失败"分别给码 |
| `plugin_manager_vtable.cpp` | 表入口与 helper（`queryStringIo` / `queryScopedStringIo` / `onScopedInstanceIo` / `mountCall`）：`!out` → `ERR_ARG`、空正文 → `ERR_NOT_FOUND`、`scopeOk` 失败 → `ERR_NO_AGENT`、异常兜底 → `ERR_INTERNAL` |
| `plugin_manager_runtime.cpp`、`plugin_manager_runtime_trigger.cpp` | 运行时表 16 个入口：改名 + 裸 `-1`/`0` 按语义落码 |
| `client_plugin_manager.cpp` | client 六张表入口 + `ClientPluginManager::*`：同上分类；`send_plugin_data` 的 `-2`（名字非法）→ `ERR_ARG`、投递失败 → `ERR_FAILED` |
| `builtin_tool_renderers.cpp` | 内置渲染器的 `0`/`-1` → `AGENTXX_RESULT_OK` / `ERR_ARG` |
| `plugin_kit.h` | 没实现能力 → `ERR_UNSUPPORTED`；空参数 → `ERR_ARG`（原先把"无接口"和"参数错"混在一个 `return -1` 里，拆成两段）；`!= 0` 判定改 `!= AGENTXX_RESULT_OK` |

**保持原样的地方**（不是返回码，`agentxx_result.h` 头里也写了这条）：

- 句柄类：`add_share_store`(-1)、`open_channel`/`add_trigger`/`addRuntimeTimer`(0)、
  `write_session_messages`/`messages_count` 返回条数；
- 0/1 查询：`is_canceled` / `is_visible`；
- 框架生命周期入口（`create`/`start`/`stop`/挂载事务）：约定仍是"0 成功、非 0 失败"，
  主程序只判非 0 并记日志，具体码不参与分支（内置插件与示例保持 `-1`）；
- `NetResult::status()`（HTTP 状态码）与网络结果 JSON 的 `ok`（业务结论）。

## 第 5 步：插件与测试核对

- `grep 'AGENTXX_PLUGIN_RUNTIME_|AGENTXX_PLUGIN_ERR_'` 已清零（只剩历史记录里的描述）；
- 插件侧没有任何 `== -1` / `!= -1` / `== -2` 的码比较（改名前后都只会拿宏比），
  `!= 0` / `== 0` 判定语义不变（`OK` 仍是 0）；
- 测试侧没有写死数字的断言（`-5000`/`-2` 一类都没有），断言全走宏名。

## 第 6 步：文档

- `docs/zh-cn/design/plugins.md`：新增 **§8.3 返回码 (`agentxx_result.h`)**（整张表 + 规则：
  段位、不写数字、句柄/计数不是返回码、生命周期入口只判非 0、新增码的条件）；
  §8.2 段末的"读取类返回 -1"改成"读句柄/计数的成员返回 -1"并指向 §8.3；
  `agentxx.agent.mount` 的 `get` 改为 `ERR_NOT_FOUND`；§19.2 的返回码段改为新码表（`ERR_BUDGET` → `ERR_LIMIT`）。
- `plugin_api.h` / `client_plugin_api.h`：各成员的 `return` 注释从"非 0"改成具体码名。
- `AGENTS.md`：§"接口表版本与基线"下补一条硬规则（业务表入口只用 `AGENTXX_RESULT_*`，
  不写裸 `-1`/`0`，判定用宏名，句柄/计数/0-1 查询不算返回码）。

## 第 7 步：编译与测试

- `agent/script/windows_debug_build.bat` 全量构建 4 轮，最终 exit=0（无新增告警：
  改动文件里只剩原有的 C4100/C4456）。
- `agent/script/build_plugins.ps1` 编出全部 25 个插件 DLL（`exec/plugins/<插件>`）。
- 全量测试 `agentxx_test.exe --log-level warn`：**passed=42775 failed=0**。
- 定点重跑 `boundaries plugins plugin_cleanup plugin_agent_runtime plugin_sdk plugin_bridge
  plugin_host_mount plugin_multi_mount client_plugins`：passed=2211 failed=0。

中途出现的 4 处红与原因（都是断言写死旧码，不是实现问题）：

| 测试 | 断言 | 处理 |
|------|------|------|
| `boundaries` | 规则 15 用词检查 | `agentxx_result.h` 头部注释里的"宿主"改回"主程序"（`plan.md` 引用的那句同步改） |
| `test_plugins.cpp:239` | `get_tool_prompt` 未知工具 == `-1` | 改断言为 `AGENTXX_RESULT_ERR_NOT_FOUND` |
| `test_plugin_cleanup.cpp:407` | 非占用者 `setGraphJson` == `-1` | 改断言为 `AGENTXX_RESULT_ERR_NOT_OWNER` |
| `test_plugin_agent_runtime.cpp:1070` | `list_runtime(out=nullptr)` == `-1` | 改断言为 `AGENTXX_RESULT_ERR_ARG` |

## 第 8 步：复核与偏差

- `grep 'AGENTXX_PLUGIN_RUNTIME_|AGENTXX_PLUGIN_ERR_'` 在 `agent/lib`、`agent/client`、
  `agent/plugins`、`agent/test`、`docs`、`AGENTS.md` 上为零；`git diff --check` 干净；
  改过的文件无 CR（全 LF）。
- 新头已随 `agentxx_lib_repo` 的 install 步骤落到
  `{build}/agentxx-project-install/include/agentxx/plugin/api/agentxx_result.h`，
  test/client 的 `find_package(agentxx_static)` 能直接看到它。
- 偏差 1：`agent/plugins/**` 只做了"宏改名 + 语义核对"，**没有**把框架生命周期入口
  （`create` / `start` / `stop` / 挂载事务）与插件内部 helper 的 `-1` 改成码表值 ——
  那些返回值按约定只判"非 0 = 失败"，主程序只记日志，码不参与分支；改它们只是噪声。
- 偏差 2：句柄/计数成员（`add_share_store`、`open_channel`、`add_trigger`、
  `addRuntimeTimer`、`messages_count`、`write_session_messages`）保持 `-1`/`0` 语义，
  已在 `agentxx_result.h` 头部与 §8.3 写明"不是返回码"。
- 偏差 3：FFI 的 `AGENTXX_FFI_ERR_*` 不动（独立嵌入 API，有 `agentxx_ffi_strerror`）；
  插件 API 代次保持 5，不抬。
- 偏差 4：`client_plugin_manager.cpp` 的 `send_plugin_data` 原来用 `-2` 表示"名字不合法"，
  现在那个值改成 `ERR_ARG`；投递失败 `-1` → `ERR_FAILED`。- 收尾又统一了两处"判断回调结果"的写法（值不变，只说清语义）：
  `client_plugin_manager.cpp` 里两处工具特化渲染器调用点 `rc == 0` → `rc == AGENTXX_RESULT_OK`
  （初值 `-1` → `AGENTXX_RESULT_ERR_INTERNAL`），`builtin_tool_renderers.cpp` 的
  `runRenderer` 与 `plugin_manager_vtable.cpp` 的 `check_paths` 判定同样改成与 `AGENTXX_RESULT_OK` 比。
  改完重新构建 + 插件重编，定点跑 `client_plugins plugins plugin_cleanup plugin_agent_runtime
  ui_items tui_ui_items`：passed=2067 failed=0。