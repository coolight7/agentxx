// agentxx_rag_search —— RAG 语义检索工具插件 (agentxx_rag_search)
// - 从 libagentxx src/tools/rag_search 拆分独立 (同名同行为)
// - 配置经宿主 agentxx.agent.model 接口表读取 (原 lib AgentConfig 同名字段,
//   yaml 配置不变): model.baseUrl/apiKey/modelName 作 embedding 服务配置,
//   ragDocsPaths 为文档扫描路径; ragDocsPaths 为空时不注册工具 (与原 lib 行为一致)
// - 文档扫描 + embedding 索引构建在 entry 时执行 (宿主把 entry 卸载到线程池,
//   不阻塞 io 线程); 原实现在 CodeAgent::initTools 内 co_await 同语义
// - 业务逻辑在 rag_search_impl.h (纯函数, 测试直测同一实现)
#include "rag_plugin.h"
#include "rag_search_impl.h"
#include <cstring>
#include <string>
#include <vector>

using namespace agentxx_rag_plugin;

namespace {

constexpr auto kNameSearch = "agentxx_rag_search";

constexpr auto kDepictSearch = R"(Search the knowledge base using semantic similarity.
Use this to find relevant documents before answering questions.
Returns the most relevant documents with content, source, and similarity score.)";

/// 参数说明兜底 (正常情况下由宿主 toolPrompt 提供完整文案)
std::string argDesc(const ToolPromptText& p, const char* key, const char* fallback) {
    auto it = p.args.find(key);
    if (it != p.args.end() && !it->second.empty()) {
        return it->second;
    }
    return fallback;
}

} // namespace

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_get_info(void) {
    // C ABI 边界异常守卫: 异常返回 NULL (宿主按"未导出"处理)
    XX_PGUARD_BEGIN
    static const AgentxxPluginInfo info{
        AGENTXX_PLUGIN_API_VERSION,
        AGENTXX_SV("agentxx_rag_search"),
        AGENTXX_SV("1.0.0"),
        AGENTXX_SV("RAG semantic search over configured docs paths (embedding based)"),
    };
    return &info;
    XX_PGUARD_END_RET(nullptr)
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_entry(const AgentxxHost* host, void** plugin_ctx) {
    // C ABI 边界异常守卫: entry 含向量索引构建等大量可抛操作 (文档扫描/
    // embedding HTTP), 异常返回 -1 走宿主加载失败清理路径
    XX_PGUARD_BEGIN
    if (!host || !host->vtable || !plugin_ctx) {
        return -1;
    }
    g_host      = host;
    g_if        = agentxx::plugin::AgentIfaces::query(host);
    *plugin_ctx = nullptr;

    // ---- 读取模型 / RAG 配置 (与原 CodeAgent::initTools 的装配来源一致) ----
    if (!g_if.model || !g_if.model->get_config) {
        XX_LOGW(
            "agentxx_rag_search: host model iface unavailable, `{}' not registered",
            kNameSearch
        );
        return 0;
    }
    char*       json = g_if.model->get_config(g_host);
    neograph::json cfg;
    bool           hasCfg = false;
    if (json) {
        std::string cfgJson = json;
        g_host->vtable->free(json);
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
    // ragDocsPaths 为空: 不注册工具 (原 lib 仅在非空时创建 RAGSearchTool)
    if (ragDocsPaths.empty()) {
        XX_LOGI(
            "agentxx_rag_search: no rag_docs_paths configured, `{}' not registered",
            kNameSearch
        );
        return 0;
    }
    if (baseUrl.empty() || modelName.empty()) {
        XX_LOGW(
            "agentxx_rag_search: model baseUrl/modelName unavailable, `{}' not registered",
            kNameSearch
        );
        return 0;
    }

    // ---- 构建向量索引 (文档扫描 + embedding 生成; 耗时操作在线程池内执行) ----
    static agentxx::rag_plugin::VectorStore g_store
        = agentxx::rag_plugin::VectorStore(agentxx::rag_plugin::makeHttpEmbedder(baseUrl, modelName));

    // 逐步上报启动进度: RAG 文档扫描 + embedding 生成耗时较长 (原实现同语义;
    // 插件 entry 阶段宿主进度事件不可用, 经日志提示)
    XX_LOGI("RAG: loading documents and generating vector index ...");
    auto docs     = g_store.scanDocument(ragDocsPaths);
    auto docxSize = docs.size();
    bool isAddSuccess = g_store.addDocuments(std::move(docs));
    XX_LOGD(
        R"_(
┏━━━━━━ RAG Embedding ━━━━━━┓
{}
┗━━━━━━ RAG Embedding ━━━━━━┛
)_",
        isAddSuccess ? fmt::format("┣━ ✅ success: append {} docs", docxSize) : "┣━ ❌ failed"
    );

    // ---- 注册工具 (embedding 失败仍注册, 与原 lib 行为一致: 查询时报错) ----
    {
        ToolPromptText p      = readToolPrompt(kNameSearch);
        std::string    depict = p.depict;
        if (depict.empty()) {
            depict = kDepictSearch;
        }
        static std::vector<std::string> g_storage;
        g_storage.push_back(std::move(depict));
        // schema 与 lib AgentPrompt 条目一致 (query/top_k)
        std::string schema = neograph::json{
            {"type", "object"},
            {"required", neograph::json::array({"query"})},
            {"properties",
             {
                 {"query",
                  {
                      {"type", "string"},
                      {"description",
                       argDesc(p, "query", "Search query to find relevant documents.")},
                  }},
                 {"top_k",
                  {
                      {"type", "integer"},
                      {"description", argDesc(p, "top_k", "Number of results to return. Default: 3.")},
                  }},
             }},
        }
                              .dump();
        g_storage.push_back(std::move(schema));

        AgentxxSyncToolSpec spec{};
        spec.name = agentxx_plugin_sv(kNameSearch, std::strlen(kNameSearch));
        spec.description
            = agentxx_plugin_sv(g_storage[0].data(), g_storage[0].size());
        spec.parameters_json = agentxx_plugin_sv(g_storage[1].data(), g_storage[1].size());
        spec.user_data       = nullptr;
        spec.flags           = AGENTXX_TOOL_FLAG_AUTO_SUMMARY;
        // 阻塞委托型: embedding + 向量检索为慢同步操作 (offload 池线程执行)
        spec.execute         = [](void*                   user_data,
                          AgentxxPluginStringView args_json,
                          AgentxxPluginStringView thread_id,
                          AgentxxPluginStringView tool_call_id,
                          volatile int*           cancel_flag,
                          char**                  error_out) -> char* {
            (void)user_data;
            (void)thread_id;
            (void)tool_call_id;
            (void)cancel_flag;
            try {
                std::string argsStr(args_json.data ? args_json.data : "", args_json.size);
                auto arguments
                    = argsStr.empty() ? neograph::json::object() : neograph::json::parse(argsStr);

                auto query = arguments.value("query", std::string{});
                if (query.empty()) {
                    return pluginStrdup(R"({"error":"Arg `query` is empty"})");
                }
                // 限制 top_k 范围: 过大的 top_k 会把海量结果灌入上下文,
                // 且可能超出文档数量
                int top_k = std::clamp(arguments.value("top_k", 3), 1, 50);

                auto results = g_store.search(query, static_cast<size_t>(top_k));
                if (false == results.has_value()) {
                    return pluginStrdup(fmt::format("Search error: {}", results.error()).c_str());
                }
                if (results->empty()) {
                    return pluginStrdup(
                        fmt::format("No relevant documents found for: {}", query).c_str()
                    );
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
                auto text = output.dump(2);
                return pluginStrdup(text.c_str());
            } catch (const std::exception& ex) {
                if (error_out) {
                    *error_out = pluginStrdup(ex.what());
                }
                return nullptr;
            } catch (...) {
                if (error_out) {
                    *error_out = pluginStrdup("unknown exception");
                }
                return nullptr;
            }
        };
        if (agentxx_register_sync_tool(g_host, &spec) != 0) {
            XX_LOGW("agentxx_rag_search: register tool {} failed", kNameSearch);
        }
    }

    return 0;
    XX_PGUARD_END_RET(-1)
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_unload(void* plugin_ctx) {
    // C ABI 边界异常守卫: 卸载回调异常不得外泄
    XX_PGUARD_BEGIN
    (void)plugin_ctx;
    // 向量索引随进程生存 (静态存储), unload 仅解除宿主句柄引用
    g_host = nullptr;
    g_if   = {};
    XX_PGUARD_END_VOID()
}
