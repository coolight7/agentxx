#include "agentxx-client/mode_runners.h"

#include "agentxx-client/io/stdio/agent_stdio.h"
#include "agentxx-client/io/stdio/cli_plugin_adapter.h"
#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx-client/io/tui/tui_plugin_adapter.h"
#include "agentxx/agent/agent_host.h"
#include "agentxx/agent/config_static.h"
#include "agentxx/agent/io/agent_server.h"
#include "agentxx/agent/io/channel_io_transport.h"
#include "agentxx/agent/io/session_server_agent_io.h"
#include "agentxx/agent/io/ws_io_transport.h"
#include "agentxx/agent/model_registry.h"
#include "agentxx/plugin/client_plugin_manager.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/log.h"
#include "agentxx/util/ws_client.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/executor_work_guard.hpp"
#include "asio/io_context.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include "neograph/json.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>
#include <thread>

// 获取当前进程 PID: 标准库与 asio 均无公开接口 (asio 内部实现也直接调用
// ::getpid()/::GetCurrentProcessId(), 如 connect_pipe.ipp), 无更通用的替代;
// 此为跨平台标准做法, 与 boost.process 内部实现一致。
#ifdef _WIN32
#include <windows.h> // GetCurrentProcessId
#else
#include <unistd.h> // getpid
#endif

