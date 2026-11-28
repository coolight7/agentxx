/// agentxx_codegraph —— CodeGraph 代码分析插件
#include "agentxx/plugin/api/client_plugin_api.h"
#include "codegraph_manager.h"
#include "codegraph_plugin.h"
#include "fmt/format.h"
#include "fmt/ranges.h"
#include "utilxx_base/json.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace agentxx_codegraph_plugin {

using CodeGraphManager = agentxx_codegraph_plugin::CodeGraphManager;

struct HostConfig {
    /// 本插件数据目录 (`{data_dir}/plugins/data/agentxx_codegraph/`; 空 = 不落盘)
    std::string              dataDir;
    std::string              projectRoot;
    std::vector<std::string> loadPaths;
    std::vector<std::string> ignorePaths;
    bool                     useGitignore = true;
    bool                     loadCwd      = true;
};

struct PluginCtx : public agentxx::plugin::PluginBase {
    std::shared_ptr<agentxx_codegraph_plugin::CodeGraphManager> mgr;
    std::string                                                 projectRoot;
};

static asio::awaitable<HostConfig> readHostConfigAsync(PluginCtx& ctx) {
    HostConfig          cfg;
    const PluginxxHost* host = ctx.host;
    if (!host || !ctx.iface.config) {
        co_return cfg;
    }
    // 索引库放在插件自己的数据目录下 (主程序加载时建好; 空 = 不落盘)
    cfg.dataDir = co_await ctx.dataDirAsync();
    // 表成员是异步操作契约: 提交 + co_await (create 跑在插件执行线程, 不能同步等)
    if (ctx.iface.config->get_config) {
        auto r = co_await tableOpAsync(
            ctx,
            [host, &ctx](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                return ctx.iface.config->get_config(host, cb, ud, err);
            }
        );
        const std::string s{r.ok() ? r.text() : std::string_view{}};
        if (!s.empty()) {
            agentxx::plugin::ArgReader args(s);
            cfg.projectRoot = args.value<std::string>("projectRoot", std::string{});
        }
    }
    if (ctx.iface.config->get_plugin_args) {
        auto r = co_await tableOpAsync(
            ctx,
            [host, &ctx](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                return ctx.iface.config->get_plugin_args(host, cb, ud, err);
            }
        );
        const std::string s{r.ok() ? r.text() : std::string_view{}};
        if (!s.empty()) {
            agentxx::plugin::ArgReader args(s);
            cfg.loadPaths    = args.value<std::vector<std::string>>("paths", {});
            cfg.ignorePaths  = args.value<std::vector<std::string>>("ignore_paths", {});
            cfg.useGitignore = args.value<bool>("use_gitignore", cfg.useGitignore);
            cfg.loadCwd      = args.value<bool>("load_cwd", cfg.loadCwd);
        }
    }
    co_return cfg;
}

struct ToolPrompt {
    std::string depict;
    std::string query;
    std::string limit;
    std::string symbol;
    std::string maxDepth;
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
        p.limit = "Maximum number of symbols to return. Default `20`.";
    } else if (name == "agentxx_codegraph_context") {
        p.depict
            = "Get rich context for a code symbol: its definition, callers, callees, and methods "
              "(for classes).\n"
              "Useful for understanding how a function or class is used throughout the codebase.\n"
              "Returns plain multi-line text with \"symbol:\", \"callers (N):\", \"callees "
              "(N):\", \"methods (N):\" and \"edges (N):\" sections.";
        p.symbol = "Symbol name, plain (`myMethod`) or qualified (`MyClass::myMethod`).";
        p.limit
            = "Maximum results per section (callers / callees / methods / edges). Default `10`.";
        p.maxDepth = "Maximum call-graph traversal depth. Default `3`.";
    } else if (name == "agentxx_codegraph_callers") {
        p.depict
            = "Find all functions that call a given symbol (reverse call-graph traversal).\n"
              "Use this to understand what depends on a function before modifying it.\n"
              "Returns plain multi-line text: \"Callers (N):\" followed by one block per symbol.";
        p.symbol   = "Symbol name to find callers for (plain or qualified).";
        p.maxDepth = "Maximum traversal depth. Default `3`.";
    } else if (name == "agentxx_codegraph_callees") {
        p.depict
            = "Find all functions that a given symbol calls (forward call-graph traversal).\n"
              "Use this to understand a function's dependencies.\n"
              "Returns plain multi-line text: \"Callees (N):\" followed by one block per symbol.";
        p.symbol   = "Symbol name to find callees for (plain or qualified).";
        p.maxDepth = "Maximum traversal depth. Default `3`.";
    } else if (name == "agentxx_codegraph_path") {
        p.depict
            = "Find the call-chain path between two symbols in the call graph.\n"
              "Use this to trace how execution flows from one function to another.\n"
              "Returns plain multi-line text: \"Path (N):\" followed by one block per symbol on "
              "the path.";
        p.from     = "Starting symbol name (plain or qualified).";
        p.to       = "Target symbol name (plain or qualified).";
        p.maxDepth = "Maximum search depth. Default `10`.";
    }
    return p;
}

/// 启用时注入的 system prompt 附加提示词 —— 强调符号定义/引用查找优先使用 CodeGraph
constexpr std::string_view kSystemCodegraphPrompt = R"_(
## CodeGraph

You have access to CodeGraph code analysis tools for precise, index-based code navigation. **When you need to locate symbol definitions or find references / call relationships, prefer CodeGraph tools over text search (`agentxx_filesystem_grep`/`glob`)**:

- `agentxx_codegraph_search` — search symbols by name (partial match) to locate definitions quickly
- `agentxx_codegraph_context` — rich context for a symbol: definition, callers, callees, methods and edges
- `agentxx_codegraph_callers` — reverse call-graph: find all functions that call a given symbol
- `agentxx_codegraph_callees` — forward call-graph: find all functions that a given symbol calls
- `agentxx_codegraph_path` — call-chain path between two symbols

Workflow: use `search` to resolve the exact symbol name, then `context`/`callers`/`callees`/`path` as needed. Use these for tasks like "where is X defined", "who calls X", "what does X call", "what depends on X", "is it safe to modify X" before falling back to grep/glob. Results may be incomplete while indexing is in progress (tool output will warn).
)_";

/// 注入系统提示词段 (**协程形态**: 调用即提交, 结果里才知道写没写成)
static asio::awaitable<void>
    injectCodegraphSystemPrompt(PluginCtx& ctx, agentxx::plugin::ScopeRef ref) {
    if (!ctx.host || !ctx.iface.prompt || !ctx.iface.prompt->set_prompt) {
        co_return;
    }
    codegraph::Json j        = codegraph::Json::object();
    codegraph::Json append   = codegraph::Json::object();
    append["codegraph"]      = std::string(kSystemCodegraphPrompt);
    j["appendSystemPrompts"] = append;
    std::string payload      = j.dump();
    auto        r            = co_await agentxx::plugin::setPromptJsonAsync(ctx, ref.ptr(), payload);
    if (!r.ok()) {
        pluginLog(
            ctx.host,
            ctx.iface.log,
            3,
            fmt::format(
                "agentxx_codegraph: set appendSystemPrompts[codegraph] failed: {}",
                r.accepted() ? std::string(r.text()) : r.startError
            )
        );
    } else {
        pluginLog(
            ctx.host,
            ctx.iface.log,
            2,
            "agentxx_codegraph: appendSystemPrompts[codegraph] injected via prompt iface"
        );
    }
    co_return;
}

