#pragma once

#include "agentxx/agent/context.h"
#include "agentxx/util/container_util.h"
#include "agentxx/util/log.h"
#include "asio/io_context.hpp"
#include "fmt/format.h"
#include <any>
#include <cstdlib>
#include <functional>
#include <memory>
#include <neograph/llm/rate_limited_provider.h>
#include <neograph/llm/schema_provider.h>
#include <neograph/neograph.h>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace asio = ::boost::asio;

namespace agentxx {
namespace tools {
class XXToolBase;
class XXToolWrap;
} // namespace tools

namespace middleware {

using onGraphNodeBeforeCallFunc
    = std::function<asio::awaitable<void>(neograph::graph::NodeInput& in)>;
using onGraphNodeAfterCallFunc = std::function<asio::awaitable<
    void>(const neograph::graph::NodeInput& in, neograph::graph::NodeOutput& result)>;

class MiddlewareContext;
class InterruptHandleArg;

class BaseMiddlewareState {
public:

    BaseMiddlewareState() {}

    virtual ~BaseMiddlewareState() {}
};

template<typename T>
concept BaseMiddlewareStateType
    = std::same_as<T, BaseMiddlewareState> || std::derived_from<T, BaseMiddlewareState>;

/// 接口类型
/// - 主要用于接收多种泛型参数, 见
/// [MiddlewareContext::handles]，handles
///   需要接收多种不同继承后的模版类型
///   BaseMiddlewareHandle<BaseMiddlewareStateType>，当 state
///   被继承时编译会失败，因此拉出 [BaseMiddlewareHandleInterface] 无 state
///   模版参数作为基本类型
class BaseMiddlewareHandleInterface {
protected:
public:

    /// 谨慎存储/修改 middleware 中的变量，
    /// 这是一个agent中所有会话共享的，单会话变量应该放 state 内

    /// 名称
    std::string                                 name;
    std::weak_ptr<agentxx::agent::AgentContext> agentContext;
    /// 是否已禁用 (禁用后 WrapHandleBaseNode 遍历跳过, 不执行任何钩子)
    /// - 供插件热卸载/禁用使用: 运行中置位安全 (len 已缓存), 轮末由
    ///   PluginManager::flushPendingCleanup 从 handles 摘除
    /// - 普通中间件不使用, 保持 false
    bool disabled = false;
    /// 会被添加移动到 agent 中，完成后此处留空数组
    std::vector<std::unique_ptr<agentxx::tools::XXToolBase>> toolcalls{};
    /// 每个 [Middleware] 全局共享，按会话ID 取值 <sessionId, state>
    std::map<std::string, std::shared_ptr<BaseMiddlewareState>, std::less<>> states{};

    BaseMiddlewareHandleInterface(
        std::string_view                            in_name,
        std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
    );

    /// ================ warp call ================
    virtual asio::awaitable<void> onAgentcallStartFunc(neograph::graph::NodeInput& in) = 0;

    virtual asio::awaitable<void> onAgentcallEndFunc(
        const neograph::graph::NodeInput& in,
        neograph::graph::NodeOutput&      result
    ) = 0;

    virtual asio::awaitable<void> onModelcallStartFunc(neograph::graph::NodeInput& in) = 0;

    virtual asio::awaitable<void> onModelcallRunFunc(neograph::graph::NodeInput& in) = 0;

    virtual asio::awaitable<void> onModelcallEndFunc(
        const neograph::graph::NodeInput& in,
        neograph::graph::NodeOutput&      result
    ) = 0;

    virtual asio::awaitable<void> onToolcallStartFunc(neograph::graph::NodeInput& in) = 0;

    virtual asio::awaitable<void>
        onToolcallEndFunc(const neograph::graph::NodeInput& in, neograph::graph::NodeOutput& result)
        = 0;

    virtual ~BaseMiddlewareHandleInterface();

    static neograph::json getLastMessageJson(const neograph::graph::NodeInput& in);

