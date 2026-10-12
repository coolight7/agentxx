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

- `agent/test/core/test_remote_agent.cpp`
  - 新增 `keepAliveRun(Fn fn)`: 把可调用对象作为**按值协程参数**传入 → 参数会被复制进
    协程帧, 闭包与它捕获的 shared_ptr 随协程存活; 注释写清规则、用法与反例
  - 20 处 `co_spawn(ex, X->成员协程(), ...)` 改为
    `co_spawn(ex, keepAliveRun([X]() -> awaitable<void> { co_await X->成员协程(); }), ...)`
    (端点/服务端 shared_ptr 按值捕获)
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

## 验证

- release 全量 (修复后, 共 6 轮):
  - 敌意环境 `TERM=dumb NO_COLOR=1 LC_ALL=C LANG=C` 3 轮: 37581/0
  - 插桩 `MallocScribble=1 MallocGuardEdges=1 MallocErrorAbort=1` (释放内存填 0x55,
    大块分配加保护页, malloc 报错即中止) 2 轮: 37581/0
  - 默认环境 1 轮: 37581/0
  - (37581 = 原 37579 + 新增的 2 条退出断言)
- 单模块: `remote_agent` 512/0; `fake_provider remote_agent cancel` 603/0
- 环境矩阵: `tui_settings` + `tui_input` 在 5 种 locale 组合下 676/0
- debug/ASan: `remote_agent fake_provider` 558/0; 加 `tui_settings tui_input` 连跑 2 轮
  1234/0, 无 sanitizer 报告
- 结论: 修复后 6 轮全量 (含插桩) 未再复现 SIGSEGV; 修复前同类环境约 1/8 次出现

## 遗留

- 同类形状 (闭包 / 按引用捕获 + detached 启动) 在其它模块仍有, 目前未复现崩溃:
  `[ep = fx->endpoint]() -> awaitable<void> { co_await ep->runTransportLoop(); }` 形态
  (见 `test_input_delivery` / `test_jsonl_mode` / `test_prompt_stability` /
  `test_session_admin` / `test_session_sync` / `test_shutdown_stages`) —— 端点由 fixture
  持有所以暂时安全, 后续可统一改用 `keepAliveRun` 形态
- `test_fake_provider` 的 `cancelWatcherWhenTokenReady` 只保证不越界, 未等它退出
  (它最多循环 5s; 用例返回后仍可能继续跑, 只是不再触碰用例栈对象)
