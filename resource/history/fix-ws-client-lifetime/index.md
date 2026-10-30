- 难度: B
- 类型: bug修复
- 基于commit: 325f5726
- 时间: 2026-10-30
- 需求:
```md
请修复最近的运行时崩溃 "D:\0Acoolight\Program\cpp\chatbot\agentxx_asan.28680"
（随后追加：继续修复 chatbotStop 问题）
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

## 验证

- `agentxx_test websocket` → passed=254 failed=0
- `agentxx_test remote_agent network_timeout` → passed=567 failed=0
- `agentxx_test session_sync shutdown_stages` → passed=47 failed=0
- `agentxx_test websocket chatbot_core chatbot_qq plugin_example_chatbot` → passed=1171 failed=0
- 全量 debug 构建通过 (含 `libagentxx_chatbot.dll` 重编), 改动文件无新增编译告警

停机顺序部分没有单独的用例: 现有 `plugin_example_chatbot` 的"卸载插件"一步走的就是
`chatbotStop` → `chatbotDestroy` 全流程 (假平台 + 真实 `AgentHost`, Debug/ASan 构建)。

## 顺带发现 (未改)

- **`plugin_example_chatbot` 本身不稳定** (与本次修复无关): 在本机连跑多次经常挂, 典型:
  `json.cpp:1382 Json::const_iterator::operator==` 的 heap-use-after-free, 偶尔
  `_Container_base12 / std::exchange` 的 access-violation, 偶尔工具超时后测试仍把
  `[Error: chatbot 插件未就绪 (io 线程无响应)]` 当 JSON 解析。把插件的改动全部 stash 掉
  (改动前基线) 重编对比: 基线同样频繁失败, 失败形态一致。初判是测试装置自身的竞争
  (模拟模型服务器线程与用例协程共用 `g_da_sim_*` 那些全局 `Json`)。
- **`fast_tool` + 阻塞等 io 线程**: `chatbot_status` / `chatbot_chat_info` / `chatbot_history` /
  `chatbot_set_mode` / `chatbot_debug_outbox` 注册为 `fast_tool` (在宿主 IO 线程上就地跑),
  工作体却是 `runOnIoBlocking` —— 实测日志出现 "Plugin operation `chatbot_status` start
  blocked IO for over 100ms", 最坏情况会在宿主 IO 线程与插件 IO 线程之间互等到 25 秒超时。
  要么改注册为 `blocking_tool` (移到工作线程), 要么改成异步工具; 属于单独一轮的活。
- **禁用 → 启用不恢复**: `disable` 之后 `enable` 不恢复工具与根 (再往后卸载报
  `plugin operation rejected: provider is closing`)。基线同样如此 —— 是"同一实例 stop 后
  再 start"这条路径本身的问题 (插件把 `router`/`engine`/`outbound` 的会话状态跨轮次留着,
  框架侧 disable 事务也是投递执行, 需要等它完成才能 enable)。要支持热启用, 得单独排一遍。
