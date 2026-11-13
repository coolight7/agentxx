# cxx_* 依赖库更新适配（统一结果码表 / 网络错误名 / utf8IsAvail 语义）

## 目标

跟进 `agent/third_party/` 下 cxx 依赖库的更新，让 agentxx 编译通过并保持对外契约自洽：

1. `cxx_pluginxx` 把返回码收成一份表（新 `pluginxx/api/result.h`），删掉了 `PLUGINXX_NET_*`
   宏并给广播段换了数字 —— 主程序与插件 SDK 里的旧宏必须改名；
2. `cxx_utilxx` 把网络的业务错误名（结果 JSON 的 `error`）统一到固定词表，并新增
   `utilxx/net_error.h` —— 本项目提到旧名字的注释、文档要跟着改；
3. 顺手把"插件面能看到的网络入口码"钉死在 `pluginxx/api/result.h` 的通用段（不额外发明）。
4. `cxx_utilxx_base` 的 `utf8IsAvail` 已改成“空串 / 含 U+FFFD 一律不可用，默认
   `allowBackstop = false`”（这条早就在当前指针里，但 agentxx 没跟上）——把调用点与
   测试断言对齐到“要合法 UTF-8 就传 `true`，要‘有内容且干净’就用默认”。

## 范围

- `agent/lib/include/agentxx/plugin/api/plugin_kit.h`（插件 SDK 的网络便捷层）
- `agent/lib/{include/agentxx/plugin/net_service.h,src/plugins/net_service.cpp}`（网络适配层）
- `agent/lib/src/plugins/plugin_manager_vtable.cpp`、`client_plugin_manager.cpp`（`.net` 表入口）
- `agent/third_party/cxx_pluginxx/include/pluginxx/api/result.h`（称呼修正: 宿主 → 主程序，3 处注释）
- `agent/test/{core/test_chatbot_media_io.cpp,plugin/test_client_plugins.cpp}`
- `agent/lib/src/agent/session_store.cpp`（`utf8IsAvail` 新语义）
- `agent/plugins/agentxx_chatbot/chatbot_net.cpp`、`chatbot_engine.cpp`（注释里的旧错误名、`utf8IsAvail` 新语义）
- `agent/test/{core/test_string_util.cpp,core/test_session_persistence.cpp}`
- `docs/zh-cn/design/plugins.md` §20.1 / §22、`AGENTS.md`（网络能力条目、`utf8IsAvail` 调用规则）

## 结论（事后来看这一页就够）

| 变化 | 影响 | 改法 |
|---|---|---|
| `PLUGINXX_NET_OK/ERR_*` 删除，码表统一到 `pluginxx/api/result.h` | SDK 与两个测试文件共 4 个文件编译不过 | 全部换成 `PLUGINXX_RESULT_*`（`ERR_QUEUE_FULL`→`ERR_BUSY`、`ERR_INTERNAL` 的取值 -99 → -999）；`netCodeName()` 改为查 `pluginxx::result::nameOf()`，只给老主程序留下的 -99 兜一个 `internal` |
| 广播段重编号：`not_declared` -2→-200、`no_value` -4→-201、`ended` -5→-202，新增 `not_owner` -203 | 代码里都用宏名，没有数字比较，无需改代码 | 文档写清"数字只到 C 边界，判断用宏名"，列一次新旧分段 |
| 网络结果 JSON 的 `error` 名统一词表（`bad_args`→`invalid_argument`、`too_many`→`busy`、`cancelled`→`canceled`、`not_running`→`not_connected`、`bad_url`→`invalid_url`、`listen_failed`/`join_failed`→`bind_failed`、`no_data_dir`→`write_failed` …） | 本项目只有 `net_service.cpp` 按名字判 `not_found`（名字没变）与 chatbot 的一处注释提到 `no_data_dir` | 判断处不变；注释、文档改成新名，并在 §22 列出词表来源（通用段 `result.h` / 网络段 `utilxx/net_error.h`） |
| `utilxx::NetHub` 内部错误仍返回 -99，而插件面的"内部错误"是 -999 | 插件比较 `PLUGINXX_RESULT_ERR_INTERNAL` 会落空 | `.net` 表边界做一次换算（`PluginNetService` 返回处 + 两个 vtable 的异常兜底默认值） |
| 新的 `result.h` 注释里用了已停用的称呼 `宿主` | `boundaries` 规则 15（称呼与用词）3 条违规，该规则明确也检查自研的 cxx_* | 把 cxx_pluginxx 的 `result.h` 三处 `宿主` 改成 `主程序`（纯注释，不改行为与 ABI；库仓库需要一起提交） |
| `cxx_utilxx_base` 的 `utf8IsAvail` 新语义（空串 / 含 U+FFFD 一律算“不可用”，默认 `allowBackstop=false`）早就在当前指针里，但 agentxx 没跟上 | 全量测试 3 条红（`string_util` 2 条、`session_persistence` 1 条），且客户端会上两个调用点会把“含 U+FFFD 的合法文本”当非法处理 | 三处调用点按意图传 `allowBackstop = true`（`dumpJsonUtf8` / `saveLlmMessages` / chatbot 文本附件判定），三处测试断言跟上新语义 |

## 偏差

- agentxx 自己的业务表错误码（`AGENTXX_PLUGIN_ERR_*` / `AGENTXX_PLUGIN_RUNTIME_ERR_*` 等，
  取值 -1~-7）落在 `result.h` 的通用段区间里。`result.h` 建议应用自定义码用 -5000~-9999，
  但那是一次对外码表搬迁（要抬 `AGENTXX_PLUGIN_API_VERSION` 并改全部插件），与本次库更新
  无关，未做，只记录在此。
- 除上面那三行注释（`宿主` → `主程序`）外，未改 `cxx_pluginxx` / `cxx_utilxx` /
  `cxx_utilxx_base` 的任何代码；本项目的适配全部落在 agentxx 自己的源码与文档里。
