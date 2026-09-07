# Agentxx 代码架构设计与重构优化方案

## 1. 概述与背景

Agentxx 是基于 **C++23（编译器开启 C++26/C17 标准）** 构建的跨平台 AI Agent 框架，目标是稳定运行于生产环境，支持 Linux x86_64 (含 WSL 扩展)、Windows 10+ x86_64 以及 Android 5.0+。

框架核心采用单线程/多协程并发架构（基于 Boost.Asio 与 Neograph 图执行引擎），在保持低资源消耗和高并发处理能力的同时，实现了分层解耦的 Client-Agent 模型：
- **Client 端**：通过 FTXUI（TUI）或 Stdio（CLI）负责界面展示与用户交互；
- **Server/Agent 端**：`BaseAgent` 和 `CodeAgent` 负责驱动 ReAct 执行循环、LLM API 调用、工具调度、上下文管理与多会话隔离；
- **插件层**：采用遵循 API v1 规范的纯 C ABI 体系，支持跨平台隔离与多实例共存。

随着功能演进（如 worktree 模式、双端插件数据通信、分页尾窗同步等特性的引入），代码库中逐渐积累了一些模式重复、职责分散、历史语法冗余以及潜在的安全性与性能瓶颈。本文档系统梳理了各核心模块的架构现状，并给出了具体、可落地的重构与优化方案。

---

## 2. 系统整体架构与功能拓扑

```
+-------------------------------------------------------------------------+
|                               Client 端                                 |
|  +--------------------+   +---------------------+   +----------------+  |
|  | MessageListComponent|   |  Sidebar / Logs UI  |   | Overlays (HIL) |  |
|  +--------------------+   +---------------------+   +----------------+  |
|                         \            |            /                     |
|                   +----------------------------------+                  |
|                   |         TUIClientAgentIO         |                  |
|                   +----------------------------------+                  |
+--------------------------------------|----------------------------------+
                                       | WireMessage (Channel / WS)
+--------------------------------------v----------------------------------+
|                              Server/Agent 端                            |
|                   +----------------------------------+                  |
|                   |       SessionServerAgentIO       |                  |
|                   +----------------------------------+                  |
|                                      |                                  |
|     +--------------------------------v----------------------------+     |
|     |                         BaseAgent                           |     |
|     |  +---------------------+        +------------------------+  |     |
|     |  |   GraphEngine       |        |      EventBus          |  |     |
|     |  | (ReAct: Start->LLM  |        | (强类型事件流 /        |  |     |
|     |  |  ->Toolcall->End)   |        |  HIL 请求-响应流)      |  |     |
|     |  +---------------------+        +------------------------+  |     |
|     |  +---------------------+        +------------------------+  |     |
|     |  |     Sessions        |        |   Middleware Context   |  |     |
|     |  | (viewMsgs+llmMsgs)  |        | (Permission/Summarize/ |  |     |
|     |  |  SQLite 持久化      |        |  Skill/Memory/Subagent)|  |     |
|     |  +---------------------+        +------------------------+  |     |
|     +-------------------------------------------------------------+     |
|                                      |                                  |
|         +----------------------------+----------------------------+     |
|         |                            |                            |     |
|         v                            v                            v     |
|  +--------------+             +--------------+             +----------+ |
|  | C ABI 插件集 |             |   MCP 客户端 |             | Worktree | |
|  | (文件/命令等)|             | (外部工具集) |             | 隔离环境 | |
|  +--------------+             +--------------+             +----------+ |
+-------------------------------------------------------------------------+
```

### 核心设计机制
1. **双消息集模型**：
   - `viewMessages`：append-only 完整展示历史，用于客户端同步、尾窗分页加载与 UI 渲染，配合 `ChainHash`（FNV-1a 逐段链式哈希）校验状态指纹。
   - `llmMessages`：可裁剪压缩的上下文数组，每次进入 LLM 前由 `ModelCallWrapNode::repairMessages` 进行修复（合并连续同角色消息、去重并补齐工具调用 ID），并由 `SummarizationMiddleware` 做动态滑动窗口压缩。
