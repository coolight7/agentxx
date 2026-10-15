# 修复 debug 测试套件: 源码路径推导 + ASan 堆统计 + 两处内存泄漏
- 难度: B
- 类型: bug修复 (测试)
- 基于commit: 5413f06ec5352d3b3f1f26239506b340d8bad49d
- 时间: 2026-10-15 08:10
- 需求:
```md
请修复最近 agentxx debug 的 agentxx_test 运行时崩溃
```

## 现象

debug (ASan + UBSan) 全量测试跑完后进程以**非 0 退出码**结束, 看上去像崩溃:

```
Total: passed=37680 failed=2
==294404==ERROR: LeakSanitizer: detected memory leaks
SUMMARY: AddressSanitizer: 3264 byte(s) leaked in 23 allocation(s).
```

- `[FAIL] line 1103: expected >= 30, got 0` (`config_keys`)
- `[FAIL] line 309: expected >= 102400, got 0` (`ftxui_text`)
- `[SKIP] boundaries: agent 源码根目录未找到 (从 __FILE__ 推导失败), 跳过` —— 整个
  架构边界自检模块静默不跑
- 泄漏可拆成两块, 与模块单独运行的读数完全对得上: `plugin_feature` 1744 字节 /
  16 次分配, `remote_agent` 1520 字节 / 7 次分配

复现稳定 (连续 3 轮全量失败项与泄漏字节数完全一致), 没有段错误、没有 ASan 报越界。

## 根因

### 1. `__FILE__` 被 ccache 改写成相对路径 (config_keys 失败 / boundaries 跳过)

最小实验 (在 `CCACHE_BASEDIR` 之内, 用绝对路径调用编译器):

```sh
CCACHE_BASEDIR=/path/to/agent ccache g++ -c /path/to/agent/.../p.cpp -o p2.o
strings p2.o | grep p.cpp      # → "p.cpp" (只剩文件名)
```

构建脚本 (`script/linux_*_build.sh` / `macos_*` / `cross_*`) 都 export
`CCACHE_BASEDIR=$src_dir`, 目的是"项目目录移动后缓存仍命中"; 代价是 ccache 会把编译器
收到的源文件绝对路径归一化成**相对编译工作目录**的路径, `__FILE__` 跟着变相对。

于是两个按"`__FILE__` 是绝对路径"写的自检都失效:

- `config_keys`: `fs::path{__FILE__}/../../..` 推出的是不存在的位置, 扫 `config_loader.cpp`
  得到 0 个键 → `expected >= 30, got 0`
- `boundaries`: 由 `__FILE__` 向上回溯找 `lib+client+test+plugins` 的目录特征, 找不到就
  静默 SKIP (最糟的一类失败: 自检被跳过却看不出问题)

### 2. ASan 接管分配器后 `mallinfo2` 恒为 0 (ftxui_text 失败)

`test_ftxui_text` 的渲染树内存上界断言用 `mallinfo2().uordblks` 的差值。ASan 只拦截了
`mallinfo` (没有 `mallinfo2`), 而 ASan 自己的分配器不经过 glibc 堆, 所以读数恒 0:

```sh
# 探针程序 (100 × 1KB std::string)
ASan 构建:   uordblks before=0     after=0      delta=0
普通构建:    uordblks before=74400 after=183584 delta=109184
```

该断言自 `c46cebe2` (引入 `measureHeapDelta`) 起在 Debug 下就**不可能通过**;
同文件里 markdown 渲染树那条只设上界, 于是"读到 0"被当成通过 (读数同样无意义)。

### 3. `plugin_feature` 泄漏 1744 字节: 三个用例加载插件后从不卸载

`test_plugin_impl_overrides_core_ask` / `test_call_busy_protection` / `test_spec_size_gate`
都 `loadPluginAsync("example_feature")` 后直接 `co_return`。实例 (及其功能点 shim /
钩子 shim) 只能由 `unloadAsync` (或管理器析构) 释放 —— 同模块其它用例都成对调用,
只有这三个漏了。`plugins` 模块单独跑无泄漏, 说明这是用例写法问题, 不是插件框架问题。

### 4. `remote_agent` 泄漏 1520 字节: 端点循环退出没等就返回

