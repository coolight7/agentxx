#pragma once

#include "agentxx/agent/config.h"
#include "agentxx/protocol/provider_common.h"
#include "agentxx/util/exception.h"
#include "asio/awaitable.hpp"
#include "asio/use_awaitable.hpp"
#include "utilxx/http_client.h"
#include "utilxx_base/json_view.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"
#include <charconv>
#include <chrono>
#include <map>
#include <memory>
#include <neograph/api.h>
#include <neograph/provider.h>
#include <string>
#include <vector>

namespace agentxx {
namespace protocol {

/// Anthropic Messages API provider (非流式/流式/tool_use/扩展思考)
/// 文档: https://vercel.com/docs/ai-gateway/sdks-and-apis/anthropic-messages-api
class AnthropicProvider : public neograph::Provider {
public:

    /// ChatMessage.extra 中保存带 signature 的 thinking/redacted_thinking 原始块的键。
    /// Anthropic 要求多轮对话回传 thinking 块时携带响应中的原始 signature, 故解析响应时
    /// 按序存入 extra, convertMessages 时原样回传。
    static constexpr const char* kThinkingBlocksKey = "anthropic_thinking_blocks";

    static std::unique_ptr<AnthropicProvider> create(const agentxx::agent::ModelConfig& config);

    static std::shared_ptr<neograph::Provider>
        create_shared(const agentxx::agent::ModelConfig& config);

    ~AnthropicProvider() override = default;

    std::string get_name() const override;

    asio::awaitable<neograph::ChatCompletion> invoke(
        const neograph::CompletionParams& params,
        neograph::StreamCallback          on_chunk = nullptr
    ) override;

    asio::awaitable<neograph::ChatCompletion> invoke_format_data(
        const neograph::CompletionParams&  params,
        neograph::FormatDataStreamCallback on_chunk = nullptr
    ) override;

    // --- 公开静态工具函数 (暴露供单元测试) ---

    /// 将 neograph 消息转换为 Anthropic 格式
    /// - `return` {system_string, messages_json_array}
    /// - [sendThinking] 是否携带 thinking 内容块
    static std::pair<std::string, utilxx_base::Json> convertMessages(
        const std::vector<neograph::ChatMessage>& messages,
        bool                                      sendThinking = false
    );

    /// 将 neograph 工具定义转换为 Anthropic 格式
    static utilxx_base::Json convertTools(const std::vector<neograph::ChatTool>& tools);

    /// 组装 Anthropic 用量 (计划 LLM-8; 非流式与流式两条路径共用的唯一入口)
    ///
    /// 上游语义与 OpenAI 不同: `input_tokens` **只统计未命中缓存的输入**,
    /// 命中缓存的读取量与写入量分别是 `cache_read_input_tokens` /
    /// `cache_creation_input_tokens`。这里统一折算成与其他 provider 一致的
    /// `prompt_tokens` (整段 prompt 的规模), 并把缓存读/写量分别写到
    /// `usage.cached_prompt_tokens` 与补充用量额外通道 (见 provider_common.h 的
    /// [agentxx::protocol::setUsageDetail]), 供模型调用节点写入用量记录。
    ///
    /// - `args`:
    ///     - [completion] 待填充的补全结果
    ///     - [inputTokens] `input_tokens` (未命中缓存的输入)
    ///     - [cacheReadTokens] `cache_read_input_tokens` (命中缓存读取)
    ///     - [cacheWriteTokens] `cache_creation_input_tokens` (写入缓存)
    ///     - [outputTokens] `output_tokens`; < 0 表示本次事件未携带 (保持原值)
    static void applyUsage(
        neograph::ChatCompletion& completion,
        int                       inputTokens,
        int                       cacheReadTokens,
        int                       cacheWriteTokens,
        int                       outputTokens
    ) {
        completion.usage.prompt_tokens = inputTokens + cacheReadTokens + cacheWriteTokens;
        if (outputTokens >= 0) {
            completion.usage.completion_tokens = outputTokens;
        }
        completion.usage.total_tokens
            = completion.usage.prompt_tokens + completion.usage.completion_tokens;
        completion.usage.cached_prompt_tokens = cacheReadTokens;
        agentxx::protocol::setUsageDetail(
            completion,
            agentxx::protocol::UsageDetail{cacheReadTokens, cacheWriteTokens}
        );
    }

