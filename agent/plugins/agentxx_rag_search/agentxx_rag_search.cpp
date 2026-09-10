/// agentxx_rag_search —— RAG 语义检索工具插件 (agentxx_rag_search)
#include "rag_plugin.h"
#include "rag_search_impl.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace agentxx_rag_plugin;
using namespace agentxx::plugin;

namespace {

constexpr std::string_view kNameSearch = "agentxx_rag_search";

constexpr std::string_view kDepictSearch = R"(Search the knowledge base using semantic similarity.
Use this to find relevant documents before answering questions.
Returns the most relevant documents with content, source, and similarity score.)";

} // namespace

struct RagPluginCtx : public PluginBase {
    std::unique_ptr<VectorStore> store;
};

/// 注册事务 (start 的实际内容): 读取配置、构建索引并注册检索工具。
static int32_t ragSetup(RagPluginCtx& ctx) {
        if (!ctx.iface.model || !ctx.iface.model->get_config) {
            ctx.log.warn(fmt::format(
                "agentxx_rag_search: host model iface unavailable, `{}` not registered",
                kNameSearch
            ));
            return 0;
        }
        AgentxxPluginString json{nullptr, 0};
        ctx.iface.model->get_config(ctx.host, &json);
        agentxx::util::Json cfg;
        bool                hasCfg = false;
        if (json.data) {
            std::string cfgJson(json.data, static_cast<size_t>(json.size));
            PluginString::free(ctx.host, &json);
            try {
                cfg    = agentxx::util::Json::parse(cfgJson);
                hasCfg = true;
            } catch (...) {
                hasCfg = false;
            }
        }

        std::vector<std::string> ragDocsPaths;
        std::string              baseUrl;
        std::string              modelName;
        if (hasCfg) {
            baseUrl   = cfg.value("baseUrl", std::string{});
            modelName = cfg.value("modelName", std::string{});
            if (cfg.contains("ragDocsPaths") && cfg["ragDocsPaths"].is_array()) {
                for (const auto& item : cfg["ragDocsPaths"]) {
                    if (item.is_string()) {
                        ragDocsPaths.push_back(item.get<std::string>());
                    }
                }
            }
        }
        if (ragDocsPaths.empty()) {
            ctx.log.info(
                "agentxx_rag_search: `ragDocsPaths` not configured or empty, search tool skipped"
            );
            return 0;
        }

        ctx.store = std::make_unique<VectorStore>(makeHttpEmbedder(baseUrl, modelName));

        ctx.log.info("RAG: loading documents and generating vector index ...");
        auto docs         = ctx.store->scanDocument(ragDocsPaths);
        auto docxSize     = docs.size();
        bool isAddSuccess = ctx.store->addDocuments(std::move(docs));
        ctx.log.log(
            isAddSuccess ? 2 : 3,
            fmt::format(
                "RAG: loading {} documents to vector index {}",
                docxSize,
                isAddSuccess ? "done" : "failed"
            )
        );

        if (!ctx.iface.tools || !ctx.iface.tools->register_tool) {
            return 0;
        }

        auto schema
            = ctx.schema(kNameSearch)
                  .string(
                      "query",
                      "Search query text to find relevant documents.",
                      /*required=*/true
                  )
                  .integer(
                      "top_k",
                      "Number of top relevant results to return (default 3, min 1, max 50).",
                      false,
                      3
                  )
                  .build();

        blocking_tool(
            ctx,
            kNameSearch,
            kDepictSearch,
            schema,
            [](RagPluginCtx& c,
               std::string_view args_json,
               const AgentxxPluginCancelToken* cancel_token) -> std::string {
                if (agentxx_plugin_cancel_is_requested(cancel_token)) {
                    throw agentxx::plugin::CancelledException("rag_search cancelled");
                }
                ArgReader args(args_json);
                auto      query = args.require<std::string>("query");
                if (!args.ok()) {
                    return args.errorMessage();
                }
                int top_k = std::clamp(args.value("top_k", 3), 1, 50);

                if (!c.store) {
                    return R"({"error":"rag index not initialized"})";
                }
                auto results = c.store->search(query, static_cast<size_t>(top_k));
                if (agentxx_plugin_cancel_is_requested(cancel_token)) {
                    throw agentxx::plugin::CancelledException("rag_search cancelled");
                }
                if (!results.has_value()) {
                    return fmt::format("Search error: {}", results.error());
                }
                if (results->empty()) {
                    return fmt::format("No relevant documents found for: {}", query);
                }

                auto output = agentxx::util::Json::array();
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
                return output.dump(2);
            }
        );

    return 0;
}

static void* ragStart(
    RagPluginCtx& ctx, const AgentxxPluginOperatorNotify* notify, AgentxxPluginString* error
) {
    if (!notify) {
        if (error) {
            agentxx::plugin::PluginString::set(
                ctx.host, error, "agentxx_rag_search start: notify required"
            );
        }
        return nullptr;
    }
    if (ragSetup(ctx) != 0) {
        if (error) {
            agentxx::plugin::PluginString::set(
                ctx.host, error, "agentxx_rag_search start: registration failed"
            );
        }
        return nullptr;
    }
    notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
    return nullptr;
}

static void* ragStop(
    RagPluginCtx&, const AgentxxPluginOperatorNotify* notify, AgentxxPluginString*
) {
    notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
    return nullptr;
}

AGENTXX_PLUGIN_AGENT_LIFECYCLE_EXPORT(RagPluginCtx, ragStart, ragStop)

AGENTXX_PLUGIN_AGENT_EXPORT(
    RagPluginCtx,
    "agentxx_rag_search",
    "1.0.0",
    "RAG semantic search over configured docs paths (embedding based)",
    [](RagPluginCtx&) -> int32_t {
        // create 只构造上下文; 索引构建与工具注册在 start 事务中执行。
        return 0;
    }
);
