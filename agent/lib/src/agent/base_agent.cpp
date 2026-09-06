#include "agentxx/agent/base_agent.h"

#include "agentxx/agent/agent_runner.h"
#include "agentxx/agent/checkpoint_store.h"
#include "agentxx/agent/config_static.h"
#include "agentxx/agent/io/session_server_agent_io.h"
#include "agentxx/agent/session_store.h"
#include "agentxx/middlewares/permission.h"
#include "agentxx/middlewares/subagent_manager.h"
#include "agentxx/middlewares/summarization.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/util/diff_util.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/string_util.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "fmt/format.h"
#include "neograph/graph/compiler.h"
#include "neograph/graph/validator.h"
#include "neograph/llm/openai_provider.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <sstream>
#include <unordered_set>

namespace agentxx {
namespace agent {

BaseAgent::BaseAgent(std::shared_ptr<agentxx::agent::AgentConfig> in_config) {
    ioCtx                     = std::make_shared<asio::io_context>();
    agentContext              = std::make_shared<AgentContext>();
    agentContext->agentConfig = in_config;
    assert(nullptr != in_config);
    assert(in_config->model.isValid());

    // 会话 SQLite 持久化 (开启时): 消息上下文/展示历史/share store
    // 落库到 {root}/{sessionId}/, 供重启恢复; root 可经配置重定向
    // - 要求 dataDir 非空 (root 由 {dataDir}/sqlite/sessions/ 派生)
    //   或显式指定了 sessionStoreDirectory; 否则不创建持久化实例,
    //   会话仅存内存 (SessionStore/MiddlewareContext 均已判空, 安全)
    // - dataDir 未配置的警告在 init() 中输出 (构造函数可能早于日志
    //   sink 注册, 警告会丢失)
    if (in_config->enableSessionStore) {
        if (!in_config->dataDir.empty() || !in_config->sessionStoreDirectory.empty()) {
            std::string root = in_config->sessionStoreDirectory;
            if (root.empty()) {
                root = agentxx::agent::AgentConfigStatic::getSessionsDir(in_config->dataDir);
            }
            agentContext->sessions->sessionStore = std::make_shared<SessionStore>(std::move(root));
        } else {
            XX_LOGD("Session store disabled: dataDir not set and sessionStoreDirectory empty "
                    "(in-memory only)");
        }
    }
}

/// 递归检查工具参数 JSON Schema 的合法性 (含 tool prompt 填充后的定义)
/// - 背景: 严格网关 (SCNet / opencode Console Go / Gemini 等) 会校验 tools schema,
///   非法结构 (如 enum 嵌套数组 [[...]]、array 缺 items、联合类型 + items)
///   直接在请求期返回 HTTP 400, 且错误信息含糊 (invalid_prompt / INVALID_ARGUMENT
///   / Provider returned error), 难排查; 故在启动装配工具时即校验, 尽早暴露问题
/// - 检查点:
///   * enum 必须是数组, 且元素必须是标量 (string/number/boolean/null),
///     不能是数组/对象 —— 嵌套容器属于非法 enum schema;
///     (曾出现 neograph::json{vector} 列表初始化误选 initializer_list
///     构造函数产生 [["x"]] 嵌套数组的案例, 见
///     [subagent.cpp](/agent/lib/src/tools/subagent.cpp))
///   * array 类型必须带 items 字段 (Gemini 缺 items 报 "missing field")
///   * 带 items 时 type 必须为单一字符串 "array":
///     - 联合类型数组 ["string","array"] 不被 Gemini protobuf Schema 支持,
///       且带 items 而 type!=ARRAY 报 "field predicate failed: $type==Type.ARRAY"
///     - 缺 type 或 type 非字符串也视为非法 (JSON Schema 要求 type 为字符串)
///   * 递归进入 properties.* 与 items 检查子 schema
/// - 非对象/非数组节点直接跳过 (宽松), 不阻断工具注册
static void checkToolSchemaValidity(
    const neograph::json& schema,
    std::string_view      toolName,
    std::string_view      path
) {
    if (!schema.is_object()) {
        return;
    }
    if (schema.contains("enum")) {
        const auto& e = schema["enum"];
        if (!e.is_array()) {
            XX_LOGE(
                "Tool `{}` schema `{}`: enum must be an array, got {} (strict gateways "
                "reject with HTTP 400)",
                toolName,
                path,
                e.dump()
            );
            return;
        }
        for (const auto& v : e) {
            if (v.is_array() || v.is_object()) {
                XX_LOGE(
                    "Tool `{}` schema `{}`: enum element must be a scalar "
                    "(string/number/boolean), got nested {}; strict gateways reject with "
                    "HTTP 400",
                    toolName,
                    path,
                    v.dump()
                );
            }
        }
    }
    if (schema.contains("items")) {
        // 带 items 时必须声明单一 "array" 类型 (Gemini 谓词校验 $type==Type.ARRAY)
        const bool typeIsArray = schema.contains("type") && schema["type"].is_string()
                                 && schema["type"].get<std::string>() == "array";
        if (!typeIsArray) {
            XX_LOGE(
                "Tool `{}` schema `{}`: items present but type is not single \"array\" "
                "(got {}); Gemini rejects with HTTP 400 INVALID_ARGUMENT "
                "(\"field predicate failed: $type==Type.ARRAY\")",
                toolName,
                path,
                schema.contains("type") ? schema["type"].dump() : "<missing>"
            );
        }
    } else if (schema.contains("type") && schema["type"].is_string() && schema["type"].get<std::string>() == "array") {
        // array 类型必须带 items (Gemini 缺 items 报 "missing field")
        XX_LOGE(
            "Tool `{}` schema `{}`: type \"array\" must have an \"items\" field; "
            "Gemini rejects with HTTP 400 INVALID_ARGUMENT (\"missing field\")",
            toolName,
            path
        );
    }
    if (schema.contains("properties") && schema["properties"].is_object()) {
        for (const auto& [k, v] : schema["properties"].items()) {
            checkToolSchemaValidity(v, toolName, fmt::format("{}.properties.{}", path, k));
        }
    }
    if (schema.contains("items")) {
        checkToolSchemaValidity(schema["items"], toolName, fmt::format("{}.items", path));
    }
}

asio::awaitable<void> BaseAgent::init() {
    // dataDir 未配置 (为空) 时: 设置/会话/codegraph 等数据均不落盘, 仅存内存
    // - 警告提示用户: 重启后设置与历史会话无法恢复
    // - 会话持久化/CodeGraph 索引在无 dataDir 时自动禁用 (见构造函数/CodeAgent)
    // - 放在 init() 而非构造函数: 构造函数可能早于日志 sink 注册, 警告会丢失
    if (agentContext->agentConfig->dataDir.empty()) {
        XX_LOGW("AgentConfig::dataDir is not set: settings/sessions/codegraph data "
                "will NOT be persisted to disk (in-memory only)");
    }

    // 逐步上报启动进度 (客户端 TUI 在"启动中"banner 展示当前正在执行的操作)
    notifyInitProgress("检测系统环境 ...");
    // 在 agent 线程完成环境探测 (PowerShell 等) 并刷新依赖它的提示词:
    // - AgentPrompt 构造时为避免阻塞 UI/主线程启动使用非阻塞占位描述 (不探测)
    // - 此处 (agent ioCtx 线程, UI 已先行启动) 执行阻塞式子进程探测并覆盖占位;
    //   tool definition 每次 LLM 请求时从 toolPrompt 重读 (见 modelcall build_params),
    //   首个请求前必然拿到最终描述
    agentContext->agentConfig->prompt.refreshEnvDetectedPrompts();

    notifyInitProgress("初始化模型注册表 ...");
    initModelRegistry();
    notifyInitProgress("初始化事件总线 ...");
    initEventBus();

    agentContext->middlewareHandleContext
        = std::make_shared<agentxx::middleware::MiddlewareContext>(
            agentContext->sessions->sessionStore
        );

    // 插件系统装配: 工具注册表 + 插件管理器 (挂在 AgentContext, 供
    // ToolcallWrapNode/ModelCallWrapNode/中间件栈取用)
    // - 在 initMiddleware 之前创建: 插件钩子注册 (加载插件时) push 到
    //   handles 栈, 与既有中间件并存
    // - 配置插件的实际加载在 init 末尾 (engine 构建后, 见下方)
    notifyInitProgress("初始化插件系统 ...");
    agentContext->toolRegistry  = std::make_shared<agentxx::plugin::ToolRegistry>();
    agentContext->pluginManager = std::make_shared<agentxx::plugin::PluginManager>(agentContext);
    // 装配 io executor: 插件 vtable 的跨线程调用 (JS 线程等) 经 post 到 io 线程
    // 执行并同步等待 (init 运行于 io 线程, 此处记录的线程 id 即 io 线程)
    agentContext->pluginManager->setIoExecutor(co_await asio::this_coro::executor);

    {
        auto registry = std::make_shared<neograph::graph::GraphRegistry>();
        initRegisterNodes(*registry);
        graphRegistry = std::move(registry);
        // 注入 AgentContext: 插件 graph 接口表经此注册节点类型/读写图定义
        agentContext->graphRegistry = graphRegistry;
    }

    notifyInitProgress("注册中间件 (权限 / Skill / Memory / 规划) ...");
    co_await initMiddleware();

    notifyInitProgress("创建工具集 ...");
    auto tools = co_await initTools();

    initMiddlewareTools(tools);

    // 工具白名单过滤 (子代理"无工具/自定义/继承父工具"场景):
    // - 作用于 initTools + 中间件收集后的完整工具集
    // - 仅按名称过滤; 白名单中不存在的名称自然跳过 (不报错)
    if (agentContext->agentConfig->enableToolFiltering) {
        const auto& whitelist = agentContext->agentConfig->toolWhitelist;
        tools.erase(
            std::remove_if(
                tools.begin(),
                tools.end(),
                [&](const std::unique_ptr<agentxx::tools::XXToolBase>& tool) {
                    return std::find(whitelist.begin(), whitelist.end(), tool->get_name())
                           == whitelist.end();
                }
            ),
            tools.end()
        );
        XX_LOGD(
            "Tool whitelist filter: keep {} tools of whitelist {}",
            tools.size(),
            whitelist.size()
        );
    }

    notifyInitProgress("初始化上下文压缩 ...");
    initSummarizationHandles(tools);

    // 检查 tools 的提示词 (tool prompt 经 AgentPrompt::toolPrompt 填充到工具
    // 定义; 启动时校验, 避免请求期才暴露问题导致严格网关 HTTP 400)
    for (const auto& item : tools) {
        const auto& name = item->get_name();
        const auto  def  = item->get_definition();
        assert(def.name == name);

        // - tool prompt 描述 (depict) 为空时, 定义 description 为空,
        //   模型无法理解工具用途; 插件/MCP 工具可无 toolPrompt 条目,
        //   故仅检查最终生成的 description 而非条目存在性
        if (def.description.empty()) {
            XX_LOGW(
                "Tool `{}` definition description is empty (missing/empty toolPrompt "
                "depict); add an entry to AgentPrompt::toolPrompt",
                name
            );
        }

        // - parameters 缺失/null 时兜底为空对象 schema (部分严格网关如 SCNet 会因 "parameters":
        // null 返回 400 "Format Error")
        assert(def.parameters.is_object());
        // - 递归校验 parameters JSON Schema (enum 扁平标量数组等),
        //   非法 schema 会被严格网关以 HTTP 400 拒绝
        checkToolSchemaValidity(def.parameters, name, "parameters");
    }

    // 先计算默认执行图定义 (名称 "agentxx.default"), 供插件经 graph 接口表
    // 查看/修改; 插件加载(下方)完成后, 以最终值构建 engine
    agentContext->graphDefinitionJson = initGraphDefinition();

    // 加载配置启用的插件 (yaml `plugins` 段; 加载失败仅记日志不影响主流程)
    // - 提前到 engine 构建之前: 插件可在 create 阶段注册自定义节点类型
    //   (graph 接口表 register_node_type, 注入 per-agent GraphRegistry) 并
    //   修改执行图 JSON (set_graph_json, 覆盖默认图); 加载完成后下方以
    //   最终图定义构建 engine
    notifyInitProgress("加载插件 ...");
    if (agentContext->pluginManager && !agentContext->agentConfig->plugins.empty()) {
        co_await agentContext->pluginManager->loadConfiguredPlugins(
            agentContext->agentConfig->plugins
        );
    }

    // 构建执行图: 使用插件可能修改后的最终图定义; 插件修改非法时回退默认图
    notifyInitProgress("构建执行图 ...");
    auto graphDef = agentContext->graphDefinitionJson;
    if (false == graphDef.is_object()) {
        XX_LOGE("Graph definition is not an object, falling back to default graph");
        graphDef = initGraphDefinition();
    }

    auto config = agentContext->agentConfig;

    neograph::graph::NodeContext nodeContext{};
    nodeContext.instructions = config->prompt.systemPrompt;
    nodeContext.provider     = ModelProviderRegistry::createProvider(config->model);
    nodeContext.extra_config = neograph::json{
        {agentxx::nodes::ModelCallWrapNode::defUseModelRegistryKey, true},
    };

    std::vector<neograph::Tool*> toolPtrs;
    toolPtrs.reserve(tools.size());
    for (auto& t : tools) {
        toolPtrs.push_back(t.get());
    }
    nodeContext.tools = std::move(toolPtrs);

    auto topology = neograph::graph::GraphCompiler::parse(graphDef, *graphRegistry);
    auto validated
        = neograph::graph::GraphValidator::require_valid(std::move(topology), *graphRegistry);

    neograph::graph::EngineConfig engineConfig;
    engineConfig.node_context = std::move(nodeContext);
    // 仅保留每个 session 最新一个 checkpoint:
    // - engine 恢复 (resume / update_state) 只依赖最新 checkpoint 与其 pending writes
    // - 历史 checkpoint 仅用于 fork / 时间旅行, agentxx 未使用
    // - 避免每轮会话累积 O(super-steps) 的 checkpoint 内存, 无需轮末手动裁剪
    engineConfig.checkpoint_store
        = std::make_shared<agentxx::agent::InMemorySingleCheckpointStore>();

    neograph::graph::EngineResources resources;
    resources.registry = graphRegistry;

    try {
        engine = neograph::graph::GraphEngine::link(
            std::move(validated),
            std::move(engineConfig),
            std::move(resources)
        );
    } catch (const std::exception& e) {
        // 插件修改的图定义非法 (未通过编译/校验): 回退默认图, 保证 agent 可启动
        // - 注意: validated/engineConfig/resources 已被 move, 需重建
        XX_LOGE(
            "Graph build failed (plugin-modified graph likely invalid): {}\n"
            "Falling back to default graph",
            e.what()
        );
        agentContext->graphDefinitionJson = initGraphDefinition();
        graphDef                          = agentContext->graphDefinitionJson;
        auto topology2 = neograph::graph::GraphCompiler::parse(graphDef, *graphRegistry);
        auto validated2
            = neograph::graph::GraphValidator::require_valid(std::move(topology2), *graphRegistry);
        neograph::graph::EngineConfig engineConfig2;
        engineConfig2.node_context = nodeContext;
        engineConfig2.checkpoint_store
            = std::make_shared<agentxx::agent::InMemorySingleCheckpointStore>();
        neograph::graph::EngineResources resources2;
        resources2.registry = graphRegistry;
        engine              = neograph::graph::GraphEngine::link(
            std::move(validated2),
            std::move(engineConfig2),
            std::move(resources2)
        );
    }
    assert(nullptr != engine);
    {
        // 装配静态工具名集合 (插件工具注册冲突检测用; 覆盖内置/中间件/MCP 工具)
        // - 必须在 own_tools 之前收集: own_tools 会把 tools 元素 move 成空
        //   unique_ptr, 之后遍历将解引用空指针
        if (agentContext->toolRegistry) {
            std::vector<std::string> staticNames;
            staticNames.reserve(tools.size());
            for (auto& tool : tools) {
                staticNames.push_back(tool->get_name());
            }
            agentContext->toolRegistry->setStaticToolNames(std::move(staticNames));
        }
        // 记录本 agent 装配的工具名列表 (供子代理"全量继承父工具"使用)
        agentContext->toolNames.clear();
        agentContext->toolNames.reserve(tools.size());
        for (auto& tool : tools) {
            agentContext->toolNames.push_back(tool->get_name());
        }

        auto crudeTools = std::vector<std::unique_ptr<neograph::Tool>>{};
        for (auto& tool : tools) {
            crudeTools.push_back(std::move(tool));
        }
        engine->own_tools(std::move(crudeTools));
    }

    co_return;
}

void BaseAgent::notifyInitProgress(std::string_view step) {
    // 启动进度通知经 AgentContext::initNotifier 转发给客户端端点 (TUI);
    // 未注册回调时为 no-op, 不影响启动流程 (Server/CLI/headless 模式无此显示)
    if (agentContext && agentContext->initNotifier) {
        agentContext->initNotifier(step);
    }
}

void BaseAgent::initModelRegistry() {
    auto config   = agentContext->agentConfig;
    auto registry = std::make_shared<agentxx::agent::ModelProviderRegistry>();
    for (const auto& [name, mc] : config->availableModels) {
        registry->registerModel(name, mc);
    }
    if (config->availableModels.empty()) {
        registry->registerModel(config->model.modelName, config->model);
        registry->setDefaultModel(config->model.modelName);
    } else if (!config->currentModelName.empty()) {
        if (registry->hasModel(config->currentModelName)) {
            registry->setDefaultModel(config->currentModelName);
        } else {
            // 指定了不存在的模型
            XX_LOGE("指定使用的模型不存在: `{}`", config->currentModelName);
        }
    }
    // currentModelName 为空时默认模型为 registerModel 首个注册的模型
    agentContext->modelRegistry = std::move(registry);
}

void BaseAgent::initEventBus() {
    agentContext->bus = std::make_shared<agentxx::event::EventBus>(ioCtx->get_executor());
}

void BaseAgent::initRegisterNodes(neograph::graph::GraphRegistry& registry) {
    // 注意: 必须以 weak_ptr 捕获 agentContext, 防止循环引用
    // (AgentContext → graphRegistry → 节点工厂 lambda → AgentContext):
    // - 自插件 graph 接口表支持后, graphRegistry 同时被 AgentContext 持有
    //   (agentContext->graphRegistry), 若此处强引用捕获, AgentContext 永不释放
    //   (权限中间件/会话等全部泄漏)
    // - 节点构造时 lock 保证 agentContext 存活期内创建安全; 节点自身持 weak_ptr
    std::weak_ptr<AgentContext> ctx = agentContext;
    registry.register_type(
        std::string{agentxx::nodes::AgentStartCallWrapNode::defNodeType},
        [ctx](const std::string& name, const neograph::json&, const neograph::graph::NodeContext&) {
            return std::make_unique<agentxx::nodes::AgentStartCallWrapNode>(name, ctx.lock());
        }
    );
    registry.register_type(
        std::string{agentxx::nodes::AgentEndCallWrapNode::defNodeType},
        [ctx](const std::string& name, const neograph::json&, const neograph::graph::NodeContext&) {
            return std::make_unique<agentxx::nodes::AgentEndCallWrapNode>(name, ctx.lock());
        }
    );
    registry.register_type(
        std::string{agentxx::nodes::ModelCallWrapNode::defNodeType},
        [ctx](
            const std::string& name,
            const neograph::json&,
            const neograph::graph::NodeContext& nodeCtx
        ) {
            return std::make_unique<agentxx::nodes::ModelCallWrapNode>(name, nodeCtx, ctx.lock());
        }
    );
    registry.register_type(
        std::string{agentxx::nodes::ToolcallWrapNode::defNodeType},
        [ctx](
            const std::string& name,
            const neograph::json&,
            const neograph::graph::NodeContext& nodeCtx
        ) {
            return std::make_unique<agentxx::nodes::ToolcallWrapNode>(name, nodeCtx, ctx.lock());
        }
    );
}

neograph::json BaseAgent::initGraphDefinition() {
    // JSON definition equivalent to the Agent::run() ReAct loop:
    //                 ------- sub_agent_task <--- toolcall/sub_agent_task
    //                 |                            |
    //                 |<---------------------------|
    //                 |                            |
    //                 v                            |
    //  __start__  -> llm ->  has_tool_calls  ->  tools
    //                               |
    //                               v
    //                            __end__

    // clang-format off
    return neograph::json{
        {"name", std::string{kDefaultGraphName}},
        {
            "channels", {
                {"messages", {{"reducer", "append"}}},
                {
                    agentxx::middleware::MiddlewareContext::channel_savedGraphData,
                    {{"reducer", "overwrite"}},
                },
            }, 
        },
        {
            "nodes", {
                {
                    "agent_start",
                    {{
                        "type",
                        agentxx::nodes::AgentStartCallWrapNode::defNodeType,
                    }},
                },
                {
                    "agent_end",
                    {{
                        "type",
                        agentxx::nodes::AgentEndCallWrapNode::defNodeType,
                    }},
                },
                {
                    "tools",
                    {{
                        "type",
                        agentxx::nodes::ToolcallWrapNode::defNodeType,
                    }},
                },
                {
                    "llm",
                    {{
                        "type",
                        agentxx::nodes::ModelCallWrapNode::defNodeType,
                    }},
                },
            }, 
        },
        {
            "edges", neograph::json::array({
                {{"from", "__start__"}, {"to", "agent_start"}},
                {{"from", "agent_start"}, {"to", "llm"}},
                {
                    {"from", "llm"},
                    {"type", "conditional"},
                    {"condition", "has_tool_calls"},
                    {"routes", {{"true", "tools"}, {"false", "agent_end"}}},
                },
                {{"from", "tools"}, {"to", "llm"}},
                {{"from", "agent_end"}, {"to", "__end__"}},
            }),
         },
    };
    // clang-format on
}

asio::awaitable<void> BaseAgent::initMiddleware() {
    auto config = agentContext->agentConfig;
    {
        // subagent 委派管理中间件: 独立中间件持有 SubAgentManagerTool,
        // 注册事件总线服务 (service.subagent.execute), 并按配置决定是否
        // 把 `agentxx_subagent` 工具注入给模型
        auto subagentMiddleware
            = std::make_shared<agentxx::middleware::SubagentManagerMiddlewareHandle>(agentContext);
        agentContext->middlewareHandleContext->handles.push_back(subagentMiddleware);
    }

    {
        // 上下文压缩 (summarization) 中间件: 由 AgentConfig::enableSummarization 控制
        // - 子代理默认继承父配置; summarization 发起的压缩子代理显式关闭,
        //   避免对透传的上下文前缀二次压缩 (破坏 KV/prefix cache 一致性)
        if (config->enableSummarization) {
            auto summarizationMiddleware
                = std::make_shared<agentxx::middleware::SummarizationMiddlewareHandle>(agentContext
                );
            summarizationMiddleware->registerOnBus(agentContext->bus);
            agentContext->middlewareHandleContext->handles.push_back(summarizationMiddleware);
        }
    }

    {
        auto permission
            = std::make_shared<agentxx::middleware::PermissionMiddlewareHandle>(agentContext);
        // 权限规则注册 (按 yaml 配置 permission 块: mode / whitelist / blacklist,
        // 读写均注册同一套规则):
        // - 白名单: 始终放行 (最长前缀匹配, 支持 * 通配符)
        // - 黑名单: 始终拒绝 (与白名单同路径时后注册覆盖白名单, 优先生效)
        // - 模式默认规则经 noRuleOperator 生效 (未命中任何已注册规则时的
        //   兜底处理, 见 PermissionMiddlewareHandle::noRuleOperator):
        //   ask=工作目录内 ALLOW + 其余 INTERRUPT / all_ask=INTERRUPT /
        //   pass=ALLOW / deny=DENY
        // 注: 不依赖 "/*" 兜底规则 — 路由最长前缀回退到深层注册子树 (如白名单
        // 目录) 时不会命中根节点 "/*" 规则, 必须由 noRuleOperator 保证语义
        for (const auto& p : config->permissionAllowPaths) {
            permission->setFilesystemPermission(
                p,
                agentxx::middleware::PermissionOperator::ALLOW,
                agentxx::middleware::PermissionMiddlewareHandle::FilesystemPermissionWRITE
            );
            permission->setFilesystemPermission(
                p,
                agentxx::middleware::PermissionOperator::ALLOW,
                agentxx::middleware::PermissionMiddlewareHandle::FilesystemPermissionREAD
            );
        }
        for (const auto& p : config->permissionDenyPaths) {
            permission->setFilesystemPermission(
                p,
                agentxx::middleware::PermissionOperator::DENY,
                agentxx::middleware::PermissionMiddlewareHandle::FilesystemPermissionWRITE
            );
            permission->setFilesystemPermission(
                p,
                agentxx::middleware::PermissionOperator::DENY,
                agentxx::middleware::PermissionMiddlewareHandle::FilesystemPermissionREAD
            );
        }
        switch (config->permissionMode) {
            case agentxx::agent::PermissionMode::Pass:
                // 全部放行: 无规则即放行
                permission->noRuleOperator = agentxx::middleware::PermissionOperator::ALLOW;
                break;
            case agentxx::agent::PermissionMode::Deny:
                // 全部拒绝: 无规则即拒绝
                permission->noRuleOperator = agentxx::middleware::PermissionOperator::DENY;
                break;
            case agentxx::agent::PermissionMode::AllAsk:
                // 所有路径均询问: 无规则即询问
                permission->noRuleOperator = agentxx::middleware::PermissionOperator::INTERRUPT;
                break;
            case agentxx::agent::PermissionMode::Ask:
            default:
                // 会话工作目录内允许, 其他路径询问
                // - 工作目录取 AgentConfig::workDir (yaml work_dir; 未配置回退进程
                //   cwd), 使嵌入多实例/远程 server 场景下权限边界跟随会话配置而非
                //   进程启动目录
                // - 工作目录获取失败 (返回空串) 时不注册默认放行规则, 所有路径
                //   均询问 (安全兜底: 注册根目录 "/" 会退化为放行所有路径)
                {
                    const auto workPath = config->resolvedWorkDir();
                    if (workPath.empty()) {
                        XX_LOGW("PermissionMode::Ask: getCurrentWorkPath failed, "
                                "no default allow rule registered, all paths will be asked");
                    } else {
                        // 注册工作目录本身: 权限路由最长前缀回退 (prefix_fallback)
                        // 使其下任意子路径均命中此规则 (与 "{workPath}/*" 等价且
                        // 额外覆盖对工作目录自身的访问, 如列出工作目录)
                        permission->setFilesystemPermission(
                            workPath,
                            agentxx::middleware::PermissionOperator::ALLOW,
                            agentxx::middleware::PermissionMiddlewareHandle::
                                FilesystemPermissionWRITE
                        );
                        permission->setFilesystemPermission(
                            workPath,
                            agentxx::middleware::PermissionOperator::ALLOW,
                            agentxx::middleware::PermissionMiddlewareHandle::
                                FilesystemPermissionREAD
                        );
                    }
                }
                permission->noRuleOperator = agentxx::middleware::PermissionOperator::INTERRUPT;
                break;
        }
        // 注册 tool 名 -> 权限处理函数; 未调用则 handles 为空, 权限拦截不会触发
        permission->registerHandles();
        permission->registerOnBus(agentContext->bus);
        agentContext->middlewareHandleContext->handles.push_back(permission);
    }
    co_return;
}

asio::awaitable<std::vector<std::unique_ptr<agentxx::tools::XXToolBase>>> BaseAgent::initTools() {
    std::vector<std::unique_ptr<agentxx::tools::XXToolBase>> tools{};
    tools.push_back(std::make_unique<agentxx::tools::SessionShareStoreTool>(agentContext));
    // agentxx_get_current_datetime 已迁移至 agentxx_system 插件 (同名同行为,
    // 经 PluginManager 注册)
    // subagent tool (`agentxx_subagent`) 由 SubagentManagerMiddlewareHandle
    // 注入 (initMiddlewareTools 自动收集), 按 AgentConfig::enableSubagent
    // 决定是否注入; 不再在此处添加
    co_return tools;
}

void BaseAgent::initMiddlewareTools(std::vector<std::unique_ptr<agentxx::tools::XXToolBase>>& tools
) {
    for (auto& item : agentContext->middlewareHandleContext->handles) {
        if (false == item->toolcalls.empty()) {
            tools.insert(
                tools.end(),
                std::make_move_iterator(item->toolcalls.begin()),
                std::make_move_iterator(item->toolcalls.end())
            );
        }
    }
}

void BaseAgent::initSummarizationHandles(
    const std::vector<std::unique_ptr<agentxx::tools::XXToolBase>>& tools
) {
    for (auto& handle : agentContext->middlewareHandleContext->handles) {
        auto* summarization
            = dynamic_cast<agentxx::middleware::SummarizationMiddlewareHandle*>(handle.get());
        if (nullptr == summarization) {
            continue;
        }
        for (const auto& tool : tools) {
            auto toolHandle = tool->createSummarizationToolHandle();
            if (toolHandle.has_value()) {
                summarization->summarizationToolHandles[tool->get_name()] = toolHandle.value();
            }
        }
        break;
    }
}

void BaseAgent::selectModel(std::string_view sessionId, std::string_view modelName) {
    if (false == modelName.empty() && agentContext->modelRegistry
        && agentContext->modelRegistry->hasModel(modelName)) {
        agentContext->getSession(sessionId)->setModelName(modelName);
    }
}

std::string BaseAgent::getLanguage(std::string_view sessionId) const {
    if (agentContext) {
        return agentContext->getLanguage(sessionId);
    }
    return "en";
}

void BaseAgent::setLanguage(std::string_view language, std::string_view sessionId) {
    if (agentContext) {
        agentContext->setLanguage(language, sessionId);
    }
}

void BaseAgent::collectAppendComponentInfo(std::vector<AppendComponentNotification>& notifications
) {
    // MCP 工具
    for (const auto& mcp : agentContext->appendComponentInfo.mcpTools) {
        notifications.push_back(AppendComponentNotification{
            .type         = AppendComponentNotification::Type::Mcp,
            .name         = mcp,
            .success      = true,
            .errorMessage = "",
        });
    }

    // Skill
    for (const auto& skill : agentContext->appendComponentInfo.skills) {
        notifications.push_back(AppendComponentNotification{
            .type         = AppendComponentNotification::Type::Skill,
            .name         = skill,
            .success      = true,
            .errorMessage = "",
        });
    }

    // Memory 文件
    for (const auto& memory : agentContext->appendComponentInfo.memoryFiles) {
        notifications.push_back(AppendComponentNotification{
            .type         = AppendComponentNotification::Type::Memory,
            .name         = memory,
            .success      = true,
            .errorMessage = "",
        });
    }

    // Agent 侧加载的插件 (数量 + 列表; success 反映 enabled 状态)
    if (agentContext->pluginManager) {
        for (const auto& plugin : agentContext->pluginManager->list()) {
            notifications.push_back(AppendComponentNotification{
                .type         = AppendComponentNotification::Type::Plugin,
                .name         = plugin.name,
                .success      = plugin.enabled,
                .errorMessage = plugin.enabled ? "" : "disabled",
            });
        }
    }

    // 加载失败的组件 (MCP 连接失败 / Skill 目录不存在 / Memory 文件不存在等;
    // 由各加载点在启动阶段写入, success=false + errorMessage)
    // - 保留记录不清空: 客户端可能多次拉取 (重连/重同步), 需保证响应一致
    for (const auto& failed : agentContext->appendComponentInfo.failedComponents) {
        notifications.push_back(failed);
    }
}

std::string BaseAgent::getCurrentModelName(std::string_view sessionId) const {
    return agentContext->getSessionCurrentModelName(sessionId);
}

asio::awaitable<BaseAgent::TurnResult> BaseAgent::runTurnAsync(
    std::string_view             sessionId,
    std::string_view             userInput,
    std::shared_ptr<AgentIOBase> io, // server-io
    std::string_view             modelName
) {
    TurnResult turnResult;
    auto       session = agentContext->getSession(sessionId);
    session->bindIoThread();
    session->assertIoThread();

    // 插件轮次边界: 每轮开始先清理上一轮遗留的待摘除中间件 (异常路径残留自愈),
    // 再登记本轮进行中 (供 disable 立即/延迟生效判定)
    if (agentContext->pluginManager) {
        agentContext->pluginManager->flushPendingCleanup();
        agentContext->pluginManager->onTurnBegin();
    }

    if (!session->bus) {
        session->bus
            = std::make_shared<agentxx::event::EventBus>(co_await asio::this_coro::executor);
    }
    if (io) {
        io->registerOnBus(session->bus);
    }
    session->io = std::move(io);

    auto ioPtr = session->io;

    // 基于 system_clock 记录开始时间，用于后续时长计算
    const auto startTime = std::chrono::system_clock::now();
    // 记录轮次开始时间 (毫秒, Unix 时间戳, 用于显示)
    const auto startTimeMs = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(startTime.time_since_epoch()).count()
    );
    // 产出增量事件的唯一出口: 经 EventBridge 分配会话级递增 seq 后 ioPtr->sendToPeer
    // 发往对端 (server 端点会缓冲并经 transport 转发 client; io 为 nullptr 的
    // headless 场景则丢弃)
    auto eventBridge = std::make_shared<agentxx::event::EventBridge>(
        agentContext->agentConfig->agentName,
        std::string{sessionId},
        agentContext,
        session,
        ioPtr
    );

