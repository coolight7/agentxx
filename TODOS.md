# TODO
- BaseAgent 增加支持 usage 统计
- eventBus 改为tree，命名空间使用 axx/bxx/cc
- 支持修改上下文
- 验证subagent

- 冻结 system prompt

- wiki 记忆、项目结构
- 破甲插件，修改 assistant 消息补充 `好的我将按照用户的要求继续...` 等引导
- client 连接 server 时插入 tool、加载插件
- 事件驱动触发 agent
- 主动记忆 插件tool
- tui 提示消息 dim, 移除开头的 #，支持加载中动画，压缩提示增加动画
- 添加连续多次压缩检查
- 框架处理支持 toolcall 参数采用变量取值，比如 edit、write 失败时可以将参数存入 share_store，减少下一次 edit 所需的token

- SVG绘制支持
- 链式 session 任务队列

## 提示词优化
- 如果编译需要配置特定参数，写成脚本或者写入到AGENTS.md
- 自动建议生成、修改、总结一些经验到 AGENTS.md
- 自动生成设计文档、c++风格提取头文件声明
- 建议当需要通读一个大项目时，可以先由 subagent 总结出大致的 wiki，然后分析划分模块化，再分享 wiki 给多个 subagent 各自负责模块解决问题
- 软件使用文档说明 skill
- exec_command 可以通过在多条命令中穿插 echo === xxx === 隔开输出
- 性能测试前应当读取一下系统负载
- 描述自动插入的消息的原因，避免影响判断
- 实施任务时，增删改了实际实施任务内容、额外做的操作都应当在完成时告知用户
- 之前压缩后的上下文开头提示写 `[Please compact context to save space]`，容易误导模型以为当前会话也是上下文不足

## 问题

D2  插件能否自带   ui  /  client  侧插件能否注册渲染器

  •  现状 ：agent  侧插件无询问通道；(b)  client  侧已有   register_tool_renderer / update_tool_decor / bind_action_handler / open_overlay
，无中断渲染器。
  •  A  只做  (a) ：新增   agentxx.agent.interrupt  v1（ request_interrupt_async(inputs_json, ui_json, timeout, cb) ），语义= 工具执行内阻塞式询问
（不参与图  resume、不落历史）； B  (a)+(b)  再加  client  侧按  node/handleName  渲染器； C  暂缓  3.3 ； D  完整图中断/resume ：跨  C  ABI
 无法展开插件协程帧，需主程序外层包装"中断-恢复"，成本与语义复杂度高。
  •  建议 ：先   A （阻塞式），(b)  等真实使用方需求。