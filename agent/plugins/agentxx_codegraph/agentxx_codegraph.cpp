// agentxx_codegraph —— CodeGraph 代码分析插件
// - 从 lib 迁移: CodeGraphManager + 8 个 codegraph 工具
//   (search/context/callers/callees/impact/status/index/path)
// - 插件不链接 libagentxx: 装配期经 vtable get_config 读取宿主通用信息
//   (dataDir/projectRoot), 经 get_plugin_args 读取本插件参数 (宿主不解析
//   字段语义); 索引进度经 publish 事件通知宿主 (topic 约定 `{插件名}.{事件名}`,
//   宿主原样转发 WirePluginData, 客户端据此展示)
// - 依赖: codegraph_core + tree_sitter 系 + sqlite3 + simdjson + glob
#include "codegraph_manager.h"
#include "codegraph_plugin.h"
#include "fmt/format.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace agentxx_codegraph_plugin {

/// CodeGraphManager (定义于 agentxx::expand; 插件内简称)
using CodeGraphManager = agentxx::expand::CodeGraphManager;

/// 插件配置 (宿主 get_config 通用信息 + get_plugin_args 业务参数)
/// - 宿主只整体传递 args json, 不解析其字段语义; 字段名由本插件定义:
///   paths / ignore_paths / load_cwd / use_gitignore (与原 yaml codegraph 段一致)
struct HostConfig {
    std::string              dataDir;
    std::string              projectRoot;
    std::vector<std::string> loadPaths;
    std::vector<std::string> ignorePaths;
    bool                     useGitignore = true;
    bool                     loadCwd      = true;
};

static HostConfig readHostConfig() {
    HostConfig cfg;
    if (!g_host || !g_host->vtable) {
        return cfg;
    }
    // ---- 通用宿主信息 (get_config) ----
    if (g_host->vtable->get_config) {
        char* json = g_host->vtable->get_config(g_host);
        if (json) {
            std::string s{json};
            g_host->vtable->free(json);
            SimpleJson j(s);
            if (j.ok()) {
                jsonGetString(j.doc().at_pointer("/dataDir"), cfg.dataDir);
                jsonGetString(j.doc().at_pointer("/projectRoot"), cfg.projectRoot);
            }
        }
    }
    // ---- 本插件业务参数 (get_plugin_args; 宿主原样传递) ----
    if (g_host->vtable->get_plugin_args) {
        char* json = g_host->vtable->get_plugin_args(g_host);
        if (json) {
            std::string s{json};
            g_host->vtable->free(json);
            SimpleJson j(s);
            if (j.ok()) {
                jsonGetStringArray(j.doc().at_pointer("/paths"), cfg.loadPaths);
                jsonGetStringArray(j.doc().at_pointer("/ignore_paths"), cfg.ignorePaths);
                jsonGetBool(j.doc().at_pointer("/load_cwd"), cfg.loadCwd);
                jsonGetBool(j.doc().at_pointer("/use_gitignore"), cfg.useGitignore);
            }
        }
    }
    return cfg;
}

/// 工具提示词 (经 vtable get_tool_prompt 读取; 未配置回退内置默认)
struct ToolPrompt {
    std::string depict;
    std::string query, limit, symbol, maxDepth, path, incremental, from, to;
};

static ToolPrompt readToolPrompt(const std::string& toolName) {
    ToolPrompt out;
    if (!g_host || !g_host->vtable || !g_host->vtable->get_tool_prompt) {
        return out;
    }
    char* json = g_host->vtable->get_tool_prompt(
        g_host, agentxx_plugin_sv(toolName.data(), toolName.size())
    );
    if (!json) {
        return out;
    }
    std::string s{json};
    g_host->vtable->free(json);
    SimpleJson j(s);
    if (!j.ok()) {
        return out;
    }
    jsonGetString(j.doc().at_pointer("/depict"), out.depict);
    jsonGetString(j.doc().at_pointer("/args/query"), out.query);
    jsonGetString(j.doc().at_pointer("/args/limit"), out.limit);
    jsonGetString(j.doc().at_pointer("/args/symbol"), out.symbol);
    jsonGetString(j.doc().at_pointer("/args/max_depth"), out.maxDepth);
    jsonGetString(j.doc().at_pointer("/args/path"), out.path);
    jsonGetString(j.doc().at_pointer("/args/incremental"), out.incremental);
    jsonGetString(j.doc().at_pointer("/args/from"), out.from);
    jsonGetString(j.doc().at_pointer("/args/to"), out.to);
    return out;
}

