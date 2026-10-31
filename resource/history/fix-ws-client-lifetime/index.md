- 难度: B
- 类型: bug修复
- 基于commit: 325f5726
- 时间: 2026-10-30
- 需求:
```md
请修复最近的运行时崩溃 "D:\0Acoolight\Program\cpp\chatbot\agentxx_asan.28680"
（随后追加：继续修复 chatbotStop 问题）
（再追加：先把 chatbot 的几个 fast_tool 改成 blocking_tool，
 再排查 plugin_example_chatbot 测试的异常原因并修复）
```

## 现象

chatbot 插件运行一段时间后进程被 ASan 掐断 (`agentxx_asan.28680`):

```
ERROR: AddressSanitizer: heap-use-after-free ... thread T20
READ of size 8 ... in std::unique_ptr<utilxx::WsClient::Impl>::operator->()
  #1 utilxx::WsClient::sendText::<lambda_2>::operator()  .../cxx_utilxx/src/ws_client.cpp:161
freed by thread T20 here:
  #1 utilxx::WsClient::`scalar deleting dtor'
  #4 agentxx::chatbot::QqGateway::closeSocket()      .../platform/qq/qq_gateway.cpp:85
  #5 agentxx::chatbot::QqGateway::scheduleReconnect() .../platform/qq/qq_gateway.cpp:389
  #6 agentxx::chatbot::QqGateway::startHeartbeat::<lambda_1>()
