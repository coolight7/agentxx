/// 各 LLM Provider (OpenAI Chat Completions / OpenAI Responses / Anthropic) 与
/// 模型调用节点共用的工具函数
///
/// - 这些函数原先在 openai_provider.cpp / anthropic_provider.cpp /
///   modelcall.cpp 各存一份拷贝, 收敛到这里避免实现细节漂移
/// - 只放与具体协议无关的通用逻辑; 协议特化 (请求体组装/SSE 解析/错误提取等)
///   仍留在各自 provider 内
#pragma once

#include "fmt/format.h"
#include "neograph/api.h"
#include <chrono>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>

namespace agentxx {
namespace server {

/// 生成唯一的 tool_call id: 毫秒时间戳 + 32 位随机数
/// - 无需与已有 id 比较, 碰撞概率 ~2^-32 (同一毫秒内), 跨毫秒必然不同
/// - 相比按下标回填 call_{i}, 不会与 LLM 返回的 call_N 形式 id 冲突
///
/// - `args`:
///     - [index] 同一毫秒内仍需要区分多个 id 时使用的序号 (默认 0)
///
/// - `return` 形如 `call_{毫秒时间戳}_{index}_{8位hex随机数}` 的 id
inline std::string makeUniqueToolCallId(size_t index = 0) {
    thread_local std::mt19937_64 rng{
        static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())
    };
    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()
    )
                        .count();
    return fmt::format("call_{}_{}_{:08x}", ts, index, static_cast<uint32_t>(rng()));
}

/// 判定是否为"有效空响应": content / 明文思考 / tool_calls 全空, 且无加密思考载体
/// - 加密思考载体存于 `message.extra[carrierKey]`, 是供应商网关只回传思考密文
///   (summary/content 均空) 时唯一的有效载体; 数组非空即视为有载体
/// - 空响应对 Agent 而言等于本次生成失败: 无内容可展示、无 tool_calls 可路由,
///   由调用方抛出异常, 经 modelcall 重试链路自动重试并提示 UI
///
/// - `args`:
///     - [completion] 待判定的补全结果
///     - [carrierKey] 加密思考载体在 `message.extra` 中的键
///       (OpenAI Responses: [OpenAIProvider::kResponsesReasoningItemsKey];
///        Anthropic: [AnthropicProvider::kThinkingBlocksKey])
///
/// - `return` true 表示本次响应无任何有效内容 (应视为失败)
inline bool
    isEmptyResponse(const neograph::ChatCompletion& completion, std::string_view carrierKey) {
    const auto& msg = completion.message;
    if (!msg.content.empty() || !msg.reasoning_content.empty() || !msg.tool_calls.empty()) {
        return false;
    }
    const std::string key{carrierKey};
    return !(msg.extra.contains(key) && msg.extra[key].is_array() && !msg.extra[key].empty());
}

} // namespace server
} // namespace agentxx
