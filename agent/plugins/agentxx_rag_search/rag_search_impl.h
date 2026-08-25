// agentxx_rag_search 插件 —— 工具实现 (纯函数, 不含 C ABI 胶水)
// - 从 libagentxx src/tools/rag_search 拆分: 同名工具同行为 (agentxx_rag_search)
// - 头文件-only: 插件入口与测试共同包含, 保证插件行为与测试覆盖一致
// - 依赖: agentxx_util (HttpClient / 字符串工具 / 异常捕获) + fmt + neograph(json)
// - 与原实现的差异点:
//   - 原版 asio 协程接口改为同步实现 (插件 execute 回调已运行在宿主线程池,
//     阻塞安全); embedding 网络调用经局部 io_context 驱动至完成 (与
//     agentxx_websearch 的 runSync 同模式)
//   - embedding 客户端经 EmbedFn 函数对象注入: 插件入口装配真实 HTTP 版本,
//     测试可注入假实现 (不依赖网络), 与 filesystem_impl.h 的 IsCancelledFn
//     注入同思路; 分块/相似度/检索逻辑为纯函数, 行为与原版一致
#pragma once

#include "agentxx/util/exception.h"
#include "agentxx/util/http_client.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace agentxx_rag_plugin {

// =========================================================================
// Text Chunk Splitting — split modes & config (原 RAGSearchTool::VectorStore)
// =========================================================================

enum class SplitMode {
    FixedLength,                 // Only fixed-length UTF-8 splitting
    Character,                   // Only character delimiter splitting
    Structural,                  // Only markdown-structure splitting
    StructuralThenChar,          // Structural -> character fallback
    StructuralThenCharThenFixed, // Structural -> character -> fixed-length
};

struct SplitConfig {
    SplitMode mode = SplitMode::StructuralThenCharThenFixed;

    /// Block item max utf8 length
    size_t maxUtf8Length = 256;

    /// 20%
    double overlapPercent = 20.0;

    /// Delimiters tried in priority order (most significant first)
    std::vector<std::string> delimiters{
        "\n\n",
        "\n",
        "。",
        "！",
        "？",
        "；",
        "，",
        ". ",
        "! ",
        "? ",
        "; ",
        ", ",
    };
};

/// RAG 文档块 (原 RAGSearchTool::Document)
struct Document {
    std::string                      id;
    std::string                      title;
    std::vector<std::string>         content;
    std::string                      source;
    std::vector<std::vector<double>> embedding;
};

/// 批量 embedding 回调: 输入文本块列表, 输出等长向量列表或错误信息
/// (插件入口装配 HttpClient 实现; 测试注入假实现)
using EmbedFn = std::function<
    std::expected<std::vector<std::vector<double>>, std::string>(
        const std::vector<std::string>&
    )>;

// =========================================================================
// 分块纯函数 (与原 lib 实现逐行一致)
// =========================================================================

inline std::vector<std::string> splitByFixedLength(
    std::string_view text,
    size_t           blockSize      = 256,
    double           overlapPercent = 0.0
) {
    if (overlapPercent <= 0.0 || overlapPercent >= 100.0) {
        auto result = std::vector<std::string>{};
        for (size_t index = 0; index < text.size();) {
            auto target = agentxx::util::findIndexByUtf8Length(text, blockSize, index);
            if (target <= 0) {
                target = text.size();
            }
            result.push_back(std::string{text.substr(index, target - index)});
            index = target;
        }
        return result;
    }

    size_t overlapChars = static_cast<size_t>(blockSize * overlapPercent / 100.0);
    size_t stepChars    = blockSize - overlapChars;
    if (stepChars == 0) {
        stepChars = 1;
    }

    auto   result = std::vector<std::string>{};
    size_t index  = 0;
    while (index < text.size()) {
        auto target = agentxx::util::findIndexByUtf8Length(text, blockSize, index);
        if (target <= 0) {
            target = text.size();
        }
        result.push_back(std::string{text.substr(index, target - index)});
        if (target >= text.size()) {
            break;
        }

        auto nextStart = agentxx::util::findIndexByUtf8Length(text, stepChars, index);
        if (nextStart <= 0 || nextStart >= text.size()) {
            break;
        }
        index = nextStart;
    }
    return result;
}

