# TODO
- tui 特化各种 toolcall 渲染， json 格式化
- node 可支持插件化加载动态库 + graph json 定义
- BaseAgent 增加支持 usage 统计
- 会话独立日志、yaml配置文件
- eventBus 改为tree，命名空间使用 axx/bxx/cc
- 支持修改上下文
- markdown 支持渲染 状态图
- 插件化支持
- llm压缩时，保持同一上下文，直接添加新user消息提示压缩成一段话，然后覆盖回去
- provider 连接池、保持连接活跃
- 检查toolcall id 重复、调整自动补充 toolcallid
- codegraph 支持并发索引多个加载路径、按文件粒度增量更新
- codegraph 索引结果支持 git 提交历史感知 (仅索引最近变更) 等高级策略

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