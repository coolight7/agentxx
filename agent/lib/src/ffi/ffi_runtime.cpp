#include "ffi_runtime.h"

#include "agentxx/agent/agent_host.h"
#include "agentxx/agent/code_agent.h"
#include "agentxx/agent/config.h"
#include "agentxx/agent/io/session_server_agent_io.h"
#include "agentxx/agent/io/wire_protocol.h"
#include "agentxx/agent/session_runtime.h"
#include "agentxx/util/exception.h"
#include "agentxx/version.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/post.hpp"
#include "asio/use_future.hpp"
#include "fmt/format.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <random>

#ifdef _WIN32
#include <windows.h> // GetCurrentProcessId
#else
#include <unistd.h> // getpid
#endif

namespace agentxx {
namespace ffi {

using agentxx::agent::AgentConfig;
using agentxx::agent::ModelConfig;

namespace {

agentxx::agent::PermissionMode permissionModeFromString(const std::string& s) {
    if (s == "all_ask") {
        return agentxx::agent::PermissionMode::AllAsk;
    }
    if (s == "pass") {
        return agentxx::agent::PermissionMode::Pass;
    }
    if (s == "deny") {
        return agentxx::agent::PermissionMode::Deny;
    }
    return agentxx::agent::PermissionMode::Ask;
}

agentxx::agent::PluginSide pluginSideFromString(const std::string& s) {
    if (s == "agent") {
        return agentxx::agent::PluginSide::Agent;
    }
    if (s == "client") {
        return agentxx::agent::PluginSide::Client;
    }
    return agentxx::agent::PluginSide::Auto;
}

} // namespace

// ---------------------------------------------------------------------------
// 日志
// ---------------------------------------------------------------------------

void FfiAgentRuntime::FfiLogSink::onLog(const utilxx_base::LogEntry& entry) {
    owner_.pushLogItem(LogItem{
        static_cast<int>(entry.level),
        entry.message,
    });
}

void FfiAgentRuntime::pushLogItem(LogItem item) {
    std::lock_guard<std::mutex> lock(logMutex_);
    if (logRing_.size() >= kLogRingCap) {
        logRing_.pop_front();
    }
    logRing_.push_back(std::move(item));
}

void FfiAgentRuntime::scheduleLogPump() {
    if (!logPumpTimer_ || !clientIoCtx_ || clientIoCtx_->stopped()) {
        return;
    }
    logPumpTimer_->expires_after(std::chrono::seconds{1});
    auto self = shared_from_this();
    logPumpTimer_->async_wait([self](const std::error_code& ec) {
        if (ec) {
            return; // 已取消 (收尾)
        }
        if (self->logSink_) {
            self->logSink_->pump();
        }
        self->scheduleLogPump();
    });
}

std::string FfiAgentRuntime::drainLogs() {
    // LogSink 队列满时丢**新**条目: 取走前先搬运一次, 让队列里没有待处理的日志,
    // 否则高频日志会把队列尾部 (最新) 全部丢掉
    if (logSink_) {
        logSink_->pump();
    }
    std::deque<LogItem> drained;
    {
        std::lock_guard<std::mutex> lock(logMutex_);
        drained.swap(logRing_);
    }
    utilxx_base::Json arr = utilxx_base::Json::array();
    for (const auto& item : drained) {
        utilxx_base::Json entry;
        entry["level"]   = item.level;
        entry["message"] = item.message;
        arr.push_back(std::move(entry));
    }
    return arr.dump();
}

// ---------------------------------------------------------------------------
// 构造 / 创建
// ---------------------------------------------------------------------------

FfiAgentRuntime::FfiAgentRuntime() :
    logSink_(std::make_shared<FfiLogSink>(*this)) {}

std::string FfiAgentRuntime::generateSessionId() {
    const auto ts = std::chrono::high_resolution_clock::now().time_since_epoch().count();
#ifdef _WIN32
    const long pid = static_cast<long>(::GetCurrentProcessId());
#else
    const long pid = static_cast<long>(::getpid());
#endif
    static std::atomic<uint32_t> seq{0};
    const uint32_t               cnt  = seq.fetch_add(1, std::memory_order_relaxed);
    uint32_t                     seed = 0;
    try {
        std::random_device rd;
        seed = rd();
    } catch (...) {
        seed = static_cast<uint32_t>(ts);
    }
    return fmt::format("ffi-{:x}-{}-{:08x}-{:04x}", ts, pid, seed, cnt);
}

std::shared_ptr<FfiAgentRuntime> FfiAgentRuntime::create(
    const AgentxxStringView*   config_json,
    const AgentxxStringView*   model_json,
    const AgentxxFFICallbacks* cb,
    std::string&               err
) {
    auto rt = std::shared_ptr<FfiAgentRuntime>(new FfiAgentRuntime());
    if (cb != nullptr) {
        rt->callbacks_ = *cb;
    }
    rt->sessionId_ = generateSessionId();
    if (!rt->buildConfigs(config_json, model_json, err)) {
        return nullptr;
    }
    return rt;
}

bool FfiAgentRuntime::buildConfigs(
    const AgentxxStringView* config_json,
    const AgentxxStringView* model_json,
    std::string&             err
) {
    auto config = std::make_shared<AgentConfig>();

    // ---- 顶层配置 (config_json) ----
    utilxx_base::Json cfgJ;
    auto              cfgSv = toSv(config_json);
    if (!cfgSv.empty()) {
        try {
            cfgJ = utilxx_base::Json::parse(cfgSv);
        } catch (const std::exception& e) {
            err = fmt::format("config_json 非法 JSON: {}", e.what());
            return false;
        }
        config->dataDir = cfgJ.value("dataDir", "");
        // 会话工作目录: 相对路径/`~` 在此按进程 cwd 展开为绝对路径
        // (嵌入多实例场景下各句柄可绑定独立项目目录, 见 AgentConfig::workDir)
        {
            auto workDir = utilxx_base::expandUserHomePath(cfgJ.value("workDir", ""));
            if (!workDir.empty()) {
                std::filesystem::path wp{workDir};
                config->workDir = wp.is_absolute() ? wp.lexically_normal().generic_string()
                                                   : (std::filesystem::current_path() / wp)
                                                         .lexically_normal()
                                                         .generic_string();
            }
        }
        config->enableSessionStore    = cfgJ.value("enableSessionStore", false);
        config->sessionStoreDirectory = cfgJ.value("sessionStoreDirectory", "");
        config->agentName             = cfgJ.value("agentName", config->agentName);
        config->llmMaxRetry           = cfgJ.value("llmMaxRetry", config->llmMaxRetry);
        config->language              = agent::normalizeLanguage(cfgJ.value("language", "en"));
        // 配置 JSON 显式给出 language 时标记为显式: 连接客户端不再用界面语言覆盖
        config->languageExplicit = cfgJ.contains("language");
        // 开发者模式 (配置 JSON `devMode`): 只控制"记录数据"的收集
        config->devMode = cfgJ.value("devMode", false);
        {
            std::lock_guard<std::mutex> lock(langMutex_);
            language_ = config->language;
        }
        config->permissionMode = permissionModeFromString(cfgJ.value("permissionMode", "ask"));
        config->permissionAllowPaths
            = utilxx_base::jsonGetStringArray(cfgJ, "permissionAllowPaths");
        config->permissionDenyPaths = utilxx_base::jsonGetStringArray(cfgJ, "permissionDenyPaths");
        config->skillDirPaths       = utilxx_base::jsonGetStringArray(cfgJ, "skills");
        config->memoryFilePaths     = utilxx_base::jsonGetStringArray(cfgJ, "memoryFiles");
        config->websearchApiUrl     = cfgJ.value("websearchApiUrl", config->websearchApiUrl);

        // 首屏只同步尾窗 (默认 200 条): 更早历史由 `context.view_page` 按页拉
        // - 0 = 完整同步 (旧行为; 嵌入方自己按页拉时不需要)
        initialSyncTailCount_
            = static_cast<size_t>(std::max<int64_t>(0, cfgJ.value("syncTailCount", int64_t{200})));

        // MCP 服务器: {"ns": {"url": "...", "timeoutSec": 120}}
        if (cfgJ.contains("mcpServers") && cfgJ["mcpServers"].is_object()) {
            for (const auto& [ns, v] : cfgJ["mcpServers"].items()) {
                const std::string nsStr{ns};
                if (!v.is_object()) {
                    continue;
                }
                agentxx::agent::McpServerConfig mc;
                mc.url                       = v.value("url", "");
                const int timeoutSec         = v.value("timeoutSec", 120);
                mc.toolTimeout               = std::chrono::milliseconds(timeoutSec * 1000);
                config->mcpServerUrls[nsStr] = std::move(mc);
            }
        }

        // 插件: [{"path","enabled","sides","args"}]
        if (cfgJ.contains("plugins") && cfgJ["plugins"].is_array()) {
            for (const auto& item : cfgJ["plugins"]) {
                if (!item.is_object()) {
                    continue;
                }
                agentxx::agent::PluginConfig pc;
                pc.path    = item.value("path", "");
                pc.enabled = item.value("enabled", true);
                pc.sides   = pluginSideFromString(item.value("sides", "auto"));
                if (item.contains("args")) {
                    pc.args = item["args"];
                }
                if (!pc.path.empty()) {
                    config->plugins.push_back(std::move(pc));
                }
            }
        }

        // HIL 中断等待嵌入方程序应答超时 (秒; 0=不限)
        interruptTimeout_
            = std::chrono::milliseconds(cfgJ.value("interruptTimeoutSec", int64_t{0}) * 1000);
    }

    // ---- 模型配置 (model_json 优先, 其次 config_json.model) ----
    utilxx_base::Json mj;
    auto              modelSv = toSv(model_json);
    if (!modelSv.empty()) {
        try {
            mj = utilxx_base::Json::parse(modelSv);
        } catch (const std::exception& e) {
            err = fmt::format("model_json 非法 JSON: {}", e.what());
            return false;
        }
    } else if (cfgJ.contains("model")) {
        mj = cfgJ["model"];
    }
    if (mj.is_null() || !mj.is_object()) {
        err = "缺少模型配置: 请传 model_json 或 config_json.model";
        return false;
    }
    ModelConfig mc;
    mc.name                     = mj.value("name", "");
    mc.type                     = mj.value("type", "openai");
    mc.baseUrl                  = mj.value("baseUrl", "");
    mc.apiKey                   = mj.value("apiKey", "EMPTY");
    mc.modelName                = mj.value("modelName", "");
    mc.apiPath                  = mj.value("apiPath", "");
    mc.connectTimeoutSeconds    = mj.value("connectTimeoutSeconds", 16);
    mc.readChunkTimeoutSeconds  = mj.value("readChunkTimeoutSeconds", 100);
    mc.maxConcurrentConnections = mj.value("maxConcurrentConnections", size_t{5});
    mc.anthropicVersion         = mj.value("anthropicVersion", "2023-06-01");
    mc.modelContextMaxToken     = mj.value("modelContextMaxToken", size_t{0});
    mc.sendThinking             = mj.value("sendThinking", false);
    // 多模态输入能力 (与 yaml 的 image_input/audio_input/video_input 同义):
    // 决定嵌入方程序能否上传图片/音频/视频, 经 WireModelInfo.capabilities 下发;
    // 字段名兼容驼峰与下划线
    mc.imageInput = mj.value("imageInput", mj.value("image_input", false));
    mc.audioInput = mj.value("audioInput", mj.value("audio_input", false));
    mc.videoInput = mj.value("videoInput", mj.value("video_input", false));
    if (mj.contains("sslVerify") && !mj["sslVerify"].is_null() && mj["sslVerify"].is_boolean()) {
        mc.sslVerify = mj["sslVerify"].get<bool>();
    }
    if (mj.contains("extraHeaders") && mj["extraHeaders"].is_object()) {
        for (const auto& [k, v] : mj["extraHeaders"].items()) {
            if (v.is_string()) {
                mc.extraHeaders[std::string{k}] = v.get<std::string>();
            }
        }
    }
    if (mj.contains("extraConfig") && mj["extraConfig"].is_object()) {
        mc.extraConfig = mj["extraConfig"];
    }
    if (mc.modelName.empty()) {
        mc.modelName = mc.name.empty() ? "Agentxx" : mc.name;
    }
    if (mc.name.empty()) {
        mc.name = mc.modelName;
    }
    if (!mc.isValid()) {
        err = "模型配置非法: 需 baseUrl 非空 或 apiKey != \"EMPTY\"";
        return false;
    }
    config->model                    = mc;
    config->availableModels[mc.name] = mc;
    config->currentModelName         = mc.name;

    try {
        agent_ = std::make_shared<agentxx::agent::CodeAgent>(config);
    } catch (const std::exception& e) {
        err = fmt::format("CodeAgent 构造失败: {}", e.what());
        return false;
    }
    // 复用 CodeAgent 自带的 io_context 作为 Server-IO 线程执行器
    serverIoCtx_ = agent_->ioCtx;
    return true;
}

void FfiAgentRuntime::joinFinishThreadIfNeeded() {
    if (!finishThread_.joinable()) {
        return;
    }
    if (finishThread_.get_id() == std::this_thread::get_id()) {
        // 自我 join 会 std::terminate; 收尾线程自己不负责回收自己
        finishThread_.detach();
        return;
    }
    finishThread_.join();
}

FfiAgentRuntime::~FfiAgentRuntime() {
    // 析构时若收尾线程还在跑, 先等它结束 (它负责发 EVT_STOPPED)
    joinFinishThreadIfNeeded();

    // 保护: 若未显式 stop, 保证内部线程与所有资源安全退出与释放
    const State st = state();
    if (st != State::Stopped && st != State::Created && !isOnAnyInternalThread()) {
        stopRequested_.store(true, std::memory_order_release);
        stopInternal();
        state_.store(State::Stopped, std::memory_order_release);
    }

    // 显式释放所有持有 transport/channel 的对象, 确保它们在 ioCtx 析构前完成清理
    clientIO_.reset();
    runtime_.reset();
    agent_.reset();
    calls_.reset();
    logPumpTimer_.reset();
}

bool FfiAgentRuntime::isOnAgentThread() const {
    const auto tid = serverThread_.get_id();
    return tid != std::thread::id{} && std::this_thread::get_id() == tid;
}

bool FfiAgentRuntime::isOnClientThread() const {
    const auto tid = clientThread_.get_id();
    return tid != std::thread::id{} && std::this_thread::get_id() == tid;
}

bool FfiAgentRuntime::isOnFinishThread() const {
    const auto tid = finishThread_.get_id();
    return tid != std::thread::id{} && std::this_thread::get_id() == tid;
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

int FfiAgentRuntime::start(std::string& err) {
    State expected = State::Created;
    if (!state_.compare_exchange_strong(expected, State::Starting)) {
        err = "状态错误: 仅 Created 状态可 start";
        return AGENTXX_FFI_ERR_STATE;
    }

    clientIoCtx_ = std::make_shared<asio::io_context>();
    logPumpTimer_ = std::make_shared<asio::steady_timer>(*clientIoCtx_);

    // FFI client 端点 (绑定 client io 线程; 端点自带合批/中断等待)
    clientIO_ = std::make_shared<FfiClientAgentIO>(clientIoCtx_->get_executor(), callbacks_);
    clientIO_->setSessionId(sessionId_);
    clientIO_->initSelf(clientIO_);

    // 启动进度 → 日志环形缓冲 (FFI 不用端点进度通道, 自己接管)
    agent_->agentContext->initNotifier = [this](std::string_view step) {
        pushLogItem(LogItem{2, fmt::format("[startup] {}", step)});
    };

    // 待完成调用表: 回执统一经 deliverResult (已在 client io 线程时同步发出)
    auto weakSelf = std::weak_ptr<FfiAgentRuntime>{shared_from_this()};
    calls_        = std::make_shared<CallRegistry>(
        *clientIoCtx_,
        [weakSelf](uint64_t reqId, std::string_view op, const CallOutcome& outcome) {
            if (auto sp = weakSelf.lock()) {
                sp->deliverResult(reqId, op, outcome);
            }
        }
    );

    // 应答分流: client io 线程收到带 reqId 的应答 → 结算对应调用
    clientIO_->onCallReply
        = [weakSelf](
              uint64_t          reqId,
              utilxx_base::Json result,
              bool              ok,
              std::string       reason,
              std::string       message
          ) {
              if (auto sp = weakSelf.lock()) {
                  sp->onCallReply(reqId, std::move(result), ok, std::move(reason), std::move(message));
              }
          };
    // 端点收尾: 仍未完成的调用按取消收尾 (在 join 之前, 因此排在 EVT_STOPPED 之前)
    clientIO_->onStopping = [weakSelf]() {
        if (auto sp = weakSelf.lock()) {
            sp->onRuntimeStopping();
        }
    };

    // 接入日志分发器
    utilxx_base::LogDispatcher::instance().addSink(logSink_);

    // 创建 work guards
    serverWorkGuard_.emplace(asio::make_work_guard(*serverIoCtx_));
    clientWorkGuard_.emplace(asio::make_work_guard(*clientIoCtx_));

    // 启动两条工作线程
    serverThread_ = std::thread([this]() {
        serverIoCtx_->run();
    });
    clientThread_ = std::thread([this]() {
        clientIoCtx_->run();
    });

    clientIO_->setAgentThreadId(serverThread_.get_id());
    clientIO_->setClientThreadId(clientThread_.get_id());

    // 统一启动壳: 两端接起来 + init/驱动/关闭的顺序全部交给它
    // - loadPluginsInHost=false: FFI 不让主程序代劳加载插件 (加载要 co_await 插件的
    //   start/挂载事务, 这里交给 agent 自己在 init 里加载, 与单 agent 模式同一路径)
    // - forwardInitProgress=false: 进度已经写进本类的日志环形缓冲
    agent::AgentSessionRuntime::Config rtCfg;
    rtCfg.sessionId            = sessionId_;
    rtCfg.clientExecutor       = clientIoCtx_->get_executor();
    rtCfg.interruptTimeout     = interruptTimeout_;
    rtCfg.deltaBufferCap       = 0; // 嵌入方程序不重连重放: 不占缓冲内存
    rtCfg.initialSyncTailCount = initialSyncTailCount_;
    rtCfg.loadPluginsInHost    = false;
    rtCfg.forwardInitProgress  = false;
    rtCfg.autoRequestComponents = true;
    {
        std::lock_guard<std::mutex> lock(langMutex_);
        rtCfg.language = language_;
    }
    rtCfg.onTransportStarted = [self = shared_from_this()]() {
        // 握手后立即拉一次模型信息 (EVT_MODEL_INFO 回填嵌入方程序的模型选择 UI)
        self->clientIO_->sendToPeer(agent::WireGetModel{self->currentSessionId()});
    };
    rtCfg.onInitError = [self = shared_from_this()](std::string_view errmsg) {
        self->clientIO_->notifyError(
            AGENTXX_FFI_ERR_INIT,
            "init_failed",
            fmt::format("agent init failed: {}", errmsg)
        );
        State expected = State::Starting;
        self->state_.compare_exchange_strong(expected, State::Failed);
    };
    rtCfg.onServerReady = [self = shared_from_this()]() {
        // 先置 Ready 再通知: 就绪回调里嵌入方程序可能立刻操作, 不能遇到 ERR_STATE
        self->state_.store(State::Ready, std::memory_order_release);
    };

    runtime_ = agent::AgentSessionRuntime::create(agent_, clientIO_, std::move(rtCfg));
    if (!runtime_) {
        err = "启动壳装配失败 (参数不完整)";
        state_.store(State::Failed, std::memory_order_release);
        return AGENTXX_FFI_ERR_INTERNAL;
    }
    runtime_->start();

    // 日志搬运: client io 上每秒一次 (LogSink 队列满时丢新条目, 必须低频持续搬运)
    asio::post(*clientIoCtx_, [self = shared_from_this()] {
        self->scheduleLogPump();
    });

    return AGENTXX_FFI_OK;
}

std::shared_ptr<agent::SessionServerAgentIO> FfiAgentRuntime::serverIO() const {
    return runtime_ ? runtime_->serverIO() : nullptr;
}

void FfiAgentRuntime::stopInternal() {
    // 1) 通知端点收尾 (失败挂起中断 + 未完成调用按取消结算) 并关传输, 再停服务端点
    if (runtime_) {
        runtime_->stop();
    }

    // 2) 等端点把"未完成调用按取消收尾"跑完: 那些回执必须排在 EVT_STOPPED 之前,
    //    而它们可能是在收尾线程上投递进 client io 的, 因此在停 client io 之前
    //    投一个屏障并等它执行 (超时只记日志, 不让收尾卡死)
    if (clientIoCtx_ && !clientIoCtx_->stopped()) {
        auto promise = std::make_shared<std::promise<void>>();
        auto future  = promise->get_future();
        asio::post(*clientIoCtx_, [this, promise] {
            onRuntimeStopping();
            try {
                promise->set_value();
            } catch (...) {
            }
        });
        if (future.wait_for(std::chrono::seconds{5}) != std::future_status::ready) {
            XX_LOGW("[ffi] stop: pending calls were not canceled within 5s");
        }
    } else {
        onRuntimeStopping();
    }

    // 3) 等待服务端点驱动循环退出 (最长 20s)
    if (auto server = serverIO()) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
        while (server->running() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (server->running()) {
            XX_LOGW("[ffi] stop: serverIO run loop did not exit within 20s, forcing stop");
        }
    }

    // 4) 摘除日志 sink
    utilxx_base::LogDispatcher::instance().removeSink(logSink_);

    // 5) 插件关闭 (必须先于 server io 停止): shutdownAsync 在 agent IO 线程上 stop →
    //    等 lease 归零 → destroy/dlclose。executor 一旦停止就只能走同步析构保护
    //    (实例保留 CloseFailed, 动态库不卸载)。
    if (agent_ && serverIoCtx_ && !serverIoCtx_->stopped()) {
        try {
            auto closed = asio::co_spawn(
                serverIoCtx_->get_executor(),
                agent_->shutdownAsync(std::chrono::seconds{20}),
                asio::use_future
            );
            if (closed.wait_for(std::chrono::seconds{25}) != std::future_status::ready) {
                XX_LOGW("[ffi] stop: plugin shutdown did not finish within 25s");
            } else if (!closed.get()) {
                XX_LOGW("[ffi] stop: plugin shutdown incomplete; instances kept as CloseFailed");
            }
        } catch (const std::exception& e) {
            XX_LOGW("[ffi] stop: plugin shutdown threw: {}", e.what());
        }
    }
    if (serverIoCtx_) {
        serverWorkGuard_.reset();
        serverIoCtx_->stop();
    }
    if (serverThread_.joinable()) {
        serverThread_.join();
    }

    // 6) 停止并 join Client-IO 线程
    if (logPumpTimer_) {
        logPumpTimer_->cancel();
    }
    if (clientIoCtx_) {
        clientWorkGuard_.reset();
        clientIoCtx_->stop();
    }
    if (clientThread_.joinable()) {
        clientThread_.join();
    }

    // 7) 显式释放所有持有 transport/channel 的对象, 确保它们在 ioCtx 析构前完成清理
    clientIO_.reset();
    runtime_.reset();
    agent_.reset();
    calls_.reset();
    logPumpTimer_.reset();
}

void FfiAgentRuntime::finishStop() {
    state_.store(State::Stopped, std::memory_order_release);
    // EVT_STOPPED 是最后一个事件: 发完之后不会再有任何回调
    if (callbacks_.on_event != nullptr) {
        emitOwnedEvent(callbacks_, AGENTXX_FFI_EVT_STOPPED, "{}");
    }
}

int FfiAgentRuntime::stop(std::string& err) {
    // 幂等: 只有第一次受理负责发 EVT_STOPPED
    if (stopRequested_.exchange(true, std::memory_order_acq_rel)) {
        return AGENTXX_FFI_OK;
    }

    const State st = state();
    if (st == State::Stopped) {
        return AGENTXX_FFI_OK;
    }
    if (st == State::Created) {
        // 还没启动任何内部线程: 直接在调用线程收尾并发 EVT_STOPPED
        // (此时不可能与其它事件并发)
        state_.store(State::Stopping, std::memory_order_release);
        clientIO_.reset();
        runtime_.reset();
        agent_.reset();
        finishStop();
        return AGENTXX_FFI_OK;
    }

    state_.store(State::Stopping, std::memory_order_release);
    try {
        finishThread_ = std::thread([self = shared_from_this()] {
            self->stopInternal();
            self->finishStop();
        });
    } catch (const std::exception& e) {
        // 起不了收尾线程: 退回同步收尾 (调用方可能被阻塞, 但总比不收尾好)
        XX_LOGE("[ffi] stop: cannot start finish thread ({}); stopping synchronously", e.what());
        stopInternal();
        finishStop();
    }
    return AGENTXX_FFI_OK;
}

int FfiAgentRuntime::destroy(std::string& err) {
    if (isOnAnyInternalThread()) {
        err = "不能在内部线程 (事件回调/收尾线程) 内调用 destroy; "
              "请在等到 EVT_STOPPED 的线程上调用";
        return AGENTXX_FFI_ERR_STATE;
    }
    const State st = state();
    if (st != State::Created && st != State::Stopped) {
        err = fmt::format(
            "状态错误: 当前状态 {} 不能 destroy; 先 stop 并等 EVT_STOPPED",
            static_cast<int>(st)
        );
        return AGENTXX_FFI_ERR_STATE;
    }
    // 纯释放: 收尾线程早在发 EVT_STOPPED 之前就做完了业务, 这里只回收线程
    joinFinishThreadIfNeeded();
    return AGENTXX_FFI_OK;
}

// ---------------------------------------------------------------------------
// 调用 (提交 + 回执)
// ---------------------------------------------------------------------------

void FfiAgentRuntime::deliverResult(
    uint64_t          reqId,
    std::string_view  op,
    const CallOutcome& outcome
) {
    // EVT_STOPPED 之后不再有事件 (收尾已走完, 迟到的回执直接丢弃)
    if (state() == State::Stopped) {
        XX_LOGD("[ffi] result for call #{} dropped (handle stopped)", reqId);
        return;
    }
    auto emit = [this, reqId, opName = std::string{op}, outcome] {
        if (!clientIO_) {
            return;
        }
        clientIO_->emitCallResult(
            reqId,
            opName,
            outcome.ok,
            outcome.code,
            outcome.errorName,
            outcome.message,
            outcome.result
        );
    };
    // 已在 client io 线程 (服务端应答 / 超时 / 收尾) 就同步发, 保证"每个 reqId 恰好
    // 一条回执且排在 EVT_STOPPED 之前"; 其余线程 (call_cancel / 本地 op) 投递过去
    if (isOnClientThread() || !clientIoCtx_ || clientIoCtx_->stopped()) {
        emit();
        return;
    }
    asio::post(*clientIoCtx_, std::move(emit));
}

void FfiAgentRuntime::onCallReply(
    uint64_t          reqId,
    utilxx_base::Json result,
    bool              ok,
    std::string       reason,
    std::string       message
) {
    if (!calls_) {
        return;
    }
    CallOutcome outcome;
    outcome.ok        = ok;
    outcome.result    = std::move(result);
    outcome.errorName = std::move(reason);
    outcome.message   = std::move(message);
    if (!calls_->settle(reqId, std::move(outcome))) {
        // 迟到应答 (已超时/已取消/已收尾): 丢弃, 不产生第二条回执
        XX_LOGD("[ffi] late reply for call #{} dropped", reqId);
    }
}

void FfiAgentRuntime::onRuntimeStopping() {
    if (!calls_) {
        return;
    }
    const size_t canceled
        = calls_->failAll(AGENTXX_FFI_ERR_CANCELED, "canceled", "runtime is stopping");
    if (canceled > 0) {
        XX_LOGD("[ffi] {} pending call(s) canceled by runtime stop", canceled);
    }
    calls_->stop();
}

void FfiAgentRuntime::notifyError(int code, std::string name, std::string message) {
    if (clientIO_) {
        clientIO_->notifyError(code, std::move(name), std::move(message));
    }
}

int FfiAgentRuntime::call(std::string_view requestJson, uint64_t* outReqId, std::string& err) {
    if (outReqId == nullptr) {
        err = "out_req_id 不能为空";
        return AGENTXX_FFI_ERR_INVALID;
    }
    *outReqId = 0;
    if (!acceptsCalls()) {
        err = "状态错误: 句柄未启动或已停止";
        return AGENTXX_FFI_ERR_STATE;
    }
    if (!calls_ || !clientIO_ || !clientIoCtx_) {
        err = "状态错误: 调用通道尚未就绪";
        return AGENTXX_FFI_ERR_STATE;
    }

    utilxx_base::Json req;
    try {
        req = utilxx_base::Json::parse(requestJson);
    } catch (const std::exception& e) {
        err = fmt::format("request_json 非法 JSON: {}", e.what());
        return AGENTXX_FFI_ERR_JSON;
    }
    if (!req.is_object()) {
        err = "request_json 必须是 JSON 对象, 形如 {\"op\":\"model.list\",\"args\":{}}";
        return AGENTXX_FFI_ERR_INVALID;
    }

    std::string op;
    if (req.contains("op") && !req["op"].is_null()) {
        if (!req["op"].is_string()) {
            err = "request_json.op 必须是字符串";
            return AGENTXX_FFI_ERR_INVALID;
        }
        op = req["op"].get<std::string>();
    }
    if (op.empty()) {
        err = "request_json 缺少 op";
        return AGENTXX_FFI_ERR_INVALID;
    }
    const FfiOpDef* def = findOp(op);
    if (def == nullptr) {
        err = fmt::format("未知 op '{}' (用 get_capabilities 取可用 op 清单)", op);
        return AGENTXX_FFI_ERR_INVALID;
    }

    utilxx_base::Json args = utilxx_base::Json::object();
    if (req.contains("args") && !req["args"].is_null()) {
        if (!req["args"].is_object()) {
            err = "request_json.args 必须是对象";
            return AGENTXX_FFI_ERR_INVALID;
        }
        args = req["args"];
    }

    int32_t timeoutMs = def->defaultTimeoutMs;
    if (req.contains("timeoutMs") && !req["timeoutMs"].is_null()) {
        if (!req["timeoutMs"].is_number_integer()) {
            err = "request_json.timeoutMs 必须是整数 (毫秒)";
            return AGENTXX_FFI_ERR_INVALID;
        }
        const int64_t v = req["timeoutMs"].get<int64_t>();
        if (v < 0) {
            err = "request_json.timeoutMs 不能为负数 (0 = 不超时)";
            return AGENTXX_FFI_ERR_INVALID;
        }
        timeoutMs = static_cast<int32_t>(std::min<int64_t>(v, INT32_MAX));
    }
    // 本地 op 不进 wire: 不登记超时
    if (def->completion == FfiCompletion::Local) {
        timeoutMs = 0;
    }

    const uint64_t reqId = calls_->begin(*def, timeoutMs);

    CallContext ctx;
    ctx.reqId       = reqId;
    ctx.def         = def;
    ctx.sessionId   = currentSessionId();
    ctx.clientIO    = clientIO_;
    ctx.clientIoCtx = clientIoCtx_.get();
    // 本地 op 的完成回调可能被异步调用 (挂起的中断等), 只持弱引用
    const std::weak_ptr<FfiAgentRuntime> weakSelf = weak_from_this();
    ctx.completeLocal
        = [weakSelf, reqId](utilxx_base::Json result) {
              auto sp = weakSelf.lock();
              if (!sp || !sp->calls_) {
                  return;
              }
              CallOutcome outcome;
              outcome.ok     = true;
              outcome.result = std::move(result);
              sp->calls_->settle(reqId, std::move(outcome));
          };

    std::string opErr;
    int32_t     rc = 0;
    try {
        rc = def->run(*this, args, ctx, opErr);
    } catch (const std::exception& e) {
        rc    = AGENTXX_FFI_ERR_INTERNAL;
        opErr = fmt::format("op '{}' threw: {}", op, e.what());
    } catch (...) {
        rc    = AGENTXX_FFI_ERR_INTERNAL;
        opErr = fmt::format("op '{}' threw an unknown exception", op);
    }
    if (rc != 0) {
        // 同步失败: 撤销登记, 不写 out_req_id, 也就没有回执
        calls_->abort(reqId);
        err = opErr.empty() ? fmt::format("op '{}' failed synchronously", op) : opErr;
        return rc;
    }

    *outReqId = reqId;
    return AGENTXX_FFI_OK;
}

int FfiAgentRuntime::callCancel(uint64_t reqId, std::string& err) {
    if (reqId == 0) {
        err = "req_id 不能为 0";
        return AGENTXX_FFI_ERR_INVALID;
    }
    if (!calls_) {
        err = fmt::format("call #{} already completed (nothing to cancel)", reqId);
        return AGENTXX_FFI_OK;
    }
    if (calls_->cancel(reqId)) {
        return AGENTXX_FFI_OK;
    }
    // 幂等: 已完成/从未发出 —— 给一条说明但不当作错误 (避免竞态里出现伪错误)
    err = fmt::format("call #{} already completed (nothing to cancel)", reqId);
    return AGENTXX_FFI_OK;
}

// ---------------------------------------------------------------------------
// 本地同步信息
// ---------------------------------------------------------------------------

std::string FfiAgentRuntime::getCapabilities() const {
    utilxx_base::Json ops = utilxx_base::Json::array();
    for (const auto& def : opsDefinition()) {
        ops.push_back(std::string{def.name});
    }
    utilxx_base::Json features = utilxx_base::Json::array(
        {"call_api", "call_cancel", "event_owned_payload", "async_stop", "op_ack"}
    );
    utilxx_base::Json j = {
        {"apiVersion",     AGENTXX_FFI_API_VERSION      },
        {"libraryVersion", std::string{agentxx::kVersion}},
        {"ops",            std::move(ops)               },
        {"features",       std::move(features)          },
    };
    return j.dump();
}

// ---------------------------------------------------------------------------
// 语言与合批
// ---------------------------------------------------------------------------

std::string FfiAgentRuntime::currentSessionId() const {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    return sessionId_;
}

void FfiAgentRuntime::setSessionBinding(std::string newSessionId) {
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        sessionId_ = newSessionId;
    }
    if (clientIO_ && clientIoCtx_) {
        auto client = clientIO_;
        asio::post(*clientIoCtx_, [client, newSessionId = std::move(newSessionId)]() mutable {
            client->setSessionId(std::move(newSessionId));
        });
    }
}

std::string FfiAgentRuntime::language() const {
    std::lock_guard<std::mutex> lock(langMutex_);
    return language_.empty() ? "en" : language_;
}

void FfiAgentRuntime::applyLanguage(std::string normalized) {
    {
        std::lock_guard<std::mutex> lock(langMutex_);
        language_ = normalized;
    }
    if (agent_ && serverIoCtx_) {
        auto ag  = agent_;
        auto sid = currentSessionId();
        asio::post(*serverIoCtx_, [ag, sid, normalized]() {
            ag->setLanguage(normalized, sid);
        });
    }
}

void FfiAgentRuntime::applyDeltaBatch(int32_t maxDelayMs) {
    if (!clientIO_ || !clientIoCtx_) {
        return;
    }
    auto clientIO = clientIO_;
    asio::post(*clientIoCtx_, [clientIO, maxDelayMs]() {
        clientIO->setDeltaBatch(maxDelayMs);
    });
}

bool FfiAgentRuntime::hasPendingInterrupt(int64_t interruptId) const {
    if (!clientIO_) {
        return false;
    }
    return clientIO_->hasPendingInterrupt(interruptId);
}

} // namespace ffi
} // namespace agentxx