    // 插入提示消息: 由 agent 线程追加到会话历史 (viewMessages) 并发送
    // InsertMessage WireDelta 通知 UI 追加对应消息。UI 端直接处理完整 ViewMessage,
    // 保证 viewMessages / Sync 恢复 / 持久化与展示内容一致。
    // - 无对端 (headless) 时不插入 (提示为展示用途, headless 无处理者)
    auto insertMessageTip =
        [&](std::string text, ViewMessage::TipLevel level, int64_t startMs = 0, int64_t durMs = 0) {
            if (!session->io) {
                return;
            }
            auto vm
                = ViewMessage::makeText(ViewMessage::Role::Tip, std::move(text), startMs, durMs);
            vm.tip->tipLevel = level;
            vm.collapsed     = true;
            vm.id            = session->appendViewMessage(vm);
            eventBridge->emitDelta(WireDelta{
                .type    = WireDelta::Type::InsertMessage,
                .message = std::make_shared<ViewMessage>(std::move(vm)),
            });
        };

    selectModel(sessionId, modelName);

    bool resumeInterrupt = false;
    if (false == agentContext->middlewareHandleContext->graphData.contains(sessionId)) {
        auto data = engine->get_state(std::string{sessionId}).value_or(neograph::json{});
        if (data.is_object()
            && data.contains(agentxx::middleware::MiddlewareContext::channel_savedGraphData)
            && data[agentxx::middleware::MiddlewareContext::channel_savedGraphData].is_object()) {
            resumeInterrupt = true;
            agentContext->middlewareHandleContext->setGraphDataFromState(
                data[agentxx::middleware::MiddlewareContext::channel_savedGraphData],
                sessionId
            );
        }
    }

