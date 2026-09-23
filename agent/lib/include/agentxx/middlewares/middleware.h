#pragma once

#include "agentxx/agent/context.h"
#include "agentxx/middlewares/interrupt_ui.h"
#include "agentxx/util/neograph_json_bridge.h"
#include "asio/io_context.hpp"
#include "fmt/format.h"
#include "utilxx_base/container_util.h"
#include "utilxx_base/log.h"
#include "utilxx_base/lru_cache.h"
#include <any>
#include <cstdlib>
#include <functional>
#include <memory>
#include <neograph/llm/rate_limited_provider.h>
#include <neograph/llm/schema_provider.h>
#include <neograph/neograph.h>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
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

    static utilxx_base::Json getLastMessageJson(const neograph::graph::NodeInput& in);

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
        utilxx_base::insertOrAssignHeterogeneous(states, sessionId, ptr);
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

/// 中间件生命周期拦截钩子集合
/// - 采用聚合体设计, 未指定的阶段天然默认为 nullptr
/// - 支持 C++20 指定初始化器 (Designated Initializers)
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
        MiddlewareHooks                             in_hooks = {}
    ) :
        BaseMiddlewareHandle<T>(in_name, in_agentContext),
        onAgentcallStart(std::move(in_hooks.onAgentcallStart)),
        onAgentcallEnd(std::move(in_hooks.onAgentcallEnd)),
        onModelcallStart(std::move(in_hooks.onModelcallStart)),
        onModelcallRun(std::move(in_hooks.onModelcallRun)),
        onModelcallEnd(std::move(in_hooks.onModelcallEnd)),
        onToolcallStart(std::move(in_hooks.onToolcallStart)),
        onToolcallEnd(std::move(in_hooks.onToolcallEnd)) {}

    MiddlewareWrapHandle(
        std::string_view                            in_name,
        std::weak_ptr<agentxx::agent::AgentContext> in_agentContext,
        const onGraphNodeBeforeCallFunc&            in_onAgentcallStart,
        const onGraphNodeAfterCallFunc&             in_onAgentcallEnd,
        const onGraphNodeBeforeCallFunc&            in_onModelcallStart,
        const onGraphNodeBeforeCallFunc&            in_onModelcallRun,
        const onGraphNodeAfterCallFunc&             in_onModelcallEnd,
        const onGraphNodeBeforeCallFunc&            in_onToolcallStart,
        const onGraphNodeAfterCallFunc&             in_onToolcallEnd
    ) :
        MiddlewareWrapHandle(
            in_name,
            in_agentContext,
            MiddlewareHooks{
                .onAgentcallStart = in_onAgentcallStart,
                .onAgentcallEnd   = in_onAgentcallEnd,
                .onModelcallStart = in_onModelcallStart,
                .onModelcallRun   = in_onModelcallRun,
                .onModelcallEnd   = in_onModelcallEnd,
                .onToolcallStart  = in_onToolcallStart,
                .onToolcallEnd    = in_onToolcallEnd,
            }
        ) {}

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
    std::function<std::optional<std::string>(const utilxx_base::Json& args)>
        generateDeduplicationKey;

    /// 当发现重复（旧数据已被新数据覆盖）时，截断旧的 toolcall request
    std::function<void(neograph::ToolCall&)> truncateRequest;

    /// 当发现重复（旧数据已被新数据覆盖）时，截断旧的 toolcall response
    std::function<void(neograph::ChatMessage&)> truncateResponse;
};

class InterruptHandleArg {
public:

    std::string       name;
    utilxx_base::Json arg;
    std::string       resultId;
    /// 中断 UI 描述 (声明式; 客户端通用渲染, 见 [interrupt_ui.h])
    ///
    /// - 走客户端 HIL 的中断**必填**: 生产者用 `preset::*` 预设模板生成
    ///   ([interrupt_presets.h], 如 `preset::inputForm` / `preset::permissionCard`)
    ///   或自行组装 [InterruptUi::blocks] (内容块 + 控件块)
    /// - 经总线由宿主处理、不进入客户端渲染路径的中断 (如 subagent 委派) 可为空
    /// - 一条中断请求 = 一份表单 (客户端渲染为一条消息, 可含多个控件),
    ///   用户一次提交全部值
    /// - 结果契约 (客户端 → agent): `{"values": {"<控件 id>": 值}}`
    ///   (见 [makeInterruptResult]; 空对象 = 未应答/取消)
    InterruptUi ui;

    static bool isAccordingFormat(const utilxx_base::Json& data);

    static std::optional<InterruptHandleArg> fromJson(const utilxx_base::Json& data);

    utilxx_base::Json toJson() const;

    static std::vector<InterruptHandleArg> listFromJson(const utilxx_base::Json& data);

