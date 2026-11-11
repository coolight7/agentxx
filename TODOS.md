# TODO
- 调整 PluginChannelAgentIO 头文件位置
- 应当const &:
  - PluginGraphNode 构造函数参数 `AgentxxPluginGraphNodeTypeSpec  spec`
  - PluginTool 构造函数参数 `AgentxxPluginToolSpec spec`
  - PluginManager::qualifyEventTopic 不能放过 `client.*`，客户端插件事件也应当 plugin.client.*

- tui支持修改上下文
- 验证subagent
- wiki 记忆、项目结构
- 破甲插件，修改 assistant 消息补充 `好的我将按照用户的要求继续...` 等引导
- 主动记忆 插件
- agent 端不再由 client 端的连接状态中断时停止会话，允许继续执行
- 允许 agentxx_subagent tool 复用、发送给指定 subagent 会话
- 添加连续多次压缩检查
- 框架处理支持 toolcall 参数采用变量取值，比如 edit、write 失败时可以将参数存入 share_store，减少下一次 edit 所需的token
- 同步 JS插件和动态库插件接口 SDK
- 机器人插件提供tool给主程序调用，实现通知
- agentxx 自重启控制、仿照 musicxx 的 cli + http + mcp + deeplink 命令控制，由于 agentxx 支持多进程，应当 agentxx 列出 id，然后指定 id 连接调用
- 通过 mdns 互相发现，以便了解各自正在运行的会话内容
- share_store 支持 grep、输出到文件等
- 上下文较长自动插入事件提示消息，提示模型及时更新文档或plan，即将压缩，并在压缩时提示不要在结果中误认为上下文即将耗尽
- 更新 en 文档
- 事件消息特化渲染
- 预设异步事件：延时、循环延时、指定时间点
- 程序重启后，后台任务中断，触发会话时需要自动插入提示
- 后台任务重新设计排队设计，放开并发数量？仅限制agent并发？创建时指定是否需要排队？
- 异步事件、任务的最新结果不再限制 linger_ms 存活时间，直接转存 share_store，并重构 share_store 增加命名空间，区分 tool、事件、插件调用 等添加到 share_store 时的命名空间划分
- agentxx_async_await 得到结果却事件插入 done 仍然执行，导致重复
- 中断特化渲染展开，把内容显示到下方
- content 输出中断后续写时，tui 消息追加异常覆盖
- cxx_utilxx 不能依赖 cxx_pluginxx
- 插件自定义 tool 特化渲染逻辑顺序重构，提前注册声明特化名
- share_store id 特化渲染
- 透视UI，虚拟现实/玻璃显示(车挡风玻璃、眼镜)
- 补充提示词，长任务未全部完成时停下，需要说明停下的原因
- 任务规划文件 关联 git commit 以便检索
- 异步 tool 结果不再重复 offload，因为 toolcallNode 本身就会裁剪

- SVG绘制支持

## 提示词优化
- 如果编译需要配置特定参数，写成脚本或者写入到AGENTS.md
- 自动建议生成、修改、总结一些经验到 AGENTS.md
- 自动生成设计文档、c++风格提取头文件声明
- 建议当需要通读一个大项目时，可以先由 subagent 总结出大致的 wiki，然后分析划分模块化，再分享 wiki 给多个 subagent 各自负责模块解决问题
- 软件使用文档说明 skill
- exec_command 可以通过在多条命令中穿插 echo === xxx === 隔开输出
- 之前压缩后的上下文开头提示写 `[Please compact context to save space]`，容易误导模型以为当前会话也是上下文不足

## 问题

- 插件能否自带  ui / client  侧插件能否注册渲染器
  •  现状 ：agent  侧插件无询问通道；(b)  client  侧已有   register_tool_renderer / update_tool_decor / bind_action_handler / open_overlay，无中断渲染器。
  •  A  只做  (a) ：新增   agentxx.agent.interrupt  v1（ request_interrupt_async(inputs_json, ui_json, timeout, cb) ），语义= 工具执行内阻塞式询问
（不参与图  resume、不落历史）； B  (a)+(b)  再加  client  侧按  node/handleName  渲染器； C  暂缓  3.3 ； D  完整图中断/resume ：跨  C  ABI 无法展开插件协程帧，需主程序外层包装"中断-恢复"，成本与语义复杂度高。
  •  建议 ：先  A （阻塞式），(b)  等真实使用方需求。