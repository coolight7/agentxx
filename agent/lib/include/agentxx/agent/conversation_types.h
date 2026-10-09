#pragma once

#include <filesystem>
#include <memory>

#include "utilxx_base/json.h"
#include "utilxx_base/string_util.h"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace agent {

/// 多模态媒体类型
enum class MediaType : uint8_t {
    Image,
    Audio,
    Video
};

inline std::string_view mediaTypeToString(MediaType t) noexcept {
    switch (t) {
        case MediaType::Image:
            return "image";
        case MediaType::Audio:
            return "audio";
        case MediaType::Video:
            return "video";
    }
    return "image";
}

inline MediaType mediaTypeFromString(std::string_view s) noexcept {
    if (s == "audio") {
        return MediaType::Audio;
    }
    if (s == "video") {
        return MediaType::Video;
    }
    return MediaType::Image;
}

/// 多模态单文件/单次限额 (与 TUI 预检/服务端收敛一致)
inline constexpr uint64_t kMaxImageBytes            = 10ULL * 1024 * 1024;
inline constexpr uint64_t kMaxAudioBytes            = 25ULL * 1024 * 1024;
inline constexpr uint64_t kMaxVideoBytes            = 50ULL * 1024 * 1024;
inline constexpr size_t   kMaxAttachmentsPerMessage = 5;

/// 小写后缀 (含 '.') -> MediaType, 非媒体返回 nullopt
inline std::optional<MediaType> mediaTypeFromExtension(std::string_view ext) noexcept {
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" || ext == ".gif"
        || ext == ".bmp") {
        return MediaType::Image;
    }
    if (ext == ".wav" || ext == ".mp3" || ext == ".ogg" || ext == ".m4a" || ext == ".aac"
        || ext == ".flac") {
        return MediaType::Audio;
    }
    if (ext == ".mp4" || ext == ".mov" || ext == ".webm" || ext == ".mkv") {
        return MediaType::Video;
    }
    return std::nullopt;
}

/// 小写后缀 -> MIME, 未知返回空
inline std::string_view mimeTypeFromExtension(std::string_view ext) noexcept {
    if (ext == ".png") {
        return "image/png";
    }
    if (ext == ".jpg" || ext == ".jpeg") {
        return "image/jpeg";
    }
    if (ext == ".webp") {
        return "image/webp";
    }
    if (ext == ".gif") {
        return "image/gif";
    }
    if (ext == ".bmp") {
        return "image/bmp";
    }
    if (ext == ".wav") {
        return "audio/wav";
    }
    if (ext == ".mp3") {
        return "audio/mpeg";
    }
    if (ext == ".ogg") {
        return "audio/ogg";
    }
    if (ext == ".m4a") {
        return "audio/mp4";
    }
    if (ext == ".aac") {
        return "audio/aac";
    }
    if (ext == ".flac") {
        return "audio/flac";
    }
    if (ext == ".mp4") {
        return "video/mp4";
    }
    if (ext == ".mov") {
        return "video/quicktime";
    }
    if (ext == ".webm") {
        return "video/webm";
    }
    if (ext == ".mkv") {
        return "video/x-matroska";
    }
    return "";
}

inline uint64_t maxBytesForMediaType(MediaType t) noexcept {
    switch (t) {
        case MediaType::Image:
            return kMaxImageBytes;
        case MediaType::Audio:
            return kMaxAudioBytes;
        case MediaType::Video:
            return kMaxVideoBytes;
    }
    return kMaxImageBytes;
}

/// 通用附件对象
struct MediaAttachment {
    MediaType   type = MediaType::Image;
    std::string displayName; ///< 文件名 (展示用，如 "chart.png")
    std::string mimeType;    ///< MIME 类型 (如 "image/png", "audio/wav")
    std::string pathOrUrl;   ///< 本地绝对路径或 HTTP(S) URL
    std::string dataUrl;     ///< RFC 2397 格式: "data:<mime>;base64,<payload>"
    uint64_t    sizeBytes = 0;

    inline static std::string_view mediaTypeIcon(agentxx::agent::MediaType type) {
        switch (type) {
            case agentxx::agent::MediaType::Image:
                return "📷︎";
            case agentxx::agent::MediaType::Audio:
                return "🎵︎";
            case agentxx::agent::MediaType::Video:
                return "🎬︎";
        }
        return "📷︎";
    }