2. **节点包装与中间件拦截**：
   - 节点均继承自 `WrapHandleBaseNode<T>`，在执行核心逻辑外围注入 `onNodeStart` -> 中间件 handles 循环 -> `baseRun` -> 中间件逆序 `onNodeEnd`，形成栈式调用结构。
3. **强类型事件总线与 HIL**：
   - 单向流（`EventStream<T>`）承载增量流式输出；双向流（`RequestResponseStream<Req, Resp>`）承载工具权限拦截与用户中断确认，通过超时控制与取消令牌防止死锁。

---

## 3. 重点重构方案与代码示例

### 方案 1：WireProtocol 编解码重构与传输层解耦

#### 现状问题
- `agent/lib/include/agentxx/agent/io/wire_protocol.h` 包含了全部约 1000 行的序列化辅助函数，而 `agent/lib/src/agent/wire_protocol.cpp` 却是个空的占位文件。
- 反序列化函数 `deserialize` 被分散在 `agent/lib/src/agent/io/ws_io_transport.cpp` 中，包含近 30 个 `else if (t == io::MsgType::...)` 分支，里面手写字段提取逻辑（例如 `req.id = j.value("id", int64_t{0})`）。
- 部分结构体在 `wire_protocol.h` 中有独立的 `fromJson` 函数，另一部分却直接硬编码在 `ws_io_transport.cpp` 内部，格式严重不一致。
- 导致协议编解码与 WebSocket 传输层强耦合，无法被其他传输层（如 stdio / ACP / 进程间管道）直接复用。

#### 重构设计
1. **对称设计 `toJson` 与 `fromJson`**：为每个 `WireXxx` 结构体提供成对的转换函数。
2. **下沉实现至 `wire_protocol.cpp`**：
   头文件只保留结构声明与轻量接口，将 `serialize(const WireMessage&)` 和 `deserialize(std::string_view)` 集中实现在 `wire_protocol.cpp` 中。
3. **采用函数指针/映射表替代冗长的 `else if` 链**。

#### 优化后代码设计

```cpp
// agent/lib/include/agentxx/agent/io/wire_protocol.h
namespace agentxx::agent::io {

// 1. 每个 Wire 类型具备统一声明的 toJson / fromJson
neograph::json toJson(const WireHello& msg);
WireHello helloFromJson(const neograph::json& j);

neograph::json toJson(const WireInterruptRequest& msg);
WireInterruptRequest interruptRequestFromJson(const neograph::json& j);

// 2. 对外提供统一的序列化与反序列化接口
std::string serialize(const WireMessage& msg);
std::optional<WireMessage> deserialize(std::string_view jsonText);

} // namespace agentxx::agent::io
```

```cpp
// agent/lib/src/agent/wire_protocol.cpp
#include "agentxx/agent/io/wire_protocol.h"
#include <unordered_map>

namespace agentxx::agent::io {

std::string serialize(const WireMessage& msg) {
    return std::visit([](const auto& m) -> std::string {
        return toJson(m).dump();
    }, msg);
}

using DeserializerFn = WireMessage(*)(const neograph::json&);

static const std::unordered_map<std::string_view, DeserializerFn>& getDeserializerMap() {
    static const std::unordered_map<std::string_view, DeserializerFn> s_map = {
        {MsgType::Hello,             [](const neograph::json& j) -> WireMessage { return helloFromJson(j); }},
        {MsgType::HelloAck,          [](const neograph::json& j) -> WireMessage { return helloAckFromJson(j); }},
        {MsgType::UserInput,         [](const neograph::json& j) -> WireMessage { return userInputFromJson(j); }},
        {MsgType::InterruptRequest,  [](const neograph::json& j) -> WireMessage { return interruptRequestFromJson(j); }},
        {MsgType::InterruptResponse, [](const neograph::json& j) -> WireMessage { return interruptResponseFromJson(j); }},
        {MsgType::DeltaMsg,          [](const neograph::json& j) -> WireMessage { return deltaMsgFromJson(j).value_or(WireDelta{}); }},
        // ... 注册全部类型
    };
    return s_map;
}

std::optional<WireMessage> deserialize(std::string_view jsonText) {
    try {
        auto j = neograph::json::parse(jsonText);
        if (!j.is_object()) return std::nullopt;

        std::string type = j.value("type", "");
        const auto& map = getDeserializerMap();
        auto it = map.find(type);
        if (it != map.end()) {
            return it->second(j);
        }
    } catch (...) {
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace agentxx::agent::io
```

