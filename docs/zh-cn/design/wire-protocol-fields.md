# Wire 协议字段清单

> 本文是**生成物**, 请勿手工编辑。字段与示例值的完整形态见
> `agent/schema/wire-schema.json`。
>
> 重新生成: 设 `AGENTXX_UPDATE_WIRE_SCHEMA=1` 运行测试模块 `wire_schema`
> (生成后请人工 review diff 再提交)。
>
> 相关文档: [index.md](index.md) · [配置与设置边界](configuration.md)

协议版本: `1` · 消息类型数: 48

字段类型按序列化后的 JSON 取值推导 (`array<T>` 表示数组, `object{...}` 展开一层
字段); 省略的字段表示该字段可缺省 (取零值/默认值), 老对端不认识的新字段会被忽略。

| # | 消息 | `type` | 字段 (JSON 类型) |
|---:|---|---|---|
| 0 | hello | `hello` | `type` string, `sessionId` string, `token` string, `lastSeq` integer, `tailHash` string, `language` string, `afterViewSeq` integer, `protocolVersion` integer, `capabilities` array<string> |
| 1 | hello_ack | `hello_ack` | `type` string, `ok` boolean, `sessionId` string, `tailHash` string, `models` array<string>, `protocolVersion` integer, `capabilities` array<string>, `deviceId` string, `workDir` string, `plugins` array<object{name:string, version:string, interfaces:array<string>}> |
| 2 | user_input | `user_input` | `type` string, `sessionId` string, `text` string, `model` string, `delivery` string, `requestId` integer, `attachments` array<object{type:string, display_name:string, mime_type:string, path_or_url:string, data_url:string, size_bytes:integer}> |
| 3 | input_ack | `input_ack` | `type` string, `status` string, `requestId` integer, `sessionId` string, `delivery` string, `reason` string, `detail` string, `itemId` string |
| 4 | cancel | `cancel` | `type` string, `sessionId` string |
| 5 | select_model | `select_model` | `type` string, `sessionId` string, `model` string |
| 6 | interrupt_request | `interrupt_request` | `type` string, `id` integer, `sessionId` string, `node` string, `value` string, `argJson` string |
| 7 | interrupt_response | `interrupt_response` | `type` string, `id` integer, `result` object{allow:string} |
| 8 | interrupt_expired | `interrupt_expired` | `type` string, `id` integer, `sessionId` string |
| 9 | delta | `delta` | `type` string, `seq` integer, `text` string, `msgId` string, `tool_name` string, `tool_call_id` string, `arguments` string, `result` string, `historyCount` integer, `tailHash` string, `startTimeMs` integer, `durationMs` integer, `tps` number, `nodeName` string, `message` object{role:string, text:string, startTimeMs:integer, durationMs:integer}, `kind` string |
| 10 | sync | `sync` | `fromIndex` integer, `tailHash` string, `deltaSeq` integer, `totalMessages` integer, `lastViewSeq` integer, `incremental` boolean, `messages` array<object{role:string, text:string, startTimeMs:integer, durationMs:integer}>, `message_queue` array<object{id:string, text:string, createdAtMs:integer, model:string, delivery:string, recovered:boolean}>, `queue_state` string, `type` string |
| 11 | turn_result | `turn_result` | `type` string, `sessionId` string, `hasError` boolean, `interrupted` boolean, `errorMessage` string, `startTimeMs` integer, `durationMs` integer |
| 12 | context_stats | `context_stats` | `type` string, `contextTokens` integer, `maxContextTokens` integer, `tps` number |
| 13 | error | `error` | `type` string, `code` integer, `message` string |
| 14 | log | `log` | `type` string, `level` integer, `message` string |
| 15 | get_model | `get_model` | `type` string, `sessionId` string |
| 16 | model_info | `model_info` | `type` string, `currentModel` string, `models` array<string>, `capabilities` array<object{name:string, image_input:boolean, audio_input:boolean, video_input:boolean}> |
| 17 | get_append_component_info | `get_append_component_info` | `type` string, `sessionId` string |
| 18 | append_component_info | `append_component_info` | `type` string, `notifications` array<object{type:integer, name:string, success:boolean, errorMessage:string}> |
| 19 | get_context | `get_context` | `type` string, `sessionId` string |
| 20 | compact_context | `compact_context` | `type` string, `sessionId` string |
| 21 | context_messages | `context_messages` | `type` string, `messages` array<object{role:string, content:string}> |
| 22 | list_sessions | `list_sessions` | `type` string, `beforeMs` integer, `beforeId` string, `limit` integer |
| 23 | session_list | `session_list` | `type` string, `sessions` array<object{sessionId:string, lastActiveMs:integer, title:string}>, `totalCount` integer, `hasMore` boolean |
| 24 | switch_session | `switch_session` | `type` string, `sessionId` string |
| 25 | plugin_data | `plugin_data` | `type` string, `plugin` string, `event` string, `data` string |
| 26 | plugin_data_up | `plugin_data_up` | `type` string, `plugin` string, `event` string, `data` string |
| 27 | message_queue_update | `message_queue_update` | `type` string, `sessionId` string, `state` string, `items` array<object{id:string, text:string, createdAtMs:integer}> |
| 28 | clear_message_queue | `clear_message_queue` | `type` string, `sessionId` string |
| 29 | remove_queue_item | `remove_queue_item` | `type` string, `sessionId` string, `itemId` string |
| 30 | interrupt_and_run_next | `interrupt_and_run_next` | `type` string, `sessionId` string |
| 31 | get_view_messages | `get_view_messages` | `type` string, `sessionId` string, `beforeIndex` integer, `count` integer |
| 32 | view_messages_page | `view_messages_page` | `type` string, `sessionId` string, `startIndex` integer, `totalCount` integer, `messages` array<object{role:string, text:string, startTimeMs:integer, durationMs:integer}> |
| 33 | list_dir | `list_dir` | `type` string, `reqId` integer, `path` string, `allowedExtensions` array<string> |
| 34 | list_dir_result | `list_dir_result` | `type` string, `reqId` integer, `ok` boolean, `currentDir` string, `parentDir` string, `entries` array<object{name:string, fullPath:string, isDir:boolean, supported:boolean, sizeBytes:integer, mediaType:integer}> |
| 35 | get_permission_state | `get_permission_state` | `type` string |
| 36 | set_full_auth | `set_full_auth` | `type` string, `fullAuth` boolean |
| 37 | permission_state | `permission_state` | `type` string, `fullAuth` boolean |
| 38 | add_model | `add_model` | `type` string, `sessionId` string, `name` string, `modelType` string, `baseUrl` string, `apiKey` string, `modelName` string, `apiPath` string, `modelContextMaxToken` integer, `maxConcurrentConnections` integer, `connectTimeoutSeconds` integer, `readChunkTimeoutSeconds` integer, `sslVerify` boolean, `sendThinking` boolean, `requestReasoningSummary` boolean, `imageInput` boolean, `extraApiConfig` object{k:integer}, `extraHeaders` object{H:string} |
| 39 | add_model_result | `add_model_result` | `type` string, `ok` boolean, `name` string, `error` string |
| 40 | rename_session | `rename_session` | `type` string, `sessionId` string, `title` string |
| 41 | rename_session_result | `rename_session_result` | `type` string, `ok` boolean, `sessionId` string, `title` string |
| 42 | remove_model | `remove_model` | `type` string, `sessionId` string, `name` string |
| 43 | remove_model_result | `remove_model_result` | `type` string, `ok` boolean, `name` string |
| 44 | host_tool_register | `host_tool_register` | `type` string, `sessionId` string, `tools` array<object{name:string, description:string, inputSchema:object, timeoutSec:integer, maxConcurrent:integer}> |
| 45 | host_tool_unregister | `host_tool_unregister` | `type` string, `sessionId` string, `names` array<string> |
| 46 | host_tool_call | `host_tool_call` | `type` string, `callId` integer, `sessionId` string, `name` string, `argsJson` string, `timeoutSec` integer |
| 47 | host_tool_result | `host_tool_result` | `type` string, `callId` integer, `ok` boolean, `resultJson` string, `errorMessage` string |

## 说明

- 每个 `type` 对应 `WireMessage` 变体的一个成员; 服务端按 `type` 分派, 未知
  `type` 返回 `nullopt` (连接不断开)。
- `error.code` 取值见 `WireErrorCode`: 0 internal / 1 invalid state / 2 session not
  found / 3 session mismatch / 4 invalid args / 5 message not found; 未知码按 0 处理。
- 连接阶段取值见 `WireConnectionStage`: unhandshaken / unbound / ready /
  reconnecting / draining (未握手前只接受 `hello`)。
- 心跳 (`ping` / `pong`) 是裸 JSON, 不进入 `WireMessage` 变体。
