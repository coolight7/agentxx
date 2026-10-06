#pragma once

#include "agentxx/agent/conversation_types.h"
#include "asio/awaitable.hpp"
#include "utilxx_base/asio_error.h"
#include "utilxx_base/json.h"
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

/// 输入受理状态 (`WireInputAck.status`; 计划 LOOP-3)
///
/// 客户端据此展示"已开始 / 已排队 / 已注入 / 被拒绝", 不再从 delta 猜测。
struct InputStatus {
    inline static constexpr std::string_view Started  = "started";  ///< 已立即开始新轮次
    inline static constexpr std::string_view Queued   = "queued";   ///< 已受理, 排队等待
    inline static constexpr std::string_view Steered  = "steered";  ///< 已注入当前轮次
    inline static constexpr std::string_view Rejected = "rejected"; ///< 未受理 (原因见 reason)
};

/// 输入被拒绝的结构化原因 (`WireInputAck.reason`; 与展示文本 detail 分开)
struct InputRejectReason {
    /// 文本与附件都为空
    inline static constexpr std::string_view EmptyContent = "empty_content";
    /// 会话不可用 (agent 已释放/会话无法创建)
    inline static constexpr std::string_view SessionNotFound = "session_not_found";
    /// 请求携带的会话 ID 与本端点绑定的会话不一致
    inline static constexpr std::string_view SessionMismatch = "session_mismatch";
    /// 服务端已停止接受输入
    inline static constexpr std::string_view ServerStopped = "server_stopped";
    /// 投递模式取值非法
    inline static constexpr std::string_view BadDelivery = "bad_delivery";
    /// 队列在本次输入提交前被清空 (collect 合并窗口内的输入被丢弃)
    inline static constexpr std::string_view QueueCleared = "queue_cleared";
};

/// 服务端消息队列状态 (计划 LOOP-4: 取代"两个 bool"的隐式状态)
///
/// 状态含义与转移:
/// - `Idle`:     无进行中轮次; 队列为空时等待用户输入, 非空时即将开始执行
/// - `Running`:  有进行中轮次, 队列中消息将在本轮正常结束后按序继续
/// - `Paused`:   上一轮以取消/异常/中断结束, 队列暂停不自动继续;
///               只有新的用户输入或"打断并运行下一条"能解除
/// - `Draining`: 暂停解除后正在按序消化积压队列 (诊断/界面区分用, 与 Running
///               同义但表示"队列非空且刚开始消化")
enum class SessionQueueState : uint8_t {
    Idle = 0,
    Running,
    Paused,
    Draining,
};

/// 队列状态文本 (Wire 传输与日志用; 客户端按字符串识别, 未知值按 Idle 处理)
inline std::string_view sessionQueueStateText(SessionQueueState state) noexcept {
    switch (state) {
        case SessionQueueState::Running:
            return "running";
        case SessionQueueState::Paused:
            return "paused";
        case SessionQueueState::Draining:
            return "draining";
        case SessionQueueState::Idle:
        default:
            return "idle";
    }
}

/// 解析队列状态文本 (未知/空 → Idle)
inline SessionQueueState sessionQueueStateFromText(std::string_view text) noexcept {
    if (text == "running") {
        return SessionQueueState::Running;
    }
    if (text == "paused") {
        return SessionQueueState::Paused;
    }
    if (text == "draining") {
        return SessionQueueState::Draining;
    }
    return SessionQueueState::Idle;
}

struct WireHello {
    std::string sessionId;
    std::string token;
    uint64_t    lastSeq = 0;
    std::string tailHash;
    std::string model;
    std::string language = "en"; ///< 界面/客户端指定使用的语言 (默认 "en", 不支持 auto)
    /// 已持有的展示历史序号 (计划 STO-4): >0 时服务端按"该序号之后的消息"增量补拉
    /// (见 WireSyncPayload::incremental), 0 = 未提供 (全量/尾窗同步)
    uint64_t afterViewSeq = 0;
};

