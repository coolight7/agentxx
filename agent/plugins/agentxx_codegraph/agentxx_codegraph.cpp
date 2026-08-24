// agentxx_codegraph —— CodeGraph 代码分析插件
// - 从 lib 迁移: CodeGraphManager + 8 个 codegraph 工具
//   (search/context/callers/callees/impact/status/index/path)
// - 插件不链接 libagentxx: 装配期经 agentxx.agent.config 接口表 get_config 读取宿主通用信息
//   (dataDir/projectRoot), 经 get_plugin_args 读取本插件参数 (宿主不解析
//   字段语义); 索引进度经 publish 事件通知宿主 (topic 约定 `{插件名}.{事件名}`,
//   宿主原样转发 WirePluginData, 客户端据此展示)
// - 依赖: codegraph_core + tree_sitter 系 + sqlite3 + simdjson + glob
#include "agentxx/plugin/client_plugin_api.h"
#include "agentxx/plugin/plugin_iface_helper.h"
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
    if (!g_host || !g_if.config || !g_if.config->get_config) {
        return cfg;
    }
    // ---- 通用宿主信息 (get_config) ----
    if (g_if.config && g_if.config->get_config) {
        char* json = g_if.config->get_config(g_host);
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
    if (g_if.config && g_if.config->get_plugin_args) {
        char* json = g_if.config->get_plugin_args(g_host);
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

/// 工具提示词 (经 agentxx.agent.config 接口表 get_tool_prompt 读取; 未配置回退内置默认)
struct ToolPrompt {
    std::string depict;
    std::string query, limit, symbol, maxDepth, path, incremental, from, to;
};

static ToolPrompt readToolPrompt(const std::string& toolName) {
    ToolPrompt out;
    if (!g_host || !g_if.config || !g_if.config->get_tool_prompt) {
        return out;
    }
    char* json = g_if.config->get_tool_prompt(
        g_host,
        agentxx_plugin_sv(toolName.data(), toolName.size())
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

/// 插件内置默认工具提示词 (从 lib AgentPrompt 剥离迁移, 2026-08)
/// - 宿主 toolPrompt 无对应条目时经 set_prompt 写入 (见 ensureToolPromptsInHost),
///   用户可经 yaml 覆盖 (覆盖发生在插件加载前, 插件写入前先 get_prompt
///   检查条目是否已存在, 已存在则尊重用户配置不覆盖)
static ToolPrompt defaultToolPrompt(const std::string& toolName) {
    ToolPrompt p;
    if (toolName == "agentxx_codegraph_search") {
        p.depict
            = "Search for code symbols (functions, classes, variables, etc.) by name using the "
              "codegraph index.\n"
              "Use this to quickly locate definitions across a large codebase.\n"
              "Returns plain multi-line text: one block per symbol (\"[N] kind name\" plus "
              "indented file:line and signature lines).";
        p.query = "Symbol name to search for. Supports partial matching.";
        p.limit = "Maximum number of results to return. Default: 20.";
    } else if (toolName == "agentxx_codegraph_context") {
        p.depict
            = "Get rich context for a code symbol: its definition, callers, callees, and methods "
              "(for classes).\n"
              "Useful for understanding how a function or class is used throughout the codebase.\n"
              "Returns plain multi-line text with \"symbol:\", \"callers (N):\", \"callees (N):\", "
              "\"methods (N):\" and \"edges (N):\" sections.";
        p.symbol   = "Fully qualified symbol name (e.g. `MyClass::myMethod`).";
        p.limit    = "Maximum results per category. Default: 10.";
        p.maxDepth = "Maximum call-graph traversal depth. Default: 3.";
    } else if (toolName == "agentxx_codegraph_callers") {
        p.depict
            = "Find all functions that call a given symbol (reverse call-graph traversal).\n"
              "Use this to understand what depends on a function before modifying it.\n"
              "Returns plain multi-line text: \"Callers (N):\" followed by one block per symbol.";
        p.symbol   = "Symbol name to find callers for.";
        p.maxDepth = "Maximum traversal depth. Default: 3.";
    } else if (toolName == "agentxx_codegraph_callees") {
        p.depict
            = "Find all functions that a given symbol calls (forward call-graph traversal).\n"
              "Use this to understand a function's dependencies.\n"
              "Returns plain multi-line text: \"Callees (N):\" followed by one block per symbol.";
        p.symbol   = "Symbol name to find callees for.";
        p.maxDepth = "Maximum traversal depth. Default: 3.";
    } else if (toolName == "agentxx_codegraph_impact") {
        p.depict
            = "Analyze the impact of modifying a symbol. Finds all downstream symbols that may be\n"
              "affected (callers, references). Use this before refactoring to assess blast radius.\n"
              "Returns plain multi-line text: \"Impact (N):\" followed by one block per symbol.";
        p.symbol   = "Symbol name to analyze impact for.";
        p.maxDepth = "Maximum traversal depth. Default: 5.";
    } else if (toolName == "agentxx_codegraph_path") {
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

/// 把插件默认提示词写入宿主 toolPrompt (仅当宿主无对应条目时; io 线程)
/// - 用户 yaml 覆盖早于插件加载 → get_prompt 已含覆盖 → 跳过 (尊重用户配置)
/// - 宿主未提供 get_prompt/set_prompt (旧宿主) → 跳过, registerTool 回退插件默认
static void ensureToolPromptsInHost() {
    if (!g_host || !g_if.prompt || !g_if.prompt->get_prompt || !g_if.prompt->set_prompt) {
        return;
    }
    char* json = g_if.prompt->get_prompt(g_host);
    if (!json) {
        return;
    }
    std::string s{json};
    g_host->vtable->free(json);
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
        // 宿主已有条目 (用户 yaml 覆盖 / 之前已写入): 尊重, 不覆盖
        // - at_pointer 惰性求值, 指针字符串须存局部变量 (临时 c_str 生命周期不足)
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
    if (g_if.prompt->set_prompt(g_host, agentxx_plugin_sv(payload.data(), payload.size()))
        != 0) {
        pluginLog(3, "agentxx_codegraph: set_prompt failed");
    }
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

static std::string
    impactToText(std::string_view title, const codegraph::Json& impact, CodeGraphManager* m) {
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
        props[name] = codegraph::Json({
            {"type",        "string"},
            {"description", desc    }
        });
    }

    void num(const std::string& name, const std::string& desc) {
        props[name] = codegraph::Json({
            {"type",        "number"},
            {"description", desc    }
        });
    }

    void boolean(const std::string& name, const std::string& desc) {
        props[name] = codegraph::Json({
            {"type",        "boolean"},
            {"description", desc     }
        });
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
    CodeGraphManager*                                          mgr = nullptr;
    std::function<std::string(CodeGraphManager*, SimpleJson&)> fn;
};

static void registerTool(
    CodeGraphManager*                                          mgr,
    const char*                                                name,
    const char*                                                defaultDepict,
    const std::function<std::string(const ToolPrompt&)>&       schemaBuilder,
    std::function<std::string(CodeGraphManager*, SimpleJson&)> fn,
    int                                                        flags
) {
    auto        prompt = readToolPrompt(name);
    std::string depict = prompt.depict.empty() ? std::string{defaultDepict} : prompt.depict;
    std::string schema = schemaBuilder(prompt);

    // 插件侧持有 schema/depict 字符串的稳定存储 (注册后宿主拷贝 spec)
    static std::vector<std::string> g_storage;
    g_storage.push_back(std::move(depict));
    g_storage.push_back(std::move(schema));

    static std::vector<std::unique_ptr<ToolEntry>> g_entries;
    auto                                           entry = std::make_unique<ToolEntry>();
    entry->mgr                                           = mgr;
    entry->fn                                            = std::move(fn);
    auto* entryPtr                                       = entry.get();
    g_entries.push_back(std::move(entry));

    AgentxxToolSpec spec{};
    spec.name        = agentxx_plugin_sv(name, std::strlen(name));
    spec.description = agentxx_plugin_sv(
        g_storage[g_storage.size() - 2].data(),
        g_storage[g_storage.size() - 2].size()
    );
    spec.parameters_json = agentxx_plugin_sv(g_storage.back().data(), g_storage.back().size());
    spec.user_data       = entryPtr;
    spec.flags           = flags;
    spec.execute         = +[](void*                   ud,
                       AgentxxPluginStringView args_json,
                       AgentxxPluginStringView,
                       AgentxxPluginStringView,
                       char** err) -> char* {
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
    if (g_if.tools->register_tool(g_host, &spec) != 0) {
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
                p.query.empty() ? "Symbol name to search for. Supports partial matching." : p.query
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
            int  limit = static_cast<int>(lim64);
            auto r     = m->searchSymbols(query, limit);
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
            int  limit     = static_cast<int>(limit64);
            int  max_depth = static_cast<int>(depth64);
            auto r         = m->getSymbolContext(symbol, limit, max_depth);
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
            int  max_depth = static_cast<int>(depth64);
            auto r         = m->getCallers(symbol, max_depth);
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
            int  max_depth = static_cast<int>(depth64);
            auto r         = m->getCallees(symbol, max_depth);
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
            int  max_depth = static_cast<int>(depth64);
            auto r         = m->getImpact(symbol, max_depth);
            if (!r.success) {
                return fmt::format("error: {}", r.error);
            }
            return impactToText("Impact", r.impact, m);
        },
        kAutoSummary
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
            int  max_depth = static_cast<int>(depth64);
            auto r         = m->findPath(from, to, max_depth);
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

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_get_info(void) {
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
    /// 项目根目录 (entry 时确定; client_attached 快照重发时携带)
    std::string projectRoot;
};

// ---------------------------------------------------------------------------
// 宿主约定事件响应 (client_attached): 重发当前状态快照
//
// 修复"status/progress 为一次性事件、先于端点订阅或客户端接入而丢失 →
// 客户端 Info 段永久滞留 'wait for index'"的问题:
// - 服务端在端点就绪/每次握手后发布 agentxx_host.client_attached (见
//   SessionServerAgentIO), 本插件收到后重发完整状态快照 (幂等):
//   status {loaded:true, project_root} + 进度快照 progress
//   {processed==total==total_files} → 客户端渲染为 "available · N"
// - getStatus 查 sqlite 统计 (30s LRU 缓存), 经宿主 offload 到阻塞池执行,
//   done 回 io 线程 publish —— 不阻塞 io 线程
// ---------------------------------------------------------------------------
static void snapshotQueryDone(void* ud, void* result, char* error) {
    auto* ctx   = static_cast<PluginCtx*>(ud);
    auto* files = static_cast<int64_t*>(result);
    if (g_host && g_if.events && g_if.events->publish) {
        codegraph::Json j = codegraph::Json::object();
        j["loaded"]       = true;
        if (ctx && !ctx->projectRoot.empty()) {
            j["project_root"] = ctx->projectRoot;
        }
        std::string payload = j.dump();
        g_if.events->publish(
            g_host,
            AGENTXX_SV("agentxx_codegraph.status"),
            agentxx_plugin_sv(payload.data(), payload.size())
        );
        // 进度快照: 已有索引 (total_files>0) 时发"完成"语义进度
        // (processed==total>0 → 客户端判定索引结束, 显示 available)
        if (files && *files > 0) {
            codegraph::Json p = codegraph::Json::object();
            p["processed"]    = *files;
            p["total"]        = *files;
            p["current_file"] = "";
            std::string pp    = p.dump();
            g_if.events->publish(
                g_host,
                AGENTXX_SV("agentxx_codegraph.progress"),
                agentxx_plugin_sv(pp.data(), pp.size())
            );
        }
    }
    if (g_host && g_host->vtable && g_host->vtable->free) {
        if (files) {
            g_host->vtable->free(files);
        }
        if (error) {
            g_host->vtable->free(error);
        }
    }
}

static void* snapshotQueryWork(void* ud, char** error_out) {
    (void)error_out;
    auto* ctx = static_cast<PluginCtx*>(ud);
    // 结果经宿主堆分配 (offload 契约: result 须 host->alloc, done 内 free)
    auto  files = static_cast<int64_t*>(g_host->vtable->alloc(sizeof(int64_t)));
    if (files) {
        *files = -1;
    }
    if (ctx && ctx->mgr && files) {
        auto st = ctx->mgr->getStatus();
        if (st.success) {
            *files = st.total_files;
        }
    }
    return files;
}

/// 订阅回调: 收到 client_attached 后经阻塞池查询状态并重发快照
static void on_client_attached(AgentxxPluginStringView event_json, void* ud) {
    (void)event_json;
    if (!g_host || !g_if.scheduler || !g_if.scheduler->offload) {
        return;
    }
    g_if.scheduler->offload(g_host, snapshotQueryWork, snapshotQueryDone, ud);
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_entry(const AgentxxHost* host, void** plugin_ctx) {
    g_host = host;
    // COM 风格接口表查询 (entry 一次性查询缓存; 进程级静态数据, 长期有效)
    static const agentxx::plugin::AgentIfaces s_if = agentxx::plugin::AgentIfaces::query(host);
    g_if = s_if;
    if (!g_if.tools || !g_if.tools->register_tool || !g_if.events) {
        pluginLog(4, "agentxx_codegraph: host lacks agentxx.agent.tools/agentxx.agent.events interfaces");
        return -1;
    }

    auto cfg = readHostConfig();
    // 加载由宿主决定 (yaml plugins 条目 enabled + CodeAgent 调用加载);
    // 插件仅需 dataDir 非空才能落盘索引 (内存模式跳过)
    if (cfg.dataDir.empty()) {
        pluginLog(
            3,
            "agentxx_codegraph: dataDir is not set, skip codegraph tools "
            "(configure data_dir for index sessionStore)"
        );
        return 0;
    }

    // 索引过滤配置 (与原 CodeAgent 一致)
    agentxx::expand::CodeGraphIndexConfig cgConfig;
    cgConfig.loadPaths           = cfg.loadPaths;
    cgConfig.ignorePaths         = cfg.ignorePaths;
    cgConfig.useGitignore        = cfg.useGitignore;
    cgConfig.autoLoadProjectRoot = cfg.loadCwd;

    auto        ctx       = std::make_unique<PluginCtx>();
    std::string sqliteDir = (std::filesystem::path(cfg.dataDir) / "sqlite").string();
    ctx->mgr = std::make_shared<agentxx::expand::CodeGraphManager>(sqliteDir, cgConfig);

    // 索引进度回调 → publish("agentxx_codegraph.progress") 通知宿主
    // (topic 约定 `{插件名}.{事件名}`; 频率由 CodeGraphManager 内部节流)
    ctx->mgr->setProgressCallback([](int processed, int total, std::string_view currentFile) {
        if (!g_host || !g_if.events || !g_if.events->publish) {
            return;
        }
        codegraph::Json j   = codegraph::Json::object();
        j["processed"]      = processed;
        j["total"]          = total;
        j["current_file"]   = std::string{currentFile};
        std::string payload = j.dump();
        g_if.events->publish(
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
    ctx->projectRoot = projectRoot;

    // 默认提示词写入宿主 (剥离自 lib AgentPrompt; 用户 yaml 覆盖优先)
    ensureToolPromptsInHost();

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

    // 订阅宿主约定事件 client_attached: 客户端接入/重连后重发状态快照
    // (修复晚接入客户端滞留 "wait for index"; 见 snapshotQueryDone 注释)
    if (!g_if.events || !g_if.events->subscribe
        || !g_if.events->subscribe(
            host,
            AGENTXX_SV("agentxx_host.client_attached"),
            on_client_attached,
            ctx.get()
        )) {
        pluginLog(3, "agentxx_codegraph: subscribe client_attached failed");
    }

    *plugin_ctx = ctx.release();
    pluginLog(2, "agentxx_codegraph loaded (8 tools)");

    // 发布加载状态事件 (客户端据此显示插件可用)
    if (g_host && g_if.events && g_if.events->publish) {
        codegraph::Json j   = codegraph::Json::object();
        j["loaded"]         = true;
        j["project_root"]   = projectRoot;
        std::string payload = j.dump();
        g_if.events->publish(
            g_host,
            AGENTXX_SV("agentxx_codegraph.status"),
            agentxx_plugin_sv(payload.data(), payload.size())
        );
    }
    return 0;
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_unload(void* plugin_ctx) {
    auto* ctx = static_cast<PluginCtx*>(plugin_ctx);
    if (!ctx) {
        return;
    }
    // 发布卸载状态事件 (客户端据此隐藏插件状态)
    if (g_host && g_if.events && g_if.events->publish) {
        codegraph::Json j   = codegraph::Json::object();
        j["loaded"]         = false;
        std::string payload = j.dump();
        g_if.events->publish(
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

/* =====================================================================
 * client 侧入口 (agentxx_client_entry) —— CodeGraph 索引状态渲染
 *
 * TUI 的 CodeGraph 索引状态渲染已迁移到本插件, 渲染进侧边栏 Info 栏:
 * - agent 侧发布的事件 (agentxx_codegraph.status / agentxx_codegraph.progress)
 *   经服务端原样转发为 WirePluginData, 宿主 (TUI) 再经事件接收器分发到
 *   client 插件系统 (AGENTXX_CLIENT_EVT_PLUGIN_DATA, 见 TUI onPeerMessage)
 * - 本入口订阅该事件, 过滤本插件事件后更新 Info 栏段落
 *   (id "agentxx_codegraph.status", title "CodeGraph"), 展示加载状态/
 *   索引进度/当前文件
 * - CLI 宿主 (无 INFO_SECTION 能力) 下 register_info_section 返回 NULL,
 *   插件降级 (仅保持数据接收, 不渲染)
 * ===================================================================== */

static const AgentxxClientHost* g_client_host = nullptr;
/// client 侧接口表缓存 (entry 时 ClientIfaces::query 一次查询)
static const agentxx::plugin::ClientIfaces* g_client_if = nullptr;
/// "agentxx.client.ui" 展示接口表 (Info 段落等; 不支持子能力成员为 NULL, 调用前判空)
static const AgentxxClientUiIface* g_client_ui = nullptr;
static AgentxxInfoSection*      g_section     = nullptr;
/// 索引状态缓存 (事件 handler 与面板刷新均在 client io 线程, 无跨线程竞争)
static bool        g_loaded       = false;
static bool        g_has_progress = false;
static int64_t     g_processed    = 0;
static int64_t     g_total        = 0;
static std::string g_current_file;

/// 字符串 → JSON 字符串字面量 (经宿主 agentxx.client.json 接口表; 结果含引号)
static std::string clientJsonEscape(const std::string& s) {
    if (!g_client_host || !g_client_if || !g_client_if->json || s.empty()) {
        return "\"\"";
    }
    char* esc
        = g_client_if->json->json_escape(g_client_host, agentxx_plugin_sv(s.data(), s.size()));
    if (!esc) {
        return "\"\"";
    }
    std::string out{esc};
    g_client_host->vtable->free(esc);
    return out;
}

/// 组装 Info 栏段落 items JSON (kind: text/progress; text 支持 role:
/// title=高亮 / normal=普通(默认) / hint=减淡; schema 见
/// client_plugin_api.h register_info_section; 段落标题 "CodeGraph" 已在注册
/// 时指定, 不再重复输出 badge; 列表项由宿主按 Append 段样式 "|  xxx" 展示,
/// 插件不拼接前缀)
/// - 各条目用 fmt::format 构造, 最后 fmt::join 组装 (避免手工字符串拼接)
static std::string buildInfoItemsJson() {
    std::vector<std::string> items;
    auto textItem = [&](const std::string& text, const std::string& role = "normal") {
        items.push_back(fmt::format(
            R"({{"kind":"text","role":{},"text":{}}})",
            clientJsonEscape(role),
            clientJsonEscape(text)
        ));
    };

    if (!g_loaded) {
        // 插件未加载 (agent 侧未装配/已卸载)
        textItem("|- wait for load", "hint");
        return fmt::format(R"({{"items":[{}]}})", fmt::join(items, ","));
    }

    // 进度判定 (与迁移前 TUI 渲染逻辑一致):
    // - total>0 且 processed>=total: 索引正常结束
    // - processed==0 且 total==0: 无文件可索引, 同样视为结束
    // - 其余视为进行中 (流式遍历 processed>0,total=0 / resolve 阶段)
    const bool indexing = g_total > 0 ? !(g_processed >= g_total) : (g_processed > 0);
    if (g_has_progress && indexing) {
        if (g_total > 0) {
            // 索引进行中: 45% (12/60)
            const double pct = static_cast<double>(g_processed) / static_cast<double>(g_total);
            textItem(fmt::format("|- indexing {:.0f}% ({}/{})", pct * 100.0, g_processed, g_total));
            items.push_back(fmt::format(R"({{"kind":"progress","value":{:.3f}}})", pct));
        } else {
            // 流式遍历/收集阶段 (文件总数未知): 显示已发现文件数
            textItem(fmt::format("|- Indexing {} files", g_processed));
        }
        if (!g_current_file.empty()) {
            // 仅显示文件名 (目录路径过长; 无 lib 工具, 简单按分隔符取尾段)
            std::string fname = g_current_file;
            const auto  pos   = fname.find_last_of("/\\");
            if (pos != std::string::npos) {
                fname = fname.substr(pos + 1);
            }
            textItem(fmt::format("|  {}", fname), "hint");
        }
    } else if (g_has_progress && g_total > 0) {
        // 索引完成
        textItem(fmt::format("|- available · {}", g_total));
    } else {
        // 已加载但尚未开始索引
        textItem("|- wait for index", "hint");
    }
    return fmt::format(R"({{"items":[{}]}})", fmt::join(items, ","));
}

/// 用最新缓存刷新 Info 段落 (client io 线程调用)
static void refreshSection() {
    if (!g_client_host || !g_section || !g_client_ui || !g_client_ui->update_info_section) {
        return;
    }
    const std::string json = buildInfoItemsJson();
    g_client_ui->update_info_section(
        g_client_host,
        g_section,
        agentxx_plugin_sv(json.data(), json.size())
    );
}

/// PLUGIN_DATA 事件: 过滤 agentxx_codegraph 的 status/progress 事件
static void on_client_plugin_data(AgentxxPluginStringView payload_json, void* ud) {
    (void)ud;
    if (!g_client_host || !g_section) {
        return;
    }
    // payload: {"plugin","event","data"}
    char* plugin
        = g_client_if->json->json_get_string(g_client_host, payload_json, AGENTXX_SV("plugin"));
    char* event
        = g_client_if->json->json_get_string(g_client_host, payload_json, AGENTXX_SV("event"));
    char* data
        = g_client_if->json->json_get_string(g_client_host, payload_json, AGENTXX_SV("data"));
    if (plugin && event && data && std::strcmp(plugin, "agentxx_codegraph") == 0) {
        SimpleJson j(std::string{data});
        if (std::strcmp(event, "status") == 0) {
            // 加载状态: {"loaded":bool}; loaded=false 时清空进度缓存
            bool loaded = false;
            if (j.ok() && jsonGetBool(j.doc().at_pointer("/loaded"), loaded)) {
                g_loaded = loaded;
                if (!loaded) {
                    g_has_progress = false;
                    g_processed    = 0;
                    g_total        = 0;
                    g_current_file.clear();
                }
                refreshSection();
            }
        } else if (std::strcmp(event, "progress") == 0) {
            // 索引进度: {"processed","total","current_file"}
            if (j.ok()) {
                int64_t     processed = 0, total = 0;
                std::string cur;
                jsonGetInt(j.doc().at_pointer("/processed"), processed);
                jsonGetInt(j.doc().at_pointer("/total"), total);
                jsonGetString(j.doc().at_pointer("/current_file"), cur);
                g_processed    = processed;
                g_total        = total;
                g_current_file = std::move(cur);
                g_has_progress = true;
                g_loaded       = true;
                refreshSection();
            }
        }
    }
    if (plugin) {
        g_client_host->vtable->free(plugin);
    }
    if (event) {
        g_client_host->vtable->free(event);
    }
    if (data) {
        g_client_host->vtable->free(data);
    }
}

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxClientPluginInfo* agentxx_client_get_info(void) {
    static const AgentxxClientPluginInfo info{
        AGENTXX_CLIENT_PLUGIN_API_VERSION,
        AGENTXX_SV("agentxx_codegraph"),
        AGENTXX_SV("1.0.0"),
        AGENTXX_SV("CodeGraph index status (sidebar Info section)"),
    };
    return &info;
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_client_entry(const AgentxxClientHost* host, void** plugin_ctx) {
    g_client_host = host;
    (void)plugin_ctx;

    // COM 风格接口表查询 (entry 一次性查询缓存; 进程级静态数据)
    static const agentxx::plugin::ClientIfaces s_if = agentxx::plugin::ClientIfaces::query(host);
    g_client_if = &s_if;
    g_client_ui = s_if.ui;

    // 1. 侧边栏 Info 栏段落 (title "CodeGraph"; 内容由 refreshSection 更新)
    g_section = g_client_ui && g_client_ui->register_info_section
                  ? g_client_ui->register_info_section(
                      host,
                      AGENTXX_SV("agentxx_codegraph.status"),
                      AGENTXX_SV(R"({"title":"CodeGraph"})")
                  )
                  : nullptr;
    // 宿主不支持 Info 段落 (如 CLI) 时成员为 NULL, 插件降级 (不视为失败)
    if (g_section) {
        refreshSection(); // 初始占位 (等待 agent 侧 status 事件)
    }

    // 2. 事件订阅: agent 侧发布的 codegraph 事件 (服务端转发的 WirePluginData)
    if (!s_if.events || !s_if.events->subscribe
        || !s_if.events->subscribe(
            host,
            AGENTXX_CLIENT_EVT_PLUGIN_DATA,
            on_client_plugin_data,
            nullptr
        )) {
        return -1;
    }

    if (s_if.log && s_if.log->log) {
        s_if.log->log(host, 2, AGENTXX_SV("agentxx_codegraph client loaded"));
    }
    return 0;
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_client_unload(void* plugin_ctx) {
    (void)plugin_ctx;
    if (!g_client_host || !g_client_if) {
        return;
    }
    if (g_section && g_client_ui && g_client_ui->unregister_info_section) {
        g_client_ui->unregister_info_section(g_client_host, g_section);
        g_section = nullptr;
    }
    g_current_file.clear();
    if (g_client_if->log && g_client_if->log->log) {
        g_client_if->log->log(
            g_client_host,
            2,
            AGENTXX_SV("agentxx_codegraph client unloaded")
        );
    }
    g_client_host = nullptr;
    g_client_if   = nullptr;
    g_client_ui   = nullptr;
}
