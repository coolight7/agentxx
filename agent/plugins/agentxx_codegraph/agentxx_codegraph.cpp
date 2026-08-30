// agentxx_codegraph —— CodeGraph 代码分析插件
#include "agentxx/plugin/client_plugin_api.h"
#include "codegraph_manager.h"
#include "codegraph_plugin.h"
#include "fmt/format.h"
#include "fmt/ranges.h"
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

using CodeGraphManager = agentxx_codegraph_plugin::CodeGraphManager;

struct HostConfig {
    std::string              dataDir;
    std::string              projectRoot;
    std::vector<std::string> loadPaths;
    std::vector<std::string> ignorePaths;
    bool                     useGitignore = true;
    bool                     loadCwd      = true;
};

struct PluginCtx : public agentxx::kit::PluginBase {
    std::shared_ptr<agentxx_codegraph_plugin::CodeGraphManager> mgr;
    std::thread                                                 warmup;
    std::atomic<bool>                                           stop{false};
    std::string                                                 projectRoot;
    volatile int                                                snapshot_cancel_flag = 0;

    ~PluginCtx() {
        if (warmup.joinable()) {
            stop.store(true, std::memory_order_release);
            warmup.join();
        }
    }
};

static auto ctxGuardLogger(PluginCtx* ctx) noexcept {
    return [ctx](const char* msg) noexcept {
        if (ctx) {
            ctx->log.error(msg ? msg : "");
        }
    };
}

static HostConfig
    readHostConfig(const AgentxxHost* host, const agentxx::plugin::AgentIfaces& iface) {
    HostConfig cfg;
    if (!host || !iface.config) {
        return cfg;
    }
    if (iface.config->get_config) {
        char* json = iface.config->get_config(host);
        if (json) {
            std::string s{json};
            host->vtable->free(json);
            SimpleJson j(s);
            if (j.ok()) {
                jsonGetString(j.doc().at_pointer("/dataDir"), cfg.dataDir);
                jsonGetString(j.doc().at_pointer("/projectRoot"), cfg.projectRoot);
            }
        }
    }
    if (iface.config->get_plugin_args) {
        char* json = iface.config->get_plugin_args(host);
        if (json) {
            std::string s{json};
            host->vtable->free(json);
            SimpleJson j(s);
            if (j.ok()) {
                auto& doc  = j.doc();
                auto  pRes = doc.at_pointer("/paths");
                if (!pRes.error()) {
                    auto arr = pRes.get_array();
                    if (!arr.error()) {
                        for (auto item : arr.value()) {
                            std::string str;
                            if (jsonGetString(item, str) && !str.empty()) {
                                cfg.loadPaths.push_back(str);
                            }
                        }
                    }
                }
                auto ipRes = doc.at_pointer("/ignore_paths");
                if (!ipRes.error()) {
                    auto arr = ipRes.get_array();
                    if (!arr.error()) {
                        for (auto item : arr.value()) {
                            std::string str;
                            if (jsonGetString(item, str) && !str.empty()) {
                                cfg.ignorePaths.push_back(str);
                            }
                        }
                    }
                }
                jsonGetBool(doc.at_pointer("/use_gitignore"), cfg.useGitignore);
                jsonGetBool(doc.at_pointer("/load_cwd"), cfg.loadCwd);
            }
        }
    }
    return cfg;
}

struct ToolPrompt {
    std::string depict;
    std::string query;
    std::string limit;
    std::string symbol;
    std::string maxDepth;
    std::string path;
    std::string incremental;
    std::string from;
    std::string to;
};