`test_session_controller_message_queue` 用分离协程起了三条循环: 客户端接收循环、
`sc->runTransportLoop()`、`sc->run()`。收尾只等**客户端接收循环**退出 (`clientLoopExited`),
另两条不等就返回。实测 (临时加 `weak_ptr` 探针) 收尾后 `sc` 仍有 2 个强引用、被释放的传输
只剩端点内部持有 —— 两条端点循环的挂起帧里还留着 `shared_ptr<传输>` 与 `shared_ptr<端点>`,
进程退出时这些帧还没被唤醒, 于是传输对象 + 两个 channel (及其内部 deque) 留在堆上:

```
Direct leak of 48 byte(s)   <- ChannelAgentIOTransport (makePair 里的 serverEnd)
Indirect leak of 248/248/384/384/144/64  <- 两个 channel 的控制块与内部缓冲
```

## 修改

- `agent/test/CMakeLists.txt`
  - 新增构建期宏 `AGENTXX_TEST_SOURCE_ROOT` (绝对路径 = `agent/`), 供读源码自检的模块使用
- `agent/test/include/agentxx-test/test_framework.h`
  - 新增 `sourceRootDir()` / `sourceFilePath()`: 依次尝试 构建期宏 → `__FILE__` 推导 →
    当前工作目录向上回溯, 每个候选都校验目录特征 (`lib/include`+`client`+`test`+`plugins`);
    两条注释写清"为什么不能只靠 `__FILE__`"
- `agent/test/core/test_boundaries.cpp`
  - 本地 `agentSourceRoot()` 改为调用共享 `sourceRootDir()`; SKIP 提示改为
    "检查构建是否传入 AGENTXX_TEST_SOURCE_ROOT"
- `agent/test/core/test_config_keys.cpp`
  - 加载器源码路径改用 `sourceFilePath("client/src/config_loader.cpp")`; 读不到时先打印
    一条说明 (源码根目录/该传什么宏), 再让既有下限断言失败
- `agent/test/client/test_ftxui_text.cpp`
  - 启用 ASan 时 (`__SANITIZE_ADDRESS__` / `__has_feature(address_sanitizer)`) 堆用量改读
    ASan 自己的计数 `__sanitizer_get_current_allocated_bytes` (GCC 的 sanitizer 头里没有
    `allocator_interface.h`, 故按 C 接口自行声明), 其余平台分支不变
- `agent/test/plugin/test_plugin_feature.cpp`
  - 三个用例收尾补 `unloadAsync("example_feature", 5s)` 并断言成功
- `agent/test/core/test_remote_agent.cpp`
  - 两侧传输都改为 `shared_ptr` 持有 (分离协程可能在用例返回后才被唤醒, 传输必须活得比
    用例栈长), `serverT` 交给端点后立即释放本地持有
  - `runTransportLoop()` / `run()` 各加退出标志, 收尾等**三条**分离协程全部退出并各断言一次
- `AGENTS.md`
  - `agent/test` 小节补两条: 源码根目录用 `AGENTXX_TEST_SOURCE_ROOT` (不能靠 `__FILE__`),
    ASan 构建下堆用量断言要用 ASan 自己的计数

## 验证

- debug 全量 (脚本 `script/linux_debug_build.sh` 构建, ASan+UBSan):
  `Total: passed=37701 failed=0`, **退出码 0, 无 sanitizer / LeakSanitizer 报告**
  (37680 → 37701 = boundaries 恢复运行 14 + 各修复模块新增断言 7)
- 单模块: `boundaries` 14/0 (修前 SKIP)、`config_keys` 163/0、`ftxui_text` 41/0、
  `plugin_feature` 145/0、`remote_agent` 518/0 连续 3 轮无泄漏
- 堆统计读数恢复真实值: `text nodes: 100 x 1024B -> 124100 bytes allocated`
  (断言区间 102400 ~ 204800)、`markdown render tree: 929B source -> 23568 bytes (x25.4)`
  (上界 ×60)

## 遗留

- ASan 分配计数走的是编译器运行库的接口: GCC/Clang 的 ASan 已验证 (Linux), MSVC 的
  `/fsanitize=address` 未在本机验证 —— 若该工具链不定义 `__SANITIZE_ADDRESS__`, 代码走
  原 CRT 分支 (行为与修前一致), 不会链接失败
- ccache 归一化 (`CCACHE_BASEDIR`) 仍会让 `__FILE__` 变相对路径 (调试信息不受影响:
  DWARF 的 `DW_AT_comp_dir` 仍是编译目录)。后续若有其它模块要按 `__FILE__` 定位源码,
  同样应改用 `sourceRootDir()` / 构建期宏
- 其它模块若也存在"加载插件后不卸载"的写法, 现在会由 ASan 退出检查报告; 本次只修了
  已报出来的三处
