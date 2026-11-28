# 任务卡片: TUI 分页滚动屏蔽 / async 工具行 / 插件数据目录规范

## 目标

使用中记下的三件事:

1. **TUI 历史分页时屏蔽滚动**: 加载更早消息期间用户继续上滑, 页面前插后会"跳到新加载
   消息的开头" —— 加载期间不接收滚轮。
2. **async 系列工具行看不到内容**: `agentxx_async_*` 运行中只有 `正在调用工具...`,
   没有内容 / 特化渲染 —— 与 `agentxx_share_store` / `agentxx_subagent` 一样给显示名与
   折叠内容。
3. **插件数据目录规范化**: 框架提供接口取"插件数据目录", 各自归类到
   `{data_dir}/plugins/data/{插件 id}/`; 以后安装插件用 `{data_dir}/plugin/installed/`;
   迁移全部插件的数据保存目录。

## 范围

| 部分 | 主要文件 |
|---|---|
| 1 滚动屏蔽 | `agent/client/{include,src}/io/tui/{lazy_scrollable,agent_tui,components/message_list}.*` |
| 2 工具行内容 | `agent/lib/src/plugins/builtin_tool_renderers.cpp`、`agent/lib/include/agentxx/plugin/builtin_tool_renderers.h` |
| 3 插件数据目录 | 新增 `agentxx/plugin/plugin_paths.h`、插件管理器 (`plugin_manager*` / `client_plugin_manager`)、SDK kit、`agent/plugins/**`、`agent/lib/src/plugins/net_service.cpp` |

## 状态

已完成 (三个任务都已提交; 实施过程与验证见 [work.md](work.md))

## 结论

- **TUI 分页滚动**: 页请求在飞期间用 `LazyScrollable::setScrollSuspended` 吞滚轮,
  页响应处理完才放开; 8 秒看门狗防丢页后一直不动
- **async 工具行**: 五个 `agentxx_async_*` 在 `registerBuiltinToolRows` 里有显示名与
  折叠内容 (运行中不再只有默认文案)
- **插件数据目录**: 布局落到 `{data_dir}/plugins/data/{插件 id}/`, 唯一实现
  `agentxx/plugin/plugin_paths.h`; 插件经 `get_own_info` 的 `dataDir` 字段 /
  SDK 的 `ctx.dataDirAsync()` 取, 不再自己拼 `data_dir`; 四个插件 (planning /
  codegraph / screen_capture / chatbot) 与 `http.download` 都迁到新位置
- 偏差: 安装根用复数 `plugins/installed` (用户原话 `plugin/installed`)