inline std::vector<std::string> splitByDelimiter(std::string_view text, std::string_view delimiter) {
    auto result = std::vector<std::string>{};
    if (delimiter.empty()) {
        if (!text.empty()) {
            result.push_back(std::string{text});
        }
        return result;
    }
    size_t start = 0;
    size_t end;
    while ((end = text.find(delimiter, start)) != std::string_view::npos) {
        auto part = text.substr(start, end - start);
        if (!part.empty()) {
            result.push_back(std::string{part});
        }
        start = end + delimiter.size();
    }
    auto last = text.substr(start);
    if (!last.empty()) {
        result.push_back(std::string{last});
    }
    return result;
}

inline std::vector<std::string> splitByStructure(std::string_view text) {
    std::vector<std::string> blocks;
    size_t                   len       = text.size();
    size_t                   lineStart = 0;
    std::string              currentBlock;
    bool                     inCodeBlock       = false;
    [[maybe_unused]] bool    currentIsHeading  = false;
    bool                     currentIsListItem = false;

    auto flushBlock = [&]() {
        if (!currentBlock.empty()) {
            blocks.push_back(std::move(currentBlock));
            currentBlock.clear();
        }
    };

    auto isCodeFence = [](std::string_view line) -> bool {
        return line.size() >= 3 && line.substr(0, 3) == "```";
    };

    auto isHeading = [](std::string_view line) -> bool {
        return !line.empty() && line[0] == '#' && line.size() > 1 && line[1] == ' ';
    };

    auto isListItem = [](std::string_view line) -> bool {
        if (line.empty()) {
            return false;
        }
        if (line.size() >= 2 && (line.substr(0, 2) == "- " || line.substr(0, 2) == "* ")) {
            return true;
        }
        if (line.size() >= 3 && std::isdigit(static_cast<unsigned char>(line[0])) && line[1] == '.'
            && line[2] == ' ') {
            return true;
        }
        return false;
    };

    for (size_t i = 0; i <= len; ++i) {
        if (i == len || text[i] == '\n') {
            std::string_view line = text.substr(lineStart, i - lineStart);
            lineStart             = i + 1;

            if (isCodeFence(line)) {
                if (!inCodeBlock) {
                    flushBlock();
                    inCodeBlock  = true;
                    currentBlock = std::string{line};
                } else {
                    currentBlock += fmt::format("\n{}", line);
                    flushBlock();
                    inCodeBlock = false;
                }
                currentIsHeading  = false;
                currentIsListItem = false;
                continue;
            }

            if (inCodeBlock) {
                if (!currentBlock.empty()) {
                    currentBlock += "\n";
                }
                currentBlock += std::string{line};
                continue;
            }

            if (line.empty()) {
                flushBlock();
                currentIsHeading  = false;
                currentIsListItem = false;
            } else if (isHeading(line)) {
                flushBlock();
                currentBlock      = std::string{line};
                currentIsHeading  = true;
                currentIsListItem = false;
            } else if (isListItem(line)) {
                if (!currentIsListItem) {
                    flushBlock();
                }
                if (!currentBlock.empty()) {
                    currentBlock += "\n";
                }
                currentBlock      += std::string{line};
                currentIsListItem  = true;
                currentIsHeading   = false;
            } else {
                if (!currentBlock.empty()) {
                    currentBlock += "\n";
                }
                currentBlock      += std::string{line};
                currentIsHeading   = false;
                currentIsListItem  = false;
            }
        }
    }
    flushBlock();

    // Merge heading with its following content block
    std::vector<std::string> merged;
    for (size_t i = 0; i < blocks.size();) {
        bool blockIsHeading = !blocks[i].empty() && blocks[i][0] == '#' && blocks[i].size() > 1
                              && blocks[i][1] == ' ';
        if (blockIsHeading && i + 1 < blocks.size() && !blocks[i + 1].empty()
            && blocks[i + 1][0] != '#') {
            std::string mergedBlock = fmt::format("{}\n\n{}", blocks[i], blocks[i + 1]);
            merged.push_back(std::move(mergedBlock));
            i += 2;
        } else {
            merged.push_back(std::move(blocks[i]));
            i++;
        }
    }

    return merged;
}

inline std::vector<std::string> splitByDelimiters(
    std::string_view                text,
    size_t                          maxUtf8Length,
    const std::vector<std::string>& delimiters
) {
    if (text.empty()) {
        return {};
    }

    for (const auto& delim : delimiters) {
        auto parts = splitByDelimiter(text, delim);
        if (parts.size() <= 1) {
            continue;
        }

        std::vector<std::string> result;
        bool                     allFit = true;
        for (auto& part : parts) {
            if (agentxx::util::utf8GetLength(part) > maxUtf8Length) {
                allFit        = false;
                auto subParts = splitByDelimiters(part, maxUtf8Length, delimiters);
                for (auto& sp : subParts) {
                    result.push_back(std::move(sp));
                }
            } else {
                result.push_back(std::move(part));
            }
        }

        if (allFit) {
            return result;
        }
        if (!result.empty()) {
            return result;
        }
    }

    return splitByFixedLength(text, maxUtf8Length);
}