---

### 方案 2：异常分类机制统一与 `catchError` 原生支持 `void`

#### 现状问题
1. **`WrapHandleBaseNode::run` 中的三重异常捕获样板**：
   在 `agent/lib/include/agentxx/nodes/wrap_handle.h` 的 `run` 方法中，`onHandleStart`、`baseRun` 以及 `onHandleEnd` 三个执行阶段分别包含了 7 个相同的 `catch` 分支（处理 `CancelledException`、`NodeInterrupt`、`isCancelAbort`、`boost::exception`、`std::exception`、`...`），代码重复达 150+ 行。
2. **`catchError` 不支持 `void` 带来广泛的写法变形**：
   `agent/lib/include/agentxx/util/exception.h` 显式注释 `// 不可传入 void 类型，编译会失败，可以用 bool 占位`，迫使全工程充斥着：
   ```cpp
   co_await catchErrorAsync<bool>(
       [&]() -> asio::awaitable<bool> {
           // 业务逻辑 ...
           co_return true;
       },
       [](std::string) -> asio::awaitable<bool> { co_return false; }
   );
   ```
3. **强制 `std::function` 传参引发类型擦除与堆分配**。

#### 重构设计
1. 提取结构化异常分类函数 `classifyCurrentException`。
2. 利用 C++20 `if constexpr (std::is_void_v<T>)` 使 `catchError` 与 `catchErrorAsync` 原生支持 `void`。
3. 允许接收任意模板可调用对象 `Callable&&`。

#### 优化后代码设计

```cpp
// agent/lib/include/agentxx/util/exception.h
namespace agentxx::util {

struct ExceptionClassification {
    bool isControlFlow = false; // 取消信号或中断异常
    std::string errInfo;
    std::exception_ptr exPtr;
};

/// 统一异常分类分析工具
inline ExceptionClassification classifyCurrentException(
    const std::shared_ptr<neograph::graph::CancelToken>& cancelToken = nullptr
) noexcept {
    ExceptionClassification res;
    try {
        throw;
    } catch (const neograph::graph::CancelledException& e) {
        res.isControlFlow = true;
        res.errInfo = "cancelled";
        res.exPtr = std::current_exception();
    } catch (const neograph::graph::NodeInterrupt& e) {
        res.isControlFlow = true;
        res.errInfo = "interrupt";
        res.exPtr = std::current_exception();
    } catch (const neograph_asio_system_error& e) {
        if (isCancelAbort(e, cancelToken)) {
            res.isControlFlow = true;
            res.errInfo = "operation cancelled";
            res.exPtr = std::make_exception_ptr(
                neograph::graph::CancelledException("operation aborted")
            );
        } else {
            res.errInfo = autoTryConvertToUtf8(e.what());
            res.exPtr = std::current_exception();
        }
    } catch (const boost::exception& e) {
        res.errInfo = autoTryConvertToUtf8(boost::diagnostic_information(e));
        res.exPtr = std::current_exception();
    } catch (const std::exception& e) {
        res.errInfo = autoTryConvertToUtf8(e.what());
        res.exPtr = std::current_exception();
    } catch (...) {
        res.errInfo = "Unknown error";
        res.exPtr = std::current_exception();
    }
    return res;
}

/// 原生支持 void 与非 void 返回类型的异步异常捕获
template<typename T = void, typename Func, typename OnError>
asio::awaitable<T> catchErrorAsync(
    Func&& func,
    OnError&& onError,
    std::shared_ptr<neograph::graph::CancelToken> cancelToken = nullptr
) {
    try {
        if constexpr (std::is_void_v<T>) {
            co_await func();
            co_return;
        } else {
            co_return co_await func();
        }
    } catch (...) {
        auto info = classifyCurrentException(cancelToken);
        if (info.isControlFlow) {
            std::rethrow_exception(info.exPtr);
        }
        if constexpr (std::is_void_v<T>) {
            co_await onError(std::move(info.errInfo));
            co_return;
        } else {
            co_return co_await onError(std::move(info.errInfo));
        }
    }
}

} // namespace agentxx::util
```