/// 把五个工具的描述补进主程序提示词表 (已有条目不覆盖)
/// - **协程形态**: 先读当前提示词 JSON, 再写回本实例的贡献
/// - 主程序按所有者记录贡献, 插件卸载/禁用时自动撤销
static asio::awaitable<void>
    ensureToolPromptsInHost(PluginCtx& ctx, agentxx::plugin::ScopeRef ref) {
    if (!ctx.host || !ctx.iface.prompt || !ctx.iface.prompt->get_prompt
        || !ctx.iface.prompt->set_prompt) {
        co_return;
    }
    auto readResult = co_await agentxx::plugin::promptJsonAsync(ctx, ref.ptr());
    if (!readResult.ok() || readResult.text().empty()) {
        co_return;
    }
    std::string s{readResult.text()};
    agentxx::plugin::ArgReader promptArgs(s);
    static const char*         kToolNames[] = {
        "agentxx_codegraph_search",
        "agentxx_codegraph_context",
        "agentxx_codegraph_callers",
        "agentxx_codegraph_callees",
        "agentxx_codegraph_path",
    };
    codegraph::Json patch = codegraph::Json::object();
    codegraph::Json tools = codegraph::Json::object();
    bool            dirty = false;
    // 已有提示词经 ArgReader 原始 Json 判断存在性
    const auto& raw = promptArgs.raw();
    const bool  hasToolPrompt
        = raw.is_object() && raw.contains("toolPrompt") && raw["toolPrompt"].is_object();
    for (const char* name : kToolNames) {
        if (hasToolPrompt && raw["toolPrompt"].contains(name)) {
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
        co_return;
    }
    patch["toolPrompt"] = tools;
    std::string payload = patch.dump();
    auto        wr      = co_await agentxx::plugin::setPromptJsonAsync(ctx, ref.ptr(), payload);
    if (!wr.ok()) {
        pluginLog(
            ctx.host,
            ctx.iface.log,
            3,
            fmt::format(
                "agentxx_codegraph: set_prompt failed: {}",
                wr.accepted() ? std::string(wr.text()) : wr.startError
            )
        );
    }
    co_return;
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

/// 一段时长 -> 人读的"多久以前" (如 `3h 12m ago`)
static std::string humanAge(int64_t ageMs) {
    if (ageMs < 60'000) {
        return "just now";
    }
    const int64_t minutes = ageMs / 60'000;
    if (minutes < 60) {
        return fmt::format("{} min ago", minutes);
    }
    const int64_t hours = minutes / 60;
    if (hours < 24) {
        return minutes % 60 == 0 ? fmt::format("{}h ago", hours)
                                 : fmt::format("{}h {}m ago", hours, minutes % 60);
    }
    return fmt::format("{}d ago", hours / 24);
}

/// 毫秒时间戳 -> 本地时间文本 (`YYYY-MM-DD HH:MM`; 取不到时返回空串)
static std::string formatLocalTimeMs(int64_t ms) {
    if (ms <= 0) {
        return {};
    }
    const std::time_t t = static_cast<std::time_t>(ms / 1000);
    std::tm           tmv{};
#if XX_IS_WIN_D
    if (::localtime_s(&tmv, &t) != 0) {
        return {};
    }
#else
    if (::localtime_r(&t, &tmv) == nullptr) {
        return {};
    }
#endif
    return fmt::format(
        "{:04d}-{:02d}-{:02d} {:02d}:{:02d}",
        tmv.tm_year + 1900,
        tmv.tm_mon + 1,
        tmv.tm_mday,
        tmv.tm_hour,
        tmv.tm_min
    );
}

/// 索引信息尾注: 让调用方知道这批结果基于什么时间的索引
/// - 索引为空时额外提示"可能索引旧了", 避免把"索引没扫到"当成"代码里没有"
/// - [empty] 本次结果是否为空
static std::string indexFooter(CodeGraphManager* m, bool empty) {
    if (!m) {
        return {};
    }
    const auto info = m->getIndexInfo();
    if (false == info.available) {
        return info.indexing
                   ? std::string{"\nwarning: CodeGraph is still indexing, results may be "
                                 "incomplete"}
                   : std::string{"\n[index] no index yet: run `agentxx_codegraph_index` first"};
    }

    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch()
    )
                           .count();
    std::string out = fmt::format("\n[index] {} file(s) / {} symbol(s)", info.files, info.symbols);
    if (info.updatedAtMs > 0) {
        const auto built = formatLocalTimeMs(info.updatedAtMs);
        const auto age   = humanAge(std::max<int64_t>(0, nowMs - info.updatedAtMs));
        out += built.empty() ? fmt::format(", built {}", age)
                             : fmt::format(", built {} ({})", built, age);
    }
    if (empty) {
        out += "\nno match. If you expected one, the index may be stale (see the build time "
               "above): rebuild it with `agentxx_codegraph_index`, or check the spelling of the "
               "name.";
    }
    if (info.indexing) {
        out += "\nwarning: CodeGraph is still indexing, results may be incomplete";
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
    if (out.empty()) {
        out = "(empty context)";
    }
    out += indexFooter(m, false);
    return out;
}

static std::string
    impactToText(std::string_view title, const codegraph::Json& impact, CodeGraphManager* m) {
    std::string out;
    const bool  empty = !(impact.contains("nodes") && impact["nodes"].is_array()
                         && impact["nodes"].size() > 0);
    if (impact.contains("nodes") && impact["nodes"].is_array()) {
        out += jsonNodesToText(title, impact["nodes"]);
    } else {
        out += fmt::format("{} (0):\n  (none)", title);
    }
    if (impact.contains("edges") && impact["edges"].is_array() && impact["edges"].size() > 0) {
        out += fmt::format("\n{}", jsonEdgesToText(impact["edges"]));
    }
    out += indexFooter(m, empty);
    return out;
}

struct SchemaBuilder {
    codegraph::Json properties = codegraph::Json::object();

    /// 字符串参数 (无默认值)
    void str(std::string_view key, std::string_view desc) {
        properties[std::string(key)] = codegraph::Json{
            {"type",        "string"         },
            {"description", std::string(desc)}
        };
    }

    /// 整数参数 (带默认值)
    void integer(std::string_view key, std::string_view desc, int64_t defVal) {
        properties[std::string(key)] = codegraph::Json{
            {"type",        "integer"        },
            {"description", std::string(desc)},
            {"default",     defVal           }
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

/// 注册五个查询工具 (**协程形态**)
/// - 每个注册都是“提交 + `co_await`”: 提交就已经生效, 等结果只为了拿到失败原因
static asio::awaitable<void>
    registerAllTools(PluginCtx& ctx, agentxx::plugin::ScopeRef ref) {
    constexpr int kAutoSummary = AGENTXX_PLUGIN_TOOL_FLAG_AUTO_SUMMARY;

    // agentxx_codegraph_search
    {
        auto          p      = co_await toolPromptOfAsync(ctx, "agentxx_codegraph_search");
        const auto    def    = defaultToolPrompt("agentxx_codegraph_search");
        std::string   depict = p.depict.empty() ? def.depict : p.depict;
        SchemaBuilder b;
        b.str("query", p.args.contains("query") ? p.args["query"] : def.query);
        b.integer("limit", p.args.contains("limit") ? p.args["limit"] : def.limit, 20);
        co_await toolAsync(
            ctx,
            ref,
            "agentxx_codegraph_search",
            depict,
            b.dump({"query"}),
            agentxx::plugin::onWorkerAsync([](PluginCtx& c, std::string_view args_json) -> std::string {
                agentxx::plugin::ArgReader args(args_json);
                std::string                query = args.value<std::string>("query", std::string{});
                if (query.empty()) {
                    throw std::invalid_argument{"Arg `query` is empty"};
                }
                int64_t limit64 = args.value<int64_t>("limit", 20);
                auto    r       = c.mgr->searchSymbols(query, static_cast<int>(limit64));
                if (!r.success) {
                    throw std::runtime_error{
                        r.error.empty() ? "CodeGraph request failed" : r.error
                    };
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
                out += indexFooter(c.mgr.get(), r.nodes.empty());
                return out;
            }),
            0,
            kAutoSummary
        );
    }

    // agentxx_codegraph_context
    {
        auto          p      = co_await toolPromptOfAsync(ctx, "agentxx_codegraph_context");
        const auto    def    = defaultToolPrompt("agentxx_codegraph_context");
        std::string   depict = p.depict.empty() ? def.depict : p.depict;
        SchemaBuilder b;
        b.str("symbol", p.args.contains("symbol") ? p.args["symbol"] : def.symbol);
        b.integer("limit", p.args.contains("limit") ? p.args["limit"] : def.limit, 10);
        b.integer(
            "max_depth",
            p.args.contains("max_depth") ? p.args["max_depth"] : def.maxDepth,
            3
        );
        co_await toolAsync(
            ctx,
            ref,
            "agentxx_codegraph_context",
            depict,
            b.dump({"symbol"}),
            agentxx::plugin::onWorkerAsync([](PluginCtx& c, std::string_view args_json) -> std::string {
                agentxx::plugin::ArgReader args(args_json);
                std::string symbol = args.value<std::string>("symbol", std::string{});
                if (symbol.empty()) {
                    throw std::invalid_argument{"Arg `symbol` is empty"};
                }
                int64_t limit64 = args.value<int64_t>("limit", 10);
                int64_t depth64 = args.value<int64_t>("max_depth", 3);
                auto    r       = c.mgr->getSymbolContext(
                    symbol,
                    static_cast<int>(limit64),
                    static_cast<int>(depth64)
                );
                if (!r.success) {
                    throw std::runtime_error{
                        r.error.empty() ? "CodeGraph request failed" : r.error
                    };
                }
                return contextToText(r.context, c.mgr.get());
            }),
            0,
            kAutoSummary
        );
    }

    // agentxx_codegraph_callers
    {
        auto          p      = co_await toolPromptOfAsync(ctx, "agentxx_codegraph_callers");
        const auto    def    = defaultToolPrompt("agentxx_codegraph_callers");
        std::string   depict = p.depict.empty() ? def.depict : p.depict;
        SchemaBuilder b;
        b.str("symbol", p.args.contains("symbol") ? p.args["symbol"] : def.symbol);
        b.integer(
            "max_depth",
            p.args.contains("max_depth") ? p.args["max_depth"] : def.maxDepth,
            3
        );
        co_await toolAsync(
            ctx,
            ref,
            "agentxx_codegraph_callers",
            depict,
            b.dump({"symbol"}),
            agentxx::plugin::onWorkerAsync([](PluginCtx& c, std::string_view args_json) -> std::string {
                agentxx::plugin::ArgReader args(args_json);
                std::string symbol = args.value<std::string>("symbol", std::string{});
                if (symbol.empty()) {
                    throw std::invalid_argument{"Arg `symbol` is empty"};
                }
                int64_t depth64 = args.value<int64_t>("max_depth", 3);
                auto    r       = c.mgr->getCallers(symbol, static_cast<int>(depth64));
                if (!r.success) {
                    throw std::runtime_error{
                        r.error.empty() ? "CodeGraph request failed" : r.error
                    };
                }
                return impactToText("Callers", r.impact, c.mgr.get());
            }),
            0,
            kAutoSummary
        );
    }

    // agentxx_codegraph_callees
    {
        auto          p      = co_await toolPromptOfAsync(ctx, "agentxx_codegraph_callees");
        const auto    def    = defaultToolPrompt("agentxx_codegraph_callees");
        std::string   depict = p.depict.empty() ? def.depict : p.depict;
        SchemaBuilder b;
        b.str("symbol", p.args.contains("symbol") ? p.args["symbol"] : def.symbol);
        b.integer(
            "max_depth",
            p.args.contains("max_depth") ? p.args["max_depth"] : def.maxDepth,
            3
        );
        co_await toolAsync(
            ctx,
            ref,
            "agentxx_codegraph_callees",
            depict,
            b.dump({"symbol"}),
            agentxx::plugin::onWorkerAsync([](PluginCtx& c, std::string_view args_json) -> std::string {
                agentxx::plugin::ArgReader args(args_json);
                std::string symbol = args.value<std::string>("symbol", std::string{});
                if (symbol.empty()) {
                    throw std::invalid_argument{"Arg `symbol` is empty"};
                }
                int64_t depth64 = args.value<int64_t>("max_depth", 3);
                auto    r       = c.mgr->getCallees(symbol, static_cast<int>(depth64));
                if (!r.success) {
                    throw std::runtime_error{
                        r.error.empty() ? "CodeGraph request failed" : r.error
                    };
                }
                return impactToText("Callees", r.impact, c.mgr.get());
            }),
            0,
            kAutoSummary
        );
    }

    // agentxx_codegraph_path
    {
        auto          p      = co_await toolPromptOfAsync(ctx, "agentxx_codegraph_path");
        const auto    def    = defaultToolPrompt("agentxx_codegraph_path");
        std::string   depict = p.depict.empty() ? def.depict : p.depict;
        SchemaBuilder b;
        b.str("from", p.args.contains("from") ? p.args["from"] : def.from);
        b.str("to", p.args.contains("to") ? p.args["to"] : def.to);
        b.integer(
            "max_depth",
            p.args.contains("max_depth") ? p.args["max_depth"] : def.maxDepth,
            10
        );
        co_await toolAsync(
            ctx,
            ref,
            "agentxx_codegraph_path",
            depict,
            b.dump({"from", "to"}),
            agentxx::plugin::onWorkerAsync([](PluginCtx& c, std::string_view args_json) -> std::string {
                agentxx::plugin::ArgReader args(args_json);
                std::string                from = args.value<std::string>("from", std::string{});
                std::string                to   = args.value<std::string>("to", std::string{});
                if (from.empty() || to.empty()) {
                    throw std::invalid_argument{"Args `from` and `to` are both required"};
                }
                int64_t depth64 = args.value<int64_t>("max_depth", 10);
                auto    r       = c.mgr->findPath(from, to, static_cast<int>(depth64));
                if (!r.success) {
                    throw std::runtime_error{
                        r.error.empty() ? "CodeGraph request failed" : r.error
                    };
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
                out += indexFooter(c.mgr.get(), r.path.empty());
                return out;
            }),
            0,
            kAutoSummary
        );
    }
    co_return;
}

static void
    snapshotQueryDone(void* ud, int32_t status, void* result, const PluginxxStringView* error) {
    (void)error;
    auto* ctx   = static_cast<PluginCtx*>(ud);
    auto* files = static_cast<int64_t*>(result);
    agentxx::plugin::guardCallVoid(
        [ctx](const char* m) noexcept {
            pluginLog(ctx ? ctx->host : nullptr, ctx ? ctx->iface.log : nullptr, 4, m ? m : "");
        },
        [&] {
            if (status == PLUGINXX_OPERATOR_OK && ctx && ctx->host && ctx->iface.broadcasts
                && ctx->iface.broadcasts->publish_broadcast) {
                codegraph::Json j = codegraph::Json::object();
                j["loaded"]       = true;
                if (!ctx->projectRoot.empty()) {
                    j["project_root"] = ctx->projectRoot;
                }
                std::string payload = j.dump();
                auto        statusTopic
                    = agentxx::plugin::PluginStringView::fromCstr("plugin.agentxx_codegraph.status");
                auto payloadSv
                    = agentxx::plugin::PluginStringView::from(payload.data(), payload.size());
                agentxx::plugin::publishPluginBroadcastNoWait(ctx->host, ctx->iface.broadcasts, statusTopic, payloadSv);
                if (files && *files > 0) {
                    codegraph::Json p = codegraph::Json::object();
                    p["processed"]    = *files;
                    p["total"]        = *files;
                    p["current_file"] = "";
                    std::string pp    = p.dump();
                    auto        progTopic
                        = agentxx::plugin::PluginStringView::fromCstr("plugin.agentxx_codegraph.progress");
                    auto ppSv = agentxx::plugin::PluginStringView::from(pp.data(), pp.size());
                    agentxx::plugin::publishPluginBroadcastNoWait(ctx->host, ctx->iface.broadcasts, progTopic, ppSv);
                }
            }
        }
    );
    if (ctx && ctx->host && ctx->host->vtable && ctx->host->vtable->free) {
        if (files) {
            ctx->host->vtable->free(files);
        }
    }
}

static void* snapshotQueryWork(void* ud, const PluginxxCancelToken*, PluginxxString*) {
    auto* ctx = static_cast<PluginCtx*>(ud);
    return agentxx::plugin::guardCall(
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

static void PLUGINXX_CALL on_client_attached(const PluginxxStringView*, void* ud) {
    auto* ctx = static_cast<PluginCtx*>(ud);
    agentxx::plugin::guardCallVoid(
        [ctx](const char* m) noexcept {
            pluginLog(ctx ? ctx->host : nullptr, ctx ? ctx->iface.log : nullptr, 4, m ? m : "");
        },
        [&] {
            if (!ctx || !ctx->host || !ctx->iface.scheduler || !ctx->iface.scheduler->offload) {
                return;
            }
            ctx->iface.scheduler
                ->offload(ctx->host, snapshotQueryWork, snapshotQueryDone, ctx, nullptr);
        }
    );
}

} // namespace agentxx_codegraph_plugin

using namespace agentxx_codegraph_plugin;

/// 实例初始化 (协程): 打开索引库并初始化 (可降级: 无插件数据目录 / 初始化失败时保持 mgr 为空)
///
/// 接口存在性校验放在 createCtxEntry 的 check 里 (同步、不碰表), 这里只做需要
/// co_await 的配置读取与本地初始化。
static asio::awaitable<std::string> codegraphInit(PluginCtx& ctx) {
    auto cfg = co_await readHostConfigAsync(ctx);
    if (cfg.dataDir.empty()) {
        pluginLog(
            ctx.host,
            ctx.iface.log,
            3,
            "agentxx_codegraph: plugin data dir not available, skip tools"
        );
        co_return std::string{};
    }

    agentxx_codegraph_plugin::CodeGraphIndexConfig cgConfig;
    cgConfig.loadPaths           = cfg.loadPaths;
    cgConfig.ignorePaths         = cfg.ignorePaths;
    cgConfig.useGitignore        = cfg.useGitignore;
    cgConfig.autoLoadProjectRoot = cfg.loadCwd;

    std::string sqliteDir = (std::filesystem::path(cfg.dataDir) / "sqlite").string();
    ctx.mgr = std::make_shared<agentxx_codegraph_plugin::CodeGraphManager>(sqliteDir, cgConfig);
    ctx.mgr->setLogSink([ctxPtr = &ctx](int level, const std::string& msg) {
        pluginLog(ctxPtr->host, ctxPtr->iface.log, level, msg);
    });

    PluginCtx* ctxRaw = &ctx;
    ctx.mgr->setProgressCallback([ctxRaw](int processed, int total, std::string_view currentFile) {
        try {
            if (!ctxRaw || !ctxRaw->host || !ctxRaw->iface.broadcasts
                || !ctxRaw->iface.broadcasts->publish_broadcast) {
                return;
            }
            codegraph::Json j   = codegraph::Json::object();
            j["processed"]      = processed;
            j["total"]          = total;
            j["current_file"]   = std::string{currentFile};
            std::string payload = j.dump();
            auto        progTopic = agentxx::plugin::PluginStringView::fromCstr(
                "plugin.agentxx_codegraph.progress"
            );
            auto payloadSv
                = agentxx::plugin::PluginStringView::from(payload.data(), payload.size());
            agentxx::plugin::publishPluginBroadcastNoWait(
                ctxRaw->host,
                ctxRaw->iface.broadcasts,
                progTopic,
                payloadSv
            );
        } catch (...) {
        }
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
        pluginLog(ctx.host, ctx.iface.log, 4, "agentxx_codegraph: get current path failed");
        co_return std::string{};
    }

    if (!ctx.mgr->initialize(projectRoot)) {
        pluginLog(ctx.host, ctx.iface.log, 4, "agentxx_codegraph: initialize failed");
        co_return std::string{};
    }
    ctx.projectRoot = projectRoot;
    co_return std::string{};
}

/// 进程级启动工作 (后台索引预热 + 状态事件), 定义在下方
static int codegraphAgentStartSetup(PluginCtx& ctx);

extern "C" PLUGINXX_EXPORT void agentxx_agent_plugin_create(
    const PluginxxHost*           host,
    void**                        plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               err
) {
    agentxx::plugin::detail::createCtxEntry<PluginCtx>(
        host,
        plugin_ctx,
        notify,
        err,
        "agentxx_codegraph",
        [](PluginCtx& ctx) -> std::string {
            if (!ctx.iface.tools || !ctx.iface.tools->register_tool || !ctx.iface.broadcasts) {
                return "agentxx_codegraph: lacks tools/broadcasts iface";
            }
            return {};
        },
        [](PluginCtx& ctx, const PluginxxOperatorNotify* n, PluginxxString*) {
            // 配置读取 / 索引库初始化 / 进程级启动工作都经协程: 完成通知随之异步发出
            auto later = agentxx::plugin::finishLater(n);
            ctx.spawn([later](PluginCtx& c, agentxx::plugin::OpCtl&) -> asio::awaitable<void> {
                try {
                    auto reason = co_await codegraphInit(c);
                    if (!reason.empty()) {
                        later.failed(reason);
                        co_return;
                    }
                    if (codegraphAgentStartSetup(c) != 0) {
                        later.failed("agentxx_codegraph: start setup failed");
                        co_return;
                    }
                    later.ok();
                } catch (const std::exception& e) {
                    later.failed(e.what());
                }
                co_return;
            });
        }
    );
}

/// 挂载事务 (mount 的实际内容): 工具/prompt/事件订阅 (**协程形态**)。
/// - `ref` 为本次挂载的 agent 引用, 提示词/工具/订阅都记在该 agent 名下
/// - 插件数据目录缺失或 initialize 失败时 mgr 为空: 保持降级加载, mount 直接完成
///   (与迁移前 create 的降级行为一致)。
/// - 每条注册都 `co_await` 完再继续: 登记本身已经生效, 等结果只为了拿到失败原因;
///   `co_await` 期间不占线程 (主程序在 IO 线程上是内联完成的, 不会多一次等待)
static asio::awaitable<void>
    codegraphAgentMountSetup(PluginCtx& ctx, agentxx::plugin::ScopeRef ref) {
    if (!ctx.mgr) {
        co_return;
    }
    co_await ensureToolPromptsInHost(ctx, ref);
    co_await injectCodegraphSystemPrompt(ctx, ref);
    co_await registerAllTools(ctx, ref);

    if (ctx.iface.broadcasts) {
        co_await subscribePluginBroadcastAsync(
            ctx,
            ref.ptr(),
            "agentxx.agent.host.client_attached",
            on_client_attached,
            &ctx
        );
    }
    co_return;
}

/// 进程级启动工作: 后台索引预热 + 状态事件。
/// - 索引数据在实例上下文中, 与挂载数无关, 因此只在 start 做一次
/// - 后台 warmup 由 std::thread 改为主程序托管任务 (ctx.spawn): 卸载时可取消,
///   且 offload 期间持有实例 lease, destroy/dlclose 前必然等待索引代码返回。
static int codegraphAgentStartSetup(PluginCtx& ctx) {
    if (!ctx.mgr) {
        return 0;
    }

    ctx.spawn([](PluginCtx& c, agentxx::plugin::OpCtl ctl) -> asio::awaitable<void> {
        co_await sleepAsync(c, 2000);
        if (ctl.canceled()) {
            co_return;
        }
        const auto startedAt = std::chrono::steady_clock::now();
        const bool ok = co_await offloadAsync(c, [&c](const PluginxxCancelToken*) {
            return c.mgr->updateIndex();
        });
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - startedAt
        )
                            .count();
        pluginLog(
            c.host,
            c.iface.log,
            ok ? 2 : 3,
            fmt::format("[codegraph] background warmup index {} ({}ms)", ok ? "done" : "failed", ms)
        );
    });

    if (ctx.iface.broadcasts) {
        codegraph::Json j   = codegraph::Json::object();
        j["loaded"]         = true;
        j["project_root"]   = ctx.projectRoot;
        std::string payload = j.dump();
        auto statusTopic = agentxx::plugin::PluginStringView::fromCstr("plugin.agentxx_codegraph.status");
        auto payloadSv   = agentxx::plugin::PluginStringView::from(payload.data(), payload.size());
        agentxx::plugin::publishPluginBroadcastNoWait(ctx.host, ctx.iface.broadcasts, statusTopic, payloadSv);
    }
    pluginLog(ctx.host, ctx.iface.log, 2, "agentxx_codegraph started (6 tools)");
    return 0;
}

static void* codegraphAgentMount(
    PluginCtx&                         ctx,
    const AgentxxPluginAgentMountSpec* spec,
    const PluginxxOperatorNotify*      notify,
    PluginxxString*                    error
) {
    if (!notify) {
        if (error) {
            agentxx::plugin::PluginString::set(
                ctx.host,
                error,
                "agentxx_codegraph mount: notify required"
            );
        }
        return nullptr;
    }
    if (!ctx.iface.tasks || !ctx.iface.tasks->register_task) {
        if (error) {
            agentxx::plugin::PluginString::set(
                ctx.host,
                error,
                "agentxx_codegraph mount: host has no pluginxx.tasks (spawn unavailable)"
            );
        }
        return nullptr;
    }
    auto later = agentxx::plugin::finishLater(notify);
    // 注册事务交给协程: 工具/prompt/订阅逐条"提交 + co_await", 全部生效后再放行挂载
    const agentxx::plugin::AgentRef agentRef(*spec);
    ctx.spawn([later, agentRef](PluginCtx& c, agentxx::plugin::OpCtl&) -> asio::awaitable<void> {
        try {
            co_await codegraphAgentMountSetup(c, agentRef);
            later.ok();
        } catch (const std::exception& e) {
            later.failed(e.what());
        }
        co_return;
    });
    return later.provider();
}

static void codegraphAgentUnmount(
    PluginCtx&, void*, const PluginxxOperatorNotify* notify, PluginxxString*
) {
    // 无 per-agent 状态; 工具/提示词/订阅由主程序按挂载记录撤销, 索引数据保留
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return;
}

static void codegraphAgentStop(PluginCtx&, const PluginxxOperatorNotify* notify, PluginxxString*) {
    // 注册记录与后台任务句柄由主程序在 stop 后统一撤销/取消; 索引数据保留在
    // 实例上下文中, 下次 start 直接复用。
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return;
}

AGENTXX_AGENT_PLUGIN_LIFECYCLE_EXPORT(
    PluginCtx,
    codegraphAgentStop,
    codegraphAgentMount,
    codegraphAgentUnmount
)

extern "C" PLUGINXX_EXPORT void agentxx_agent_plugin_destroy(
    void*                         plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               err
) {
    agentxx::plugin::detail::destroyCtxEntry<PluginCtx>(
        plugin_ctx,
        notify,
        err,
        [](PluginCtx* ctx) {
            if (!ctx) {
                return;
            }
            pluginLog(ctx->host, ctx->iface.log, 2, "agentxx_codegraph unloaded");
            delete ctx;
        }
    );
}

/// ==================== client 侧入口 —— CodeGraph 索引状态渲染 (Append 段风格, 恢复 1e524e62)
/// ====================

/// client 侧每实例上下文 (多实例契约: 原进程级 static 状态全部移入)
struct ClientCtx : agentxx::plugin::ClientPluginBase {
    const AgentxxClientUiIface*   ui           = nullptr;
    /// Info 段落内容 (声明项 + 内容; 卸载时由主程序撤销)
    std::shared_ptr<agentxx::plugin::Content> section;
    bool                          loaded       = false;
    bool                          has_progress = false;
    int64_t                       processed    = 0;
    int64_t                       total        = 0;
    std::string                   current_file;

    void logErr(const char* m) const noexcept {
        agentxx::plugin::logTo(host, iface.log, 4, "agentxx_codegraph", m ? m : "");
    }
};

static std::string buildInfoDocJson(ClientCtx& c) {
    // 组件项经描述层与扩展 kit 组装 (文本行用 kit::text, 进度用构建器)
    std::vector<pluginxx::ui::Item> items;
    auto                            pushText = [&](const std::string& text, std::string color = {}) {
        utilxx_base::Json params;
        params["text"] = text;
        if (!color.empty()) {
            params["color"] = std::move(color);
        }
        items.push_back(agentxx::ui::kit::text(params));
    };
    if (!c.loaded) {
        pushText("|- wait for load", "hint");
    } else {
        const bool indexing = c.total > 0 ? !(c.processed >= c.total) : (c.processed > 0);
        if (c.has_progress && indexing) {
            if (c.total > 0) {
                const double pct = static_cast<double>(c.processed) / static_cast<double>(c.total);
                pushText(
                    fmt::format("|- indexing {:.0f}% ({}/{})", pct * 100.0, c.processed, c.total)
                );
                items.push_back(pluginxx::ui::build::progress(pct * 100.0, 100.0));
            } else {
                pushText(fmt::format("|- Indexing {} files", c.processed));
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
            pushText(fmt::format("|- available \u00b7 {}", c.total));
        } else {
            pushText("|- wait for index", "hint");
        }
    }
    utilxx_base::Json out;
    out["blocks"] = pluginxx::ui::dumpBlocks(items);
    return out.dump();
}

static void refreshSection(ClientCtx& c) {
    // 内容注册归 client start 事务 (协程里 co_await 注册); 这里只推内容 (提交即返回)
    if (!c.section) {
        return;
    }
    // 文档在这里算 (状态也在这里更新), publish 记住一份, 生成函数 (插件执行线程)
    // 之后回的是那份拷贝
    c.section->publish(buildInfoDocJson(c));
}

/// 取频道载荷 JSON 对象 (非法/空值返回空对象)
static utilxx_base::Json payloadObject(const PluginxxStringView* payload) {
    if (!payload || !payload->data || payload->size == 0) {
        return utilxx_base::Json::object();
    }
    try {
        auto j = utilxx_base::Json::parse(
            std::string{payload->data, static_cast<size_t>(payload->size)}
        );
        return j.is_object() ? j : utilxx_base::Json::object();
    } catch (...) {
        return utilxx_base::Json::object();
    }
}

/// 索引状态频道 (`plugin.agentxx_codegraph.status`)
static void PLUGINXX_CALL onClientStatus(const PluginxxStringView* payload_json, void* ud) {
    auto* ctx = static_cast<ClientCtx*>(ud);
    if (!ctx || !ctx->host) {
        return;
    }
    const auto d    = payloadObject(payload_json);
    bool       loaded = d.value("loaded", false);
    ctx->loaded     = loaded;
    if (!loaded) {
        ctx->has_progress = false;
        ctx->processed    = 0;
        ctx->total        = 0;
        ctx->current_file.clear();
    }
    refreshSection(*ctx);
}

/// 索引进度频道 (`plugin.agentxx_codegraph.progress`)
static void PLUGINXX_CALL onClientProgress(const PluginxxStringView* payload_json, void* ud) {
    auto* ctx = static_cast<ClientCtx*>(ud);
    if (!ctx || !ctx->host) {
        return;
    }
    const auto  d         = payloadObject(payload_json);
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

extern "C" PLUGINXX_EXPORT int32_t agentxx_client_plugin_get_info(PluginxxInfo* out) {
    return agentxx::plugin::detail::fillClientPluginInfo(
        out,
        "agentxx_codegraph",
        "1.0.0",
        "CodeGraph index status (sidebar Info section)"
    );
}

/// client 侧实例构造: 只查接口表 (UI 项由 create 的注册事务负责)
static std::string codegraphClientCtor(ClientCtx& ctx) {
    ctx.ui = agentxx::plugin::queryInterface<AgentxxClientUiIface>(ctx.host, AGENTXX_IFACE_CLIENT_UI);
    return {};
}

/// client 侧注册事务 (定义在下方, 写成协程)
static void codegraphClientStart(
    ClientCtx&                    ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               err
);

extern "C" PLUGINXX_EXPORT void agentxx_client_plugin_create(
    const PluginxxHost*           host,
    void**                        plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               err
) {
    agentxx::plugin::detail::createCtxEntry<ClientCtx>(
        host,
        plugin_ctx,
        notify,
        err,
        "agentxx_codegraph",
        &codegraphClientCtor,
        &codegraphClientStart
    );
}

/// client 侧注册事务 (协程): Info 段落 + 两条跨端数据订阅
static asio::awaitable<void> codegraphClientSetupAsync(ClientCtx& ctx) {
    const PluginxxHost* host = ctx.host;
    const auto          ui   = ctx.ui;
    if (ui) {
        utilxx_base::Json data = utilxx_base::Json::object();
        data["title"]          = "CodeGraph";
        ctx.section            = co_await agentxx::plugin::clientRegisterContentAsync(
            ctx,
            agentxx::plugin::ContentSpec{
                .slot  = "info",
                .id    = "agentxx_codegraph.status",
                .title = "CodeGraph",
                .data  = data,
            }
        );
        if (!ctx.section) {
            throw std::runtime_error(
                "register info section failed (declarative content rejected)"
            );
        }
        // 注册完立刻根据已有状态刷一次 (内部是提交即返回的推送)
        refreshSection(ctx);
    }

    const auto broadcasts = ctx.iface.broadcasts;
    if (!broadcasts || !broadcasts->subscribe_broadcast) {
        throw std::runtime_error("broadcasts table missing");
    }
    struct SubSpec {
        std::string_view                                  id;
        void(PLUGINXX_CALL* handler)(const PluginxxStringView*, void*);
    };
    const SubSpec subs[] = {
        {"plugin.agentxx_codegraph.status",   onClientStatus  },
        {"plugin.agentxx_codegraph.progress", onClientProgress},
    };
    for (const auto& sub : subs) {
        const auto idSv = agentxx::plugin::PluginStringView::from(sub.id.data(), sub.id.size());
        auto res = co_await agentxx::plugin::clientTableOpAsync(
            ctx,
            [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* e) {
                return broadcasts->subscribe_broadcast(
                    host,
                    nullptr,
                    &idSv,
                    sub.handler,
                    &ctx,
                    nullptr,
                    cb,
                    ud,
                    e
                );
            }
        );
        if (!res.ok()) {
            throw std::runtime_error(
                fmt::format("subscribe `{}` failed: {}", sub.id, res.text())
            );
        }
    }

    if (ctx.iface.log && ctx.iface.log->log) {
        auto msgSv = agentxx::plugin::PluginStringView::fromCstr("agentxx_codegraph client loaded");
        ctx.iface.log->log(host, 2, &msgSv);
    }
    co_return;
}

/// client 侧 start: 注册事务写成协程 (提交后不阻塞插件执行线程)
static void codegraphClientStart( 
    ClientCtx&                    ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               err
) {
    if (!notify) {
        agentxx::plugin::PluginString::set(ctx.host, err, "codegraph client start: notify required");
        return;
    }
    auto later = agentxx::plugin::finishLater(notify);
    ctx.spawn([later](ClientCtx& c, pluginxx::OpCtl&) -> asio::awaitable<void> {
        try {
            co_await codegraphClientSetupAsync(c);
        } catch (const std::exception& e) {
            later.failed(e.what());
            co_return;
        }
        later.ok();
        co_return;
    });
    later.provider();
    return;
}

/// client 侧 stop: 放掉内容句柄 (可重复调用)
///
/// 声明项与内容由主程序在 stop 之后统一撤销, 这里不做反注册: 句柄要活到实例析构,
/// 生成函数还在用它。
static void codegraphClientStop(ClientCtx& ctx, const PluginxxOperatorNotify* notify, PluginxxString*) {
    ctx.section.reset();
    ctx.current_file.clear();
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return;
}

AGENTXX_CLIENT_PLUGIN_LIFECYCLE_EXPORT(ClientCtx, codegraphClientStop)

extern "C" PLUGINXX_EXPORT void agentxx_client_plugin_destroy(
    void*                         plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               err
) {
    agentxx::plugin::detail::destroyCtxEntry<ClientCtx>(
        plugin_ctx,
        notify,
        err,
        [](ClientCtx* ctx) {
            if (!ctx) {
                return;
            }
            // destroy 只释放本地状态: UI 项撤销已由 stop 完成
            if (ctx->host && ctx->iface.log && ctx->iface.log->log) {
                auto msgSv = agentxx::plugin::PluginStringView::fromCstr(
                    "agentxx_codegraph client unloaded"
                );
                ctx->iface.log->log(ctx->host, 2, &msgSv);
            }
            delete ctx;
        }
    );
}
