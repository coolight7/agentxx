- 难度: C
- 类型: bug修复 (崩溃/内存安全)
- 基于commit: 149d11c1
- 时间: 2026-11-07
- 需求:
```md
请修复 agentxx 崩溃 D:\0Acoolight\Program\cpp\agentxx\agentxx_asan.29288
```

## 现象

`agentxx_asan.29288` (Debug + ASan, TUI 本地一体化模式):

- `heap-use-after-free`, `READ of size 8`, 崩溃线程是 agent io 线程 (跑 `ioCtx->run()`);
- 地址 `1792 bytes inside of 3520-byte region`;
- 分配栈: `thread_info_base::allocate<awaitable_frame_tag>` (即一具协程帧);
- 释放栈: `thread_info_base::deallocate` / `~thread_info_base` (帧回收池销毁);
- 读取栈里能看到 `boost::process::v2::basic_process::async_wait_op_` 的完成处理器
  (`executor_function::impl<binder0<completer>>::ptr::reset`) 与 async-work 相关帧。

即: 一具协程帧被销毁后, 后台仍在跑的协程又去访问了它里面的东西 —— 撞上的是
"命令还在跑的时候后台任务被取消/收尾"这条路径。

## 根因

`AsyncWorkManager::runToolWork` (`agent/lib/src/agent/async_work.cpp`) 启动后台工具执行体时写成:

```c++
asio::co_spawn(
    ex,
    [state, tool, args = std::move(args)]() -> asio::awaitable<void> { ... }(),  // ← 内联调用
    asio::detached
);
```

- 传给 `co_spawn` 的是一具 **`awaitable`**, 它是 `[...]{...}()` **内联调用**产生的:
  按 C++ 规则, lambda 协程的帧只保存**闭包指针** (`this`), 不复制闭包; 内联调用产生的是
  **临时闭包**, 语句结束即析构 (`args` / `tool` / `state` 这些捕获随之销毁);
- 而这具协程是**独立的后台任务**: 取消时 `runToolWork` 先返回 (它只每 50 ms 看一次
  取消令牌, 然后 `co_return`, 工具继续跑到自己的超时, 结果被丢弃), 帧随之销毁;
- 后台协程之后从工具的异步等待里恢复 (`agentxx_execute_*` 的子进程 `async_wait`、
  定时器一类), 再去读闭包里的 `args` / `tool` → **use-after-free**。

`agentxx/util/task_scope.h` 的 `spawn` 注释里早就记过同一坑
("lambda 协程的帧只保存闭包指针, 内联调用产生的是临时闭包…实测 ASan 报
stack-use-after-scope"), 本处是唯一一处没遵守的 `co_spawn` 调用点
(全仓 `co_spawn` / `TaskScope::spawn` 逐个核对过)。

为什么最近才炸: 后台工具 (`kind=tool`) 是 2026-11-06 新加的能力; 只有"命令型工具
真的会挂起、且后台任务在它跑完前就被取消/收尾"时才会读到悬垂闭包。工具参数小的时候
(Json 没有堆分配) 读到的是"没被覆写的旧内存", 看起来一切正常 —— 长参数/进程型工具
才会踩到真实释放, 表现为随机崩溃。

## 修改

1. `agent/lib/src/agent/async_work.cpp` (`runToolWork`): 把**可调用对象**交给
   `co_spawn` (去掉末尾的 `()`)。这样闭包由 `co_spawn` 复制进 `co_spawn_state`,
   该状态活在入口协程帧里, 生命周期与后台任务一致; 后台协程访问捕获变量始终有效。
   注释里写清"取消时本函数先退出、工具仍在跑"这一语义, 并指向 task_scope.h 的同一约定。
2. `agent/plugins/agentxx_execute_command/execute_command_impl.h` (`procCancelWatchLoop`):
   同一类问题的第二处 —— 取消回调里 `asio::post(ex, [&outpip, &errpip, cancelTimer]{...})`
   捕获了**本协程帧里的 pipe 引用**。投递体在主工作收尾、帧销毁之后才跑到时就是悬垂引用。
   改为: 回调只 kill 进程 + 投递一个只捕获 `shared_ptr<steady_timer>` 的"取消计时器";
   关闭管道读句柄放回**本协程内部** (步骤 4, 唤醒后在自己的帧里做), 语义不变
   (进程树未杀净时仍能让 `async_read` 立即结束, 不会挂到 EOF)。

## 复现与测试

- 新增 `async_tools` 用例 `test_tool_closure_lifetime`:
  `agentxx_async_start {tool:"at_test_sleep", args:{sleepMs:400, marker:<512 字节>}}`
  → 等工具进入执行体 (停在睡眠里) → `cancelAsyncWork(id)` → 等工具自己跑完
  → 断言"挂起之后再读参数仍是原值"。
- 反向验证 (临时把 `},` 改回 `}()` 重新编译): 该用例**稳定复现**同类崩溃, 报告与用户那份
  几乎一致 —— 同一个 `awaitable_frame` 分配 (`3520-byte region`), 同一个释放栈
  (`runToolWork` 帧析构 → `thread_info_base::deallocate`), 只是读取偏移
  1824 vs 1792 字节 (闭包成员布局不同)。恢复修复后用例通过。
- 回归: `async_tools`(90) `async_work`(121) `event_subagent`(52) `event_tools`(97)
  `agent_host`(181) `command`(63) `worktree`(25) 全绿; 全量套件见 work.md。

## 后续注意

- 规则已同步到两处长期文档: `AGENTS.md` 功能设计新增"协程闭包生命周期"
  (传可调用对象, 不传内联调用/具名 lambda 产生的 awaitable),
  `docs/zh-cn/design/events.md` §8 的 `kind=tool` 小节写明后台工具协程这条路径。
- 后台工具"取消后继续跑到自己的超时、结果丢弃"是既有语义, 不改; 需要的是后台协程
  自己能安全地活到那一刻 (本次修复)。
- 同类排查口径: 以后看到 `co_spawn(...)` 的参数以 `}()` 或 `fn()` 结尾, 都要确认那个
  闭包/对象是否活得比协程久; 具名 lambda 也一样 (`&lambda` 会随作用域消失)。