static ToolPrompt defaultToolPrompt(std::string_view name) {
    ToolPrompt p;
    if (name == "agentxx_codegraph_search") {
        p.depict
            = "Search for code symbols (functions, classes, variables, etc.) by name using the "
              "codegraph index.\n"
              "Use this to quickly locate definitions across a large codebase.\n"
              "Returns plain multi-line text: one block per symbol (\"[N] kind name\" plus "
              "indented file:line and signature lines).";
        p.query = "Symbol name to search for. Supports partial matching.";
        p.limit = "Maximum number of results to return. Default: 20.";
    } else if (name == "agentxx_codegraph_context") {
        p.depict
            = "Get rich context for a code symbol: its definition, callers, callees, and methods "
              "(for classes).\n"
              "Useful for understanding how a function or class is used throughout the codebase.\n"
              "Returns plain multi-line text with \"symbol:\", \"callers (N):\", \"callees "
              "(N):\", \"methods (N):\" and \"edges (N):\" sections.";
        p.symbol   = "Fully qualified symbol name (e.g. `MyClass::myMethod`).";
        p.limit    = "Maximum results per category. Default: 10.";
        p.maxDepth = "Maximum call-graph traversal depth. Default: 3.";
    } else if (name == "agentxx_codegraph_callers") {
        p.depict
            = "Find all functions that call a given symbol (reverse call-graph traversal).\n"
              "Use this to understand what depends on a function before modifying it.\n"
              "Returns plain multi-line text: \"Callers (N):\" followed by one block per symbol.";
        p.symbol   = "Symbol name to find callers for.";
        p.maxDepth = "Maximum traversal depth. Default: 3.";
    } else if (name == "agentxx_codegraph_callees") {
        p.depict
            = "Find all functions that a given symbol calls (forward call-graph traversal).\n"
              "Use this to understand a function's dependencies.\n"
              "Returns plain multi-line text: \"Callees (N):\" followed by one block per symbol.";
        p.symbol   = "Symbol name to find callees for.";
        p.maxDepth = "Maximum traversal depth. Default: 3.";
    } else if (name == "agentxx_codegraph_impact") {
        p.depict
            = "Analyze the impact of modifying a symbol. Finds all downstream symbols that may be\n"
              "affected (callers, references). Use this before refactoring to assess blast "
              "radius.\n"
              "Returns plain multi-line text: \"Impact (N):\" followed by one block per symbol.";
        p.symbol   = "Symbol name to analyze impact for.";
        p.maxDepth = "Maximum traversal depth. Default: 5.";
    } else if (name == "agentxx_codegraph_path") {
        p.depict
            = "Find the call-chain path between two symbols in the call graph.\n"
              "Use this to trace how execution flows from one function to another.\n"
              "Returns plain multi-line text: \"Path (N):\" followed by one block per symbol on "
              "the path.";
        p.from     = "Starting symbol name.";
        p.to       = "Target symbol name.";
        p.maxDepth = "Maximum search depth. Default: 10.";
    }
    return p;
}

static void
    ensureToolPromptsInHost(const AgentxxHost* host, const agentxx::plugin::AgentIfaces& iface) {
    if (!host || !iface.prompt || !iface.prompt->get_prompt || !iface.prompt->set_prompt) {
        return;
    }
    char* json = iface.prompt->get_prompt(host);
    if (!json) {
        return;
    }
    std::string s{json};
    host->vtable->free(json);
    SimpleJson j(s);
    if (!j.ok()) {
        return;
    }
    static const char* kToolNames[] = {
        "agentxx_codegraph_search",
        "agentxx_codegraph_context",
        "agentxx_codegraph_callers",
        "agentxx_codegraph_callees",
        "agentxx_codegraph_impact",
        "agentxx_codegraph_path",
    };
    codegraph::Json patch = codegraph::Json::object();
    codegraph::Json tools = codegraph::Json::object();
    bool            dirty = false;
    for (const char* name : kToolNames) {
        std::string pointer = "/toolPrompt/" + std::string{name};
        if (!j.doc().at_pointer(pointer).error()) {
            continue;
        }
        auto p = defaultToolPrompt(name);
        if (p.depict.empty()) {
            continue;
        }
        codegraph::Json tp   = codegraph::Json::object();
        tp["depict"]         = p.depict;
        codegraph::Json args = codegraph::Json::object();
        if (!p.query.empty()) {
            args["query"] = p.query;
        }
        if (!p.limit.empty()) {
            args["limit"] = p.limit;
        }
        if (!p.symbol.empty()) {
            args["symbol"] = p.symbol;
        }
        if (!p.maxDepth.empty()) {
            args["max_depth"] = p.maxDepth;
        }
        if (!p.path.empty()) {
            args["path"] = p.path;
        }
        if (!p.incremental.empty()) {
            args["incremental"] = p.incremental;
        }
        if (!p.from.empty()) {
            args["from"] = p.from;
        }
        if (!p.to.empty()) {
            args["to"] = p.to;
        }
        tp["args"]  = args;
        tools[name] = tp;
        dirty       = true;
    }
    if (!dirty) {
        return;
    }
    patch["toolPrompt"] = tools;
    std::string payload = patch.dump();
    if (iface.prompt->set_prompt(host, agentxx_plugin_sv(payload.data(), payload.size())) != 0) {
        pluginLog(host, iface.log, 3, "agentxx_codegraph: set_prompt failed");
    }
}

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
        const auto& e  = arr[i];
        out           += fmt::format(
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
        const auto& s  = ctx["symbol"];
        out           += fmt::format("symbol: {} {}", s.value("kind", ""), s.value("name", ""));
        appendJsonNodeDetails(out, s);
    }
    if (ctx.contains("callers") && ctx["callers"].is_array()) {
        if (!out.empty()) {
            out += "\n";
        }
        out += jsonNodesToText("callers", ctx["callers"]);
    }
    if (ctx.contains("callees") && ctx["callees"].is_array()) {
        if (!out.empty()) {
            out += "\n";
        }
        out += jsonNodesToText("callees", ctx["callees"]);
    }
    if (ctx.contains("methods") && ctx["methods"].is_array()) {
        if (!out.empty()) {
            out += "\n";
        }
        out += jsonNodesToText("methods", ctx["methods"]);
    }
    if (ctx.contains("edges") && ctx["edges"].is_array()) {
        if (!out.empty()) {
            out += "\n";
        }
        out += jsonEdgesToText(ctx["edges"]);
    }
    if (m && m->isIndexing()) {
        out += "\nwarning: CodeGraph is still indexing, results may be incomplete";
    }
    return out.empty() ? "(empty context)" : out;
}