    utilxx_base::Json toJson() const {
        utilxx_base::Json j = utilxx_base::Json::object();
        j["type"]           = std::string(mediaTypeToString(type));
        if (!displayName.empty()) {
            j["display_name"] = displayName;
        }
        if (!mimeType.empty()) {
            j["mime_type"] = mimeType;
        }
        if (!pathOrUrl.empty()) {
            j["path_or_url"] = pathOrUrl;
        }
        if (!dataUrl.empty()) {
            j["data_url"] = dataUrl;
        }
        if (sizeBytes > 0) {
            j["size_bytes"] = sizeBytes;
        }
        return j;
    }

    static MediaAttachment fromJson(const utilxx_base::Json& j) {
        MediaAttachment att;
        att.type        = mediaTypeFromString(j.value("type", j.value("kind", std::string{})));
        att.displayName = j.value("display_name", j.value("displayName", j.value("name", std::string{})));
        att.mimeType    = j.value("mime_type", j.value("mimeType", std::string{}));
        att.pathOrUrl   = j.value("path_or_url", j.value("pathOrUrl", j.value("path", std::string{})));
        att.dataUrl     = j.value("data_url", j.value("dataUrl", std::string{}));
        if (j.contains("size_bytes") && j["size_bytes"].is_number()) {
            att.sizeBytes = j["size_bytes"].get<uint64_t>();
        } else if (j.contains("sizeBytes") && j["sizeBytes"].is_number()) {
            att.sizeBytes = j["sizeBytes"].get<uint64_t>();
        } else if (j.contains("size") && j["size"].is_number()) {
            att.sizeBytes = j["size"].get<uint64_t>();
        } else {
            att.sizeBytes = 0;
        }
        return att;
    }
};

/// 估算附件字节大小 (优先取 sizeBytes; 其次估算 dataUrl Base64 解码后大小; 再次检查本地文件大小)
inline uint64_t estimateAttachmentSizeBytes(const MediaAttachment& att) {
    if (att.sizeBytes > 0) {
        return att.sizeBytes;
    }
    if (!att.dataUrl.empty()) {
        auto comma = att.dataUrl.find(',');
        if (comma != std::string::npos) {
            size_t b64len = att.dataUrl.size() - comma - 1;
            return (b64len * 3) / 4;
        }
        return att.dataUrl.size();
    }
    if (!att.pathOrUrl.empty() && !att.pathOrUrl.starts_with("http://")
        && !att.pathOrUrl.starts_with("https://")) {
        std::error_code ec;
        auto            p  = utilxx_base::utf8ToPath(att.pathOrUrl);
        auto            sz = std::filesystem::file_size(p, ec);
        if (!ec) {
            return sz;
        }
    }
    return 0;
}

/// UI 展示消息 (server Session::viewMessages / wire Sync / client 渲染共用)
///
/// 设计: 通用字段 (role/text/时间戳/折叠) 平铺, 角色专属字段按 role 放入
/// optional 子结构, 避免单结构背负所有角色的字段:
/// - Role::Tool:      tool (toolName/toolCallId/toolResult/toolFinished/diff)
/// - Role::Interrupt: interrupt (中断 UI 描述 + 表单状态/结果)
/// - Role::Tip:       tip (tipLevel)
///
/// 注意: 纯 UI 交互状态 (输入框编辑文本/选中项/校验提示/结果回传通道等) 不属于
/// 消息内容, 由渲染端 (如 TUI MessageListComponent) 独立维护, 不进入本结构。
///
/// 序列化 (toJson/fromJson) 供 wire Sync 与链式哈希使用; 角色专属字段只在
/// 对应 role 时输出/解析。
struct ViewMessage {
    enum class Role : uint8_t {
        User,
        Assistant,
        Think,
        System,
        Tool,
        /// 中断表单消息 (内嵌交互控件, 直接渲染在消息列表中)
        Interrupt,
        /// 消息提示
        Tip
    };
    /// 提示消息级别 (System 提示消息使用, 与 agentxx::agent::WireDelta::TipType 对应)
    enum class TipLevel : uint8_t {
        Info,
        Warning,
        Error
    };
    /// 中断表单状态 (Role::Interrupt 消息使用)
    enum class InterruptStatus : uint8_t {
        /// 等待用户操作 (可交互)
        Waiting,
        /// 已提交 (interruptResult 保存结果展示文本)
        Confirmed,
        /// 已取消 (用户主动取消整个中断请求)
        Cancelled,
        /// 已过期 (server 通知中断超时/会话取消, 不再可交互)
        Expired
    };

