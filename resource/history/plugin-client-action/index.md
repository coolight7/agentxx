# 插件 Client 端事件处理与声明式交互重构
- 难度: 中
- 类型: 架构重构
- 基于commit: 3043de7769d94cb4f589e6e0d3033ef2941a5ea4
- 需求:
```md
规划完善的插件 Client 端事件处理、按钮与点击相关的重构方案，彻底移除 TUI 核心层对特定插件的特化分支，实现由插件完全自主指定生成渲染内容并绑定函数执行交互。详细重构方案见 resource/history/plugin-client-action/plugin.md
```
