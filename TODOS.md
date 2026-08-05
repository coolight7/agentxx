# TODO
- tui 特化各种 toolcall 渲染
- node 可支持插件化加载动态库 + graph json 定义
- BaseAgent 增加支持 usage 统计
- 侧边栏显示cpu占用和内存占用
- 优化各种tool的效果，贴近命令行
- ctrl+c 结束时卡很久
- 会话独立日志
- glob、grep 接收相对路径时拼接转换

## 提示词优化
- 建议对大的操作、改动先进行规划，每完成一步更新规划，最后测试完成后 -> 回顾修改的代码检查是否有问题 -> 最终整体总结
- 如果编译需要配置特定参数，写成脚本或者写入到AGENTS.md
- 自动建议生成、修改、总结一些经验到 AGENTS.md
- 使用 grep/glob 时尽量精确到更具体的文件，避开 .gitignore 内定义的目录、third_party、build、node_modules 等
- 提示生成临时文件的目录
- 建议当需要通读一个大项目时，可以先由一个 subagent 总结出大致的 wiki，然后分析划分模块化，再分享 wiki 给多个 subagent 各自负责模块解决问题

## 问题
分析一下 这些 cmakelist 有没有问题 /home/coolight/program/agentxx/agent/CMakeLists.txt、/home/coolight/program/agentxx/agent/lib/CMakeLists.txt、/home/coolight/program/agentxx/agent/client/CMakeLists.txt、/home/coolight/program/agentxx/agent/test/CMakeLists.txt
通读 ftxui 的源码，了解其正确使用方式，然后重构 tui，目前的 tui 实现太繁杂了，组件的事件杂糅在一起处理、要自己做渲染缓存等问题。请仔细思考并给出tui重构方案后，先告诉我跟我沟通确认
- 请排查一下 tui本地模式启动时（agentxx_cli tui）内存占用会随着对话持续大幅增加的问题，看看是内存泄漏还是什么代码申请的内存太多了

- 请优化修复：
    - 给 checkpoint store 裁剪，每轮结束后只保留最近的 1- 2 个
    - 检查 fullHistory 在 agent-server 线程上读写，通过拷贝传输给其他线程或远程网络传输，似乎并不需要快照和锁，能否简化
- 然后排查 agent 多轮运行后会不会内存暴涨，排查具体涨在什么地方