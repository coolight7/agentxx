#pragma once

#include "neograph/json.h"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace agent {

/// UI 展示消息 (server Session::viewMessages / wire Sync / client 渲染共用)
///
/// 设计: 通用字段 (role/text/时间戳/折叠) 平铺, 角色专属字段按 role 放入
/// optional 子结构, 避免单结构背负所有角色的字段:
/// - Role::Tool:      tool (toolName/toolCallId/toolResult/toolFinished/diff)
/// - Role::System:    system (tipLevel)
/// - Role::Interrupt: interrupt (中断输入项数据)
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
        Thinking,
        System,
        Tool,
        /// 中断输入项消息 (内嵌交互控件, 直接渲染在消息列表中)
        Interrupt
    };
    /// 提示消息级别 (System 提示消息使用, 与 agentxx::agent::Delta::TipType 对应)
    enum class TipLevel : uint8_t {
        Info,
        Warning,
        Error
    };
    /// 中断输入项状态 (Role::Interrupt 消息使用)
    enum class InterruptStatus : uint8_t {
        /// 等待用户操作 (可交互)
        Waiting,
        /// 已确认 (interruptResult 保存结果)
        Confirmed,
        /// 已取消 (用户主动取消整个中断请求)
        Cancelled,
        /// 已过期 (server 通知中断超时/会话取消, 不再可交互)
        Expired
    };

    // ---- 通用字段 (所有 role) ----
    /// 历史消息 id (appendHistory 分配); 客户端本地消息 (如 TUI 中断消息) 可为空
    std::string id;
    Role        role = Role::User;
    /// 正文: User/Assistant/Thinking/System 消息文本; Tool 消息为工具参数
    /// (arguments JSON 字符串, 与渲染侧现有约定一致)
    std::string text;
    int64_t     startTimeMs = 0; ///< 开始时间戳 (毫秒, Unix 时间戳)
    int64_t     durationMs  = 0; ///< 运行时长 (毫秒)
    bool        collapsed   = false; ///< 折叠展示 (Thinking/Tool 消息)

    // ---- Role::Tool 专属 ----
    struct ToolData {
        std::string toolName;
        std::string toolCallId;
        std::string toolResult;
        /// edit 工具参数 unified diff (server 生成预留; 当前渲染端自行计算, 未消费)
        std::string diff;
        bool        toolFinished = false;
    };
    // ---- Role::System 专属 ----
    struct SystemData {
        TipLevel tipLevel = TipLevel::Info;
    };
    // ---- Role::Interrupt 专属 ----
    struct InterruptData {
        /// 中断请求 wire id (对应 WireInterruptRequest.id); 0 = 非中断消息
        int64_t interruptId = 0;
        /// 输入项描述 (InterruptHandleInputItem 字段)
        std::string inputLabel;
        std::string inputDepict;
        /// 返回值类型: bool / int / double / string / enum
        std::string inputType;
        std::string inputDefault;
        std::vector<std::string> inputEnums;
        /// 输入项序号 (1-based) / 总数 (仅进度展示)
        int inputIndex = 0;
        int inputTotal = 0;
        /// 中断输入项状态
        InterruptStatus interruptStatus = InterruptStatus::Waiting;
        /// 确认结果 (interruptStatus == Confirmed 时有效)
        std::string interruptResult;
    };

    std::optional<ToolData>      tool;      ///< Role::Tool 有效
    std::optional<SystemData>    system;    ///< Role::System 有效
    std::optional<InterruptData> interrupt; ///< Role::Interrupt 有效

    /// 便捷构造: 纯文本消息 (User/Assistant/Thinking/System)
    /// - System 消息自动创建 system 子结构 (tipLevel 默认 Info)
    static ViewMessage makeText(
        Role        role,
        std::string text,
        int64_t     startTimeMs = 0,
        int64_t     durationMs  = 0
    ) {
        ViewMessage m;
        m.role        = role;
        m.text        = std::move(text);
        m.startTimeMs = startTimeMs;
        m.durationMs  = durationMs;
        if (role == Role::System) {
            m.system = SystemData{};
        }
        return m;
    }

    /// 序列化为 wire/哈希 JSON (角色专属字段按 role 输出)
    neograph::json toJson() const;
    /// 从 wire/哈希 JSON 解析; 非法 role 或缺省字段时按默认值解析。
    /// 保证 role 专属子结构在对应 role 下非空 (Tool/System/Interrupt)
    static ViewMessage fromJson(const neograph::json& j);
};

/// 加载组件通知：显示加载的 MCP/Skill/Memory 信息
struct AppendComponentNotification {
    enum class Type : uint8_t {
        Mcp,    // MCP 工具
        Skill,  // Skill
        Memory, // Memory 文件
    };

    Type        type;
    std::string name;         // 名称 (MCP 命名空间 / Skill 名 / Memory 文件名)
    bool        success;      // 是否加载成功
    std::string errorMessage; // 失败时的错误信息
};

class ChainHash {
public:

    void append(std::string_view serialized);
    void reset();

    uint64_t    tail() const;
    uint64_t    count() const;
    std::string tailHex() const;

private:

    uint64_t hash_  = 0;
    uint64_t count_ = 0;
};

struct Delta {
    /// 提示消息级别 (MessageTip 使用)
    enum class TipType : uint8_t {
        Info,    ///< 普通提示
        Warning, ///< 警告
        Error,   ///< 错误
    };