此时，`WrapHandleBaseNode::run` 的异常捕获简化为：
```cpp
// 优化前：每个阶段手写 7 个 catch 块
// 优化后：
try {
    co_await onHandleStart(*item, in);
    startedIdxs.push_back(i);
    continue;
} catch (...) {
    auto info = agentxx::util::classifyCurrentException(in.ctx.cancel_token);
    errorRethrow = info.isControlFlow;
    errInfo = std::move(info.errInfo);
    errorPtr = info.exPtr;
    onHandleStartError(errorRethrow, true, errInfo, *item, in, out);
    break;
}
```
三处总共减少 120+ 行冗余代码。同时，全工程可直接编写 `co_await catchErrorAsync<void>([&]() -> asio::awaitable<void> { ... });`，不再需要多余的 `bool` 占位和 `co_return true;`。

---

### 方案 3：`XXRouter` 前缀树内存模型优化与实例隔离

#### 现状问题
在 `agent/lib/include/agentxx/util/router.h` 中：
1. **$O(N^2)$ 的低效子树析构**：
   `RouterTreePort::clearChild` 使用手动指针管理：
   ```cpp
   for (auto it = child.begin(); it != child.end();) {
       it->second->clearChild();
       delete (it->second);
       child.erase(it);
       it = child.begin(); // 每次删除重置迭代器，导致 O(N^2)
   }
   ```
2. **共享静态 LRU 缓存引发竞态与悬挂指针**：
   ```cpp
   static agentxx::util::LruCache<std::string, _RouterCacheValue_s>& getCacheMap() {
       thread_local agentxx::util::LruCache<std::string, _RouterCacheValue_s> instance{1024};
       return instance;
   }
   ```
   `_RouterCacheValue_s` 中缓存的是节点裸指针 `RouterTreePort* treeptr`。如果在同一个线程内有两个 `XXRouter` 实例（例如权限系统的文件路由与 HTTP Server 路由），它们将共享同一个静态 LRU 缓存，导致一个实例查到的指针指向另一个实例的树，产生严重隐患；并且 `~XXRouter()` 强制全局 `clear()` 会破坏其他同线程路由实例的缓存。

#### 重构设计
1. 将 `child` 改为 `std::map<std::string, std::unique_ptr<RouterTreePort>>`，通过智能指针管理生命周期，子树释放直接调用 `child.clear()`，时间复杂度降为线性的 $O(N)$。
2. 将 LRU 缓存直接作为 `XXRouter` 类的普通成员变量，实现各个实例完全独立的缓存空间，彻底消除悬挂指针与全局状态污染。

#### 优化后代码设计