    auto processedInput = std::string{userInput};
    agentxx::util::autoConvertToUtf8(processedInput);

    auto userMsgJson = neograph::json{
        {"role",    "user"        },
        {"content", processedInput},
    };
    // 展示历史 (ViewMessage) 与 LLM 上下文 (原始 json) 分集维护:
    // 历史用于 client 同步/展示, 上下文仅用于调用 LLM API
    // - 附带开始时间戳: 会话列表的 lastActiveMs 依赖此值 (持久化 meta),
    //   无时间戳时列表无法显示活动时间
    const auto userMsgId = session->appendViewMessage(
        ViewMessage::makeText(ViewMessage::Role::User, processedInput, startTimeMs)
    );
    session->llmMessages.push_back(std::move(userMsgJson));

    // 记录轮次开始: 重置轮级 LLM API 平均生成速度 (token/s) 统计
    eventBridge->handleTurnStart();

    eventBridge->emitDelta(WireDelta{
        .type        = WireDelta::Type::TurnStart,
        .text        = processedInput,
        .msgId       = userMsgId,
        .startTimeMs = startTimeMs,
    });

    auto cancelToken = std::make_shared<neograph::graph::CancelToken>();
    session->setCancelToken(cancelToken);

    // 中断等待超时: 由 AgentRunner 内部统一解析 (取 IO 端点
    // SessionServerAgentIO::interruptTimeout 配置, <=0 不限制)

