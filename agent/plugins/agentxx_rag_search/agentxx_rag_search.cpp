/// agentxx_rag_search —— RAG 语义检索工具插件 (agentxx_rag_search)
#include "rag_plugin.h"
#include "rag_search_impl.h"
#include "utilxx_base/json.h"
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

constexpr int32_t kAutoSummary = AGENTXX_PLUGIN_TOOL_FLAG_AUTO_SUMMARY;

} // namespace

struct RagPluginCtx : public PluginBase {};

/// per-agent 状态 (每个挂载一份): 向量索引与挂载引用
struct RagAgentState {
    AgentRef                    ref;
    std::unique_ptr<VectorStore> store;
};

/// 注册事务 (mount 的实际内容): 读取配置、构建索引并注册检索工具。
static int32_t ragSetup(RagPluginCtx& ctx, RagAgentState& st) {
    if (!ctx.iface.model || !ctx.iface.model->get_config) {
        ctx.log.warn(fmt::format(
            "agentxx_rag_search: host model iface unavailable, `{}` not registered",
            kNameSearch
        ));
        return 0;
    }
    auto           modelRef = st.ref.raw();
    PluginxxString json{nullptr, 0};
    ctx.iface.model->get_config(ctx.host, &modelRef, &json);
    utilxx_base::Json cfg;
    bool              hasCfg = false;
    if (json.data) {
        std::string cfgJson(json.data, static_cast<size_t>(json.size));
        PluginString::free(ctx.host, &json);
        try {
            cfg    = utilxx_base::Json::parse(cfgJson);
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

    st.store = std::make_unique<VectorStore>(makeHttpEmbedder(baseUrl, modelName));

    ctx.log.info("RAG: loading documents and generating vector index ...");
    auto docs         = st.store->scanDocument(ragDocsPaths);
    auto docxSize     = docs.size();
    bool isAddSuccess = st.store->addDocuments(std::move(docs));
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

    auto schema = ctx.schema(kNameSearch)
                      .string(
                          "query",
                          "Search query to find relevant documents.",
                          /*required=*/true
                      )
                      .integer(
                          "limit",
                          "Number of results to return. Range: [1, 50]. Default `3`.",
                          false,
                          3
                      )
                      .build();

    blocking_tool(
        ctx,
        st.ref,
        kNameSearch,
        kDepictSearch,
        schema,
        [&st](RagPluginCtx& c, std::string_view args_json, const PluginxxCancelToken* cancel_token
        ) -> std::string {
            (void)c;
            if (pluginxx_cancel_is_requested(cancel_token)) {
                throw agentxx::plugin::CancelledException("rag_search cancelled");
            }
            ArgReader args(args_json);
            auto      query = args.require<std::string>("query");
            if (!args.ok()) {
                return args.errorMessage();
            }
            int limit = std::clamp(args.value("limit", 3), 1, 50);

            if (!st.store) {
                throw std::runtime_error{"RAG index is not initialized"};
            }
            auto results = st.store->search(query, static_cast<size_t>(limit));
            if (pluginxx_cancel_is_requested(cancel_token)) {
                throw agentxx::plugin::CancelledException("rag_search cancelled");
            }
            if (!results.has_value()) {
                throw std::runtime_error{fmt::format("RAG search failed: {}", results.error())};
            }
            if (results->empty()) {
                return fmt::format("No relevant documents found for: {}", query);
            }

            auto output = utilxx_base::Json::array();
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
        },
        0,
        kAutoSummary
    );

    return 0;
}

static void*
    ragStart(RagPluginCtx& ctx, const PluginxxOperatorNotify* notify, PluginxxString*) {
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    (void)ctx;
    return nullptr;
}

static void* ragMount(
    RagPluginCtx&                      ctx,
    const AgentxxPluginAgentMountSpec* spec,
    const PluginxxOperatorNotify*      notify,
    PluginxxString*                    error
) {
    if (!notify) {
        if (error) {
            PluginString::set(ctx.host, error, "agentxx_rag_search mount: notify required");
        }
        return nullptr;
    }
    auto* st = new RagAgentState{AgentRef(*spec), nullptr};
    if (ragSetup(ctx, *st) != 0) {
        delete st;
        if (error) {
            PluginString::set(ctx.host, error, "agentxx_rag_search mount: setup failed");
        }
        return nullptr;
    }
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return st;
}

static void*
    ragUnmount(RagPluginCtx&, void* ud, const PluginxxOperatorNotify* notify, PluginxxString*) {
    delete static_cast<RagAgentState*>(ud);
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

static void* ragStop(RagPluginCtx&, const PluginxxOperatorNotify* notify, PluginxxString*) {
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

AGENTXX_PLUGIN_AGENT_EXPORT(
    RagPluginCtx,
    "agentxx_rag_search",
    "1.0.0",
    "RAG semantic search over configured docs paths (embedding based)",
    ragStart,
    ragStop,
    ragMount,
    ragUnmount
);