inline std::vector<std::string> applyChunkOverlap(
    const std::vector<std::string>& chunks,
    size_t                          maxUtf8Length,
    double                          overlapPercent
) {
    if (overlapPercent <= 0.0 || chunks.size() <= 1) {
        return chunks;
    }

    size_t overlapChars = static_cast<size_t>(maxUtf8Length * overlapPercent / 100.0);
    if (overlapChars == 0) {
        return chunks;
    }

    std::vector<std::string> result;
    result.reserve(chunks.size());
    result.push_back(chunks[0]);

    for (size_t i = 1; i < chunks.size(); ++i) {
        const auto& prev        = result.back();
        size_t      prevUtf8Len = agentxx::util::utf8GetLength(prev);

        if (prevUtf8Len <= overlapChars) {
            result.push_back(chunks[i]);
            continue;
        }

        size_t overlapStart
            = agentxx::util::findIndexByUtf8Length(prev, prevUtf8Len - overlapChars);
        if (overlapStart == 0) {
            result.push_back(chunks[i]);
            continue;
        }

        result.push_back(fmt::format("{}{}", prev.substr(overlapStart), chunks[i]));
    }

    return result;
}

inline std::vector<std::string> splitTextToChunks(std::string_view text, const SplitConfig& config) {
    if (text.empty()) {
        return {};
    }

    size_t maxLen     = config.maxUtf8Length;
    double overlapPct = config.overlapPercent;

    switch (config.mode) {
        case SplitMode::FixedLength:
            return splitByFixedLength(text, maxLen, overlapPct);
        case SplitMode::Character:
            return applyChunkOverlap(
                splitByDelimiters(text, maxLen, config.delimiters),
                maxLen,
                overlapPct
            );
        case SplitMode::Structural: {
            auto                     blocks = splitByStructure(text);
            std::vector<std::string> result;
            for (auto& block : blocks) {
                if (agentxx::util::utf8GetLength(block) <= maxLen) {
                    result.push_back(std::move(block));
                } else {
                    auto fixedParts = splitByFixedLength(block, maxLen);
                    for (auto& fp : fixedParts) {
                        result.push_back(std::move(fp));
                    }
                }
            }
            return applyChunkOverlap(result, maxLen, overlapPct);
        }
        case SplitMode::StructuralThenChar: {
            auto                     blocks = splitByStructure(text);
            std::vector<std::string> result;
            for (auto& block : blocks) {
                if (agentxx::util::utf8GetLength(block) <= maxLen) {
                    result.push_back(std::move(block));
                } else {
                    auto subParts = splitByDelimiters(block, maxLen, config.delimiters);
                    for (auto& sp : subParts) {
                        result.push_back(std::move(sp));
                    }
                }
            }
            return applyChunkOverlap(result, maxLen, overlapPct);
        }
        case SplitMode::StructuralThenCharThenFixed: {
            auto                     blocks = splitByStructure(text);
            std::vector<std::string> result;
            for (auto& block : blocks) {
                if (agentxx::util::utf8GetLength(block) <= maxLen) {
                    result.push_back(std::move(block));
                } else {
                    auto subParts = splitByDelimiters(block, maxLen, config.delimiters);
                    for (auto& sp : subParts) {
                        if (agentxx::util::utf8GetLength(sp) <= maxLen) {
                            result.push_back(std::move(sp));
                        } else {
                            auto fixedParts = splitByFixedLength(sp, maxLen);
                            for (auto& fp : fixedParts) {
                                result.push_back(std::move(fp));
                            }
                        }
                    }
                }
            }
            return applyChunkOverlap(result, maxLen, overlapPct);
        }
    }

    return applyChunkOverlap(splitByFixedLength(text, maxLen), maxLen, overlapPct);
}

// =========================================================================
// 相似度 / 向量库 (原 RAGSearchTool::cosineSimilarity / VectorStore)
// =========================================================================

inline double cosineSimilarity(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size() || a.empty()) {
        return 0.0;
    }
    double dot = 0.0, norm_a = 0.0, norm_b = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot    += a[i] * b[i];
        norm_a += a[i] * a[i];
        norm_b += b[i] * b[i];
    }
    double denom = std::sqrt(norm_a) * std::sqrt(norm_b);
    return denom > 0.0 ? dot / denom : 0.0;
}