    // llm callback: 由 EventBridge 统一处理 GraphEvent -> 会话增量 WireDelta/历史/总线发布
    auto eventCallback = eventBridge->makeCallback();
    auto cfg           = neograph::graph::RunConfig{
                  .thread_id   = std::string{sessionId},
                  .input       = {{"messages", session->llmMessages}},
                  .max_steps   = 1 << 30,
                  .stream_mode = neograph::graph::StreamMode::EVENTS | neograph::graph::StreamMode::TOKENS
                       | neograph::graph::StreamMode::VALUES | neograph::graph::StreamMode::UPDATES,
                  .cancel_token = cancelToken,
        // 固定 false, 不随 isFirstMsg 变化:
        // - 引擎 checkpoint 仅进程内存活 (InMemorySingleCheckpointStore),
        //   真重启后无 checkpoint, 该标志无效
        // - 同进程内端点重建 (客户端重连/切换回会话) 时引擎仍有该线程
        //   checkpoint, 若为 true 会先 restore 再按 append reducer
        //   应用全量 input (input 每轮携带完整历史), 导致上下文翻倍
        // - 中断恢复走 AgentRunner 的 initialResult/resume_async 路径,
        //   不依赖本标志; input 全量历史在 fresh state 上应用即正确
                  .resume_if_exists = false,
    };

