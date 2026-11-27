# 实施记录

## 进度

- [x] 1. 会话与存储 (LLM id / 整段替换 / 端点重推同步)
- [x] 2. 接口表 (会话表 v5 + 删 context 表 + API 代次 9)
- [x] 3. 主程序实现 (vtable 10 个成员 + PluginMount)
- [x] 4. 消费面 (kit / JS 桥 / 示例插件)
- [x] 5. 测试 (boundaries / host_mount / plugin_runtime 新用例)
- [x] 6. 文档 (plugins.md zh+en / index.md / AGENTS.md)
- [x] 7. 构建 + 跑测试

## 记录

### 1. 会话与存储

- `agent/lib/include/agentxx/agent/context.h` / `src/agent/context.cpp`
  - 新增 `messageIds_` (与 `messages_` 同下标的 id 表)、`llmMsgIdCounter_`、
    `nextLlmMessageId()` (`llm_000001` 递增)、`messageIdAt()`;
  - 新增 `llmMessagesJsonWithIds()` (逐条 `neograph::to_json` 后补 `id`)、
    `appendMessagesWithIds()`、`replaceMessagesWithIds()`;
  - 新增 `updateMessagesByIds()` (返回 `LlmMessageUpdateResult{updated, missing}`) 与
    `replaceViewMessages()` (整段替换展示历史, 返回重分配后的 id 列表);
  - `appendMessages` / `replaceMessages` / `setSystemMessage` / `truncateMessages` 一并维护 id;
  - `SessionStoreHooks` 表尾追加 `onRewriteViewMessages` (必须放最后, MSVC 指定初始化顺序)。
- `agent/lib/include/agentxx/agent/session_store.h` / `src/agent/session_store.cpp`
  - 新增 `viewMessageExists()` (`SELECT 1 ... WHERE msg_id = ?`, 给窗口外更新判定);
  - 新增 `rewriteViewMessages()` (单事务 DELETE + 按 seq=1..N 重写 + 写回 meta 计数)。
- `agent/lib/include/agentxx/agent/io/agent_io.h`: 新增虚函数 `onViewMessagesReplaced()`
  (默认空); `session_server_agent_io.{h,cpp}` 覆写为重推 `buildTailSync(effectiveSyncTailCount())`
  (`asio::dispatch(ex_, ...)`), 客户端 `ViewReplica` 因此按整份同步重建。

### 2. 接口表

- `agent/lib/include/agentxx/plugin/api/plugin_api.h`
  - 删除整张 `AgentxxPluginContextIface` 与 4 个 `AGENTXX_PLUGIN_IFACE_AGENT_CONTEXT_*` 宏;
  - `AgentxxPluginSessionIface` 抬到 v5 (`VERSION`/`MIN_VERSION` = 5, `BASELINE_SIZE` = `sizeof`),
    删除 `emit_message_tip`, 新增 10 个成员 (view / llm 各 `get_/count_/set_/append_/update_`),
    每个成员带载荷形态与 id 语义的中文注释 (含"流式中拒绝 set"、"视觉为空丢弃").
- `agent/lib/include/agentxx/plugin/api/plugin_iface_meta.h`: 删除 context 表特化。
- 全局代次: `agent/CMakeLists.txt` 的 `AGENTXX_PLUGIN_API_VERSION` 8 → 9 (含缓存纠正分支);
  `agent/test/plugin/test_plugin_abi_c17.c` 静态断言改 9。

### 3. 主程序实现

- `agent/lib/src/plugins/plugin_manager_vtable.cpp`
  - 删除 `xx_context_get_messages` / `xx_context_messages_count` / `xx_emit_message_tip` /
    `g_ifaceContext` 与分发分支;
  - 新增 10 个 vtable 函数与匿名 namespace 辅助 (`parseMessageArray` / `parseViewMessagesJson` /
    `parseLlmMessagesJson` / `idArrayJson` / `updateResultJson` / `mountOfOrThrow` /
    `sessionMessageWriteCall`), `g_ifaceSession` 按新顺序初始化。
- `plugin_mount.h` + `plugin_manager_vtable.cpp` 的 `PluginMount`: 新增
  `findSession` / 10 个同名方法与 `PluginMount` 访问器; 读方法在会话不存在时抛
  `TableOpError("... session '<id>' not found")`; append/update 发 `WireDelta`;
  set 后调 `onViewMessagesReplaced`。语义: `update_*` 回 `{"updated":N,"missing":[...]}`;
  `set_view_messages` 在 `streamingView` 非空时失败; 视觉为空的消息丢弃;
  `update_view_messages` 对窗口外 id 先用 `SessionStore::viewMessageExists` 判定。
- `PluginManager::kInterfaceTableCount` = 9 + 9 = 18。

### 4. 消费面

- `agent/lib/include/agentxx/plugin/api/agent_plugin_kit.h`: 删 `AgentIfaces::context` 与
  `getSessionMessages` / `sessionMessagesCount`; 新增 `sessionMessageOp` /
  `sessionMessageWriteOp` 骨架与 10 个协程入口 (`getViewMessages` … `updateLlmMessages`)。