/// 内存向量库 (embedder 经 EmbedFn 注入; 全部接口同步, 仅宿主线程池内调用)
class VectorStore {
public:
    explicit VectorStore(EmbedFn in_embedder) : embedder(std::move(in_embedder)) {}

    VectorStore(EmbedFn in_embedder, const SplitConfig& in_splitCfg)
        : splitConfig(in_splitCfg),
          embedder(std::move(in_embedder)) {}

    /// 扫描路径下的 .md 文档并分块 (单文件读取失败仅记录日志, 不中断整体扫描)
    std::vector<Document> scanDocument(const std::vector<std::string>& pathlist) {
        auto result = std::vector<Document>{};

        auto onAppendItem = [&](const std::string& path) -> bool {
            auto filepath = std::filesystem::path{path};
            if (filepath.extension() == ".md") {
                std::ifstream stream;
                // 单个文件读取失败仅记录日志, 不中断整体扫描
                return agentxx::util::catchError<bool>(
                    [&]() -> bool {
                        stream.open(path);
                        if (!stream) {
                            auto ec = std::error_code{errno, std::system_category()};
                            throw std::runtime_error{
                                fmt::format(R"(Can not open file. Error: {})", ec.message())
                            };
                        }
                        auto content = std::string{
                            std::istreambuf_iterator<char>(stream),
                            std::istreambuf_iterator<char>()
                        };
                        stream.close();

                        result.push_back(Document{
                            .id      = std::to_string(result.size()),
                            .title   = filepath.filename().generic_string(),
                            .content = splitTextToChunks(content, splitConfig),
                            .source  = std::string{path},
                        });
                        return true;
                    },
                    [&](std::string errmsg) -> bool {
                        XX_LOGD("RAG/scanDocument item exception: {} / {}", path, errmsg);
                        return false;
                    }
                );
            }
            return false;
        };

        auto content = std::string{};
        for (const auto& itemPath : pathlist) {
            content += fmt::format("try: {}\n", itemPath);
            std::error_code ec;
            if (std::filesystem::is_directory(itemPath, ec)) {
                for (const auto& entity :
                     std::filesystem::recursive_directory_iterator(itemPath, ec)) {
                    if (entity.is_regular_file()) {
                        if (onAppendItem(entity.path().generic_string())) {
                            auto& doc  = result.back();
                            content   += fmt::format(
                                "┣━ ✅ Load success: `{}`(Block {} | {} )\n",
                                doc.title,
                                doc.content.size(),
                                itemPath
                            );
                        }
                    }
                }
            } else if (std::filesystem::is_regular_file(itemPath, ec)) {
                if (onAppendItem(itemPath)) {
                    auto& doc  = result.back();
                    content   += fmt::format(
                        "┣━ ✅ Load success: `{}`(Block {} | {} )\n",
                        doc.title,
                        doc.content.size(),
                        itemPath
                    );
                }
            }
        }
        XX_LOGD(
            R"_(
┏━━━━━━ RAG Docs Load ━━━━━━┓
{}
┗━━━━━━ RAG Docs Load ━━━━━━┛
)_",
            content
        );