    // ---- Role::Tool 专属 ----
    struct ToolData {
        std::string toolName;
        std::string toolCallId;
        std::string toolResult;
        /// edit 工具参数 unified diff (server 生成预留; 当前渲染端自行计算, 未处理)
        std::string diff;
        bool        toolFinished = false;
    };

    // ---- Role::Tip 专属 ----
    struct TipData {
        TipLevel tipLevel = TipLevel::Info;
    };

    // ---- Role::Think 专属 ----
    struct ThinkData {
        int  reasoningTokens = 0;     ///< 思考消耗 token 数 (若网关提供)
        bool isEncrypted     = false; ///< 是否为加密 thinking 载体
    };

    // ---- Role::Interrupt 专属 ----
    struct InterruptData {
        /// 中断请求 wire id (对应 WireInterruptRequest.id); 0 = 非中断消息
        int64_t interruptId = 0;
        /// 中断 UI 描述 (声明式; 由 agent 侧生成, 客户端通用渲染)
        /// - 一条中断请求 = 一份表单 (一条消息, ui.items 内可含多个输入控件),
        ///   见 [interrupt_ui.h](/agent/lib/include/agentxx/middlewares/interrupt_ui.h)
        /// - **必填** (服务端 InterruptHandleArg::toJson 恒下发; 缺失 = 契约违规,
        ///   客户端输出诊断行且不可交互)
        utilxx_base::Json ui;
        /// 表单状态
        InterruptStatus interruptStatus = InterruptStatus::Waiting;
        /// 提交结果展示文本 (interruptStatus == Confirmed 时有效; 多控件时为
        /// 各控件结果值的展示拼接)
        std::string interruptResult;
    };

    std::optional<ToolData>      tool      = std::nullopt; ///< Role::Tool 有效
    std::optional<InterruptData> interrupt = std::nullopt; ///< Role::Interrupt 有效

    // ---- 成员: 按对齐/尺寸从大到小排列, 减少结构体内填充字节 ----
    // ---- 通用字段 (所有 role) ----
    /// 历史消息 id (appendViewMessage 分配); 客户端本地消息 (如 TUI 中断消息) 可为空
    std::string id;
    /// 正文: User/Assistant/Think/System 消息文本; Tool 消息为工具参数
    /// (arguments JSON 字符串, 与渲染侧现有约定一致)
    std::string text;

    /// 多模态媒体附件列表
    std::vector<MediaAttachment> attachments;
    int64_t     startTimeMs = 0; ///< 开始时间戳 (毫秒, Unix 时间戳)
    int64_t     durationMs  = 0; ///< 运行时长 (毫秒)
    std::optional<ThinkData>     think     = std::nullopt; ///< Role::Think 有效
    std::optional<TipData>       tip       = std::nullopt; ///< Role::Tip 有效
    Role        role = Role::User;
    /// 折叠展示 (Think/Tool/System/Tip 消息; 点击可折叠/展开)
    bool collapsed = false;

    /// 便捷构造: 纯文本消息 (User/Assistant/Think/System/Tip)
    /// - Tip 消息自动创建 tip 子结构 (tipLevel 默认 Info), 且默认折叠展示
    ///   (提示类消息内容通常较长, 折叠避免占据消息列表空间, 点击可展开)
    static ViewMessage
        makeText(Role role, std::string text, int64_t startTimeMs = 0, int64_t durationMs = 0) {
        ViewMessage m;
        m.role        = role;
        m.text        = std::move(text);
        m.startTimeMs = startTimeMs;
        m.durationMs  = durationMs;
        if (role == Role::Tip) {
            m.tip       = TipData{};
            m.collapsed = true;
        }
        return m;
    }

