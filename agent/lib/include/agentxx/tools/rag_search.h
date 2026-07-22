#pragma once

#include "agentxx/tools/tool.h"
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace agentxx {
namespace tools {

class EmbeddingClient {
public:
  EmbeddingClient(std::string_view in_baseUrl, std::string_view in_apiKey,
                  std::string_view in_model);

  // Embed multiple texts in one API call
  asio::awaitable<std::expected<std::vector<std::vector<double>>, std::string>>
  embed_batch(const std::vector<std::string> &texts) const;

private:
  std::string baseUrl;
  std::string apiKey;
  std::string model;
};

class RAGSearchTool : public XXToolBase {
public:
  // =========================================================================
  // Vector Store — in-memory with cosine similarity search
  // =========================================================================
  struct Document {
    std::string id;
    std::string title;
    std::vector<std::string> content;
    std::string source;
    std::vector<std::vector<double>> embedding;
  };

  static double cosineSimilarity(const std::vector<double> &a,
                                 const std::vector<double> &b);

  class VectorStore {
  public:
    // =========================================================================
    // Text Chunk Splitting — split modes & config
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
          "\n\n", "\n", "。", "！", "？", "；",
          "，",   ". ", "! ", "? ", "; ", ", ",
      };
    };

    VectorStore(std::shared_ptr<EmbeddingClient> in_embedder);

    VectorStore(std::shared_ptr<EmbeddingClient> in_embedder,
                const SplitConfig &in_splitCfg);

    inline static std::vector<std::string>
    splitByFixedLength(std::string_view text, size_t blockSize = 256,
                       double overlapPercent = 0.0);

    // Split text by a single delimiter string
    inline static std::vector<std::string>
    splitByDelimiter(std::string_view text, std::string_view delimiter);

    // Split by markdown structure: headings, code blocks, lists, paragraphs
    inline static std::vector<std::string>
    splitByStructure(std::string_view text);

    // Split by character delimiters with length limit, falling back to
    // fixed-length if no delimiter produces small-enough chunks
    inline static std::vector<std::string>
    splitByDelimiters(std::string_view text, size_t maxUtf8Length,
                      const std::vector<std::string> &delimiters);

    // Apply overlap between adjacent chunks by prepending the tail of the
    // previous chunk to the current chunk. overlapPercent=0 disables overlap.
    inline static std::vector<std::string>
    applyChunkOverlap(const std::vector<std::string> &chunks,
                      size_t maxUtf8Length, double overlapPercent);

    // Main entry: split text into chunks according to config, guaranteeing
    // every chunk is within maxUtf8Length (UTF-8 characters).
    // When overlapPercent > 0, adjacent chunks will overlap by the given
    // percentage of maxUtf8Length.
    inline static std::vector<std::string>
    splitTextToChunks(std::string_view text, const SplitConfig &config);

    asio::awaitable<std::vector<Document>>
    scanDocument(const std::vector<std::string> &pathlist);

    // Add documents and compute their embeddings
    asio::awaitable<bool> addDocuments(std::vector<Document> &&appendDocs);

    // Search by cosine similarity
    asio::awaitable<std::expected<
        std::vector<std::tuple<const Document &, size_t, double>>, std::string>>
    search(std::string_view query, size_t top_k = 3) const;

  protected:
    SplitConfig splitConfig;
    std::shared_ptr<EmbeddingClient> embedder;
    std::vector<Document> docs;
  };

  std::shared_ptr<VectorStore> store;

  RAGSearchTool(std::shared_ptr<VectorStore> in_store,
                std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

  neograph::ChatTool get_definition() const override;

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override;
};

} // namespace tools
} // namespace agentxx
