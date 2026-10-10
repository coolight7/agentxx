#include "agentxx/middlewares/summarization.h"
#include "agentxx/nodes/session_context.h"
#include "agentxx/util/neograph_json_bridge.h"

#include "agentxx/agent/agent_host.h"
#include "agentxx/agent/io/agent_io.h"
#include "agentxx/agent/io/agent_io_transport.h"
#include "agentxx/agent/model_registry.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/event/events.h"
#include "agentxx/tools/subagent.h"
#include "agentxx/util/exception.h"
#include "fmt/format.h"
#include "utilxx/async_offload.h"
#include "utilxx_base/string_util.h"
#include <algorithm>
#include <chrono>
#include <limits>
#include <set>
#include <sstream>

namespace agentxx {
namespace middleware {

namespace {

/// 判断是否为 AutoInserted 提示噪音消息 (连续出现时折叠)
bool isNoiseMessage(const neograph::ChatMessage& m) {
    if (!neograph::hasFlag(m.flags, neograph::MessageFlag::AutoInserted)) {
        return false;
    }
    if (!m.tool_calls.empty()) {
        return false;
    }
    const std::string_view c = m.content;
    return c == "[Please continue]" || c == "[User cancelled]" || c == "[Exception aborted]"
           || c == "[Empty]" || c.empty();
}

/// 将消息中的多模态 data URL (Base64) 降级为文本标记, 避免大量 Base64 消耗上下文 Token
void downgradeMultimodalUrlsToText(std::vector<neograph::ChatMessage>& messages) {
    for (auto& msg : messages) {
        if (!msg.image_urls.empty()) {
            for (const auto& url : msg.image_urls) {
                if (url.starts_with("data:")) {
                    msg.content += "\n[用户附带了图片]";
                }
            }
            msg.image_urls.clear();
        }
        if (!msg.audio_urls.empty()) {
            for (const auto& url : msg.audio_urls) {
                if (url.starts_with("data:")) {
                    msg.content += "\n[用户附带了音频]";
                }
            }
            msg.audio_urls.clear();
        }
        if (!msg.video_urls.empty()) {
            for (const auto& url : msg.video_urls) {
                if (url.starts_with("data:")) {
                    msg.content += "\n[用户附带了视频]";
                }
            }
            msg.video_urls.clear();
        }
    }
}

/// 判断两条消息是否完全等价 (用于相邻重复折叠)
bool isSameMessage(const neograph::ChatMessage& a, const neograph::ChatMessage& b) {
    if (a.role != b.role || a.content != b.content || a.tool_call_id != b.tool_call_id
        || a.tool_name != b.tool_name || a.reasoning_content != b.reasoning_content
        || a.image_urls != b.image_urls || a.audio_urls != b.audio_urls
        || a.video_urls != b.video_urls || a.tool_calls.size() != b.tool_calls.size()) {
        return false;
    }
    for (size_t i = 0; i < a.tool_calls.size(); ++i) {
        const auto& x = a.tool_calls[i];
        const auto& y = b.tool_calls[i];
        if (x.id != y.id || x.name != y.name || x.arguments != y.arguments) {
            return false;
        }
    }
    return true;
}

} // namespace

SummarizationMiddlewareHandle::SummarizationMiddlewareHandle(
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext,
    size_t                                      in_defaultModelSupportMaxToken,
    double                                      in_asciiCharsPerToken,
    double                                      in_unicodeCharsPerToken,
    double                                      in_tokensPerImage,
    double                                      in_extraTokensPerMessage,
    size_t                                      in_summaryMaxTokens
) :
    BaseMiddlewareHandle<_SummarizationMiddlewareState>(
        "SummarizationMiddlewareHandle",
        std::move(in_agentContext)
    ),
    modelSupportMaxTokenDefault(in_defaultModelSupportMaxToken),
    asciiCharsPerToken(in_asciiCharsPerToken),
    unicodeCharsPerToken(in_unicodeCharsPerToken),
    tokensPerImage(in_tokensPerImage),
    extraTokensPerMessage(in_extraTokensPerMessage),
    summaryMaxTokens(in_summaryMaxTokens) {
    assert(asciiCharsPerToken >= 0);
    assert(unicodeCharsPerToken >= 0);
    assert(tokensPerImage >= 0);
    assert(extraTokensPerMessage >= 0);

    // token 估算规则: 一份实例, 功能点核心实现与同步快路径共用
    // (规则本身在 agentxx::feature::TokenEstimator 里, 这里只注入本中间件的系数)
    tokenEstimator          = std::make_shared<agentxx::feature::TokenEstimator>();
    tokenEstimator->asciiCharsPerToken     = asciiCharsPerToken;
    tokenEstimator->unicodeCharsPerToken   = unicodeCharsPerToken;
    tokenEstimator->tokensPerImage         = tokensPerImage;
    tokenEstimator->extraTokensPerMessage  = extraTokensPerMessage;

    // 声明上下文两点 (token 估算 / 压缩) 并登记核心实现
    // - 中间件在插件装载前装配, 因此插件登记实现时点一定已存在
    // - 单测直接构造本中间件 (无 AgentContext) 时 featurePoints 为空, 走同步路径即可
    if (auto ctx = agentContext.lock(); ctx != nullptr && ctx->features != nullptr) {
        agentxx::feature::ContextPointOptions pointOptions;
        pointOptions.estimator = *tokenEstimator;
        pointOptions.summarizeImpl
            = [this](const agentxx::feature::SummarizeRequest&  req,
                     const agentxx::feature::ImplContext& /*ictx*/)
            -> asio::awaitable<std::optional<agentxx::feature::SummarizeValue>> {
            // 核心实现: 只产出摘要文本, 不写回会话 (编排在中间件)
            auto summary = co_await doSummarizeWithLLM(req.sessionId, req.messages, req.manual);
            if (summary.empty()) {
                co_return std::nullopt; // 失败按"没意见"处理, 由编排方兜底
            }
            agentxx::feature::SummarizeValue value;
            value.summary   = std::move(summary);
            value.truncated = false;
            co_return value;
        };
        featurePoints = agentxx::feature::registerContextPoints(*ctx->features, pointOptions);
    } else {
        XX_LOGD("SummarizationMiddlewareHandle: 未取到功能点注册表, 只用同步估算路径");
    }
}

size_t SummarizationMiddlewareHandle::countTokensForUtf8Str(std::string_view in_str) const {
    // 转发到功能点核心实现所用的同一份估算规则 (只保留一份实现)
    return tokenEstimator->estimateText(in_str);
}

size_t SummarizationMiddlewareHandle::countTokens(
    const std::vector<std::string>&           systemMsgs,
    const std::vector<neograph::ChatMessage>& messages,
    bool                                      countThinking
) const {
    size_t count = 0;
    for (const auto& msg : systemMsgs) {
        count += static_cast<size_t>(extraTokensPerMessage) + tokenEstimator->estimateText(msg);
    }
    count += tokenEstimator->estimateMessages(messages, countThinking);
    return count;
}

asio::awaitable<size_t> SummarizationMiddlewareHandle::countTokensViaPoint(
    std::string_view                          sessionId,
    const std::vector<neograph::ChatMessage>& messages,
    bool                                      countThinking
) {
    if (featurePoints.countTokens == nullptr) {
        co_return countTokens({}, messages, countThinking);
    }
    agentxx::feature::CountTokensRequest req;
    req.messages      = messages;
    req.kind          = "messages";
    req.countThinking = countThinking;
    if (auto ctx = agentContext.lock(); ctx != nullptr) {
        req.model = ctx->getSessionCurrentModelName(sessionId);
    }
    auto result = co_await featurePoints.countTokens->ask(
        req,
        agentxx::feature::AskOptions{.sessionId = std::string{sessionId}}
    );
    if (!result.ok()) {
        // 点不可用时不影响压缩流程: 回退同步快路径
        co_return countTokens({}, messages, countThinking);
    }
    co_return static_cast<size_t>(std::max<int64_t>(result.value->tokens, 0));
}

asio::awaitable<std::string> SummarizationMiddlewareHandle::summarizeViaPoint(
    std::string_view                          sessionId,
    const std::vector<neograph::ChatMessage>& messages,
    bool                                      direct
) {
    if (featurePoints.summarize == nullptr) {
        co_return co_await doSummarizeWithLLM(sessionId, messages, direct);
    }
    agentxx::feature::SummarizeRequest req;
    req.sessionId        = std::string{sessionId};
    req.messages         = messages;
    req.maxSummaryTokens = static_cast<int64_t>(summaryMaxTokens);
    req.manual           = direct;
    if (auto ctx = agentContext.lock(); ctx != nullptr) {
        req.model    = ctx->getSessionCurrentModelName(sessionId);
        req.language = ctx->getLanguage(sessionId);
    }
    auto result = co_await featurePoints.summarize->ask(
        req,
        agentxx::feature::AskOptions{.sessionId = std::string{sessionId}}
    );
    if (!result.ok()) {
        co_return std::string{};
    }
    co_return result.value->summary;
}

std::string SummarizationMiddlewareHandle::messagesToText(
    const std::vector<neograph::ChatMessage>& msgs,
    bool                                      includeSystem
) const {
    std::ostringstream oss;
    for (const auto& m : msgs) {
        if (!includeSystem && m.role == "system") {
            continue;
        }
        oss << fmt::format("[{}]: ", m.role) << m.content << std::endl;
        if (!m.tool_calls.empty()) {
            for (const auto& tc : m.tool_calls) {
                oss << fmt::format("  - [toolcall:{}] {}", tc.name, tc.arguments) << std::endl;
            }
        }
    }
    return oss.str();
}

void SummarizationMiddlewareHandle::cleanNoiseMessages(std::vector<neograph::ChatMessage>& messages
) {
    // 1. 删除完全空的消息 + 2. 相邻完全相同的消息只保留最后一条
    std::vector<neograph::ChatMessage> out;
    out.reserve(messages.size());
    for (auto& m : messages) {
        if (m.content.empty() && m.tool_calls.empty() && m.reasoning_content.empty()
            && m.image_urls.empty() && m.audio_urls.empty() && m.video_urls.empty()
            && m.history_contents.empty()) {
            continue; // 空消息: 零信息, 删除
        }
        if (!out.empty() && isSameMessage(out.back(), m)) {
            out.back() = std::move(m); // 相邻重复: 用新的覆盖旧的 (保留最新)
            continue;
        }
        out.push_back(std::move(m));
    }

    // 3. 连续出现的 AutoInserted 提示噪音只保留最后一条 (失败重试噪音折叠)
    std::vector<neograph::ChatMessage> out2;
    out2.reserve(out.size());
    for (size_t i = 0; i < out.size(); ++i) {
        if (isNoiseMessage(out[i])) {
            size_t j = i;
            while (j + 1 < out.size() && isNoiseMessage(out[j + 1])) {
                ++j;
            }
            out2.push_back(std::move(out[j]));
            i = j;
        } else {
            out2.push_back(std::move(out[i]));
        }
    }
    messages = std::move(out2);
}

void SummarizationMiddlewareHandle::cleanThinkingMessages(std::vector<neograph::ChatMessage>& messages
) {
    // 只保留最新一条 thinking: 旧轮 thinking 对续写没有价值 (结论已写在 content /
    // tool 结果里), 且部分模型的 thinking 是加密内容, 按字符估算 token 不准
    int64_t keepIndex = -1;
    for (int64_t i = static_cast<int64_t>(messages.size()) - 1; i >= 0; --i) {
        if (!messages[static_cast<size_t>(i)].reasoning_content.empty()) {
            keepIndex = i;
            break;
        }
    }
    for (int64_t i = 0; i < static_cast<int64_t>(messages.size()); ++i) {
        if (i != keepIndex) {
            messages[static_cast<size_t>(i)].reasoning_content.clear();
        }
    }
}

std::string SummarizationMiddlewareHandle::fitSummaryMaxTokens(std::string summary) const {
    if (summary.empty()) {
        return summary;
    }
    const size_t summaryTokens = countTokensForUtf8Str(summary);
    if (summaryTokens <= summaryMaxTokens) {
        return summary;
    }
    // 截断说明 (放在摘要末尾, 让模型知道后面还有内容被省略)
    static constexpr std::string_view kTruncatedNote
        = "\n[Summary truncated to fit the context limit.]";
    // 二分: 找满足上限的最大前缀 (与 hardTruncate 的单条内容截断同一思路)
    // - 探测时把截断说明一起算进去: 前缀与说明分别计数后相加, 整数取整可能多出
    //   1 个 token, 合并计数才能保证最终结果不超上限
    const auto countPrefixWithNote = [&](size_t prefixBytes) -> size_t {
        std::string probe = summary.substr(0, prefixBytes);
        probe.append(kTruncatedNote);
        return countTokensForUtf8Str(probe);
    };
    size_t lo = 0, hi = summary.size();
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        if (countPrefixWithNote(mid) <= summaryMaxTokens) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    std::string out = summary.substr(0, lo);
    // 回退到 UTF-8 字符边界: 避免末尾留下半个字符 (按字节切分可能切断多字节字符)
    while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) {
        out.pop_back();
    }
    out.append(kTruncatedNote);
    XX_LOGW(
        "SummarizationMiddlewareHandle: 摘要超长 ({} -> {} token, 上限 {}), 已截断",
        summaryTokens,
        countTokensForUtf8Str(out),
        summaryMaxTokens
    );
    return out;
}

void SummarizationMiddlewareHandle::foldExploratoryToolcalls(
    std::vector<neograph::ChatMessage>& messages
) {
    // 从后往前扫描, 找"同一工具的连续单工具调用段" (中间无 user/system 打断,
    // 仅间隔 tool 结果消息); 段长 >= 3 时, 删除除最后一组外的整组
    // (assistant + 对应 tool 结果): 探索过程无价值, 结论在最后一组
    // - 仅折叠"读类"工具: 注册了 truncateResponse 且无 truncateRequest
    //   (如 read_file/grep/glob; 写类工具 truncateRequest 非空, 不折叠)
    std::vector<std::pair<size_t, size_t>> groupsToRemove; // [begin, endExclusive) 删除范围
    std::string                            runTool;
    std::vector<size_t>                    runAssistantIdx; // 降序 (从后往前 push)

    auto finalizeRun = [&]() {
        if (runAssistantIdx.size() >= 3) {
            // 删除 [最早 assistant, 最后一条 assistant) 范围内的全部消息
            // (该范围内只有 assistant + 其 tool 结果, 无 user/system 打断);
            // runAssistantIdx 降序: back=最早(索引最小), front=最后一条(索引最大, 保留)
            groupsToRemove.emplace_back(runAssistantIdx.back(), runAssistantIdx.front());
        }
        runTool.clear();
        runAssistantIdx.clear();
    };

    for (int64_t i = static_cast<int64_t>(messages.size()) - 1; i >= 0; --i) {
        const auto& m = messages[static_cast<size_t>(i)];
        if (m.role == "assistant" && !m.tool_calls.empty()) {
            const bool foldable = m.tool_calls.size() == 1 && [&]() {
                auto it = summarizationToolHandles.find(m.tool_calls[0].name);
                return it != summarizationToolHandles.end()
                       && nullptr != it->second.truncateResponse
                       && nullptr == it->second.truncateRequest;
            }();
            if (foldable) {
                const auto& name = m.tool_calls[0].name;
                if (runTool.empty()) {
                    runTool = name;
                } else if (name != runTool) {
                    finalizeRun();
                    runTool = name;
                }
                runAssistantIdx.push_back(static_cast<size_t>(i));
                continue;
            }
            finalizeRun(); // 多工具调用/不可折叠工具: 打断连续段
        } else if (m.role == "user" || m.role == "system") {
            finalizeRun(); // user/system: 打断连续段
        }
        // tool 结果消息: 不打断 (属于当前段)
    }
    finalizeRun();

    if (groupsToRemove.empty()) {
        return;
    }
    // 从后往前删除, 索引不失效
    for (auto it = groupsToRemove.rbegin(); it != groupsToRemove.rend(); ++it) {
        messages.erase(messages.begin() + it->first, messages.begin() + it->second);
    }
}

void SummarizationMiddlewareHandle::doSummarizeToolcall(std::vector<neograph::ChatMessage>& messages
) {
    {
        auto                          agentCtxPtr = agentContext.lock();
        std::map<std::string, size_t> lastWriteIndex{};
        // 从后往前遍历 (含索引 0): 无 system 消息时首个 assistant(tool_calls) 可能位于
        // 消息索引 0, 其后续 tool 结果 (索引 >=1) 需要与之配对去重; 若排除索引 0,
        // 该组 (assistant, tool) 的去重逻辑会漏掉 (外层循环从 i >= 1 开始, tool 消息
        // 在索引 0 时永远不会被处理)
        for (int64_t i = static_cast<int64_t>(messages.size()) - 1; i >= 0; --i) {
            auto& msg = messages[i];
            if ("tool" == msg.role) {
                auto itemHandleIt = summarizationToolHandles.find(msg.tool_name);
                if (itemHandleIt != summarizationToolHandles.end()
                    && itemHandleIt->second.generateDeduplicationKey
                    && itemHandleIt->second.truncateResponse) {
                    // 寻找 llm toolcall message
                    int64_t lastMsgIndex  = i - 1;
                    int64_t toolcallIndex = -1;
                    // 从 0 开始遍历: 首个 assistant(tool_calls) 可能位于消息索引 0
                    // (无 system 消息时), 不能排除该位置, 否则该组 (assistant,tool)
                    // 永远无法去重
                    for (; lastMsgIndex >= 0; --lastMsgIndex) {
                        for (int64_t j = 0;
                             j < static_cast<int64_t>(messages[lastMsgIndex].tool_calls.size());
                             ++j) {
                            if (msg.tool_call_id == messages[lastMsgIndex].tool_calls[j].id) {
                                toolcallIndex = j;
                                break;
                            }
                        }
                        if (toolcallIndex >= 0) {
                            break;
                        }
                    }

                    utilxx_base::Json args;
                    if (toolcallIndex >= 0) {
                        // 非法 JSON 参数: 跳过该条而非中断整轮压缩
                        agentxx::util::catchError<bool>(
                            [&]() -> bool {
                                args = utilxx_base::Json::parse(
                                    messages[lastMsgIndex].tool_calls[toolcallIndex].arguments
                                );
                                return true;
                            },
                            [](std::string) -> bool {
                                return false;
                            }
                        );
                    }

                    auto key = itemHandleIt->second.generateDeduplicationKey(args);
                    if (key.has_value()) {
                        if (lastWriteIndex.contains(*key)) {
                            itemHandleIt->second.truncateResponse(msg);
                        } else {
                            lastWriteIndex[*key] = i;
                        }
                    }
                }
            } else {
                // assistant
                for (auto& tc : msg.tool_calls) {
                    auto itemHandleIt = summarizationToolHandles.find(tc.name);
                    if (itemHandleIt != summarizationToolHandles.end()
                        && itemHandleIt->second.generateDeduplicationKey
                        && itemHandleIt->second.truncateRequest) {
                        utilxx_base::Json args;
                        // 非法 JSON 参数: 跳过该条而非中断整轮压缩
                        agentxx::util::catchError<bool>(
                            [&]() -> bool {
                                args = utilxx_base::Json::parse(tc.arguments);
                                return true;
                            },
                            [](std::string) -> bool {
                                return false;
                            }
                        );
                        auto key = itemHandleIt->second.generateDeduplicationKey(args);
                        if (key.has_value()) {
                            if (lastWriteIndex.contains(*key)) {
                                itemHandleIt->second.truncateRequest(tc);
                            } else {
                                lastWriteIndex[*key] = i;
                            }
                        }
                    }
                }
            }
        }
    }

    // 探索型调用序列折叠 (在去重之后: 去重已截断旧内容, 折叠删除整组)
    foldExploratoryToolcalls(messages);
}

size_t SummarizationMiddlewareHandle::splitRecentByTokenBudget(
    const std::vector<neograph::ChatMessage>& messages,
    size_t                                    systemCount,
    size_t                                    tokenBudget
) const {
    if (messages.size() <= systemCount) {
        return messages.size();
    }
    // 从后往前累计预算; recent 至少保留 1 条 (最近消息最重要, 且压缩必须有空间);
    // 预算充足时 recent 收至 system 之后全部消息
    size_t end    = messages.size();
    size_t budget = tokenBudget;
    while (end > systemCount) {
        const size_t t = countTokens({}, {messages[end - 1]}, false);
        if (t > budget) {
            // 该条超出剩余预算: recent 为空时仍纳入该条 (至少 1 条, 可能略超预算)
            if (end == messages.size()) {
                --end;
            }
            break;
        }
        budget -= t;
        --end;
    }

    // 对齐 1: recent 开头为 tool 消息 → 回退到发起这组 toolcall 的 assistant,
    // 把整组纳入 recent, 避免产生孤儿 tool 结果 / 悬空 tool_calls
    while (end > systemCount && end < messages.size() && "tool" == messages[end].role) {
        --end;
    }
    // 对齐 2: 压缩段末尾为 assistant(tool_calls) 且其 tool 结果在 recent 内 →
    // 整组划入 recent, 避免压缩段以悬挂 tool_calls 结尾
    while (end > systemCount) {
        const auto& last = messages[end - 1];
        if ("assistant" != last.role || last.tool_calls.empty()) {
            break;
        }
        bool hasResultInRecent = false;
        for (const auto& tc : last.tool_calls) {
            if (tc.id.empty()) {
                continue;
            }
            for (size_t i = end; i < messages.size(); ++i) {
                if ("tool" == messages[i].role && messages[i].tool_call_id == tc.id) {
                    hasResultInRecent = true;
                    break;
                }
            }
            if (hasResultInRecent) {
                break;
            }
        }
        if (!hasResultInRecent) {
            break;
        }
        --end;
    }
    return end;
}

asio::awaitable<std::string> SummarizationMiddlewareHandle::doSummarizeWithLLM(
    std::string_view                          sessionId,
    const std::vector<neograph::ChatMessage>& messages,
    bool                                      direct
) {
    auto agentCtxPtr = agentContext.lock();
    if (nullptr == agentCtxPtr || nullptr == agentCtxPtr->agentConfig
        || nullptr == agentCtxPtr->bus) {
        co_return std::string{};
    }
    if (messages.empty()) {
        co_return std::string{};
    }

    // 压缩指令模板 (优先取独立配置 prompt.summarizePrompt，回退兼容旧配置 appendSystemPrompts["summarization"])
    std::string summarizePrompt = agentCtxPtr->agentConfig->prompt.summarizePrompt;
    if (summarizePrompt.empty()) {
        auto itSumm = agentCtxPtr->agentConfig->prompt.appendSystemPrompts.find("summarization");
        if (itSumm != agentCtxPtr->agentConfig->prompt.appendSystemPrompts.end()) {
            summarizePrompt = itSumm->second;
        }
    }
    if (summarizePrompt.empty()) {
        co_return std::string{};
    }

    // 模型上下文上限 (压缩请求载荷裁剪用; 与子代理实际模型一致,
    // 因同上下文模式强制使用父会话当前模型)
    size_t modelMaxToken = modelSupportMaxTokenDefault;
    {
        const auto& currentModelConfig = agentCtxPtr->getSessionCurrentModelConfig(sessionId);
        if (currentModelConfig.modelContextMaxToken > 0) {
            modelMaxToken = currentModelConfig.modelContextMaxToken;
        }
    }

    // 同上下文: 原始消息副本 (system + 压缩段) + 末尾追加压缩指令
    auto reqMsgs = messages;

    // 载荷裁剪: 压缩段本身超限时 (如超大附件), 从最旧消息开始丢弃;
    // 仅影响请求副本, 不影响覆盖回写结构; 丢弃数写入指令提示模型
    size_t droppedCount = 0;
    while (reqMsgs.size() > 1 && countTokens({}, reqMsgs, false) > modelMaxToken * 0.95) {
        size_t dropIdx = ("system" == reqMsgs[0].role) ? 1 : 0;
        if (dropIdx >= reqMsgs.size()) {
            break;
        }
        reqMsgs.erase(reqMsgs.begin() + static_cast<int64_t>(dropIdx));
        ++droppedCount;
    }

    std::string omittedNote;
    if (droppedCount > 0) {
        omittedNote = fmt::format(
            "NOTE: The oldest {} message(s) were omitted from the input above due to context "
            "limits.\n",
            droppedCount
        );
    }

    neograph::ChatMessage promptMsg;
    promptMsg.role = "user";
    // 模板来自 AgentPrompt (运行时字符串), 经 fmt::runtime 动态解析
    promptMsg.content = fmt::format(
        fmt::runtime(summarizePrompt),
        fmt::arg("omitted_note", omittedNote),
        fmt::arg("max_words", summaryMaxTokens / 4)
    );
    promptMsg.flags = neograph::MessageFlag::AutoInserted;
    reqMsgs.push_back(std::move(promptMsg));

    // 通过 subagent 完成压缩 (同上下文模式):
    // - messages: 结构化透传 (system + 压缩段 + 压缩指令), 无文本转录
    // - sessionId: 父线程 → 子代理与父会话相同 session_id + 相同模型,
    //   命中 provider KV/prefix cache
    // - tools: 不传入 → 子代理无任何工具, 仅对当前上下文原样做压缩
    //   (不再提供 agentxx_share_store, 长内容由模型直接写进摘要;
    //   避免 subagent 侧工具执行成本与父会话 store 的跨会话耦合)
    // - enable_summarization: false → 禁止对透传前缀二次压缩
    // - subagent 内部完成"阅读上下文 → 输出摘要"的完整 agent 循环,
    //   最终纯文本输出即为摘要
    // - 首次调用抛 NodeInterrupt 暂停父轮次, Session 派生 subagent,
    //   resume 后此调用返回 subagent 输出 (摘要)
    neograph::json neoReqMsgs;
    neograph::to_json(neoReqMsgs, reqMsgs);
    utilxx_base::Json reqMsgsJson = agentxx::util::fromNeographJson(neoReqMsgs);

    if (direct) {
        // 手动压缩直派模式 (agent 空闲触发, 无 AgentRunner 中断循环):
        // 不再走 SubagentExecute RR → requestInterrupt 抛 NodeInterrupt 的
        // 中断委派路径 —— 该路径的中断只能由 AgentRunner 处理, 空闲时
        // 会穿过所有 catchErrorAsync 逃逸到 EventBus publish 的 detached
        // 协程被 asio 静默丢弃, 表现为压缩永久卡在 "Summarizing..."。
        // 改为直接经宿主 AgentHost::spawnBatch 派生压缩子代理并等待完成,
        // 与中断路径复用同一 spawnOneTask (同上下文模式语义一致: 相同
        // sessionId + 父会话模型 → KV cache 命中), 不抛中断。
        auto host = agentCtxPtr->host.lock();
        if (host != nullptr) {
            // 单任务批量请求; tools=[] 显式无工具 (与压缩注释意图一致, 避免
            // 子代理默认全量工具的执行成本与副作用), enable_summarization=false
            // 禁止二次压缩, sessionId=父线程 (同上下文模式)
            events::ReqSubagentBatch batchReq;
            batchReq.parentAgentName
                = agentCtxPtr->agentConfig ? agentCtxPtr->agentConfig->agentName : std::string{};
            batchReq.parentSessionId = std::string{sessionId};
            batchReq.tasks.push_back(events::SubagentBatchItem{
                .subagentName = "subagent_task",
                .messages     = reqMsgsJson, // 结构化透传 (含压缩指令), 无文本转录
                .sessionId    = std::string{sessionId},
                .tools        = utilxx_base::Json::array(), // []: 无工具
                .enableSummarization = false,
            });

            // 超时保护: 手动压缩无父轮次取消令牌可级联, 子代理 LLM 卡住时不能
            // 无限等待 (占用 io 线程); 超时放弃等待 (子代理后台完成自回收),
            // 返回空串由调用方走 hardTruncate 兜底
            constexpr std::chrono::milliseconds kManualCompactTimeout{std::chrono::minutes{2}};
            auto batchResp = co_await utilxx::asyncWithTimeout<events::RespSubagentBatch>(
                [&]() -> asio::awaitable<events::RespSubagentBatch> {
                    co_return co_await host->spawnBatch(batchReq, agentCtxPtr);
                },
                kManualCompactTimeout,
                []() -> events::RespSubagentBatch {
                    return events::RespSubagentBatch{};
                }
            );
            if (batchResp.results.empty()) {
                XX_LOGW("SummarizationMiddlewareHandle 手动压缩: 子代理无结果 (超时或失败)");
                co_return "";
            }
            const auto& item = batchResp.results[0];
            if (item.hasError) {
                XX_LOGE(
                    "SummarizationMiddlewareHandle 手动压缩 subagent 执行失败: {}",
                    item.errorMessage
                );
                co_return "";
            }
            // 错误串透传防护 (与中断路径一致): 不得把错误串当摘要写回
            if (item.content.size() >= 2 && item.content.front() == '{'
                && item.content.find("\"error\"") != std::string::npos) {
                XX_LOGW(
                    "SummarizationMiddlewareHandle 手动压缩结果为错误串, 按失败处理: {}",
                    std::string_view{item.content}.substr(0, 256)
                );
                co_return "";
            }
            // 摘要超长 (模型不遵守指令里的字数要求): 截断后写回, 避免刚压下去的
            // 上下文被摘要重新撑大
            co_return fitSummaryMaxTokens(item.content);
        }
        // 无 host 时 (如单测 mock 环境), 降级走总线请求
    }

    auto args = utilxx_base::Json{
        {"subagent",             "subagent_task"       },
        {"messages",             std::move(reqMsgsJson)},
        {"sessionId",            std::string{sessionId}},
        {"enable_summarization", false                 },
    };

    // 经总线请求压缩 subagent (service.subagent.execute)
    auto requestViaBus = [&]() -> asio::awaitable<std::string> {
        // SubAgentManagerTool 经 requestInterrupt 抛 NodeInterrupt:
        // 自动路径由 catchErrorAsync 放行传播给 AgentRunner 中断循环处理
        auto resp = co_await agentCtxPtr->bus
                        ->request<events::ReqSubagentExecute, events::RespSubagentExecute>(
                            events::Topic::SubagentExecute,
                            events::ReqSubagentExecute{.arguments = args},
                            std::chrono::milliseconds{0}
                        );
        if (!resp.has_value()) {
            XX_LOGE("SummarizationMiddlewareHandle 压缩 subagent 请求失败: {}", resp.error());
            co_return "";
        }
        if (resp->hasError) {
            XX_LOGE(
                "SummarizationMiddlewareHandle 压缩 subagent 执行失败: {}",
                resp->errorMessage
            );
            co_return "";
        }
        // 取消/错误串透传防护: 子代理被取消时宿主返回
        // `{"error":"Sub-agent cancelled..."}` (AgentRunner 已优先按取消
        // 抛, 此处为直接调用路径的兜底), 不得当作有效摘要写回上下文,
        // 否则表现为"压缩成功 + 继续执行", 取消形同虚设
        if (resp->result.size() >= 2 && resp->result.front() == '{'
            && resp->result.find("\"error\"") != std::string::npos) {
            XX_LOGW(
                "SummarizationMiddlewareHandle 压缩结果为错误串, 按失败处理: {}",
                std::string_view{resp->result}.substr(0, 256)
            );
            co_return "";
        }
        // 摘要超长 (模型不遵守指令里的字数要求): 截断后写回
        co_return fitSummaryMaxTokens(resp->result);
    };

    if (direct) {
        // 直派模式: 无 AgentRunner 中断循环, 中断异常从这里抛出后无人处理 ——
        // 手动压缩的调用方 (事件订阅协程) 不在中断循环里, 逃逸的中断会让
        // 压缩静默失败; 溢出压缩更是运行在轮次内, 会造成本轮以"中断未完成"
        // 静默结束。故此处把控制流异常 (中断/取消) 转成"压缩失败"返回空串,
        // 由调用方走硬截断兜底 (见 summarization.h 说明)
        co_return co_await agentxx::util::catchErrorAsync<std::string>(
            requestViaBus,
            [](std::string errmsg) -> asio::awaitable<std::string> {
                XX_LOGE("SummarizationMiddlewareHandle 直派压缩 subagent 调用失败: {}", errmsg);
                co_return "";
            },
            [](std::string& errmsg) -> std::optional<std::string> {
                XX_LOGW(
                    "SummarizationMiddlewareHandle 直派压缩无中断处理者, 按压缩失败处理: {}",
                    errmsg
                );
                return std::optional<std::string>{std::string{}};
            }
        );
    }

    co_return co_await agentxx::util::catchErrorAsync<std::string>(
        requestViaBus,
        [](std::string errmsg) -> asio::awaitable<std::string> {
            XX_LOGE("SummarizationMiddlewareHandle 压缩 subagent 调用失败: {}", errmsg);
            co_return "";
        }
    );
}

std::vector<neograph::ChatMessage> SummarizationMiddlewareHandle::hardTruncate(
    const std::vector<neograph::ChatMessage>& messages,
    size_t                                    systemCount,
    size_t                                    maxToken
) const {
    std::vector<neograph::ChatMessage> out;
    if (systemCount > 0 && !messages.empty()) {
        out.push_back(messages[0]); // system 原样保留
    }
    // 截断说明 (user 角色, 置于 recent 之前, 保证角色顺序合法)
    neograph::ChatMessage note;
    note.role    = "user";
    note.content = "[Earlier conversation was truncated due to context limit. Ask the user for "
                   "details if needed.]";
    note.flags   = neograph::MessageFlag::AutoInserted | neograph::MessageFlag::Summarized;
    out.push_back(std::move(note));

    // 最近消息: 30% 预算
    const size_t recentBudget = static_cast<size_t>(maxToken * 0.30);
    const size_t end          = splitRecentByTokenBudget(messages, systemCount, recentBudget);
    for (size_t i = end; i < messages.size(); ++i) {
        out.push_back(messages[i]);
    }

    // ---- 兜底: 仍超限时保证请求能发出 ----
    // 场景: recent 中存在单条超大消息 (如超大附件/长日志), 即使切分已收至最少
    // 1 条, 或 system 本身很大, 结果仍可能 >= 95% 上限。
    // 1) 优先从最旧 recent 开始丢弃, 至少保留最后 1 条消息 (会话不能为空)
    // 2) 只剩最后 1 条仍超限 → 二分截断该条文本内容 (保留开头语义), 消息结构与
    //    角色不变, 让请求载荷能发出 (模型可据此继续)
    const size_t maxAllowed  = static_cast<size_t>(maxToken * 0.95);
    const size_t recentStart = (systemCount > 0) ? 2 : 1; // out 中 recent 段起点
    while (out.size() > recentStart + 1 && countTokens({}, out, false) > maxAllowed) {
        out.erase(out.begin() + static_cast<int64_t>(recentStart));
    }
    if (out.size() > recentStart && countTokens({}, out, false) > maxAllowed) {
        auto&        last         = out.back();
        const size_t prefixTokens = countTokens(
            {},
            std::vector<neograph::ChatMessage>(out.begin(), out.end() - 1),
            false
        );
        if (prefixTokens < maxAllowed) {
            const size_t budget    = maxAllowed - prefixTokens;
            const auto   countWith = [&](std::string_view content) -> size_t {
                auto copy    = last;
                copy.content = std::string{content};
                return countTokens({}, {copy}, false);
            };
            size_t lo = 0, hi = last.content.size();
            while (lo < hi) {
                const size_t mid = (lo + hi + 1) / 2;
                if (countWith(std::string_view{last.content}.substr(0, mid)) <= budget) {
                    lo = mid;
                } else {
                    hi = mid - 1;
                }
            }
            last.content = last.content.substr(0, lo);
        }
    }
    return out;
}

asio::awaitable<void>
    SummarizationMiddlewareHandle::onModelcallRunFunc(neograph::graph::NodeInput& in) {
    auto agentCtxPtr = agentContext.lock();
    if (nullptr == agentCtxPtr) {
        co_return;
    }
    auto session = agentCtxPtr->sessions->getOrCreate(in.ctx.thread_id);
    if (session->messages().empty()) {
        co_return;
    }
    // 上下文以会话为唯一权威: 压缩过程需要就地改写, 取一份拷贝
    auto messages = session->messages();

    const auto& sessionId = in.ctx.thread_id;

    // 从会话的模型配置提取模型支持的最大 token, 模型配置未指定时使用默认值
    size_t modelContextMaxToken = modelSupportMaxTokenDefault;
    bool   enableCountThinking   = false;
    {
        const auto& currentModelConfig = agentCtxPtr->getSessionCurrentModelConfig(sessionId);
        if (currentModelConfig.modelContextMaxToken > 0) {
            modelContextMaxToken = currentModelConfig.modelContextMaxToken;
        }
        enableCountThinking = currentModelConfig.sendThinking;
    }

    // - 接口返回的 token usage，可能不准确，因为 llm node
    // 重试时可能会额外附加消息、也可能是上一轮的 api 返回的，本轮开始已经添加了
    // toolcall / userInput 等消息
    size_t apiTokenUsage = 0;
    {
        const auto& apiTokenUsageJson
            = agentCtxPtr->middlewareHandleContext->getGraphDataItemValue<utilxx_base::Json>(
                in.ctx.thread_id,
                agentxx::middleware::MiddlewareContext::graphDataKey_LLMTokenUsage
            );
        if (apiTokenUsageJson.is_number_integer()) {
            apiTokenUsage = apiTokenUsageJson.get<size_t>();
        }
    }

    // 预算计算经功能点取 token 数: 插件提供的 tokenizer 在这里可见
    // (TPS 显示的同步快路径不参与, 见 plan §8.1)
    const auto countTokenUsage
        = co_await countTokensViaPoint(in.ctx.thread_id, messages, enableCountThinking);
    const auto tokenUsage      = (apiTokenUsage > 0) ? apiTokenUsage : countTokenUsage;
    // 发布上下文统计到对应会话, 供 UI 显示上下文占用百分比
    if (session->contextStats) {
        // UI显示优先使用 apiTokenUsage 即可
        session->contextStats->contextTokens    = tokenUsage;
        session->contextStats->maxContextTokens = modelContextMaxToken;
    }

    // ---- 超过 75% 上限时自动压缩 ----
    if (tokenUsage >= modelContextMaxToken * 0.75) {
        const auto startTime = std::chrono::steady_clock::now();
        const auto startTimeMs
            = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::system_clock::now().time_since_epoch()
            )
                                       .count());
        const auto oldTokens = tokenUsage;