    /// 序列化为 wire/哈希 JSON (角色专属字段按 role 输出)
    utilxx_base::Json toJson() const;
    /// 从 wire/哈希 JSON 解析; 非法 role 或缺省字段时按默认值解析。
    /// 保证 role 专属子结构在对应 role 下非空 (Tool/System/Interrupt)
    static ViewMessage fromJson(const utilxx_base::Json& j);
};

/// 会话列表条目摘要 (会话选择弹窗展示用)
/// - sessionId:     会话唯一标识
/// - title:        会话名称 (取首条用户消息的单行预览; 无用户消息时为空, 展示端回退 sessionId)
/// - lastActiveMs: 最近活动时间 (毫秒时间戳; 取末条消息开始时间, 无消息时为 0)
struct SessionInfo {
    std::string sessionId;
    std::string title;
    int64_t     lastActiveMs = 0;
    /// 检索命中片段 (计划 RET-1a; 仅检索结果填充, 普通列表为空)
    /// - 正文命中时为关键词附近的文本片段; 标题命中时为空 (标题本身即命中处)
    std::string snippet;
};

/// 加载组件通知：显示加载的插件/MCP/Skill/Memory 信息
struct AppendComponentNotification {
    enum class Type : uint8_t {
        Mcp,    // MCP 工具
        Skill,  // Skill
        Memory, // Memory 文件
        Plugin, // Agent 侧加载的插件
    };
    // 成员按对齐/尺寸从大到小排列, 减少结构体内填充字节
    std::string name;         // 名称 (MCP 命名空间 / Skill 名 / Memory 文件名)
    std::string errorMessage; // 失败时的错误信息

    Type        type;
    bool        success;      // 是否加载成功
};

/// 链式哈希 (FNV-1a 逐段追加): 将消息序列逐个 append 形成一条哈希链,
/// 供会话消息集合做内容指纹 (wire 同步/轮询时比对 tailHash 是否一致)
/// - 每个消息需序列化为字符串后 append, 顺序敏感 (追加顺序影响最终哈希)
/// - `return` 见 [tail] / [tailHex]
class ChainHash {
public:

    /// 追加一段数据到链尾 (内部自动以其当前哈希作为下一段的种子)
    void append(std::string_view serialized);
    /// 清空链 (哈希与计数归零)
    void reset();

    /// 当前链尾哈希值 (uint64; 未追加任何数据时为 0)
    uint64_t tail() const;
    /// 已追加的段数 (供调用方判断是否首次追加)
    uint64_t count() const;
    /// 链尾哈希的十六进制字符串 (固定 16 位, 如 "9c3f..." 小写)
    std::string tailHex() const;

private:

    uint64_t hash_  = 0;
    uint64_t count_ = 0;
};

struct WireDelta {
    /// 提示消息级别 (MessageUITip 使用)
    enum class TipType : uint8_t {
        Info,    ///< 普通提示
        Warning, ///< 警告
        Error,   ///< 错误
    };

    enum class Type : uint8_t {
        TextToken,
        ThinkToken,
        ToolStart,
        ToolEnd,
        TurnStart,
        TurnEnd,
        NodeStart,
        NodeEnd,
        /// 通用瞬态提示消息 (info/warning/error, 仅 UI 展示, 不入会话历史)
        MessageUITip,
        /// 完整 ViewMessage 消息插入 (原子消息载荷, 如 Tip 提示、完整卡片等)
        InsertMessage,
        /// 完整 ViewMessage 消息更新 (按 msgId 定位更新已插入的消息)
        UpdateMessage,
    };

    // 成员按对齐/尺寸从大到小排列, 减少结构体内填充字节
    std::string text;

    std::string msgId;
    std::string toolName;
    std::string toolCallId;
    std::string arguments;

    std::string result;

    std::string nodeName;
    std::string tailHash;

    uint64_t seq          = 0;
    uint64_t historyCount = 0;

    // 运行时长统计
    int64_t startTimeMs = 0; // 开始时间戳 (毫秒)
    int64_t durationMs  = 0; // 运行时长 (毫秒)

    // 轮次统计 (TurnEnd 使用): 本轮会话 LLM API 平均生成速度 (token/s, 估算值)
    // - 0 = 本轮无 LLM 流式输出 (如纯工具错误轮)
    double tps = 0.0;