```

`ws_client.cpp:161` 当时是发送错误分支里的 `impl_->closed_ = true;` —— 读的是已释放的
`WsClient` 对象的成员 `impl_` (整个对象只有一个 `unique_ptr` 成员, 所以报的是 8 字节)。

## 根因

`WsClient` 的收发接口是成员协程, 协程帧里只存了 `this`; 挂起 (等 `async_write_some`)
之后再恢复, 所有成员访问都经由这个 `this`:

- `QqGateway::closeSocket()` 的写法是 `abort()` 紧接 `reset()`, 按旧注释 ("调用后 recv
  会立即以错误返回, 此后方可安全析构") 这看起来是安全的;
- 但 `abort()` 只是 `shutdown + close` 底层 socket —— 挂起的写操作是被**取消**,
  它的完成回调要等线程回到 io_context 才会执行, 也就是 `reset()` 之后;
- 协程恢复后落到错误分支 `impl_->closed_ = true;`, `WsClient` 已经析构 → use-after-free。

触发链正是心跳: `startHeartbeat` 的定时器回调里先 `sendHeartbeat()` (投递一次发送),
判定 `missedAcks_ >= 2` 后 `scheduleReconnect()` → `closeSocket()` 销毁连接; 此前
已经在飞的发送随后以"socket 已关闭"的错误恢复, 撞上已释放对象。

同类风险不只一处: 只要调用方"先 abort 再释放连接", 任何在飞的 send/recv/ping/close
都会读到已释放的 `WsClient`。库的注释把这件事说反了, 是库的问题。

## 修改

### 1. cxx_utilxx: 收发协程自己持有实现体 (`include/utilxx/ws_client.h`, `src/ws_client.cpp`)

- `impl_` 由 `std::unique_ptr<Impl>` 改为 `std::shared_ptr<Impl>` (构造参数仍是
  `unique_ptr`, 调用方无感);
- 收发实现搬进匿名命名空间的自由协程 (`sendFrameOnImpl` 供 sendText/sendBinary 共用,
  以及 `pingOnImpl` / `closeOnImpl` / `recvOnImpl`), 参数就是 `std::shared_ptr<Impl>`
  —— **协程参数在协程创建时复制进帧**, 所以 `sendText("x")` 被调用的那一刻引用计数
  已经 +1, 之后析构 `WsClient` 不影响它;
- 公开方法变成**非协程**的转发函数 (无 `co_await`), 实现体引用在调用当刻就交出去:
  ```cpp
  asio::awaitable<std::expected<void, std::string>> WsClient::sendText(std::string_view payload) {
      return sendFrameOnImpl(impl_, payload, true);
  }
  ```
- 顺带去掉三份重复的 ssl/非 ssl 写循环, 合并成模板 `writeMessageLoop`;
- `~WsClient()` / move 赋值会调 `closeTransport(impl_.get())` (原 `abort()` 的语义),
  保证"丢弃连接对象"仍然会关掉 socket, 只是不再顺手把实现体删掉;
- 头文件注释改成事实: abort 之后可以立刻析构, 但挂起的操作不会当场返回。

### 2. chatbot 插件: 收发期间持有连接 (`platform/qq/qq_gateway.{h,cpp}`)

- `ws_` 改为 `std::shared_ptr<utilxx::WsClient>`; `sendFrame` / `readLoop` 各自
  copy 一份进协程 (`self->ws_ != client` 作为"连接已被换掉"的判据);
- `closeSocket()` 仍是 `abort()` + 放开引用, 但注释改成实际语义。

### 3. chatbot 停机顺序: 先停线程, 再析构平台对象 (`agentxx_chatbot.cpp` 等)

第一处崩溃修完之后, 同一类问题还留在停机路径上 (`chatbotStop` 的"遗留"部分):

- `ChatbotCtx::stopIo()` 原本在 join 之后 `io.reset()`, 即**立刻释放 io_context**,
  而账号/平台/面板定时器里都存着它的 `executor`; 之后再析构这些对象就是访问已释放的
  service (定时器析构要回查 timer service);
- `accounts.clear()` 原本在**插件 io 线程上**执行: 析构返回后该线程仍会继续派发已排队的
  handler, 挂起的 `connect()` (等 `api_.gatewayUrl()`) / `doSend()` (等
  `api_->sendMessage()`) 若在那时恢复, 碰到的是已释放的平台/接口对象;
- `QqPlatform::stop()` 自己在 io 线程上 `gateway.reset()` + `api_.reset()` +
  `uploader_.reset()` —— 同样是在协程还挂在它们的请求上时就把对象删了。

改法:

- `stopIo()` 只停线程 (`work.reset → io->stop → join`), **不释放** `io_context`;
  由于 `io` 是 `ChatbotCtx` 的第一个成员 (逆序析构时最后一个死), 平台/定时器都在它之前
  析构。新增 `ioStopped` 原子标记替代原来的 `!io` 判断 (`runOnIo` / `runOnIoBlocking`
  在停线程后直接丢弃工作, 不会把任务投进一个不再运行的 context 里干等 25 秒);
- `chatbotStop` 顺序改为: 平台 `stop()` + outbound/engine `shutdown()` (在 io 线程上) →
  关通道 → 卸根 → **`stopIo()`** → **清 `accounts`** (加 `accountsMutex`) →
  `panelTimer.reset()`; `chatbotStart` 的人设根挂载失败回滚路径同样处理;
- `QqPlatform::stop()` 只做"停机" (`stopped_` / `sink_` / 网关 `stop()`), 不再释放
  `gateway_` / `api_` / `uploader_` —— 它们随对象析构释放 (声明顺序 api_→gateway_→uploader_,
  逆序析构保证网关先于 api_ 死)。`doSend()` 在上传之后补一次 `stopped_` 复查, 停了就不发;
- `ChatbotEngine::shutdown()` / `ChatbotOutbound::shutdown()` 除了 cancel 也把定时器
  对象一起 `reset()`; `QqGateway::stop()` 同样 `reconnectTimer_.reset()`;
- `~ChatbotCtx()` 补 `stopIo()`: 宿主若跳过 stop 直接销毁实例, `std::thread` 仍可 join
  时析构会 `std::terminate`。

### 4. 工具参数跨线程传递 (`agentxx_chatbot.cpp` 的 `runToolOnIo`)

7 个 `chatbot_*` 工具的工作体都是"把活 post 到插件 io 线程再等结果", 但原来把
`args` / `tid` (宿主给的两个 `string_view`)**按引用**捕进那个 lambda, 结果 / 错误串
也在调用方栈上:

```cpp
const bool ok = c.runOnIoBlocking([&]() {
    result = ac::toolSendMessage(*deps, tid, args);   // args/tid 可能已悬空
});
```

只要 `runOnIoBlocking` 因超时 (25 s) 先返回, 调用方就收摊了, 已排队的 lambda 再执行
时读的就是已释放的宿主缓冲 —— 实测报 `chatbot_tools.cpp:28` 的
`allocation-size-too-big` (从垃圾长度构造 `std::string`), 回写结果则往已释放的栈上写。

现在统一走 `runToolOnIo(c, args, tid, body)`: 输入拷一份、输出放 `shared_ptr`, 工具体
只拿到自己的副本; 7 个包装体的重复样板也一起收掉了。

### 5. 回归测试 (`agent/test/core/test_websocket.cpp`)

新增 `test_ws_destroy_with_pending_op` (模块 `websocket`, 输出行
`[ws] destroy with pending op...`):

- 服务端只握手、不读不写; 客户端分别 (a) 发 16 MiB (堵在 `async_write_some`),
  (b) 等 `recv()`;
- 确认操作确实挂起后 `abort()` + 析构连接对象, 再断言操作在 5 秒内以错误收尾。

改到库上做验证 (临时 stash 掉 ws_client 的修改再编译) 时, 该用例复现出与用户报告
**完全一致**的 ASan 报告 (`ws_client.cpp:161` + `unique_ptr<WsClient::Impl>::operator->`);
改回修复版后 `websocket` 模块 254 项全通过。

停机顺序部分没有单独的用例: 现有 `plugin_example_chatbot` 的"卸载插件"一步走的就是
`chatbotStop` → `chatbotDestroy` 全流程 (假平台 + 真实 `AgentHost`, Debug/ASan 构建)。

### 6. `fast_tool` → `blocking_tool` (`agentxx_chatbot.cpp` 的 `registerToolsOn`)

7 个 `chatbot_*` 工具里, 除 `chatbot_send_message` 外的 5 个 (`chatbot_chat_info` /
`chatbot_history` / `chatbot_status` / `chatbot_set_mode` / `chatbot_debug_outbox`)
原来注册成 `fast_tool`, 即在**主程序 IO 线程上就地执行**; 但它们的工作体都是
`runOnIoBlocking` —— 投递到**插件** io 线程再阻塞等结果。而插件 io 线程上的活
(开通道 / 投输入 / 要面板驱动票) 反过来又要等主程序 IO 线程。两边互等:

- 日志大量出现 `Plugin operation \`chatbot_status\` start blocked IO for over 100ms`;
- 极端情况一直卡到 `runOnIoBlocking` 的 25 秒超时, 工具返回
  `[Error: chatbot 插件未就绪 (io 线程无响应)]` (53 字节)。

