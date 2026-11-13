# 计划：跟进 cxx_utilxx / cxx_pluginxx 的结果码与网络错误名更新

## 1. 背景

`agent/third_party/` 下的两个依赖库被更新（父仓库记录的提交 vs 当前工作树）：

- `cxx_pluginxx`：`46a9812` → `eaaccbe`（3 个头文件）
- `cxx_utilxx`：`2fd4afc` → `ff9d6b8`（新增 1 个头文件 + `net_hub.cpp`）
- `cxx_utilxx_base`：`605109d`（未变）、`cxx_pluginxx_ui`：`fa4e323`（未变）

`git diff <旧> <新> --stat` 已确认改动范围只有上面两处，没有别的文件。

## 2. 现状调查（库侧到底改了什么）

### 2.1 `cxx_pluginxx`

| 文件 | 变化 |
|---|---|
| `include/pluginxx/api/result.h`（**新增**） | 全库唯一的返回码表。分段：成功 `0`；通用段 `-1 ~ -99`（`ARG` -1 / `STATE` -2 / `JSON` -3 / `NOT_FOUND` -4 / `TIMEOUT` -5 / `PERMISSION` -6 / `BUSY` -7 / `NO_MEMORY` -8）；广播段 `-200 ~ -299`（`NOT_DECLARED` / `NO_VALUE` / `ENDED` / `NOT_OWNER`）；生命周期段 `NOT_READY` -500；内部段 `INTERNAL` -999；应用自定义 `-5000 ~ -9999`、插件自定义 `-10000 ~ -19999`。带 `pluginxx::result::nameOf()` / `categoryOf()` |
| `include/pluginxx/api/net.h` | 删掉 `PLUGINXX_NET_OK/ERR_ARG/ERR_STATE/ERR_JSON/ERR_NOT_FOUND/ERR_TIMEOUT/ERR_PERMISSION/ERR_QUEUE_FULL/ERR_NO_MEMORY/ERR_INTERNAL` 十个宏，改为 `#include "pluginxx/api/result.h"`；注释里的判断码改成 `PLUGINXX_RESULT_OK` / `PLUGINXX_RESULT_ERR_NOT_FOUND`。网络段（-100 ~ -199）不在此表，属于实现网络的 `utilxx/net_error.h` |
| `include/pluginxx/api/tables.h` | 广播表的码也搬进 `result.h`，并且**重编号**：`not_declared` -2→-200、`no_value` -4→-201、`ended` -5→-202，新增 `not_owner` -203（旧 `permission` -3 现在统一走通用段 -6） |

### 2.2 `cxx_utilxx`

| 文件 | 变化 |
|---|---|
| `include/utilxx/net_error.h`（**新增**） | 网络段 `-100 ~ -199`：`INVALID_URL` -100 / `INVALID_HOST` -101 / `INVALID_PORT` -102 / `INVALID_ID` -103 / `EXISTS` -104 / `NOT_CONNECTED` -105 / `WRONG_KIND` -106 / `BIND_FAILED` -107 / `WRITE_FAILED` -108 / `UNSUPPORTED_OP` -109 / `CANCELED` -110，另有 `utilxx::net_error::nameOf()` |
| `src/net_hub.cpp` | 结果 JSON 的 `error`（字符串）统一到一份词表：`bad_args`/`bad_base64` → `invalid_argument`，`bad_json` → `invalid_json`，`too_many`/`too_many_requests` → `busy`，`cancelled` → `canceled`，`not_running`/`closed` → `not_connected`，`bad_url`/`unsupported_scheme` → `invalid_url`，`bad_host`/`bad_group` → `invalid_host`，`bad_port` → `invalid_port`，`bad_id` → `invalid_id`，`listen_failed`/`join_failed` → `bind_failed`，`no_data_dir` → `write_failed`（`exists` / `wrong_kind` / `unsupported_op` / `not_found` / `timeout` 不变）。`utilxx::net::*` 入口码常量**没动**（`Ok` 0、`ErrArg` -1 … `ErrInternal` -99） |

### 2.3 `cxx_utilxx_base`（指针未变，但 agentxx 一直没跟进）

记录指针 `605109d` 就是当前 HEAD，所以树差分为空 —— 但这条提交改了 `utf8IsAvail` 的语义：