static std::string
    impactToText(std::string_view title, const codegraph::Json& impact, CodeGraphManager* m) {
    std::string out;
    if (impact.contains("nodes") && impact["nodes"].is_array()) {
        out += jsonNodesToText(title, impact["nodes"]);
    } else {
        out += fmt::format("{} (0):\n  (none)", title);
    }
    if (impact.contains("edges") && impact["edges"].is_array() && impact["edges"].size() > 0) {
        out += "\n" + jsonEdgesToText(impact["edges"]);
    }
    if (m && m->isIndexing()) {
        out += "\nwarning: CodeGraph is still indexing, results may be incomplete";
    }
    return out;
}

struct SchemaBuilder {
    codegraph::Json properties = codegraph::Json::object();

    void str(std::string_view key, std::string_view desc) {
        properties[std::string(key)] = codegraph::Json{
            {"type",        "string"         },
            {"description", std::string(desc)}
        };
    }

    void num(std::string_view key, std::string_view desc) {
        properties[std::string(key)] = codegraph::Json{
            {"type",        "number"         },
            {"description", std::string(desc)}
        };
    }

    std::string dump(std::initializer_list<std::string_view> req = {}) const {
        codegraph::Json s;
        s["type"]       = "object";
        s["properties"] = properties;
        if (req.size() > 0) {
            auto arr = codegraph::Json::array();
            for (auto r : req) {
                arr.push_back(codegraph::Json(std::string(r)));
            }
            s["required"] = arr;
        }
        return s.dump();
    }
};

