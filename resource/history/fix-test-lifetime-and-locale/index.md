# 修复测试套件: 分离协程生命周期 + 环境敏感用例
- 难度: B
- 类型: bug修复 (测试)
- 基于commit: 699ff7d9552a327e92824d34e22536457098309f
- 时间: 2026-10-12 11:10
- 需求:
```md
好的请继续修复 其他测试问题
```

## 现象

release 全量测试在多数环境下稳定通过 (`Total: passed=37579 failed=0`), 但有两类问题:

1. 环境敏感: 换 shell / 换终端就失败
   - `tui_settings` 的 `test_auto_language_detection`: 用例只改 `LANG`, 会被运行环境里
     已有的 `LC_ALL` 覆盖 (产品按 POSIX 优先级读语言: `LC_ALL` → `LC_MESSAGES` →
     `LANG` → `LANGUAGE`, 见 `agent/client/src/io/tui/tui_settings.cpp`), 于是同一份
     测试在不同 shell 下结果不同
   - `tui_input` 的 `test_file_picker_navigation_without_filter`: 断言列表末端条目
     (`song.mp3`) 出现在渲染结果里, 而文件选择弹窗的可见行数取自**物理终端尺寸**
     (`ctx.terminalSize()`), 尺寸/界面文案长度一变末端条目就被裁掉
2. 偶发 SIGSEGV (约 1/8 次): `TERM=dumb NO_COLOR=1 LC_ALL=C LANG=C` 下全量运行, 无任何
   输出直接 `exit=139`。日志停在 `remote_agent` 模块的 `[remote] cancel...` 之后,
   无 core、无 `.ips` 报告 (系统 `kern.coredump=1`、`ulimit -c` 为 0 ⇒ 没留下转储)

## 根因

### 环境敏感用例

用例写法问题 (产品行为正确):

- 语言: 只设 `LANG` 时, 若环境已有 `LC_ALL`, 产品按优先级取到的仍是 `LC_ALL`
- 文件选择: 渲染尺寸跟随真实终端, 用例不该依赖它

### 偶发 SIGSEGV: 分离协程的生命周期没管住

用例里大量使用 `asio::co_spawn(..., asio::detached)` 启动长时间运行的接收循环。
关键语言规则 (本项目 `agentxx::util::TaskScope::spawn` 的注释里已有同一条):

> lambda 协程调用只把 **this 指针**存进协程帧, **闭包对象本身不入帧**。

最小实验 (`/tmp` 里 20 行验证程序, 与本仓库无关):

```cpp
auto closure = [p = std::make_shared<Probe>(11)]() -> Task {
    co_await SuspendOnce{};   // 挂起
    std::printf("恢复后 v=%d\n", p->v);
};
closure();          // 协程挂起后闭包离开作用域
                    // → 实测输出 "恢复后 v=0": 捕获的 shared_ptr 已释放, 读到已回收内存
```

于是两种写法都会越界:

- `asio::co_spawn(ex, io->runTransportLoop(), asio::detached)`: 帧里只有端点的 this 指针,
  端点所有权在用例栈上的 `shared_ptr` 上。用例返回后端点析构, 而挂起的 `recv()` 可能在
  这之后才被唤醒 → 恢复后 `onPeerMessage()` 访问已释放对象 (虚调用跳到野指针, 因此
  **无任何输出**、偶发)
- `auto clientLoop = [&]() -> awaitable<void> { while (...) { co_await ...; } };` +
  分离启动: 闭包对象与按引用捕获的 `mu` / `turnResults` 都是用例栈上的对象, 用例返回
  后唤醒即访问已释放内存

崩溃观测点 (`[remote] cancel...` 之后) 与这个形状吻合: 上一个用例返回 → 它的分离协程在
下一次 `co_await` 让出控制权时被唤醒 → 野指针跳转。相同原因在本模块早前还修过一次
(`test_session_controller_queue_resume_after_abort` 的注释: 传输对象要按值捕获)。

## 修改

- `agent/test/include/agentxx-test/test_coro_util.h` (新增共享头)
  - `agentxx::test::keepAliveRun(Fn fn)`: 把可调用对象作为**按值协程参数**传入 →
    参数会复制进协程帧, 闭包与它按值捕获的 shared_ptr 随协程存活; 注释写清规则、
    用法与反例 (与 `agentxx::util::TaskScope::spawn` 的既有做法一致)
