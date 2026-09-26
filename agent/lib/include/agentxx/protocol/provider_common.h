/// 各 LLM Provider (OpenAI Chat Completions / OpenAI Responses / Anthropic) 与
/// 模型调用节点共用的工具函数
///
/// - 这些函数原先在 openai_provider.cpp / anthropic_provider.cpp /
///   modelcall.cpp 各存一份拷贝, 收敛到这里避免实现细节漂移
/// - 只放与具体协议无关的通用逻辑; 协议特化 (请求体组装/SSE 解析/错误提取等)
///   仍留在各自 provider 内
#pragma once

#include "agentxx/util/neograph_json_bridge.h"
#include "fmt/format.h"
#include "neograph/api.h"
#include "neograph/provider.h"
#include "utilxx/http_header.h"
#include <chrono>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agentxx {
namespace protocol {

/// 会话 ID 关联请求头键名
inline constexpr std::string_view kHeaderSessionId       = "X-Session-Id";
inline constexpr std::string_view kHeaderOpencodeSession = "X-Opencode-Session";

/// CompletionParams.extra_fields 中传递会话 ID 的保留键名
inline constexpr std::string_view kExtraFieldSessionId = "session_id";

/// 从 CompletionParams 中提取本次会话的 sessionId
///
/// 提取优先级:
/// 1. extra_fields["session_id"]
/// 2. extra_fields["sessionId"]
/// 3. extra_fields["X-Session-Id"]
/// 4. extra_fields["X-Opencode-Session"]
///
/// - `args`:
///     - [params] 补全参数
///
/// - `return` 本次会话的 sessionId 字符串, 未提取到时返回空
inline std::string extractSessionId(const neograph::CompletionParams& params) {
    if (!params.extra_fields.is_object() || params.extra_fields.empty()) {
        return {};
    }
    static constexpr std::string_view candidateKeys[] = {
        "session_id",
        "sessionId",
        "X-Session-Id",
        "X-Opencode-Session",
    };
    for (const auto& key : candidateKeys) {
        const std::string keyStr{key};
        if (params.extra_fields.contains(keyStr)) {
            auto valStr = params.extra_fields.value(keyStr, "");
            if (!valStr.empty()) {
                return valStr;
            }
            auto child = params.extra_fields[keyStr];
            if (child.is_number()) {
                return child.dump();
            }
        }
    }
    return {};
}

/// 判定 extra_fields 中的键是否为内部控制字段 (不应序列化进向 upstream LLM 发送的 JSON 请求体中)
///
/// - `args`:
///     - [key] 待检查的键名
///
/// - `return` true 表示为内部保留控制键, 应在组装 request body 时跳过
inline bool isInternalExtraField(std::string_view key) {
    return key == "session_id" || key == "sessionId" || key == kHeaderSessionId
           || key == kHeaderOpencodeSession;
}

/// 向请求头中设置本次会话的 sessionId
///
/// 按照规范使用两个 key:
/// - X-Session-Id
/// - X-Opencode-Session
/// 相同都携带 sessionId 作为 value
///
/// - `args`:
///     - [headers] 待填充的请求头映射
///     - [sessionId] 本次会话的 sessionId, 为空时不添加
inline void applySessionHeaders(utilxx::HeaderMap& headers, std::string_view sessionId) {
    if (!sessionId.empty()) {
        headers.set(kHeaderSessionId, sessionId);
        headers.set(kHeaderOpencodeSession, sessionId);
    }
}

/// 向请求头中设置由 CompletionParams 提取的本次会话 sessionId
///
/// - `args`:
///     - [headers] 待填充的请求头映射
///     - [params] 补全参数
inline void
    applySessionHeaders(utilxx::HeaderMap& headers, const neograph::CompletionParams& params) {
    applySessionHeaders(headers, extractSessionId(params));
}

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

/// 把 typed 上下文直接组装为 OpenAI Chat Completions 请求体的 `messages` 数组
///
/// - 与 [neograph::messages_to_json] 的输出逐字段一致: 角色 / 正文 / tool_calls /
///   tool 应答 / 多模态分片顺序均相同, 只是直接产出 `utilxx_base::Json`,
///   省掉 "typed -> neograph json -> utilxx Json" 里的一次整段 DOM 拷贝
///   (每轮请求都会发生, 长上下文时是主要的内存与分配量来源之一)
/// - 二者必须保持一致: `/test` 的 `openai_provider` 模块有逐字节对照用例
///
/// - `args`:
///     - [messages] 会话上下文 (typed)
///     - [sendThinking] 是否回传明文思考 (`reasoning_content`);
///       关闭时不写入该字段, 等价于旧实现的"构造后逐条删除该键"
///
/// - `return` OpenAI messages 数组
inline utilxx_base::Json chatMessagesToOpenAIJson(
    const std::vector<neograph::ChatMessage>& messages,
    bool                                      sendThinking
) {
    auto arr = utilxx_base::Json::array();
    for (const auto& msg : messages) {
        utilxx_base::Json j;
        j["role"] = msg.role;

        if (msg.role == "tool") {
            j["content"]       = msg.content;
            j["tool_call_id"]  = msg.tool_call_id;
        } else if (!msg.tool_calls.empty()) {
            // 带工具调用的助手消息: 无正文时 content 为 null (与 neograph 一致)
            j["content"] = msg.content.empty() ? utilxx_base::Json{} : utilxx_base::Json(msg.content);
            auto tcArr   = utilxx_base::Json::array();
            for (const auto& tc : msg.tool_calls) {
                tcArr.push_back(utilxx_base::Json{
                    {"id", utilxx_base::Json(tc.id)},
                    {"type", utilxx_base::Json("function")},
                    {"function",
                     utilxx_base::Json{
                         {"name", utilxx_base::Json(tc.name)},
                         {"arguments", utilxx_base::Json(tc.arguments)},
                     }},
                });
            }
            j["tool_calls"] = std::move(tcArr);
        } else if (!msg.image_urls.empty() || !msg.audio_urls.empty() || !msg.video_urls.empty()) {
            // 多模态: 文本 + 图片/音频/视频 (OpenAI 分片格式)
            auto parts = utilxx_base::Json::array();
            if (!msg.content.empty()) {
                parts.push_back(utilxx_base::Json{
                    {"type", utilxx_base::Json("text")},
                    {"text", utilxx_base::Json(msg.content)},
                });
            }
            for (const auto& url : msg.image_urls) {
                parts.push_back(utilxx_base::Json{
                    {"type", utilxx_base::Json("image_url")},
                    {"image_url", utilxx_base::Json{{"url", utilxx_base::Json(url)}}},
                });
            }
            for (const auto& url : msg.audio_urls) {
                if (auto parsed = neograph::parse_data_url(url)) {
                    // data URL 可拆成 base64 数据 + 格式; HTTP URL 拆不出, 走 url 字段
                    parts.push_back(utilxx_base::Json{
                        {"type", utilxx_base::Json("input_audio")},
                        {"input_audio",
                         utilxx_base::Json{
                             {"data", utilxx_base::Json(parsed->second)},
                             {"format",
                              utilxx_base::Json(neograph::media_format_from_mime(parsed->first))},
                         }},
                    });
                } else {
                    parts.push_back(utilxx_base::Json{
                        {"type", utilxx_base::Json("input_audio")},
                        {"input_audio", utilxx_base::Json{{"url", utilxx_base::Json(url)}}},
                    });
                }
            }
            for (const auto& url : msg.video_urls) {
                parts.push_back(utilxx_base::Json{
                    {"type", utilxx_base::Json("video_url")},
                    {"video_url", utilxx_base::Json{{"url", utilxx_base::Json(url)}}},
                });
            }
            j["content"] = std::move(parts);
        } else {
            j["content"] = msg.content;
        }

        if (msg.role == "assistant") {
            if (!msg.reasoning_details.empty()) {
                if (!msg.reasoning_details.is_array()) {
                    throw std::invalid_argument("ChatMessage reasoning_details must be an array");
                }
                j["reasoning_details"] = agentxx::util::fromNeographJson(msg.reasoning_details);
            } else if (sendThinking && !msg.reasoning_content.empty()) {
                j["reasoning_content"] = msg.reasoning_content;
            }
        }

        arr.push_back(std::move(j));
    }
    return arr;
}

/// 把 typed 工具定义直接组装为 OpenAI Chat Completions 请求体的 `tools` 数组
///
/// - 与 [neograph::tools_to_json] 的输出逐字段一致, 同样省掉一次整段 DOM 拷贝
/// - 无参数 schema 的工具回退为空对象 schema (严格校验的网关会拒绝
///   `"parameters": null`)
///
/// - `args`:
///     - [tools] 工具定义列表
///
/// - `return` OpenAI tools 数组
inline utilxx_base::Json chatToolsToOpenAIJson(const std::vector<neograph::ChatTool>& tools) {
    auto arr = utilxx_base::Json::array();
    for (const auto& tool : tools) {
        const neograph::json& params
            = tool.parameters.is_object() ? tool.parameters : neograph::json::object();
        arr.push_back(utilxx_base::Json{
            {"type", utilxx_base::Json("function")},
            {"function",
             utilxx_base::Json{
                 {"name", utilxx_base::Json(tool.name)},
                 {"description", utilxx_base::Json(tool.description)},
                 {"parameters", agentxx::util::fromNeographJson(params)},
             }},
        });
    }
    return arr;
}

/// 判定是否为"有效空响应": content / 明文思考 / tool_calls 全空, 且无加密思考载体
/// - 加密思考载体存于 `message.extra[carrierKey]`, 是供应商网关只回传思考密文
///   (summary/content 均空) 时唯一的有效载体; 数组非空即视为有载体
/// - 空响应对 Agent 而言等于本次生成失败: 无内容可展示、无 tool_calls 可路由,
///   由调用方抛出异常, 经 modelcall 重试流程自动重试并提示 UI
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

} // namespace protocol
} // namespace agentxx