    /// 在请求体里加 prompt 缓存断点 (计划 LLM-8; 仅 `ModelConfig::cacheControl` 打开时调用)
    ///
    /// 断点加在**稳定前缀**的末尾, 使上游能把这些内容缓存下来供后续请求命中:
    /// ① 系统提示 (system 由字符串转内容块数组, 末块带断点);
    /// ② 工具定义 (最后一个工具带断点);
    /// ③ 最后一条**非请求期插入**的消息 —— 请求末尾的动态段
    ///   (`<dynamic_context ...>`, `MessageFlag::AutoInserted`) 每轮都可能变化,
    ///   断点必须落在它之前, 否则每轮都写新缓存而读不到旧缓存。
    ///
    /// Anthropic 单请求最多 4 个断点, 这里最多用 3 个; 上游不支持该字段时应保持
    /// 关闭 (默认关闭, 见 `ModelConfig::cacheControl`)。
    static void applyCacheBreakpoints(utilxx_base::Json& body);

    /// 解析非流式 Anthropic 响应
    static neograph::ChatCompletion parseResponse(const utilxx_base::Json& resp);

    /// 向 completion.message.extra[kThinkingBlocksKey] 追加一个 thinking 相关块
    /// (thinking/redacted_thinking), 首次追加时初始化为数组
    static void
        appendThinkingBlock(neograph::ChatCompletion& completion, const utilxx_base::Json& block);

    // (实现见 anthropic_provider.cpp: 经 bridge 转入 message.extra(neograph::json))

    /// 解析 Anthropic SSE 响应缓冲
    /// - 事件分隔符同时支持 "\n\n" 与 "\r\n\r\n" (SSE 规范允许 \r\n 行结尾)
    /// - thinkingTexts/blockSignatures: 按 block index 累积 thinking 文本与 signature,
    ///   content_block_stop 时组装为带 signature 的 thinking 块存入 completion.message.extra
    /// - finalFlush: 连接关闭时对末尾未以 "\n\n" 结尾的最后一个事件块也进行解析
    /// - 返回本次调用是否处理到了 "message_stop" 结束事件 (用于检测流截断)
    static bool processSseBuffer(
        std::string&                       buf,
        neograph::ChatCompletion&          completion,
        std::string&                       fullContent,
        std::string&                       fullThinking,
        std::map<int, neograph::ToolCall>& tcMap,
        std::map<int, std::string>&        blockTypes,
        std::map<int, std::string>&        thinkingTexts,
        std::map<int, std::string>&        blockSignatures,
        neograph::FormatDataStreamCallback on_chunk,
        bool                               finalFlush = false
    ) {
        bool done = false;
        while (true) {
            // SSE 规范允许 \n 或 \r\n 行结尾, 事件分隔符相应可能是 "\n\n" 或 "\r\n\r\n",
            // 取最先出现者为界
            auto   posLf   = buf.find("\n\n");
            auto   posCrlf = buf.find("\r\n\r\n");
            size_t pos, sepLen;
            if (posCrlf != std::string::npos && (posLf == std::string::npos || posCrlf < posLf)) {
                pos    = posCrlf;
                sepLen = 4;
            } else if (posLf != std::string::npos) {
                pos    = posLf;
                sepLen = 2;
            } else {
                break;
            }
            std::string block = buf.substr(0, pos);
            buf.erase(0, pos + sepLen);
            done |= processSseBlock(
                block,
                completion,
                fullContent,
                fullThinking,
                tcMap,
                blockTypes,
                thinkingTexts,
                blockSignatures,
                on_chunk
            );
        }
        if (finalFlush && !buf.empty()) {
            // 连接 abrupt 关闭时, 最后一个事件可能没有 trailing "\n\n", 此处补解析
            std::string block = std::move(buf);
            buf.clear();
            done |= processSseBlock(
                block,
                completion,
                fullContent,
                fullThinking,
                tcMap,
                blockTypes,
                thinkingTexts,
                blockSignatures,
                on_chunk
            );
        }
        return done;
    }