```cpp
template<typename HANLDE_TPYE, size_t HANLDE_NUM>
class XXRouter {
protected:
    struct RouterTreePort {
        std::string path;
        RouterTreePort* parent = nullptr; // 非拥有型弱引用
        std::array<std::shared_ptr<HANLDE_TPYE>, HANLDE_NUM> handles{};
        std::map<std::string, std::unique_ptr<RouterTreePort>> child;

        explicit RouterTreePort(std::string_view in_path = "") noexcept : path(in_path) {}

        void clearChild() noexcept {
            child.clear(); // 自动递归析构，O(N) 线性释放
        }
    };

    struct RouterCacheValue {
        std::string path;
        RouterTreePort* treeptr = nullptr;
    };

    // 缓存作为实例独占成员，避免全局/thread_local 实例间相互冲刷与野指针
    agentxx::util::LruCache<std::string, RouterCacheValue> cacheMap_{1024};
    RouterTreePort routerTree{"/"};

public:
    XXRouter() noexcept = default;
    ~XXRouter() = default; // 默认析构即可安全释放整棵树与局部缓存
    // ...
};
```

---

### 方案 4：`MiddlewareWrapHandle` 结构化参数重构

#### 现状问题
在 `agent/lib/include/agentxx/middlewares/middleware.h` 中，`MiddlewareWrapHandle` 的构造函数接受 7 个回调参数：
```cpp
MiddlewareWrapHandle(
    std::string_view name,
    std::weak_ptr<AgentContext> ctx,
    const onGraphNodeBeforeCallFunc& in_onAgentcallStart = nullptr,
    const onGraphNodeAfterCallFunc&  in_onAgentcallEnd   = nullptr,
    const onGraphNodeBeforeCallFunc& in_onModelcallStart = nullptr,
    const onGraphNodeBeforeCallFunc& in_onModelcallRun   = nullptr,
    const onGraphNodeAfterCallFunc&  in_onModelcallEnd   = nullptr,
    const onGraphNodeBeforeCallFunc& in_onToolcallStart  = nullptr,
    const onGraphNodeAfterCallFunc&  in_onToolcallEnd    = nullptr
);
```
在 `agent/lib/src/agent/code_agent.cpp` 装配 `LogPrint` 时出现如下代码：
```cpp
agentContext->middlewareHandleContext->handles.push_back(
    std::make_shared<MiddlewareWrapHandle<BaseMiddlewareState>>(
        "LogPrint",
        agentContext,
        (onGraphNodeBeforeCallFunc) nullptr,
        (onGraphNodeAfterCallFunc) nullptr,
        (onGraphNodeBeforeCallFunc) nullptr,
        [config = ...](NodeInput& in) -> asio::awaitable<void> { ... },
        (onGraphNodeAfterCallFunc) nullptr,
        [...](NodeInput& in) -> asio::awaitable<void> { ... },
        [...](const NodeInput& in, NodeOutput& out) -> asio::awaitable<void> { ... }
    )
);
```
连续 4 个显式 `(func_type) nullptr` 转换严重破坏了代码的可读性，且位置极易错乱。

#### 重构设计
引入 `MiddlewareHooks` 结构体，结合 C++20 指定初始化器（Designated Initializers），让未用到的钩子天然默认为 `nullptr`。

#### 优化后代码设计

```cpp
// agent/lib/include/agentxx/middlewares/middleware.h
namespace agentxx::middleware {

struct MiddlewareHooks {
    onGraphNodeBeforeCallFunc onAgentcallStart = nullptr;
    onGraphNodeAfterCallFunc  onAgentcallEnd   = nullptr;
    onGraphNodeBeforeCallFunc onModelcallStart = nullptr;
    onGraphNodeBeforeCallFunc onModelcallRun   = nullptr;
    onGraphNodeAfterCallFunc  onModelcallEnd   = nullptr;
    onGraphNodeBeforeCallFunc onToolcallStart  = nullptr;
    onGraphNodeAfterCallFunc  onToolcallEnd    = nullptr;
};

template<BaseMiddlewareStateType T>
class MiddlewareWrapHandle : public BaseMiddlewareHandle<T> {
public:
    MiddlewareHooks hooks;

    MiddlewareWrapHandle(
        std::string_view name,
        std::weak_ptr<agentxx::agent::AgentContext> ctx,
        MiddlewareHooks in_hooks = {}
    ) : BaseMiddlewareHandle<T>(name, ctx), hooks(std::move(in_hooks)) {}

    asio::awaitable<void> onModelcallRunFunc(neograph::graph::NodeInput& in) override {
        if (hooks.onModelcallRun) {
            co_await hooks.onModelcallRun(in);
        }
        co_return;
    }
    // 其余钩子依此类推 ...
};

} // namespace agentxx::middleware
```

