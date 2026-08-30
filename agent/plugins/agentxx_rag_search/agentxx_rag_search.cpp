// agentxx_rag_search —— RAG 语义检索工具插件 (agentxx_rag_search)
#include "rag_plugin.h"
#include "rag_search_impl.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace agentxx_rag_plugin;

namespace {

constexpr auto kNameSearch = "agentxx_rag_search";

constexpr auto kDepictSearch = R"(Search the knowledge base using semantic similarity.
Use this to find relevant documents before answering questions.
Returns the most relevant documents with content, source, and similarity score.)";

std::string argDesc(const agentxx::kit::ToolPromptText& p, const char* key, const char* fallback) {
    auto it = p.args.find(key);
    if (it != p.args.end() && !it->second.empty()) {
        return it->second;
    }
    return fallback;
}

} // namespace

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_agent_get_info(void) {
    return agentxx::plugin_guard::guardCall(
        [](const char*) noexcept {},
        nullptr,
        [&]() -> const AgentxxPluginInfo* {
            static const AgentxxPluginInfo info{
                AGENTXX_PLUGIN_API_VERSION,
                AGENTXX_SV("agentxx_rag_search"),
                AGENTXX_SV("1.0.0"),
                AGENTXX_SV("RAG semantic search over configured docs paths (embedding based)"),
            };
            return &info;
        }
    );
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_agent_create(const AgentxxHost* host, void** plugin_ctx) {
    PluginCtx* raw = nullptr;
    return agentxx::plugin_guard::guardCall(
        [&raw](const char* msg) noexcept {
            pluginLog(raw, 4, msg ? msg : "");
        },
        -1,
        [&]() -> int {
            if (!host || !host->vtable || !plugin_ctx) {
                return -1;
            }
            auto ctx = std::make_unique<PluginCtx>();
            ctx->init(host);
            raw = ctx.get();

            if (!ctx->iface.model || !ctx->iface.model->get_config) {
                pluginLog(
                    ctx.get(),
                    3,
                    fmt::format(
                        "agentxx_rag_search: host model iface unavailable, `{}' not registered",
                        kNameSearch
                    )
                );
                return 0;
            }
            char*          json = ctx->iface.model->get_config(ctx->host);
            neograph::json cfg;
            bool           hasCfg = false;
            if (json) {
                std::string cfgJson = json;
                ctx->host->vtable->free(json);
                try {
                    cfg    = neograph::json::parse(cfgJson);
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
                pluginLog(
                    ctx.get(),
                    2,
                    fmt::format(
                        "agentxx_rag_search: no rag_docs_paths configured, `{}' not registered",
                        kNameSearch
                    )
                );
                return 0;
            }
            if (baseUrl.empty() || modelName.empty()) {
                pluginLog(
                    ctx.get(),
                    3,
                    fmt::format(
                        "agentxx_rag_search: model baseUrl/modelName unavailable, `{}' not registered",
                        kNameSearch
                    )
                );
                return 0;
            }

            auto* g_store = new agentxx_rag_plugin::VectorStore(
                agentxx_rag_plugin::makeHttpEmbedder(baseUrl, modelName)
            );
            ctx->store_opaque = g_store;

            pluginLog(ctx.get(), 2, "RAG: loading documents and generating vector index ...");
            auto docs         = g_store->scanDocument(ragDocsPaths);
            auto docxSize     = docs.size();
            bool isAddSuccess = g_store->addDocuments(std::move(docs));
            pluginLog(
                ctx.get(),
                isAddSuccess ? 2 : 3,
                fmt::format(
                    "RAG: loading {} documents to vector index {}",
                    docxSize,
                    isAddSuccess ? "done" : "failed"
                )
            );

            if (!ctx->iface.tools || !ctx->iface.tools->register_tool) {
                return 0;
            }

            auto        p      = ctx->toolPrompt(kNameSearch);
            std::string depict = p.depict.empty() ? kDepictSearch : p.depict;
            std::string schema = neograph::json{
                {"type",       "object"                        },
                {"properties",
                 {{"query",
                   {{"type", "string"},
                    {"description",
                     argDesc(p, "query", "Search query text to find relevant documents.")}}},
                  {"top_k",
                   {{"type", "integer"},
                    {"default", 3},
                    {"description",
                     argDesc(
                         p,
                         "top_k",
                         "Number of top relevant results to return (default 3, min 1, max 50)."
                     )}}}}                                     },
                {"required",   neograph::json::array({"query"})}
            }.dump();

            agentxx::kit::blocking_tool(
                *ctx,
                kNameSearch,
                depict,
                schema,
                [](PluginCtx& c, std::string_view args_json) -> std::string {
                    std::string argsStr(args_json.data() ? args_json.data() : "", args_json.size());
                    auto        arguments = argsStr.empty() ? neograph::json::object()
                                                            : neograph::json::parse(argsStr);

                    auto query = arguments.value("query", std::string{});
                    if (query.empty()) {
                        return R"({"error":"Arg `query` is empty"})";
                    }
                    int top_k = std::clamp(arguments.value("top_k", 3), 1, 50);

                    auto* store = static_cast<agentxx_rag_plugin::VectorStore*>(c.store_opaque);
                    if (!store) {
                        return R"({"error":"rag index not initialized"})";
                    }
                    auto results = store->search(query, static_cast<size_t>(top_k));
                    if (!results.has_value()) {
                        return fmt::format("Search error: {}", results.error());
                    }
                    if (results->empty()) {
                        return fmt::format("No relevant documents found for: {}", query);
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
                    return output.dump(2);
                }
            );

            *plugin_ctx = ctx.release();
            return 0;
        }
    );
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<PluginCtx*>(plugin_ctx);
    agentxx::plugin_guard::guardCallVoid(
        [ctx](const char* msg) noexcept {
            pluginLog(ctx, 4, msg ? msg : "");
        },
        [&] {
            if (!ctx) {
                return;
            }
            if (ctx->store_opaque) {
                delete static_cast<agentxx_rag_plugin::VectorStore*>(ctx->store_opaque);
                ctx->store_opaque = nullptr;
            }
            delete ctx;
        }
    );
}