    enum class Type : uint8_t {
        TextToken,
        ThinkingToken,
        ToolStart,
        ToolEnd,
        TurnStart,
        TurnEnd,
        NodeStart,
        NodeEnd,
        MessageTip, ///< 通用提示消息 (info/warning/error, UI 插入提示消息)
    };

    Type     type;
    uint64_t seq = 0;

    std::string text;

    std::string msgId;
    std::string toolName;
    std::string toolCallId;
    std::string arguments;

    std::string result;
    bool        hasError = false;

    std::string nodeName;

    // MessageTip: 通用提示消息 (文本复用 text 字段)
    TipType tipType = TipType::Info; ///< 提示级别 (Info/Warning/Error)

    uint64_t    historyCount = 0;
    std::string tailHash;

    // 运行时长统计
    int64_t startTimeMs = 0; // 开始时间戳 (毫秒)
    int64_t durationMs  = 0; // 运行时长 (毫秒)
};

struct SyncPayload {
    uint64_t             fromIndex = 0;
    std::vector<ViewMessage> messages;
    std::string          tailHash;
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
        case R::Thinking:
            return "thinking";
        case R::System:
            return "system";
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
        return R::Thinking;
    }
    if (s == "system") {
        return R::System;
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

inline std::string_view viewMessageInterruptStatusToString(ViewMessage::InterruptStatus s) noexcept {
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

inline ViewMessage::InterruptStatus
    viewMessageInterruptStatusFromString(std::string_view s) noexcept {
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

inline neograph::json ViewMessage::toJson() const {
    neograph::json j = neograph::json::object();
    if (!id.empty()) {
        j["id"] = id;
    }
    j["role"]        = std::string(viewMessageRoleToString(role));
    j["text"]        = text;
    j["start_time_ms"] = startTimeMs;
    j["duration_ms"]  = durationMs;
    if (collapsed) {
        j["collapsed"] = true;
    }
    if (tool) {
        neograph::json t = neograph::json::object();
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
    if (system) {
        j["system"] = neograph::json{
            {"tip_level", std::string(viewMessageTipLevelToString(system->tipLevel))},
        };
    }
    if (interrupt) {
        neograph::json it = neograph::json::object();
        it["interrupt_id"] = interrupt->interruptId;
        if (!interrupt->inputLabel.empty()) {
            it["input_label"] = interrupt->inputLabel;
        }
        if (!interrupt->inputDepict.empty()) {
            it["input_depict"] = interrupt->inputDepict;
        }
        if (!interrupt->inputType.empty()) {
            it["input_type"] = interrupt->inputType;
        }
        if (!interrupt->inputDefault.empty()) {
            it["input_default"] = interrupt->inputDefault;
        }
        if (!interrupt->inputEnums.empty()) {
            neograph::json arr = neograph::json::array();
            for (const auto& e : interrupt->inputEnums) {
                arr.push_back(e);
            }
            it["input_enums"] = std::move(arr);
        }
        it["input_index"] = interrupt->inputIndex;
        it["input_total"] = interrupt->inputTotal;
        it["interrupt_status"]
            = std::string(viewMessageInterruptStatusToString(interrupt->interruptStatus));
        if (!interrupt->interruptResult.empty()) {
            it["interrupt_result"] = interrupt->interruptResult;
        }
        j["interrupt"] = std::move(it);
    }
    return j;
}

inline ViewMessage ViewMessage::fromJson(const neograph::json& j) {
    ViewMessage m;
    m.id          = j.value("id", std::string{});
    m.text        = j.value("text", std::string{});
    m.startTimeMs = j.value("start_time_ms", int64_t{0});
    m.durationMs  = j.value("duration_ms", int64_t{0});
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
        case ViewMessage::Role::System: {
            ViewMessage::SystemData s;
            if (j.contains("system")) {
                s.tipLevel = viewMessageTipLevelFromString(j["system"].value("tip_level", std::string{}));
            }
            m.system = std::move(s);
            break;
        }
        case ViewMessage::Role::Interrupt: {
            ViewMessage::InterruptData it;
            if (j.contains("interrupt")) {
                const auto& ij       = j["interrupt"];
                it.interruptId       = ij.value("interrupt_id", int64_t{0});
                it.inputLabel        = ij.value("input_label", std::string{});
                it.inputDepict       = ij.value("input_depict", std::string{});
                it.inputType         = ij.value("input_type", std::string{});
                it.inputDefault      = ij.value("input_default", std::string{});
                it.inputIndex        = ij.value("input_index", 0);
                it.inputTotal        = ij.value("input_total", 0);
                it.interruptStatus   = viewMessageInterruptStatusFromString(
                    ij.value("interrupt_status", std::string{})
                );
                it.interruptResult   = ij.value("interrupt_result", std::string{});
                if (ij.contains("input_enums") && ij["input_enums"].is_array()) {
                    for (const auto& e : ij["input_enums"]) {
                        it.inputEnums.push_back(e.is_string() ? e.get<std::string>() : std::string{});
                    }
                }
            }
            m.interrupt = std::move(it);
            break;
        }
        case ViewMessage::Role::User:
        case ViewMessage::Role::Assistant:
        case ViewMessage::Role::Thinking:
            break;
    }
    return m;
}

} // namespace agent
} // namespace agentxx