统一改成 `blocking_tool` (offload 到工作线程执行, 主程序 IO 线程不再被占), 两边
都只是"等", 不会互堵。`registerToolsOn` 的注释写清了这条理由, 免得以后有人按
"读一下状态很快"又改回 `fast_tool`。改完后实测 `blocked IO` 告警归零。

### 7. 测试装置自身的竞争 (`test_agent.cpp` / `test_plugin_example_chatbot.cpp`)

`plugin_example_chatbot` 连跑多次会偶发 ASan 报错, 栈是
`json.cpp:1382 Json::const_iterator::operator==` 的 heap-use-after-free, 释放点
在 `test_agent.cpp:308` 的 `g_da_sim_requests.push_back(...)`。根因是测试装置, 与插件无关:

- `test_plugin_example_chatbot.cpp` 的 `lastRequestText()` 直接在**测试线程**上
  `g_da_sim_requests.back().dump()`, 而模拟模型服务器的 HTTP 线程正在
  `push_back` —— `vector` 扩容时旧元素被释放, 测试线程还在读;
- 另外 `g_da_sim_response_content` / `g_da_sim_tool_calls` / `g_da_sim_delay_ms` 这些
  配置量同样是"用例写、服务线程读"的裸全局, 没有同步。

改法:

- `test_agent.h` 增加 `daSimMutex()` (当时叫 `g_da_sim_mutex`, 后来随 §8 改成递归锁) 与几个存取函数
  (`daSimLastRequestText` / `daSimRequestsSnapshot` / `daSimSetToolCalls` /
  `daSimSetResponseContent` / `daSimSetDelayMs`), 注释写明"碰这些全局量必须在锁内,
  `g_da_sim_request_count` 是原子量例外";