    /// 完整 ViewMessage 载荷 (InsertMessage 使用)
    std::shared_ptr<ViewMessage> message = nullptr;

    /// TurnStart 即时回显附件 (服务端 viewMessages 权威副本经 Sync 补齐):
    /// - TurnStart 历史上仅 text+msgId，附件会丢失导致首屏 user 消息无卡片
    /// - 此处仅传元数据（displayName/mimeType/pathOrUrl/sizeBytes/type），
    ///   不传 dataUrl（Base64 体积大，走 Sync 全量时再补齐）
    std::vector<MediaAttachment> attachments;

    // Think 结构体 (Role::Think 消息专属, 如加密思考/token统计)
    std::optional<ViewMessage::ThinkData> think = std::nullopt;

    Type type = Type::TextToken;

    // MessageUITip: 通用提示消息 (文本复用 text 字段)
    TipType tipType = TipType::Info; ///< 提示级别 (Info/Warning/Error)

    bool hasError = false;
};

/// 估算一条展示消息在内存中大致占用的字节数 (对象本身 + 各字段字符串)
/// - 用于按内存量控制缓冲规模 (见服务端重放缓冲的字节上限), 不追求精确
/// - attachments 按元数据字符串计, dataUrl 之外的解码数据不计
inline size_t estimateViewMessageBytes(const ViewMessage& msg) {
    size_t bytes  = sizeof(ViewMessage);
    bytes        += msg.id.size() + msg.text.size();
    if (msg.tool) {
        bytes += msg.tool->toolName.size() + msg.tool->toolCallId.size()
                 + msg.tool->toolResult.size() + msg.tool->diff.size();
    }
    if (msg.interrupt) {
        // ui 为声明式表单 JSON, 只粗估一个常量, 避免为估算而序列化
        bytes += msg.interrupt->interruptResult.size() + 256;
    }
    for (const auto& a : msg.attachments) {
        bytes += sizeof(MediaAttachment) + a.displayName.size() + a.mimeType.size()
                 + a.pathOrUrl.size() + a.dataUrl.size();
    }
    return bytes;
}

/// 估算一条会话增量在内存中大致占用的字节数 (对象本身 + 各字符串 + 携带的消息载荷)
/// - 流式 token 增量很小, 但带完整消息载荷的增量 (InsertMessage/UpdateMessage,
///   如工具结果回填) 可能是几十 KB 级, 需要按字节核算
inline size_t estimateWireDeltaBytes(const WireDelta& delta) {
    size_t bytes  = sizeof(WireDelta);
    bytes        += delta.text.size() + delta.msgId.size() + delta.toolName.size()
             + delta.toolCallId.size() + delta.arguments.size() + delta.result.size()
             + delta.nodeName.size() + delta.tailHash.size();
    for (const auto& a : delta.attachments) {
        bytes += sizeof(MediaAttachment) + a.displayName.size() + a.mimeType.size()
                 + a.pathOrUrl.size() + a.dataUrl.size();
    }
    if (delta.message) {
        bytes += estimateViewMessageBytes(*delta.message);
    }
    return bytes;
}

/// 输入投递模式 (`WireUserInput.delivery`; 计划 LOOP-2)
///
/// - `next-turn` (默认): 排队等待当前轮次结束后作为新轮次执行
/// - `next-step`: 在当前轮次的下一个 modelcall 安全边界注入 (写入会话权威上下文),
///   不打断正在进行的 provider 流; 空闲时等同 `next-turn`
/// - `inject`: 只进入下一次请求的动态注入队列 (请求级可见, 不改写权威上下文,
///   也不唤醒会话); 空闲时保持待注入, 直到下一次请求装配时被取走
/// - `collect`: 在短暂静默窗口内合并同一客户端的连续输入 (计划 LOOP-11),
///   窗口结束后按 `next-turn` 语义提交为一条
///
/// 放在会话类型头 (而非 wire 头): 会话内待注入输入与消息队列条目都使用同一套
/// 取值, 非 wire 代码 (节点/会话) 也需要引用。
struct InputDelivery {
    inline static constexpr std::string_view NextTurn = "next-turn";
    inline static constexpr std::string_view NextStep = "next-step";
    inline static constexpr std::string_view Inject   = "inject";
    inline static constexpr std::string_view Collect  = "collect";

