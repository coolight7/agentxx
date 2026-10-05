#pragma once

#include "agentxx/nodes/wrap_handle.h"
#include "utilxx_base/json.h"
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace nodes {

class NEOGRAPH_API ToolcallWrapNode : public WrapHandleBaseNode<neograph::graph::ToolDispatchNode> {
protected:
public:

    inline static constexpr auto defNodeType = std::string_view{"xx_Toolcall"};

    ToolcallWrapNode(
        std::string_view                            in_name,
        const neograph::graph::NodeContext&         in_ctx,
        std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
    );

    void onHandleStartError(
        bool                                                errorRethrow,
        bool                                                isCurrentError,
        std::string_view                                    exceptionStr,
        agentxx::middleware::BaseMiddlewareHandleInterface& item,
        neograph::graph::NodeInput&                         in,
        neograph::graph::NodeOutput&                        result
    ) noexcept override;

    void onHandleBaseRunError(
        bool                         errorRethrow,
        bool                         isCurrentError,
        std::string_view             exceptionStr,
        neograph::graph::NodeInput&  in,
        neograph::graph::NodeOutput& result
    ) noexcept override;

    asio::awaitable<void> onHandleStart(
        agentxx::middleware::BaseMiddlewareHandleInterface& item,
        neograph::graph::NodeInput&                         in
    ) override;

    asio::awaitable<void> onHandleEnd(
        agentxx::middleware::BaseMiddlewareHandleInterface& item,
        const neograph::graph::NodeInput&                   in,
        neograph::graph::NodeOutput&                        result
    ) override;

    /// 一次工具调用的执行阶段划分 (prepare → run → finalize)
    /// - prepare 与 finalize 按声明顺序串行; 只有 run 阶段可以被并发执行,
    ///   因此并发安全 (只读) 的工具必须在 [supportsParallel] 上声明
    /// - 结构在单轮 toolcall 的协程内使用, 不跨轮次保存
    struct PreparedToolCall {
        /// 声明的 tool 调用 (指向本轮协程内拷出的声明列表, 生命周期覆盖整批执行)
        const neograph::ToolCall* tc = nullptr;
        /// 执行体 (静态注册表工具或动态插件工具)
        neograph::Tool* tool = nullptr;
        /// 动态插件工具保活 (静态工具为空; 插件卸载等待 inflight 归零)
        std::shared_ptr<neograph::Tool> keepAlive{};
        /// 注入 sessionId / tool_call_id 后的参数
        utilxx_base::Json args{};
        /// 重复调用标识 key 与是否命中循环检测
        std::string repeatKey{};
        bool        repeatHit = false;
        /// 无需执行执行体时的最终内容 (命中中断缓存 / 工具不存在 / 权限拒绝 /
        /// 重复调用被用户拒绝); 有值即直接作为结果
        std::optional<std::string> shortCircuit{};
        /// 开始时间 (毫秒时间戳; 结果消息的 startTimeMs)
        int64_t startMs = 0;
        /// 是否允许与其他并行安全工具并发 (执行体显式声明)
        bool parallelSafe = false;
        /// 本轮取消令牌 (会话级): 执行前检查, 取消后不再启动新的执行体
        std::shared_ptr<neograph::graph::CancelToken> cancelToken{};
        /// 本调用是否在 prepare 阶段触发了中断 (重复调用确认): 结果为
        /// `[Interrupt]` 占位并带 Interrupt 标记, 交给控制流处理
        bool interrupted = false;

        /// 本调用是否有可执行的执行体 (false = shortCircuit 直接给结果)
        bool needRun() const {
            return !shortCircuit.has_value();
        }
    };

    /// prepare 阶段: 工具查找 (静态表 → 动态插件注册表) + 参数注入与类型修正 +
    /// 权限检查 + 连续重复调用确认
    /// - 全程串行执行, 保持与模型声明相同的顺序 (权限询问与 HIL 顺序稳定)
    /// - `return` 是否需要执行执行体 (false 时 [PreparedToolCall::shortCircuit] 为结果)
    /// - 取消 (CancelledException) 与中断 (NodeInterrupt) 照常向外传播
    asio::awaitable<bool> prepareToolCall(
        const neograph::ToolCall&                            tc,
        std::string_view                                     sessionId,
        const std::shared_ptr<neograph::graph::CancelToken>& cancelToken,
        const std::set<std::string>&                         repeatTriggeredKeys,
        const std::map<std::string, std::string>&            toolcallsCache,
        PreparedToolCall&                                    out
    ) const;

    /// run 阶段: 执行执行体 (含按 [XXToolBase::maxRetry] 的重试), 返回**原始**结果文本
    /// - 只做执行, 不写会话、不裁剪结果: 并发安全与否只取决于执行体本身
    /// - 取消/中断/致命异常按原语义抛出
    asio::awaitable<std::string> runToolCallBody(const PreparedToolCall& prepared) const;

    /// finalize 阶段: 结果定稿 (超出限制时经 share_store 卸载并给出定位提示)
    asio::awaitable<std::string> finalizeToolCall(
        const PreparedToolCall& prepared,
        std::string             rawResult
    ) const;

    /// 执行单个 tool
    /// - [cancelToken] 当前轮次取消令牌: 传递给 ContextualAsyncTool 以便 tool
    ///   轮询取消或传播到其传输层; 可为 nullptr (无取消支持)
    /// - [repeatCallTriggered] 当前调用是否触发连续重复阈值 (由 baseRun 经
    ///   findConsecutiveRepeatCallKeys 基于 messages 的 llm <-> tool 交替链检测);
    ///   为 true 且该 tool 启用了 [agentxx::tools::XXToolBase::repeatCallCheck]
    ///   时, 经 permission 总线发起询问警告用户, 用户确认后才继续执行
    /// - [repeatCallKey] 当前调用的重复标识 key (见 makeRepeatCallKey),
    ///   用于询问提示中向用户展示
    /// - 本函数是"单次调用"的完整路径 (prepare → run → finalize), 顺序执行时使用;
    ///   并发批次的各阶段由 [prepareToolCall] / [runToolCallBody] / [finalizeToolCall]
    ///   直接调度
    asio::awaitable<std::string> execTool(
        neograph::Tool*                                      tool,
        utilxx_base::Json&                                   args,
        const std::shared_ptr<neograph::graph::CancelToken>& cancelToken,
        bool                                                 repeatCallTriggered = false,
        std::string_view                                     repeatCallKey       = {}
    ) const;

    /// 并发执行一批已准备好的工具调用 (batch 内全部为并行安全调用)
    /// - 结果按 batch 内顺序写入 `outMsgs` 对应槽位 (并发完成顺序 ≠ 提交顺序)
    /// - 工具触发的中断 (NodeInterrupt) 记为 `[Interrupt]` 结果并置 `outInterrupted`
    /// - 取消 (CancelledException) 与其他异常记入 `outError` 并按批次停止新调用:
    ///   未启动/未完成的槽位保持空, 由调用方补取消占位
    asio::awaitable<void> runToolBatch(
        const std::vector<PreparedToolCall>&              batch,
        std::vector<std::optional<neograph::ChatMessage>>& outMsgs,
        std::exception_ptr&                               outError,
        bool&                                             outInterrupted
    ) const;

    /// 执行单个已准备的调用并组装结果消息 (run + finalize + 结果消息封装)
    /// - 无需执行 (shortCircuit) 时直接给出结果消息
    /// - 取消: `*outError` 记录异常并返回 nullopt (调用方补占位)
    /// - 中断: `*outInterrupted` 置位并返回 `[Interrupt]` 结果消息
    /// - 普通异常: 返回 `[Exception aborted: ...]` 结果消息 (与其他工具调用无关)
    /// - 执行体由调用方复制进 batch (协程捕获), 生命周期覆盖整批执行
    asio::awaitable<std::optional<neograph::ChatMessage>> runPreparedToolCall(
        const PreparedToolCall& prepared,
        std::exception_ptr*     outError,
        bool*                   outInterrupted
    ) const;

    /// 执行单条已准备的调用并把结果写入槽位, 所有异常在此收口 (不向外抛)
    /// - 取消 (含 asio 取消导致的 operation_aborted): 记为 `outError`, 槽位保持空
    /// - 中断 (NodeInterrupt): 置 `outInterrupted`, 槽位写 `[Interrupt]` 结果
    /// - 其余异常: 兜底按取消处理 (普通工具错误已在 [runPreparedToolCall] 内转为结果文本)
    /// - 派生协程调用它时不得让异常逃逸 (detached 处理器的未捕获异常会终止进程)
    asio::awaitable<void> runPreparedToolCallGuarded(
        const PreparedToolCall&                prepared,
        std::optional<neograph::ChatMessage>&  outMsg,
        std::exception_ptr&                    outError,
        bool&                                  outInterrupted
    ) const;

    /// 执行体是否声明并行安全 (未声明 = 独占)
    static bool isParallelSafeTool(const neograph::Tool& tool);

    /// 并行安全调用的并发上限 (AgentConfig::toolParallelMaxConcurrency, 夹到 [1, 32])
    size_t toolParallelLimit() const;


    /// 计算重复调用标识 key: `{toolName}_{arg字符串长度}_{arg哈希值}`
    /// - 相同 tool + 相同参数 (arguments 原始 JSON 字符串) 得到相同 key;
    ///   长度参与拼接可降低哈希碰撞被误判为相同调用的概率
    static std::string makeRepeatCallKey(std::string_view toolName, std::string_view arguments);

    /// 检测 messages 中以 [assistantMsg] 结尾的连续 llm <-> tool 交替链内的循环调用,
    /// 返回达到 [threshold] 次连续相同调用 (key 口径见 makeRepeatCallKey) 的 key 集合
    /// (通常为空; 同轮并行调用可能多于一个)
    /// - 连续链: 从 assistantMsg 起向前仅允许出现 assistant(带 tool_calls) 与
    ///   tool 结果消息, 二者交替; 遇到 user/system 等其他角色消息即视为断开;
    ///   不带 tool_calls 的 assistant (最终文本回复) 同样视为断开
    /// - 性能 (提前终止, 回溯有界):
    ///   - 从 assistantMsg 向前最多回溯 threshold 条 assistant 消息: 若某 key 真的
    ///     连续出现达 threshold 次, 最近 threshold 条 assistant 每条必含该 key,
    ///     更早的消息不可能再补足缺口
    ///   - 某条 (非当前轮) assistant 的所有 tool_call 生成的 key 均不在已统计
    ///     集合中时, 说明该条开启了全新调用, 重复计数至多延续到这里, 终止回溯
    ///   - 任一 key 计数达到 threshold 时立即返回 (已确定存在循环调用)
    static std::set<std::string> findConsecutiveRepeatCallKeys(
        const std::vector<neograph::ChatMessage>& messages,
        size_t                                    assistantMsgIndex,
        size_t                                    threshold
    );

    /// 根据 tool 的参数 JSON Schema 自动修正参数类型兼容性, 尽量让 arg 类型匹配参数需求:
    /// - string -> 字符串数组: 参数声明为数组 (字符串数组) 而传入单个字符串时, 包装为 `[str]`
    /// - string -> number/integer: 参数声明为数值而传入字符串时, 若字符串可完整解析为数值则转换
    ///   (integer 仅接受整数写法; number 支持小数/指数; 前导 '+', 首尾空白会被容忍)
    /// - number/integer -> string: 参数声明为字符串而传入数值时, 转为十进制字符串
    /// - bool -> string / string("true"/"false") -> boolean: 布尔与字符串互相转换
    /// - [单字符串数组] -> string: 参数声明为字符串而传入单元素字符串数组时, 解包为字符串
    /// - 仅当目标类型不包含 arg 当前类型时转换; 无法解析或类型不明确时保持原样
    /// - `return` 是否发生了参数转换
    static bool autoFixArgsType(const neograph::ChatTool& def, utilxx_base::Json& args);

    asio::awaitable<void> baseRun(
        std::vector<std::shared_ptr<agentxx::middleware::BaseMiddlewareHandleInterface>>& handles,
        neograph::graph::NodeInput&                                                       in,
        neograph::graph::NodeOutput&                                                      out
    ) override;

    /// 打印本次 toolcall 的调用参数 (调试日志; 上下文取自会话, 需要调用方传入
    /// agent 上下文; 未传时仅打印空参数列表)
    static void defStdoutLogOnToolcallStart(
        neograph::graph::NodeInput&                          in,
        size_t                                               limitOutput = 0,
        const std::shared_ptr<agentxx::agent::AgentContext>& ctx         = nullptr
    );

    static void defStdoutLogOnToolcallEnd(
        const neograph::graph::NodeInput& in,
        neograph::graph::NodeOutput&      result,
        size_t                            limitOutput = 0
    );
};
} // namespace nodes
} // namespace agentxx