    co_await agentxx::util::catchErrorAsync<bool>(
        [&]() -> asio::awaitable<bool> {
            // 统一的 "运行 + 中断处理 + 恢复" 循环 (与子代理共用 AgentRunner):
            // - 语义与旧内联循环完全一致 (checkpoint 持久化 / tempMessages 恢复 /
            //   MessageTip / IO 端点 HIL 超时)
            // - 委派经 ctx->bus 请求 service.subagent (宿主 registerServer), 不限制超时
            //   (修复旧实现总线默认 30s 截断长任务子代理的问题)
            std::optional<neograph::graph::RunResult> recovered;
            if (resumeInterrupt) {
                // 程序重启恢复中断: 跳过首跑, 从恢复的 graphData 重建中断结果,
                // 直接进入中断处理循环 (重新处理可能未完成的中断并 resume)
                neograph::graph::RunResult r;
                r.interrupted = true;
                r.interrupt_node
                    = agentContext->middlewareHandleContext->getGraphDataItemValue<std::string>(
                        sessionId,
                        agentxx::middleware::MiddlewareContext::graphDataKey_interruptNode
                    );
                r.interrupt_value
                    = agentContext->middlewareHandleContext->getGraphDataItemValue<neograph::json>(
                        sessionId,
                        agentxx::middleware::MiddlewareContext::graphDataKey_interruptValue
                    );
                recovered = std::move(r);
            }

            auto runnerOutcome = co_await AgentRunner{}.run(
                agentContext,
                engine.get(),
                sessionId,
                std::move(cfg),
                cancelToken,
                AgentRunner::Hooks{
                    .eventCallback = eventCallback,
                    .onInterruptTip =
                        [&](std::string_view node, std::string_view value, std::string_view handle
                        ) {
                            // 中断头消息: 由 agent 线程插入会话历史并通知 UI
                            // (原由 client 端 handleInterrupt 构造); 后续中断
                            // 输入项消息 (Role::Interrupt) 仍由 client 端插入
                            std::string msg
                                = fmt::format("Interrupted at: {}\nValue: {}", node, value);
                            if (!handle.empty()) {
                                msg += fmt::format("\nHandle: {}", handle);
                            }
                            insertMessageTip(std::move(msg), ViewMessage::TipLevel::Info);
                        },
                    .onBeforeResume = nullptr,
                    .onRunResult    = nullptr,
                },
                recovered
            );
            turnResult.interrupted = runnerOutcome.interrupted;

            co_return true;
        },
        [&](std::string errmsg) -> asio::awaitable<bool> {
            XX_LOGE("Agent Session Response failed: {}", errmsg);
            turnResult.hasError     = true;
            turnResult.errorMessage = std::move(errmsg);
            // 错误提示: agent 线程插入会话历史并通知 UI
            insertMessageTip(turnResult.errorMessage, ViewMessage::TipLevel::Error);
            co_return true;
        },
        [&](std::string& errmsg) -> std::optional<bool> {
            XX_LOGI("Agent Session Cancelled: {}", errmsg);
            turnResult.hasError     = true;
            turnResult.errorMessage = "Cancelled by user";
            // 取消提示: agent 线程插入会话历史并通知 UI
            insertMessageTip("[Cancel Request]", ViewMessage::TipLevel::Info);
            return true;
        },
        // 传入取消令牌: engine 内未被转换的 operation_aborted (asio 取消信号)
        // 按取消语义处理, 确保 turnResult 报告 "Cancelled by user" 而非普通错误
        cancelToken
    );