    /// 归一化投递模式: 空取 [NextTurn]; 其余原样返回 (未知取值由调用方拒绝)
    static std::string_view normalize(std::string_view v) noexcept {
        return v.empty() ? NextTurn : v;
    }

    /// 是否为已知投递模式
    static bool known(std::string_view v) noexcept {
        return v == NextTurn || v == NextStep || v == Inject || v == Collect;
    }
};

/// 排队等待发送的消息条目 (服务端按会话维护, 同步到客户端展示)
struct MessageQueueItem {
    std::string id;    ///< 条目唯一标识 (如 "q-1")
    std::string text;  ///< 消息内容
    std::string model; ///< 本条消息指定的待应用模型 (空 = 默认/当前)
    std::vector<MediaAttachment> attachments;     ///< 排队项保留附件
    int64_t                      createdAtMs = 0; ///< 创建时间戳 (毫秒)
    /// 投递模式 (取值见 agentxx::agent::InputDelivery; 空 = next-turn)
    std::string delivery;
    /// 是否为进程重启后恢复的待确认输入 (计划 LOOP-1):
    /// 恢复项不会自动执行, 由用户确认 (发送新输入解除暂停) 或删除
    bool recovered = false;
};

/// 会话内待注入的输入 (next-step / inject; 计划 LOOP-2)
///
/// 与 MessageQueueItem 的区别: 排队条目等待"下一个轮次", 本结构等待"下一个
/// 安全的 modelcall 边界", 因此不进入消息队列, 也不唤醒会话。
struct SessionPendingInput {
    std::string id;    ///< 条目标识 (与持久化收件箱 session_input.id 对应; 可空)
    std::string text;  ///< 文本内容
    std::string model; ///< 本条输入指定的待应用模型 (空 = 默认/当前)
    /// 来源 (计划 LOOP-2 要求"记录来源"): "user" = 用户, 其余为插件名/内部来源
    std::string source = "user";
    /// 投递模式 (InputDelivery::NextStep / Inject)
    std::string delivery;
    /// 附件 (next-step/inject 不接受附件: 带附件时按 next-turn 排队)
    std::vector<MediaAttachment> attachments;
    int64_t                      createdAtMs = 0;
};

/// 带持久化序号的展示历史消息 (计划 STO-4)
/// - `seq` 为该消息在会话库 `view_message.seq` 中的序号 (会话内单调递增)
/// - 断线重连增量补拉时按序号取差量 (见 SessionStore::loadViewMessagesAfter)
struct SequencedViewMessage {
    uint64_t    seq = 0;
    ViewMessage message;
};

struct WireSyncPayload {
    /// 本批 messages 首条在服务端完整 viewMessages 中的绝对下标
    /// - 全量同步: 0
    /// - 尾窗同步 (历史分页, initialSyncTailCount>0): 窗口起始下标 (>0 表示
    ///   上方还有更早消息未同步), 客户端据此支持"向上滚动加载更早历史"
    uint64_t                 fromIndex = 0;
    std::vector<ViewMessage> messages;
    std::string              tailHash;
    /// 服务端会话总消息数 (0 = 未提供/未知; 全量同步时 == messages.size())
    uint64_t                      totalMessages = 0;
    std::vector<MessageQueueItem> messageQueue;

    /// 本快照对应的服务端增量序号 (Session::deltaSeq):
    /// 快照已包含 seq <= deltaSeq 的全部增量, 客户端据此重置去重用的序号。
    /// - 服务端进程重启/会话重建后 seq 从 0 重新计数, 客户端若保留旧序号
    ///   (如 5000) 则会把新会话的 seq=1,2,... 全部判为重复并丢弃 (界面不再刷新)
    /// - 0 = 未提供/无会话 (客户端按"复位为 0"处理, 放行后续全部增量)
    uint64_t deltaSeq = 0;

    /// 消息队列状态 (取值见 SessionQueueState; 空 = 旧服务端未提供)
    std::string queueState;

    /// 本快照最后一条展示消息的持久化序号 (计划 STO-4; 0 = 无消息/未提供)
    /// - 客户端记录后在重连 hello 中回传 (`WireHello::afterViewSeq`), 请求
    ///   "该序号之后"的增量补拉, 避免重连时重传整个尾窗
    uint64_t lastViewSeq = 0;