- 空串 / 首字节 `'\0'` 由 `true` 改为 `false`；
- 新增 `allowBackstop` 参数，默认 **false**：含 U+FFFD 的文本算“不可用”，即这个函数现在筛的是
  “有内容且合法且不含占位符”；`utf8GetLengthCheckAvail` 的默认仍是 `allowBackstop = true`（只看合法性）。

agentxx 侧有三处调用点 + 三处测试断言按旧语义写的，全量测试里就是它们红（见 `work.md` 第 7 步）。

## 3. 影响面（本项目）

`grep -rn 'PLUGINXX_NET_|PLUGINXX_RESULT_|utilxx::net::'` 的结果，需要动的地方：

| 位置 | 现状 | 处理 |
|---|---|---|
| `agent/lib/include/agentxx/plugin/api/plugin_kit.h` 385/390/395/428~444/498/531/617/644/649/671/686/701/738/749/975/989/1023 | SDK 网络便捷层用 `PLUGINXX_NET_*` | 换 `PLUGINXX_RESULT_*`；`netCodeName()` 改成查 `pluginxx::result::nameOf()` |
| `agent/lib/src/plugins/net_service.cpp` 25/29/113/122/139/179/202/206/217 | `PluginNetService` 直接用 `utilxx::net::*` | 返回值做 -99 → -999 换算；`cancel` 里按名字判 `not_found` 名字没变，保留 |
| `agent/lib/src/plugins/plugin_manager_vtable.cpp` 1671/1676/1685/1704/1710/1737/1759/1764 | `.net` 表三个入口，异常兜底默认值是 `utilxx::net::ErrInternal` | 兜底改成 `PLUGINXX_RESULT_ERR_INTERNAL`（`ErrArg` -1 与通用段一致，保留） |
| `agent/lib/src/plugins/client_plugin_manager.cpp` 2290/2295/2320/2326/2353/2375/2380 | 同上（client 侧 `.net` 表） | 同上 |
| `agent/lib/include/agentxx/plugin/{net_service.h,plugin_manager.h,client_plugin_manager.h}` 的注释 | 写"返回 `utilxx::net::*` 结果码" | 改成"插件面结果码 (`PLUGINXX_RESULT_*`)" |
| `agent/test/core/test_chatbot_media_io.cpp` 473/501 | 测试侧 `TestNet` 用 `PLUGINXX_NET_*` | 换 `PLUGINXX_RESULT_*` |
| `agent/test/plugin/test_client_plugins.cpp` 552/568/579 | 同上 | 换 `PLUGINXX_RESULT_*` |
| `agent/plugins/agentxx_chatbot/chatbot_net.cpp` 注释 | 提到 `no_data_dir` | 改成 `write_failed` |
| `docs/zh-cn/design/plugins.md` 2239/2391/2417/2447 | 旧宏名、旧错误名 | 改宏名，补码表分段与错误名词表来源 |
| `AGENTS.md` 网络能力条目 | 只说"入口码只表示受理层面" | 补一句"入口码 = `result.h` 通用段，业务错误名 = 通用段名称 + `utilxx/net_error.h`" |

`agent/plugins/example_event/*`、`plugin_manager_domain_services.cpp`、`plugin_manager_runtime_trigger.cpp`
用的是宏名（`PLUGINXX_RESULT_OK` / `ERR_PERMISSION`），重编号不影响，无需改。

## 4. 方案

按"码表只留一份、数字只在 C 边界出现"的原则：

1. **SDK（`plugin_kit.h`）**
   - 宏改名：`NET_OK`→`RESULT_OK`、`NET_ERR_ARG`→`RESULT_ERR_ARG`、`NET_ERR_STATE`→`RESULT_ERR_STATE`、
     `NET_ERR_JSON`→`RESULT_ERR_JSON`、`NET_ERR_NOT_FOUND`→`RESULT_ERR_NOT_FOUND`、
     `NET_ERR_TIMEOUT`→`RESULT_ERR_TIMEOUT`、`NET_ERR_PERMISSION`→`RESULT_ERR_PERMISSION`、
     `NET_ERR_QUEUE_FULL`→`RESULT_ERR_BUSY`、`NET_ERR_NO_MEMORY`→`RESULT_ERR_NO_MEMORY`、
     `NET_ERR_INTERNAL`→`RESULT_ERR_INTERNAL`。
   - `detail::netCodeName(code)`：先查 `pluginxx::result::nameOf(code)`（与框架同一份名字），
     查不到再兜 -99（老主程序直接透传 `utilxx::net::ErrInternal`）为 `internal`，最后 `unknown`。
   - `NetAsyncState::code` 的默认值从 `INTERNAL` 改成 `STATE`：文档承诺"主程序没提供网络能力时
     `code` 为 `ERR_STATE`"，而 `await_ready()==true`（没有 `.net` 表）时返回的正是这个默认值。
     现在默认值是 -99，与承诺不符。
   - 文档注释里的宏名同步改。