    static utilxx_base::Json listToJson(const std::vector<InterruptHandleArg>& data);
};

class MiddlewareContext {
public:

    /// share store 会话内存状态
    /// - 内容只在 SQLite (session.db 的 store 表) 中保存一份, 内存仅保留
    ///   最近使用的少数条目 ([cache]), 取值未命中时按 id 回库读取
    ///   (内存占用与会话的条目数量无关)
    /// - 未注入持久化时没有可回读的库, 内存就是唯一副本, 此时条目不能淘汰,
    ///   全部保存在 [items] 中
    class SessionShareStore {
    public:

        /// 最近使用条目的条数上限 (内存缓存的容量)
        static constexpr size_t kCacheCapacity = 3;

        /// 最近使用的条目: id -> value (仅在注入了持久化时使用)
        utilxx_base::LruCache<size_t, std::string> cache{kCacheCapacity};

        /// 全部条目: id -> value (仅在未注入持久化时使用)
        std::map<size_t, std::string> items{};

        /// 已分配 id 的最大值 (自增 id): id 超过它表示该 id 从未分配过, 属于非法参数
        /// - 首次访问会话时从库中取 max(id) 恢复, 之后每分配一个 id 就更新
        size_t lastId = 0;
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
    /// - 用于自动压缩冷却: 若上次压缩后消息条数增长不足且当前仍超过上限, 避免反复派生
    /// subagent 做无效压缩
    inline static const std::string graphDataKey_summarizationLastMsgCount{
        "xx_summarizationLastMsgCount"
    };
    /// 本次压缩挂起中的提示消息 id ("Summarizing LLM Context..." 的 viewMessage id)
    /// - 自动压缩经 NodeInterrupt 派生压缩子代理, resume 后压缩中间件从头重新执行,
    ///   需据该 id 复用首次创建的提示消息 (更新而非再追加), 避免每次压缩遗留重复提示
    /// - 压缩完成 (摘要结果写回提示消息) 时清除
    inline static const std::string graphDataKey_summarizationTipMsgId{"xx_summarizationTipMsgId"};
    inline static const std::string graphDataKey_interruptArgs{"xx_interruptArgs"};
    inline static const std::string graphDataKey_interruptResult{"xx_interruptResult"};
    /// 中断发生的节点名 (供程序重启恢复中断时复用)
    inline static const std::string graphDataKey_interruptNode{"xx_interruptNode"};
    /// 中断携带的值 (供程序重启恢复中断时复用)
    inline static const std::string graphDataKey_interruptValue{"xx_interruptValue"};
    inline static const std::string graphDataKey_interruptToolcallCache{"xx_interruptToolcallCache"
    };

    /// <sessionId, SessionShareStore>
    /// - 存储变量内容，留出 id 到 上下文中，llm 需要时可以通过
    /// toolcall/agentxx_share_store 读取
    /// - 如: 压缩上下文时会将部分长文本存入这里替换为 id
    /// - 内容本体持久化在会话 SQLite 的 store 表中, 内存只保留每个会话最近使用的
    ///   [SessionShareStore::kCacheCapacity] 条; 未注入持久化时内存里是全部数据
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

    /// 将 std::any 转为 utilxx_base::Json（用于序列化到 state）
    static utilxx_base::Json anyToJson(const std::any& val);

