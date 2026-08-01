# TODO
- tui 特化各种 toolcall 渲染
- node 可支持插件化加载动态库 + graph json定义
- 拆分需要区分不同线程，继承出不同class，屏蔽不可用函数或检查
- exec: 支持指定多条指令
- BaseAgent 增加支持 usage 统计
- 添加测试 provide llm api 响应 toolcall 时没有 toolcall_id

## 提示词优化
- 建议对大的操作、改动先进行规划，每完成一步更新规划，最后测试完成后 -> 回顾修改的代码检查是否有问题 -> 最终整体总结
- 如果编译需要配置特定参数，写成脚本或者写入到AGENTS.md
- 自动建议生成、修改、总结一些经验到 AGENTS.md
- 建议当需要通读一个大项目时，可以先由一个 subagent 总结出大致的 wiki，然后分析划分模块化，再分享 wiki 给多个 subagent 各自负责模块解决问题

## 问题
重构 tui，通读 ftxui 了解正确使用方式，然后参考 flutter 的Widget和渲染，拆分组件，隔离重绘区域，避免每次都需要请求整个屏幕重绘。重构scrollable，仿照 flutter 的 ListView 实现可见范围内 viewport 局部绘制，提升性能，优化跟随鼠标滚动速度，固定每次滚动1行高度

通读 ftxui，然后分析 tui 写的是否正确 /home/coolight/program/agentxx/agent/client/src/io/tui

请修复 tui的 messages 折叠状态时鼠标点击不会展开了；以及 thinking 消息 完成时没有显示思考时长