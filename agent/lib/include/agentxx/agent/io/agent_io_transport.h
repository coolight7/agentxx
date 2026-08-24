#pragma once

#include "agentxx/agent/conversation_types.h"
#include "asio/awaitable.hpp"
#include "neograph/json.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace agentxx {
namespace agent {

// ---------------------------------------------------------------------------
// WireMessage: 两个 AgentIOBase 端点之间传递的结构化消息
// - Channel 传输: 直接传递 C++ 对象 (免序列化)
// - WS 传输: 内部负责 JSON 编解码, 调用方无感知
// ---------------------------------------------------------------------------

struct WireHello {
    std::string sessionId;
    std::string token;
    uint64_t    lastSeq = 0;
    std::string tailHash;
    std::string model;
};

struct WireHelloAck {
    bool        ok = false;
    std::string sessionId;
    std::string tailHash;
    std::vector<std::string> models;
    /// 服务端已加载的 agent 侧插件名列表 (供 client 插件判断对端可用性;
    /// 旧版服务端不携带该字段 → 反序列化为空数组, 客户端按"未知"处理)
    std::vector<std::string> plugins;
};

struct WireUserInput {
    std::string sessionId;
    std::string text;
    /// 本条消息携带的模型选择 (空 = 不切换): TUI 切模型不再即时发送
    /// WireSelectModel, 而是随下一次用户消息携带, BaseAgent 执行该轮会话
    /// 开始时 (runTurnAsync 内 selectModel) 自动切换
    std::string model;
};

struct WireCancel {
    std::string sessionId;
};

struct WireSelectModel {
    std::string sessionId;
    std::string model;
};

struct WireInterruptRequest {
    int64_t     id = 0;
    std::string sessionId;
    std::string node;
    std::string value;
    std::string argJson;
};

struct WireInterruptResponse {
    int64_t        id = 0;
    neograph::json result;
};

/// 服务端通知中断已过期 (超时/会话取消) (Server -> Client)
/// - id 对应 WireInterruptRequest.id; 客户端应将对应未操作的中断消息标记为过期
struct WireInterruptExpired {
    int64_t     id = 0;
    std::string sessionId;
};

struct WireTurnResult {
    std::string sessionId;
    bool        hasError = false;
    std::string errorMessage;
    bool        interrupted = false;
    int64_t     startTimeMs = 0; // 轮次开始时间戳 (毫秒)
    int64_t     durationMs  = 0; // 运行时长 (毫秒)
};

struct WireContextStats {
    uint64_t contextTokens    = 0;
    uint64_t maxContextTokens = 0;
    /// 当前 ModelCall 平均生成速度 (token/s, 估算值); 0 = 无流式/无数据
    double tps = 0.0;
};

struct WireError {
    int         code = 0;
    std::string message;
};

/// 服务端日志转发 (Server -> Client)
struct WireLog {
    int         level = 0;
    std::string message;
};

/// 客户端请求当前模型信息 (Client -> Server)
struct WireGetModel {
    std::string sessionId;
};

/// 服务端模型信息响应 (Server -> Client)
struct WireModelInfo {
    std::string              currentModel;
    std::vector<std::string> models;
};

/// 客户端请求会话启动信息 (Client -> Server): 拉取已加载的 MCP/Skill/Memory 列表
struct WireGetAppendComponentInfo {
    std::string sessionId;
};

/// 服务端加载组件响应 (Server -> Client): collectAppendComponentInfo 收集的结果
struct WireAppendComponentInfo {
    std::vector<AppendComponentNotification> notifications;
};

/// 客户端请求当前会话 LLM 上下文消息 (Client -> Server)
struct WireGetContext {
    std::string sessionId;
};

/// 服务端 LLM 上下文消息响应 (Server -> Client)
struct WireContextMessages {
    neograph::json messages;
};

/// 客户端请求持久化会话列表 (Client -> Server): 会话选择弹窗数据源
/// - 不携带 sessionId: 列举全部持久化会话, 与当前连接会话无关
/// - 支持分页 (keyset 游标): 客户端先请求最新一页, 浏览到末尾时按游标续取,
///   避免会话很多时一次性扫描/传输/渲染全量; limit == 0 为旧行为 (全量),
///   旧客户端默认构造的空请求即走该路径, 向后兼容
struct WireListSessions {
    /// 游标: 仅返回排序位于该时间点之后的会话 (毫秒时间戳); <= 0 = 从最新开始
    int64_t beforeMs = 0;
    /// 游标平局裁决: 与 beforeMs 相同时间戳的会话按 sessionId 升序排列,
    /// 游标取"上一页最后一条"的 (lastActiveMs, sessionId)
    std::string beforeId;
    /// 页大小; 0 = 全量列举 (旧行为)
    uint32_t limit = 0;
};

/// 服务端持久化会话列表响应 (Server -> Client)
/// - sessions 按最近活动时间降序排列 (最新在前); 分页响应仅含一页
struct WireSessionList {
    std::vector<SessionInfo> sessions;
    /// 持久化会话总数 (供客户端展示 x/y 与判断加载完成); 旧版服务端无此字段 → 0
    uint64_t totalCount = 0;
    /// 是否还有未加载的更早会话; 旧版服务端无此字段 → false (视为全量响应)
    bool hasMore = false;
};

/// 客户端请求切换当前连接的会话 (Client -> Server): 将会话端点重新绑定到
/// 目标 sessionId, 服务端加载其历史并回推 Sync/模型/上下文统计 (见
/// SessionServerAgentIO::switchSession)
struct WireSwitchSession {
    std::string sessionId;
};

/// 客户端记住权限选择 (Client -> Server): 将路径规则注册到服务端权限中间件,
/// 后续访问该路径或其子目录时按规则直接允许/拒绝, 不再询问
struct WireSetPermission {
    std::string sessionId;
    /// 标准化绝对路径 (规则作用于该路径及其子目录, 最长前缀匹配)
    std::string path;
    /// true = 允许 (PermissionOperator::ALLOW), false = 拒绝 (PermissionOperator::DENY)
    bool allow = true;
    /// 规则作用域: FilesystemPermissionREAD(0) / FilesystemPermissionWRITE(1)
    size_t index = 0;
};

/// 插件事件转发 (Server -> Client)
/// - 插件经事件总线发布 (topic 约定 `{插件名}.{事件名}`) 的事件原样转发,
///   宿主不解析载荷语义; 频率由插件自身控制
/// - 客户端据此判断插件可用性并展示 (如 agentxx_codegraph 索引进度、
///   agentxx_system_monitor 周期采集的 usage 事件)
struct WirePluginData {
    /// 插件名 (如 "agentxx_codegraph")
    std::string plugin;
    /// 事件名 (如 "progress" / "status")
    std::string event;
    /// JSON 载荷字符串 (语义由插件定义; 如 {"processed","total","current_file"})
    std::string data;
};

/// client 插件事件上行 (Client -> Server)
/// - client 侧插件 (agentxx_client_entry) 经 send_plugin_data 发出的跨端事件;
///   服务端收到后发布到事件总线 topic `client.{插件名}.{事件名}` (载荷 std::string),
///   由 agent 侧同名插件订阅消费
/// - 宿主不解析载荷语义; 频率由插件自身控制 (与 WirePluginData 对称)
struct WirePluginDataUp {
    /// 发送方插件名 (client 侧实例名, 与 agent 侧同名插件对应)
    std::string plugin;
    /// 事件名 (如 "rebuild_request")
    std::string event;
    /// JSON 载荷字符串 (语义由插件定义)
    std::string data;
};

/// 服务端消息队列同步 (Server -> Client)
struct WireMessageQueueUpdate {
    std::string                   sessionId;
    std::vector<MessageQueueItem> items;
};

/// 客户端请求清空消息队列 (Client -> Server)
struct WireClearMessageQueue {
    std::string sessionId;
};

/// 客户端请求删除单条排队消息 (Client -> Server)
struct WireRemoveQueueItem {
    std::string sessionId;
    std::string itemId;
};

/// 客户端请求打断当前会话执行并立即运行消息队列首条 (Client -> Server)
struct WireInterruptAndRunNext {
    std::string sessionId;
};

/// 客户端请求 viewMessages 历史分页 (Client -> Server)
///
/// 背景: 长会话恢复时服务端仅同步末尾窗口 (SessionServerAgentIO::Config
/// ::initialSyncTailCount), 客户端 (TUI) 用户向上滚动到窗口顶部时经本消息
/// 分页拉取更早历史, 服务端以 WireViewMessagesPage 回应。
/// - 语义: 请求绝对下标区间 [max(0, beforeIndex - count), beforeIndex) 的消息
///   (viewMessages 为 append-only, 绝对下标恒定, 无竞态)
/// - beforeIndex == 0 视为 "从末尾向前取 count 条" (客户端首次加载兜底;
///   正常分页流程中窗口顶部为 0 时已无更早消息, 客户端不应再请求)
struct WireGetViewMessages {
    std::string sessionId;
    /// 请求该绝对下标之前的消息 (exclusive 上界); 0 = 从末尾向前取
    uint64_t beforeIndex = 0;
    /// 请求条数; 0 = 服务端使用默认页大小
    uint32_t count = 0;
};

/// 服务端 viewMessages 历史分页响应 (Server -> Client)
/// - 携带绝对下标区间 [startIndex, startIndex + messages.size()) 的消息,
///   客户端前插到本地已加载窗口上方并按 (startIndex 差值) 做滚动锚定
struct WireViewMessagesPage {
    std::string sessionId;
    /// 本页首条消息在服务端完整 viewMessages 中的绝对下标
    uint64_t startIndex = 0;
    /// 服务端会话总消息数 (供客户端判断 hasMore: startIndex > 0 即还有更早消息)
    uint64_t                 totalCount = 0;
    std::vector<ViewMessage> messages;
};

/// 所有可能的线消息类型 (tagged variant)
using WireMessage = std::variant<
    WireHello,
    WireHelloAck,
    WireUserInput,
    WireCancel,
    WireSelectModel,
    WireInterruptRequest,
    WireInterruptResponse,
    WireInterruptExpired,
    Delta,
    SyncPayload,
    WireTurnResult,
    WireContextStats,
    WireError,
    WireLog,
    WireGetModel,
    WireModelInfo,
    WireGetAppendComponentInfo,
    WireAppendComponentInfo,
    WireGetContext,
    WireContextMessages,
    WireListSessions,
    WireSessionList,
    WireSwitchSession,
    WireSetPermission,
    WirePluginData,
    WirePluginDataUp,
    WireMessageQueueUpdate,
    WireClearMessageQueue,
    WireRemoveQueueItem,
    WireInterruptAndRunNext,
    WireGetViewMessages,
    WireViewMessagesPage>;

// ---------------------------------------------------------------------------
// AgentIOTransportBase: 两个 AgentIOBase 端点之间的协议传输层
//
// 设计原则:
// - 对调用方隐藏传输细节 (编解码/重连/心跳/序列化)
// - Channel 实现: 进程内线程间, 直接传递 WireMessage 对象, 无序列化开销
// - WS 实现: 内部处理 JSON 编解码、hello 握手、心跳、断线重连
// - 语义: 至多一个 outstanding recv + 一个 outstanding send
// ---------------------------------------------------------------------------

class AgentIOTransportBase {
public:

    virtual ~AgentIOTransportBase() = default;

    /// 发送结构化消息到对端
    /// - Channel: 直接 move 到 channel (零拷贝)
    /// - WS: 内部序列化为 JSON 帧发送
    virtual void send(WireMessage msg) = 0;

    /// 接收对端发来的下一条消息 (协程阻塞直到有消息)
    /// - 返回 nullopt 表示传输已关闭/对端断开 (不可恢复)
    /// - WS 实现内部处理重连; 重连成功时调用方无感知, 重连失败才返回 nullopt
    virtual asio::awaitable<std::optional<WireMessage>> recv() = 0;

    /// 建立连接 (WS: TCP+握手+hello; Channel: 构造时已连通, 此为 no-op)
    /// - 返回 false 表示连接/鉴权失败
    virtual asio::awaitable<bool> connect(const WireHello& hello) {
        (void)hello;
        co_return true;
    }

    /// 关闭传输 (线程安全; 使挂起的 recv 返回 nullopt)
    virtual void close() = 0;

    /// 传输是否仍然存活
    virtual bool alive() const noexcept = 0;

    /// 会话切换通知: 更新客户端重连时握手携带的 sessionId, 并复位增量重放状态
    /// (新会话的 delta seq 独立编号, 旧会话的 seq/tailHash 不再适用)。
    /// - WS 客户端模式: 覆写实现 (见 WsAgentIOTransport)
    /// - Channel/服务端模式: 无重连, 默认 no-op
    /// 线程安全: 可从任意线程调用 (实现内部投递回自身 executor)
    virtual void updateReconnectSessionId(std::string /*newSessionId*/) {}
};

} // namespace agent
} // namespace agentxx
