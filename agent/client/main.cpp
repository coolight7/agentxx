#include "agentxx-client/config_loader.h"
#include "agentxx-client/io/stdio/agent_stdio.h"
#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx-client/io/tui/framework/tui_settings.h"
#include "agentxx-client/mode_runners.h"
#include "agentxx-client/train/train.h"
#include "agentxx-client/util/util.h"
#include "agentxx/agent/code_agent.h"
#include "agentxx/agent/config_static.h"
#include "agentxx/agent/io/agent_server.h"
#include "agentxx/protocol/acp_server.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/settings_db.h"
#include "agentxx/util/string_util.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/signal_set.hpp"
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>

#if XX_IS_WIN_D
#include <windows.h> // GetModuleFileNameW / MAX_PATH
#endif

using namespace agentxx::client;

// ASan 默认选项 (仅在链接了 AddressSanitizer 时生效;  release/无 ASan 构建中为无害的弱符号)。
//
// 背景: TUI 全屏模式会切到备用屏 (alternate screen) + raw 模式 + 鼠标捕获。
// ASan 检测到错误后默认以 _exit() 结束进程, 绕过 ftxui 注册的退出/信号清理,
// 导致终端停留在备用屏/raw 模式: 备用屏无回滚缓冲、raw 模式滚轮失效 -> 终端"卡住",
// 且备用屏上的 ASan 报告无法滚动查看完整。
//
// 对策:
// - abort_on_error=1: ASan 改用 abort() 结束 -> 触发 SIGABRT -> ftxui 的信号处理
//   恢复终端 (退出备用屏/恢复 termios/显示光标), 终端不再卡住。
// - log_path=agentxx_asan: ASan 报告写入工作目录下 agentxx_asan.<pid> 文件
//   (恢复终端会退出备用屏, 屏上报告随之消失, 故必须落盘才能看完整报告)。
//   崩溃后用 `less agentxx_asan.*` 查看。
// 注: 环境变量 ASAN_OPTIONS 会整体覆盖此默认值。
extern "C" const char* __asan_default_options() {
    return "abort_on_error=1:log_path=agentxx_asan";
}

/// 获取当前可执行程序 (agentxx_cli) 所在目录
/// - Windows: GetModuleFileNameW 获取 exe 全路径后取目录
/// - Unix (Linux/Android): 读取 /proc/self/exe 符号链接后取目录
/// - 统一返回正斜杠 (generic_string), 与 AGENTXX_WORK_DIR 一致 (yaml 拼接无转义问题)
/// - 取不到时返回空串 (main 中注入空值 = 清除该内置变量)
static std::string getExecutableDir() noexcept {
#if XX_IS_WIN_D
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD len = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) {
            XX_LOGW("GetModuleFileNameW failed");
            return "";
        }
        if (len < buf.size()) {
            buf.resize(len);
            break;
        }
        // 缓冲不足 (长路径): 扩容重试
        buf.resize(buf.size() * 2);
    }
    return std::filesystem::path(buf).parent_path().generic_string();
#else
    std::error_code ec;
    auto            exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) {
        XX_LOGW("read_symlink(/proc/self/exe) failed: {}", ec.message());
        return "";
    }
    return exe.parent_path().generic_string();
#endif
}

/// 检查配置中是否存在可用 LLM 模型; 无可用模型时输出启动引导并返回 false
/// - 用于无 agentxx-config.yaml / 配置中未配置有效模型的启动场景:
///   在构造 CodeAgent 之前拦截, 输出配置引导后退出,
///   避免携带无效模型进入 BaseAgent 构造触发断言 abort 崩溃
///   (base_agent.cpp: assert(in_config->model.isValid()))
/// - configLoaded: 配置文件是否成功加载 (区分"文件缺失"与"文件内未配置"引导文案)
static bool ensureModelConfigured(
    const std::map<std::string, agentxx::agent::ModelConfig>& models,
    std::string_view                                          useModelKey,
    std::string_view                                          roleDesc,
    std::string_view                                          configPath,
    bool                                                      configLoaded
) {
    if (resolveModelConfig(models, useModelKey).isValid()) {
        return true;
    }
    if (!configLoaded) {
        XX_LOGE(
            R"_([Config] .yaml config file '{}' not found. 
Please copy the template to create one (agentxx-config.yaml in the project root directory, or refer to the README for usage instructions), and configure `models` and `use_model`.)_",
            configPath
        );
    } else {
        XX_LOGE(
            "[Config] No available LLM model configured: {} (use_model.{}), startup aborted.",
            roleDesc,
            useModelKey
        );
        XX_LOGE(
            R"_([Config] No valid model entry found in .yaml config file '{}' (use_model.{} = '{}'). 
Please add a model to the models list and specify the default model.
For example:

models:
    - name: my-model
        type: openai
        base_url: https://api.openai.com/v1
        api_key: ${{LLM_API_KEY}}

use_model:
    default: my-model
)_",
            configPath,
            useModelKey,
            useModelKey
        );
    }
    return false;
}

