/// 上下文核心功能点的实现 (token 估算规则 / 请求编解码 / 核心实现登记)
///
/// 一条实现、两个入口:
/// - 同步快路径: TPS 计算直接调 [TokenEstimator::estimateText] (不经功能点,
///   插件实现不参与这条路径);
/// - 功能点: `agentxx.context.countTokens` 的核心实现调用同一份规则。
#include "agentxx/feature/points.h"

#include "agentxx/feature/registry.h"
#include "agentxx/util/exception.h"
#include "fmt/format.h"

namespace agentxx {
namespace feature {

// ==================== TokenEstimator ====================

size_t TokenEstimator::estimateText(std::string_view text) const {
    size_t unicodeCount = 0;
    size_t asciiCount   = 0;
    for (size_t i = 0, step = 0; i < text.size(); i += step) {
        const unsigned char byte = static_cast<unsigned char>(text[i]);
        if (byte >= 0xF8) {
            // 0xF8-0xFF: 无效 UTF-8 前导 (5/6 字节编码已被 RFC 3629 废弃),
            // 按 ascii 单字节处理, 避免吞掉后续字节少计
            step = 1;
            ++asciiCount;
            continue;
        }
        if (byte >= 0xF0) {
            step = 4; // 4 字节前导 0xF0-0xF7
        } else if (byte >= 0xE0) {
            step = 3; // 3 字节前导 0xE0-0xEF
        } else if (byte >= 0xC0) {
            step = 2; // 2 字节前导 0xC0-0xDF
        } else {
            // ascii 0x00-0x7F / 续字节 0x80-0xBF (单独出现无效): 单字节处理
            step = 1;
            ++asciiCount;
            continue;
        }
        ++unicodeCount;
    }
    // ascii 与 unicode 分别折算取整后再相加 (与文档/测试语义一致:
    // "ascii + unicode 分别折算"), 避免先相加再整体截断导致高估
    return static_cast<size_t>(static_cast<double>(unicodeCount) / unicodeCharsPerToken)
           + static_cast<size_t>(static_cast<double>(asciiCount) / asciiCharsPerToken);
}

size_t TokenEstimator::estimateMessages(
    const std::vector<neograph::ChatMessage>& messages,
    bool                                      countThinking
) const {
    size_t count = 0;
    for (const auto& item : messages) {
        count += static_cast<size_t>(extraTokensPerMessage) + estimateText(item.role)
                 + estimateText(item.content);
        if (countThinking) {
            count += estimateText(item.reasoning_content);
        }
        for (const auto& tool : item.tool_calls) {
            count += estimateText(tool.id) + estimateText(tool.name) + estimateText(tool.arguments);
        }
        count += static_cast<size_t>(tokensPerImage * static_cast<double>(item.image_urls.size()));
        // 音视频附件同样按图片 token 估算 (各家 API 对多媒体计费粒度不一, 粗略按图片计)
        count += static_cast<size_t>(tokensPerImage * static_cast<double>(item.audio_urls.size()));
        count += static_cast<size_t>(tokensPerImage * static_cast<double>(item.video_urls.size()));
    }
    return count;
}

std::string TokenEstimator::identityOf(const CountTokensRequest& req) const {
    const std::string kind
        = req.kind.empty() ? (req.messages.empty() ? "text" : "messages") : req.kind;
    std::string payload = req.text;
    if (!req.messages.empty()) {
        // messages 形态: 按拼接后的文本哈希 (逐条角色 + 正文 + 工具调用)
        payload.clear();
        for (const auto& msg : req.messages) {
            payload.append(msg.role);
            payload.push_back('\x1f');
            payload.append(msg.content);
            payload.push_back('\x1e');
            for (const auto& tool : msg.tool_calls) {
                payload.append(tool.name);
                payload.push_back('\x1d');
                payload.append(tool.arguments);
                payload.push_back('\x1e');
            }
        }
        if (req.countThinking) {
            payload.append("\x1f?thinking");
        }
    }
    return fmt::format(
        "{}|{}|{:016x}",
        req.model.empty() ? "-" : req.model,
        kind,
        utilxx_base::hash::fnv1a64(payload)
    );
}

std::string TokenEstimator::requestToJson(const CountTokensRequest& req) {
    return Codec<CountTokensRequest>::toJson(req);
}

// ==================== 请求 / 结果的 JSON 编解码 ====================

std::string Codec<CountTokensRequest>::toJson(const CountTokensRequest& req) {
    utilxx_base::Json json = utilxx_base::Json::object();
    json["text"] = req.text;
    if (!req.messages.empty()) {
        neograph::json neoMsgs;
        neograph::to_json(neoMsgs, req.messages);
        json["messages"] = agentxx::util::fromNeographJson(neoMsgs);
    }
    json["model"]         = req.model;
    json["kind"]          = req.kind;
    json["countThinking"] = req.countThinking;
    return json.dump();
}

std::optional<CountTokensRequest> Codec<CountTokensRequest>::fromJson(std::string_view text) {
    return utilxx_base::catchError<std::optional<CountTokensRequest>>(
        [&]() -> std::optional<CountTokensRequest> {
            const auto json = utilxx_base::Json::parse(text);
            if (!json.is_object()) {
                return std::nullopt;
            }
            CountTokensRequest req;
            req.text          = json.value<std::string>("text", "");
            req.model         = json.value<std::string>("model", "");
            req.kind          = json.value<std::string>("kind", "");
            req.countThinking = json.value<bool>("countThinking", false);
            if (auto it = json.find("messages"); it != json.end() && it->is_array()) {
                const auto neoJson = agentxx::util::toNeographJson(*it);
                for (const auto& item : neoJson) {
                    neograph::ChatMessage msg;
                    neograph::from_json(item, msg);
                    req.messages.push_back(std::move(msg));
                }
            }
            if (req.text.empty() && req.messages.empty() && req.kind.empty()) {
                return std::nullopt; // 两个输入都没有: 参数不合法
            }
            return req;
        },
        [](std::string errmsg) -> std::optional<CountTokensRequest> {
            XX_LOGW("功能点: Token 估算请求解码失败: {}", errmsg);
            return std::nullopt;
        }
    );
}

std::optional<CountTokensValue> Codec<CountTokensValue>::fromJson(std::string_view text) {
    return utilxx_base::catchError<std::optional<CountTokensValue>>(
        [&]() -> std::optional<CountTokensValue> {
            const auto json = utilxx_base::Json::parse(text);
            if (!json.is_object()) {
                return std::nullopt;
            }
            CountTokensValue value;
            value.tokens = json.value<int64_t>("tokens", -1);
            if (value.tokens < 0) {
                return std::nullopt;
            }
            return value;
        },
        [](std::string errmsg) -> std::optional<CountTokensValue> {
            XX_LOGW("功能点: Token 估算结果解码失败: {}", errmsg);
            return std::nullopt;
        }
    );
}

std::string Codec<SummarizeRequest>::toJson(const SummarizeRequest& req) {
    utilxx_base::Json json = utilxx_base::Json::object();
    json["sessionId"]        = req.sessionId;
    if (!req.messages.empty()) {
        neograph::json neoMsgs;
        neograph::to_json(neoMsgs, req.messages);
        json["messages"] = agentxx::util::fromNeographJson(neoMsgs);
    }
    json["model"]            = req.model;
    json["targetTokens"]     = req.targetTokens;
    json["maxSummaryTokens"] = req.maxSummaryTokens;
    json["language"]         = req.language;
    return json.dump();
}

std::optional<SummarizeRequest> Codec<SummarizeRequest>::fromJson(std::string_view text) {
    return utilxx_base::catchError<std::optional<SummarizeRequest>>(
        [&]() -> std::optional<SummarizeRequest> {
            const auto json = utilxx_base::Json::parse(text);
            if (!json.is_object()) {
                return std::nullopt;
            }
            SummarizeRequest req;
            req.sessionId        = json.value<std::string>("sessionId", "");
            req.model            = json.value<std::string>("model", "");
            req.targetTokens     = json.value<int64_t>("targetTokens", 0);
            req.maxSummaryTokens = json.value<int64_t>("maxSummaryTokens", 0);
            req.language         = json.value<std::string>("language", "");
            if (auto it = json.find("messages"); it != json.end() && it->is_array()) {
                const auto neoJson = agentxx::util::toNeographJson(*it);
                for (const auto& item : neoJson) {
                    neograph::ChatMessage msg;
                    neograph::from_json(item, msg);
                    req.messages.push_back(std::move(msg));
                }
            }
            if (req.sessionId.empty() && req.messages.empty()) {
                return std::nullopt; // 算哪一份数据都没说清: 参数不合法
            }
            return req;
        },
        [](std::string errmsg) -> std::optional<SummarizeRequest> {
            XX_LOGW("功能点: 上下文压缩请求解码失败: {}", errmsg);
            return std::nullopt;
        }
    );
}

std::string Codec<SummarizeValue>::toJson(const SummarizeValue& value) {
    utilxx_base::Json json = utilxx_base::Json::object();
    json["summary"]   = value.summary;
    json["truncated"] = value.truncated;
    return json.dump();
}

std::optional<SummarizeValue> Codec<SummarizeValue>::fromJson(std::string_view text) {
    return utilxx_base::catchError<std::optional<SummarizeValue>>(
        [&]() -> std::optional<SummarizeValue> {
            const auto json = utilxx_base::Json::parse(text);
            if (!json.is_object() || !json.contains("summary")) {
                return std::nullopt;
            }
            SummarizeValue value;
            value.summary   = json.value<std::string>("summary", "");
            value.truncated = json.value<bool>("truncated", false);
            return value;
        },
        [](std::string errmsg) -> std::optional<SummarizeValue> {
            XX_LOGW("功能点: 上下文压缩结果解码失败: {}", errmsg);
            return std::nullopt;
        }
    );
}

std::string summarizeIdentity(const SummarizeRequest& req) {
    // 身份只含"这次算哪一份输入": 会话 + 消息内容指纹 + 模型
    // (不含 targetTokens 这类策略参数; 要不同策略请显式给 identity 或 refresh)
    std::string payload;
    for (const auto& msg : req.messages) {
        payload.append(msg.role);
        payload.push_back('\x1f');
        payload.append(msg.content);
        payload.push_back('\x1e');
    }
    if (payload.empty()) {
        payload = req.sessionId;
    }
    return fmt::format(
        "{}|{}|{:016x}",
        req.sessionId.empty() ? "-" : req.sessionId,
        req.model.empty() ? "-" : req.model,
        utilxx_base::hash::fnv1a64(payload)
    );
}

// ==================== 核心实现装配 ====================

ContextPoints registerContextPoints(Registry& registry, const ContextPointOptions& options) {
    ContextPoints out;
    auto estimator = std::make_shared<TokenEstimator>(options.estimator);

    // ---- agentxx.context.countTokens (可调; 按身份的值缓存) ----
    PointOptions countOpts;
    countOpts.title     = "文本 Token 估算";
    countOpts.depict    = "按模型口径估算一段文本或一组消息的 token 数";
    countOpts.callable  = true;
    countOpts.callDoc
        = "参数: text 或 messages (二选一, 两个都给时以 messages 为准), "
          "可选 model / kind / countThinking; 返回: {tokens}";
    countOpts.cache    = CacheMode::ByIdentity;
    countOpts.maxItems = 64;
    countOpts.maxBytes = 256u * 1024u;
    out.countTokens    = &registry.provide<CountTokensRequest, CountTokensValue>(
        points::kContextCountTokens,
        std::move(countOpts),
        [estimator](const CountTokensRequest& req) {
            return estimator->identityOf(req);
        }
    );
    out.countTokens->addCoreImpl(
        std::string{points::kOwnerCountTokens},
        0, // 核心层内的顺序 (跨层顺序由层决定: 插件层恒在 core 层之前)
        [estimator](const CountTokensRequest& req, const ImplContext&) 
            -> asio::awaitable<std::optional<CountTokensValue>> {
            CountTokensValue value;
            if (!req.messages.empty()) {
                value.tokens = static_cast<int64_t>(
                    estimator->estimateMessages(req.messages, req.countThinking)
                );
            } else {
                value.tokens = static_cast<int64_t>(estimator->estimateText(req.text));
            }
            co_return value;
        }
    );

    // ---- agentxx.context.summarize (本轮不可调; 结果由编排方写回会话) ----
    PointOptions summarizeOpts;
    summarizeOpts.title      = "上下文压缩";
    summarizeOpts.depict     = "把一段上下文压成一段摘要文本 (不写回会话)";
    summarizeOpts.callable   = false;
    summarizeOpts.cache      = CacheMode::None;
    summarizeOpts.implTimeoutMs = 0;
    out.summarize            = &registry.provide<SummarizeRequest, SummarizeValue>(
        points::kContextSummarize,
        std::move(summarizeOpts),
        [](const SummarizeRequest& req) {
            return summarizeIdentity(req);
        }
    );
    if (options.summarizeImpl) {
        auto impl = options.summarizeImpl;
        out.summarize->addCoreImpl(
            std::string{points::kOwnerSummarize},
            0,
            [impl](const SummarizeRequest& req, const ImplContext& ctx) 
                -> asio::awaitable<std::optional<SummarizeValue>> {
                co_return co_await impl(req, ctx);
            }
        );
    }
    return out;
}

} // namespace feature
} // namespace agentxx