static void registerAllTools(PluginCtx& ctx) {
    constexpr int kAutoSummary = AGENTXX_TOOL_FLAG_AUTO_SUMMARY;

    // agentxx_codegraph_search
    {
        auto        p = ctx.toolPrompt("agentxx_codegraph_search");
        std::string depict
            = p.depict.empty() ? defaultToolPrompt("agentxx_codegraph_search").depict : p.depict;
        SchemaBuilder b;
        b.str(
            "query",
            p.args.contains("query") ? p.args["query"]
                                     : "Symbol name to search for. Supports partial matching."
        );
        b.num(
            "limit",
            p.args.contains("limit") ? p.args["limit"]
                                     : "Maximum number of results to return. Default: 20."
        );
        agentxx::kit::blocking_tool(
            ctx,
            "agentxx_codegraph_search",
            depict,
            b.dump({"query"}),
            [](PluginCtx& c, std::string_view args_json) -> std::string {
                std::string argsStr(args_json.data() ? args_json.data() : "{}", args_json.size());
                SimpleJson  a(argsStr.empty() ? "{}" : argsStr);
                std::string query;
                jsonGetString(a.doc().at_pointer("/query"), query);
                if (query.empty()) {
                    return "error: Arg `query` is empty";
                }
                int64_t limit64 = 20;
                jsonGetInt(a.doc().at_pointer("/limit"), limit64);
                auto r = c.mgr->searchSymbols(query, static_cast<int>(limit64));
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
                if (c.mgr->isIndexing()) {
                    out += "\nwarning: CodeGraph is still indexing, results may be incomplete";
                }
                return out;
            },
            0,
            kAutoSummary
        );
    }

    // agentxx_codegraph_context
    {
        auto        p = ctx.toolPrompt("agentxx_codegraph_context");
        std::string depict
            = p.depict.empty() ? defaultToolPrompt("agentxx_codegraph_context").depict : p.depict;
        SchemaBuilder b;
        b.str("symbol", p.args.contains("symbol") ? p.args["symbol"] : "Target symbol name.");
        b.num(
            "limit",
            p.args.contains("limit") ? p.args["limit"]
                                     : "Maximum results per category. Default: 10."
        );
        b.num(
            "max_depth",
            p.args.contains("max_depth") ? p.args["max_depth"]
                                         : "Maximum traversal depth. Default: 3."
        );
        agentxx::kit::blocking_tool(
            ctx,
            "agentxx_codegraph_context",
            depict,
            b.dump({"symbol"}),
            [](PluginCtx& c, std::string_view args_json) -> std::string {
                std::string argsStr(args_json.data() ? args_json.data() : "{}", args_json.size());
                SimpleJson  a(argsStr.empty() ? "{}" : argsStr);
                std::string symbol;
                jsonGetString(a.doc().at_pointer("/symbol"), symbol);
                if (symbol.empty()) {
                    return "error: Arg `symbol` is empty";
                }
                int64_t limit64 = 10, depth64 = 3;
                jsonGetInt(a.doc().at_pointer("/limit"), limit64);
                jsonGetInt(a.doc().at_pointer("/max_depth"), depth64);
                auto r = c.mgr->getSymbolContext(
                    symbol,
                    static_cast<int>(limit64),
                    static_cast<int>(depth64)
                );
                if (!r.success) {
                    return fmt::format("error: {}", r.error);
                }
                return contextToText(r.context, c.mgr.get());
            },
            0,
            kAutoSummary
        );
    }

    // agentxx_codegraph_callers
    {
        auto        p = ctx.toolPrompt("agentxx_codegraph_callers");
        std::string depict
            = p.depict.empty() ? defaultToolPrompt("agentxx_codegraph_callers").depict : p.depict;
        SchemaBuilder b;
        b.str("symbol", p.args.contains("symbol") ? p.args["symbol"] : "Target symbol name.");
        b.num(
            "max_depth",
            p.args.contains("max_depth") ? p.args["max_depth"]
                                         : "Maximum traversal depth. Default: 3."
        );
        agentxx::kit::blocking_tool(
            ctx,
            "agentxx_codegraph_callers",
            depict,
            b.dump({"symbol"}),
            [](PluginCtx& c, std::string_view args_json) -> std::string {
                std::string argsStr(args_json.data() ? args_json.data() : "{}", args_json.size());
                SimpleJson  a(argsStr.empty() ? "{}" : argsStr);
                std::string symbol;
                jsonGetString(a.doc().at_pointer("/symbol"), symbol);
                if (symbol.empty()) {
                    return "error: Arg `symbol` is empty";
                }
                int64_t depth64 = 3;
                jsonGetInt(a.doc().at_pointer("/max_depth"), depth64);
                auto r = c.mgr->getCallers(symbol, static_cast<int>(depth64));
                if (!r.success) {
                    return fmt::format("error: {}", r.error);
                }
                return impactToText("Callers", r.impact, c.mgr.get());
            },
            0,
            kAutoSummary
        );
    }

    // agentxx_codegraph_callees
    {
        auto        p = ctx.toolPrompt("agentxx_codegraph_callees");
        std::string depict
            = p.depict.empty() ? defaultToolPrompt("agentxx_codegraph_callees").depict : p.depict;
        SchemaBuilder b;
        b.str("symbol", p.args.contains("symbol") ? p.args["symbol"] : "Target symbol name.");
        b.num(
            "max_depth",
            p.args.contains("max_depth") ? p.args["max_depth"]
                                         : "Maximum traversal depth. Default: 3."
        );
        agentxx::kit::blocking_tool(
            ctx,
            "agentxx_codegraph_callees",
            depict,
            b.dump({"symbol"}),
            [](PluginCtx& c, std::string_view args_json) -> std::string {
                std::string argsStr(args_json.data() ? args_json.data() : "{}", args_json.size());
                SimpleJson  a(argsStr.empty() ? "{}" : argsStr);
                std::string symbol;
                jsonGetString(a.doc().at_pointer("/symbol"), symbol);
                if (symbol.empty()) {
                    return "error: Arg `symbol` is empty";
                }
                int64_t depth64 = 3;
                jsonGetInt(a.doc().at_pointer("/max_depth"), depth64);
                auto r = c.mgr->getCallees(symbol, static_cast<int>(depth64));
                if (!r.success) {
                    return fmt::format("error: {}", r.error);
                }
                return impactToText("Callees", r.impact, c.mgr.get());
            },
            0,
            kAutoSummary
        );
    }

    // agentxx_codegraph_impact
    {
        auto        p = ctx.toolPrompt("agentxx_codegraph_impact");
        std::string depict
            = p.depict.empty() ? defaultToolPrompt("agentxx_codegraph_impact").depict : p.depict;
        SchemaBuilder b;
        b.str("symbol", p.args.contains("symbol") ? p.args["symbol"] : "Target symbol name.");
        b.num(
            "max_depth",
            p.args.contains("max_depth") ? p.args["max_depth"]
                                         : "Maximum traversal depth. Default: 5."
        );
        agentxx::kit::blocking_tool(
            ctx,
            "agentxx_codegraph_impact",
            depict,
            b.dump({"symbol"}),
            [](PluginCtx& c, std::string_view args_json) -> std::string {
                std::string argsStr(args_json.data() ? args_json.data() : "{}", args_json.size());
                SimpleJson  a(argsStr.empty() ? "{}" : argsStr);
                std::string symbol;
                jsonGetString(a.doc().at_pointer("/symbol"), symbol);
                if (symbol.empty()) {
                    return "error: Arg `symbol` is empty";
                }
                int64_t depth64 = 5;
                jsonGetInt(a.doc().at_pointer("/max_depth"), depth64);
                auto r = c.mgr->getImpact(symbol, static_cast<int>(depth64));
                if (!r.success) {
                    return fmt::format("error: {}", r.error);
                }
                return impactToText("Impact", r.impact, c.mgr.get());
            },
            0,
            kAutoSummary
        );
    }

    // agentxx_codegraph_path
    {
        auto        p = ctx.toolPrompt("agentxx_codegraph_path");
        std::string depict
            = p.depict.empty() ? defaultToolPrompt("agentxx_codegraph_path").depict : p.depict;
        SchemaBuilder b;
        b.str("from", p.args.contains("from") ? p.args["from"] : "Starting symbol name.");
        b.str("to", p.args.contains("to") ? p.args["to"] : "Target symbol name.");
        b.num(
            "max_depth",
            p.args.contains("max_depth") ? p.args["max_depth"]
                                         : "Maximum search depth. Default: 10."
        );
        agentxx::kit::blocking_tool(
            ctx,
            "agentxx_codegraph_path",
            depict,
            b.dump({"from", "to"}),
            [](PluginCtx& c, std::string_view args_json) -> std::string {
                std::string argsStr(args_json.data() ? args_json.data() : "{}", args_json.size());
                SimpleJson  a(argsStr.empty() ? "{}" : argsStr);
                std::string from, to;
                jsonGetString(a.doc().at_pointer("/from"), from);
                jsonGetString(a.doc().at_pointer("/to"), to);
                if (from.empty() || to.empty()) {
                    return "error: Args `from` and `to` are required";
                }
                int64_t depth64 = 10;
                jsonGetInt(a.doc().at_pointer("/max_depth"), depth64);
                auto r = c.mgr->findPath(from, to, static_cast<int>(depth64));
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
                if (c.mgr->isIndexing()) {
                    out += "\nwarning: CodeGraph is still indexing, results may be incomplete";
                }
                return out;
            },
            0,
            kAutoSummary
        );
    }
}