    if (turnResult.hasError) {
        // - 出现异常时 state.messages 已经被回滚，提取临时保存的上下文，并写回 state
        auto& im = agentContext->middlewareHandleContext->getGraphDataItemValue<neograph::json>(
            sessionId,
            agentxx::middleware::MiddlewareContext::graphDataKey_tempMessages
        );
        if (im.is_array()) {
            XX_LOGD(
                "Recover(By exception) LLM-Messages Context: old({}) -> new({})",
                session->llmMessages.size(),
                im.size()
            );
            session->llmMessages = std::move(im);
            engine->update_state(std::string{sessionId}, [&](neograph::graph::GraphState& state) {
                state.overwrite("messages", session->llmMessages);
            });
        }
        // 处理后即清理 (含 getGraphDataItemValue 对缺失键自动创建的空条目):
        // 防止过期快照残留, 在后续无关错误中被误用作上下文回退源
        agentContext->middlewareHandleContext->removeGraphDataItem(
            sessionId,
            agentxx::middleware::MiddlewareContext::graphDataKey_tempMessages
        );
    }

    engine->update_state(std::string{sessionId}, [&](neograph::graph::GraphState& state) {
        // 中断已经处理完成，清理 graphData
        state.remove(agentxx::middleware::MiddlewareContext::channel_savedGraphData);
    });