- 模拟器 handler 把本次响应要用的值在锁内**一次性**拷成局部量 `SimSnapshot`,
  之后跨 `co_await` 不再持锁; 前缀缓存状态、请求记录写入、收尾清理、
  `startDaSimServer()` / `DaSimServer::stop()` 的重置块都进锁;
- `test_plugin_example_chatbot.cpp` 的配置写入与请求读回全部改走上面的函数。

改完后该模块连跑 4 次全绿 (156 项 / 36~37 s)。

> 第二处诱发因素也一并消失了: 5 个 `fast_tool` 阻塞主程序 IO 线程导致的 25 秒超时
> 之后, 测试会把 53 字节的错误文本当 JSON 解析 (`TAPE_ERROR: input 53 bytes`)。
> 迁移到 `blocking_tool` 后不再出现。

### 8. 模拟器共享量改成持锁包装 (`agentxx-test/core/test_agent.h`)

上面 §7 只盖到了 `plugin_example_chatbot`, 其余约 20 个测试文件仍在裸读写 `g_da_sim_*`
(写点约 300 处, 读点约 60 处): `test_agent_host` / `test_prompt_cache_prefix` /
`test_prompt_stability` / `test_remote_agent` / `test_input_delivery` /
`test_http_recorder` / `test_jsonl_mode` / `test_jsonl_runner` / `test_cancel` 等。
逐点加锁改动面太大, 所以改成"写法不变、内部加锁":

- 新增 `template <typename T> class DaSimValue`: 读 (隐式转换成 `T` 或 `.get()`) 在锁内
  拷一份副本, 写 (`= 值`) 在锁内整体替换; 并按 `requires` 只在用得上的类型上提供
  `clear` / `size` / `empty` / `push_back` / `back` / `operator[]` / `dump` / `value` /
  `contains` / `++` / `--` 等成员;
- 拷贝构造 = 锁内取快照, 所以 `const auto request1 = g_da_sim_last_request;` 拿到的是
  独立副本 —— 原写法一行都不用改;
- 全局量声明成 `DaSimValue<std::string|int|bool|double|utilxx_base::Json|std::vector<Json>>`,
  `g_da_sim_request_count` 保持 `std::atomic<int>`;
- 锁改成函数内静态递归锁 `daSimMutex()` (函数内静态量避开跨翻译单元初始化顺序问题):
  包装量自己拿锁, 需要"多个量一起改"的地方 (`daSimSetToolCalls`、模拟器 handler 的
  `SimSnapshot`、`startDaSimServer` / `DaSimServer::stop` 的重置块) 再显式 `lock_guard`; 递归锁
  保证包装量与显式加锁可以嵌套 (否则 handler 的锁内再读包装量会自己锁死)。

编译器随后只报出一处缺口 (`test_prompt_stability.cpp` 用了 `.contains`) —— 补齐即可,
其余文件零改动。

### 9. 禁用 / 卸载时拆挂载的时机

现象: 插件测试跑完后日志里有
`agent 'root' 卸载插件 ... 失败: plugin operation rejected: provider is closing`,
并伴随 `AgentContext destroyed with plugin mount ... still attached`。