- `agent/plugins/agentxx_javascript_engine/agentxx_javascript_engine.cpp`: 桥接 id 重排
  (`B_GET_LLM_MESSAGES` / `B_COUNT_LLM_MESSAGES` + 8 个新 id, 删 `B_EMIT_MESSAGE_TIP`),
  `def(...)` 注册新全局 (保留 `getMessages` / `messagesCount` 别名), 新增
  `sessionMessageReadJs` / `sessionMessageWriteJs` 两个桥接 lambda 与 10 个 case。
- `agent/plugins/example_graph_node/example_graph_node.cpp`: 改用 `ctx.getLlmMessages(...)`。
- `agent/plugins/example_js/plugin.js`: `js_sdk_probe` 增加 view / llm 五个成员的形状探测。

### 5. 测试

- `agent/test/core/test_boundaries.cpp`: 规则 7/8/9/10 的表数量改 18 / 24, 删 context 表项,
  更新会话表成员清单与四件套校验。
- `agent/test/plugin/test_plugin_host_mount.cpp`: 18 张表, 删 context 表断言。
- `agent/test/plugin/test_plugin_runtime.cpp`: 新增「会话消息表」用例块 (临时目录 +
  `SessionStore` + `SessionsManager`): 缺会话 / 缺 id / 非法 JSON / append 多条丢弃空正文 /
  count / get / update 含 missing / set 落库重建 / llm append+get+update+set 后旧 id 失效 /
  流式中拒 set。
- `agent/test/plugin/test_plugins.cpp`: JS 探针新增 10 条断言。
- 测试辅助函数 (`findPluginDir` 系列) 的目录判定: 由"目录里有 `.so/.dll/.dylib`"扩展为
  "有动态库, 或有 `plugin.yaml` + `plugin.js`" —— 纯脚本插件 (`example_js`) 没有动态库,
  见下面的"遇到的问题"。改动覆盖 17 个测试源文件的同名 lambda。

### 6. 文档

- `docs/zh-cn/design/plugins.md`: 18 张表 / 业务表 9 张; 会话表行改 v5; 删 context 表行;
  新增 §8.1.1 (两份消息的读写语义); 代次档位 v9; 测试表 18 张。
- `docs/zh-cn/design/index.md`: 图状态说明、目录树 18 张、接口表小节。
- `docs/en/design/plugins.md`: 两张表行 + 18 / 9 计数。
- `AGENTS.md`: 接口表数量 18、会话消息读写说明、代次 9/9、v9 档位、LLM 上下文归属段。

### 7. 构建与验证

- `agent/script/windows_debug_build.bat` 全量构建通过 (lib / client / test / 插件都到 API 9)。
- 通过模块 (断言数):
  `boundaries`(20)、`plugin_runtime`(817)、`plugin_sdk`(142)、`plugins`(723)、
  `plugin_cleanup`(95)、`plugin_dispatch`(119)、`plugin_executor`(670)、
  `plugin_resources`(102)、`plugin_host_mount`(147)、`plugin_multi_mount`(48)、
  `plugin_multi_instance`(82)、`client_plugins`(925)、`persist_semantics`(35)、
  `session_schema`(121)、`storage_consistency`(260)、`view_replica`(114)、
  `wire_roundtrip`(286)、`event_hub`(288)、`event_delivery`(62)、`async_tools`(178)、
  `plugin_bridge`(72)、`plugin_feature`(37)、`plugin_point`(73)、`plugin_broadcast`(102)、
  `plugin_agent_runtime`(222)、`plugin_example_runtime`(51)、`ffi_c_api`(487)、
  `math_tools`(82)、`codegraph`(28)、`cpu_gpu`(55)。

## 遇到的问题

- **纯脚本插件目录被当成"不存在"**: `exec/plugins/example_js/` 原来是纯脚本插件 (只有
  `plugin.yaml` + `plugin.js`), 里面留着上一代 C++ 壳的 `libexample_js.dll` / `.pdb`。
  换 API 代次后这些旧库必须清掉, 清掉后测试的 `findPluginDir("example_js")` 因"目录里
  没有动态库"返回空, `plugin_multi_instance` 报 `expected true at line 378`。
  处理: 目录判定扩展为"有动态库, 或有清单 + `plugin.js`"; `agent/plugins/example_js/CMakeLists.txt`
  本来就只拷资源 (纯脚本插件是设计形态, 不是缺产物)。
- **改写测试文件时行尾被写成 CRLF**: 用脚本批量改 17 个文件时 `Path.write_text` 按 Windows
  默认行尾写盘, 整文件变 CRLF (`git diff` 出现上万行噪声)。已用字节级 `\r\n` → `\n`
  还原, `git diff` 恢复为每文件 9 行新增。
- **`build_plugins.ps1` 对 `example_js` 报错**: 它是 `add_custom_target` 资源目标
  (`example_js_resources`), 没有 `.vcxproj`, 脚本逐个目标编译时报
  `MSBUILD : error MSB1009: 项目文件不存在` 并整体退出 1 (预先存在的问题, 其余插件都编成)。
  `plugin.js` 改动用手工拷贝到 `exec/plugins/example_js/plugin.js`。
