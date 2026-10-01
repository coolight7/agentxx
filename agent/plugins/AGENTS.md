# agent/plugins 局部约束

本文件只写本目录内的硬约束；跨目录规则见仓库根 `AGENTS.md`。插件设计见
[docs/zh-cn/design/plugins.md](../../docs/zh-cn/design/plugins.md)。

## 引用边界

- 宿主头只允许 SDK 公开面：`agentxx/plugin/api/*`（umbrella 是 `plugin_kit.h`）
  与 `agentxx/util/exception.h`；其余 `agentxx/*`（`agent/`、`middlewares/`、
  `nodes/`、`tools/`、`event/`、`ui/`、`protocol/`、`plugin/` 下的宿主内部头）
  一律不得引用（边界检查见测试模块 `boundaries`）。
- 复用工具可以静态链接 `cxx_utilxx_base` / `cxx_utilxx`（`utilxx_base/*`、
  `utilxx/*`）与插件框架 SDK（`pluginxx/*`）；这些符号必须保持隐藏。
- 不要修改 `agent/third_party/`；插件目录里的第三方源码副本由各插件自行维护。

## 导出与生命周期

- 插件动态库只导出入口符号 `agentxx_plugin_{agent,client}_{get_info,create,start,
  stop,destroy}`：使用 SDK 宏（`AGENTXX_PLUGIN_AGENT_EXPORT` /
  `AGENTXX_PLUGIN_{AGENT,CLIENT}_LIFECYCLE_EXPORT` / `PLUGINXX_EXPORT`），
  不要手写 `__declspec(dllexport)` 或 `visibility("default")`。
- 生命周期契约：`create` 只构造上下文（不注册、不起线程），`start` 是注册事务
  （失败返回 NULL + error，由宿主回滚），`stop` 撤销自管资源（可重复），
  `destroy` 只释放本地对象。缺少 start/stop 的插件宿主会拒绝加载。
- 多实例三铁律：禁止可变全局/函数级 static 缓存；实例状态只放 `*plugin_ctx`
  堆块；接口表查询结果存实例上下文，回调经 `spec.user_data` 恢复。

## 宿主交互

- 与宿主只经 C ABI 接口表交换数据；不传递标准库容器/结构体，字符串用
  `PluginxxString` 或其他宿主可解析的形态。
- 工具的权限限制必须声明（`agentxx.agent.permission` 接口表 / SDK 便捷层），
  否则权限中间件不会对它们生效。
- 每个插件目录的 `CMakeLists.txt` 开头声明支持的平台；无该平台实现时跳过编译
  （经 `cmake/plugin_platform_support.cmake`）。跨平台插件默认放行。

## 测试

- 插件相关改动跑 `plugins`、`plugin_resources`、`plugin_multi_instance`、
  `client_plugins`、`plugin_sdk`、`plugin_bridge` 模块；测试源码在
  `agent/test/plugin`。