        // 1. 触发压缩时，先发送一条 viewMessage 提示 "正在压缩上下文"
        // - 自动压缩经 NodeInterrupt 中断父轮次派生压缩子代理, resume 后本函数
        //   从头重新执行 (压缩请求命中中断结果缓存, 不再派生): 第二次执行必须
        //   复用首次创建的提示消息 (更新而非追加), 否则每次压缩都会遗留一条
        //   永远停留在 "Summarizing LLM Context..." 的重复提示消息
        agentxx::agent::ViewMessage vm = agentxx::agent::ViewMessage::makeText(
            agentxx::agent::ViewMessage::Role::Tip,
            "Summarizing LLM Context...",
            startTimeMs
        );
        vm.tip->tipLevel = agentxx::agent::ViewMessage::TipLevel::Info;
        vm.collapsed     = true;
        if (session) {
            const std::string pendingTipId
                = agentCtxPtr->middlewareHandleContext->getGraphDataItemValue<std::string>(
                    sessionId,
                    agentxx::middleware::MiddlewareContext::graphDataKey_summarizationTipMsgId
                );
            if (pendingTipId.empty()) {
                vm.id = session->appendViewMessage(vm);
                agentCtxPtr->middlewareHandleContext->setGraphDataItemValue<std::string>(
                    sessionId,
                    agentxx::middleware::MiddlewareContext::graphDataKey_summarizationTipMsgId,
                    vm.id
                );
                if (session->io) {
                    session->io->sendToPeer(agentxx::agent::WireDelta{
                        .seq     = session->nextDeltaSeq(),
                        .message = std::make_shared<agentxx::agent::ViewMessage>(vm),
                        .type    = agentxx::agent::WireDelta::Type::InsertMessage,
                    });
                }
            } else {
                // 续跑 (中断恢复后重新执行): 复用首次的提示消息
                vm.id = pendingTipId;
                session->updateViewMessage(vm);
                if (session->io) {
                    session->io->sendToPeer(agentxx::agent::WireDelta{
                        .seq     = session->nextDeltaSeq(),
                        .message = std::make_shared<agentxx::agent::ViewMessage>(vm),
                        .type    = agentxx::agent::WireDelta::Type::UpdateMessage,
                    });
                }
            }
        }