// =====================================================================
// 文本格式化辅助 (context/impact 结果 → 多行文本; 与原 lib 输出一致)
// =====================================================================

static std::string appendJsonNodeDetails(std::string& out, const codegraph::Json& node) {
    if (node.contains("file")) {
        auto file = node["file"].get<std::string>();
        if (node.contains("line")) {
            out += fmt::format("\n    file: {}:{}", file, node["line"].get<int64_t>());
        } else {
            out += fmt::format("\n    file: {}", file);
        }
    }
    if (node.contains("signature")) {
        auto sig = node["signature"].get<std::string>();
        if (!sig.empty()) {
            out += fmt::format("\n    signature: {}", sig);
        }
    }
    return out;
}

static std::string jsonNodesToText(std::string_view title, const codegraph::Json& arr) {
    std::string out = fmt::format("{} ({}):", title, arr.size());
    if (arr.size() == 0) {
        out += "\n  (none)";
        return out;
    }
    for (size_t i = 0; i < arr.size(); ++i) {
        const auto& n = arr[i];
        out += fmt::format("\n[{}] {} {}", i + 1, n.value("kind", ""), n.value("name", ""));
        appendJsonNodeDetails(out, n);
    }
    return out;
}

static std::string jsonEdgesToText(const codegraph::Json& arr) {
    std::string out = fmt::format("edges ({}):", arr.size());
    if (arr.size() == 0) {
        out += "\n  (none)";
        return out;
    }
    for (size_t i = 0; i < arr.size(); ++i) {
        const auto& e = arr[i];
        out += fmt::format(
            "\n[{}] {} -> {} ({})",
            i + 1,
            e.value("src", int64_t{0}),
            e.value("dst", int64_t{0}),
            e.value("kind", "")
        );
    }
    return out;
}

static std::string contextToText(const codegraph::Json& ctx, CodeGraphManager* m) {
    std::string out;
    if (ctx.contains("symbol")) {
        const auto& s = ctx["symbol"];
        out += fmt::format("symbol: {} {}", s.value("kind", ""), s.value("name", ""));
        appendJsonNodeDetails(out, s);
    }
    if (ctx.contains("callers")) {
        out += "\n" + jsonNodesToText("callers", ctx["callers"]);
    }
    if (ctx.contains("callees")) {
        out += "\n" + jsonNodesToText("callees", ctx["callees"]);
    }
    if (ctx.contains("methods")) {
        out += "\n" + jsonNodesToText("methods", ctx["methods"]);
    }
    if (ctx.contains("edges")) {
        out += "\n" + jsonEdgesToText(ctx["edges"]);
    }
    if (m->isIndexing()) {
        out += "\nwarning: CodeGraph is still indexing, results may be incomplete";
    }
    return out;
}

static std::string impactToText(std::string_view title, const codegraph::Json& impact, CodeGraphManager* m) {
    std::string out;
    if (impact.contains("nodes")) {
        out = jsonNodesToText(title, impact["nodes"]);
    } else if (impact.contains("error")) {
        out = "error: " + impact["error"].get<std::string>();
    } else {
        out = "error: unknown result";
    }
    if (m->isIndexing()) {
        out += "\nwarning: CodeGraph is still indexing, results may be incomplete";
    }
    return out;
}

// =====================================================================
// 工具注册辅助: 参数 schema 经 codegraph::Json 构建 (自动转义)
// =====================================================================

struct SchemaBuilder {
    codegraph::Json props = codegraph::Json::object();

    void str(const std::string& name, const std::string& desc) {
        props[name] = codegraph::Json({{"type", "string"}, {"description", desc}});
    }

    void num(const std::string& name, const std::string& desc) {
        props[name] = codegraph::Json({{"type", "number"}, {"description", desc}});
    }

    void boolean(const std::string& name, const std::string& desc) {
        props[name] = codegraph::Json({{"type", "boolean"}, {"description", desc}});
    }

