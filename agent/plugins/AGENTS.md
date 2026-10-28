# agent/plugins 局部约束

本文件只写本目录内的硬约束；跨目录规则见仓库根 `AGENTS.md`。插件设计见
[docs/zh-cn/design/plugins.md](../../docs/zh-cn/design/plugins.md)。

## 引用边界

- 主程序头只允许 SDK 公开面：`agentxx/plugin/api/*`（umbrella 是 `plugin_kit.h`）
  与 `agentxx/util/exception.h`；其余 `agentxx/*`（`agent/`、`middlewares/`、
  `nodes/`、`tools/`、`event/`、`ui/`、`protocol/`、`plugin/` 下的主程序内部头）
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
  （失败返回 NULL + error，由主程序回滚），`stop` 撤销自管资源（可重复），
  `destroy` 只释放本地对象。缺少 start/stop 的插件主程序会拒绝加载。
- 多实例三铁律：禁止可变全局/函数级 static 缓存；实例状态只放 `*plugin_ctx`
  堆块；接口表查询结果存实例上下文，回调经 `spec.user_data` 恢复。

## 主程序交互

- 与主程序只经 C ABI 接口表交换数据；不传递标准库容器/结构体，字符串用
  `PluginxxString` 或其他主程序可解析的形态。
- 工具的权限限制必须声明（`agentxx.agent.permission` 接口表 / SDK 便捷层），
  否则权限中间件不会对它们生效。
- 每个插件目录的 `CMakeLists.txt` 开头声明支持的平台；无该平台实现时跳过编译
  （经 `cmake/plugin_platform_support.cmake`）。跨平台插件默认放行。
- 主程序堆字符串 (`host->alloc`) 用完必须释放（SDK 的 `PluginString::free` 已封装）。

## 插件内驱动 agent 会话 (`agentxx.agent.runtime`)

写"聊天平台机器人"这类插件时, 插件需要**一个自己的根 agent** —— 提示词 / 工具白名单 /
插件挂载 / 模型只能在 `AgentConfig` 级说, 而"再加载一个自己"不成立 (实例全程只一份)。
完整示例见 `example_runtime`, 设计见 [plugins.md](../../docs/zh-cn/design/plugins.md) §19。

- 建根只能在 `start`: 在 `mount` 回调里调 `mount_root` 返回 `ERR_STATE`
  (否则每个 agent 再建一根, 嵌套没有终点)。
- 建根 / 开通道 / 关通道都是**发起后返回**; 要"确实可用"就等 `READY` 事件。
- 通道事件回调在该根所属 io 线程上执行, **必须快速返回**: 整个进程的会话都在这条线程,
  不要在这里等网络收发或人工答复, 把内容交给自己管理的线程后立即返回。
- 事件里的字符串是借用视图, 回调结束后要用就自己拷成 `std::string`。
- 在事件回调里关通道是安全的 (SDK 的转接对象先拷一份目标函数再调用)。
- 插件禁用 / 卸载 / 关闭时主程序按实例名整批回收根、通道、触发器、定时器;
  自己在 `stop` 里再收一次也无妨 (入口幂等)。
- 数量级按"一个根 64 通道"规划: 多个群共用一个根, 人设在根配置里;
  真需要"一群一套人设"时才拆成多个根 (每根一次 init, 启动与内存线性增长)。

## 测试

- 插件相关改动跑 `plugins`、`plugin_resources`、`plugin_multi_instance`、
  `client_plugins`、`plugin_sdk`、`plugin_bridge` 模块；测试源码在
  `agent/test/plugin`。
- 搭根开通道类插件另跑 `plugin_agent_runtime`（表语义）与
  `plugin_example_runtime`（真实插件全流程）。