        // 确定性压缩 (toolcall 去重/探索折叠 + 噪音清理)
        doSummarizeToolcall(messages);
        cleanNoiseMessages(messages);

        // 冷却检查: 若上次压缩后消息增长不足 (<= 2 条) 且当前消息数仍 >= 75% 上限，
        // 说明普通的 LLM 摘要无法把消息数降下来，为避免每轮 modelcall 都反复派生 subagent
        // 做无效压缩, 直接降级硬截断以彻底释放空间
        size_t lastSummarizedMsgCount = 0;
        {
            const auto& countJson
                = agentCtxPtr->middlewareHandleContext->getGraphDataItemValue<utilxx_base::Json>(
                    sessionId,
                    agentxx::middleware::MiddlewareContext::graphDataKey_summarizationLastMsgCount
                );
            if (countJson.is_number_integer()) {
                lastSummarizedMsgCount = countJson.get<size_t>();
            }
        }
        size_t currentFailCount = 0;
        {
            const auto& fcJson
                = agentCtxPtr->middlewareHandleContext->getGraphDataItemValue<utilxx_base::Json>(
                    sessionId,
                    agentxx::middleware::MiddlewareContext::graphDataKey_summarizationFailCount
                );
            if (fcJson.is_number_integer()) {
                currentFailCount = fcJson.get<size_t>();
            }
        }
        const bool coolDownActive
            = (currentFailCount == 0 && lastSummarizedMsgCount > 0
               && messages.size() <= lastSummarizedMsgCount + 2);