        return result;
    }

    /// 批量生成文档 embedding 并入库 (embedding 数量与 chunk 数不一致时整体失败)
    bool addDocuments(std::vector<Document>&& appendDocs) {
        if (appendDocs.empty()) {
            return true;
        }
        // Batch embed all documents at once
        std::vector<std::string> texts;
        for (auto& doc : appendDocs) {
            texts.insert(texts.end(), doc.content.begin(), doc.content.end());
        }

        auto embeddings = embedder(texts);

        if (embeddings.has_value()) {
            auto& embVec = embeddings.value();
            if (embVec.size() != texts.size()) {
                // embedding 数量与输入 chunk 数不一致 (部分失败/截断),
                // 直接返回避免迭代器越界
                XX_LOGE(
                    "RAG addDocuments: embedding count {} != text chunk count {}",
                    embVec.size(),
                    texts.size()
                );
                return false;
            }
            auto start = embVec.begin();
            for (size_t i = 0; i < appendDocs.size(); ++i) {
                if (false == appendDocs[i].content.empty()) {
                    appendDocs[i].embedding
                        = std::vector<std::vector<double>>{start, start + appendDocs[i].content.size()};
                    start += appendDocs[i].content.size();
                    docs.push_back(std::move(appendDocs[i]));
                }
            }
            return true;
        }
        return false;
    }

    /// 余弦相似度检索 top_k 文档块 (查询文本同样分块后取平均相似度)
    std::expected<std::vector<std::tuple<const Document&, size_t, double>>, std::string>
        search(std::string_view query, size_t top_k = 3) const {
        auto queryVecExp = embedder(splitTextToChunks(query, splitConfig));
        if (false == queryVecExp.has_value()) {
            return std::unexpected{queryVecExp.error()};
        }
        auto& queryVectors = queryVecExp.value();
        if (queryVectors.empty()) {
            return std::vector<std::tuple<const Document&, size_t, double>>{};
        }

        /// <docIndex, contentIndex, sim>
        auto scores = std::vector<std::tuple<size_t, size_t, double>>{};
        for (size_t i = 0; i < docs.size(); ++i) {
            if (!docs[i].embedding.empty()) {
                for (size_t j = 0; j < docs[i].embedding.size(); ++j) {
                    double sim = 0;
                    for (const auto& queryVecItem : queryVectors) {
                        sim += cosineSimilarity(queryVecItem, docs[i].embedding[j]);
                    }
                    scores.push_back({i, j, sim / queryVectors.size()});
                }
            }
        }

        std::sort(
            scores.begin(),
            scores.end(),
            [](const std::tuple<size_t, size_t, double>& a,
               const std::tuple<size_t, size_t, double>& b) {
                const auto& [_a1, _a2, aSim] = a;
                const auto& [_b1, _b2, bSim] = b;
                return aSim > bSim;
            }
        );

        auto results = std::vector<std::tuple<const Document&, size_t, double>>{};
        for (size_t i = 0; i < top_k && i < scores.size(); ++i) {
            const auto [docIndex, contentIndex, sim] = scores[i];
            results.push_back({docs[docIndex], contentIndex, sim});
        }
        return results;
    }

protected:
    SplitConfig splitConfig{};
    EmbedFn     embedder;
    std::vector<Document> docs;
};

// =========================================================================
// 真实 embedding 客户端 (原 EmbeddingClient: OpenAI 兼容 /embeddings 接口)
// =========================================================================

/// 构造真实 EmbedFn (OpenAI 兼容 POST {baseUrl}/embeddings; 局部 io_context
/// 同步驱动, 仅宿主线程池内调用; readChunkTimeout=15s 与原实现一致)
inline EmbedFn makeHttpEmbedder(std::string baseUrl, std::string model) {
    return [baseUrl = std::move(baseUrl), model = std::move(model)](
               const std::vector<std::string>& texts
           ) -> std::expected<std::vector<std::vector<double>>, std::string> {
        if (texts.empty()) {
            return std::vector<std::vector<double>>{};
        }

        auto body     = neograph::json::object();
        body["model"] = model;
        body["input"] = neograph::json(texts);

        // 与原 EmbeddingClient 一致: 不携带额外请求头 (embedding 服务多为
        // 本地部署, 原实现即空 headers)
        asio::io_context io;
        auto             respExp = agentxx::util::HttpClient::postAsync(
            fmt::format("{}/embeddings", baseUrl),
            body,
            agentxx::util::HeaderMap{},
            agentxx::util::HttpClient::RequestConfig{.readChunkTimeout = std::chrono::seconds{15}
            }
        );
        std::expected<agentxx::util::HttpResponse, std::string> resp;
        // awaitable 为 move-only: 必须 move 捕获并 co_await 右值 (不可拷贝)
        asio::co_spawn(
            io,
            [&resp, w = std::move(respExp)]() mutable -> asio::awaitable<void> {
                resp = co_await std::move(w);
            },
            asio::detached
        );
        io.run();

        if (false == resp.has_value()
            || false == agentxx::util::HttpClient::respIsSucc(resp.value())) {
            std::string str;
            if (resp.has_value()) {
                str = std::to_string(resp.value().status);
            } else {
                str = resp.error();
            }

            return std::unexpected{fmt::format("[embedding] API error: {}", str)};
        }

        try {
            auto                             respBody = neograph::json::parse(resp.value().body);
            std::vector<std::vector<double>> embeddings;

            for (const auto& item : respBody["data"]) {
                std::vector<double> vec;
                for (const auto& v : item["embedding"]) {
                    vec.push_back(v.get<double>());
                }
                embeddings.push_back(std::move(vec));
            }

            return embeddings;
        } catch (const std::exception& ex) {
            return std::unexpected{fmt::format("[embedding] parse response failed: {}", ex.what())};
        }
    };
}

} // namespace agentxx_rag_plugin
