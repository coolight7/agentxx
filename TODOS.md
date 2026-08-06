# TODO
- tui 特化各种 toolcall 渲染
- node 可支持插件化加载动态库 + graph json 定义
- BaseAgent 增加支持 usage 统计
- 优化各种tool的效果，贴近命令行
- ctrl+c 结束时卡很久
- 会话独立日志
- eventBus 改为tree，命名空间使用 axx/bxx/cc
- 内置tool增加命名空间

## 提示词优化
- 如果编译需要配置特定参数，写成脚本或者写入到AGENTS.md
- 自动建议生成、修改、总结一些经验到 AGENTS.md
- 提示生成临时文件的目录
- 建议当需要通读一个大项目时，可以先由 subagent 总结出大致的 wiki，然后分析划分模块化，再分享 wiki 给多个 subagent 各自负责模块解决问题
- 使用 filesystem_grep/filesystem_glob 时尽量缩小扫描范围，避开 .gitignore 内定义的目录、third_party、build、node_modules 等文件夹
- 软件使用文档说明 skill

## 问题
- 请排查一下 tui本地模式启动时（agentxx_cli tui）内存占用会随着对话持续大幅增加的问题，看看是内存泄漏还是什么代码申请的内存太多了