    // 计算轮次持续时间
    const auto durationMs
        = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now() - startTime
        )
                                   .count());

    // - 取走过本轮 LLM API 平均生成速度 (token/s), 同时填入 TurnEnd WireDelta 与
    // 轮次统计系统提示
    const double turnTps = eventBridge->takeTurnTps();

    // 轮次统计系统提示: 由 agent 线程插入会话历史并发送 WireDelta (原由 UI 端
    // 在 TurnEnd 时自行构造), 模型名之后显示本轮 LLM API 平均生成速度;
    // 必须在 flushViewMessages 之前插入, 确保提示消息落盘持久化到 SQLite
    insertMessageTip(
        [&]() {
            std::string modelName = agentContext->getSessionCurrentModelName(sessionId);
            if (turnTps > 0.0) {
                if (modelName.empty()) {
                    modelName = fmt::format("{:.1f}t/s", turnTps);
                } else {
                    modelName = fmt::format("{} · {:.1f}t/s", modelName, turnTps);
                }
            }
            return fmt::format(
                "{} · {} · {}",
                std::move(modelName),
                agentxx::util::formatDurationMilliseconds(durationMs),
                agentxx::util::formatTimestampMilliseconds(startTimeMs + durationMs)
            );
        }(),
        ViewMessage::TipLevel::Info,
        startTimeMs,
        durationMs
    );

    // 持久化 LLM 上下文消息 (每轮结束时整表替换, 供重启恢复会话)
    // - 持久化回调内部捕获异常, 失败仅记日志, 不影响本轮结果
    // - 轮内已由 EventBridge 按消息结算节流落盘 (appendSettledLlmMessages),
    //   此处为权威终态同步; 进程中途被杀最多丢一个节流窗口 (<3s) 的增量
    session->saveLlmMessages();
    // 补存节流窗口内尚未落库的 view 消息操作, 保证正常结束的轮次其展示历史
    // 全部落库 (含刚插入的轮次统计系统提示; 仅进程中途被杀才可能丢失窗口内 <3s 的尾部消息)
    session->flushViewMessages();

    eventBridge->emitDelta(WireDelta{
        .type         = WireDelta::Type::TurnEnd,
        .historyCount = session->chainHash.count(),
        .tailHash     = session->chainHash.tailHex(),
        .startTimeMs  = startTimeMs,
        .durationMs   = durationMs,
        .tps          = turnTps,
    });

    // checkpoint store 采用 InMemorySingleCheckpointStore, save 时自动淘汰
    // 该 session 的历史 checkpoint, 轮末无需额外裁剪

    // 插件轮次结束: 正常路径登记轮次退出 (异常路径下轮开始时自愈)
    if (agentContext->pluginManager) {
        agentContext->pluginManager->onTurnEnd();
    }

    co_return turnResult;
}