    std::string dump(const std::vector<std::string>& required) {
        codegraph::Json schema = codegraph::Json::object();
        schema["type"]         = "object";
        schema["properties"]   = props;
        schema["required"]     = codegraph::Json::array();
        for (const auto& r : required) {
            schema["required"].push_back(r);
        }
        return schema.dump();
    }
};

/// 注册工具 (描述经 toolPrompt 覆盖; 参数说明经 args 覆盖)
/// - fn: 执行回调 (CodeGraphManager*, args json → 结果字符串)
/// - 静态存储: spec.execute 为静态 lambda (无捕获), mgr/fn 打包为
///   ToolEntry 存于静态区 (unique_ptr 保证地址稳定), 经 user_data 传递;
///   插件生命周期内有效, unload 后宿主不再调用
struct ToolEntry {
    CodeGraphManager* mgr = nullptr;
    std::function<std::string(CodeGraphManager*, SimpleJson&)> fn;
};

static void registerTool(
    CodeGraphManager*                                                                   mgr,
    const char*                                                                         name,
    const char*                                                                         defaultDepict,
    const std::function<std::string(const ToolPrompt&)>&                                schemaBuilder,
    std::function<std::string(CodeGraphManager*, SimpleJson&)>                          fn,
    int                                                                                 flags
) {
    auto prompt        = readToolPrompt(name);
    std::string depict = prompt.depict.empty() ? std::string{defaultDepict} : prompt.depict;
    std::string schema = schemaBuilder(prompt);

    // 插件侧持有 schema/depict 字符串的稳定存储 (注册后宿主拷贝 spec)
    static std::vector<std::string> g_storage;
    g_storage.push_back(std::move(depict));
    g_storage.push_back(std::move(schema));

    static std::vector<std::unique_ptr<ToolEntry>> g_entries;
    auto entry = std::make_unique<ToolEntry>();
    entry->mgr = mgr;
    entry->fn  = std::move(fn);
    auto* entryPtr = entry.get();
    g_entries.push_back(std::move(entry));

    AgentxxToolSpec spec{};
    spec.name            = agentxx_plugin_sv(name, std::strlen(name));
    spec.description     = agentxx_plugin_sv(g_storage[g_storage.size() - 2].data(), g_storage[g_storage.size() - 2].size());
    spec.parameters_json = agentxx_plugin_sv(g_storage.back().data(), g_storage.back().size());
    spec.user_data       = entryPtr;
    spec.flags           = flags;
    spec.execute         = +[](void* ud, AgentxxPluginStringView args_json, AgentxxPluginStringView, AgentxxPluginStringView, char** err
                           ) -> char* {
        auto* e = static_cast<ToolEntry*>(ud);
        try {
            std::string argsStr{args_json.data ? args_json.data : "{}", args_json.size};
            SimpleJson  args(argsStr.empty() ? "{}" : argsStr);
            if (!args.ok()) {
                throw std::runtime_error("invalid args json");
            }
            std::string out = e->mgr ? e->fn(e->mgr, args) : "error: CodeGraphManager unavailable";
            return pluginStrdup(out.c_str());
        } catch (const std::exception& ex) {
            if (err) {
                *err = pluginStrdup(ex.what());
            }
            return nullptr;
        } catch (...) {
            if (err) {
                *err = pluginStrdup("unknown exception");
            }
            return nullptr;
        }
    };
    if (g_host->vtable->register_tool(g_host, &spec) != 0) {
        pluginLog(3, fmt::format("agentxx_codegraph: register tool {} failed", name));
    }
}

// =====================================================================
// 8 个工具定义
// =====================================================================

