# TODO
- hyperscan 匹配 \w 时包含了换行符
- 移除不必要的锁

想支持 deepagent (/home/coolight/program/agentxx/agent/lib/include/agentxx/agent/deepagent.h) 启动成 http/ws 服务，以便其他GUI连接访问和执行会话.

现在我的想法是 从client (/home/coolight/program/agentxx/agent/client/main.cpp)启动的话，client main本身是主线程，如果启动了 tui 则增加 tui 线程用于 ftxui事件循环; 然后增加 client main启动参数`--agent` 支持http/ws连接远程 deepagent 服务（此时 client 不再启动 deepagent，而是使用远程的 deepagent），如果没有指定 `--agent` 则增加 agent 线程去启动 deepagent; client main启动参数增加 `deepagent` 启动 deepagent 为 http/ws 服务. 把 agent 线程视作一个独立进程.

deepagent 在 agent_io (/home/coolight/program/agentxx/agent/lib/include/agentxx/agent/agent_io.h、/home/coolight/program/agentxx/agent/client/include/agentxx-client/io/) 增加支持读写数据处理，那么要支持 client 和 agent 的数据交互，则需要修改 agent_io，增加一套数据交互接口，支持实现成 http/ws 网络连接交互、线程间数据交互. 比如网络交互，用 WsClientAgentIO包裹TUIAgentIO + WsServerAgentIO

网络相关实现时请使用 asio、boost.beast、/home/coolight/program/agentxx/agent/lib/include/agentxx/util/http_client.h、/home/coolight/program/agentxx/agent/lib/include/agentxx/util/http_server.h
需要为 HttpServer 增加 WebSocket 升级支持
当前 HttpServer (agent/lib/src/util/http_server.cpp) 仅处理 HTTP/1.1。需要添加：
- WS 升级路径: HTTP GET + Upgrade: websocket → boost::beast::websocket::stream
- 升级后的 WS 读写在 worker io_context 上的协程中进行
- 位置: agent/lib/include/agentxx/util/http_server.h → 增加 enableWebSocket(path, handler) 方法
需要实现 WS 协议设计 (双向 JSON 消息)

请仔细思考这个方案可行性，以及具体实现细节、应该优化改进的点


┌──────────────────────────────────────────────────┐
│ Client (agentxx_cli --agent ws://host:port)      │
│                                                   │
│  Main Thread (ioCtx)                              │
│  ┌────────────────────────────────────────────┐  │
│  │ CLI/TUI Loop (与当前相同)                  │  │
│  │  WsClientAgentIO (AgentIOBase)            │  │
│  │  ├─ onToken(token, kind) → local render   │  │
│  │  ├─ getInput() → WS send "prompt" cmd     │  │
│  │  ├─ promptPermission() → WS rpc + wait    │  │
│  │  └─ 从 WS 接收 token/tool/事件 并显示      │  │
│  └────────────────┬───────────────────────────┘  │
│                   │ WebSocket                     │
└───────────────────┼───────────────────────────────┘
                    │
┌───────────────────┼───────────────────────────────┐
│ Server (agentxx_cli deepagent 单线程, 多协程)     │
│                   │                               │
│  Main: HttpServer                                 │
│  ┌────────────────────────────────────────────┐  │
│  │ WS Connect → WsAgentSession                │  │
│  │  ├─ WsServerAgentIO (AgentIOBase)         │  │
│  │  │  onToken → WS send                     │  │
│  │  │  onToolStart/End → WS send             │  │
│  │  ├─ WsPermissionPrompter (EventBus ↔ WS)  │  │
│  │  ├─ WsInterruptHandler (EventBus ↔ WS)    │  │
│  │  └─ 运行 session 循环:                     │  │
│  │     WS recv "prompt" →                     │  │
│  │     runConversation(..., io, eventBridge)  │  │
│  └────────────────────────────────────────────┘  │
│                                                   │
│  ┌────────────────────────────────────────────┐  │
│  │ DeepAgent                                  │  │
│  │  engine, ioCtx, agentContext               │  │
│  └────────────────────────────────────────────┘  │
└───────────────────────────────────────────────────┘