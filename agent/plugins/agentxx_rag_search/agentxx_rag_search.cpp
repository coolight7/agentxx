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
    // C ABI 边界异常守卫: 异常返回 NULL (宿主按"未导出"处理);
    // 本边界为纯静态元数据, 无实例上下文可捕获 → 空操作日志闭包
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
    });
}

// RAG 检索执行 (C ABI 静态函数): 向量索引取自 user_data 恢复的本实例
// PluginCtx (多实例契约 —— 各实例持有各自索引)
static char* ragSearchExecute(
    void*                   user_data,
    AgentxxPluginStringView args_json,
    AgentxxPluginStringView thread_id,
    AgentxxPluginStringView tool_call_id,
    volatile int*           cancel_flag,
    char**                  error_out
) {
    auto*              ctx  = static_cast<PluginCtx*>(user_data);
    const AgentxxHost* host = ctx ? ctx->host : nullptr;
    (void)thread_id;
    (void)tool_call_id;
    (void)cancel_flag;
    try {
        std::string argsStr(args_json.data ? args_json.data : "", args_json.size);
        auto arguments
            = argsStr.empty() ? neograph::json::object() : neograph::json::parse(argsStr);

        auto query = arguments.value("query", std::string{});
        if (query.empty()) {
            return pluginStrdup(host, R"({"error":"Arg `query` is empty"})");
        }
        // 限制 top_k 范围: 过大的 top_k 会把海量结果灌入上下文,
        // 且可能超出文档数量
        int top_k = std::clamp(arguments.value("top_k", 3), 1, 50);

        auto* store   = static_cast<agentxx_rag_plugin::VectorStore*>(ctx ? ctx->store_opaque
                                                                         : nullptr);
        if (!store) {
            return pluginStrdup(host, R"({"error":"rag index not initialized"})");
        }
        auto results = store->search(query, static_cast<size_t>(top_k));
        if (false == results.has_value()) {
            return pluginStrdup(host, fmt::format("Search error: {}", results.error()).c_str());
        }
        if (results->empty()) {
            return pluginStrdup(
                host, fmt::format("No relevant documents found for: {}", query).c_str()
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
        return pluginStrdup(host, text.c_str());
    } catch (const std::exception& ex) {
        if (error_out) {
            *error_out = pluginStrdup(host, ex.what());
        }
        return nullptr;
    } catch (...) {
        if (error_out) {
            *error_out = pluginStrdup(host, "unknown exception");
        }
        return nullptr;
    }
}

extern "C" AGENTXX_PLUGIN_EXPORT int agentxx_plugin_create(const AgentxxHost* host, void** plugin_ctx) {
    // C ABI 边界异常守卫: create 含向量索引构建等大量可抛操作 (文档扫描/
    // embedding HTTP), 异常返回 -1 走宿主加载失败清理路径;
    // 守卫日志闭包捕获局部裸指针 (ctx 装配前置空 → 异常路径静默丢弃)
    PluginCtx* raw = nullptr;
    return agentxx::plugin_guard::guardCall(
        [&raw](const char* msg) noexcept { pluginLog(raw, 4, msg ? msg : ""); },
        -1,
        [&]() -> int {
        if (!host || !host->vtable || !plugin_ctx) {
            return -1;
        }
        auto ctx   = std::make_unique<PluginCtx>();
        ctx->host  = host;
        ctx->iface = agentxx::plugin::AgentIfaces::query(host);
        raw        = ctx.get();

        // ---- 读取模型 / RAG 配置 (与原 CodeAgent::initTools 的装配来源一致) ----
        if (!ctx->iface.model || !ctx->iface.model->get_config) {
            pluginLog(ctx.get(), 3,
                      fmt::format("agentxx_rag_search: host model iface unavailable, `{}' not registered",
                                  kNameSearch));
            return 0;
        }
        char*       json = ctx->iface.model->get_config(ctx->host);
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
        // ragDocsPaths 为空: 不注册工具 (原 lib 仅在非空时创建 RAGSearchTool)
        if (ragDocsPaths.empty()) {
            pluginLog(ctx.get(), 2,
                      fmt::format("agentxx_rag_search: no rag_docs_paths configured, `{}' not registered",
                                  kNameSearch));
            return 0;
        }
        if (baseUrl.empty() || modelName.empty()) {
            pluginLog(ctx.get(), 3,
                      fmt::format("agentxx_rag_search: model baseUrl/modelName unavailable, `{}' not registered",
                                  kNameSearch));
            return 0;
        }

        // ---- 构建向量索引 (文档扫描 + embedding 生成; 耗时操作在线程池内执行;
        //  每实例独立索引 —— 原静态存储在多实例下会串索引) ----
        auto* g_store = new agentxx_rag_plugin::VectorStore(
            agentxx_rag_plugin::makeHttpEmbedder(baseUrl, modelName));
        ctx->store_opaque = g_store;

        // 逐步上报启动进度: RAG 文档扫描 + embedding 生成耗时较长 (原实现同语义;
        // 插件 entry 阶段宿主进度事件不可用, 经日志提示)
        pluginLog(ctx.get(), 2, "RAG: loading documents and generating vector index ...");
        auto docs     = g_store->scanDocument(ragDocsPaths);
        auto docxSize = docs.size();
        bool isAddSuccess = g_store->addDocuments(std::move(docs));
        pluginLog(ctx.get(),
                  1,
                  fmt::format(
                      R"__(
┏━━━━━━ RAG Embedding ━━━━━━┓
{}
┗━━━━━━ RAG Embedding ━━━━━━┛
)__",
                      isAddSuccess ? fmt::format("┣━ ✅ success: append {} docs", docxSize)
                                   : "┣━ ❌ failed"));

        // ---- 注册工具 (embedding 失败仍注册, 与原 lib 行为一致: 查询时报错) ----
        {
            ToolPromptText p      = readToolPrompt(ctx->host, ctx->iface, kNameSearch);
            std::string    depict = p.depict;
            if (depict.empty()) {
                depict = kDepictSearch;
            }
            auto& storage = ctx->storage;
            storage.push_back(std::move(depict));
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
            storage.push_back(std::move(schema));

            // 垫片适配器: 实例内嵌存储 (ctx 持有, 随实例销毁释放; 多实例契约)
            auto shim = std::make_unique<AgentxxSyncToolShim>();

            AgentxxSyncToolSpec spec{};
            spec.name = agentxx_plugin_sv(kNameSearch, std::strlen(kNameSearch));
            spec.description
                = agentxx_plugin_sv(storage[0].data(), storage[0].size());
            spec.parameters_json = agentxx_plugin_sv(storage[1].data(), storage[1].size());
            spec.user_data       = ctx.get();
            spec.flags           = AGENTXX_TOOL_FLAG_AUTO_SUMMARY;
            // 阻塞委托型: embedding + 向量检索为慢同步操作 (offload 池线程执行);
            // 执行体为静态函数, 索引经 user_data 从本实例 ctx 读取
            spec.execute         = &ragSearchExecute;
            if (agentxx_register_sync_tool(host, &spec, shim.get()) != 0) {
                pluginLog(ctx.get(), 3,
                          fmt::format("agentxx_rag_search: register tool {} failed", kNameSearch));
                return 0;
            }
            ctx->sync_tool_shims.push_back(std::move(shim));
        }

        *plugin_ctx = ctx.release(); ///< 所有权移交宿主 (destroy 时取回归还)
        return 0;
    });
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_destroy(void* plugin_ctx) {
    // C ABI 边界异常守卫: 销毁回调异常不得外泄
    auto* ctx = static_cast<PluginCtx*>(plugin_ctx);
    agentxx::plugin_guard::guardCallVoid(
        [ctx](const char* msg) noexcept { pluginLog(ctx, 4, msg ? msg : ""); },
        [&] {
            if (ctx && ctx->store_opaque) {
                delete static_cast<agentxx_rag_plugin::VectorStore*>(ctx->store_opaque);
                ctx->store_opaque = nullptr;
            }
            delete ctx; // 垫片适配器为 ctx 内嵌存储, 随 ctx 一并释放
        });
}