    /// 将 utilxx_base::Json 转为 T（用于从 state 恢复后按需转换）
    template<typename T>
    static T jsonToValue(const utilxx_base::Json& j) {
        if constexpr (std::is_same_v<T, utilxx_base::Json>) {
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
                    // item 为 utilxx_base::Json: 经桥接转回 neograph::json 再反序列化
                    // (middleware.h 不直引 bridge 头, 此处经 dump/parse 文本中转,
                    //  graphData 恢复为低频路径, 开销可忽略)
                    neograph::from_json(neograph::json::parse(item.dump()), msg);
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
        if (val.type() == typeid(utilxx_base::Json)) {
            auto j = std::any_cast<utilxx_base::Json>(std::move(val));
            val    = jsonToValue<T>(j);
            return;
        }
        // 兼容: 历史存入的 neograph::json 先桥接为业务 Json 再转换
        if (val.type() == typeid(neograph::json)) {
            auto j = agentxx::util::fromNeographJson(std::any_cast<neograph::json>(std::move(val)));
            if constexpr (std::is_same_v<T, utilxx_base::Json>) {
                val = std::move(j);
            } else {
                val = jsonToValue<T>(j);
            }
            return;
        }
        if constexpr (std::is_same_v<T, utilxx_base::Json>) {
            val = anyToJson(val);
        }
    }

    /// 读取 share store 条目 (id 必须已被分配, 见下)
    /// - `id > SessionShareStore::lastId` (从未分配过的 id) 抛 std::invalid_argument;
    ///   条目不存在 (如曾被显式 set 高 id 的旧数据) 返回 nullopt
    /// - 注入持久化时先查内存缓存, 未命中时回库读取并填入缓存
    std::optional<std::string> getShareStoreItemValue(std::string_view sessionId, const size_t id);

    /// 覆盖已有 id 的内容 (id 必须已被分配, 否则抛 std::invalid_argument)
    /// - 写操作同步落库 (注入持久化时), 并更新内存副本
    void
        setShareStoreItemValue(std::string_view sessionId, const size_t id, std::string_view value);

    /// 追加新条目, 返回分配的新 id (从 1 开始递增)
    /// - 同步落库 (注入持久化时) 并写入内存副本; 落库失败或无持久化时按内存计数递增
    size_t addShareStoreItemValue(std::string_view sessionId, std::string_view value);

    void removeGraphDataItem(std::string_view sessionId, std::string_view key);

    /// 清理指定 thread 的全部中间件状态 (graphData / shareStore / 各 handle states)
    /// - 供一次性会话 (subagent、headless run) 结束后的资源回收, 防止按 thread 累积泄漏
    ///   (Session 由 SessionStore::remove 另行移除)
    /// - 须由 agent io 线程调用 (与状态读写同一线程)
    void cleanupSession(std::string_view sessionId);

    template<typename T>
    T& getGraphDataItemValue(std::string_view sessionId, std::string_view key) {
        auto& itemGraphData = utilxx_base::getOrCreateHeterogeneous(graphData, sessionId);
        auto  it            = itemGraphData.find(key);
        if (it == itemGraphData.end()) {
            auto [insertIt, _]
                = utilxx_base::insertHeterogeneous(itemGraphData, std::string{key}, T{});
            it = insertIt;
        } else {
            ensureAnyType<T>(it->second);
        }
        return std::any_cast<T&>(it->second);
    }

    template<typename T>
    void setGraphDataItemValue(std::string_view sessionId, std::string_view key, T value) {
        auto& itemGraphData = utilxx_base::getOrCreateHeterogeneous(graphData, sessionId);
        utilxx_base::insertOrAssignHeterogeneous(itemGraphData, key, std::move(value));
    }

    template<typename T>
    void modifyGraphDataItemValue(
        std::string_view          sessionId,
        std::string_view          key,
        std::function<void(T&)>&& modify
    ) {
        auto& itemGraphData = utilxx_base::getOrCreateHeterogeneous(graphData, sessionId);
        auto  it            = itemGraphData.find(key);
        if (it == itemGraphData.end()) {
            auto value = T{};
            modify(value);
            utilxx_base::insertHeterogeneous(itemGraphData, std::string{key}, std::move(value));
        } else {
            ensureAnyType<T>(it->second);
            modify(std::any_cast<T&>((it->second)));
        }
    }

    /// 一般用于捕获到 NodeInterrupt 后重新抛出，而不能作为首次抛出使用
    void throwNodeInterruptBase(std::string_view sessionId, const utilxx_base::Json& msgs);

    /// 工具请求中断：检查已有结果（resume 后）或存储参数并抛异常
    asio::awaitable<utilxx_base::Json> requestInterrupt(
        std::string_view                           sessionId,
        const std::function<InterruptHandleArg()>& onCreateArg,
        const utilxx_base::Json&                   msgs
    );

    /// 将 graphData 中 JSON 兼容条目序列化到 state channel
    neograph::json
        getGraphDataToState(neograph::graph::GraphState& state, std::string_view sessionId);

    /// 从 state channel 恢复 graphData (用于中断 resume)
    void setGraphDataFromState(neograph::graph::GraphState& state, std::string_view sessionId);

    void setGraphDataFromState(utilxx_base::Json j, std::string_view sessionId);

private:

    /// 取 (必要时创建) 指定会话的 share store 内存状态
    /// - 首次访问该会话时只从库中取回自增 id 计数 (max(id)), 不读内容
    SessionShareStore& shareStoreState(std::string_view sessionId);

    /// 校验 id: 0 或大于 [SessionShareStore::lastId] 表示该 id 从未分配过,
    /// 抛 std::invalid_argument (调用方工具按工具错误上报)
    static void checkShareStoreId(
        const SessionShareStore& state,
        std::string_view         sessionId,
        const size_t             id
    );

    /// 缓存未命中时按 id 回库读取并填入缓存 (缓存已有/未注入持久化/条目不存在时不动)
    void ensureShareStoreItemCached(
        SessionShareStore& state,
        std::string_view   sessionId,
        const size_t       id
    );

    /// 会话 SQLite 持久化 (为空时 share store 仅内存存储)
    std::shared_ptr<agentxx::agent::SessionStore> persistence_ = nullptr;
};

} // namespace middleware
} // namespace agentxx