        // LLM 同上下文压缩
        const size_t systemCount = (!messages.empty() && messages[0].role == "system") ? 1 : 0;

        std::vector<neograph::ChatMessage> compressedMessages;
        bool                               compacted = false;
        if (coolDownActive) {
            XX_LOGD(
                "SummarizationMiddlewareHandle: 上次压缩后消息增长不足 ({} <= {} + 2), 处于冷却期, 跳过重复 LLM 压缩",
                messages.size(),
                lastSummarizedMsgCount
            );
            compressedMessages = messages;
        } else if (messages.size() > systemCount) {
            // 同上下文压缩请求: 当前完整上下文 (system + 全部消息) + 末尾压缩指令。
            // 最近的消息也交给模型: 由它在摘要里写清末尾要点 (进行中的动作, 最近一次
            // 工具调用的结果, 下一步), 因此压缩后不再原样保留最近消息
            auto toSummarize = messages;

            // 压缩前清洗消息中的多模态 data URL (Base64) 替换为纯文本标签
            // TODO: 替换前存储为文件，记录路径
            downgradeMultimodalUrlsToText(toSummarize);
            // 只保留最新一条 thinking, 其余清空 (旧轮 thinking 无续写价值)
            cleanThinkingMessages(toSummarize);

            /// llm 压缩 (同上下文 subagent, 中断后由 Session 派生并 resume)
            /// - 经功能点取摘要文本: 插件 / FFI 宿主的实现可在这里替换核心压缩;
            ///   编排 (提示消息 / 写回 / 兜底) 仍在本中间件
            auto summary = co_await summarizeViaPoint(sessionId, toSummarize, /*direct=*/false);

            enum class ReplaceAction {
                None,
                Compact,
                HardTruncate
            };
            ReplaceAction action = ReplaceAction::None;
            if (!summary.empty()) {
                action = ReplaceAction::Compact;
                // 压缩成功: 重置失败计数
                agentCtxPtr->middlewareHandleContext->setGraphDataItemValue<size_t>(
                    sessionId,
                    agentxx::middleware::MiddlewareContext::graphDataKey_summarizationFailCount,
                    size_t{0}
                );
            } else {
                // 压缩失败: 计数 (同一轮内重试/多轮 modelcall 累积);
                // 连续失败 >= 2 次或超限严重 (>= 95%) 时硬截断兜底
                size_t failCount
                    = agentCtxPtr->middlewareHandleContext->getGraphDataItemValue<size_t>(
                          sessionId,
                          agentxx::middleware::MiddlewareContext::
                              graphDataKey_summarizationFailCount
                      )
                      + 1;
                agentCtxPtr->middlewareHandleContext->setGraphDataItemValue<size_t>(
                    sessionId,
                    agentxx::middleware::MiddlewareContext::graphDataKey_summarizationFailCount,
                    failCount
                );
                if (failCount >= 2 || tokenUsage >= modelContextMaxToken * 0.95) {
                    action = ReplaceAction::HardTruncate;
                }
                XX_LOGD(
                    "SummarizationMiddlewareHandle: llm 压缩失败 (计数 {}), {}",
                    failCount,
                    (action == ReplaceAction::HardTruncate) ? "触发硬截断兜底" : "保留原消息重试"
                );
            }

            if (action == ReplaceAction::Compact) {
                compacted = true;
                if (systemCount > 0) {
                    // 系统消息
                    compressedMessages.push_back(messages[0]);
                }
                // 追加压缩后的信息
                // system | user | assistant
                // - 不再追加最近消息: 末尾的消息已交给 subagent, 要点写在摘要里;
                //   追加会让刚压下去的上下文立刻被最近消息重新填满
                compressedMessages.push_back(neograph::ChatMessage{
                    .role    = "user",
                    .content = "[Please compact context to save space]",
                    .flags
                    = neograph::MessageFlag::AutoInserted | neograph::MessageFlag::Summarized,
                });
                compressedMessages.push_back(neograph::ChatMessage{
                    .role    = "assistant",
                    .content = fmt::format("[Previous conversation summary]: \n{}", summary),
                    .flags
                    = neograph::MessageFlag::AutoInserted | neograph::MessageFlag::Summarized,
                });
            } else if (action == ReplaceAction::HardTruncate) {
                compressedMessages = hardTruncate(messages, systemCount, modelContextMaxToken);
            } else {
                compressedMessages = messages;
            }
        } else {
            compressedMessages = messages;
        }

