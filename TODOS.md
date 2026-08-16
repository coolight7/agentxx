# TODO
- graph json 定义
- BaseAgent 增加支持 usage 统计
- 会话独立日志、yaml配置文件
- 会话恢复时是否取原来的 配置、插件
- eventBus 改为tree，命名空间使用 axx/bxx/cc
- 支持修改上下文
- llm压缩时，保持同一上下文，直接添加新user消息提示压缩成一段话，然后覆盖回去
- 插件化支持内置合并编译
- 客户端 viewMessage 分页
- 插件实现 execute_javascript_command
- 调整插件代码结构，增加测试、划分文件夹

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