- 全测试套件迁移 (**21 个文件、82 处**): `asio::co_spawn(..., <闭包>, asio::detached)`
  一律改为 `asio::co_spawn(..., keepAliveRun(<闭包>), ...)`
  - 各夹具的接收循环 / 端点驱动循环 (`[clientT = fx->clientT, recorder = fx->recorder]`,
    `[ep = fx->endpoint]() { co_await ep->runTransportLoop(); }`, `ep->run()`):
    `test_input_delivery` `test_jsonl_mode` `test_prompt_stability` `test_session_admin`
    `test_session_sync` `test_shutdown_stages`
  - 并发 / 工具 / provider 用例: `test_concurrency` `test_command_tools`
    `test_anthropic_provider` `test_openai_provider` `test_agent_host` `test_cancel`
    `test_filesystem_tools` `test_memgrowth` `test_session_persistence`
  - 插件用例: `test_plugins` `test_client_plugins`
  - 客户端用例与测试主程序: `test_update_check` `test.cpp` (主协程, 限定名调用)
  - 另外两处第三参数不是 `asio::detached` 但同样会游离的站点也一并处理:
    `test_filesystem_tools` (完成处理器形态) 与 `test_http` (`bind_cancellation_slot` 形态)
- `agent/test/core/test_remote_agent.cpp`
  - 20 处 `co_spawn(ex, X->成员协程(), ...)` 改为 `keepAliveRun([X]() { ... })`
    (端点 / 服务端 shared_ptr 按值捕获; 文件内局部实现已删除, 改用共享头)
  - 2 处 `clientLoop`: 经 `keepAliveRun` 保活; 并新增退出标志, 用例收尾关闭传输后等它
    退出再返回 (`XX_TEST_EXPECT_TRUE(loopExited)`, 最长等 500ms)
- `agent/test/core/test_fake_provider.cpp`
  - `cancelWatcher` 由用例内 lambda 改为自由协程 `cancelWatcherWhenTokenReady(ex, agent)`,
    `agent` 按值入参 (分离协程可能在用例返回之后才被唤醒)
- `agent/test/client/test_tui_settings.cpp`
  - 模拟系统语言时同时设置 `LC_ALL` / `LC_MESSAGES` / `LANG` 并清掉 `LANGUAGE`,
    用例结束恢复原值
- `agent/test/client/test_tui_input.cpp`
  - 文件选择用例固定离屏视口 (`ctx.viewportWidth/viewportHeight = 100/30`, 与该用例
    渲染的尺寸一致), 不再随物理终端漂移
- 4 处"形态特殊但已确认安全"的分离启动补了说明注释 (为何不需要 keepAliveRun):
  `test_agent_host` (spawnOne("a"))、`test_http` (命名闭包 run)、
  `test_plugin_runtime` (op->wait() + 取消槽)

## 验证

- release 全量 (每轮约 4 分钟), 共 12 轮全 37581/0:
  - 迁移前 6 轮: 敌意环境 `TERM=dumb NO_COLOR=1 LC_ALL=C LANG=C` 3 轮、
    插桩 `MallocScribble=1 MallocGuardEdges=1 MallocErrorAbort=1` (释放内存填 0x55、
    大块分配加保护页、malloc 报错即中止) 2 轮、默认环境 1 轮
  - 全量迁移后 6 轮: 敌意 3 轮、插桩 2 轮、默认 1 轮
  - (37581 = 原 37579 + `remote_agent` 新增的 2 条退出断言)
- 单模块: `remote_agent` 512/0; `fake_provider remote_agent cancel` 603/0;
  `agent_host http plugin_runtime` 1196/0
- 环境矩阵: `tui_settings` + `tui_input` 在 5 种 locale 组合下 676/0
- debug/ASan: 20 个受影响模块 3438/0, `plugins client_plugins` 1203/0, 无 sanitizer 报告
- 内存: 迁移后全量 RSS 未见上升 (示例: 迁移前 144 MB → 迁移后 130 MB; 夹具析构时已
  关闭传输 / 停止端点, 循环随之退出并释放保活引用)
- 结论: 修复与迁移后 12 轮全量 (含插桩放大) 未再出现 SIGSEGV; 修复前同类环境约 1/8 次出现

## 遗留

- 仍有一类写法未改: 分离协程按**引用**捕获用例栈局部量并跨挂起点使用 (例如
  `test_client_plugins` 里 `[&]` 捕获 `probe` 的定时器用例)。经 `keepAliveRun` 后闭包
  存储已安全, 但被引用的局部量仍需用例自身在作用域内把协程跑完 (这些用例会 drain io);
  后续可继续收紧: 把共享状态放进 `shared_ptr` 并按值捕获
- 未复现的那次 SIGSEGV 无 core 可查 (shell `ulimit -c` 为 0); 若再出现, 可用
  `MallocScribble=1 MallocGuardEdges=1 MallocErrorAbort=1` 放大后定位
