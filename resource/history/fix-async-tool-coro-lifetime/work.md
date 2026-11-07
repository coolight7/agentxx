# 实施记录

## 进度

- [x] 读 ASan 报告, 定位崩溃线程/对象类别 (协程帧, agent io 线程)
- [x] 逐处核对 `co_spawn` / `TaskScope::spawn` 调用点的闭包生命周期
- [x] 修复 `runToolWork` 的临时闭包 (根因)
- [x] 修复 `procCancelWatchLoop` 取消回调里的 pipe 悬垂引用 (同类, 同路径)
- [x] 补回归用例 `async_tools::test_tool_closure_lifetime`
- [x] 反向验证 (改回 `}()` 复现 → 恢复修复后通过)
- [x] 增量编译 (含插件 `agentxx_execute_command`) + 相关模块回归
- [x] 文档回填 (`AGENTS.md` 功能设计, `docs/zh-cn/design/events.md` §8)

## 每步改了什么 / 怎么验证

### 1. 从 ASan 报告定位对象与线程

- 报告里 `READ of size 8` 落在 "1792 bytes inside of 3520-byte region", 分配栈是
  `thread_info_base::allocate<awaitable_frame_tag>` → 对象是一具**协程帧**;
  释放栈是 `thread_info_base::deallocate` / `~thread_info_base` → 帧已被销毁
  (帧回收池那条路);
- 线程创建栈指向本地一体化模式里 `runLocalTuiUnifiedAsync` 起的线程, 且崩溃栈里有
  `io_context::run()` / `do_one` → agent io 线程;
- 崩溃栈里还有 `basic_process::async_wait_op_` 完成处理器和 async-work 相关帧 →
  后台任务 + 子进程命令。

符号化在 MSVC + ASan 下抖动很厉害 (报告里出现 `simdjson` / `std::filesystem` 之类的
无关帧), 所以只采信"对象类别 + 分配/释放路径 + 线程"这三条硬信息。

### 2. 核对闭包生命周期

把全仓 `asio::co_spawn(` 与 `TaskScope::spawn(` 的调用点逐个过了一遍:

- 绝大多数传的是可调用对象 (`[captures]() -> awaitable<...> {...}`), 由 `co_spawn`
  把闭包复制进 `co_spawn_state` (第 341 行), 生命周期与协程一致 → 安全;
- `agent/lib/src/agent/async_work.cpp` 的 `runToolWork` 传的是
  `[...]{...}()` —— 内联调用产生的**临时闭包**;
- 其它 `}()` 位置是普通同步 IIFE (取颜色/拼字符串) 或 `co_await [&]{...}()`
  (临时闭包在同一全表达式内结束, 安全), 以及 `co_spawn(ex, someFunc(arg), token)`
  这类"协程函数返回 awaitable"(参数已进帧) → 安全。

### 3. 修复

- `agent/lib/src/agent/async_work.cpp`: `runToolWork` 的 `co_spawn` 去掉末尾 `()`,
  并写注释说明"取消时本函数先退出、工具仍在跑", 与 `task_scope.h` 的约定互相引用。
- `agent/plugins/agentxx_execute_command/execute_command_impl.h`:
  - 取消回调 (可能在其他线程) 只做 `killProcGroup` + 投递 `[cancelTimer] { cancelTimer->cancel(); }`;
  - 关闭管道读句柄移到 `procCancelWatchLoop` 步骤 4 (定时器被唤醒后、在自己的帧里);
  - 回调不再捕获 `&outpip` / `&errpip`, 投递体不再持有任何帧内引用。

### 4. 回归用例与反向验证

`agent/test/core/test_async_tools.cpp`:

- `AtSleepTool` 增加"挂起之后再读一次参数" (读 `marker` 写进 `markerAfterSleep`,
  置 `finished`), 并提供 `callCount()` / `finishedOk()` / `markerAfterSleepText()`;
- 新用例 `test_tool_closure_lifetime`: 提交 `sleepMs=400` + 512 字节 `marker` 的
  后台工具 → 等它进入执行体 → `cancelAsyncWork` → 等它自己跑完 → 断言 marker 原值。

反向验证 (临时改回 `}()`, 单独编译 agentxx_test):

```
==ERROR: AddressSanitizer: heap-use-after-free
READ of size 1 ...
    #0 utilxx_base::Json::is_object()
    #1 AtSleepTool::execute_async$_ResumeCoro$1 test_async_tools.cpp:145
freed by thread T0 here:
    #1 boost::asio::aligned_delete
    #2 thread_info_base::deallocate<awaitable_frame_tag>
    #4 agentxx::agent::runToolWork$_ResumeCoro$1 async_work.cpp:311
    #7 awaitable_frame_base::destroy
0x... is located 1824 bytes inside of 3520-byte region
```

与用户那份报告同一类对象、同一释放路径、同一帧大小 (3520 字节), 只是读取偏移不同
(1824 vs 1792, 闭包成员布局差异)。恢复修复后该用例通过。

### 5. 编译与回归

- `agent/script/windows_debug_build.bat`: 成功 (exit=0);
- `agent/script/build_plugins.ps1 agentxx_execute_command`: 成功 (插件 dll 时间戳已更新);
- 相关模块 (`--log-level error`): `async_tools` 90 / `async_work` 121 /
  `event_subagent` 52 / `event_tools` 97 / `agent_host` 181 / `command` 63 /
  `worktree` 25, 合计 **629 断言全通过**;
- 全量 `agentxx_test` (`--log-level error`): **42231 通过 / 1 失败**, 唯一失败是
  `boundaries` 的"称呼与用词"规则在第三方库 `agent/third_party/cxx_pluginxx_ui` 的
  README/文档里命中 `载体` —— 与本次改动无关 (d8dd9309 的记录里同一条已知遗留, 那次是
  42117 通过 / 1 失败), 不在本次范围内修改 (`third_party` 非必要不改)。

## 遇到的问题 / 备注

- 崩溃二进制在 21:11 被重新编译过 (日志写于 20:35), 无法用原二进制再符号化;
  靠"帧大小 + 释放路径"与复现报告对上号。
- ASan 报告里的函数名大面积错位 (MSVC + 内联 + 回收分配器), 不要按帧名判断调用关系;
  能用的只有对象类别、分配/释放栈、线程, 以及 `agent/lib` 里未改动的源文件行号。
- 插件代码改动后必须跑 `build_plugins.ps1` 才会更新 `exec/plugins/<插件名>` 下的 dll
  (顶层 ALL 构建不产出插件动态库)。