static std::string extractTokenFromUrl(std::string& url) {
    auto q = url.find('?');
    if (q == std::string::npos) {
        return "";
    }
    std::string query = url.substr(q + 1);
    url               = url.substr(0, q);
    std::string token;
    size_t      pos = 0;
    while (pos < query.size()) {
        auto        amp = query.find('&', pos);
        std::string kv
            = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        auto eq = kv.find('=');
        if (eq != std::string::npos && kv.substr(0, eq) == "token") {
            token = kv.substr(eq + 1);
        }
        if (amp == std::string::npos) {
            break;
        }
        pos = amp + 1;
    }
    return token;
}

/// 应用各模式共享的运行时装配 (MCP / Skill / Memory / 权限 / 子代理 / 插件)
/// - tui / cli / server / acp 模式共用: 分离进程启动 agent (server/acp) 时
///   同样需要完整装配, 否则 agent 侧不加载插件等扩展组件
/// - 相对路径按程序工作目录解析为绝对路径 (resolvePath 由 main 注入)
static void applySharedRuntimeConfig(
    std::shared_ptr<agentxx::agent::AgentConfig>&            config,
    const YamlAppConfig&                                     yamlCfg,
    const std::function<std::string(std::string_view path)>& resolvePath
) {
    // MCP 服务器 (来自 config.yaml 的 mcp, key 为命名空间)
    config->mcpServerUrls = yamlCfg.mcpServers;

    for (const auto& p : yamlCfg.skillDirPaths) {
        config->skillDirPaths.push_back(resolvePath(p));
    }
    for (const auto& p : yamlCfg.memoryFilePaths) {
        config->memoryFilePaths.push_back(resolvePath(p));
    }
    // CodeGraph 参数经 plugins 配置传递 (宿主不解析 args 字段语义;
    // 所有插件统一经 path 外置指定加载)

    // 权限配置 (模式 + 白/黑名单; CodeAgent 启动时按此注册文件系统读写规则)
    config->permissionMode       = yamlCfg.permissionMode;
    config->permissionAllowPaths = yamlCfg.permissionAllowPaths;
    config->permissionDenyPaths  = yamlCfg.permissionDenyPaths;

    // 子代理开关 (yaml `subagent.enable`, 默认 true)
    config->enableSubagent = yamlCfg.enableSubagent;
    // git worktree 模式 (yaml `worktree.enable`, 默认 false)
    config->enableWorktree = yamlCfg.worktreeEnable;
    // 插件配置 (yaml `plugins` 段): 相对路径按程序工作目录解析为绝对路径
    // (与 skill/memory 一致; BaseAgent::init 按此加载, 拓扑排序见 PluginManager)
    for (const auto& pc : yamlCfg.plugins) {
        agentxx::agent::PluginConfig pluginCfg;
        pluginCfg.path    = resolvePath(pc.path);
        pluginCfg.enabled = pc.enabled;
        pluginCfg.sides   = pc.sides;
        pluginCfg.args    = pc.args;
        config->plugins.push_back(std::move(pluginCfg));
    }
    if (!config->plugins.empty()) {
        XX_LOGI("[Config] plugins: {}", config->plugins.size());
    }
}