    static std::optional<neograph::ChatMessage> getLastMessage(const neograph::graph::NodeInput& in
    );

    static const neograph::ChatMessage*
        getLastAssistantToolcallMessage(std::vector<neograph::ChatMessage>& messages);

    static const neograph::ChatMessage*
        getLastToolcallResultMessage(std::vector<neograph::ChatMessage>& messages);

    static void printMessage(const neograph::ChatMessage& msg, size_t index = 1);

    static void printMessages(
        const std::vector<neograph::ChatMessage>& messages,
        bool                                      printSystemMsg = true
    );
};

template<BaseMiddlewareStateType T>
class BaseMiddlewareHandle : public BaseMiddlewareHandleInterface {
protected:
public:

    BaseMiddlewareHandle(
        std::string_view                            in_name,
        std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
    ) :
        BaseMiddlewareHandleInterface(in_name, in_agentContext) {}

    // 如果想添加 system msg，应当在 [onAgentcallStartFunc] 等一轮只执行一次的节点中处理
    // 否则会被重复添加多次
    asio::awaitable<void> onAgentcallStartFunc(neograph::graph::NodeInput& in) override {
        co_return;
    }

    asio::awaitable<void> onAgentcallEndFunc(
        const neograph::graph::NodeInput& in,
        neograph::graph::NodeOutput&      result
    ) override {
        co_return;
    }

    asio::awaitable<void> onModelcallStartFunc(neograph::graph::NodeInput& in) override {
        co_return;
    }

    asio::awaitable<void> onModelcallRunFunc(neograph::graph::NodeInput& in) override {
        co_return;
    }

    asio::awaitable<void> onModelcallEndFunc(
        const neograph::graph::NodeInput& in,
        neograph::graph::NodeOutput&      result
    ) override {
        co_return;
    }

    asio::awaitable<void> onToolcallStartFunc(neograph::graph::NodeInput& in) override {
        co_return;
    }

    asio::awaitable<void> onToolcallEndFunc(
        const neograph::graph::NodeInput& in,
        neograph::graph::NodeOutput&      result
    ) override {
        co_return;
    }

    /// ================ state ================
    virtual asio::awaitable<void> stateReadBlock(const std::function<asio::awaitable<void>()>& func
    ) {
        if (nullptr != func) {
            co_await func();
        }
    }

    virtual asio::awaitable<void> stateWriteBlock(const std::function<asio::awaitable<void>()>& func
    ) {
        if (nullptr != func) {
            co_await func();
        }
    }

    /// 延迟加载 state
    /// - 如果 thread 很多，可以等需要时从硬盘加载进内存
    virtual asio::awaitable<std::shared_ptr<T>> loadStateItem(std::string_view sessionId) {
        // TODO: 从磁盘读取
        auto ptr = std::make_shared<T>();
        util::insertOrAssignHeterogeneous(states, sessionId, ptr);
        co_return ptr;
    }

    virtual asio::awaitable<std::shared_ptr<T>> getStateItem(std::string_view sessionId) {
        {
            auto it = states.find(sessionId);
            if (it != states.end()) {
                co_return (std::static_pointer_cast<T>(it->second));
            }
        }
        co_return co_await loadStateItem(sessionId);
    }

    virtual asio::awaitable<void> saveStateItem(std::string_view sessionId, bool offload = true) {
        std::shared_ptr<agentxx::middleware::BaseMiddlewareState> oldEntity = nullptr;
        bool                                                      doSave    = false;
        if (offload) {
            {
                auto it = states.find(sessionId);
                if (it != states.end()) {
                    doSave    = true;
                    oldEntity = states.erase(it)->second;
                }
            }
        } else {
            auto it = states.find(sessionId);
            if (it != states.end()) {
                doSave    = true;
                oldEntity = it->second;
            }
        }
        if (doSave && nullptr != oldEntity) {
            // TODO: old 写入磁盘
        }
        co_return;
    }