调用方代码重构后：
```cpp
// 消除全部强转，仅指定需要拦截的节点阶段
agentContext->middlewareHandleContext->handles.push_back(
    std::make_shared<agentxx::middleware::MiddlewareWrapHandle<agentxx::middleware::BaseMiddlewareState>>(
        "LogPrint",
        agentContext,
        agentxx::middleware::MiddlewareHooks{
            .onModelcallRun = [config = agentContext->agentConfig](neograph::graph::NodeInput& in) -> asio::awaitable<void> {
                if (config->logPrintMessagesBeforeLLM) {
                    agentxx::middleware::BaseMiddlewareHandleInterface::printMessages(
                        in.state.get_messages(),
                        config->logPrintMessagesBeforeLLMWithSystemMsg
                    );
                }
                co_return;
            },
            .onToolcallStart = [ctx = std::weak_ptr<AgentContext>(agentContext), config = agentContext->agentConfig](neograph::graph::NodeInput& in) -> asio::awaitable<void> {
                if (config->logPrintToolcall) {
                    agentxx::nodes::ToolcallWrapNode::defStdoutLogOnToolcallStart(in);
                }
                if (auto ctxPtr = ctx.lock()) {
                    if (auto session = ctxPtr->sessions->get(in.ctx.thread_id)) {
                        session->activity = SessionActivity::ExecutingTool;
                    }
                }
                co_return;
            },
            .onToolcallEnd = [ctx = std::weak_ptr<AgentContext>(agentContext), config = agentContext->agentConfig](const neograph::graph::NodeInput& in, neograph::graph::NodeOutput& result) -> asio::awaitable<void> {
                if (config->logPrintToolcall) {
                    agentxx::nodes::ToolcallWrapNode::defStdoutLogOnToolcallEnd(in, result);
                }
                if (auto ctxPtr = ctx.lock()) {
                    if (auto session = ctxPtr->sessions->get(in.ctx.thread_id)) {
                        session->activity = SessionActivity::Idle;
                    }
                }
                co_return;
            }
        }
    )
);
```

---

### 方案 5：公共哈希函数库沉淀与魔数修正

#### 现状问题
1. `agent/lib/src/agent/conversation_types.cpp` 中定义：
   ```cpp
   static uint64_t fnv1a(std::string_view data, uint64_t seed) {
       uint64_t hash = seed;
       for (unsigned char c : data) {
           hash ^= c;
           hash *= 1099511628211ULL;
       }
       return hash;
   }
   // 种子: 14695981039346656037ULL
   ```
2. `agent/lib/src/agent/session_store.cpp` 中重新定义：
   ```cpp
   static uint64_t fnv1a64(std::string_view s) {
       uint64_t hash = 1469598103934665603ULL; // 笔误：末尾少了一位 '7'
       for (unsigned char c : s) {
           hash ^= c;
           hash *= 1099511628211ULL;
       }
       return hash;
   }
   ```
不仅逻辑重复，而且常数出现笔误（非标准的 FNV-1a 偏移基准），可能导致散列分布质量下降。

#### 重构设计
在 `agent/lib/include/agentxx/util/hash.h` 中提供标准、通用的 `fnv1a64` 算法，供 `ChainHash`、`SessionStore` 等统一复用。

#### 优化后代码设计

```cpp
// agent/lib/include/agentxx/util/hash.h
#pragma once
#include <cstdint>
#include <string_view>

namespace agentxx::util::hash {

inline constexpr uint64_t kFnv1a64OffsetBasis = 14695981039346656037ULL;
inline constexpr uint64_t kFnv1a64Prime       = 1099511628211ULL;

[[nodiscard]] inline constexpr uint64_t fnv1a64(
    std::string_view s,
    uint64_t seed = kFnv1a64OffsetBasis
) noexcept {
    uint64_t hash = seed;
    for (unsigned char c : s) {
        hash ^= c;
        hash *= kFnv1a64Prime;
    }
    return hash;
}

} // namespace agentxx::util::hash
```