static void snapshotQueryDone(void* ud, void* result, char* error) {
    auto* ctx   = static_cast<PluginCtx*>(ud);
    auto* files = static_cast<int64_t*>(result);
    agentxx::plugin_guard::guardCallVoid(
        [ctx](const char* m) noexcept {
            pluginLog(ctx ? ctx->host : nullptr, ctx ? ctx->iface.log : nullptr, 4, m ? m : "");
        },
        [&] {
            if (ctx && ctx->host && ctx->iface.events && ctx->iface.events->publish) {
                codegraph::Json j = codegraph::Json::object();
                j["loaded"]       = true;
                if (!ctx->projectRoot.empty()) {
                    j["project_root"] = ctx->projectRoot;
                }
                std::string payload = j.dump();
                ctx->iface.events->publish(
                    ctx->host,
                    AGENTXX_SV("agentxx_codegraph.status"),
                    agentxx_plugin_sv(payload.data(), payload.size())
                );
                if (files && *files > 0) {
                    codegraph::Json p = codegraph::Json::object();
                    p["processed"]    = *files;
                    p["total"]        = *files;
                    p["current_file"] = "";
                    std::string pp    = p.dump();
                    ctx->iface.events->publish(
                        ctx->host,
                        AGENTXX_SV("agentxx_codegraph.progress"),
                        agentxx_plugin_sv(pp.data(), pp.size())
                    );
                }
            }
        }
    );
    if (ctx && ctx->host && ctx->host->vtable && ctx->host->vtable->free) {
        if (files) {
            ctx->host->vtable->free(files);
        }
        if (error) {
            ctx->host->vtable->free(error);
        }
    }
}

static void* snapshotQueryWork(void* ud, volatile int*, char**) {
    auto* ctx = static_cast<PluginCtx*>(ud);
    return agentxx::plugin_guard::guardCall(
        [ctx](const char* m) noexcept {
            pluginLog(ctx ? ctx->host : nullptr, ctx ? ctx->iface.log : nullptr, 4, m ? m : "");
        },
        nullptr,
        [&]() -> void* {
            auto files
                = ctx ? static_cast<int64_t*>(ctx->host->vtable->alloc(sizeof(int64_t))) : nullptr;
            if (files) {
                *files = -1;
                if (ctx && ctx->mgr) {
                    auto st = ctx->mgr->getStatus();
                    if (st.success) {
                        *files = st.total_files;
                    }
                }
            }
            return files;
        }
    );
}

