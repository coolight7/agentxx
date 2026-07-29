#pragma once

#include "neograph/json.h"
#include <cstdint>
#include <string>
#include <vector>

namespace agentxx {
namespace agent {

struct HistoryMessage {
    std::string    id;
    neograph::json data;
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
    enum class Type : uint8_t {
        TextToken,
        ThinkingToken,
        ToolStart,
        ToolEnd,
        TurnStart,
        TurnEnd,
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

    uint64_t    historyCount = 0;
    std::string tailHash;

    // 运行时统计 (仅 TurnEnd 使用)
    int32_t startTimeMs = 0; // 开始时间戳 (毫秒)
    int32_t durationMs  = 0; // 运行时长 (毫秒)
};

struct SyncPayload {
    uint64_t                    fromIndex = 0;
    std::vector<HistoryMessage> messages;
    std::string                 tailHash;
};

} // namespace agent
} // namespace agentxx
