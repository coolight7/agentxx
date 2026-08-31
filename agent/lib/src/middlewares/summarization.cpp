#include "agentxx/middlewares/summarization.h"

#include "agentxx/agent/io/agent_io.h"
#include "agentxx/agent/io/agent_io_transport.h"
#include "agentxx/agent/model_registry.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/event/events.h"
#include "agentxx/tools/subagent.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/string_util.h"
#include "fmt/format.h"
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
    double                                      in_recentTokenBudgetRatio,
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
    recentTokenBudgetRatio(in_recentTokenBudgetRatio),
    summaryMaxTokens(in_summaryMaxTokens) {
    assert(asciiCharsPerToken >= 0);
    assert(unicodeCharsPerToken >= 0);
    assert(tokensPerImage >= 0);
    assert(extraTokensPerMessage >= 0);
    assert(recentTokenBudgetRatio >= 0 && recentTokenBudgetRatio < 1.0);
}

size_t SummarizationMiddlewareHandle::countTokensForUtf8Str(std::string_view in_str) const {
    size_t unicodeCount = 0, asciiCount = 0;
    for (size_t i = 0, step = 0; i < in_str.size(); i += step) {
        unsigned char byte = in_str[i];
        if (byte >= 0xF8) {
            // 0xF8-0xFF: 无效 UTF-8 前导 (5/6 字节编码已被 RFC 3629 废弃),
            // 按 ascii 单字节处理, 避免吞掉后续字节少计
            step = 1;
            ++asciiCount;
            continue;
        } else if (byte >= 0xF0) {
            // 4 字节前导 0xF0-0xF7
            step = 4;
        } else if (byte >= 0xE0) {
            // 3 字节前导 0xE0-0xEF
            step = 3;
        } else if (byte >= 0xC0) {
            // 2 字节前导 0xC0-0xDF
            step = 2;
        } else {
            // ascii 0x00-0x7F / 续字节 0x80-0xBF (单独出现无效): 单字节处理
            step = 1;
            ++asciiCount;
            continue;
        }
        ++unicodeCount;
    }
    // ascii / unicode 分别按各自折算比例取整后再相加 (与测试/文档语义一致:
    // "ascii + unicode 分别折算"), 避免先相加再整体截断导致高估 token 数
    return static_cast<size_t>(unicodeCount / unicodeCharsPerToken)
           + static_cast<size_t>(asciiCount / asciiCharsPerToken);
}

