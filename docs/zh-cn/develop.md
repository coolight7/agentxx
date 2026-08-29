# 开发指南

> 关联: [design.md](../../docs/agent/design.md) (架构) · [plugins.md](../../docs/agent/plugins.md) (插件) · [ffi.md](../../docs/agent/ffi.md) (FFI)

## 1. 测试

### 运行测试

```bash
# 运行全部 (同步+异步, 含 client 侧)
./agent/build/linux-debug/exec/agentxx_test

# 失败即停
./agent/build/linux-debug/exec/agentxx_test -f

# 仅指定模块
./agent/build/linux-debug/exec/agentxx_test string_util regex agent plugins
```

测试模块名见 `agent/test/test.cpp` 顶部注册表；`AGENTXX_BUILD_CLIENT` 条件下额外编译 `config_loader/tui_*` 等 10 个 client 侧模块。

### 新增测试模块约定

- 头文件仅保留函数声明；断言计数器定义在 `*.cpp` 匿名命名空间内，并在 `cpp` 内 `#define XX_TEST_PASSED g_xxx_passed` / `XX_TEST_FAILED` 映射 `test_framework.h` 宏；末尾 `return TestResult{g_xxx_passed, g_xxx_failed};`
- 禁止在头文件做宏覆盖或 `extern` 导出计数器 (跨 TU 宏泄漏曾导致多模块计数错乱)
- 异步模块签名 `asio::awaitable<TestResult> run_xxx_tests()` / 同步模块 `TestResult testXxx()`

### 模拟 LLM (DaSimServer)

`test_agent.h` 提供的 `DaSimServer` 为共享 mock LLM Server，其它模块 (`agent_host/session_persistence/remote_agent/cancel/memgrowth` 等) 直接复用；支持按请求体返回 `content/thinking/tool_calls` 流式 SSE。

## 2. 新增工具 / 中间件

- **内置工具**：已全部迁移至插件 (`agent/plugins/agentxx_*`)，新增工具优先以插件形式实现 (经 `plugin_kit.h` 的 `tool/fast_tool/blocking_tool` 注册)，同名同行为，测试直测同一 `*_impl.h` 实现
- **中间件**：继承 `MiddlewareHandleBase`，在 `CodeAgent::initMiddleware` 中按栈顺序注册；`onHandleStart/End` 可挂载到 `agent_start/modelcall/toolcall` 三节点
- **权限**：文件系统权限经 `PermissionMiddleware` 的最长前缀匹配 (`XXRouter`) + 通配符 `*`，默认规则由 `permission.mode` 决定；新增受控资源需定义 `category` 并走 `service.permission` 询问

## 3. 插件开发

- 入口 `agentxx_plugin_create/destroy` (client 侧 `agentxx_client_create/destroy`)，`AGENTXX_PLUGIN_EXPORT` 标记
- 遵守三铁律：无可变全局 static / 状态经 `user_data` 闭包恢复 / 接口表缓存入实例上下文
- 复用 `agentxx_util` 时 `find_package(agentxx_util)` + `target_link_libraries(PRIVATE agentxx_util)` (内置插件便捷，第三方仅需纯 C 头)
- 平台矩阵在各插件 `CMakeLists.txt` 开头经 `plugin_platform_support.cmake` 的 `gate` 判定

详见 [plugins.md](../../docs/agent/plugins.md)

## 4. 调试与日志

- 统一使用 `XX_LOG*` (见 `agent/lib/include/agentxx/util/log.h`)，而非 `std::cout/cerr`，避免干扰 TUI
- `TUILogSink` 接入右侧日志面板；`TestWarnErrorLogSink` 在测试中把 Warn/Error 透出到 stderr
- 捕获异常优先 `agentxx::util::catchError/catchErrorAsync` (放行 `CancelledException/NodeInterrupt`)，协程中勿 `catch(...)` 吞取消

## 5. 网络与重连测试

可利用 `clash` 等代理在运行时手动切断网络，测试自动重连与增量重放：

```sh
export http_proxy=http://127.0.0.1:7980
export https_proxy=http://127.0.0.1:7980
agentxx_cli tui
# 在 clash「连接」页面关闭 agent 连接，观察重连、Hello(lastSeq/tailHash) 与 Delta 去重
```

## 6. 编码与路径

- 优先 `std::string_view` 替代 `const std::string&`
- 路径统一经 `string_util` 的 `toCurrentSystemAbsolutePath` / `normalizePermissionPath` 做 `~/ ${VAR}` 展开与 `unix/windows/auto` 标准化；会话工作目录统一经 `AgentContext::getSessionWorkDir(sessionId)` 取值

## 7. 编译加速与排障

- Debug 默认启用 `ccache` + `mold/gold` + `PCH` + 单次编译 62 源文件复用；可用环境变量/选项覆盖 (见 `build/linux.md`)
- 编译器 ICE 时先重试或清理缓存 (`echo 3 > /proc/sys/vm/drop_caches`)，仍失败再排查代码
- Windows 禁止 `/FS` `/MP` (见 AGENTS.md)，`build` 目录勿手动改文件 (可能被覆盖)
