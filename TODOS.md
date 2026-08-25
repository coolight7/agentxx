# TODO
- BaseAgent 增加支持 usage 统计
- eventBus 改为tree，命名空间使用 axx/bxx/cc
- 支持修改上下文
- ASAN、TSAN、UBSAN
- git worktree
- 验证subagent、summarization
- CI/CD
- 文档翻译
- release 编译发布时携带 标准库
- graph json 定义，支持插件生成 graph json、注册 node; (最多只能有一个插件生成 graph，都没有则默认生成 CodeAgent)
- 调整插件代码结构，增加测试、划分文件夹
- 整理文档
- 插件宏辅助代码
- 插件支持多次加载，目前用了许多全局变量存储结构体，可能有问题
- 移除 agent server 的 证书支持
- 记录加载失败的 Append 组件，传入tui显示

- 请分析如何agent实现gitworktree支持，启用该模式时，独立创建一个worktree环境、编译目录环境，以便在同一目录启动多个agent
会话各自实现代码互不影响。请通读当前agentxx架构设计，分析如何接入实现worktree模式，比如启用该模式时，添加worktree的相关tool（创建、读取当前worktree
环境，删除），提示词提醒agent在任务开始时创建独立的worktree，并在必要时提醒用户提交代码、最终删除worktree
完成任务。这样设计是否合理，能否进一步优化，或是通过更多的代码层面限制约束agent更规范地执行worktree模式。你可以通过网络搜索了解已有的agent 的相关设计，参考并结合当前项目的实际架构分析完善的设计方案
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