BaseAgent::~BaseAgent() {
    engine = nullptr;
}

neograph::graph::GraphEngine* BaseAgent::getEngine() {
    return engine.get();
}

std::shared_ptr<AgentContext> BaseAgent::getContext() {
    return agentContext;
}

asio::awaitable<BaseAgent::SimpleRunResult> BaseAgent::runInternalAsync(
    std::string_view                     sessionId,
    std::vector<neograph::ChatMessage>   messages,
    neograph::graph::GraphStreamCallback callback,
    std::string_view                     modelName,
    bool                                 cleanupAfter
) {
    selectModel(sessionId, modelName);
    auto inputMessages = neograph::json::array();
    for (auto& msg : messages) {
        neograph::json j;
        neograph::to_json(j, msg);
        inputMessages.push_back(std::move(j));
    }
    neograph::graph::RunConfig cfg{
        .thread_id        = std::string{sessionId},
        .input            = {{"messages", std::move(inputMessages)}},
        .resume_if_exists = false,
    };
    std::string oss;
    // 统一的 LLM_TOKEN 收集 + 透传 callback
    auto wrappedCb = [callback, &oss](const neograph::graph::GraphEvent& ev) {
        if (ev.type == neograph::graph::GraphEvent::Type::LLM_TOKEN) {
            try {
                if (ev.data.is_string()) {
                    oss += ev.data.get<std::string>();
                } else if (ev.data.is_object()) {
                    neograph::ChatStreamChunk ch;
                    neograph::from_json(ev.data, ch);
                    if (ch.type != neograph::ChatStreamChunk::TYPE_THINKING) {
                        oss += ch.data;
                    }
                }
            } catch (...) {
            }
        }
        if (callback) {
            callback(ev);
        }
    };
    auto result
        = co_await engine->run_stream_async(cfg, neograph::graph::GraphStreamCallback{[&](auto& e) {
                                                wrappedCb(e);
                                            }});
    if (cleanupAfter) {
        if (agentContext->middlewareHandleContext) {
            agentContext->middlewareHandleContext->cleanupSession(std::string{sessionId});
        }
        agentContext->sessions->remove(sessionId);
    }
    co_return SimpleRunResult{.content = std::move(oss), .fullResult = std::move(result)};
}

asio::awaitable<std::string> BaseAgent::runOverMsgsTurnAsync(
    std::string_view                                        sessionId,
    const std::vector<neograph::ChatMessage>&               messages,
    std::function<void(const neograph::graph::GraphEvent&)> callback,
    std::string_view                                        modelName
) {
    neograph::graph::GraphStreamCallback cb;
    if (callback) {
        cb = [callback](const neograph::graph::GraphEvent& e) {
            callback(e);
        };
    }
    auto r = co_await runInternalAsync(
        sessionId,
        std::vector<neograph::ChatMessage>(messages),
        cb,
        modelName,
        false
    );
    co_return r.content;
}

asio::awaitable<std::string> BaseAgent::runSingleInputAsync(
    std::string_view sessionId,
    std::string_view userInput,
    std::string_view systemPrompt,
    std::string_view modelName
) {
    std::vector<neograph::ChatMessage> messages;
    if (!systemPrompt.empty()) {
        messages.push_back(
            neograph::ChatMessage{.role = "system", .content = std::string{systemPrompt}}
        );
    }
    messages.push_back(neograph::ChatMessage{.role = "user", .content = std::string{userInput}});
    co_return co_await runOverMsgsTurnAsync(sessionId, messages, nullptr, modelName);
}

asio::awaitable<BaseAgent::SimpleRunResult> BaseAgent::runStreamTurnAsync(
    const std::vector<neograph::ChatMessage>& messages,
    std::string_view                          modelName
) {
    static std::atomic<uint64_t> runStreamSeq{0};
    const auto                   ts = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto sessionId            = fmt::format("subagent_{}_{}", ts, runStreamSeq.fetch_add(1));
    auto       r                    = co_await runInternalAsync(
        sessionId,
        std::vector<neograph::ChatMessage>(messages),
        nullptr,
        modelName,
        true
    );
    co_return r;
}

} // namespace agent
} // namespace agentxx