根因: 禁用/卸载只把实例从挂载上**标记**成待拆 (`detachInstance` 设
`pluginMounted=false` / `unmountPending=true`), 真正调插件 `unmount` 的
`PluginMount::unmountOneAsync` 只在禁用事务里调一次, 卸载路径从不调 —— 于是:

- 卸载后挂载记录留在 `entries_` 里, 一直揎着实例到挂载析构 (告警"仍有实例未拆挂载");
- 之后 `unmountAgentAsync` 再拆时实例已经 Closed, 插件 `unmount` 被拒 (即
  `provider is closing`)。

修复:

- 框架 `PluginHostLifecycle` 新增扩展点 `onInstanceUnloadingAsync` (默认空实现),
  在 `unloadAsyncUntil` 里摘完注册、调插件 `stop` 与 `releaseInstanceResources` +
  **之前**调用 —— 此时实例是"关闭中"不是 Closed, 插件 `unmount` 还能跑, 插件自管的根
  也还没被销毁;
- agent 侧 `PluginManager` 把"从全部挂载上拆下来"抽成
  `detachInstanceFromMountsAsync`, 禁用与卸载两个钩子共用;
- `PluginMount::mountOneAsync` 补上"欠着的 unmount": 挂载记录还在但
  `unmountPending` 为真时 (禁用后不等完成就启用的快路径, 禁用事务会主动作废)
  先把旧的 unmount 补完再重新挂载, 否则旧挂载状态丢失, 插件再也拿不到释放它的机会;
- `requestStopForDisable` 不再因"没有 stop 要补"就整个跳过 —— 禁用事务里除了 stop
  还有拆作用域那一步。

回到原来的问题: 禁用 → 启用现在能完整恢复 (工具、人设根、通道都可重建), 关键是要
**等禁用完成再启用**; 面板上连点的情况由上面"补欠着的 unmount"与
`startForEnable` 里"先补 stop 再 start"应付。

## 验证

- `agentxx_test websocket` → passed=254 failed=0
- `agentxx_test remote_agent network_timeout` → passed=567 failed=0
- `agentxx_test session_sync shutdown_stages` → passed=47 failed=0
- `agentxx_test websocket chatbot_core chatbot_qq plugin_example_chatbot` → passed=1171 failed=0
- 全量 debug 构建通过 (含 `libagentxx_chatbot.dll` 重编), 改动文件无新增编译告警
- `agentxx_test plugin_example_chatbot` → passed=185 failed=0 (38 s; 含新增的
  禁用→启用、禁用后不等完成就启用两段用例)
- `agentxx_test boundaries plugins plugin_cleanup plugin_feature plugin_hooks
  plugin_resources plugin_host_mount plugin_multi_mount plugin_multi_instance
  plugin_runtime plugin_agent_runtime plugin_example_runtime plugin_llm_uncensored
  client_plugins` → passed=3141 failed=0

## 剩余已知问题

- **插件在自己的 `stop` 里同步卸根时仍会打"挂载未拆"告警**: chatbot 的 `chatbotStop`
  用 `unmountRoot` 卸人设根, 那是同步销毁 agent 的路径 (`destroyAgent`), 而实例当时
  还挂在那个根上, 于是日志出现
  `destroying agent ... with pending plugin shutdown` 与
  `AgentContext destroyed with plugin mount ... still attached`。
  验证过另一种顺序 (先拆挂载再调 stop) 会让 `chatbotStop` 的 `stopIo()` 卡在
  `ioThread.join()` 上 (插件 io 线程此时正等主程序 io 线程, 而后者正阻塞在 join),
  已回退。要彻底消掉这两条告警, 需要 `AgentHost::unmountRoot` 走异步销毁
  (`destroyAgentAsync`) 或给 `PluginMount` 增加"不调插件回调的同步拆全部"。
- **`test_agent_host.cpp` 一类文件的写点仍未包成块**: 包装量保证单次读写无竞争, 但
  "连续写 4~5 个量"仍可能被服务线程看到中间态 (断言据此对比同一轮请求时不受影响)。