    virtual bool containsItem(std::string_view sessionId) {
        return states.contains(sessionId);
    }
};

template<BaseMiddlewareStateType T>
class MiddlewareWrapHandle : public BaseMiddlewareHandle<T> {
public:

    onGraphNodeBeforeCallFunc onAgentcallStart;
    onGraphNodeAfterCallFunc  onAgentcallEnd;
    onGraphNodeBeforeCallFunc onModelcallStart;
    onGraphNodeBeforeCallFunc onModelcallRun;
    onGraphNodeAfterCallFunc  onModelcallEnd;
    onGraphNodeBeforeCallFunc onToolcallStart;
    onGraphNodeAfterCallFunc  onToolcallEnd;

    MiddlewareWrapHandle(
        std::string_view                            in_name,
        std::weak_ptr<agentxx::agent::AgentContext> in_agentContext,
        const onGraphNodeBeforeCallFunc&            in_onAgentcallStart = nullptr,
        const onGraphNodeAfterCallFunc&             in_onAgentcallEnd   = nullptr,
        const onGraphNodeBeforeCallFunc&            in_onModelcallStart = nullptr,
        const onGraphNodeBeforeCallFunc&            in_onModelcallRun   = nullptr,
        const onGraphNodeAfterCallFunc&             in_onModelcallEnd   = nullptr,
        const onGraphNodeBeforeCallFunc&            in_onToolcallStart  = nullptr,
        const onGraphNodeAfterCallFunc&             in_onToolcallEnd    = nullptr
    ) :
        BaseMiddlewareHandle<T>(in_name, in_agentContext),
        onAgentcallStart(in_onAgentcallStart),
        onAgentcallEnd(in_onAgentcallEnd),
        onModelcallStart(in_onModelcallStart),
        onModelcallRun(in_onModelcallRun),
        onModelcallEnd(in_onModelcallEnd),
        onToolcallStart(in_onToolcallStart),
        onToolcallEnd(in_onToolcallEnd) {}

    asio::awaitable<void> onAgentcallStartFunc(neograph::graph::NodeInput& in) override {
        if (nullptr != onAgentcallStart) {
            co_await onAgentcallStart(in);
        }
        co_return;
    }

    asio::awaitable<void> onAgentcallEndFunc(
        const neograph::graph::NodeInput& in,
        neograph::graph::NodeOutput&      result
    ) override {
        if (nullptr != onAgentcallEnd) {
            co_await onAgentcallEnd(in, result);
        }
        co_return;
    }

    asio::awaitable<void> onModelcallStartFunc(neograph::graph::NodeInput& in) override {
        if (nullptr != onModelcallStart) {
            co_await onModelcallStart(in);
        }
        co_return;
    }

    asio::awaitable<void> onModelcallRunFunc(neograph::graph::NodeInput& in) override {
        if (nullptr != onModelcallRun) {
            co_await onModelcallRun(in);
        }
        co_return;
    }

    asio::awaitable<void> onModelcallEndFunc(
        const neograph::graph::NodeInput& in,
        neograph::graph::NodeOutput&      result
    ) override {
        if (nullptr != onModelcallEnd) {
            co_await onModelcallEnd(in, result);
        }
        co_return;
    }

    asio::awaitable<void> onToolcallStartFunc(neograph::graph::NodeInput& in) override {
        if (nullptr != onToolcallStart) {
            co_await onToolcallStart(in);
        }
        co_return;
    }

    asio::awaitable<void> onToolcallEndFunc(
        const neograph::graph::NodeInput& in,
        neograph::graph::NodeOutput&      result
    ) override {
        if (nullptr != onToolcallEnd) {
            co_await onToolcallEnd(in, result);
        }
        co_return;
    }
};

class SummarizationToolHandle {
public:

    /// 根据 tool call 参数生成去重 key。
    /// 返回 std::nullopt 表示该次调用不需要去重。
    std::function<std::optional<std::string>(const neograph::json& args)> generateDeduplicationKey;

    /// 当发现重复（旧数据已被新数据覆盖）时，截断旧的 toolcall request
    std::function<void(neograph::ToolCall&)> truncateRequest;

    /// 当发现重复（旧数据已被新数据覆盖）时，截断旧的 toolcall response
    std::function<void(neograph::ChatMessage&)> truncateResponse;
};

class InterruptHandleArg {
public:

    class InterruptHandleInputItem {
    public:

        std::string label;
        std::string depict;
        /// bool / int / double / string / enum
        std::string              type;
        std::string              defaultValue;
        std::vector<std::string> enumValues;

        static InterruptHandleInputItem fromJson(const neograph::json& data);

        neograph::json toJson() const;
    };

    std::string                           name;
    neograph::json                        arg;
    std::vector<InterruptHandleInputItem> inputs;
    std::string                           resultId;

    static bool isAccordingFormat(const neograph::json& data);

    static std::optional<InterruptHandleArg> fromJson(const neograph::json& data);

    neograph::json toJson() const;

    static std::vector<InterruptHandleArg> listFromJson(const neograph::json& data);

    static neograph::json listToJson(const std::vector<InterruptHandleArg>& data);
};

class MiddlewareContext {
public:

    class SessionShareStore {
    public:

        std::map<size_t, std::string> store{};
        size_t                        storeId = 1;

        size_t getNextId() {
            storeId++;
            return storeId;
        }
    };

    inline static const std::string interruptHandleName_default = "default";
    /// graphData 需要跨 checkpoint 存储时使用该 state channel key
    inline static const std::string channel_savedGraphData{"xx_savedGraphData"};

    inline static const std::string graphDataKey_appendSystemMessage{"xx_appendSystemMessage"};
    inline static const std::string graphDataKey_messageCheckInfo{"xx_messageCheckInfo"};
    inline static const std::string graphDataKey_tempLLMThinking{"xx_ModelCallWrap_tempLLMThinking"
    };
    inline static const std::string graphDataKey_tempLLMContent{"xx_ModelCallWrap_tempLLMContent"};
    inline static const std::string graphDataKey_LLMTokenUsage{"xx_ModelCallWrap_LLMTokenUsage"};
    /// 存储 中断、异常、取消 时的 messages
    inline static const std::string graphDataKey_tempMessages{"xx_tempMessages"};
    /// LLM 压缩 (summarization) 连续失败计数
    /// - 每次 agent 轮执行清理; 同一轮内重试/多轮 modelcall 累积,
    ///   达到上限 (或超限严重) 时触发硬截断兜底
    inline static const std::string graphDataKey_summarizationFailCount{"xx_summarizationFailCount"
    };
    /// 上次成功压缩后的消息数量
    /// - 用于自动压缩冷却: 若上次压缩后消息条数增长不足且仍处于超限水位，避免反复派生 subagent
    /// 无效压缩
    inline static const std::string graphDataKey_summarizationLastMsgCount{
        "xx_summarizationLastMsgCount"
    };
    inline static const std::string graphDataKey_interruptArgs{"xx_interruptArgs"};
    inline static const std::string graphDataKey_interruptResult{"xx_interruptResult"};
    /// 中断发生的节点名 (供程序重启恢复中断时复用)
    inline static const std::string graphDataKey_interruptNode{"xx_interruptNode"};
    /// 中断携带的值 (供程序重启恢复中断时复用)
    inline static const std::string graphDataKey_interruptValue{"xx_interruptValue"};
    inline static const std::string graphDataKey_interruptToolcallCache{"xx_interruptToolcallCache"
    };