static void on_client_attached(AgentxxPluginStringView, void* ud) {
    auto* ctx = static_cast<PluginCtx*>(ud);
    agentxx::plugin_guard::guardCallVoid(
        [ctx](const char* m) noexcept {
            pluginLog(ctx ? ctx->host : nullptr, ctx ? ctx->iface.log : nullptr, 4, m ? m : "");
        },
        [&] {
            if (!ctx || !ctx->host || !ctx->iface.scheduler || !ctx->iface.scheduler->offload) {
                return;
            }
            ctx->iface.scheduler->offload(
                ctx->host,
                &ctx->snapshot_cancel_flag,
                snapshotQueryWork,
                snapshotQueryDone,
                ctx
            );
        }
    );
}

} // namespace agentxx_codegraph_plugin

using namespace agentxx_codegraph_plugin;

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_agent_get_info(void) {
    static const AgentxxPluginInfo info{
        AGENTXX_PLUGIN_API_VERSION,
        AGENTXX_SV("agentxx_codegraph"),
        AGENTXX_SV("1.0.0"),
        AGENTXX_SV("CodeGraph code analysis: symbol search/context/callers/callees/impact/path"),
    };
    return &info;
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_agent_create(const AgentxxHost* host, void** plugin_ctx) {
    PluginCtx* raw    = nullptr;
    auto       logger = [&raw](const char* m) noexcept {
        pluginLog(raw ? raw->host : nullptr, raw ? raw->iface.log : nullptr, 4, m ? m : "");
    };
    return agentxx::plugin_guard::guardCall(std::move(logger), -1, [&]() -> int {
        if (!host || !host->vtable || !plugin_ctx) {
            return -1;
        }
        auto ctx = std::make_unique<PluginCtx>();
        ctx->init(host);
        raw = ctx.get();

        if (!ctx->iface.tools || !ctx->iface.tools->register_tool || !ctx->iface.events) {
            pluginLog(host, ctx->iface.log, 4, "agentxx_codegraph: lacks tools/events iface");
            return -1;
        }

        auto cfg = readHostConfig(ctx->host, ctx->iface);
        if (cfg.dataDir.empty()) {
            pluginLog(
                ctx->host,
                ctx->iface.log,
                3,
                "agentxx_codegraph: dataDir not set, skip tools"
            );
            *plugin_ctx = ctx.release();
            return 0;
        }

        agentxx_codegraph_plugin::CodeGraphIndexConfig cgConfig;
        cgConfig.loadPaths           = cfg.loadPaths;
        cgConfig.ignorePaths         = cfg.ignorePaths;
        cgConfig.useGitignore        = cfg.useGitignore;
        cgConfig.autoLoadProjectRoot = cfg.loadCwd;

        std::string sqliteDir = (std::filesystem::path(cfg.dataDir) / "sqlite").string();
        ctx->mgr
            = std::make_shared<agentxx_codegraph_plugin::CodeGraphManager>(sqliteDir, cgConfig);
        ctx->mgr->setLogSink([ctxPtr = ctx.get()](int level, const std::string& msg) {
            pluginLog(
                ctxPtr ? ctxPtr->host : nullptr,
                ctxPtr ? ctxPtr->iface.log : nullptr,
                level,
                msg
            );
        });

        PluginCtx* ctxRaw = ctx.get();
        ctx->mgr->setProgressCallback(
            [ctxRaw](int processed, int total, std::string_view currentFile) {
                try {
                    if (!ctxRaw || !ctxRaw->host || !ctxRaw->iface.events
                        || !ctxRaw->iface.events->publish) {
                        return;
                    }
                    codegraph::Json j   = codegraph::Json::object();
                    j["processed"]      = processed;
                    j["total"]          = total;
                    j["current_file"]   = std::string{currentFile};
                    std::string payload = j.dump();
                    ctxRaw->iface.events->publish(
                        ctxRaw->host,
                        AGENTXX_SV("agentxx_codegraph.progress"),
                        agentxx_plugin_sv(payload.data(), payload.size())
                    );
                } catch (...) {
                }
            }
        );

        std::string projectRoot = cfg.projectRoot;
        if (projectRoot.empty()) {
            std::error_code ec;
            projectRoot = std::filesystem::current_path(ec).string();
            if (ec) {
                projectRoot.clear();
            }
        }
        if (projectRoot.empty()) {
            pluginLog(ctx->host, ctx->iface.log, 4, "agentxx_codegraph: get current path failed");
            *plugin_ctx = ctx.release();
            return 0;
        }

        if (!ctx->mgr->initialize(projectRoot)) {
            pluginLog(ctx->host, ctx->iface.log, 4, "agentxx_codegraph: initialize failed");
            *plugin_ctx = ctx.release();
            return 0;
        }
        ctx->projectRoot = projectRoot;

        ensureToolPromptsInHost(ctx->host, ctx->iface);
        registerAllTools(*ctx);

        ctx->stop.store(false);
        ctx->warmup = std::thread([ctxPtr = ctx.get()]() {
            try {
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
                    ctxPtr->host,
                    ctxPtr->iface.log,
                    ok ? 2 : 3,
                    fmt::format(
                        "[codegraph] background warmup index {} ({}ms)",
                        ok ? "done" : "failed",
                        ms
                    )
                );
            } catch (...) {
            }
        });

        if (ctx->iface.events && ctx->iface.events->subscribe) {
            ctx->iface.events->subscribe(
                host,
                AGENTXX_SV("agentxx_host.client_attached"),
                on_client_attached,
                ctx.get()
            );
        }

        pluginLog(host, ctx->iface.log, 2, "agentxx_codegraph loaded (6 tools)");

        if (ctx->iface.events && ctx->iface.events->publish) {
            codegraph::Json j   = codegraph::Json::object();
            j["loaded"]         = true;
            j["project_root"]   = projectRoot;
            std::string payload = j.dump();
            ctx->iface.events->publish(
                ctx->host,
                AGENTXX_SV("agentxx_codegraph.status"),
                agentxx_plugin_sv(payload.data(), payload.size())
            );
        }

        *plugin_ctx = ctx.release();
        return 0;
    });
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<PluginCtx*>(plugin_ctx);
    agentxx::plugin_guard::guardCallVoid(
        [ctx](const char* m) noexcept {
            pluginLog(ctx ? ctx->host : nullptr, ctx ? ctx->iface.log : nullptr, 4, m ? m : "");
        },
        [&] {
            if (!ctx) {
                return;
            }
            pluginLog(ctx->host, ctx->iface.log, 2, "agentxx_codegraph unloaded");
            delete ctx;
        }
    );
}

