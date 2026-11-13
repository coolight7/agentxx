# 实施记录

## 进度

- [x] 调查两个依赖库的改动范围（`git diff <旧提交> HEAD --stat`）
- [x] 列出本项目受影响的位置（grep）
- [x] `plugin_kit.h`：宏改名 + `netCodeName` + 默认值
- [x] `net_service.{h,cpp}`：边界换算 -99 → -999
- [x] 两侧 vtable：异常兜底默认值
- [x] 测试：`chatbot_media_io` / `client_plugins`
- [x] 文档：`plugins.md` §20.1/§22、`AGENTS.md`、chatbot 注释
- [ ] 编译 + 跑测试

## 第 1 步：调查

两个库的树差分（相对父仓库记录的提交）：

```
cxx_pluginxx  46a9812 -> eaaccbe
  include/pluginxx/api/net.h    |  20 +++----   (PLUGINXX_NET_* 删除, 改 include result.h)
  include/pluginxx/api/result.h | 128 +++++++++ (新增: 唯一码表 + nameOf/categoryOf)
  include/pluginxx/api/tables.h |  12 ++--     (广播段重编号 -2/-4/-5 -> -200/-201/-202, 新增 not_owner -203)
cxx_utilxx    2fd4afc -> ff9d6b8
  include/utilxx/net_error.h |  55 +++++++++      (新增: 网络段 -100~-110 + nameOf)
  src/net_hub.cpp            | 149 +++++++------- (结果 JSON 的 error 名统一词表)
```

`cxx_utilxx_base`（605109d）与 `cxx_pluginxx_ui`（fa4e323）指针没变，无需处理。
两个库的 `install(DIRECTORY include/ ...)` 是整目录安装，新头文件不需要改 CMake。
接口表布局没动（`net.h`/`tables.h` 只改注释与宏），所以 `AGENTXX_PLUGIN_API_VERSION`
保持 5，不用抬代次。

## 第 2 步：SDK（`agent/lib/include/agentxx/plugin/api/plugin_kit.h`）

- 宏改名（取值除 `ERR_INTERNAL` 外都一样）：
  `NET_OK`→`RESULT_OK`、`NET_ERR_ARG`→`RESULT_ERR_ARG`、`NET_ERR_STATE`→`RESULT_ERR_STATE`、
  `NET_ERR_JSON`→`RESULT_ERR_JSON`、`NET_ERR_NOT_FOUND`→`RESULT_ERR_NOT_FOUND`、
  `NET_ERR_TIMEOUT`→`RESULT_ERR_TIMEOUT`、`NET_ERR_PERMISSION`→`RESULT_ERR_PERMISSION`、
  `NET_ERR_QUEUE_FULL`→`RESULT_ERR_BUSY`、`NET_ERR_NO_MEMORY`→`RESULT_ERR_NO_MEMORY`、
  `NET_ERR_INTERNAL`（-99）→`RESULT_ERR_INTERNAL`（-999）。
- `detail::netCodeName`：先查 `pluginxx::result::nameOf()`（与框架同一份名字），
  再兜老主程序直接透传的 -99 为 `internal`，最后 `unknown`。原先自己一套 `bad_args` /
  `not_available` / `queue_full` 等名字全部删掉。
- `NetAsyncState::code` 默认值 `ERR_INTERNAL` → `ERR_STATE`：文档承诺"主程序没提供网络能力时
  `code` 为 `ERR_STATE`"，而 `await_ready()==true`（没有 `.net` 表）时返回的正是这个默认值。
- 文档注释里的宏名同步改（两处）。

验证：`grep -rn 'PLUGINXX_NET_' agent/lib agent/test agent/plugins` 为空。

## 第 3 步：网络适配层与表入口

- `net_service.cpp` 加 `pluginNetCode()`：`utilxx::net::ErrInternal`（-99）→
  `PLUGINXX_RESULT_ERR_INTERNAL`（-999），其余原样（`utilxx::net::*` 与结果码表通用段取值
  一一对应）。落在 `call` / `callAsync` / `cancel` 三个出口。
- `net_service.h`：注释改成"返回插件面结果码 `PLUGINXX_RESULT_*` 通用段"，
  类注释补一句入口码换算。
- `plugin_manager_vtable.cpp` / `client_plugin_manager.cpp`：`.net` 表三个入口的异常兜底
  `utilxx::net::ErrInternal` → `PLUGINXX_RESULT_ERR_INTERNAL`，参数错误 → `PLUGINXX_RESULT_ERR_ARG`，
  成功判断 → `PLUGINXX_RESULT_OK`；两个文件里 `utilxx::net::` 已无引用，顺手去掉
  `#include "utilxx/net_hub.h"`（`client_plugin_manager.cpp` 的 `utilxx::offloadAsync` 用的是
  `utilxx/async_offload.h`，不受影响）。
- `plugin_manager.h` / `client_plugin_manager.h` 的 `netCall` 注释同步。

## 第 4 步：测试

- `test/core/test_chatbot_media_io.cpp`：`TestNet` 的 `callAsyncThunk` / `cancelThunk`
  用 `PLUGINXX_RESULT_ERR_ARG` / `PLUGINXX_RESULT_OK`。
- `test/plugin/test_client_plugins.cpp`：`PLUGINXX_RESULT_OK` / `PLUGINXX_RESULT_ERR_NOT_FOUND`。
- `test/plugin/test_plugin_net.cpp`、`test_plugin_broadcast.cpp` 等比较的是 `utilxx::net::*` 与
  `PLUGINXX_RESULT_*` 宏名，取值未变，不需要改。

## 第 5 步：文档与注释