    /// <sessionId, <id, value>>
    /// - 存储变量内容，留出 id 到 上下文中，llm 需要时可以通过
    /// toolcall/agentxx_share_store 读取
    /// - 如: 压缩上下文时会将部分长文本存入这里替换为 id
    /// - 内存副本作为读缓存, 写操作同步落库 (持久化注入时)
    std::map<std::string, SessionShareStore, std::less<>> shareStore{};

    /// <sessionId, itemData>
    /// [会话独立] 每次执行的临时数据，在 [AgentStartCall] 时刷新，在
    /// [AgentEndCall] 时清理
    std::map<std::string, std::map<std::string, std::any, std::less<>>, std::less<>> graphData{};

    /// 用基类声明类型，以便支持插入不同子类
    /// - 中间的指针是必要的，直接写 std::vector<BaseMiddlewareHandleInterface>
    /// 的话元素大小是 固定为基类大小，插入子类时内存会被截断，导致后续异常
    std::vector<std::shared_ptr<BaseMiddlewareHandleInterface>> handles{};

    MiddlewareContext() = default;

    /// - [sessionStore] 会话 SQLite 持久化 (由 BaseAgent::init 注入;
    ///   为空时 share store 仅内存存储, 不落库)
    explicit MiddlewareContext(std::shared_ptr<agentxx::agent::SessionStore> sessionStore) :
        persistence_(sessionStore) {}

    /// 将 std::any 转为 neograph::json（用于序列化到 state）
    static neograph::json anyToJson(const std::any& val);

    /// 将 neograph::json 转为 T（用于从 state 恢复后按需转换）
    template<typename T>
    static T jsonToValue(const neograph::json& j) {
        if constexpr (std::is_same_v<T, neograph::json>) {
            return j;
        } else if constexpr (std::is_same_v<T, std::string>) {
            if (j.is_string()) {
                return j.get<std::string>();
            }
            if (j.is_number()) {
                return j.dump();
            }
            return {};
        } else if constexpr (std::is_same_v<T, bool>) {
            if (j.is_boolean()) {
                return j.get<bool>();
            }
            return {};
        } else if constexpr (std::is_same_v<T, int>) {
            if (j.is_number_integer()) {
                return j.get<int>();
            }
            return {};
        } else if constexpr (std::is_same_v<T, int64_t>) {
            if (j.is_number_integer()) {
                return j.get<int64_t>();
            }
            return {};
        } else if constexpr (std::is_same_v<T, size_t>) {
            if (j.is_number_integer()) {
                return j.get<size_t>();
            }
            return {};
        } else if constexpr (std::is_same_v<T, double>) {
            if (j.is_number()) {
                return j.get<double>();
            }
            return {};
        } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
            if (j.is_array()) {
                return j.get<std::vector<std::string>>();
            }
            return {};
        } else if constexpr (std::is_same_v<T, std::vector<neograph::ChatMessage>>) {
            std::vector<neograph::ChatMessage> msgs;
            if (j.is_array()) {
                for (const auto& item : j) {
                    neograph::ChatMessage msg;
                    neograph::from_json(item, msg);
                    msgs.push_back(std::move(msg));
                }
            }
            return msgs;
        } else if constexpr (std::is_same_v<T, std::vector<InterruptHandleArg>>) {
            std::vector<InterruptHandleArg> msgs;
            if (j.is_array()) {
                msgs = InterruptHandleArg::listFromJson(j);
            }
            return msgs;
        } else {
            static_assert(sizeof(T) == 0, "jsonToValue: unsupported type T");
        }
    }

    /// 确保 std::any 中的值类型为 T，支持双向自动转换:
    ///   - json → T  (从 state 恢复后按需转换回原始类型)
    ///   - 任意类型 → json  (读为 json 格式)
    template<typename T>
    static void ensureAnyType(std::any& val) {
        if (!val.has_value() || val.type() == typeid(T)) {
            return;
        }
        if (val.type() == typeid(neograph::json)) {
            auto j = std::any_cast<neograph::json>(std::move(val));
            val    = jsonToValue<T>(j);
            return;
        }
        if constexpr (std::is_same_v<T, neograph::json>) {
            val = anyToJson(val);
        }
    }