struct WireHelloAck {
    bool                     ok = false;
    std::string              sessionId;
    std::string              tailHash;
    std::vector<std::string> models;

    /// 单个服务端插件的声明信息
    struct PluginInfo {
        std::string name;
        std::string version;
        /// 插件清单声明的接口名 (interfaces.require ∪ optional; 见
        /// [plugin_interfaces.h](/agent/lib/include/agentxx/plugin/plugin_interfaces.h)
        /// 接口协商节), client 插件据此感知对端能力
        std::vector<std::string> interfaces;
    };

    /// 服务端已加载的 agent 侧插件列表 (client 插件判断对端可用性/能力的
    /// 正式通道; 服务端不携带该字段 → 反序列化为空数组, 客户端按"未知"处理)
    std::vector<PluginInfo> plugins;

    /// 服务端设备唯一标识 (32 位全小写十六进制 MD5)
    std::string deviceId;

    /// 服务端当前会话工作目录绝对路径
    std::string workDir;
};

struct WireUserInput {
    std::string sessionId;
    std::string text;
    /// 本条消息携带的模型选择 (空 = 不切换): TUI 切模型不再即时发送
    /// WireSelectModel, 而是随下一次用户消息携带, BaseAgent 执行该轮会话
    /// 开始时 (runTurnAsync 内 selectModel) 自动切换
    std::string                  model;
    std::vector<MediaAttachment> attachments; ///< 携带附件
    /// 投递模式 (取值见 [InputDelivery]; 空 = next-turn, 兼容旧客户端)
    std::string delivery;
    /// 客户端请求序号: >0 时服务端回 [WireInputAck] 明确受理结果;
    /// 0 = 不需要回执 (旧客户端; 服务端不回, 避免对端不认识新消息类型)
    uint64_t requestId = 0;
};

/// 输入受理回执 (Server -> Client; 计划 LOOP-3)
///
/// 仅在 `WireUserInput.requestId > 0` 时发送: 老客户端不认识本消息类型,
/// WS 解码遇到未知类型会断开连接, 因此不能用"总是回执"的方式。
struct WireInputAck {
    uint64_t    requestId = 0;  ///< 回显请求序号 (客户端据此关联本地输入)
    std::string sessionId;
    std::string delivery;       ///< 归一化后的投递模式 ([InputDelivery])
    std::string status;         ///< 受理状态 ([InputStatus])
    std::string reason;         ///< 拒绝原因 ([InputRejectReason]; 仅 rejected)
    std::string detail;         ///< 给人看的说明 (可本地化/含细节)
    std::string itemId;         ///< 队列条目 id (queued/started 时有值)
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
    int64_t           id = 0;
    utilxx_base::Json result;
};

/// 服务端通知中断已过期 (超时/会话取消) (Server -> Client)
/// - id 对应 WireInterruptRequest.id; 客户端应将对应未操作的中断消息标记为过期
struct WireInterruptExpired {
    int64_t     id = 0;
    std::string sessionId;
};

struct WireTurnResult {
    // 成员按对齐/尺寸从大到小排列, 减少结构体内填充字节
    std::string sessionId;
    std::string errorMessage;
    int64_t     startTimeMs = 0; // 轮次开始时间戳 (毫秒)
    int64_t     durationMs  = 0; // 运行时长 (毫秒)
    bool        hasError = false;
    bool        interrupted = false;
};

struct WireContextStats {
    uint64_t contextTokens    = 0;
    uint64_t maxContextTokens = 0;
    /// 当前 ModelCall 平均生成速度 (token/s, 估算值); 0 = 无流式/无数据
    double tps = 0.0;
};

