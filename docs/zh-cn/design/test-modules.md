# 测试模块清单 (生成物)

> 由 `agent/test/test.cpp` 的注册调用生成, 请勿手工编辑。
> 重新生成: `AGENTXX_UPDATE_TEST_MODULES=1 path/to/agentxx_test test_modules`

运行方式:

```bash
path/to/agentxx_test                    # 跑全部模块
path/to/agentxx_test --fail-fast        # 遇到失败立即终止
path/to/agentxx_test string_util regex  # 只跑指定模块
```

模块总数: 130 (同步 69 / 异步 61)

| 模块 | 调度 | 构建 |
|---|---|---|
| `boundaries` | 同步 | 通用 |
| `assembly_snapshot` | 同步 | 通用 |
| `config_validation` | 同步 | 通用 |
| `config_keys` | 同步 | 通用 |
| `log_modules` | 同步 | 通用 |
| `task_scope` | 同步 | 通用 |
| `string_util` | 同步 | 通用 |
| `invisible_text` | 同步 | 通用 |
| `chatbot_core` | 同步 | 通用 |
| `chatbot_qq` | 同步 | 通用 |
| `regex` | 同步 | 通用 |
| `json` | 同步 | 通用 |
| `persist_semantics` | 同步 | 通用 |
| `prompt_stability` | 同步 | 通用 |
| `json_view` | 同步 | 通用 |
| `json_reflection` | 同步 | 通用 |
| `llm_error` | 同步 | 通用 |
| `provider_call_scope` | 同步 | 通用 |
| `diff_util` | 同步 | 通用 |
| `events` | 同步 | 通用 |
| `concurrency` | 同步 | 通用 |
| `misc_fixes` | 同步 | 通用 |
| `graph_data` | 同步 | 通用 |
| `aho_corasick` | 同步 | 通用 |
| `observability` | 同步 | 通用 |
| `util_misc` | 同步 | 通用 |
| `training` | 同步 | 通用 |
| `settings_db` | 同步 | 通用 |
| `session_schema` | 同步 | 通用 |
| `storage_consistency` | 同步 | 通用 |
| `writer_lease` | 同步 | 通用 |
| `toolcall_args` | 同步 | 通用 |
| `wire_roundtrip` | 同步 | 通用 |
| `wire_schema` | 同步 | 通用 |
| `test_modules` | 同步 | 通用 |
| `interrupt_ui` | 同步 | 通用 |
| `ui_items` | 同步 | 通用 |
| `ui_kit` | 同步 | 通用 |
| `ffi_c_api` | 同步 | 通用 |
| `plugin_runtime` | 同步 | 通用 |
| `plugin_sdk` | 同步 | 通用 |
| `plugin_bridge` | 同步 | 通用 |
| `config_loader` | 同步 | 仅 client |
| `banner_art` | 同步 | 仅 client |
| `tui_settings` | 同步 | 仅 client |
| `update_check` | 同步 | 仅 client |
| `tui_input` | 同步 | 仅 client |
| `tui_interrupt` | 同步 | 仅 client |
| `tui_scroll` | 同步 | 仅 client |
| `tui_lazy_view` | 同步 | 仅 client |
| `tui_sidebar` | 同步 | 仅 client |
| `tui_context_overlay` | 同步 | 仅 client |
| `tui_form` | 同步 | 仅 client |
| `tui_stream` | 同步 | 仅 client |
| `tui_surface` | 同步 | 仅 client |
| `console_state` | 同步 | 仅 client |
| `tui_theme` | 同步 | 仅 client |
| `tui_tool_header` | 同步 | 仅 client |
| `tui_ui_items` | 同步 | 仅 client |
| `tui_widget` | 同步 | 仅 client |
| `ui_snapshot` | 同步 | 仅 client |
| `tui_model` | 同步 | 仅 client |
| `jsonl_runner` | 同步 | 仅 client |
| `ui_capabilities` | 同步 | 仅 client |
| `sessionId` | 同步 | 仅 client |
| `mermaid_state` | 同步 | 仅 client |
| `markdown_block` | 同步 | 仅 client |
| `ftxui_text` | 同步 | 仅 client |
| `markdown_flow` | 同步 | 仅 client |
| `event_stream` | 异步 | 通用 |
| `feature_points` | 异步 | 通用 |
| `event_bridge` | 异步 | 通用 |
| `interrupt_bus` | 异步 | 通用 |
| `subagent_bus` | 异步 | 通用 |
| `subagent_tool` | 异步 | 通用 |
| `agent_host` | 异步 | 通用 |
| `string_tools` | 异步 | 通用 |
| `math_tools` | 异步 | 通用 |
| `share_store` | 异步 | 通用 |
| `session_persistence` | 异步 | 通用 |
| `session_runtime` | 异步 | 通用 |
| `rag_search` | 异步 | 通用 |
| `datetime` | 异步 | 通用 |
| `filesystem` | 异步 | 通用 |
| `command` | 异步 | 通用 |
| `worktree` | 异步 | 通用 |
| `web_search` | 异步 | 通用 |
| `codegraph` | 异步 | 通用 |
| `screen_capture` | 异步 | 通用 |
| `cpu_gpu` | 异步 | 通用 |
| `text_selection` | 异步 | 通用 |
| `http` | 异步 | 通用 |
| `network_timeout` | 异步 | 通用 |
| `websocket` | 异步 | 通用 |
| `remote_agent` | 异步 | 通用 |
| `mcp` | 异步 | 通用 |
| `acp` | 异步 | 通用 |
| `a2a` | 异步 | 通用 |
| `openai_provider` | 异步 | 通用 |
| `permission` | 异步 | 通用 |
| `input_delivery` | 异步 | 通用 |
| `fake_provider` | 异步 | 通用 |
| `session_admin` | 异步 | 通用 |
| `session_sync` | 异步 | 通用 |
| `prompt_stability_io` | 异步 | 通用 |
| `prompt_sources` | 异步 | 通用 |
| `prompt_cache_prefix` | 异步 | 通用 |
| `assembly_snapshot_io` | 异步 | 通用 |
| `shutdown_stages` | 异步 | 通用 |
| `anthropic_provider` | 异步 | 通用 |
| `plugins` | 异步 | 通用 |
| `plugin_llm_uncensored` | 异步 | 通用 |
| `plugin_cleanup` | 异步 | 通用 |
| `plugin_feature` | 异步 | 通用 |
| `plugin_hooks` | 异步 | 通用 |
| `plugin_resources` | 异步 | 通用 |
| `plugin_host_mount` | 异步 | 通用 |
| `plugin_multi_mount` | 异步 | 通用 |
| `client_plugins` | 异步 | 通用 |
| `cancel` | 异步 | 通用 |
| `race_guards` | 异步 | 通用 |
| `message_supplement` | 异步 | 通用 |
| `summarization` | 异步 | 通用 |
| `checkpoint_store` | 异步 | 通用 |
| `agent` | 异步 | 通用 |
| `usage_ledger` | 异步 | 通用 |
| `toolcall_parallel` | 异步 | 通用 |
| `http_recorder` | 异步 | 通用 |
| `jsonl_mode` | 异步 | 通用 |
| `memgrowth` | 异步 | 通用 |