    /// 本批消息是否为**增量补拉** (计划 STO-4)
    /// - true: `messages` 是既有历史尾部的追加内容, 客户端应按 msg.id 去重后追加,
    ///   不得整体替换本地历史
    /// - false (默认): 全量或尾窗快照, 客户端整体替换本地历史
    bool incremental = false;
};

// ---------------------------------------------------------------------------
// ViewMessage <-> json (wire Sync / 链式哈希共用)
// ---------------------------------------------------------------------------

inline std::string_view viewMessageRoleToString(ViewMessage::Role role) noexcept {
    using R = ViewMessage::Role;
    switch (role) {
        case R::User:
            return "user";
        case R::Assistant:
            return "assistant";
        case R::Think:
            return "thinking";
        case R::System:
            return "system";
        case R::Tip:
            return "tip";
        case R::Tool:
            return "tool";
        case R::Interrupt:
            return "interrupt";
    }
    return "user";
}

inline std::optional<ViewMessage::Role> viewMessageRoleFromString(std::string_view s) noexcept {
    using R = ViewMessage::Role;
    if (s == "user") {
        return R::User;
    }
    if (s == "assistant") {
        return R::Assistant;
    }
    if (s == "thinking") {
        return R::Think;
    }
    if (s == "system") {
        return R::System;
    }
    if (s == "tip") {
        return R::Tip;
    }
    if (s == "tool") {
        return R::Tool;
    }
    if (s == "interrupt") {
        return R::Interrupt;
    }
    return std::nullopt;
}

inline std::string_view viewMessageTipLevelToString(ViewMessage::TipLevel l) noexcept {
    using T = ViewMessage::TipLevel;
    switch (l) {
        case T::Warning:
            return "warning";
        case T::Error:
            return "error";
        case T::Info:
            return "info";
    }
    return "info";
}

inline ViewMessage::TipLevel viewMessageTipLevelFromString(std::string_view s) noexcept {
    using T = ViewMessage::TipLevel;
    if (s == "warning") {
        return T::Warning;
    }
    if (s == "error") {
        return T::Error;
    }
    return T::Info;
}

inline std::string_view viewMessageInterruptStatusToString(ViewMessage::InterruptStatus s
) noexcept {
    using S = ViewMessage::InterruptStatus;
    switch (s) {
        case S::Confirmed:
            return "confirmed";
        case S::Cancelled:
            return "cancelled";
        case S::Expired:
            return "expired";
        case S::Waiting:
            return "waiting";
    }
    return "waiting";
}

inline ViewMessage::InterruptStatus viewMessageInterruptStatusFromString(std::string_view s
) noexcept {
    using S = ViewMessage::InterruptStatus;
    if (s == "confirmed") {
        return S::Confirmed;
    }
    if (s == "cancelled") {
        return S::Cancelled;
    }
    if (s == "expired") {
        return S::Expired;
    }
    return S::Waiting;
}