    std::optional<std::string> getShareStoreItemValue(std::string_view sessionId, const size_t id);

    void
        setShareStoreItemValue(std::string_view sessionId, const size_t id, std::string_view value);

    size_t addShareStoreItemValue(std::string_view sessionId, std::string_view value);

    void removeShareStoreItemValue(std::string_view sessionId, const size_t id);

    void removeGraphDataItem(std::string_view sessionId, std::string_view key);

    /// 清理指定 thread 的全部中间件状态 (graphData / shareStore / 各 handle states)
    /// - 供一次性会话 (subagent、headless run) 结束后的资源回收, 防止按 thread 累积泄漏
    ///   (Session 由 SessionStore::remove 另行移除)
    /// - 须由 agent io 线程调用 (与状态读写同一线程)
    void cleanupSession(std::string_view sessionId);

    template<typename T>
    T& getGraphDataItemValue(std::string_view sessionId, std::string_view key) {
        auto& itemGraphData = util::getOrCreateHeterogeneous(graphData, sessionId);
        auto  it            = itemGraphData.find(key);
        if (it == itemGraphData.end()) {
            auto [insertIt, _] = util::insertHeterogeneous(itemGraphData, std::string{key}, T{});
            it                 = insertIt;
        } else {
            ensureAnyType<T>(it->second);
        }
        return std::any_cast<T&>(it->second);
    }

    template<typename T>
    void setGraphDataItemValue(std::string_view sessionId, std::string_view key, T value) {
        auto& itemGraphData = util::getOrCreateHeterogeneous(graphData, sessionId);
        util::insertOrAssignHeterogeneous(itemGraphData, key, std::move(value));
    }

    template<typename T>
    void modifyGraphDataItemValue(
        std::string_view          sessionId,
        std::string_view          key,
        std::function<void(T&)>&& modify
    ) {
        auto& itemGraphData = util::getOrCreateHeterogeneous(graphData, sessionId);
        auto  it            = itemGraphData.find(key);
        if (it == itemGraphData.end()) {
            auto value = T{};
            modify(value);
            util::insertHeterogeneous(itemGraphData, std::string{key}, std::move(value));
        } else {
            ensureAnyType<T>(it->second);
            modify(std::any_cast<T&>((it->second)));
        }
    }

    /// 一般用于捕获到 NodeInterrupt 后重新抛出，而不能作为首次抛出使用
    void throwNodeInterruptBase(std::string_view sessionId, const neograph::json& msgs);

    /// 工具请求中断：检查已有结果（resume 后）或存储参数并抛异常
    asio::awaitable<neograph::json> requestInterrupt(
        std::string_view                           sessionId,
        const std::function<InterruptHandleArg()>& onCreateArg,
        const neograph::json&                      msgs
    );

    /// 将 graphData 中 JSON 兼容条目序列化到 state channel
    neograph::json
        getGraphDataToState(neograph::graph::GraphState& state, std::string_view sessionId);

    /// 从 state channel 恢复 graphData (用于中断 resume)
    void setGraphDataFromState(neograph::graph::GraphState& state, std::string_view sessionId);

    void setGraphDataFromState(neograph::json j, std::string_view sessionId);

private:

    /// 确保 share store 内存缓存已加载 (首次访问某 thread 时从 SQLite 恢复;
    /// 未注入持久化时 no-op)
    void ensureShareStoreLoaded(std::string_view sessionId);

    /// 会话 SQLite 持久化 (为空时 share store 仅内存存储)
    std::shared_ptr<agentxx::agent::SessionStore> persistence_ = nullptr;
    /// 已从持久化加载过 share store 的 thread (避免空存储反复加载) - O(1)
    std::unordered_set<std::string> shareStoreLoaded_{};
};

} // namespace middleware
} // namespace agentxx