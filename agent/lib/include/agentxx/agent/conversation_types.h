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

class ChainHash {
public:

    void append(const std::string& serialized);
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
};

struct SyncPayload {
    uint64_t                    fromIndex = 0;
    std::vector<HistoryMessage> messages;
    std::string                 tailHash;
};

} // namespace agent
} // namespace agentxx