size_t SummarizationMiddlewareHandle::countTokens(
    const std::vector<std::string>&           systemMsgs,
    const std::vector<neograph::ChatMessage>& messages,
    bool                                      countThinking
) const {
    size_t count = 0;
    for (const auto& msg : systemMsgs) {
        count += static_cast<size_t>(extraTokensPerMessage) + countTokensForUtf8Str(msg);
    }
    for (const auto& item : messages) {
        count += static_cast<size_t>(extraTokensPerMessage) + countTokensForUtf8Str(item.role)
                 + countTokensForUtf8Str(item.content);
        if (countThinking) {
            count += countTokensForUtf8Str(item.reasoning_content);
        }
        for (const auto& tool : item.tool_calls) {
            count += countTokensForUtf8Str(tool.id) + countTokensForUtf8Str(tool.name)
                     + countTokensForUtf8Str(tool.arguments);
        }
        count += static_cast<size_t>(tokensPerImage * item.image_urls.size());
        // 音视频附件同样按图片 token 估算 (各家 API 对多媒体计费粒度不一, 粗略按图片计)
        count += static_cast<size_t>(tokensPerImage * item.audio_urls.size());
        count += static_cast<size_t>(tokensPerImage * item.video_urls.size());
    }
    return count;
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

                    neograph::json args;
                    if (toolcallIndex >= 0) {
                        // 非法 JSON 参数: 跳过该条而非中断整轮压缩
                        agentxx::util::catchError<bool>(
                            [&]() -> bool {
                                args = neograph::json::parse(
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
                        neograph::json args;
                        // 非法 JSON 参数: 跳过该条而非中断整轮压缩
                        agentxx::util::catchError<bool>(
                            [&]() -> bool {
                                args = neograph::json::parse(tc.arguments);
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
    const std::vector<neograph::ChatMessage>& messages
) {
    auto agentCtxPtr = agentContext.lock();
    if (nullptr == agentCtxPtr || nullptr == agentCtxPtr->agentConfig
        || nullptr == agentCtxPtr->bus) {
        co_return std::string{};
    }
    if (messages.empty()) {
        co_return std::string{};
    }

    // 压缩指令模板 (可经 AgentPrompt 定制/训练序列化)
    const auto& summarizePrompt = agentCtxPtr->agentConfig->prompt.summarizationPrompt;
    if (summarizePrompt.empty()) {
        co_return std::string{};
    }

    // 模型上下文上限 (压缩请求载荷裁剪用; 与子代理实际模型一致,
    // 因同上下文模式强制使用父会话当前模型)
    size_t modelMaxToken = modelSupportMaxTokenDefault;
    {
        const auto& currentModelConfig = agentCtxPtr->getSessionCurrentModelConfig(sessionId);
        if (currentModelConfig.modelContenxtMaxToken > 0) {
            modelMaxToken = currentModelConfig.modelContenxtMaxToken;
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
    // - tools: ["agentxx_share_store"] → 模型可自主把长内容写入父会话
    //   store (id 空间一致, 摘要中的 id 父会话可直接读取)
    // - enable_summarization: false → 禁止对透传前缀二次压缩
    // - subagent 内部完成"外置长内容 → 输出摘要"的完整 agent 循环,
    //   最终纯文本输出即为摘要
    // - 首次调用抛 NodeInterrupt 暂停父轮次, Session 派生 subagent,
    //   resume 后此调用返回 subagent 输出 (摘要)
    neograph::json reqMsgsJson;
    neograph::to_json(reqMsgsJson, reqMsgs);

    auto args = neograph::json{
        {"subagent",             "subagent_task"                               },
        {"messages",             std::move(reqMsgsJson)                        },
        {"sessionId",            std::string{sessionId}                        },
        {"tools",                neograph::json::array({"agentxx_share_store"})},
        {"enable_summarization", false                                         },
    };

    co_return co_await agentxx::util::catchErrorAsync<std::string>(
        [&]() -> asio::awaitable<std::string> {
            // NodeInterrupt 会被 catchErrorAsync 放行 (中断/取消不捕获),
            // 传播到引擎后由 Session 派生 subagent, resume 后返回结果
            auto resp = co_await agentCtxPtr->bus
                            ->request<events::ReqSubagentExecute, events::RespSubagentExecute>(
                                events::Topic::SubagentExecute,
                                events::ReqSubagentExecute{.arguments = std::move(args)},
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
            co_return resp->result;
        },
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
                   "details if needed; content stored via `agentxx_share_store` remains "
                   "retrievable by id.]";
    note.flags   = neograph::MessageFlag::AutoInserted | neograph::MessageFlag::Summarized;
    out.push_back(std::move(note));

    // 最近消息: 30% 预算
    const size_t recentBudget = static_cast<size_t>(maxToken * 0.30);
    const size_t end          = splitRecentByTokenBudget(messages, systemCount, recentBudget);
    for (size_t i = end; i < messages.size(); ++i) {
        out.push_back(messages[i]);
    }
    return out;
}

asio::awaitable<void>
    SummarizationMiddlewareHandle::onModelcallRunFunc(neograph::graph::NodeInput& in) {
    auto agentCtxPtr = agentContext.lock();
    if (nullptr == agentCtxPtr) {
        co_return;
    }
    auto messages = in.state.get_messages();
    if (messages.empty()) {
        co_return;
    }

    const auto& sessionId = in.ctx.thread_id;

    // 从会话的模型配置提取模型支持的最大 token, 模型配置未指定时使用默认值
    size_t modelContenxtMaxToken = modelSupportMaxTokenDefault;
    bool   enableCountThinking   = false;
    {
        const auto& currentModelConfig = agentCtxPtr->getSessionCurrentModelConfig(sessionId);
        if (currentModelConfig.modelContenxtMaxToken > 0) {
            modelContenxtMaxToken = currentModelConfig.modelContenxtMaxToken;
        }
        enableCountThinking = currentModelConfig.sendThinking;
    }

    // - 接口返回的 token usage，可能不准确，因为 llm node
    // 重试时可能会额外附加消息、也可能是上一轮的 api 返回的，本轮开始已经添加了
    // toolcall / userInput 等消息
    size_t apiTokenUsage = 0;
    {
        const auto& apiTokenUsageJson
            = agentCtxPtr->middlewareHandleContext->getGraphDataItemValue<neograph::json>(
                in.ctx.thread_id,
                agentxx::middleware::MiddlewareContext::graphDataKey_LLMTokenUsage
            );
        if (apiTokenUsageJson.is_number_integer()) {
            apiTokenUsage = apiTokenUsageJson.get<size_t>();
        }
    }

    const auto countTokenUsage = countTokens({}, messages, enableCountThinking);
    const auto tokenUsage      = (apiTokenUsage > 0) ? apiTokenUsage : countTokenUsage;
    auto       session         = agentCtxPtr->sessions->get(sessionId);
    // 发布上下文统计到对应会话, 供 UI 显示上下文占用百分比
    if (session && session->contextStats) {
        // UI显示优先使用 apiTokenUsage 即可
        session->contextStats->contextTokens    = tokenUsage;
        session->contextStats->maxContextTokens = modelContenxtMaxToken;
    }

    neograph::json newMsgsJson;

    // ---- 超过 85% 上限时自动压缩 ----
    if (tokenUsage >= modelContenxtMaxToken * 0.85) {
        const auto startTime = std::chrono::steady_clock::now();
        const auto startTimeMs
            = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::system_clock::now().time_since_epoch()
            )
                                       .count());
        const auto oldTokens = tokenUsage;

        // 1. 触发压缩时，先发送一条 viewMessage 提示 "正在压缩上下文"
        agentxx::agent::ViewMessage vm = agentxx::agent::ViewMessage::makeText(
            agentxx::agent::ViewMessage::Role::Tip,
            "Summarizizing LLM Context...",
            startTimeMs
        );
        vm.tip->tipLevel = agentxx::agent::ViewMessage::TipLevel::Info;
        vm.collapsed     = true;
        if (session) {
            vm.id = session->appendViewMessage(vm);
            if (session->io) {
                session->io->sendToPeer(agentxx::agent::WireDelta{
                    .type    = agentxx::agent::WireDelta::Type::InsertMessage,
                    .seq     = ++session->deltaSeq,
                    .message = std::make_shared<agentxx::agent::ViewMessage>(vm),
                });
            }
        }

        // 确定性压缩 (toolcall 去重/探索折叠 + 噪音清理)
        doSummarizeToolcall(messages);
        cleanNoiseMessages(messages);

        // LLM 同上下文压缩
        const size_t systemCount = (!messages.empty() && messages[0].role == "system") ? 1 : 0;
        const size_t recentBudget
            = static_cast<size_t>(modelContenxtMaxToken * recentTokenBudgetRatio);
        const size_t oldEnd   = splitRecentByTokenBudget(messages, systemCount, recentBudget);
        const size_t oldStart = systemCount;

        std::vector<neograph::ChatMessage> compressedMessages;
        if (oldEnd > oldStart) {
            // 压缩段 (system 之后, recent 之前)
            auto oldMessages = std::vector<neograph::ChatMessage>{
                messages.begin() + oldStart,
                messages.begin() + oldEnd
            };
            auto recentMessages
                = std::vector<neograph::ChatMessage>{messages.begin() + oldEnd, messages.end()};

            // 同上下文压缩请求: system + 压缩段 (不包含 recent)
            std::vector<neograph::ChatMessage> toSummarize;
            if (systemCount > 0) {
                toSummarize.push_back(messages[0]);
            }
            toSummarize.insert(
                toSummarize.end(),
                std::move_iterator(oldMessages.begin()),
                std::move_iterator(oldMessages.end())
            );

            /// llm 压缩 (同上下文 subagent, 中断后由 Session 派生并 resume)
            auto summary = co_await doSummarizeWithLLM(sessionId, toSummarize);

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
                if (failCount >= 2 || tokenUsage >= modelContenxtMaxToken * 0.95) {
                    action = ReplaceAction::HardTruncate;
                }
                XX_LOGD(
                    "SummarizationMiddlewareHandle: llm 压缩失败 (计数 {}), {}",
                    failCount,
                    (action == ReplaceAction::HardTruncate) ? "触发硬截断兜底" : "保留原消息重试"
                );
            }

            if (action == ReplaceAction::Compact) {
                if (systemCount > 0) {
                    // 系统消息
                    compressedMessages.push_back(messages[0]);
                }
                // 追加压缩后的信息
                // system | user | assistant | [user/tool]recentMessages
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
                // 添加最近消息
                compressedMessages.insert(
                    compressedMessages.end(),
                    std::move_iterator(recentMessages.begin()),
                    std::move_iterator(recentMessages.end())
                );
            } else if (action == ReplaceAction::HardTruncate) {
                compressedMessages = hardTruncate(messages, systemCount, modelContenxtMaxToken);
            } else {
                compressedMessages = messages;
            }
        } else {
            compressedMessages = messages;
        }

        neograph::to_json(newMsgsJson, compressedMessages);
        in.state.overwrite("messages", newMsgsJson);

        // 计算新 token 量与耗时，更新刚刚的 viewMessage 为
        //     "压缩上下文 {旧}->{新}/{最大} · {耗时}"
        const auto newTokens = countTokens({}, compressedMessages, enableCountThinking);
        const auto durationMs
            = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - startTime
            )
                                       .count());
        vm.text = fmt::format(
            "Summarizied LLM Context {}->{}/{} · {}",
            oldTokens,
            newTokens,
            modelContenxtMaxToken,
            agentxx::util::formatDurationMilliseconds(durationMs)
        );
        vm.durationMs = durationMs;
        if (session) {
            session->updateViewMessage(vm);
            if (session->io) {
                session->io->sendToPeer(agentxx::agent::WireDelta{
                    .type    = agentxx::agent::WireDelta::Type::UpdateMessage,
                    .seq     = ++session->deltaSeq,
                    .message = std::make_shared<agentxx::agent::ViewMessage>(vm),
                });
            }
            if (session->contextStats) {
                session->contextStats->contextTokens    = newTokens;
                session->contextStats->maxContextTokens = modelContenxtMaxToken;
            }
        }
    }

    if (newMsgsJson.is_array() && false == newMsgsJson.empty()) {
        auto msgSize = newMsgsJson.size();
        in.state.overwrite("messages", std::move(newMsgsJson));
        if (agentCtxPtr->agentConfig->logPrintSummarizationResultTokenCount) {
            XX_LOGD(
                R"_(
┏━━━━━━ Summary ━━━━━━┓
┣━ Messages Length: {}
┣━ Api Token Usage: {}
┣━ Count Messages Token: {}
┣━ Token Limit: {}/{}
┣━ Summary To: {}
┗━━━━━━ Summary ━━━━━━┛)_",
                msgSize,
                apiTokenUsage,
                countTokenUsage,
                tokenUsage,
                modelContenxtMaxToken,
                countTokens({}, in.state.get_messages(), enableCountThinking)
            );
        }
    } else {
        if (agentCtxPtr->agentConfig->logPrintSummarizationResultTokenCount) {
            XX_LOGD(
                R"_(
┏━━━━━━ Summary ━━━━━━┓
┣━ Messages Length: {}
┣━ Api Token Usage: {}
┣━ Count Messages Token: {}
┣━ Token Limit: {}/{}
┣━ Not Need Summary
┗━━━━━━ Summary ━━━━━━┛)_",
                messages.size(),
                apiTokenUsage,
                countTokenUsage,
                tokenUsage,
                modelContenxtMaxToken
            );
        }
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

    std::vector<neograph::ChatMessage> messages;
    if (session->llmMessages.is_array()) {
        messages.reserve(session->llmMessages.size());
        for (const auto& item : session->llmMessages) {
            neograph::ChatMessage msg;
            neograph::from_json(item, msg);
            messages.push_back(std::move(msg));
        }
    }
    if (messages.empty()) {
        co_return false;
    }

    size_t modelContenxtMaxToken = modelSupportMaxTokenDefault;
    bool   enableCountThinking   = false;
    {
        const auto& currentModelConfig = agentCtxPtr->getSessionCurrentModelConfig(sessionId);
        if (currentModelConfig.modelContenxtMaxToken > 0) {
            modelContenxtMaxToken = currentModelConfig.modelContenxtMaxToken;
        }
        enableCountThinking = currentModelConfig.sendThinking;
    }

    const auto startTime = std::chrono::steady_clock::now();
    const auto startTimeMs
        = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch()
        )
                                   .count());
    const auto oldTokens = countTokens({}, messages, enableCountThinking);

    // 1. 触发压缩时，先发送一条 viewMessage 提示 "正在压缩上下文"
    agentxx::agent::ViewMessage vm = agentxx::agent::ViewMessage::makeText(
        agentxx::agent::ViewMessage::Role::Tip,
        "Summarizizing LLM Context...",
        startTimeMs
    );
    vm.tip->tipLevel = agentxx::agent::ViewMessage::TipLevel::Info;
    vm.collapsed     = true;
    vm.id            = session->appendViewMessage(vm);
    if (session->io) {
        session->io->sendToPeer(agentxx::agent::WireDelta{
            .type    = agentxx::agent::WireDelta::Type::InsertMessage,
            .seq     = ++session->deltaSeq,
            .message = std::make_shared<agentxx::agent::ViewMessage>(vm),
        });
    }

    // 2. 确定性压缩 (toolcall 去重/探索折叠 + 噪音清理)
    doSummarizeToolcall(messages);
    cleanNoiseMessages(messages);

    // 3. LLM 同上下文总结压缩
    const size_t systemCount  = (!messages.empty() && messages[0].role == "system") ? 1 : 0;
    const size_t recentBudget = static_cast<size_t>(modelContenxtMaxToken * recentTokenBudgetRatio);
    const size_t oldEnd       = splitRecentByTokenBudget(messages, systemCount, recentBudget);
    const size_t oldStart     = systemCount;

    std::vector<neograph::ChatMessage> compressedMessages;
    if (oldEnd > oldStart) {
        auto oldMessages = std::vector<neograph::ChatMessage>{
            messages.begin() + oldStart,
            messages.begin() + oldEnd
        };
        auto recentMessages
            = std::vector<neograph::ChatMessage>{messages.begin() + oldEnd, messages.end()};

        std::vector<neograph::ChatMessage> toSummarize;
        if (systemCount > 0) {
            toSummarize.push_back(messages[0]);
        }
        toSummarize.insert(
            toSummarize.end(),
            std::move_iterator(oldMessages.begin()),
            std::move_iterator(oldMessages.end())
        );

        auto summary = co_await doSummarizeWithLLM(sessionId, toSummarize);
        if (!summary.empty()) {
            if (systemCount > 0) {
                compressedMessages.push_back(messages[0]);
            }
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
            compressedMessages.insert(
                compressedMessages.end(),
                std::move_iterator(recentMessages.begin()),
                std::move_iterator(recentMessages.end())
            );
        } else {
            compressedMessages = hardTruncate(messages, systemCount, modelContenxtMaxToken);
        }
    } else {
        compressedMessages = messages;
    }

    neograph::json newMsgsJson;
    neograph::to_json(newMsgsJson, compressedMessages);
    session->llmMessages = newMsgsJson;
    session->saveLlmMessages();

    // 4. 更新统计与 viewMessage 为 "Summarizied LLM Context {旧}->{新}/{最大} · {耗时}"
    const auto newTokens = countTokens({}, compressedMessages, enableCountThinking);
    const auto durationMs
        = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - startTime
        )
                                   .count());
    vm.text = fmt::format(
        "Summarizied LLM Context {}->{}/{} · {}",
        oldTokens,
        newTokens,
        modelContenxtMaxToken,
        agentxx::util::formatDurationMilliseconds(durationMs)
    );
    vm.durationMs = durationMs;
    session->updateViewMessage(vm);
    if (session->io) {
        session->io->sendToPeer(agentxx::agent::WireDelta{
            .type    = agentxx::agent::WireDelta::Type::UpdateMessage,
            .seq     = ++session->deltaSeq,
            .message = std::make_shared<agentxx::agent::ViewMessage>(vm),
        });
        session->io->sendToPeer(agentxx::agent::WireContextStats{newTokens, modelContenxtMaxToken});
    }
    if (session->contextStats) {
        session->contextStats->contextTokens    = newTokens;
        session->contextStats->maxContextTokens = modelContenxtMaxToken;
    }

    co_return true;
}

SummarizationMiddlewareHandle::~SummarizationMiddlewareHandle() {
    unregisterFromBus();
}

void SummarizationMiddlewareHandle::registerOnBus(
    const std::shared_ptr<agentxx::event::EventBus>& bus
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