        // ---- 兜底: 压缩后仍超限 (>= 95%) → 降级硬截断, 保证请求能发出 ----
        // 场景: system 本身很大, 或摘要超长 (截断后仍超), 压缩结果仍 >= 95% 上限;
        // 硬截断 (system + 截断说明 + 最近消息 30% 预算 + 单条二分截断) 是最终兜底
        // - 与触发判定用同一个取数入口 (功能点), 避免两条口径混用
        const size_t compressedTokens
            = co_await countTokensViaPoint(sessionId, compressedMessages, enableCountThinking);
        if (compressedTokens >= modelContextMaxToken * 0.95) {
            XX_LOGW(
                "SummarizationMiddlewareHandle: 压缩后仍超限 ({}/{}), 降级硬截断兜底",
                compressedTokens,
                modelContextMaxToken
            );
            compressedMessages = hardTruncate(messages, systemCount, modelContextMaxToken);
        }

        // ---- 压缩结果写回会话上下文 (唯一权威) ----
        // 压缩结果直接替换会话上下文: 图状态不再持有上下文, 无需再写通道;
        // 同时请求一次节流落盘 —— 进程在压缩后到轮末之间退出 (崩溃/被强制结束) 时,
        // 落库的已是压缩后的上下文, 重启后不会因上下文重新超限而反复压缩
        const size_t compressedCount = compressedMessages.size();
        const auto   newTokens       = countTokens({}, compressedMessages, enableCountThinking);
        session->replaceMessages(std::move(compressedMessages));
        agentxx::nodes::updateMessagesMeta(agentCtxPtr, in.state, in.ctx.thread_id);
        // 压缩完成属"事实"变更: 立即落盘 (计划 STO-5 分级), 不等轮末节流窗口
        session->persistNow("compaction");