namespace agentxx {
namespace client {

// ---------------------------------------------------------------------------
// 会话 sessionId 生成
// ---------------------------------------------------------------------------

/// 随机数兜底: random_device 在极端环境 (部分旧 Android/嵌入式) 可能抛异常,
/// 退回以时钟为种子, 保证生成函数绝不失败
static uint32_t randomSeed() {
    return agentxx::util::catchError<uint32_t>(
        []() -> uint32_t {
            std::random_device rd;
            return rd();
        },
        [](std::string) -> uint32_t {
            return static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()
            );
        }
    );
}

/// 生成尽量唯一的会话 sessionId:
/// - 高精度时间戳: 区分不同时刻启动的会话 (纳秒级)
/// - 进程 PID:     区分同一主机上的不同进程 (多次启动 agentxx)
/// - 随机数:       增加不可预测性, 避免并发启动碰撞
/// - 自增序号:     同进程内极端同纳秒多次调用亦唯一
std::string generateUniqueSessionId() {
    const auto ts = std::chrono::high_resolution_clock::now().time_since_epoch().count();
#ifdef _WIN32
    const long pid = static_cast<long>(::GetCurrentProcessId());
#else
    const long pid = static_cast<long>(::getpid());
#endif
    static std::atomic<uint32_t> seq{0};
    const uint32_t               cnt = seq.fetch_add(1, std::memory_order_relaxed);
    return fmt::format("sess-{:x}-{}-{:08x}-{:04x}", ts, pid, randomSeed(), cnt);
}

// ---------------------------------------------------------------------------
// client 插件系统装配辅助 (TUI/CLI/本地/远程共用)
// ---------------------------------------------------------------------------

/// 创建 ClientPluginManager 并装配到端点 (同步; 加载由调用方 co_await
/// loadConfiguredClientPlugins):
/// - 端点经 setEventSink 注入 (AgentIOBase 关键路径事件 → 插件订阅分发)
/// - TUI 额外经 setPluginManager 注入 (组件渲染/命令管线读取)
template<typename IoT, typename AdapterT>
static std::shared_ptr<agentxx::plugin::ClientPluginManager> setupClientPlugins(
    asio::any_io_executor      ex,
    std::shared_ptr<IoT>       io,
    const std::string&         sessionId,
    const ClientPluginConfigs& plugins
) {
    auto mgr = std::make_shared<agentxx::plugin::ClientPluginManager>(ex);
    mgr->setUiAdapter(std::make_shared<AdapterT>(io));
    mgr->setSessionId(sessionId);
    // TUI 端点: 组件上下文 (ctx_.pluginManager) 在 start() 的 UI 线程构建时
    // 读取, 故 setPluginManager 必须在 start() 之前调用 (见各模式装配顺序)
    if constexpr (std::is_same_v<IoT, TUIClientAgentIO>) {
        io->setPluginManager(mgr);
    }
    io->setEventSink(mgr);
    if (!plugins.empty()) {
        XX_LOGI("[client_plugin] {} configured plugin(s) for client side", plugins.size());
    }
    return mgr;
}

/// 插件命令拦截 (CLI 输入循环 / 任意 io 线程调用):
/// 输入以 "/" 开头且匹配插件注册命令时执行命令回调并返回 true (已消费);
/// 未命中命令返回 false (按普通消息发送)
static bool
    tryInvokePluginCommand(agentxx::plugin::ClientPluginManager* mgr, const std::string& input) {
    if (!mgr || input.empty() || input[0] != '/') {
        return false;
    }
    const auto spacePos = input.find(' ');
    const auto cmdName
        = input.substr(1, spacePos == std::string::npos ? std::string::npos : spacePos - 1);
    if (cmdName.empty() || !mgr->hasCommand(cmdName)) {
        return false;
    }
    // 参数: 剩余部分整体放入 {"text": "..."} (语义由插件定义)
    neograph::json args = neograph::json::object();
    args["text"] = spacePos == std::string::npos ? std::string{} : input.substr(spacePos + 1);
    mgr->invokeCommand(cmdName, args.dump()); // io 线程同步调用 (快速返回约定)
    return true;
}

/// 启动时解析 TUI 主题: 从全局设置恢复上次保存的主题 (tui.theme),
/// 未设置/库不可用时默认 Dark。须在 main 中 attachDb() 之后调用。
static TUITheme resolveTuiTheme() {
    return TUISettings::instance().themeKind() == TUISettings::kThemeLight ? TUITheme::lightTheme()
                                                                           : TUITheme::darkTheme();
}

// ---------------------------------------------------------------------------
// Local unified DIRECT (ChannelAgentIOTransport 直连 TUI ↔ SessionServerAgentIO)
// ---------------------------------------------------------------------------

/// TUI 历史分页尾窗大小: 首次接入/切换会话时服务端仅同步末尾 N 条消息
/// (与会话分页页大小 TUIClientAgentIO::kHistoryPageSize 保持一致量级)
static constexpr size_t kTuiInitialSyncTailCount = 100;

/// TUI 持有 client transport, SessionServerAgentIO 持有 server transport
/// 两端点经 Channel 直连, 无中间包装层
static std::shared_ptr<agent::SessionServerAgentIO> setupLocalUnifiedDirect(
    asio::any_io_executor               clientEx,
    std::shared_ptr<agent::CodeAgent>   agent,
    std::shared_ptr<agent::AgentIOBase> clientIO,
    const std::string&                  sessionId
) {
    auto agentEx = agent->ioCtx->get_executor();
    auto [clientTransport, serverTransport]
        = agent::ChannelAgentIOTransport::makePair(clientEx, agentEx);

    // 客户端 IO 持有 client transport
    clientIO->setTransport(std::shared_ptr<agent::AgentIOTransportBase>(std::move(clientTransport))
    );

    // 服务端: SessionServerAgentIO 持有 server transport
    agent::SessionServerAgentIO::Config scCfg;
    scCfg.sessionId = sessionId;
    // 历史分页尾窗: 仅对 TUI 客户端启用 —— 恢复长会话时服务端只回推末尾
    // 100 条 (与 TUIClientAgentIO::kHistoryPageSize 分页页大小一致), 用户
    // 向上滚动到窗口顶部时经 WireGetViewMessages 分页拉取更早历史;
    // stdio 客户端无滚动交互, 保持全量同步 (initialSyncTailCount=0)
    if (dynamic_cast<TUIClientAgentIO*>(clientIO.get()) != nullptr) {
        scCfg.initialSyncTailCount = kTuiInitialSyncTailCount;
    }
    auto serverIO = std::make_shared<agent::SessionServerAgentIO>(agentEx, agent, scCfg);
    serverIO->setTransport(std::shared_ptr<agent::AgentIOTransportBase>(std::move(serverTransport))
    );

    // 注册 agent 启动进度通知: init() 各启动阶段 (agent 线程) → 客户端端点
    // (TUI 在"启动中"banner 逐步展示当前执行的操作, 如加载 MCP/Skill/RAG 等)
    // - 必须在 init() 协程启动前注册, 否则 init 前期的步骤会丢失
    // - 回调内 clientIO 为 shared_ptr 捕获 (TUI 存活期安全); StdIO 端点
    //   不覆写 onServerProgress, 回调为 no-op
    if (auto ctx = agent->agentContext) {
        ctx->initNotifier = [clientIO](std::string_view step) {
            clientIO->onServerProgress(step);
        };
    }

    // transport 接收循环先于 init() 启动:
    // init() 中 MCP 连接等网络操作可能耗时数秒, 若等 init 完成才启动接收循环,
    // 客户端在 init 期间发出的请求 (如模型选择弹窗的 WireGetModel / WireHello) 会
    // 一直排队, 导致弹窗首次打开要等 init 完成才有数据。
    // 各 Wire 消息处理在 init 前均安全:
    // - WireGetModel: getCurrentModelName 在 modelRegistry 为空时回退 agentConfig
    // - WireHello: 历史为空时全量同步为空; Session 未绑定 io 线程时 assertIoThread 为 no-op
    // - WireUserInput: 仅入队, 由 init 之后启动的 run() 循环消费 (run 依赖 engine)
    asio::co_spawn(
        *agent->ioCtx,
        [serverIO]() -> asio::awaitable<void> {
            co_await serverIO->runTransportLoop();
        },
        asio::detached
    );

    // 在 agent 线程启动: init -> supervisor -> 会话驱动循环
    // (run 依赖 init 产出的 engine, 须在 init 完成之后启动;
    //   init 前的用户输入会先缓存在 inputChannel, run 启动后正常消费)
    asio::co_spawn(
        *agent->ioCtx,
        [agent, serverIO, clientIO, sessionId]() -> asio::awaitable<void> {
            co_await agent->init();
            // 宿主 (进程级): 主 agent 与子代理平等注册, 经 HostBus 交互;
            // attachRoot 在根 agent 总线上 registerServer 子代理委派 (service.subagent),
            // 子代理由宿主派生独立 agent 运行 (独立 AgentContext/engine)
            // - 须经 shared_ptr 持有: 总线 handler 以 weak_ptr 捕获, 避免悬空 this
            // - 注: MSVC 不支持带默认成员初始化器的聚合 + 指定初始化器,
            //   故用字段赋值构造 Config
            agentxx::agent::AgentHost::Config hostCfg;
            hostCfg.ioCtx = agent->ioCtx;
            auto host     = agentxx::agent::AgentHost::create(hostCfg);
            host->attachRoot(agent);
            // 拉取启动信息 (MCP/Skill/Memory): 须在 init 完成后发送 —— transport 接收循环
            // 已先于 init 启动, 若此时 (init 前) 发送会立即得到空列表 (组件尚未加载),
            // 客户端将永远显示不出已加载的组件
            clientIO->requestAppendComponentInfo(sessionId);
            // 通知客户端: server-io 就绪 (init/组件加载完成, 会话驱动循环即将
            // 启动并开始消费用户输入)。客户端 (TUI) 据此解除"启动中"输入限制,
            // 刷新连接前排队输入 (此时发送的输入由 server-io 端 inputChannel 缓存,
            // run() 启动后正常消费)
            clientIO->onServerReady();
            co_await serverIO->run();
        },
        asio::detached
    );

    // 客户端: 启动 transport 接收循环 (在 client 线程)
    asio::co_spawn(
        clientEx,
        [clientIO]() -> asio::awaitable<void> {
            co_await clientIO->runTransportLoop();
        },
        asio::detached
    );

    // 发送 hello 触发服务端重放/同步
    clientIO->sendToPeer(agent::WireHello{sessionId, "", 0, ""});

    return serverIO;
}

template<typename Coro>
static void runLocalUnifiedMain(std::shared_ptr<agent::CodeAgent> agent, Coro coro) {
    auto        work = asio::make_work_guard(*agent->ioCtx);
    std::thread agentThread([&agent]() {
        agent->ioCtx->run();
    });

    asio::io_context clientCtx;
    asio::co_spawn(clientCtx, std::move(coro), asio::detached);
    clientCtx.run();

    work.reset();
    agent->ioCtx->stop();
    if (agentThread.joinable()) {
        agentThread.join();
    }
}

static asio::awaitable<void>
    runLocalCliUnifiedAsync(std::shared_ptr<agent::CodeAgent> agent, ClientPluginConfigs plugins) {
    auto clientEx = co_await asio::this_coro::executor;
    auto io       = std::make_shared<StdIOClientAgentIO>();
    // 每次启动生成唯一会话 id, 避免多实例/多次启动共用 "session" 导致会话串扰
    const std::string sessionId = generateUniqueSessionId();
    io->setSessionId(sessionId);
    // client 插件系统: 装配 (事件接收器) + 加载 (sides 过滤在管理器内完成)
    auto pluginMgr = setupClientPlugins<StdIOClientAgentIO, CliPluginAdapter>(
        clientEx,
        io,
        sessionId,
        plugins
    );
    co_await pluginMgr->loadConfiguredClientPlugins(plugins);

    XX_LOGI("======= Agentxx Client (CLI, in-process unified) =======");
    auto serverIO = setupLocalUnifiedDirect(clientEx, agent, io, sessionId);
    // CLI 输入循环: 从 stdin 读取并发送 (插件命令先拦截)
    std::cout << "\n>>> " << std::flush;
    for (;;) {
        auto input = co_await io->getInput();
        if (!input.has_value()) {
            break;
        }
        if (input->empty()) {
            continue;
        }
        if (tryInvokePluginCommand(pluginMgr.get(), *input)) {
            std::cout << ">>> " << std::flush;
            continue;
        }
        io->sendToPeer(agent::WireUserInput{sessionId, *input});
    }
    // 停止服务端点并等待驱动循环退出 (避免 io_context 析构时销毁其协程导致 use-after-free)
    serverIO->stop();
    while (serverIO->running()) {
        asio::steady_timer timer(clientEx);
        timer.expires_after(std::chrono::milliseconds(20));
        co_await timer.async_wait(asio::use_awaitable);
    }
}

void runLocalCliUnified(std::shared_ptr<agent::CodeAgent> agent, ClientPluginConfigs plugins) {
    runLocalUnifiedMain(agent, runLocalCliUnifiedAsync(agent, std::move(plugins)));
}

static asio::awaitable<void> runLocalTuiUnifiedAsync(
    std::shared_ptr<agent::CodeAgent> agent,
    agent::PermissionMode             permissionMode,
    ClientPluginConfigs               plugins
) {
    auto clientEx = co_await asio::this_coro::executor;
    // 每次启动生成唯一会话 id, 避免多实例/多次启动共用 "session" 导致会话串扰
    const std::string sessionId = generateUniqueSessionId();

    // 注意: TUI 不持有 AgentContext/Session (属于 server-io 线程), 所有
    // agent 侧信息 (模型列表/上下文统计/LLM 上下文) 均经 Wire 消息由服务端获取
    auto tui = std::make_shared<TUIClientAgentIO>(
        clientEx,
        sessionId,
        resolveTuiTheme(),
        permissionMode
    );
    if (agent && agent->agentContext && agent->agentContext->agentConfig) {
        tui->setDataDir(
            agentxx::agent::AgentConfigStatic::getDataDir(agent->agentContext->agentConfig->dataDir)
        );
        tui->setWorkDir(agent->agentContext->agentConfig->resolvedWorkDir());
    }
    // client 插件系统: 装配须在 start() 之前 (ctx_.pluginManager 在 UI 线程
    // 构建组件时读取); 加载 (协程) 在 start() 之后进行, 期间注册的面板经
    // postToUi 排队, UI 线程启动后正常挂载
    auto pluginMgr
        = setupClientPlugins<TUIClientAgentIO, TuiPluginAdapter>(clientEx, tui, sessionId, plugins);
    tui->start();
    co_await pluginMgr->loadConfiguredClientPlugins(plugins);

    auto serverIO = setupLocalUnifiedDirect(clientEx, agent, tui, sessionId);

    // 请求服务端当前模型信息 (WireModelInfo 回填状态栏模型名/弹窗列表):
    // 本地模式无上下文可解析默认模型, 必须显式请求; 发送时机在 transport 就绪后
    // 即可 (init 前处理 WireGetModel 安全: modelRegistry 为空时回退 agentConfig)
    tui->sendToPeer(agent::WireGetModel{sessionId});

    // TUI 模式下输入由 FTXUI 事件循环驱动 (sendUserInputLocked 经 transport 发送)
    // 此处等待 TUI 停止
    while (tui->running()) {
        asio::steady_timer timer(clientEx);
        timer.expires_after(std::chrono::milliseconds(200));
        co_await timer.async_wait(asio::use_awaitable);
    }
    tui->stop();

    // 停止服务端点并等待其驱动循环退出, 使其协程释放对 serverIO 的持有,
    // 避免 serverIO(及其 channel transport) 在 agent io_context 析构时才被销毁
    // 而触发 channel 的 use-after-free
    serverIO->stop();
    while (serverIO->running()) {
        asio::steady_timer timer(clientEx);
        timer.expires_after(std::chrono::milliseconds(20));
        co_await timer.async_wait(asio::use_awaitable);
    }
}

void runLocalTuiUnified(
    std::shared_ptr<agent::CodeAgent> agent,
    agent::PermissionMode             permissionMode,
    ClientPluginConfigs               plugins
) {
    runLocalUnifiedMain(agent, runLocalTuiUnifiedAsync(agent, permissionMode, std::move(plugins)));
}

// ---------------------------------------------------------------------------
// Remote client (WS connection to agent server, WsAgentIOTransport 直连)
// ---------------------------------------------------------------------------

static asio::awaitable<void> runRemoteCliAsync(
    std::string         url,
    std::string         token,
    std::string         model,
    ClientPluginConfigs plugins
) {
    auto ex = co_await asio::this_coro::executor;
    auto io = std::make_shared<StdIOClientAgentIO>();

    // 每次启动生成唯一会话 id: 服务端按 sessionId 区分会话,
    // 共用 "session" 会使多个客户端实例挂到同一会话上互相串扰
    const std::string sessionId = generateUniqueSessionId();
    io->setSessionId(sessionId);
    // client 插件系统: 装配 + 加载 (远程 client 进程只加载 client 侧插件;
    // 与 agent 侧条目的 sides 过滤由管理器完成)
    auto pluginMgr
        = setupClientPlugins<StdIOClientAgentIO, CliPluginAdapter>(ex, io, sessionId, plugins);
    co_await pluginMgr->loadConfiguredClientPlugins(plugins);

    agent::WsAgentIOTransport::Config transportCfg;
    util::WsClientConfig              wsCfg;
    wsCfg.recvTimeout = std::chrono::seconds{60};

    auto transport
        = std::make_shared<agent::WsAgentIOTransport>(ex, url, token, transportCfg, wsCfg);
    io->setTransport(transport);

    XX_LOGI("======= Agentxx Remote Client (CLI, auto-reconnect) =======");

    // 连接并握手
    agent::WireHello hello{sessionId, token, 0, ""};
    bool             ok = co_await transport->connect(hello);
    if (!ok) {
        XX_LOGE("[remote_cli] connection failed");
        co_return;
    }

    // 指定模型 (经独立的模型选择通道, 而非随每条输入发送)
    if (!model.empty()) {
        io->requestSelectModel(sessionId, model);
    }

    // 启动接收循环
    asio::co_spawn(ex, io->runTransportLoop(), asio::detached);

    // 客户端启动后拉取一次启动信息 (MCP/Skill/Memory)
    io->requestAppendComponentInfo(sessionId);

    // 输入循环 (插件命令先拦截)
    std::cout << "\n>>> " << std::flush;
    for (;;) {
        auto input = co_await io->getInput();
        if (!input.has_value()) {
            break;
        }
        if (input->empty()) {
            continue;
        }
        if (tryInvokePluginCommand(pluginMgr.get(), *input)) {
            std::cout << ">>> " << std::flush;
            continue;
        }
        io->sendToPeer(agent::WireUserInput{sessionId, *input});
    }
    transport->close();
}

void runRemoteCli(
    std::string_view    url,
    std::string_view    token,
    std::string_view    model,
    ClientPluginConfigs plugins
) {
    asio::io_context ctx;
    asio::co_spawn(
        ctx,
        runRemoteCliAsync(
            std::string{url},
            std::string{token},
            std::string{model},
            std::move(plugins)
        ),
        asio::detached
    );
    ctx.run();
}

static asio::awaitable<void> runRemoteTuiAsync(
    std::string           url,
    std::string           token,
    std::string           model,
    agent::PermissionMode permissionMode,
    ClientPluginConfigs   plugins
) {
    auto ex = co_await asio::this_coro::executor;

    // 每次启动生成唯一会话 id: 服务端按 sessionId 区分会话,
    // 共用 "session" 会使多个客户端实例挂到同一会话上互相串扰
    const std::string sessionId = generateUniqueSessionId();
    // 注意: TUI 不持有 AgentContext/Session (属于 server-io 线程),
    // 模型名/上下文统计等均经 Wire 消息由服务端获取
    auto io = std::make_shared<TUIClientAgentIO>(ex, sessionId, resolveTuiTheme(), permissionMode);
    io->setRemoteUrl(url);
    // client 插件系统: 装配须在 start() 之前 (ctx_.pluginManager 在 UI 线程
    // 构建组件时读取); 加载在 start() 后进行 (面板经 postToUi 排队挂载)
    auto pluginMgr
        = setupClientPlugins<TUIClientAgentIO, TuiPluginAdapter>(ex, io, sessionId, plugins);
    io->start();
    co_await pluginMgr->loadConfiguredClientPlugins(plugins);

    // TUI 立即启动: 初始 connState=Connecting, banner 显示"server-io 正在
    // 启动中", 用户输入进入待发送队列 (不发送); 连接成败均不阻塞 TUI 界面

    // 连接流程 (失败可重试, 不退出 TUI):
    // - 每次尝试创建全新 transport (失败后的 transport 已 stopped/半初始化,
    //   复用会出现握手/写队列状态残留; 新建更干净)
    // - transport->connect 内部有限次重试 (maxReconnectAttempts=2), 均失败则
    //   返回 false → banner 显示"连接失败 + [重试]"等待用户点击
    // - 用户点击重试 → requestRetry() 置 Connecting 并唤醒 waitRetry 重新循环
    // - TUI 退出 (running_=false) 时流程尽快终止
    agent::WireHello hello{sessionId, token, 0, ""};
    bool             connected = false;
    while (!connected) {
        agent::WsAgentIOTransport::Config transportCfg;
        // 有限次尝试后返回失败 (默认 0=无限内部重连, 用户永远等不到失败提示)
        transportCfg.maxReconnectAttempts = 2;
        util::WsClientConfig wsCfg;
        wsCfg.recvTimeout = std::chrono::seconds{60};

        auto transport
            = std::make_shared<agent::WsAgentIOTransport>(ex, url, token, transportCfg, wsCfg);
        io->setTransport(transport);

        connected = co_await transport->connect(hello);
        if (connected) {
            break;
        }

        XX_LOGE("[remote_tui] connection failed, waiting for user retry");
        transport->close();
        if (!io->running()) {
            break;
        }
        io->setConnState(ConnState::Failed);
        // 等待用户点击 banner 上的"重试" (TUI 退出时尽快返回, 见 waitRetry)
        co_await io->waitRetry();
        if (!io->running()) {
            break;
        }
        io->setConnState(ConnState::Connecting);
    }

    if (!connected) {
        // 用户在连接失败 (或等待重试) 期间退出了 TUI: 停止并退出, 不进入会话
        XX_LOGW("[remote_tui] quit before connection established");
        io->stop();
        co_return;
    }

    io->setConnState(ConnState::Connected);
    // 通知事件接收器: 服务端就绪 (远程模式无 onServerReady 调用路径;
    // TUI 覆写版同时置 Connected + flushPendingInput, 幂等)
    io->onServerReady();

    // 指定模型: 记录为待应用选择 (setPendingModel), 随第一条用户消息
    // (WireUserInput.model) 携带, BaseAgent 执行新一轮会话时自动切换;
    // 不即时发送 WireSelectModel (与弹窗选择同一机制, 见 setPendingModel 注释)
    if (!model.empty()) {
        io->setPendingModel(model);
    }

    // 请求服务端当前模型信息, 待 onPeerMessage 收到 WireModelInfo 后更新显示
    io->sendToPeer(agent::WireGetModel{sessionId});

    // 客户端启动后拉取一次启动信息 (MCP/Skill/Memory)
    io->requestAppendComponentInfo(sessionId);

    // 启动接收循环
    asio::co_spawn(ex, io->runTransportLoop(), asio::detached);

    // 等待 TUI 退出
    while (io->running()) {
        asio::steady_timer timer(ex);
        timer.expires_after(std::chrono::milliseconds(200));
        co_await timer.async_wait(asio::use_awaitable);
    }
    io->transport()->close();
    io->stop();
}

void runRemoteTui(
    std::string_view      url,
    std::string_view      token,
    std::string_view      model,
    agent::PermissionMode permissionMode,
    ClientPluginConfigs   plugins
) {
    asio::io_context ctx;
    asio::co_spawn(
        ctx,
        runRemoteTuiAsync(
            std::string{url},
            std::string{token},
            std::string{model},
            permissionMode,
            std::move(plugins)
        ),
        asio::detached
    );
    ctx.run();
}

} // namespace client
} // namespace agentxx