/// Wire 层错误码 (`WireError::code`)
///
/// 展示文本与机器错误码分开: `message` 给人看 (可本地化/含细节), `code` 供程序判断
/// (客户端按码决定"重试 / 提示 / 忽略")。**未知码按 [Internal] 处理**, 不要因为
/// 对端新增了码就报错或丢弃消息。
struct WireErrorCode {
    /// 其他内部错误 (未知码的兜底)
    inline static constexpr int Internal = 0;
    /// 当前状态不允许该请求 (如未完成握手就发业务消息)
    inline static constexpr int InvalidState = 1;
    /// 会话不存在 (已删除/未创建)
    inline static constexpr int SessionNotFound = 2;
    /// 请求的会话 ID 与本端点绑定的会话不一致
    inline static constexpr int SessionMismatch = 3;
    /// 请求参数不合法 (缺字段/取值越界)
    inline static constexpr int InvalidArgs = 4;
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

/// 单个模型的多模态输入能力描述
struct ModelCapabilityInfo {
    std::string name;
    bool        imageInput = false;
    bool        audioInput = false;
    bool        videoInput = false;

    bool hasMultimodalInput() const noexcept {
        return imageInput || audioInput || videoInput;
    }
};

/// 服务端模型信息响应 (Server -> Client)
struct WireModelInfo {
    std::string                      currentModel;
    std::vector<std::string>         models;
    std::vector<ModelCapabilityInfo> capabilities; ///< 各模型的多模态能力清单
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
    utilxx_base::Json messages;
};

/// 客户端请求压缩当前会话上下文 (Client -> Server)
struct WireCompactContext {
    std::string sessionId;
};

/// 客户端请求持久化会话列表 (Client -> Server): 会话选择弹窗数据源
/// - 不携带 sessionId: 列举全部持久化会话, 与当前连接会话无关
/// - 支持分页 (keyset 游标): 客户端先请求最新一页, 浏览到末尾时按游标续取,
///   避免会话很多时一次性扫描/传输/渲染全量; limit == 0 为全量
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
/// - client 侧插件 (agentxx_plugin_client_create) 经 send_plugin_data 发出的跨端事件;
///   服务端收到后发布到事件总线 topic `client.{插件名}.{事件名}` (载荷 std::string),
///   由 agent 侧同名插件订阅处理
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
    /// 队列状态 (取值见 SessionQueueState::text: idle/running/paused/draining;
    /// 空 = 旧服务端未提供). 客户端据此区分"正在排队执行"与"已暂停不再自动执行"
    std::string state;
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

/// 目录条目 (客户端-服务端文件浏览; name 为纯文件名/目录名不含图标, 图标由 UI 自主渲染)
struct WireDirEntry {
    std::string name;     ///< 文件名或目录名 (纯名称, 不含图标)
    std::string fullPath; ///< 服务端绝对路径
    uint64_t    sizeBytes = 0;
    bool        isDir     = false;
    bool        supported = true; ///< 当前模型是否支持该文件类型
    MediaType   mediaType = MediaType::Image;
};

/// 客户端请求列举服务端目录 (Client -> Server)
struct WireListDir {
    uint64_t    reqId = 0; ///< 请求自增序号
    std::string path;      ///< 服务端绝对路径 (空则使用服务端工作空间目录)
    std::vector<std::string> allowedExtensions; ///< 当前模型支持的文件扩展名白名单 (.png, .jpg 等)
};

/// 服务端列举目录响应 (Server -> Client)
struct WireListDirResult {
    uint64_t                  reqId = 0;
    bool                      ok    = false;
    std::string               currentDir;
    std::string               parentDir;
    std::vector<WireDirEntry> entries;
    std::string               error;
};

/// 客户端查询当前权限状态 (Client -> Server; 无载荷)
///
/// 界面 (TUI Info 侧边栏) 需要展示"完全授权"状态并允许用户切换, 而该状态
/// 由 agent 侧权限中间件持有, 因此客户端经本消息主动查询一次
/// (接入握手后 + 用户点击切换后), 服务端以 [WirePermissionState] 回应。
struct WireGetPermissionState {};

/// 客户端设置"完全授权"状态 (Client -> Server)
struct WireSetFullAuth {
    /// true = 完全授权 (后续不再询问权限), false = 恢复询问
    bool fullAuth = true;
};

/// 服务端权限状态 (Server -> Client)
///
/// 三种来源共用同一消息: 查询响应 / 切换后的广播 / 状态变更广播
/// (用户在权限询问卡片勾选"完全授权所有权限"后, agent 侧中间件发布事件,
/// 服务端据此广播给所有已连接客户端, 多端界面保持一致)。
struct WirePermissionState {
    /// 是否已完全授权 (对应 PermissionMiddlewareHandle::isFullAuthorized)
    bool fullAuth = false;
};

/// 客户端新增模型配置 (Client -> Server; TUI 选择模型弹窗顶部的"添加模型配置")
///
/// 服务端以此注册一个运行时可用的模型 (立即出现在模型列表、可切换) 并把配置
/// 写入数据目录的 `agentxx-config.yaml` (文件不存在则创建), 使其重启后仍可用。
/// 字段语义与 yaml `model.list` 条目一致 (见 agentxx::agent::ModelConfig)。
struct WireAddModel {
    std::string sessionId; ///< 发起请求的会话 (注册成功后该会话立即切换为新模型)
    std::string name;      ///< 模型名称 (配置键; 必填且不可与已有模型重名)
    std::string modelType; ///< openai / openai-responses / anthropic (空 = openai)
    std::string baseUrl;   ///< API 地址 (与 apiKey 至少给出其一)
    std::string apiPath;   ///< 自定义 API 路径 (可空 = 按类型用默认路径)
    std::string apiKey;    ///< API Key (无鉴权服务填 EMPTY)
    std::string modelName; ///< 请求体里的 model 字段值
    uint64_t    modelContextMaxToken    = 0;
    uint64_t    maxConcurrentConnections = 5;
    int32_t     connectTimeoutSeconds    = 16;
    int32_t     readChunkTimeoutSeconds  = 60;
    int8_t      sslVerify                = -1; ///< -1 未指定 / 0 不验证 / 1 验证
    bool        sendThinking             = false;
    bool        requestReasoningSummary  = true;
    bool        imageInput               = false;
    bool        audioInput               = false;
    bool        videoInput               = false;
    /// 额外请求体参数 (yaml `extra_api_config`; 空对象 = 未指定)
    utilxx_base::Json extraApiConfig;
    /// 额外请求头 (yaml `extra_headers`; 空对象 = 未指定)
    utilxx_base::Json extraHeaders;
};

/// 新增模型配置的结果 (Server -> Client)
struct WireAddModelResult {
    bool        ok = false;
    std::string name;  ///< 新增的模型名 (ok=true 时客户端据此立即切换)
    std::string error; ///< 失败原因 (给用户看的文本; ok=true 时为空)
};

/// 所有可能的线消息类型 (tagged variant)
using WireMessage = std::variant<
    WireHello,
    WireHelloAck,
    WireUserInput,
    WireInputAck,
    WireCancel,
    WireSelectModel,
    WireInterruptRequest,
    WireInterruptResponse,
    WireInterruptExpired,
    WireDelta,
    WireSyncPayload,
    WireTurnResult,
    WireContextStats,
    WireError,
    WireLog,
    WireGetModel,
    WireModelInfo,
    WireGetAppendComponentInfo,
    WireAppendComponentInfo,
    WireGetContext,
    WireCompactContext,
    WireContextMessages,
    WireListSessions,
    WireSessionList,
    WireSwitchSession,
    WirePluginData,
    WirePluginDataUp,
    WireMessageQueueUpdate,
    WireClearMessageQueue,
    WireRemoveQueueItem,
    WireInterruptAndRunNext,
    WireGetViewMessages,
    WireViewMessagesPage,
    WireListDir,
    WireListDirResult,
    WireGetPermissionState,
    WireSetFullAuth,
    WirePermissionState,
    WireAddModel,
    WireAddModelResult>;

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