        if (compacted) {
            // 成功压缩: 记录本次压缩后的消息条数, 供后续轮次做冷却判断
            agentCtxPtr->middlewareHandleContext->setGraphDataItemValue<size_t>(
                sessionId,
                agentxx::middleware::MiddlewareContext::graphDataKey_summarizationLastMsgCount,
                compressedCount
            );
        } else {
            // 未做 LLM 压缩或失败/硬截断: 复位冷却基准
            agentCtxPtr->middlewareHandleContext->setGraphDataItemValue<size_t>(
                sessionId,
                agentxx::middleware::MiddlewareContext::graphDataKey_summarizationLastMsgCount,
                size_t{0}
            );
        }

        // 更新刚刚的 viewMessage 为
        //     "压缩上下文 {旧}->{新}/{最大} · {耗时}"
        const auto durationMs
            = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - startTime
            )
                                       .count());
        vm.text = fmt::format(
            "Summarized LLM Context {}->{}/{} · {}",
            oldTokens,
            newTokens,
            modelContextMaxToken,
            utilxx_base::formatDurationMilliseconds(durationMs)
        );
        vm.durationMs = durationMs;
        if (session) {
            session->updateViewMessage(vm);
            if (session->io) {
                session->io->sendToPeer(agentxx::agent::WireDelta{
                    .seq     = session->nextDeltaSeq(),
                    .message = std::make_shared<agentxx::agent::ViewMessage>(vm),
                    .type    = agentxx::agent::WireDelta::Type::UpdateMessage,
                });
            }
            if (session->contextStats) {
                session->contextStats->contextTokens    = newTokens;
                session->contextStats->maxContextTokens = modelContextMaxToken;
            }
            // 本次压缩结束: 清除挂起提示标记 (下次压缩重新追加提示消息)
            agentCtxPtr->middlewareHandleContext->removeGraphDataItem(
                sessionId,
                agentxx::middleware::MiddlewareContext::graphDataKey_summarizationTipMsgId
            );
        }
    }

    if (agentCtxPtr->agentConfig->logPrintSummarizationResultTokenCount) {
        XX_LOGD(
            R"_(
┏━━━━━━ Summary ━━━━━━┓
┣━ Messages Length: {}
┣━ Api Token Usage: {}
┣━ Count Messages Token: {}
┣━ Token Limit: {}/{}
┗━━━━━━ Summary ━━━━━━┛)_",
            session->messagesCount(),
            apiTokenUsage,
            countTokenUsage,
            tokenUsage,
            modelContextMaxToken
        );
    }

    co_return;
}

