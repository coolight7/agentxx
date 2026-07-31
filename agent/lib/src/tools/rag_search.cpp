#include "agentxx/tools/rag_search.h"

#include "agentxx/util/http_client.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include "fmt/format.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace agentxx {
namespace tools {

EmbeddingClient::EmbeddingClient(
    std::string_view in_baseUrl,
    std::string_view in_apiKey,
    std::string_view in_model
) :
    baseUrl(in_baseUrl),
    apiKey(in_apiKey),
    model(in_model) {}

asio::awaitable<std::expected<std::vector<std::vector<double>>, std::string>>
    EmbeddingClient::embed_batch(const std::vector<std::string>& texts) const {
    if (texts.empty()) {
        co_return std::expected<std::vector<std::vector<double>>, std::string>{
            std::vector<std::vector<double>>{}
        };
    }

    auto body     = neograph::json::object();
    body["model"] = model;
    body["input"] = neograph::json(texts);

    auto resp = co_await agentxx::util::HttpClient::postAsync(
        fmt::format("{}/embeddings", baseUrl),
        body,
        {},
        agentxx::util::HttpClient::RequestConfig{.readTimeout = std::chrono::seconds{15}}
    );

    if (false == resp.has_value() || false == agentxx::util::HttpClient::respIsSucc(resp.value())) {
        std::string str;
        if (resp.has_value()) {
            str = std::to_string(resp.value().status);
        } else {
            str = resp.error();
        }

        co_return std::unexpected{fmt::format("[embedding] API error: {}", str)};
    }

    auto                             respBody = neograph::json::parse(resp.value().body);
    std::vector<std::vector<double>> embeddings;

    for (const auto& item : respBody["data"]) {
        std::vector<double> vec;
        for (const auto& v : item["embedding"]) {
            vec.push_back(v.get<double>());
        }
        embeddings.push_back(std::move(vec));
    }

    co_return std::expected<std::vector<std::vector<double>>, std::string>{embeddings};
}

double RAGSearchTool::cosineSimilarity(const std::vector<double>& a, const std::vector<double>& b) {
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

RAGSearchTool::VectorStore::VectorStore(std::shared_ptr<EmbeddingClient> in_embedder) :
    embedder(std::move(in_embedder)) {}

RAGSearchTool::VectorStore::VectorStore(
    std::shared_ptr<EmbeddingClient> in_embedder,
    const SplitConfig&               in_splitCfg
) :
    splitConfig(in_splitCfg),
    embedder(std::move(in_embedder)) {}

std::vector<std::string> RAGSearchTool::VectorStore::splitByFixedLength(
    std::string_view text,
    size_t           blockSize,
    double           overlapPercent
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

std::vector<std::string> RAGSearchTool::VectorStore::splitByDelimiter(
    std::string_view text,
    std::string_view delimiter
) {
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

std::vector<std::string> RAGSearchTool::VectorStore::splitByStructure(std::string_view text) {
    std::vector<std::string> blocks;
    size_t                   len       = text.size();
    size_t                   lineStart = 0;
    std::string              currentBlock;
    bool                     inCodeBlock   = false;
    bool [[maybe_unused]] currentIsHeading = false;
    bool currentIsListItem                 = false;

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
            std::string mergedBlock = blocks[i] + "\n\n" + blocks[i + 1];
            merged.push_back(std::move(mergedBlock));
            i += 2;
        } else {
            merged.push_back(std::move(blocks[i]));
            i++;
        }
    }

    return merged;
}

std::vector<std::string> RAGSearchTool::VectorStore::splitByDelimiters(
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

std::vector<std::string> RAGSearchTool::VectorStore::applyChunkOverlap(
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

std::vector<std::string> RAGSearchTool::VectorStore::splitTextToChunks(
    std::string_view   text,
    const SplitConfig& config
) {
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

asio::awaitable<std::vector<RAGSearchTool::Document>>
    RAGSearchTool::VectorStore::scanDocument(const std::vector<std::string>& pathlist) {
    auto result = std::vector<Document>{};

    auto onAppendItem = [&](const std::string& path) -> bool {
        auto filepath = std::filesystem::path{path};
        if (filepath.extension() == ".md") {
            std::ifstream stream;
            try {
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
                    .content = VectorStore::splitTextToChunks(content, splitConfig),
                    .source  = std::string{path},
                });
                return true;
            } catch (const std::exception& e) {
                XX_LOGD("RAG/scanDocument item exception: {} / {}", path, e.what());
            }
        }
        return false;
    };

    auto content = std::string{};
    for (const auto& itemPath : pathlist) {
        content += fmt::format("try: {}\n", itemPath);
        if (std::filesystem::is_directory(itemPath)) {
            for (const auto& entity : std::filesystem::recursive_directory_iterator(itemPath)) {
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
        } else if (std::filesystem::is_regular_file(itemPath)) {
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

    co_return result;
}

asio::awaitable<bool> RAGSearchTool::VectorStore::addDocuments(std::vector<Document>&& appendDocs) {
    if (appendDocs.empty()) {
        co_return true;
    }
    // Batch embed all documents at once
    std::vector<std::string> texts;
    for (auto& doc : appendDocs) {
        texts.insert(texts.end(), doc.content.begin(), doc.content.end());
    }

    auto embeddings = co_await embedder->embed_batch(texts);

    if (embeddings.has_value()) {
        auto& embVec = embeddings.value();
        if (embVec.size() != texts.size()) {
            // embedding 数量与输入 chunk 数不一致 (部分失败/截断), 直接返回避免迭代器越界
            XX_LOGE(
                "RAG addDocuments: embedding count {} != text chunk count {}",
                embVec.size(),
                texts.size()
            );
            co_return false;
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
        co_return true;
    }
    co_return false;
}

asio::awaitable<std::expected<
    std::vector<std::tuple<const RAGSearchTool::Document&, size_t, double>>,
    std::string>>
    RAGSearchTool::VectorStore::search(std::string_view query, size_t top_k) const {
    auto queryVec
        = co_await embedder->embed_batch(VectorStore::splitTextToChunks(query, splitConfig));
    if (false == queryVec.has_value()) {
        co_return std::unexpected{queryVec.error()};
    }
    if (queryVec.value().empty()) {
        co_return std::
            expected<std::vector<std::tuple<const Document&, size_t, double>>, std::string>{};
    }

    /// <docIndex, contentIndex, sim>
    auto scores = std::vector<std::tuple<size_t, size_t, double>>{};
    for (size_t i = 0; i < docs.size(); ++i) {
        if (!docs[i].embedding.empty()) {
            for (size_t j = 0; j < docs[i].embedding.size(); ++j) {
                double sim = 0;
                for (const auto& queryVecItem : queryVec.value()) {
                    sim += cosineSimilarity(queryVecItem, docs[i].embedding[j]);
                }
                scores.push_back({i, j, sim / queryVec->size()});
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
    co_return std::expected<std::vector<std::tuple<const Document&, size_t, double>>, std::string>{
        std::move(results)
    };
}

RAGSearchTool::RAGSearchTool(
    std::shared_ptr<VectorStore>                in_store,
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    XXToolBase("rag_search", in_agentContext, false, true),
    store(in_store) {}

neograph::ChatTool RAGSearchTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
                       {"type", "object"},
                       {
                "properties",
                {
                    {
                        "query",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("query")},
                        },
                    },
                    {
                        "top_k",
                        {
                            {"type", "integer"},
                            {"description", prompt.getArg("top_k")},
                        },
                    },
                },
            }, {
                "required",
                neograph::json::array({"query"}),
            }, },
    };
}

asio::awaitable<std::string> RAGSearchTool::execute_async(const neograph::json& arguments) {
    auto query = arguments.value("query", "");
    int  top_k = arguments.value("top_k", 3);

    auto results = co_await store->search(query, top_k);

    if (false == results.has_value()) {
        co_return fmt::format("Search error: {}", results.error());
    }
    if (results->empty()) {
        co_return "No relevant documents found for: " + query;
    }

    auto output = neograph::json::array();
    for (const auto& [doc, contentIndex, score] : results.value()) {
        output.push_back({
            {"id",           doc.id                             },
            {"title",        doc.title                          },
            {"contentIndex", contentIndex                       },
            {"content",      doc.content[contentIndex]          },
            {"source",       doc.source                         },
            {"similarity",   std::round(score * 1000.0) / 1000.0},
        });
    }

    co_return output.dump(2);
}

} // namespace tools
} // namespace agentxx