    /// 解析单个 SSE 事件块 (以 "\n\n" 分隔的一块, 含若干 event:/data: 行)
    /// 返回该事件块是否为 "message_stop" 结束事件
    static bool processSseBlock(
        std::string_view                   block,
        neograph::ChatCompletion&          completion,
        std::string&                       fullContent,
        std::string&                       fullThinking,
        std::map<int, neograph::ToolCall>& tcMap,
        std::map<int, std::string>&        blockTypes,
        std::map<int, std::string>&        thinkingTexts,
        std::map<int, std::string>&        blockSignatures,
        neograph::FormatDataStreamCallback on_chunk
    ) {
        std::string currentEvent;
        std::string payload;

        size_t lineStart = 0;
        while (lineStart < block.size()) {
            auto        lineEnd = block.find('\n', lineStart);
            std::string line{
                (lineEnd == std::string::npos) ? block.substr(lineStart)
                                               : block.substr(lineStart, lineEnd - lineStart)
            };
            lineStart = (lineEnd == std::string::npos) ? block.size() : lineEnd + 1;

            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            // SSE 规范: 字段名后冒号之后的单个前导空格可选; 多个 data: 行以 "\n" 拼接
            if (line.rfind("event:", 0) == 0) {
                auto val = line.substr(6);
                if (!val.empty() && val.front() == ' ') {
                    val.erase(0, 1);
                }
                currentEvent = val;
            } else if (line.rfind("data:", 0) == 0) {
                auto val = line.substr(5);
                if (!val.empty() && val.front() == ' ') {
                    val.erase(0, 1);
                }
                if (!payload.empty()) {
                    payload += "\n";
                }
                payload += val;
            }
        }

        if (payload.empty()) {
            return false;
        }

        // 高频路径: JsonView 零拷贝路由 (§4.3) + 命中后按需物化
        // - View 仅做只读导航 (event/usage/delta 标量提取无 DOM 堆分配)
        // - 仅 thinking/redacted 块组装需要 Json DOM (appendThinkingBlock 物化)
        utilxx_base::JsonView jv;
        bool                  parsed = agentxx::util::catchError<bool>(
            [&]() -> bool {
                jv = utilxx_base::JsonView::parse(payload);
                return true;
            },
            [](std::string) -> bool {
                return false;
            }
        );
        if (!parsed || !jv.is_object()) {
            return false;
        }
        auto viewStr = [](const utilxx_base::JsonView& v) -> std::string {
            if (!v.valid() || !v.is_string()) {
                return {};
            }
            try {
                return std::string(v.get_string_view());
            } catch (...) {
                return {};
            }
        };
        auto viewInt = [](const utilxx_base::JsonView& obj, std::string_view key, int def) {
            if (!obj.is_object()) {
                return def;
            }
            auto v = obj[key];
            if (!v.valid() || v.is_null()) {
                return def;
            }
            try {
                if (v.is_int64()) {
                    return static_cast<int>(v.get_int64());
                }
                if (v.is_uint64()) {
                    return static_cast<int>(v.get_uint64());
                }
                if (v.is_double()) {
                    return static_cast<int>(v.get_double());
                }
            } catch (...) {
            }
            return def;
        };

        // 允许异常时字节抛出给到 ModelCallNode ，以便自动处理
        if (currentEvent == "message_start") {
            auto msgView = jv["message"];
            if (msgView.valid() && msgView.is_object()) {
                auto usageView = msgView["usage"];
                if (usageView.valid() && usageView.is_object()) {
                    // 用量计算方式 (计划 LLM-8): input_tokens 只含未命中缓存的输入,
                    // 缓存读/写量单独回报, 统一由 [applyUsage] 折算成 prompt_tokens
                    applyUsage(
                        completion,
                        viewInt(usageView, "input_tokens", 0),
                        viewInt(usageView, "cache_read_input_tokens", 0),
                        viewInt(usageView, "cache_creation_input_tokens", 0),
                        -1
                    );
                }
            }
        } else if (currentEvent == "content_block_start") {
            int  idx    = viewInt(jv, "index", 0);
            auto cbView = jv["content_block"];
            if (cbView.valid() && cbView.is_object()) {
                std::string type;
                {
                    auto tv = cbView["type"];
                    if (tv.valid() && tv.is_string()) {
                        type = viewStr(tv);
                    }
                }
                blockTypes[idx] = type;
                if (type == "tool_use") {
                    // 工具调用开始 (见 protocol/provider_common.h): tool_use 块声明即
                    // 通知 UI (此时参数增量尚未到达, 内容不完整无法展示)
                    const bool firstFragment = (tcMap.find(idx) == tcMap.end());
                    tcMap[idx].id            = viewStr(cbView["id"]);
                    tcMap[idx].name          = viewStr(cbView["name"]);
                    if (firstFragment && on_chunk) {
                        on_chunk(neograph::ChatStreamChunk{
                            chunk_type::kToolCallStart,
                            tcMap[idx].name,
                        });
                    }
                } else if (type == "redacted_thinking") {
                    // redacted_thinking 块必须在多轮对话中原样回传 (命中后物化)
                    utilxx_base::Json b;
                    b["type"] = "redacted_thinking";
                    b["data"] = viewStr(cbView["data"]);
                    appendThinkingBlock(completion, std::move(b));
                }
            }
        } else if (currentEvent == "content_block_delta") {
            int  idx       = viewInt(jv, "index", 0);
            auto deltaView = jv["delta"];
            if (deltaView.valid() && deltaView.is_object()) {
                std::string deltaType = viewStr(deltaView["type"]);
                if (deltaType == "text_delta") {
                    auto text    = viewStr(deltaView["text"]);
                    fullContent += text;
                    if (on_chunk) {
                        on_chunk(neograph::ChatStreamChunk{
                            neograph::ChatStreamChunk::TYPE_CONTENT,
                            text,
                        });
                    }
                } else if (deltaType == "thinking_delta") {
                    auto thinking       = viewStr(deltaView["thinking"]);
                    fullThinking       += thinking;
                    thinkingTexts[idx] += thinking;
                    if (on_chunk) {
                        on_chunk(neograph::ChatStreamChunk{
                            neograph::ChatStreamChunk::TYPE_THINKING,
                            thinking
                        });
                    }
                } else if (deltaType == "signature_delta") {
                    // thinking 块的 signature, 多轮对话回传 thinking 时 Anthropic 要求携带
                    blockSignatures[idx] += viewStr(deltaView["signature"]);
                } else if (deltaType == "input_json_delta") {
                    auto partialJson      = viewStr(deltaView["partial_json"]);
                    tcMap[idx].arguments += partialJson;
                }
            }
        } else if (currentEvent == "content_block_stop") {
            int  idx = viewInt(jv, "index", 0);
            auto it  = blockTypes.find(idx);
            if (it != blockTypes.end() && it->second == "thinking") {
                auto sigIt = blockSignatures.find(idx);
                // 仅保存带 signature 的 thinking 块: 无 signature 的 thinking 回传会被 API 拒绝
                if (sigIt != blockSignatures.end() && !sigIt->second.empty()) {
                    utilxx_base::Json b;
                    b["type"]      = "thinking";
                    b["thinking"]  = thinkingTexts[idx];
                    b["signature"] = sigIt->second;
                    appendThinkingBlock(completion, std::move(b));
                }
            }
        } else if (currentEvent == "message_delta") {
            auto usageView = jv["usage"];
            if (usageView.valid() && usageView.is_object()) {
                // 缺失字段沿用已知值: output_tokens 缺失保持原值 (与原行为一致),
                // input_tokens / 缓存量缺失时沿用 message_start 报过的值 (计划 LLM-8)
                const auto detail = agentxx::protocol::readUsageDetail(completion);
                const int  knownInputTokens
                    = completion.usage.prompt_tokens
                      - static_cast<int>(completion.usage.cached_prompt_tokens)
                      - static_cast<int>(detail.cacheWriteTokens);
                const int inputTokens = viewInt(usageView, "input_tokens", knownInputTokens);
                const int cacheRead   = viewInt(
                    usageView,
                    "cache_read_input_tokens",
                    static_cast<int>(completion.usage.cached_prompt_tokens)
                );
                const int cacheWrite = viewInt(
                    usageView,
                    "cache_creation_input_tokens",
                    static_cast<int>(detail.cacheWriteTokens)
                );
                int outputTokens = -1;
                if (usageView.contains("output_tokens")) {
                    outputTokens = viewInt(usageView, "output_tokens", 0);
                }
                applyUsage(completion, inputTokens, cacheRead, cacheWrite, outputTokens);
            }
        }
        return currentEvent == "message_stop";
    }

private:

    static constexpr std::string_view kDefaultBaseUrl{"https://api.anthropic.com"};

    /// 是否为请求末尾的动态段内容块 (`<dynamic_context ...>`; 见
    /// [applyCacheBreakpoints] 的说明), 仅供缓存断点定位使用
    static bool isDynamicContextBlock(const utilxx_base::Json& block);

    /// 在一段内容 (字符串或内容块数组) 末尾的可缓存块上加 `cache_control` 断点
    /// - thinking 块 (带 signature) 不接受该字段, 命中时继续往前找
    /// - `return` 是否成功加上断点
    static bool markCacheBreakpoint(utilxx_base::Json& content);

    explicit AnthropicProvider(agentxx::agent::ModelConfig config);

    /// 填充请求头: x-api-key + anthropic-version + extraHeaders + 会话 Header (X-Session-Id,
    /// X-Opencode-Session)
    void applyHeaders(utilxx::HeaderMap& headers, const neograph::CompletionParams& params) const;

    utilxx_base::Json buildBody(const neograph::CompletionParams& params) const;

    asio::awaitable<neograph::ChatCompletion> completeAsync(const neograph::CompletionParams& params
    );

    asio::awaitable<neograph::ChatCompletion> doStream(
        const neograph::CompletionParams&  params,
        const utilxx_base::Json&           body,
        neograph::FormatDataStreamCallback on_chunk
    );

    agentxx::agent::ModelConfig config_;
};

} // namespace protocol
} // namespace agentxx