asio::awaitable<bool>
    SummarizationMiddlewareHandle::compactSessionContext(std::string_view sessionId) {
    auto agentCtxPtr = agentContext.lock();
    if (nullptr == agentCtxPtr) {
        co_return false;
    }
    auto session = agentCtxPtr->sessions->get(sessionId);
    if (!session) {
        co_return false;
    }

    // 手动压缩: 上下文取自会话 (唯一权威)
    auto messages = session->messages();
    if (messages.empty()) {
        co_return false;
    }

    size_t modelContextMaxToken = modelSupportMaxTokenDefault;
    bool   enableCountThinking   = false;
    {
        const auto& currentModelConfig = agentCtxPtr->getSessionCurrentModelConfig(sessionId);
        if (currentModelConfig.modelContextMaxToken > 0) {
            modelContextMaxToken = currentModelConfig.modelContextMaxToken;
        }
        enableCountThinking = currentModelConfig.sendThinking;
    }

    const auto startTime = std::chrono::steady_clock::now();
    const auto startTimeMs
        = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch()
        )
                                   .count());
    const auto oldTokens = co_await countTokensViaPoint(sessionId, messages, enableCountThinking);

    // 1. 触发压缩时，先发送一条 viewMessage 提示 "正在压缩上下文"
    agentxx::agent::ViewMessage vm = agentxx::agent::ViewMessage::makeText(
        agentxx::agent::ViewMessage::Role::Tip,
        "Summarizing LLM Context...",
        startTimeMs
    );
    vm.tip->tipLevel = agentxx::agent::ViewMessage::TipLevel::Info;
    vm.collapsed     = true;
    vm.id            = session->appendViewMessage(vm);
    if (session->io) {
        session->io->sendToPeer(agentxx::agent::WireDelta{
            .seq     = session->nextDeltaSeq(),
            .message = std::make_shared<agentxx::agent::ViewMessage>(vm),
            .type    = agentxx::agent::WireDelta::Type::InsertMessage,
        });
    }

    // 2. 确定性压缩 (toolcall 去重/探索折叠 + 噪音清理)
    doSummarizeToolcall(messages);
    cleanNoiseMessages(messages);

    // 3. LLM 同上下文总结压缩
    const size_t systemCount = (!messages.empty() && messages[0].role == "system") ? 1 : 0;

    std::vector<neograph::ChatMessage> compressedMessages;
    if (messages.size() > systemCount) {
        // 同上下文压缩请求: 当前完整上下文 (system + 全部消息) + 末尾压缩指令;
        // 最近的消息也交给模型判断, 因此压缩后不再原样保留最近消息
        auto toSummarize = messages;

        // 压缩前清洗消息中的多模态 data URL (Base64) 降级为纯文本标签
        downgradeMultimodalUrlsToText(toSummarize);
        // 只保留最新一条 thinking, 其余清空 (旧轮 thinking 无续写价值)
        cleanThinkingMessages(toSummarize);

        // 手动压缩在 agent 空闲时触发 (无 AgentRunner 中断循环), 不能走
        // NodeInterrupt 中断委派 (无人处理会逃逸 detached 被吞), 直派模式
        // (经功能点取摘要文本: 编排仍在本中间件)
        auto summary = co_await summarizeViaPoint(sessionId, toSummarize, /*direct=*/true);
        if (!summary.empty()) {
            if (systemCount > 0) {
                compressedMessages.push_back(messages[0]);
            }
            // system | user | assistant (不追加最近消息, 同自动压缩)
            compressedMessages.push_back(neograph::ChatMessage{
                .role    = "user",
                .content = "[Please compact context to save space]",
                .flags   = neograph::MessageFlag::AutoInserted | neograph::MessageFlag::Summarized,
            });
            compressedMessages.push_back(neograph::ChatMessage{
                .role    = "assistant",
                .content = fmt::format("[Previous conversation summary]: \n{}", summary),
                .flags   = neograph::MessageFlag::AutoInserted | neograph::MessageFlag::Summarized,
            });
        } else {
            compressedMessages = hardTruncate(messages, systemCount, modelContextMaxToken);
        }
    } else {
        compressedMessages = messages;
    }

    // ---- 兜底: 压缩后仍超限 (>= 95%) → 降级硬截断, 保证请求能发出 ----
    // (与 onModelcallRunFunc 相同语义; 手动压缩也保证结果不超限)
    if (countTokens({}, compressedMessages, enableCountThinking) >= modelContextMaxToken * 0.95) {
        XX_LOGW(
            "SummarizationMiddlewareHandle: 手动压缩后仍超限 ({}/{}), 降级硬截断兜底",
            countTokens({}, compressedMessages, enableCountThinking),
            modelContextMaxToken
        );
        compressedMessages = hardTruncate(messages, systemCount, modelContextMaxToken);
    }

    const auto newTokens
        = co_await countTokensViaPoint(sessionId, compressedMessages, enableCountThinking);
    session->replaceMessages(std::move(compressedMessages));
    // 手动压缩在轮次外执行: 立即落盘 (无轮末权威保存兜底; 计划 STO-5 分级)
    session->persistNow("manual-compaction");
    const auto durationMs
        = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - startTime
        )
                                   .count());
    vm.text = fmt::format(
        "Summarized LLM Context {}->{}/{} · {}",
        oldTokens,
        newTokens,
        modelContextMaxToken,
        utilxx_base::formatDurationMilliseconds(durationMs)
    );
    vm.durationMs = durationMs;
    session->updateViewMessage(vm);
    if (session->io) {
        session->io->sendToPeer(agentxx::agent::WireDelta{
            .seq     = session->nextDeltaSeq(),
            .message = std::make_shared<agentxx::agent::ViewMessage>(vm),
            .type    = agentxx::agent::WireDelta::Type::UpdateMessage,
        });
        session->io->sendToPeer(agentxx::agent::WireContextStats{newTokens, modelContextMaxToken});
    }
    if (session->contextStats) {
        session->contextStats->contextTokens    = newTokens;
        session->contextStats->maxContextTokens = modelContextMaxToken;
    }

    co_return true;
}

