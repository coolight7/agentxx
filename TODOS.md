# TODO
- BaseAgent 增加支持 usage 统计
- eventBus 改为tree，命名空间使用 axx/bxx/cc
- 支持修改上下文
- 验证subagent、summarization

- 冻结 system prompt
- OpWatchdog debug 时启用、配置启用

- agent-io 的 server-client 改为一对多
- 链式 session 任务队列
- 传递 多语言支持到 插件
- 修复测试错误
- 添加 XX_IS_MINGW __MINGW32__

## 提示词优化
- 如果编译需要配置特定参数，写成脚本或者写入到AGENTS.md
- 自动建议生成、修改、总结一些经验到 AGENTS.md
- 提示生成临时文件的目录
- 自动生成设计文档、c++风格提取头文件声明
- 建议当需要通读一个大项目时，可以先由 subagent 总结出大致的 wiki，然后分析划分模块化，再分享 wiki 给多个 subagent 各自负责模块解决问题
- 使用 agentxx_filesystem_grep/agentxx_filesystem_glob 时尽量缩小扫描范围，避开 .gitignore 内定义的目录、third_party、build、node_modules 等文件夹
- 软件使用文档说明 skill
- exec_command 可以通过在多条命令中穿插 echo === xxx === 隔开输出

## 问题
