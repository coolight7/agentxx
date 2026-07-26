# TODO
- tui 特化各种 toolcall 渲染

- 我觉得还得原来的分层容易理解，这样可以吗，有没有需要改进的：
    - 捋清楚数据流动方向 client -> AgentIO -> remote/channel -> AgentIO -> deepagent server
    - 所以实际上应当设计为 AgentIOBase + AgentIOTransportBase, 一个负责定义 client/server 之间的操作接口，一个负责 AgentIO 之间的通信，构造时应当写 AgentIOBase 包含 AgentIOTransportBase, 即 AgentIOBase(AgentIOTransportBase) , 这样就清晰了