SummarizationMiddlewareHandle::~SummarizationMiddlewareHandle() {
    unregisterFromBus();
}

void SummarizationMiddlewareHandle::registerOnBus(
    const std::shared_ptr<agentxx::events::EventBus>& bus
) {
    if (!bus) {
        return;
    }
    unregisterFromBus();
    registeredBus_ = bus;

    bus->registerService<size_t(std::string_view)>(
        events::Topic::TokenCount,
        [this](std::string_view text) -> size_t {
            return this->countTokensForUtf8Str(text);
        }
    );
    // 手动压缩事件: 供 SessionServerAgentIO 经 EventBus 触发, 解耦对 handle 具体类型的依赖
    compactSubId_
        = bus->get<events::EventCompactContext>(events::Topic::SummarizationCompact)
              .subscribe([this](const events::EventCompactContext& evt) -> asio::awaitable<void> {
                  co_await this->compactSessionContext(evt.sessionId);
              });
}

void SummarizationMiddlewareHandle::unregisterFromBus() {
    if (auto bus = registeredBus_.lock()) {
        bus->unregisterService(events::Topic::TokenCount);
        if (compactSubId_ != 0) {
            bus->get<events::EventCompactContext>(events::Topic::SummarizationCompact)
                .unsubscribe(compactSubId_);
            compactSubId_ = 0;
        }
    }
    registeredBus_.reset();
}

} // namespace middleware
} // namespace agentxx