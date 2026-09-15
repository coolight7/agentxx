# TODO
- BaseAgent 增加支持 usage 统计
- eventBus 改为tree，命名空间使用 axx/bxx/cc
- 支持修改上下文
- 验证subagent

- 冻结 system prompt

- wiki 记忆、项目结构
- 测试的 include 增加目录前缀
- 检查tui选择附件来源，增加区分如果 server 跟 client 不同设备，则分区支持选择附件

- 插件tool权限限制由插件注册
- 迁移 planning 提示词到插件内
- agentxx_share_store 特化渲染
- agentxx_execute_javascript 更名
- agentxx_filesystem_list 支持 * 等模糊匹配

- 调整 tui 亮色配置
- SVG绘制支持
- 链式 session 任务队列

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

D2  插件能否自带   ui  /  client  侧插件能否注册渲染器

  •  现状 ：agent  侧插件无询问通道；(b)  client  侧已有   register_tool_renderer / update_tool_decor / bind_action_handler / open_overlay
，无中断渲染器。
  •  A  只做  (a) ：新增   agentxx.agent.interrupt  v1（ request_interrupt_async(inputs_json, ui_json, timeout, cb) ），语义= 工具执行内阻塞式询问
（不参与图  resume、不落历史）； B  (a)+(b)  再加  client  侧按  node/handleName  渲染器； C  暂缓  3.3 ； D  完整图中断/resume ：跨  C  ABI
 无法展开插件协程帧，需宿主外层包装"中断-恢复"，成本与语义复杂度高。
  •  建议 ：先   A （阻塞式），(b)  等真实宿主需求。