# 插件框架与已有插件重构
- 难度: 高
- 类型: 重构
- 基于commit: 97c9669cc383a34645dd7f77c82bdb88f818276e
- 需求:
    - 分析报告
    ```md
    请先通读当前项目的插件框架代码实现 /home/coolight/program/agentxx/agent/lib/include/agentxx/plugin、/home/coolight/program/agentxx/agent/lib/src/plugins，已有的插件实现 /home/coolight/program/agentxx/agent/plugins，结合测试 /home/coolight/program/agentxx/agent/test，仔细思考了解整体的架构设计和功能，分析是否符合预期设计要求（宿主与插件之间为 C-Api、COM-API 查询 隔离，只能传递c结构体指针和基础变量，隔离异常；原生协程异步接口，宿主与插件默认在同一线程无锁，协程函数可互相交错执行，非阻塞；异步等待结果不使用轮询，实现真协程），或是有没有bug、可优化的、需要重构的、可整理的代码、可简化的设计、可简化插件开发、提升可读性的地方。

    然后将整体的架构和代码分析报告、完善的重构方案 写入到文件 resource/history/plugin-refactor-2/plugin.md
    ```
    - 实施方案
    ```md
    请完整通读 插件框架架构重构方案文档 resource/history/plugin-refactor-2/plugin.md，仔细思考分析后实现
    ```