2. **网络适配层（`net_service.cpp` / `net_service.h`）**
   - 加一个小函数把 `utilxx::net::ErrInternal`（-99）换成 `PLUGINXX_RESULT_ERR_INTERNAL`（-999），
     其余取值与通用段一一对应，原样返回。落在 `call` / `callAsync` / `cancel` 三个出口。
   - 头文件注释改成"返回插件面结果码（`PLUGINXX_RESULT_*` 通用段）"。

3. **`.net` 表入口（两侧 vtable）**
   - `guardVtableCall(utilxx::net::ErrInternal, …)` → `guardVtableCall(int32_t{PLUGINXX_RESULT_ERR_INTERNAL}, …)`，
     异常兜底也落在通用/内部段里。

4. **测试**：`test_chatbot_media_io.cpp` 的 `TestNet`、`test_client_plugins.cpp` 的断言换宏名。

5. **文档**：`plugins.md` §20.1 补广播段数字分段（并写明"判断用宏名，不要写数字"）；
   §22.1 换宏名并写清两层名词表来源；§22.2 / §22.4 的宏名同步；`AGENTS.md` 补一句。

## 5. 边界与不做的事

- 不手改 `cxx_pluginxx` / `cxx_utilxx` 源码。
- 不动 `utilxx::net::*` 入口码常量（仍与 `result.h` 通用段取值一致）。
- 不搬 agentxx 自己的业务表错误码（`AGENTXX_PLUGIN_ERR_*` 等，-1~-7）到 `result.h` 建议的
  -5000~-9999 区间：那要抬 `AGENTXX_PLUGIN_API_VERSION` 并改全部插件，是独立的一件事。
- 不引入广播 `not_owner`（-203）：`host_core` 目前对"不是属主/系统频道"仍返回通用段
  `PERMISSION`（-6），本项目 `publishBroadcastFrom` 保持一致即可。

## 6. 分步实施

1. `plugin_kit.h` 宏改名 + `netCodeName` + 默认值 + 注释
2. `net_service.{h,cpp}` 边界换算 + 注释
3. 两侧 vtable 的异常兜底默认值
4. 两个测试文件
5. 文档（plugins.md §20.1/§22、AGENTS.md）+ chatbot 注释
6. 编译（顶层 `windows_debug_build.bat`）+ 编插件目标
7. 跑测试模块并复查残留 grep

## 7. 验证

```powershell
# 编译 (顶层会刷新安装前缀里的头, 内层 test/client 才拿得到新头)
agent/script/windows_debug_build.bat
# 插件动态库也在这一轮里产出 (exec/plugins/<插件名>/*.dll), 单独编时用:
# pwsh -File agent/script/build_plugins.ps1 [插件名...]

# 测试 (全量)
agent/build/windows-debug/exec/agentxx_test.exe --log-level warn

# 无残留
grep -rn 'PLUGINXX_NET_' agent/lib agent/test agent/plugins agent/client docs
```

## 8. 风险

- 老主程序 + 新插件：插件 SDK 期望通用段，老主程序透传 -99 → `netCodeName` 已兜 `internal`；
  数值比较落空的只有 `ERR_INTERNAL`（本来就不该出现在正常路径）。
- 新主程序 + 老插件：老插件的 `PLUGINXX_NET_ERR_INTERNAL` 是 -99，主程序现在给 -999 ——
  同样是"不该出现"的分支；其余码取值完全一致，行为不变。
- 广播段重编号只影响写死数字的调用方（本项目没有）。
