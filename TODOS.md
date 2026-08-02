# TODO
- tui 特化各种 toolcall 渲染
- node 可支持插件化加载动态库 + graph json 定义
- BaseAgent 增加支持 usage 统计
- 侧边栏显示cpu占用和内存占用
- release 编译警告 ODR
- 优化各种tool的效果，贴近命令行
- tui 重构鼠标点击处理

## 提示词优化
- 建议对大的操作、改动先进行规划，每完成一步更新规划，最后测试完成后 -> 回顾修改的代码检查是否有问题 -> 最终整体总结
- 如果编译需要配置特定参数，写成脚本或者写入到AGENTS.md
- 自动建议生成、修改、总结一些经验到 AGENTS.md
- 提示生成临时文件的目录
- 建议当需要通读一个大项目时，可以先由一个 subagent 总结出大致的 wiki，然后分析划分模块化，再分享 wiki 给多个 subagent 各自负责模块解决问题

## 问题
分析一下 这些 cmakelist 有没有问题 /home/coolight/program/agentxx/agent/CMakeLists.txt、/home/coolight/program/agentxx/agent/lib/CMakeLists.txt、/home/coolight/program/agentxx/agent/client/CMakeLists.txt、/home/coolight/program/agentxx/agent/test/CMakeLists.txt
通读 ftxui 的源码，了解其正确使用方式，然后重构 tui，目前的 tui 实现太繁杂了，组件的事件杂糅在一起处理、要自己做渲染缓存等问题