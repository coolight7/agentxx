/// 启动装配快照实现 (计划 ARC-6 / TOOL-12 / CFG-3 / PLG-10)
///
/// 这里只做"把已经存在的装配事实整理成一份 JSON/文本", 不引入新的装配逻辑:
/// - 配置侧快照直接读 AgentConfig;
/// - 运行侧快照读 AgentContext (模型注册表 / 中间件栈 / 工具装配记录 / 插件管理器 /
///   执行图 / 持久化 / 已加载组件)。
///
/// 输出保持稳定顺序 (map 按键排序, 列表按装配顺序), 便于人工对比两次启动的输出。
#include "agentxx/agent/assembly_snapshot.h"

#include "agentxx/agent/context.h"
#include "agentxx/agent/model_registry.h"
#include "agentxx/agent/session_store.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/plugin/tool_registry.h"
#include "agentxx/util/observability.h"
#include "fmt/format.h"
#include "fmt/ranges.h"
#include "utilxx_base/log.h"

#include <algorithm>
#include <cstdint>

namespace agentxx {
namespace agent {

namespace {

/// 权限模式短名 (与 yaml `permission.mode` 取值一致; 仅快照输出使用)
const char* permissionModeKey(PermissionMode mode) noexcept {
    switch (mode) {
        case PermissionMode::Ask:
            return "ask";
        case PermissionMode::AllAsk:
            return "all_ask";
        case PermissionMode::Pass:
            return "pass";
        case PermissionMode::Deny:
            return "deny";
    }
    return "ask";
}

const char* pluginSideKey(PluginSide side) noexcept {
    switch (side) {
        case PluginSide::Auto:
            return "auto";
        case PluginSide::Agent:
            return "agent";
        case PluginSide::Client:
            return "client";
    }
    return "auto";
}

/// 单个模型的快照 (API key 只输出是否已设置, 不输出内容)
utilxx_base::Json modelSnapshotJson(const ModelConfig& mc) {
    utilxx_base::Json j = utilxx_base::Json::object();
    j["name"]                  = mc.name;
    j["type"]                  = mc.type;
    j["model"]                 = mc.modelName;
    j["base_url"]              = mc.baseUrl;
    j["api_path"]              = mc.apiPath;
    j["api_key_set"]           = !mc.apiKey.empty() && mc.apiKey != "EMPTY";
    j["context_max_token"]     = mc.modelContextMaxToken;
    j["max_connections"]       = mc.maxConcurrentConnections;
    j["connect_timeout_s"]     = mc.connectTimeoutSeconds;
    j["read_chunk_timeout_s"]  = mc.readChunkTimeoutSeconds;
    j["send_thinking"]         = mc.sendThinking;
    j["reasoning_summary"]     = mc.requestReasoningSummary;
    j["image_input"]           = mc.imageInput;
    j["audio_input"]           = mc.audioInput;
    j["video_input"]           = mc.videoInput;
    utilxx_base::Json headers  = utilxx_base::Json::array();
    for (const auto& [k, v] : mc.extraHeaders) {
        (void)v; // 头取值可能是凭据, 只输出名称
        headers.push_back(k);
    }
    j["extra_headers"] = std::move(headers);
    utilxx_base::Json extraKeys = utilxx_base::Json::array();
    if (mc.extraConfig.is_object()) {
        for (const auto& [k, v] : mc.extraConfig.items()) {
            (void)v;
            extraKeys.push_back(k);
        }
    }
    j["extra_config_keys"] = std::move(extraKeys);
    return j;
}

/// 装配侧记录一次的插件条目 → 快照条目
void appendPluginEntries(const AgentContext& ctx, utilxx_base::Json& pluginsOut) {
    pluginsOut = utilxx_base::Json::array();
    if (!ctx.pluginManager) {
        return;
    }
    for (const auto& v : ctx.pluginManager->list()) {
        utilxx_base::Json item = utilxx_base::Json::object();
        item["name"]                = v.name;
        item["version"]             = v.version;
        item["path"]                = v.path;
        item["config_path"]         = v.configPath;
        item["enabled"]             = v.enabled;
        item["user_disabled"]       = v.userDisabled;
        item["blocked_by_deps"]     = v.blockedByDependencies;
        item["inflight"]            = v.inflight;
        item["load_ms"]             = v.loadMs;
        item["tools"]               = v.tools;
        item["tool_count"]          = v.tools.size();
        item["hook_count"]          = v.hookCount;
        item["graph_node_count"]    = v.graphNodeCount;
        item["event_sub_count"]     = v.eventSubCount;
        item["capability_count"]    = v.capabilities.size();
        item["capabilities"]        = v.capabilities;
        item["permission_tools"]    = v.permissionToolCount;
        // 统一注册清单合计 (计划 PLG-1): 0 = 该实例当前没有向宿主贡献任何注册
        item["registration_total"]  = v.registrationTotal;
        item["prompt_key_count"]    = v.promptKeyCount;
        item["skill_dir_count"]     = v.skillDirCount;
        item["memory_file_count"]   = v.memoryFileCount;
        item["mcp_namespace_count"] = v.mcpNamespaceCount;
        item["owns_graph_definition"] = v.ownsGraphDefinition;
        item["depends"]             = v.depends;
        item["optional_depends"]    = v.optionalDepends;
        item["require_interfaces"]  = v.requiredInterfaces;
        item["optional_interfaces"] = v.optionalInterfaces;
        pluginsOut.push_back(std::move(item));
    }
}

/// 工具装配记录 → 快照条目
utilxx_base::Json toolRecordJson(const ToolAssemblyRecord& rec) {
    utilxx_base::Json item = utilxx_base::Json::object();
    item["name"]               = rec.name;
    item["source"]             = rec.source;
    item["filtered"]           = rec.filtered;
    if (rec.filtered) {
        item["filter_reason"] = rec.filterReason;
    }
    item["auto_summary"]      = rec.autoSummaryOutput;
    item["delay_load"]        = rec.canDelayLoad;
    item["repeat_check"]      = rec.repeatCallCheck;
    item["parallel_safe"]     = rec.supportsParallel;
    item["max_retry"]         = rec.maxRetry;
    return item;
}

} // namespace

std::string resolveToolSource(const AgentContext& ctx, std::string_view toolName) {
    const std::string name{toolName};
    if (auto it = ctx.toolSourceHints.find(name); it != ctx.toolSourceHints.end()) {
        return it->second;
    }
    // 插件工具 (动态注册表): 按插件实例登记的工具名反查
    if (ctx.pluginManager) {
        for (const auto& v : ctx.pluginManager->list()) {
            if (std::find(v.tools.begin(), v.tools.end(), name) != v.tools.end()) {
                return fmt::format("plugin:{}", v.name);
            }
        }
    }
    // MCP 工具: 名称前缀为 `<命名空间>_`
    if (ctx.agentConfig) {
        for (const auto& [ns, cfg] : ctx.agentConfig->mcpServerUrls) {
            (void)cfg;
            if (name.size() > ns.size() + 1 && name.compare(0, ns.size(), ns) == 0
                && name[ns.size()] == '_') {
                return fmt::format("mcp:{}", ns);
            }
        }
    }
    return "builtin";
}

utilxx_base::Json buildConfigSnapshot(const AgentConfig& config) {
    utilxx_base::Json root = utilxx_base::Json::object();
    root["section"]        = "config";

    utilxx_base::Json paths = utilxx_base::Json::object();
    paths["data_dir"]  = config.dataDir;
    paths["work_dir"]  = config.resolvedWorkDir();
    paths["session_root"] = config.sessionStoreDirectory;
    root["paths"]      = std::move(paths);

    utilxx_base::Json model = utilxx_base::Json::object();
    model["default"]        = modelSnapshotJson(config.model);
    model["current"]        = config.currentModelName;
    utilxx_base::Json sub   = utilxx_base::Json();
    if (config.subagentModel.has_value()) {
        sub = modelSnapshotJson(*config.subagentModel);
    }
    model["subagent"]  = std::move(sub);
    utilxx_base::Json web = utilxx_base::Json();
    if (config.websearchModel.has_value()) {
        web = modelSnapshotJson(*config.websearchModel);
    }
    model["websearch"] = std::move(web);
    utilxx_base::Json available = utilxx_base::Json::object();
    for (const auto& [name, mc] : config.availableModels) {
        available[name] = modelSnapshotJson(mc);
    }
    model["available"] = std::move(available);
    root["model"]      = std::move(model);

    utilxx_base::Json prompt = utilxx_base::Json::object();
    prompt["system_prompt_chars"] = config.prompt.systemPrompt.size();
    utilxx_base::Json sections    = utilxx_base::Json::array();
    for (const auto& key : config.prompt.appendSectionKeys()) {
        sections.push_back(key);
    }
    prompt["append_sections"] = std::move(sections);
    prompt["hash"]            = config.prompt.promptHash();
    root["prompt"]            = std::move(prompt);

    utilxx_base::Json perm = utilxx_base::Json::object();
    perm["mode"]           = permissionModeKey(config.permissionMode);
    perm["allow_paths"]    = config.permissionAllowPaths;
    perm["deny_paths"]     = config.permissionDenyPaths;
    root["permission"]     = std::move(perm);

    utilxx_base::Json features = utilxx_base::Json::object();
    features["session_store"] = config.enableSessionStore;
    features["summarization"] = config.enableSummarization;
    features["subagent"]      = config.enableSubagent;
    features["worktree"]      = config.enableWorktree;
    features["tool_filtering"] = config.enableToolFiltering;
    features["tool_whitelist"] = config.toolWhitelist;
    features["repair_messages"] = config.repairMessages;
    root["features"]           = std::move(features);

    utilxx_base::Json limits = utilxx_base::Json::object();
    limits["llm_max_retry"]           = config.llmMaxRetry;
    limits["tool_summary_limit"]      = config.toolcallSummaryLimitOutputLength;
    limits["tool_repeat_threshold"]   = config.toolcallRepeatCheckThreshold;
    limits["tool_parallel_max"]       = config.toolParallelMaxConcurrency;
    root["limits"]                    = std::move(limits);

    utilxx_base::Json resources = utilxx_base::Json::object();
    resources["skill_dirs"]     = config.skillDirPaths;
    resources["memory_files"]   = config.memoryFilePaths;
    resources["rag_docs"]       = config.ragDocsPaths;
    utilxx_base::Json mcp       = utilxx_base::Json::object();
    for (const auto& [ns, mc] : config.mcpServerUrls) {
        mcp[ns] = mc.url;
    }
    resources["mcp_servers"] = std::move(mcp);
    root["resources"]        = std::move(resources);

    utilxx_base::Json plugins = utilxx_base::Json::array();
    for (const auto& pc : config.plugins) {
        utilxx_base::Json item = utilxx_base::Json::object();
        item["path"]           = pc.path;
        item["enabled"]        = pc.enabled;
        item["sides"]          = pluginSideKey(pc.sides);
        item["config"]         = pc.configPath;
        item["args_keys"]      = utilxx_base::Json::array();
        if (pc.args.is_object()) {
            for (const auto& [k, v] : pc.args.items()) {
                (void)v;
                item["args_keys"].push_back(k);
            }
        }
        plugins.push_back(std::move(item));
    }
    // 键名与运行侧区分: 这里只是"配置里声明了哪些插件", 装载结果在运行侧快照的
    // `plugins` (含启用状态 / 装载耗时 / 注册计数)
    root["plugin_declarations"] = std::move(plugins);
    root["language"] = config.language;
    root["language_explicit"] = config.languageExplicit;
    root["agent_name"] = config.agentName;
    return root;
}

utilxx_base::Json buildRuntimeSnapshot(const AgentContext& ctx) {
    utilxx_base::Json root = utilxx_base::Json::object();
    root["section"]        = "assembly";

    // ---- 模型注册表 ----
    utilxx_base::Json models = utilxx_base::Json::object();
    if (ctx.modelRegistry) {
        models["count"]   = ctx.modelRegistry->size();
        models["default"] = ctx.modelRegistry->getDefaultModelName();
        models["names"]   = ctx.modelRegistry->listModelNames();
    } else {
        models["count"]   = 0;
        models["default"] = "";
        models["names"]   = utilxx_base::Json::array();
    }
    root["models"] = std::move(models);

    // ---- 中间件顺序 (执行顺序 = 数组顺序) ----
    utilxx_base::Json middlewares = utilxx_base::Json::array();
    if (ctx.middlewareHandleContext) {
        for (const auto& handle : ctx.middlewareHandleContext->handles) {
            if (!handle) {
                continue;
            }
            utilxx_base::Json item = utilxx_base::Json::object();
            item["name"]           = handle->name;
            item["disabled"]       = handle->disabled;
            item["sessions"]       = handle->states.size();
            middlewares.push_back(std::move(item));
        }
    }
    root["middlewares"] = std::move(middlewares);

    // ---- 工具清单 (含被白名单过滤掉的工具与来源) ----
    utilxx_base::Json tools = utilxx_base::Json::array();
    for (const auto& rec : ctx.toolAssembly) {
        tools.push_back(toolRecordJson(rec));
    }
    utilxx_base::Json dynamicTools = utilxx_base::Json::array();
    if (ctx.toolRegistry) {
        for (const auto& name : ctx.toolRegistry->names()) {
            utilxx_base::Json item = utilxx_base::Json::object();
            item["name"]           = name;
            item["source"]         = resolveToolSource(ctx, name);
            item["dynamic"]        = true;
            dynamicTools.push_back(std::move(item));
        }
    }
    utilxx_base::Json toolSection = utilxx_base::Json::object();
    toolSection["assembled"]      = std::move(tools);
    toolSection["dynamic"]        = std::move(dynamicTools);
    toolSection["assembled_count"] = ctx.toolAssembly.size();
    toolSection["enabled_count"]   = ctx.toolNames.size();
    toolSection["dynamic_count"]   = ctx.toolRegistry ? ctx.toolRegistry->size() : 0;
    root["tools"]                  = std::move(toolSection);

    // ---- 插件 ----
    utilxx_base::Json plugins = utilxx_base::Json::array();
    appendPluginEntries(ctx, plugins);
    root["plugins"] = std::move(plugins);

    // ---- 执行图 ----
    utilxx_base::Json graph = utilxx_base::Json::object();
    const auto&        def  = ctx.graphDefinitionJson;
    if (def.is_object()) {
        graph["name"]  = def.contains("name") && def["name"].is_string()
                             ? def["name"].get<std::string>()
                             : std::string{};
        // 节点表是"节点名 → 配置"的对象 (图定义方言); 兼容数组形态
        utilxx_base::Json nodeTypes = utilxx_base::Json::array();
        size_t            nodeCount = 0;
        if (def.contains("nodes")) {
            const auto& nodes = def["nodes"];
            if (nodes.is_object()) {
                nodeCount = nodes.size();
                for (const auto& [key, value] : nodes.items()) {
                    (void)key;
                    if (value.is_object() && value.contains("type") && value["type"].is_string()) {
                        nodeTypes.push_back(value["type"].get<std::string>());
                    }
                }
            } else if (nodes.is_array()) {
                nodeCount = nodes.size();
                for (const auto& node : nodes) {
                    if (node.is_object() && node.contains("type") && node["type"].is_string()) {
                        nodeTypes.push_back(node["type"].get<std::string>());
                    }
                }
            }
        }
        graph["nodes"]      = nodeCount;
        graph["node_types"] = std::move(nodeTypes);
        graph["edges"]      = def.contains("edges") && def["edges"].is_array()
                                  ? def["edges"].size()
                                  : 0;
    } else {
        graph["name"] = "";
    }
    // 执行图定义的独占占用者 (计划 PLG-4): 非空表示当前图定义由某插件覆盖
    graph["definition_owner"]
        = ctx.pluginManager ? ctx.pluginManager->graphDefinitionOwner() : std::string{};
    root["graph"] = std::move(graph);

    // ---- 持久化 ----
    utilxx_base::Json persist = utilxx_base::Json::object();
    const bool        hasStore = ctx.sessions && ctx.sessions->sessionStore;
    persist["enabled"]        = hasStore;
    persist["root"]           = hasStore ? ctx.sessions->sessionStore->rootDir() : std::string{};
    persist["schema_version"] = SessionStore::schemaVersion();
    if (hasStore) {
        persist["writer_lease"] = ctx.sessions->sessionStore->writerLeaseEnabled();
    }
    root["persistence"] = std::move(persist);

    // ---- 已加载组件 (skill / memory / mcp) 与加载失败项 ----
    utilxx_base::Json components  = utilxx_base::Json::object();
    components["skills"]          = ctx.appendComponentInfo.skills;
    components["memory_files"]    = ctx.appendComponentInfo.memoryFiles;
    components["mcp_tools"]       = ctx.appendComponentInfo.mcpTools;
    utilxx_base::Json failures    = utilxx_base::Json::array();
    for (const auto& fc : ctx.appendComponentInfo.failedComponents) {
        utilxx_base::Json item = utilxx_base::Json::object();
        item["name"]           = fc.name;
        item["error"]          = fc.errorMessage;
        item["type"]           = static_cast<int>(fc.type);
        failures.push_back(std::move(item));
    }
    components["failed"] = std::move(failures);
    root["components"]   = std::move(components);

    // ---- 关键指标 (计划 OBS-3): 计数与耗时, 不含任何内容 ----
    if (ctx.metrics) {
        root["metrics"] = ctx.metrics->toJson();
    }

    return root;
}

utilxx_base::Json mergeAssemblySnapshot(
    const utilxx_base::Json& configSnapshot,
    const utilxx_base::Json& runtimeSnapshot
) {
    utilxx_base::Json out = utilxx_base::Json::object();
    if (configSnapshot.is_object()) {
        for (const auto& [k, v] : configSnapshot.items()) {
            out[k] = v;
        }
    }
    if (runtimeSnapshot.is_object()) {
        for (const auto& [k, v] : runtimeSnapshot.items()) {
            out[k] = v;
        }
    }
    return out;
}

std::vector<std::string> renderAssemblySnapshot(const utilxx_base::Json& snapshot) {
    std::vector<std::string> lines;
    auto                     add = [&lines](std::string text) { lines.push_back(std::move(text)); };

    // ---- 配置侧 ----
    if (snapshot.contains("model") && snapshot["model"].is_object()) {
        const auto& model = snapshot["model"];
        if (model.contains("default")) {
            add(fmt::format(
                "model.default: {} ({})",
                model["default"].value("model", std::string{}),
                model["default"].value("base_url", std::string{})
            ));
        }
        if (model.contains("available") && model["available"].is_object()) {
            std::vector<std::string> names;
            for (const auto& [k, v] : model["available"].items()) {
                (void)v;
                names.push_back(k);
            }
            add(fmt::format("model.available: {} [{}]", names.size(), fmt::join(names, ", ")));
        }
    }
    if (snapshot.contains("paths") && snapshot["paths"].is_object()) {
        const auto& paths = snapshot["paths"];
        add(fmt::format("paths.data_dir: {}", paths.value("data_dir", std::string{})));
        add(fmt::format("paths.work_dir: {}", paths.value("work_dir", std::string{})));
        add(fmt::format("paths.session_root: {}", paths.value("session_root", std::string{})));
    }
    if (snapshot.contains("permission") && snapshot["permission"].is_object()) {
        const auto& perm = snapshot["permission"];
        add(fmt::format(
            "permission.mode: {} (allow {}, deny {})",
            perm.value("mode", std::string{}),
            perm.contains("allow_paths") ? perm["allow_paths"].size() : 0,
            perm.contains("deny_paths") ? perm["deny_paths"].size() : 0
        ));
    }
    if (snapshot.contains("language")) {
        add(fmt::format(
            "language: {}{}",
            snapshot.value("language", std::string{}),
            snapshot.value("language_explicit", false) ? " (from config)" : " (client/default)"
        ));
    }
    if (snapshot.contains("plugin_declarations") && snapshot["plugin_declarations"].is_array()
        && !snapshot["plugin_declarations"].empty()) {
        add(fmt::format("plugin declarations[{}]:", snapshot["plugin_declarations"].size()));
        for (const auto& p : snapshot["plugin_declarations"]) {
            std::vector<std::string> argv;
            for (const auto& key : p["args_keys"]) {
                argv.push_back(key.get<std::string>());
            }
            add(fmt::format(
                "  - {} [{}] sides={}{}",
                p.value("path", std::string{}),
                p.value("enabled", false) ? "enabled" : "disabled",
                p.value("sides", std::string{}),
                argv.empty() ? std::string{} : fmt::format(" args=[{}]", fmt::join(argv, ", "))
            ));
        }
    }

    // ---- 运行侧 ----
    if (snapshot.contains("models") && snapshot["models"].is_object()) {
        const auto& models = snapshot["models"];
        add(fmt::format(
            "models: default={} count={}",
            models.value("default", std::string{}),
            models.value("count", 0)
        ));
    }
    if (snapshot.contains("middlewares") && snapshot["middlewares"].is_array()) {
        std::vector<std::string> names;
        for (const auto& item : snapshot["middlewares"]) {
            names.push_back(
                fmt::format("{}{}", item.value("name", std::string{}), item.value("disabled", false) ? "(off)" : "")
            );
        }
        add(fmt::format("middlewares[{}]: {}", names.size(), fmt::join(names, " -> ")));
    }
    if (snapshot.contains("tools") && snapshot["tools"].is_object()) {
        const auto& tools = snapshot["tools"];
        add(fmt::format(
            "tools: assembled={} enabled={} dynamic={}",
            tools.value("assembled_count", 0),
            tools.value("enabled_count", 0),
            tools.value("dynamic_count", 0)
        ));
        if (tools.contains("assembled") && tools["assembled"].is_array()) {
            for (const auto& item : tools["assembled"]) {
                if (item.value("filtered", false)) {
                    add(fmt::format(
                        "  - {} [{}] FILTERED: {}",
                        item.value("name", std::string{}),
                        item.value("source", std::string{}),
                        item.value("filter_reason", std::string{})
                    ));
                } else {
                    add(fmt::format(
                        "  - {} [{}]{}",
                        item.value("name", std::string{}),
                        item.value("source", std::string{}),
                        item.value("parallel_safe", false) ? " parallel" : ""
                    ));
                }
            }
        }
        if (tools.contains("dynamic") && tools["dynamic"].is_array()) {
            for (const auto& item : tools["dynamic"]) {
                add(fmt::format(
                    "  - {} [{}] dynamic",
                    item.value("name", std::string{}),
                    item.value("source", std::string{})
                ));
            }
        }
    }
    if (snapshot.contains("plugins") && snapshot["plugins"].is_array()) {
        add(fmt::format("plugins[{}]:", snapshot["plugins"].size()));
        for (const auto& p : snapshot["plugins"]) {
            std::string state = p.value("enabled", false) ? "enabled" : "disabled";
            if (p.value("user_disabled", false)) {
                state += "/user";
            }
            if (p.value("blocked_by_deps", false)) {
                state += "/blocked-by-deps";
            }
            add(fmt::format(
                "  - {} v{} [{}] load={}ms tools={} hooks={} graphNodes={} events={} caps={} permTools={}",
                p.value("name", std::string{}),
                p.value("version", std::string{}),
                state,
                p.value("load_ms", 0),
                p.value("tool_count", 0),
                p.value("hook_count", 0),
                p.value("graph_node_count", 0),
                p.value("event_sub_count", 0),
                p.value("capability_count", 0),
                p.value("permission_tools", 0)
            ));
        }
    }
    if (snapshot.contains("graph") && snapshot["graph"].is_object()) {
        const auto& graph       = snapshot["graph"];
        const auto  definitionOwner = graph.value("definition_owner", std::string{});
        add(fmt::format(
            "graph: {} nodes={} edges={}{}",
            graph.value("name", std::string{}),
            graph.value("nodes", 0),
            graph.value("edges", 0),
            definitionOwner.empty() ? std::string{}
                                    : fmt::format(" (definition from plugin `{}`)", definitionOwner)
        ));
    }
    if (snapshot.contains("persistence") && snapshot["persistence"].is_object()) {
        const auto& persist = snapshot["persistence"];
        if (persist.value("enabled", false)) {
            add(fmt::format(
                "persistence: root={} schema=v{}{}",
                persist.value("root", std::string{}),
                persist.value("schema_version", 0),
                persist.value("writer_lease", false) ? " writer-lease" : " (no writer lease)"
            ));
        } else {
            add("persistence: OFF (in-memory only, history is lost on exit)");
        }
    }
    if (snapshot.contains("metrics") && snapshot["metrics"].is_object()) {
        const auto& m = snapshot["metrics"];
        add(fmt::format(
            "metrics: turns={} (ok={} failed={}) ttft_avg_ms={:.1f} model_calls={} tools={} compactions={}",
            m["turns"].value("total", 0),
            m["turns"].value("completed", 0),
            m["turns"].value("failed", 0),
            m["ttft"].value("avg_ms", 0.0),
            m["model"].value("calls", 0),
            m["tools"].value("total", 0),
            m["compaction"].value("count", 0)
        ));
    }
    if (snapshot.contains("components") && snapshot["components"].is_object()) {
        const auto& comp = snapshot["components"];
        add(fmt::format(
            "components: skills={} memory={} mcp={} failed={}",
            comp.contains("skills") ? comp["skills"].size() : 0,
            comp.contains("memory_files") ? comp["memory_files"].size() : 0,
            comp.contains("mcp_tools") ? comp["mcp_tools"].size() : 0,
            comp.contains("failed") ? comp["failed"].size() : 0
        ));
        if (comp.contains("failed") && comp["failed"].is_array()) {
            for (const auto& item : comp["failed"]) {
                add(fmt::format(
                    "  ! {}: {}",
                    item.value("name", std::string{}),
                    item.value("error", std::string{})
                ));
            }
        }
    }
    return lines;
}

void logAssemblySnapshot(const utilxx_base::Json& snapshot, std::string_view stage) {
    // 一行计数摘要 (Info) + 完整 JSON (Debug): 常规启动不打全量, 排查时按需打开
    std::string summary = "assembly snapshot";
    if (!stage.empty()) {
        summary += fmt::format(" ({})", stage);
    }
    summary += fmt::format(
        ": models={} middlewares={} tools={} plugins={} graph={}",
        snapshot.contains("models") ? snapshot["models"].value("count", 0) : 0,
        snapshot.contains("middlewares") ? snapshot["middlewares"].size() : 0,
        snapshot.contains("tools") ? snapshot["tools"].value("enabled_count", 0) : 0,
        snapshot.contains("plugins") ? snapshot["plugins"].size() : 0,
        snapshot.contains("graph") ? snapshot["graph"].value("name", std::string{}) : std::string{}
    );
    if (snapshot.contains("persistence")) {
        summary += snapshot["persistence"].value("enabled", false) ? " persistence=on" : " persistence=off";
    }
    XX_LOGI("{}", summary);
    XX_LOGD("{}", snapshot.dump());
}

} // namespace agent
} // namespace agentxx