/* ==================== client 侧入口 —— CodeGraph 索引状态渲染 (Append 段风格, 恢复 1e524e62)
 * ==================== */

/// client 侧每实例上下文 (多实例契约: 原进程级 static 状态全部移入)
struct ClientCtx {
    const AgentxxClientHost*      host = nullptr;
    agentxx::plugin::ClientIfaces iface{};
    const AgentxxClientUiIface*   ui           = nullptr;
    AgentxxInfoSection*           section      = nullptr;
    bool                          loaded       = false;
    bool                          has_progress = false;
    int64_t                       processed    = 0;
    int64_t                       total        = 0;
    std::string                   current_file;

    void logErr(const char* m) const noexcept {
        agentxx::plugin_guard::logTo(host, iface.log, 4, "agentxx_codegraph", m ? m : "");
    }
};

static std::string buildInfoItemsJson(ClientCtx& c) {
    neograph::json items    = neograph::json::array();
    auto           pushText = [&](const std::string& text, const std::string& role = "normal") {
        neograph::json it;
        it["kind"] = "text";
        it["role"] = role;
        it["text"] = text;
        items.push_back(std::move(it));
    };
    if (!c.loaded) {
        pushText("|- wait for load", "hint");
    } else {
        const bool indexing = c.total > 0 ? !(c.processed >= c.total) : (c.processed > 0);
        if (c.has_progress && indexing) {
            if (c.total > 0) {
                const double pct = static_cast<double>(c.processed) / static_cast<double>(c.total);
                pushText(
                    fmt::format("|- indexing {:.0f}% ({}/{})", pct * 100.0, c.processed, c.total),
                    "normal"
                );
                neograph::json prog;
                prog["kind"]  = "progress";
                prog["value"] = pct;
                items.push_back(std::move(prog));
            } else {
                pushText(fmt::format("|- Indexing {} files", c.processed), "normal");
            }
            if (!c.current_file.empty()) {
                std::string fname = c.current_file;
                const auto  pos   = fname.find_last_of("/\\");
                if (pos != std::string::npos) {
                    fname = fname.substr(pos + 1);
                }
                pushText(fmt::format("|  {}", fname), "hint");
            }
        } else if (c.has_progress && c.total > 0) {
            pushText(fmt::format("|- available \u00b7 {}", c.total), "normal");
        } else {
            pushText("|- wait for index", "hint");
        }
    }
    neograph::json out;
    out["items"] = std::move(items);
    return out.dump();
}