---

### 方案 6：`ToolcallWrapNode` 中断错误消息回填逻辑复用

#### 现状问题
在 `agent/lib/src/nodes/toolcall.cpp` 中，`ToolcallWrapNode::onHandleStartError` 与 `ToolcallWrapNode::onHandleBaseRunError` 两处代码完全相同：
- 获取当前状态中的消息；
- 获取上一条带有 `tool_calls` 的 `assistant` 消息；
- 针对每个 `tool_call` 构造包含错误提示的 `ChatMessage`；
- 序列化后打包为 `ChannelWrite{"messages", ...}` 写入结果。

两处代码除了字符串前缀 `[Start/Exception aborted: {}]` 与 `[BaseRun/Exception aborted: {}]` 之外无任何差别。

#### 优化后代码设计

```cpp
// agent/lib/src/nodes/toolcall.cpp
namespace {

void insertAbortedToolResults(
    std::string_view phasePrefix,
    std::string_view exceptionStr,
    neograph::graph::NodeInput& in,
    neograph::graph::NodeOutput& result
) noexcept {
    auto messages = in.state.get_messages();
    auto* assistantMsg
        = agentxx::middleware::BaseMiddlewareHandleInterface::getLastAssistantToolcallMessage(messages);
    if (!assistantMsg || assistantMsg->tool_calls.empty()) {
        return;
    }

    auto appendToolResult = neograph::json::array();
    for (const auto& tool : assistantMsg->tool_calls) {
        auto msg = neograph::ChatMessage{
            .role         = "tool",
            .content      = fmt::format("[{}/Exception aborted: {}]", phasePrefix, exceptionStr),
            .tool_call_id = tool.id,
            .tool_name    = tool.name,
            .flags        = neograph::MessageFlag::AutoInserted,
        };
        neograph::json msgJson;
        neograph::to_json(msgJson, msg);
        appendToolResult.push_back(std::move(msgJson));
    }
    result.writes.push_back(neograph::graph::ChannelWrite{
        "messages",
        std::move(appendToolResult),
    });
}

} // namespace

void ToolcallWrapNode::onHandleStartError(
    bool errorRethrow,
    bool isCurrentError,
    std::string_view exceptionStr,
    agentxx::middleware::BaseMiddlewareHandleInterface& /*item*/,
    neograph::graph::NodeInput& in,
    neograph::graph::NodeOutput& result
) noexcept {
    if (!errorRethrow && isCurrentError) {
        insertAbortedToolResults("Start", exceptionStr, in, result);
    }
}

void ToolcallWrapNode::onHandleBaseRunError(
    bool errorRethrow,
    bool isCurrentError,
    std::string_view exceptionStr,
    neograph::graph::NodeInput& in,
    neograph::graph::NodeOutput& result
) noexcept {
    if (!errorRethrow && isCurrentError) {
        insertAbortedToolResults("BaseRun", exceptionStr, in, result);
    }
}
```

---

### 方案 7：基础工具函数现代化与命名修正

#### 现状问题与优化
1. **`stringVectorJoin` 的现代化与泛型扩展**：
   原实现限制为 `const std::vector<T>&`，并使用较重的 `std::ostringstream`。
   借助 C++20 `std::ranges::input_range` 和 `fmt::join`（或预分配 `std::string`）：
   ```cpp
   template<std::ranges::input_range Range>
   [[nodiscard]] inline std::string stringJoin(const Range& range, std::string_view sep = ", ") {
       return fmt::format("{}", fmt::join(range, sep));
   }
   ```
   支持 `vector`、`set`、`span`、数组及视图表达式，且执行速度更快。