- `docs/zh-cn/design/plugins.md`
  - §20.1 表格后补一段：结果码只有 `pluginxx/api/result.h` 一份，列出通用段 / 广播段 / 内部段
    的数字，并写明"判断用宏名，不要写数字"（广播段 2026-11 从 -2/-4/-5 改成 -200/-201/-202）。
  - §22.1：入口码 `PLUGINXX_NET_*` → `PLUGINXX_RESULT_*` 并说明实现内部 -99 在表入口换成 -999；
    新增一段"结果 JSON 的 `error` 是名字"的词表来源（通用段 `result.h` 的 `nameOf` + 网络段
    `utilxx/net_error.h` 的 `nameOf`），列出旧名已经不再产生。
  - §22.2 / §22.4：`PLUGINXX_NET_ERR_STATE` / `PLUGINXX_NET_ERR_TIMEOUT` → `PLUGINXX_RESULT_*`。
- `AGENTS.md` 网络能力条目：补入口码来源、-99 → -999 换算、错误名词表与旧名不再产生。
- `agent/plugins/agentxx_chatbot/chatbot_net.cpp` 注释里的例子 `no_data_dir` → `write_failed`。

## 第 6 步：编译与测试

- `agent/script/windows_debug_build.bat` 全量重建通过（exit=0，40 分钟；cxx_utilxx / cxx_pluginxx
  先重建安装新头，再编 lib / client / test）。新的 `pluginxx/api/result.h` 里的 `宿主`
  撞上了 `boundaries` 的称呼规则（该规则明确也查自研的 cxx_*）；
  已把 cxx_pluginxx 的 `result.h` 三处 `宿主` 改成 `主程序`（纯注释，不涉行为与 ABI，
  **库仓库那边仍需提交**）。
- 测试（debug）：

| 模块 | 结果 |
|---|---|
| `boundaries` | 19/19（修完称呼后） |
| `plugin_net` | 35/35 |
| `plugin_broadcast` | 90/90 |
| `plugin_runtime` / `plugin_sdk` / `plugin_bridge` | 726 / 133 / 193 |
| `plugin_feature` / `plugin_point` / `plugin_dispatch` / `plugin_resources` | 34 / 68 / 112 / 101 |
| `plugin_cleanup` / `plugin_host_mount` / `plugin_multi_mount` | 90 / 146 / 47 |
| `plugin_agent_runtime` / `plugin_example_runtime` / `plugin_example_chatbot` | 221 / 50 / 265 |
| `plugin_multi_instance` | 80 |
| `chatbot_media_io` / `client_plugins` | 60 / 763 |

以上模块全部 0 失败。随后跟了两轮全量套件：

| 轮次 | 结果 |
|---|---|
| 全量第一次 | `passed=42770 failed=3`（`string_util` 2 条、`session_persistence` 1 条，全部是 `utf8IsAvail` 旧语义，见第 7 步） |
| 修完第二轮 | `passed=42775 failed=0`（exit=0） |

## 第 7 步：全量测试发现的第二处库侧变化（`utf8IsAvail` 语义）

全量套件跑出 3 条失败（`string_util` 2 条、`session_persistence` 1 条），全部指向
`cxx_utilxx_base` 的 `605109d string_util: utf8IsAvail 统一成 mediaxx 的判定`：

- 空串 / 首字节是 '\0' → 由 `true` 改为 `false`；
- 新增 `allowBackstop` 参数（默认 **false**）：默认把含 U+FFFD 的文本归为"不可用"，
  即 `utf8IsAvail(s)` 现在筛的是"有内容且合法且不含占位符"，而 `utf8GetLengthCheckAvail`
  的默认仍是 `allowBackstop = true`（只看合法性）。

这条提交早在父仓库记录的指针里（base 指针未变），所以不在第一步的树差分里，但 agentxx
侧从未跟进，表现为测试红灯。逐处处理：

| 位置 | 改动 | 原因 |
|---|---|---|
| `agent/lib/src/agent/session_store.cpp` `dumpJsonUtf8` | `utf8IsAvail(text, true)` | 这里只关心"落库文本是不是合法 UTF-8"（simdjson 能不能解析）。默认参数会把含 U+FFFD 的文本当成不可用，导致每段带占位符的文本都白跑一遍修复 |
| `agent/lib/src/agent/session_store.cpp` `saveLlmMessages` | 同上 | 同上 |
| `agent/plugins/agentxx_chatbot/chatbot_engine.cpp` 文本附件判定 | `utf8IsAvail(content, true)` | 保持原行为：只拒真正的非法字节；文件里出现 U+FFFD 是合法字符，不应因此不内联 |
| `agent/test/core/test_string_util.cpp` | `utf8IsAvail("")` 改为期望 `false`；修复产物用 `utf8IsAvail(s, true)` 并另断言默认判定为 `false` | 跟上新语义 |
| `agent/test/core/test_session_persistence.cpp` | 修复后的文本用 `utf8IsAvail(text, true)` | 修复产物按定义含 U+FFFD |

`utf8Repair` / `utf8GetLengthCheckAvail` 的默认行为未变，`modelcall.cpp` / `test_training.cpp` /
benchmark 的调用点不受影响（全量套件里只有上面 3 条红）。

## 剩余/注意

- 未搬 agentxx 自己的业务表错误码（-1~-7 → `result.h` 建议的 -5000~-9999）：
  那要抬插件 API 代次，属于独立改动（写进 `index.md` 的偏差）。
- `cxx_pluginxx` 现在是“指针更新 + 本地 3 处称呼修正”的状态，提交库仓库时把这三行一起带上。
- `cxx_utilxx_base` 的指针没变，但 `utf8IsAvail` 的新语义是本次才跟上的（见第 7 步）；
  以后自己写调用时记住：**要“有没有可用文本”就用默认参数，要“是不是合法 UTF-8”就传
  `allowBackstop = true`**。