static void refreshSection(ClientCtx& c) {
    if (!c.host || !c.ui || !c.ui->update_info_section) {
        return;
    }
    if (!c.section && c.ui->register_info_section) {
        c.section = c.ui->register_info_section(
            c.host,
            AGENTXX_SV("agentxx_codegraph.status"),
            AGENTXX_SV(R"({"title":"CodeGraph"})")
        );
    }
    if (!c.section) {
        return;
    }
    const std::string json = buildInfoItemsJson(c);
    c.ui->update_info_section(c.host, c.section, agentxx_plugin_sv(json.data(), json.size()));
}

static void onClientPluginData(AgentxxPluginStringView payload_json, void* ud) {
    auto* ctx = static_cast<ClientCtx*>(ud);
    if (!ctx || !ctx->host) {
        return;
    }
    std::string raw(payload_json.data ? payload_json.data : "{}", payload_json.size);
    try {
        auto j      = neograph::json::parse(raw);
        auto plugin = j.value("plugin", std::string{});
        auto event  = j.value("event", std::string{});
        if (plugin != "agentxx_codegraph") {
            return;
        }
        neograph::json d;
        if (j.contains("data")) {
            auto dv = j["data"];
            if (dv.is_string()) {
                try {
                    d = neograph::json::parse(dv.get<std::string>());
                } catch (...) {
                    d = neograph::json::object();
                }
            } else if (dv.is_object()) {
                d = dv;
            }
        }
        if (event == "status") {
            bool loaded = d.value("loaded", false);
            ctx->loaded = loaded;
            if (!loaded) {
                ctx->has_progress = false;
                ctx->processed    = 0;
                ctx->total        = 0;
                ctx->current_file.clear();
            }
            refreshSection(*ctx);
        } else if (event == "progress") {
            int64_t     processed = d.value<int64_t>("processed", 0);
            int64_t     total     = d.value<int64_t>("total", 0);
            std::string cur       = d.value("current_file", std::string{});
            ctx->processed        = processed;
            ctx->total            = total;
            ctx->current_file     = std::move(cur);
            ctx->has_progress     = true;
            ctx->loaded           = true;
            refreshSection(*ctx);
        }
    } catch (...) {
    }
}

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxClientPluginInfo* agentxx_plugin_client_get_info(void) {
    static const AgentxxClientPluginInfo info{
        AGENTXX_CLIENT_PLUGIN_API_VERSION,
        AGENTXX_SV("agentxx_codegraph"),
        AGENTXX_SV("1.0.0"),
        AGENTXX_SV("CodeGraph index status (sidebar Info section)"),
    };
    return &info;
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_client_create(const AgentxxClientHost* host, void** plugin_ctx) {
    ClientCtx* raw = nullptr;
    return agentxx::plugin_guard::guardCall(
        [&raw](const char* m) noexcept {
            if (raw) {
                raw->logErr(m);
            }
        },
        -1,
        [&]() -> int {
            if (!host || !host->vtable || !plugin_ctx) {
                return -1;
            }
            auto ctx   = std::make_unique<ClientCtx>();
            ctx->host  = host;
            ctx->iface = agentxx::plugin::ClientIfaces::query(host);
            ctx->ui    = AGENTXX_QUERY_IFACE(host, AgentxxClientUiIface, AGENTXX_IFACE_CLIENT_UI);
            raw        = ctx.get();

            if (ctx->ui && ctx->ui->register_info_section) {
                ctx->section = ctx->ui->register_info_section(
                    host,
                    AGENTXX_SV("agentxx_codegraph.status"),
                    AGENTXX_SV(R"({"title":"CodeGraph"})")
                );
                if (ctx->section) {
                    refreshSection(*ctx);
                }
            }

            if (!ctx->iface.events || !ctx->iface.events->subscribe
                || !ctx->iface.events->subscribe(
                    host,
                    AGENTXX_CLIENT_EVT_PLUGIN_DATA,
                    onClientPluginData,
                    ctx.get()
                )) {
                return -1;
            }

            if (ctx->iface.log && ctx->iface.log->log) {
                ctx->iface.log->log(host, 2, AGENTXX_SV("agentxx_codegraph client loaded"));
            }
            *plugin_ctx = ctx.release();
            return 0;
        }
    );
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_client_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<ClientCtx*>(plugin_ctx);
    agentxx::plugin_guard::guardCallVoid(
        [ctx](const char* m) noexcept {
            if (ctx) {
                ctx->logErr(m);
            }
        },
        [&] {
            if (!ctx || !ctx->host) {
                delete ctx;
                return;
            }
            if (ctx->section && ctx->ui && ctx->ui->unregister_info_section) {
                ctx->ui->unregister_info_section(ctx->host, ctx->section);
                ctx->section = nullptr;
            }
            ctx->current_file.clear();
            if (ctx->iface.log && ctx->iface.log->log) {
                ctx->iface.log->log(ctx->host, 2, AGENTXX_SV("agentxx_codegraph client unloaded"));
            }
            delete ctx;
        }
    );
}