2. **函数命名拼写修正**：
   - `agent/lib/include/agentxx/util/string_util.h` 中的 `strSplitCopid` 更名为 `strSplitCopied`（保留原函数为 `[[deprecated]]` inline 以保持向后兼容）。
3. **换行符规范化统一收拢**：
   - `filesystem_impl.h` 中 `normalizeCrlfToLf` 注释写反（将 CRLF 转为 LF，注释却写为“LF 转换为 CRLF”）。将其统一下沉至 `string_util.h`，对外提供 `normalizeCrlfToLf(std::string&)` 和 `normalizeLfToCrlf(std::string&)`。

---

### 方案 8：客户端巨石组件解耦（`agent_tui.cpp`）

#### 现状问题
`agent/client/src/io/tui/agent_tui.cpp` 单文件体积超过 111 KB。除了主布局之外，该文件还混杂了：
1. 平台底层的剪贴板写入实现（Windows Win32 `OpenClipboard`/`GlobalAlloc` 与 Linux/macOS 的 `OSC 52` 转义序列直接内联）；
2. 通用覆盖层模态窗口（`MermaidDiagramOverlay`、`TextOverlay` 等）的详细创建和派发细节。

#### 重构设计
1. 提取独立的剪贴板工具模块：`agentxx-client/util/clipboard.h` / `.cpp`：
   ```cpp
   namespace agentxx::client {
   bool copyTextToSystemClipboard(std::string_view text);
   }
   ```
2. 将覆盖层弹窗工厂函数提取到 `overlays.h` / `overlays.cpp` 中，使得 `TUIClientAgentIO` 专注于：
   - 驱动协议交互（`onPeerMessage` / `onDelta`）；
   - 管理 UI 与后台协程线程同步；
   - 维护核心状态容器。

---

## 4. 重构路线与优先级排期建议

| 阶段 | 重构模块 | 收益与目标 | 风险与兼容性 |
|:---|:---|:---|:---:|
| **Phase 1** | **`exception.h` 支持 `void` & 异常分类提取**<br>**哈希算法统一下沉 (`fnv1a64`)**<br>**`MiddlewareHooks` 参数结构体化** | 消除大量无用 `bool` 占位，消除 `(func_type)nullptr` 丑陋强转，修复 FNV 散列魔数笔误。 | **极低**（完全内聚，不改动外部公共 ABI） |
| **Phase 2** | **`ToolcallWrapNode` 错误回填复用**<br>**`string_util` 泛型化与拼写修正** | 消除逐行重复代码，统一工具层中断结果结构。 | **极低**（提供别名平滑兼容旧接口） |
| **Phase 3** | **`XXRouter` 内存安全重构**（智能指针 + 局部缓存） | 彻底消除 $O(N^2)$ 子树析构和多实例共享全局静态缓存的潜在竞态风险。 | **低**（需回归测试权限系统与路由单元测试） |
| **Phase 4** | **`WireProtocol` 下沉到 `.cpp` 与协议解耦**<br>**`agent_tui.cpp` 剪贴板提取与弹窗解耦** | 减少各编译单元头文件展开开销，使传输协议具备跨介质独立复用能力；大幅精简客户端代码。 | **中**（涉及网络协议测试与 UI 交互回归测试） |

---

## 5. 结论

通过上述重构方案的实施，Agentxx 将在保持高性能异步并发和纯 C ABI 插件隔离优势的前提下：
1. **代码复用度大幅提升**：消除重复编写的协议解析链、异常捕获块、哈希算法和工具错误处理逻辑；
2. **代码可读性显著增强**：通过 C++20 指定初始化器、原生 `void` 协程异常处理和模板 ranges 泛型工具，消除所有冗杂的显式类型强转与伪返回值；
3. **架构与内存安全性得到巩固**：彻底消除路由缓存的跨实例竞争、子树二次析构性能退化以及 FNV 常量偏差，为长期稳定运行于复杂生产环境提供坚实的工程支撑。