static void registerAllTools(CodeGraphManager* mgr) {
    constexpr int kAutoSummary = AGENTXX_TOOL_FLAG_AUTO_SUMMARY;

    // agentxx_codegraph_search
    registerTool(
        mgr,
        "agentxx_codegraph_search",
        "Search for code symbols (functions, classes, variables, etc.) by name using the codegraph index.",
        [](const ToolPrompt& p) {
            SchemaBuilder b;
            b.str(
                "query",
                p.query.empty()
                    ? "Symbol name to search for. Supports partial matching."
                    : p.query
            );
            b.num(
                "limit",
                p.limit.empty() ? "Maximum number of results to return. Default: 20." : p.limit
            );
            return b.dump({"query"});
        },
        [](CodeGraphManager* m, SimpleJson& a) {
            std::string query;
            jsonGetString(a.doc().at_pointer("/query"), query);
            if (query.empty()) {
                return std::string{"error: Arg `query` is empty"};
            }
            int64_t lim64 = 20;
            jsonGetInt(a.doc().at_pointer("/limit"), lim64);
            int limit = static_cast<int>(lim64);
            auto r    = m->searchSymbols(query, limit);
            if (!r.success) {
                return fmt::format("error: {}", r.error);
            }
            std::string out = fmt::format("Symbols ({}):", r.nodes.size());
            if (r.nodes.empty()) {
                out += "\n  (none)";
            }
            int idx = 0;
            for (const auto& node : r.nodes) {
                out += fmt::format(
                    "\n[{}] {} {}",
                    ++idx,
                    codegraph::node_kind_str(node.kind),
                    node.name
                );
                if (!node.qualified_name.empty() && node.qualified_name != node.name) {
                    out += fmt::format("\n    qualified_name: {}", node.qualified_name);
                }
                out += fmt::format("\n    file: {}:{}", node.file_path, node.line);
                if (!node.signature.empty()) {
                    out += fmt::format("\n    signature: {}", node.signature);
                }
            }
            if (m->isIndexing()) {
                out += "\nwarning: CodeGraph is still indexing, results may be incomplete";
            }
            return out;
        },
        kAutoSummary
    );

    // agentxx_codegraph_context
    registerTool(
        mgr,
        "agentxx_codegraph_context",
        "Get rich context for a code symbol: its definition, callers, callees, and methods.",
        [](const ToolPrompt& p) {
            SchemaBuilder b;
            b.str("symbol", p.symbol.empty() ? "Target symbol name." : p.symbol);
            b.num(
                "limit",
                p.limit.empty() ? "Maximum number of results per category. Default: 10." : p.limit
            );
            b.num(
                "max_depth",
                p.maxDepth.empty() ? "Maximum traversal depth. Default: 3." : p.maxDepth
            );
            return b.dump({"symbol"});
        },
        [](CodeGraphManager* m, SimpleJson& a) {
            std::string symbol;
            jsonGetString(a.doc().at_pointer("/symbol"), symbol);
            if (symbol.empty()) {
                return std::string{"error: Arg `symbol` is empty"};
            }
            int64_t limit64 = 10, depth64 = 3;
            jsonGetInt(a.doc().at_pointer("/limit"), limit64);
            jsonGetInt(a.doc().at_pointer("/max_depth"), depth64);
            int limit     = static_cast<int>(limit64);
            int max_depth = static_cast<int>(depth64);
            auto r        = m->getSymbolContext(symbol, limit, max_depth);
            if (!r.success) {
                return fmt::format("error: {}", r.error);
            }
            return contextToText(r.context, m);
        },
        kAutoSummary
    );

    // agentxx_codegraph_callers
    registerTool(
        mgr,
        "agentxx_codegraph_callers",
        "Find all functions that call a given symbol (reverse call-graph traversal).",
        [](const ToolPrompt& p) {
            SchemaBuilder b;
            b.str("symbol", p.symbol.empty() ? "Target symbol name." : p.symbol);
            b.num(
                "max_depth",
                p.maxDepth.empty() ? "Maximum traversal depth. Default: 3." : p.maxDepth
            );
            return b.dump({"symbol"});
        },
        [](CodeGraphManager* m, SimpleJson& a) {
            std::string symbol;
            jsonGetString(a.doc().at_pointer("/symbol"), symbol);
            if (symbol.empty()) {
                return std::string{"error: Arg `symbol` is empty"};
            }
            int64_t depth64 = 3;
            jsonGetInt(a.doc().at_pointer("/max_depth"), depth64);
            int max_depth = static_cast<int>(depth64);
            auto r        = m->getCallers(symbol, max_depth);
            if (!r.success) {
                return fmt::format("error: {}", r.error);
            }
            return impactToText("Callers", r.impact, m);
        },
        kAutoSummary
    );

    // agentxx_codegraph_callees
    registerTool(
        mgr,
        "agentxx_codegraph_callees",
        "Find all functions that a given symbol calls (forward call-graph traversal).",
        [](const ToolPrompt& p) {
            SchemaBuilder b;
            b.str("symbol", p.symbol.empty() ? "Target symbol name." : p.symbol);
            b.num(
                "max_depth",
                p.maxDepth.empty() ? "Maximum traversal depth. Default: 3." : p.maxDepth
            );
            return b.dump({"symbol"});
        },
        [](CodeGraphManager* m, SimpleJson& a) {
            std::string symbol;
            jsonGetString(a.doc().at_pointer("/symbol"), symbol);
            if (symbol.empty()) {
                return std::string{"error: Arg `symbol` is empty"};
            }
            int64_t depth64 = 3;
            jsonGetInt(a.doc().at_pointer("/max_depth"), depth64);
            int max_depth = static_cast<int>(depth64);
            auto r        = m->getCallees(symbol, max_depth);
            if (!r.success) {
                return fmt::format("error: {}", r.error);
            }
            return impactToText("Callees", r.impact, m);
        },
        kAutoSummary
    );

    // agentxx_codegraph_impact
    registerTool(
        mgr,
        "agentxx_codegraph_impact",
        "Analyze the impact of modifying a symbol: all downstream symbols that may be affected.",
        [](const ToolPrompt& p) {
            SchemaBuilder b;
            b.str("symbol", p.symbol.empty() ? "Target symbol name." : p.symbol);
            b.num(
                "max_depth",
                p.maxDepth.empty() ? "Maximum traversal depth. Default: 5." : p.maxDepth
            );
            return b.dump({"symbol"});
        },
        [](CodeGraphManager* m, SimpleJson& a) {
            std::string symbol;
            jsonGetString(a.doc().at_pointer("/symbol"), symbol);
            if (symbol.empty()) {
                return std::string{"error: Arg `symbol` is empty"};
            }
            int64_t depth64 = 5;
            jsonGetInt(a.doc().at_pointer("/max_depth"), depth64);
            int max_depth = static_cast<int>(depth64);
            auto r        = m->getImpact(symbol, max_depth);
            if (!r.success) {
                return fmt::format("error: {}", r.error);
            }
            return impactToText("Impact", r.impact, m);
        },
        kAutoSummary
    );

    // agentxx_codegraph_status
    registerTool(
        mgr,
        "agentxx_codegraph_status",
        "Get codegraph index statistics: total nodes, edges, indexed files, circular dependencies.",
        [](const ToolPrompt&) {
            SchemaBuilder b;
            return b.dump({});
        },
        [](CodeGraphManager* m, SimpleJson&) {
            auto r = m->getStatus();
            if (!r.success) {
                return fmt::format("error: {}", r.error);
            }
            std::string out = fmt::format(
                "total_nodes: {}\ntotal_edges: {}\ntotal_files: {}\ncircular_deps: {}",
                r.total_nodes,
                r.total_edges,
                r.total_files,
                r.circular_deps
            );
            if (m->isIndexing()) {
                out += "\nwarning: CodeGraph is still indexing, results may be incomplete";
            }
            return out;
        },
        0
    );

    // agentxx_codegraph_index
    registerTool(
        mgr,
        "agentxx_codegraph_index",
        "Index a directory for code analysis. Parses source files and builds the symbol database.",
        [](const ToolPrompt& p) {
            SchemaBuilder b;
            b.str("path", p.path.empty() ? "Absolute path to the directory to index." : p.path);
            b.boolean(
                "incremental",
                p.incremental.empty()
                    ? "If true, only re-index changed files. If false, full re-index. Default: true."
                    : p.incremental
            );
            return b.dump({"path"});
        },
        [](CodeGraphManager* m, SimpleJson& a) {
            std::string path;
            jsonGetString(a.doc().at_pointer("/path"), path);
            if (path.empty()) {
                return std::string{"error: Arg `path` is empty"};
            }
            bool incremental = true;
            jsonGetBool(a.doc().at_pointer("/incremental"), incremental);
            bool ok          = m->indexDirectory(path, incremental);
            auto status      = m->getStatus();
            if (ok) {
                if (status.success) {
                    return fmt::format(
                        "success: true\ntotal_nodes: {}\ntotal_edges: {}\ntotal_files: {}",
                        status.total_nodes,
                        status.total_edges,
                        status.total_files
                    );
                }
                return fmt::format(
                    "error: Indexing done, but status query failed: {}",
                    status.error
                );
            }
            if (!status.success && !status.error.empty()) {
                return fmt::format("error: Indexing failed: {}", status.error);
            }
            return std::string{"error: Indexing failed"};
        },
        0
    );

    // agentxx_codegraph_path
    registerTool(
        mgr,
        "agentxx_codegraph_path",
        "Find the call-chain path between two symbols in the call graph.",
        [](const ToolPrompt& p) {
            SchemaBuilder b;
            b.str("from", p.from.empty() ? "Starting symbol name." : p.from);
            b.str("to", p.to.empty() ? "Target symbol name." : p.to);
            b.num(
                "max_depth",
                p.maxDepth.empty() ? "Maximum search depth. Default: 10." : p.maxDepth
            );
            return b.dump({"from", "to"});
        },
        [](CodeGraphManager* m, SimpleJson& a) {
            std::string from, to;
            jsonGetString(a.doc().at_pointer("/from"), from);
            jsonGetString(a.doc().at_pointer("/to"), to);
            if (from.empty() || to.empty()) {
                return std::string{"error: Args `from` and `to` are required"};
            }
            int64_t depth64 = 10;
            jsonGetInt(a.doc().at_pointer("/max_depth"), depth64);
            int max_depth = static_cast<int>(depth64);
            auto r        = m->findPath(from, to, max_depth);
            if (!r.success) {
                return fmt::format("error: {}", r.error);
            }
            std::string out = fmt::format("Path ({}):", r.path.size());
            if (r.path.empty()) {
                out += "\n  (none)";
            }
            int idx = 0;
            for (const auto& node : r.path) {
                out += fmt::format(
                    "\n[{}] {} {}",
                    ++idx,
                    codegraph::node_kind_str(node.kind),
                    node.name
                );
                if (!node.qualified_name.empty() && node.qualified_name != node.name) {
                    out += fmt::format("\n    qualified_name: {}", node.qualified_name);
                }
                out += fmt::format("\n    file: {}:{}", node.file_path, node.line);
                if (!node.signature.empty()) {
                    out += fmt::format("\n    signature: {}", node.signature);
                }
            }
            if (m->isIndexing()) {
                out += "\nwarning: CodeGraph is still indexing, results may be incomplete";
            }
            return out;
        },
        kAutoSummary
    );
}

} // namespace agentxx_codegraph_plugin

