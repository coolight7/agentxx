# TODO
- hyperscan 匹配 \w 时包含了换行符
- 用户输入队列
- 移除不必要的锁
- 向 fullHistory 插入中断、掉线等提示消息
- tui 特化各种 toolcall 渲染

想支持 deepagent (/home/coolight/program/agentxx/agent/lib/include/agentxx/agent/deepagent.h) 启动成 http/ws 服务，以便其他GUI连接访问和执行会话.

现在我的想法是 从client (/home/coolight/program/agentxx/agent/client/main.cpp)启动的话，client main本身是主线程，如果启动了 tui 则增加 tui 线程用于 ftxui事件循环; 然后增加 client main启动参数`--agent` 支持http/ws连接远程 deepagent 服务（此时 client 不再启动 deepagent，而是使用远程的 deepagent），如果没有指定 `--agent` 则增加 agent 线程去启动 deepagent; client main启动参数增加 `deepagent` 启动 deepagent 为 http/ws 服务. 把 agent 线程视作一个独立进程. Client 视作一个"远程渲染器"/server启动器.

deepagent 在 agent_io (/home/coolight/program/agentxx/agent/lib/include/agentxx/agent/agent_io.h、/home/coolight/program/agentxx/agent/client/include/agentxx-client/io/) 增加支持读写数据处理，那么要支持 client 和 agent 的数据交互，则需要修改 agent_io，增加一套数据交互接口，支持实现成 http/ws 网络连接交互、线程间数据交互. 比如网络交互，用 WsClientAgentIO包裹TUIAgentIO + WsServerAgentIO

网络相关实现时请使用 asio、boost.beast、/home/coolight/program/agentxx/agent/lib/include/agentxx/util/http_client.h、/home/coolight/program/agentxx/agent/lib/include/agentxx/util/http_server.h
需要实现 WS 协议设计 (双向 JSON 消息)
Server 侧线程模型：单 io_context, 单线程，多协程执行，需要 http_server.h 增加 startAsync(); 在当前 executor 上运行 accept loop，不创建线程
- WS 写并发问题，onDelta() 从 DeepAgent 协程调用，getInput() 的 response 也可能触发写。多个协程同时写同一个 WS stream 会 UB
- 每个 WsServerAgentIO 内部用一个 写队列 + 单写协程，另外队列容量有限（如 4096），需要思考满时策略
断线重连与状态恢复、同步消息和状态
- agent_io的 getInput() 和 handleInterrupt() 是 co_await 挂起的。如果 Client 永远不响应（断线且未检测到），Server 协程会永远挂起。需要加超时限制，并且依赖 WS 连接的 TCP keepalive / ping-pong 检测断线，断线时做类似用户中断deepagent处理，但应当区分中断原因

如果运行client为在一个进程里运行 client(cli/tui)+deepagent，则使用 线程间通信版agent_io 包装，需要注意锁的使用，尽可能实现无锁提升性能

请仔细思考这个方案可行性，以及具体实现细节、应该优化改进的点，按能稳定在生产环境运行的目标规划方案和实现代码，做足方案的细节规划和问题排查

仔细思考检查方案没有问题的话可以开始写代码实现了，且应当添加足量的常规使用方式测试+各种边缘情况测试

- Phase 2（增强）：delta 环形缓冲 + 增量重放 + grace period 重挂 + 请求级超时细化 + wss。
- Phase 3：进程内传输 ChannelTransport（统一本地/远程路径）、远程上下文统计同步、TUI 取消键路由到远程、token delta 合并降帧
- 测试：扩展 test_websocket.cpp 风格，新增 test_remote_agent.cpp：echo/并发写/断线取消/重连 sync/超时/鉴权失败。