inline utilxx_base::Json ViewMessage::toJson() const {
    utilxx_base::Json j = utilxx_base::Json::object();
    if (!id.empty()) {
        j["id"] = id;
    }
    j["role"]        = std::string(viewMessageRoleToString(role));
    j["text"]        = text;
    j["startTimeMs"] = startTimeMs;
    j["durationMs"]  = durationMs;
    if (collapsed) {
        j["collapsed"] = true;
    }
    if (tool) {
        utilxx_base::Json t = utilxx_base::Json::object();
        if (!tool->toolName.empty()) {
            t["tool_name"] = tool->toolName;
        }
        if (!tool->toolCallId.empty()) {
            t["tool_call_id"] = tool->toolCallId;
        }
        if (!tool->toolResult.empty()) {
            t["tool_result"] = tool->toolResult;
        }
        if (!tool->diff.empty()) {
            t["diff"] = tool->diff;
        }
        if (tool->toolFinished) {
            t["tool_finished"] = true;
        }
        j["tool"] = std::move(t);
    }
    if (tip) {
        j["tip"] = utilxx_base::Json{
            {"tip_level", std::string(viewMessageTipLevelToString(tip->tipLevel))},
        };
    }
    if (think) {
        utilxx_base::Json th = utilxx_base::Json::object();
        if (think->reasoningTokens > 0) {
            th["reasoning_tokens"] = think->reasoningTokens;
        }
        if (think->isEncrypted) {
            th["is_encrypted"] = true;
        }
        if (!th.empty()) {
            j["think"] = std::move(th);
        }
    }
    if (interrupt) {
        utilxx_base::Json it = utilxx_base::Json::object();
        it["interrupt_id"]   = interrupt->interruptId;
        // 中断 UI 描述 (声明式, 服务端生成): 客户端据此通用渲染表单控件
        if (!interrupt->ui.is_null()) {
            it["ui"] = interrupt->ui;
        }
        it["interrupt_status"]
            = std::string(viewMessageInterruptStatusToString(interrupt->interruptStatus));
        if (!interrupt->interruptResult.empty()) {
            it["interrupt_result"] = interrupt->interruptResult;
        }
        j["interrupt"] = std::move(it);
    }
    if (!attachments.empty()) {
        utilxx_base::Json arr = utilxx_base::Json::array();
        for (const auto& a : attachments) {
            arr.push_back(a.toJson());
        }
        j["attachments"] = std::move(arr);
    }
    return j;
}

inline ViewMessage ViewMessage::fromJson(const utilxx_base::Json& j) {
    ViewMessage m;
    m.id          = j.value("id", std::string{});
    m.text        = j.value("text", std::string{});
    m.startTimeMs = j.value("startTimeMs", j.value("start_time_ms", int64_t{0}));
    m.durationMs  = j.value("durationMs", j.value("duration_ms", int64_t{0}));
    m.collapsed   = j.value("collapsed", false);
    if (auto role = viewMessageRoleFromString(j.value("role", std::string{}))) {
        m.role = *role;
    } else {
        m.role = ViewMessage::Role::User;
    }
    // 角色专属子结构: 对应 role 下保证非空 (渲染端可直接解引用)
    switch (m.role) {
        case ViewMessage::Role::Tool: {
            ViewMessage::ToolData t;
            if (j.contains("tool")) {
                const auto& tj = j["tool"];
                t.toolName     = tj.value("tool_name", std::string{});
                t.toolCallId   = tj.value("tool_call_id", std::string{});
                t.toolResult   = tj.value("tool_result", std::string{});
                t.diff         = tj.value("diff", std::string{});
                t.toolFinished = tj.value("tool_finished", false);
            }
            m.tool = std::move(t);
            break;
        }
        case ViewMessage::Role::Tip: {
            ViewMessage::TipData s;
            if (j.contains("tip")) {
                s.tipLevel
                    = viewMessageTipLevelFromString(j["tip"].value("tip_level", std::string{}));
            }
            m.tip = std::move(s);
            break;
        }
        case ViewMessage::Role::Interrupt: {
            ViewMessage::InterruptData it;
            if (j.contains("interrupt")) {
                const auto& ij     = j["interrupt"];
                it.interruptId     = ij.value("interrupt_id", int64_t{0});
                it.interruptStatus = viewMessageInterruptStatusFromString(
                    ij.value("interrupt_status", std::string{})
                );
                it.interruptResult = ij.value("interrupt_result", std::string{});
                if (ij.contains("ui") && ij["ui"].is_object()) {
                    it.ui = ij["ui"];
                }
            }
            m.interrupt = std::move(it);
            break;
        }
        case ViewMessage::Role::Think: {
            if (j.contains("think") && j["think"].is_object()) {
                ViewMessage::ThinkData th;
                th.reasoningTokens = j["think"].value("reasoning_tokens", 0);
                th.isEncrypted     = j["think"].value("is_encrypted", false);
                m.think            = std::move(th);
            }
            break;
        }
        case ViewMessage::Role::User:
        case ViewMessage::Role::System:
        case ViewMessage::Role::Assistant:
            break;
    }
    if (j.contains("attachments") && j["attachments"].is_array()) {
        for (const auto& aj : j["attachments"]) {
            m.attachments.push_back(MediaAttachment::fromJson(aj));
        }
    }
    return m;
}

} // namespace agent
} // namespace agentxx
