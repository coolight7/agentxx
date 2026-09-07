# 插件框架级事件驱动取消检测架构重构
- 难度: 中
- 类型: 架构重构
- 基于commit: 4586eeb2206012f9470ea6788f4efc1f4295ed0c
- 需求:
```md
事件驱动取消检测目前只在 execute_command 实现，请通读插件框架、了解整体的架构设计和功能，实现将 CancelRegistry 提升至插件框架 SDK（PluginBase / plugin_kit.h）的通用基础设施，彻底消除跨线程同步 post 到 IO 线程的轮询与密集抖动。详细方案见 resource/history/plugin-event-cancel-registry/plugin.md
```
