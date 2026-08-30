# TODO
- BaseAgent 增加支持 usage 统计
- eventBus 改为tree，命名空间使用 axx/bxx/cc
- 支持修改上下文
- 验证subagent、summarization
- CI/CD
- 文档翻译， 统一命名为 client-io、server-io
- 整理文档
- release 编译发布时携带 标准库
- filesystem utf8 path

- graph json 定义，支持插件生成 graph json、注册 node; (最多只能有一个插件生成 graph，都没有则默认生成 CodeAgent)
- 插件 readHostConfig 改为直接传入 变量路径 取值，变量取值交由主程序解析
- codegraph 疑似启动后没有更新索引

- 合并 share_store.db 到 session.db
- worktree 应当启用时增加 system prompt提示，但进入、退出不应更新 system prompt
- 冻结 system prompt
- modelcall 修复上下文合并连续 user 消息
- share_store 替换 middleware.state
- AgentxxConfigIface: get_work_dir/get_session_work_dir
- OpWatchdog debug 时启用、配置启用

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