int main(int argn, char** argv) {
#if XX_IS_WIN_D
    SetConsoleOutputCP(CP_UTF8);
#endif
#if XX_IS_DEBUG_D && (XX_IS_LINUX_D || XX_IS_WIN_D)
    agentxx::util::signalError(argv[0]);
#endif

    // 注入程序内置环境变量 (启动后立即捕获, 供 yaml 配置 ${VAR} 展开使用)
    // - AGENTXX_WORK_DIR: 程序启动后的工作目录 (当前目录)
    //   yaml 中可写 `data_dir: ${AGENTXX_WORK_DIR}/...` 等相对启动目录的路径
    // - 统一使用正斜杠 (generic_string): yaml 字符串中 `\` 需转义, 正斜杠无此问题
    setBuiltinEnvVar(
        kBuiltinWorkDirEnv,
        agentxx::util::catchError<std::string>(
            []() -> std::string {
                return std::filesystem::current_path().generic_string();
            },
            [](std::string errinfo) -> std::string {
                XX_LOGW("[Config] failed to capture AGENTXX_WORK_DIR: {}", errinfo);
                return "";
            }
        )
    );

    // - AGENTXX_EXEC_DIR: agentxx_cli 可执行程序所在目录
    //   yaml 中可写模型/插件/数据等相对 exe 目录的路径 (如 `${AGENTXX_EXEC_DIR}/plugins`)
    setBuiltinEnvVar(kBuiltinExecDirEnv, getExecutableDir());

    /// 默认启动 stdio 作为日志输出，对于 tui 等自己拦截日志的可以移除后添加自己的日志拦截器
    auto defaultLogSink = std::make_shared<StderrLogSink>();
    agentxx::util::LogDispatcher::instance().addSink(defaultLogSink);

    std::string configPath = "agentxx-config.yaml";
    bool configExplicit = false; ///< --config 是否被显式指定 (指定但文件不存在时报错)
    std::string overrideEnvPath;
    std::string mode = "tui";
    std::string agentUrl;
    std::string agentToken;
    std::string remoteModel;
    std::string srvHost = "127.0.0.1";
    uint16_t    srvPort = 7007;
    for (int i = 1; i < argn; ++i) {
        auto arg = std::string_view{argv[i]};
        if (arg == "--help" || arg == "-h") {
            XX_OUT(R"_(
Usage: agentxx_cli [mode] [options]

Modes:
    tui                  TUI 交互模式 (默认)
    cli                  命令行 stdio 交互模式
    server               启动 WebSocket agent 服务
    acp                  ACP stdio 服务模式
    train                训练模式

Options:
    -h, --help           显示帮助信息
    --config <path>      配置文件路径 (默认: agentxx-config.yaml)
    --env <path>         覆盖式环境变量文件路径
    --agent <url>        远程 agent server 地址 (ws://host:port/agent)
    --token <token>      认证 token (也可通过 url 查询串携带)
    --model <model>      远程模型名称
    --host <host>        服务监听地址 (默认: 127.0.0.1)
    --port <port>        服务监听端口 (默认: 7007)
)_");
            return 0;
        } else if (arg == "--config" && i + 1 < argn) {
            ++i;
            configPath     = argv[i];
            configExplicit = true;
        } else if (arg == "--env" && i + 1 < argn) {
            ++i;
            overrideEnvPath = argv[i];
        } else if (arg == "--agent" && i + 1 < argn) {
            ++i;
            agentUrl = argv[i];
        } else if (arg == "--token" && i + 1 < argn) {
            ++i;
            agentToken = argv[i];
        } else if (arg == "--model" && i + 1 < argn) {
            ++i;
            remoteModel = argv[i];
        } else if (arg == "--host" && i + 1 < argn) {
            ++i;
            srvHost = argv[i];
        } else if (arg == "--port" && i + 1 < argn) {
            ++i;
            // 容错解析: 非法值报错退出, 避免 std::stoi 抛异常崩溃
            auto portArg = std::string_view{argv[i]};
            if (auto r = agentxx::util::parseNumberFromString(portArg, srvPort);
                r.ec != std::errc{} || srvPort == 0) {
                XX_LOGE("Invalid --port value: `{}`", portArg);
                return 1;
            }
        } else if (arg == "tui" || arg == "cli" || arg == "server" || arg == "acp"
                   || arg == "train") {
            mode = arg;
        } else {
            XX_LOGE("Unknown arg: `{}`", arg);
        }
    }

    // token 可经 url 查询串携带: ws://host:port/path?token=xxx
    if (!agentUrl.empty() && agentToken.empty()) {
        agentToken = extractTokenFromUrl(agentUrl);
    }

    // 加载覆盖式 env 文件（--env，最高优先级）
    std::map<std::string, std::string> overrideEnvVars;
    if (!overrideEnvPath.empty()) {
        overrideEnvVars = loadOverrideEnv(overrideEnvPath);
        XX_LOGI(
            "[Config] Loaded {} override variables from: {}",
            overrideEnvVars.size(),
            overrideEnvPath
        );
    }

    // 加载 .env 文件（从当前目录和配置文件所在目录，优先级高于系统环境变量）
    // - 完整查找顺序: 程序内置变量 > --env 覆盖文件 > .env 文件 > 系统环境变量 > 保留 ${VAR} 原样
    std::map<std::string, std::string> dotEnvVars;
    {
        std::vector<std::string> envPaths;
        envPaths.push_back(".env");
        auto configDir = std::filesystem::path(configPath).parent_path();
        if (!configDir.empty()) {
            envPaths.push_back((configDir / ".env").string());
        }
        dotEnvVars = loadDotEnv(envPaths);
        if (!dotEnvVars.empty()) {
            XX_LOGI("[Config] Loaded {} variables from .env", dotEnvVars.size());
        }
    }

    // 加载 YAML 配置
    YamlAppConfig yamlCfg;
    bool configLoaded = false; ///< 配置文件是否成功加载 (用于模型缺失引导文案区分)
    if (std::filesystem::exists(configPath)) {
        auto code = agentxx::util::catchError<int>(
            [&]() -> int {
                yamlCfg = loadYamlConfig(configPath, dotEnvVars, overrideEnvVars);
                XX_LOGI("[Config] Loaded config from: {}", configPath);
                configLoaded = true;
                return 0;
            },
            [&](std::string errmsg) -> int {
                XX_LOGE("[Config] Failed to load config: {}, {}", configPath, errmsg);
                return 1;
            }
        );
        if (0 != code) {
            return code;
        }
    } else if (configExplicit) {
        // 显式 --config 指定的文件不存在: 大概率是路径拼写错误, 直接报错
        XX_LOGE("[Config] Config file not found: {}", configPath);
        return 1;
    }
    // 默认路径不存在: 静默跳过加载, 由后续模型可用性检查 (ensureModelConfigured)
    // 输出"未找到配置文件"引导后退出

    // 统一数据根目录: 可在 yaml 配置 data_dir 指定
    // - tui/cli 模式支持关键字 `default`: 使用当前系统数据目录 (平台惯例,
    //   Linux/macOS: ~/.agentxx/, Windows: %APPDATA%/agentxx/)
    // - 其他值: 展开 `~` 为用户主目录; 相对路径按程序工作目录解析为绝对路径
    // - 数据子路径: {dataDir}/sqlite/global.db (全局设置),
    //   {dataDir}/sqlite/sessions/{sessionId}/ (会话数据),
    //   {dataDir}/sqlite/codegraph/... (CodeGraph 索引)
    std::string resolvedDataDir;
    if (!yamlCfg.dataDir.empty()) {
        if (yamlCfg.dataDir == agentxx::agent::AgentConfigStatic::kDefaultDataDirKey) {
            // 关键字 default: 取系统数据目录 (平台惯例)
            resolvedDataDir = agentxx::agent::AgentConfigStatic::systemDataDir();
            XX_LOGI("[Config] data_dir: default -> {}", resolvedDataDir);
        } else {
            auto dataDirExpanded = agentxx::util::expandUserHomePath(yamlCfg.dataDir);
            std::filesystem::path fp{dataDirExpanded};
            resolvedDataDir
                = fp.is_absolute()
                      ? fp.lexically_normal().string()
                      : (std::filesystem::current_path() / fp).lexically_normal().string();
            XX_LOGI("[Config] data_dir: {}", resolvedDataDir);
        }
    }

    // 会话工作目录: 可在 yaml 配置 work_dir 指定 (AgentConfig::workDir)
    // - 为空 (默认): agent 使用进程当前工作目录 (旧行为)
    // - 非空: 展开 `~`; 相对路径按程序工作目录解析为绝对路径
    // - 生效范围: permission Ask 默认放行规则 / filesystem 工具与权限校验的
    //   相对路径解析基准 / 命令执行子进程初始目录 / 插件 projectRoot
    std::string resolvedWorkDir;
    if (!yamlCfg.workDir.empty()) {
        auto workDirExpanded = agentxx::util::expandUserHomePath(yamlCfg.workDir);
        std::filesystem::path wp{workDirExpanded};
        resolvedWorkDir
            = wp.is_absolute()
                  ? wp.lexically_normal().generic_string()
                  : (std::filesystem::current_path() / wp).lexically_normal().generic_string();
        XX_LOGI("[Config] work_dir: {}", resolvedWorkDir);
    }

    // 权限询问处理模式启动提示 (yaml `permission.mode`, 默认 ask):
    // ask = 工作目录内允许/其他询问; all_ask = 全部询问; pass = 全部放行; deny = 全部拒绝
    XX_LOGI(
        "[Config] permission.mode: {} ({})",
        agentxx::client::permissionModeName(yamlCfg.permissionMode),
        agentxx::client::permissionModeDepict(yamlCfg.permissionMode)
    );
    if (!yamlCfg.permissionAllowPaths.empty()) {
        XX_LOGI(
            "[Config] permission.whitelist (始终放行): {}",
            yamlCfg.permissionAllowPaths.size()
        );
    }
    if (!yamlCfg.permissionDenyPaths.empty()) {
        XX_LOGI("[Config] permission.blacklist (始终拒绝): {}", yamlCfg.permissionDenyPaths.size());
    }

    // 解析路径：相对路径按程序工作目录解析为绝对路径
    // (用于 skill/memory/codegraph 加载与忽略路径; 放在模式分支前定义, acp 模式也可用)
    auto resolvePath = [](std::string_view p) -> std::string {
        std::filesystem::path fp{p};
        if (fp.is_absolute()) {
            return fp.lexically_normal().string();
        }
        return (std::filesystem::current_path() / fp).lexically_normal().string();
    };

    if (mode == "train") {
        // 训练模式需要 训练/评分/优化 三个模型, 任一缺失即引导退出
        // (避免无效模型进入 BaseAgent 构造断言 abort 崩溃)
        if (!ensureModelConfigured(
                yamlCfg.models,
                yamlCfg.useModelTrain,
                "train model",
                configPath,
                configLoaded
            )
            || !ensureModelConfigured(
                yamlCfg.models,
                yamlCfg.useModelTrainScorer,
                "train scorer model",
                configPath,
                configLoaded
            )
            || !ensureModelConfigured(
                yamlCfg.models,
                yamlCfg.useModelTrainOptimizer,
                "train ooptimizer model",
                configPath,
                configLoaded
            )) {
            return 1;
        }

        auto config                                    = buildDefaultConfig();
        config->dataDir                                = resolvedDataDir;
        config->workDir                                = resolvedWorkDir;
        config->logPrintToolcall                       = false;
        config->logPrintMessagesBeforeLLM              = false;
        config->logPrintMessagesBeforeLLMWithSystemMsg = false;
        config->logPrintSummarizationResultTokenCount  = false;
        applyModelToConfig(config, yamlCfg.models, yamlCfg.useModelTrain);

        auto scorerConfig                                    = buildDefaultConfig();
        scorerConfig->dataDir                                = resolvedDataDir;
        scorerConfig->workDir                                = resolvedWorkDir;
        scorerConfig->logPrintToolcall                       = false;
        scorerConfig->logPrintMessagesBeforeLLM              = false;
        scorerConfig->logPrintMessagesBeforeLLMWithSystemMsg = false;
        scorerConfig->logPrintSummarizationResultTokenCount  = false;
        applyModelToConfig(scorerConfig, yamlCfg.models, yamlCfg.useModelTrainScorer);

        auto optimizerConfig                                    = buildDefaultConfig();
        optimizerConfig->dataDir                                = resolvedDataDir;
        optimizerConfig->workDir                                = resolvedWorkDir;
        optimizerConfig->logPrintToolcall                       = false;
        optimizerConfig->logPrintMessagesBeforeLLM              = false;
        optimizerConfig->logPrintMessagesBeforeLLMWithSystemMsg = false;
        optimizerConfig->logPrintSummarizationResultTokenCount  = false;
        applyModelToConfig(optimizerConfig, yamlCfg.models, yamlCfg.useModelTrainOptimizer);

        runTrainingMode(config, scorerConfig, optimizerConfig);
        return 0;
    }

    if (mode == "acp") {
        if (!ensureModelConfigured(
                yamlCfg.models,
                yamlCfg.useModelAcp,
                "ACP model",
                configPath,
                configLoaded
            )) {
            return 1;
        }

        auto config                                   = buildDefaultConfig();
        config->dataDir                               = resolvedDataDir;
        config->workDir                               = resolvedWorkDir;
        config->logPrintToolcall                      = false;
        config->logPrintMessagesBeforeLLM             = false;
        config->logPrintSummarizationResultTokenCount = false;
        applyModelToConfig(config, yamlCfg.models, yamlCfg.useModelAcp);
        applySubagentModelToConfig(config, yamlCfg.models, yamlCfg.useModelSubagent);
        applyWebSearchModelToConfig(config, yamlCfg.models, yamlCfg.useModelWebSearch);
        // 共享运行时装配 (插件/MCP/Skill/Memory/权限/子代理): ACP 模式下 agent
        // 运行在独立进程 (分离进程启动), 与 server/tui 模式一致需要完整装配;
        // 曾缺失导致 acp 启动的 agent 不加载任何插件
        applySharedRuntimeConfig(config, yamlCfg, resolvePath);
        // CodeGraph 参数经 plugins 配置传递 (宿主不解析 args 字段语义)
        auto agent = std::make_shared<agentxx::agent::CodeAgent>(config);
        asio::co_spawn(
            *agent->ioCtx,
            [agent]() -> asio::awaitable<void> {
                co_await agent->init();
                agentxx::server::StdioAcpServer server(agent, neograph::json::object());
                server.run();
                co_return;
            },
            asio::detached
        );
        agent->ioCtx->run();
        return 0;
    }

    auto config     = buildDefaultConfig();
    config->dataDir = resolvedDataDir;
    config->workDir = resolvedWorkDir;
    applyModelToConfig(config, yamlCfg.models, yamlCfg.useModelDefault);
    applySubagentModelToConfig(config, yamlCfg.models, yamlCfg.useModelSubagent);
    applyWebSearchModelToConfig(config, yamlCfg.models, yamlCfg.useModelWebSearch);
    applyAvailableModelsToConfig(config, yamlCfg.models, yamlCfg.useModelDefault);
    applySharedRuntimeConfig(config, yamlCfg, resolvePath);

    // ======================== TUI 全局设置持久化 ========================
    // 全局设置 (动画等级/日志等级等) 存于 {dataDir}/sqlite/global.db,
    // 绑定到 TUISettings 单例, 设置变更时同步落库, 重启后恢复
    // - dataDir 未配置 (为空) 时: 不绑定数据库, 设置仅存内存 (进程生命周期有效)
    // - 注意: 系统资源显示开关已迁移到 agentxx_system_monitor 插件 (命令 /sysinfo)
    if (mode == "tui") {
        if (resolvedDataDir.empty()) {
            XX_LOGI("[Config] data_dir not set: TUI settings will NOT be persisted "
                    "(in-memory only)");
        } else {
            auto settingsDb = std::make_shared<agentxx::util::SettingsDb>(
                agentxx::agent::AgentConfigStatic::getGlobalSettingsDbPath(resolvedDataDir)
            );
            TUISettings::instance().attachDb(std::move(settingsDb));
        }
    }

    // ======================== CodeAgent Websocket Server 服务模式 ========================
    if (mode == "server") {
        if (!ensureModelConfigured(
                yamlCfg.models,
                yamlCfg.useModelDefault,
                "default model",
                configPath,
                configLoaded
            )) {
            return 1;
        }

        config->logPrintToolcall                       = false;
        config->logPrintMessagesBeforeLLM              = false;
        config->logPrintMessagesBeforeLLMWithSystemMsg = false;
        config->logPrintSummarizationResultTokenCount  = false;

        auto agent = std::make_shared<agentxx::agent::CodeAgent>(config);

        agentxx::agent::io::AgentServer::Config srvCfg;
        srvCfg.http.address = srvHost;
        srvCfg.http.port    = srvPort;
        srvCfg.token        = agentToken;
        auto server         = std::make_shared<agentxx::agent::io::AgentServer>(agent, srvCfg);

        asio::co_spawn(
            *agent->ioCtx,
            [agent, server]() -> asio::awaitable<void> {
                co_await agent->init();
                server->start(co_await asio::this_coro::executor);
                co_return;
            },
            asio::detached
        );
        asio::co_spawn(
            *agent->ioCtx,
            [agent, server]() -> asio::awaitable<void> {
                asio::signal_set         signals(*agent->ioCtx, SIGINT, SIGTERM);
                neograph_asio_error_code ec;
                co_await signals.async_wait(asio::redirect_error(asio::use_awaitable, ec));
                XX_LOGI("[agent_server] signal received, shutting down ({})...", ec.message());
                server->stop();
                agent->ioCtx->stop();
                co_return;
            },
            asio::detached
        );
        agent->ioCtx->run();
        return 0;
    }

    // ======================== 远程 client + agent server模式 (--agent) ========================
    // client 和 agent 不在同一个进程中，使用网络交互
    if (!agentUrl.empty()) {
        agentxx::util::LogDispatcher::instance().removeSink(defaultLogSink);
        if (mode == "tui") {
            config->logPrintToolcall                       = false;
            config->logPrintMessagesBeforeLLM              = false;
            config->logPrintMessagesBeforeLLMWithSystemMsg = false;
            config->logPrintSummarizationResultTokenCount  = true;
            // 远程 client 进程只加载 client 侧插件 (yaml plugins 段经 sides
            // 过滤, 见 ClientPluginManager::loadConfiguredClientPlugins)
            runRemoteTui(
                agentUrl,
                agentToken,
                remoteModel,
                yamlCfg.permissionMode,
                config->plugins
            );
        } else {
            config->logPrintToolcall                       = false;
            config->logPrintMessagesBeforeLLM              = false;
            config->logPrintMessagesBeforeLLMWithSystemMsg = false;
            config->logPrintSummarizationResultTokenCount  = false;
            runRemoteCli(agentUrl, agentToken, remoteModel, config->plugins);
        }
        return 0;
    }

    // ======================== 同一进程内 client + agent 模式 ========================
    // client 和 agent 在同一个进程中，使用线程间数据交互
    // (remote 模式已在上面 return, 此处为本地 tui/cli, 均需本地模型)
    if (!ensureModelConfigured(
            yamlCfg.models,
            yamlCfg.useModelDefault,
            "default model",
            configPath,
            configLoaded
        )) {
        return 1;
    }

    if (mode == "tui") {
        agentxx::util::LogDispatcher::instance().removeSink(defaultLogSink);
        config->logPrintToolcall                       = false;
        config->logPrintMessagesBeforeLLM              = false;
        config->logPrintMessagesBeforeLLMWithSystemMsg = false;
        config->logPrintSummarizationResultTokenCount  = true;
        auto agent = std::make_shared<agentxx::agent::CodeAgent>(config);
        // 双端插件: agent 侧经 BaseAgent::init 加载 (同 config->plugins),
        // client 侧经 runLocalTuiUnified 加载 (sides 过滤)
        runLocalTuiUnified(agent, yamlCfg.permissionMode, config->plugins);
    } else {
        agentxx::util::LogDispatcher::instance().removeSink(defaultLogSink);
        config->logPrintToolcall                       = false;
        config->logPrintMessagesBeforeLLM              = false;
        config->logPrintMessagesBeforeLLMWithSystemMsg = false;
        config->logPrintSummarizationResultTokenCount  = false;
        auto agent = std::make_shared<agentxx::agent::CodeAgent>(config);
        runLocalCliUnified(agent, config->plugins);
    }
    return 0;
}