# bench 增加测试 agentxx_cli, libagnetxx 性能
- 难度: 很高
- 类型: 新增功能
- 基于commit: a6c45638ab4dee66643a35cf8663133b81086967
- 需求:
```md
- benchmark 仿照 test 支持模块化，然后增加支持测试 `agentxx_cli` 在加载5个常用插件(agentxx_filesystem,agentxx_execute_command,agentxx_system,agentxx_websearch,agentxx_planning)的情况下，以下模式时在（程序启动时、100K token左右上下文、200K token左右上下文）的内存占用、cpu占用:
    1. 同一进程cli
    2. 同一进程tui
    3. 拆分两个进程运行cli+server时两者分别的占用
    4. 拆分两个进程运行tui+server时两者分别的占用
    - llm消息由程序生成不使用真实api，应当为 user、assist、tool 消息交替出现，且内容固定
- 并测试 `libagentxx_shared 动态库` 同样在加载上述5个插件、相同条件的（加载动态库后、100K上下文、200K上下文；上下文内容一致）时的内存占用和cpu占用
- 请参考已有的设计文档 resource/history/benchmark-exe-dll/bench.md, 实现功能需求
```