// =====================================================================
// 插件入口 (C ABI)
// =====================================================================

using namespace agentxx_codegraph_plugin;

extern "C" const AgentxxPluginInfo* agentxx_plugin_get_info(void) {
    static const AgentxxPluginInfo info{
        AGENTXX_PLUGIN_API_VERSION,
        AGENTXX_SV("agentxx_codegraph"),
        AGENTXX_SV("1.0.0"),
        AGENTXX_SV(
            "CodeGraph code analysis: symbol search/context/callers/callees/impact/index/status/path"
        ),
    };
    return &info;
}

struct PluginCtx {
    std::shared_ptr<agentxx::expand::CodeGraphManager> mgr;
    std::thread                                        warmup;
    std::atomic<bool>                                  stop{false};
};

extern "C" int agentxx_plugin_entry(const AgentxxHost* host, void** plugin_ctx) {
    g_host = host;

    auto cfg = readHostConfig();
    // 加载由宿主决定 (yaml plugins 条目 enabled + CodeAgent 调用加载);
    // 插件仅需 dataDir 非空才能落盘索引 (内存模式跳过)
    if (cfg.dataDir.empty()) {
        pluginLog(
            3,
            "agentxx_codegraph: dataDir is not set, skip codegraph tools "
            "(configure data_dir for index persistence)"
        );
        return 0;
    }

    // 索引过滤配置 (与原 CodeAgent 一致)
    agentxx::expand::CodeGraphIndexConfig cgConfig;
    cgConfig.loadPaths           = cfg.loadPaths;
    cgConfig.ignorePaths         = cfg.ignorePaths;
    cgConfig.useGitignore        = cfg.useGitignore;
    cgConfig.autoLoadProjectRoot = cfg.loadCwd;

    auto ctx               = std::make_unique<PluginCtx>();
    std::string sqliteDir  = (std::filesystem::path(cfg.dataDir) / "sqlite").string();
    ctx->mgr               = std::make_shared<agentxx::expand::CodeGraphManager>(sqliteDir, cgConfig);

    // 索引进度回调 → publish("agentxx_codegraph.progress") 通知宿主
    // (topic 约定 `{插件名}.{事件名}`; 频率由 CodeGraphManager 内部节流)
    ctx->mgr->setProgressCallback([](int processed, int total, std::string_view currentFile) {
        if (!g_host || !g_host->vtable || !g_host->vtable->publish) {
            return;
        }
        codegraph::Json j = codegraph::Json::object();
        j["processed"]    = processed;
        j["total"]        = total;
        j["current_file"] = std::string{currentFile};
        std::string payload = j.dump();
        g_host->vtable->publish(
            g_host,
            AGENTXX_SV("agentxx_codegraph.progress"),
            agentxx_plugin_sv(payload.data(), payload.size())
        );
    });

    std::string projectRoot = cfg.projectRoot;
    if (projectRoot.empty()) {
        std::error_code ec;
        projectRoot = std::filesystem::current_path(ec).string();
        if (ec) {
            projectRoot.clear();
        }
    }
    if (projectRoot.empty()) {
        pluginLog(4, "agentxx_codegraph: get current work path failed, skip codegraph tools");
        return 0;
    }

    if (!ctx->mgr->initialize(projectRoot)) {
        pluginLog(4, "agentxx_codegraph: initialize failed, skip codegraph tools");
        return 0;
    }

    registerAllTools(ctx->mgr.get());

    // 后台预热索引 (2s 后增量索引; 与原 CodeAgent warmup 一致):
    // - 独立线程 + 原子停止位; unload 时 join, 保证 dlclose 前无执行者
    ctx->stop.store(false);
    ctx->warmup = std::thread([ctxPtr = ctx.get()]() {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (ctxPtr->stop.load(std::memory_order_acquire)) {
            return;
        }
        auto t0 = std::chrono::steady_clock::now();
        bool ok = ctxPtr->mgr->updateIndex();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0
        )
                      .count();
        pluginLog(
            ok ? 2 : 3,
            fmt::format("[codegraph] background warmup index {} ({}ms)", ok ? "done" : "failed", ms)
        );
    });

    *plugin_ctx = ctx.release();
    pluginLog(2, "agentxx_codegraph loaded (8 tools)");

    // 发布加载状态事件 (客户端据此显示插件可用)
    if (g_host && g_host->vtable && g_host->vtable->publish) {
        codegraph::Json j = codegraph::Json::object();
        j["loaded"]         = true;
        j["project_root"]   = projectRoot;
        std::string payload = j.dump();
        g_host->vtable->publish(
            g_host,
            AGENTXX_SV("agentxx_codegraph.status"),
            agentxx_plugin_sv(payload.data(), payload.size())
        );
    }
    return 0;
}

extern "C" void agentxx_plugin_unload(void* plugin_ctx) {
    auto* ctx = static_cast<PluginCtx*>(plugin_ctx);
    if (!ctx) {
        return;
    }
    // 发布卸载状态事件 (客户端据此隐藏插件状态)
    if (g_host && g_host->vtable && g_host->vtable->publish) {
        codegraph::Json j = codegraph::Json::object();
        j["loaded"]         = false;
        std::string payload = j.dump();
        g_host->vtable->publish(
            g_host,
            AGENTXX_SV("agentxx_codegraph.status"),
            agentxx_plugin_sv(payload.data(), payload.size())
        );
    }
    ctx->stop.store(true, std::memory_order_release);
    if (ctx->warmup.joinable()) {
        ctx->warmup.join();
    }
    if (ctx->mgr) {
        ctx->mgr->shutdown();
    }
    pluginLog(2, "agentxx_codegraph unloaded");
    delete ctx;
}
