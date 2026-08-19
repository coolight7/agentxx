#include "ffi_runtime.h"

#include "agentxx/agent/agent_host.h"
#include "agentxx/agent/code_agent.h"
#include "agentxx/agent/config.h"
#include "agentxx/agent/io/channel_io_transport.h"
#include "agentxx/agent/io/session_server_agent_io.h"
#include "agentxx/agent/io/wire_protocol.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/log.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/post.hpp"
#include "fmt/format.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

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

/// 宽松读取字符串 (非字符串时返回默认值)
std::string jsonStr(const neograph::json& j, const char* key, std::string def) {
    if (j.contains(key) && j[key].is_string()) {
        return j[key].get<std::string>();
    }
    return def;
}

/// 宽松读取整数 (非数字时返回默认值)
template<typename T>
T jsonInt(const neograph::json& j, const char* key, T def) {
    if (j.contains(key) && j[key].is_number_integer()) {
        return j[key].get<T>();
    }
    return def;
}

/// 宽松读取 bool
bool jsonBool(const neograph::json& j, const char* key, bool def) {
    if (j.contains(key) && j[key].is_boolean()) {
        return j[key].get<bool>();
    }
    return def;
}

/// 宽松读取字符串数组
void jsonStrArray(const neograph::json& j, const char* key, std::vector<std::string>& out) {
    if (!j.contains(key) || !j[key].is_array()) {
        return;
    }
    for (const auto& v : j[key]) {
        if (v.is_string()) {
            out.push_back(v.get<std::string>());
        }
    }
}

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

void FfiAgentRuntime::FfiLogSink::onLog(const util::LogEntry& entry) {
    // 仅收集 Info(2)/Warn(3)/Error(4), 避免 trace/debug 淹没环形缓冲
    const int lv = static_cast<int>(entry.level);
    if (lv < 2 || lv > 4) {
        return;
    }
    owner_.pushLogItem(LogItem{lv, entry.message});
}

void FfiAgentRuntime::pushLogItem(LogItem item) {
    std::lock_guard<std::mutex> lock(logMutex_);
    logRing_.push_back(std::move(item));
    while (logRing_.size() > kLogRingCap) {
        logRing_.pop_front();
    }
}

static const char* logLevelName(int lv) {
    switch (lv) {
        case 0:
            return "trace";
        case 1:
            return "debug";
        case 2:
            return "info";
        case 3:
            return "warn";
        case 4:
            return "error";
        default:
            return "out";
    }
}

std::string FfiAgentRuntime::drainLogs() {
    std::lock_guard<std::mutex> lock(logMutex_);
    neograph::json              arr = neograph::json::array();
    for (const auto& item : logRing_) {
        arr.push_back(neograph::json{
            {"level",   logLevelName(item.level)},
            {"message", item.message            },
        });
    }
    logRing_.clear();
    return arr.dump();
}

// ---------------------------------------------------------------------------
// 构造 / 配置
// ---------------------------------------------------------------------------

std::string FfiAgentRuntime::generateThreadId() {
    const auto ts = std::chrono::high_resolution_clock::now().time_since_epoch().count();
#ifdef _WIN32
    const long pid = static_cast<long>(::GetCurrentProcessId());
#else
    const long pid = static_cast<long>(::getpid());
#endif
    static std::atomic<uint64_t> seq{0};
    return fmt::format("ffi-{:x}-{}-{:08x}", ts, pid, seq.fetch_add(1, std::memory_order_relaxed));
}

FfiAgentRuntime::FfiAgentRuntime() :
    threadId_(generateThreadId()) {}

std::shared_ptr<FfiAgentRuntime> FfiAgentRuntime::create(
    const char*             config_json,
    const char*             model_json,
    const AgentxxCallbacks* cb,
    std::string&            err
) {
    auto rt = std::shared_ptr<FfiAgentRuntime>(new FfiAgentRuntime());
    if (cb != nullptr) {
        rt->callbacks_ = *cb;
    }
    if (!rt->buildConfigs(config_json, model_json, err)) {
        return nullptr;
    }
    // 日志 ring sink 在 start 时接入 LogDispatcher
    rt->logSink_ = std::make_shared<FfiLogSink>(*rt);
    return rt;
}

bool FfiAgentRuntime::buildConfigs(
    const char*  config_json,
    const char*  model_json,
    std::string& err
) {
    auto config = std::make_shared<AgentConfig>();

    // ---- AgentConfig 覆盖 JSON ----
    neograph::json cfgJ = neograph::json::object();
    if (config_json != nullptr && *config_json != '\0') {
        try {
            cfgJ = neograph::json::parse(config_json);
        } catch (const std::exception& e) {
            err = fmt::format("config_json 非法 JSON: {}", e.what());
            return false;
        }
        if (!cfgJ.is_object()) {
            err = "config_json 须为 JSON 对象";
            return false;
        }
        config->dataDir                  = jsonStr(cfgJ, "dataDir", "");
        config->enableSessionPersistence = jsonBool(cfgJ, "enableSessionPersistence", false);
        config->sessionPersistenceRoot   = jsonStr(cfgJ, "sessionPersistenceRoot", "");
        config->agentName                = jsonStr(cfgJ, "agentName", config->agentName);
        config->llmMaxRetry              = jsonInt(cfgJ, "llmMaxRetry", config->llmMaxRetry);
        config->permissionMode = permissionModeFromString(jsonStr(cfgJ, "permissionMode", "ask"));
        jsonStrArray(cfgJ, "permissionAllowPaths", config->permissionAllowPaths);
        jsonStrArray(cfgJ, "permissionDenyPaths", config->permissionDenyPaths);
        jsonStrArray(cfgJ, "skills", config->skillDirPaths);
        jsonStrArray(cfgJ, "memoryFiles", config->memoryFilePaths);
        config->websearchApiUrl = jsonStr(cfgJ, "websearchApiUrl", config->websearchApiUrl);

        // MCP 服务器: {"ns": {"url": "...", "timeoutSec": 120}}
        if (cfgJ.contains("mcpServers") && cfgJ["mcpServers"].is_object()) {
            for (const auto& [ns, v] : cfgJ["mcpServers"].items()) {
                if (!v.is_object()) {
                    continue;
                }
                agentxx::agent::McpServerConfig mc;
                mc.url                    = jsonStr(v, "url", "");
                const int timeoutSec      = jsonInt(v, "timeoutSec", 120);
                mc.toolTimeout            = std::chrono::milliseconds(timeoutSec * 1000);
                config->mcpServerUrls[ns] = std::move(mc);
            }
        }

        // 插件: [{"path","enabled","sides","args"}]
        if (cfgJ.contains("plugins") && cfgJ["plugins"].is_array()) {
            for (const auto& item : cfgJ["plugins"]) {
                if (!item.is_object()) {
                    continue;
                }
                agentxx::agent::PluginConfig pc;
                pc.path    = jsonStr(item, "path", "");
                pc.enabled = jsonBool(item, "enabled", true);
                pc.sides   = pluginSideFromString(jsonStr(item, "sides", "auto"));
                if (item.contains("args")) {
                    pc.args = item["args"];
                }
                if (!pc.path.empty()) {
                    config->plugins.push_back(std::move(pc));
                }
            }
        }

        // HIL 中断等待宿主应答超时 (秒; 0=不限)
        interruptTimeout_
            = std::chrono::milliseconds(jsonInt(cfgJ, "interruptTimeoutSec", int64_t{0}) * 1000);
    }

    // ---- 模型配置 (model_json 优先, 其次 config_json.model) ----
    neograph::json mj;
    if (model_json != nullptr && *model_json != '\0') {
        try {
            mj = neograph::json::parse(model_json);
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
    mc.name                     = jsonStr(mj, "name", "");
    mc.type                     = jsonStr(mj, "type", "openai");
    mc.baseUrl                  = jsonStr(mj, "baseUrl", "");
    mc.apiKey                   = jsonStr(mj, "apiKey", "EMPTY");
    mc.modelName                = jsonStr(mj, "modelName", "");
    mc.apiPath                  = jsonStr(mj, "apiPath", "");
    mc.connectTimeoutSeconds    = jsonInt(mj, "connectTimeoutSeconds", 16);
    mc.readChunkTimeoutSeconds  = jsonInt(mj, "readChunkTimeoutSeconds", 100);
    mc.maxConcurrentConnections = jsonInt(mj, "maxConcurrentConnections", size_t{5});
    mc.anthropicVersion         = jsonStr(mj, "anthropicVersion", "2023-06-01");
    mc.modelContenxtMaxToken    = jsonInt(mj, "modelContextMaxToken", size_t{0});
    mc.sendThinking             = jsonBool(mj, "sendThinking", false);
    if (mj.contains("sslVerify") && !mj["sslVerify"].is_null() && mj["sslVerify"].is_boolean()) {
        mc.sslVerify = mj["sslVerify"].get<bool>();
    }
    if (mj.contains("extraHeaders") && mj["extraHeaders"].is_object()) {
        for (const auto& [k, v] : mj["extraHeaders"].items()) {
            if (v.is_string()) {
                mc.extraHeaders[k] = v.get<std::string>();
            }
        }
    }
    if (mj.contains("extraConfig") && mj["extraConfig"].is_object()) {
        mc.extra_config = mj["extraConfig"];
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
    // 复用 CodeAgent 自带的 io_context (BaseAgent 构造函数创建)
    ioCtx_ = agent_->ioCtx;
    return true;
}

FfiAgentRuntime::~FfiAgentRuntime() {
    // 兜底: 若未显式 stop (异常路径), 保证 io 线程退出
    if (state() != State::Stopped && state() != State::Created) {
        if (!isOnAgentThread()) {
            stopInternal();
        }
    }
}

bool FfiAgentRuntime::isOnAgentThread() const {
    const auto tid = ioThread_.get_id();
    return tid != std::thread::id{} && std::this_thread::get_id() == tid;
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

int FfiAgentRuntime::start(std::string& err) {
    State expected = State::Created;
    if (!state_.compare_exchange_strong(expected, State::Starting)) {
        err = "状态错误: 仅 Created 状态可 start";
        return AGENTXX_ERR_STATE;
    }

    const auto agentEx = agent_->ioCtx->get_executor();

    // 进程内传输对 (client 端点 / 服务端点同 executor, 单 io 线程)
    auto [clientTrans, serverTrans] = agent::ChannelAgentIOTransport::makePair(agentEx, agentEx);

    // FFI client 端点
    clientIO_ = std::make_shared<FfiClientAgentIO>(agentEx, callbacks_);
    clientIO_->setThreadId(threadId_);
    clientIO_->setTransport(std::move(clientTrans));

    // 服务端点 (被 BaseAgent 驱动)
    agent::SessionServerAgentIO::Config scCfg;
    scCfg.threadId         = threadId_;
    scCfg.interruptTimeout = interruptTimeout_;
    serverIO_              = std::make_shared<agent::SessionServerAgentIO>(agentEx, agent_, scCfg);
    serverIO_->setTransport(std::move(serverTrans));

    // 启动进度 → 日志环形缓冲 (EVT_READY 前的启动过程日志, 可经 drain 取走)
    agent_->agentContext->startupNotifier = [this](std::string_view step) {
        pushLogItem(LogItem{2, fmt::format("[startup] {}", step)});
    };

    // 同步应答路由: io 线程收到 WireModelInfo/ContextMessages/SessionList 时完成等待方
    auto weakSelf          = std::weak_ptr<FfiAgentRuntime>{shared_from_this()};
    clientIO_->onSyncReply = [weakSelf](FfiClientAgentIO::SyncKind kind, neograph::json j) {
        if (auto sp = weakSelf.lock()) {
            sp->onSyncReplyOnIoThread(kind, std::move(j));
        }
    };

    // 接入日志分发器
    util::LogDispatcher::instance().addSink(logSink_);

    // work guard 必须先于 io 线程创建, 避免 run() 因事件队列为空立即返回
    workGuard_.emplace(asio::make_work_guard(*ioCtx_));
    ioThread_ = std::thread([this]() {
        ioCtx_->run();
    });
    clientIO_->setAgentThreadId(ioThread_.get_id());

    // 装配与启动协程 (io 线程执行)
    asio::post(*ioCtx_, [self = shared_from_this()]() {
        self->startOnIoThread();
    });

    return AGENTXX_OK;
}

void FfiAgentRuntime::startOnIoThread() {
    // 复刻 setupLocalUnifiedDirect (mode_runners.cpp):
    // 1) transport 接收循环先于 init() 启动 —— init 期间的客户端请求
    //    (WireHello/WireGetModel) 有消费方, 不排队积压
    asio::co_spawn(*ioCtx_, serverIO_->runTransportLoop(), asio::detached);
    // 2) 主协程: init → host → 组件信息 → ready → 会话驱动循环
    asio::co_spawn(
        *ioCtx_,
        [self = shared_from_this()]() -> asio::awaitable<void> {
            co_await self->runAgentMain();
        },
        asio::detached
    );
    // 3) client 接收循环 (事件分发到 C 回调)
    auto clientIO = clientIO_;
    asio::co_spawn(
        *ioCtx_,
        [clientIO]() -> asio::awaitable<void> {
            co_await clientIO->runTransportLoop();
        },
        asio::detached
    );
    // 4) hello 触发服务端全量同步/重放; 请求模型信息供 EVT_MODEL_INFO
    clientIO_->sendToPeer(agent::WireHello{threadId_, "", 0, ""});
    clientIO_->sendToPeer(agent::WireGetModel{threadId_});
}

asio::awaitable<void> FfiAgentRuntime::runAgentMain() {
    // init (含启动组件加载; 失败经 EVT_ERROR 上报并置 Failed 状态)
    const bool initOk = co_await util::catchErrorAsync<bool>(
        [self = shared_from_this()]() -> asio::awaitable<bool> {
            co_await self->agent_->init();
            co_return true;
        },
        [self = shared_from_this()](std::string errmsg) -> asio::awaitable<bool> {
            XX_LOGE("[ffi] agent init failed: {}", errmsg);
            self->clientIO_->notifyError(
                AGENTXX_ERR_INIT,
                fmt::format("agent init failed: {}", errmsg)
            );
            self->state_ = State::Failed;
            self->serverIO_->stop();
            co_return false;
        }
    );
    if (!initOk) {
        co_return;
    }

    // 宿主 (进程级): 子代理委派 (service.subagent 总线服务) 与根 agent 注册
    agentxx::agent::AgentHost::Config hostCfg;
    hostCfg.ioCtx = agent_->ioCtx;
    host_         = agentxx::agent::AgentHost::create(hostCfg);
    host_->attachRoot(agent_);

    // 启动组件信息 (init 完成后拉取; 响应经 EVT_COMPONENTS 通知)
    clientIO_->requestAppendComponentInfo(threadId_);

    // 通知客户端: 服务端就绪 (EVT_READY; 用户输入自此可被有效消费)。
    // 须先置 Ready 再通知: 回调内 (宿主收到 EVT_READY 后) 立即发起的
    // send_input 等操作依赖状态机允许投递, 避免竞态返回 ERR_STATE
    state_ = State::Ready;
    clientIO_->notifyServerReady();

    // 会话驱动循环: 消费用户输入 → runConversationTurnAsync → 推送事件
    // (持续运行直到 stop(); 停止后协程结束)
    co_await serverIO_->run();
}

void FfiAgentRuntime::stopInternal() {
    const State st = state_.load();
    if (st == State::Stopped) {
        return;
    }
    if (st == State::Created) {
        // 从未启动: 直接置 Stopped
        state_ = State::Stopped;
        return;
    }
    state_ = State::Stopping;

    // 1) 失败全部 FFI 侧挂起中断 (关闭 channel, 等待协程结束)
    clientIO_->failAllPendingInterrupts();

    // 2) 关闭 client 传输 (线程安全) → client 接收循环结束
    if (clientIO_->transport()) {
        clientIO_->transport()->close();
    }

    // 3) 停止服务端点 (dispatch 到 io 线程执行 stopImpl: 关闭输入 channel/
    //    取消轮次/失败 pending); run() 循环随之退出
    serverIO_->stop();

    // 4) 等待 run() 退出 (io 线程仍在运行, 处理 stopImpl)
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
    while (serverIO_->running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (serverIO_->running()) {
        XX_LOGW("[ffi] stop: serverIO run loop did not exit within 20s, forcing stop");
    }

    // 5) 摘除日志 sink / 释放 work guard / 停止并 join io 线程
    util::LogDispatcher::instance().removeSink(logSink_);
    workGuard_.reset();
    ioCtx_->stop();
    if (ioThread_.joinable()) {
        ioThread_.join();
    }
    state_ = State::Stopped;
}

int FfiAgentRuntime::stop(std::string& err) {
    if (isOnAgentThread()) {
        err = "不能在 agent io 线程 (事件回调) 内调用 stop; 请从宿主线程调用";
        return AGENTXX_ERR_STATE;
    }
    stopInternal();
    return AGENTXX_OK;
}

int FfiAgentRuntime::destroy(std::string& err) {
    if (isOnAgentThread()) {
        err = "不能在 agent io 线程 (事件回调) 内调用 destroy; 请从宿主线程调用";
        return AGENTXX_ERR_STATE;
    }
    stopInternal();
    return AGENTXX_OK;
}

// ---------------------------------------------------------------------------
// 会话交互 (投递 io 线程)
// ---------------------------------------------------------------------------

namespace {

/// 状态可用性检查: Starting/Ready/Failed 可投递 (Failed 仅允许清理类操作)
bool stateUsable(FfiAgentRuntime::State s) {
    return s == FfiAgentRuntime::State::Starting || s == FfiAgentRuntime::State::Ready;
}

} // namespace

int FfiAgentRuntime::sendInput(std::string_view text, std::string& err) {
    if (!stateUsable(state())) {
        err = "状态错误: 未启动或已停止";
        return AGENTXX_ERR_STATE;
    }
    if (text.empty()) {
        err = "输入文本为空";
        return AGENTXX_ERR_INVALID;
    }
    auto clientIO = clientIO_;
    auto tid      = threadId_;
    auto textStr  = std::string{text};
    // 与 TUI/CLI 同路径: WireUserInput → 服务端 inputChannel
    asio::post(*ioCtx_, [clientIO, tid = std::move(tid), text = std::move(textStr)]() mutable {
        clientIO->sendToPeer(agent::WireUserInput{std::move(tid), std::move(text)});
    });
    return AGENTXX_OK;
}

int FfiAgentRuntime::cancel(std::string& err) {
    if (!stateUsable(state())) {
        err = "状态错误: 未启动或已停止";
        return AGENTXX_ERR_STATE;
    }
    auto clientIO = clientIO_;
    auto tid      = threadId_;
    asio::post(*ioCtx_, [clientIO, tid = std::move(tid)]() mutable {
        clientIO->sendToPeer(agent::WireCancel{std::move(tid)});
    });
    return AGENTXX_OK;
}

int FfiAgentRuntime::selectModel(std::string_view modelName, std::string& err) {
    if (!stateUsable(state())) {
        err = "状态错误: 未启动或已停止";
        return AGENTXX_ERR_STATE;
    }
    if (modelName.empty()) {
        err = "模型名为空";
        return AGENTXX_ERR_INVALID;
    }
    auto clientIO = clientIO_;
    auto tid      = threadId_;
    auto model    = std::string{modelName};
    asio::post(*ioCtx_, [clientIO, tid = std::move(tid), model = std::move(model)]() mutable {
        clientIO->sendToPeer(agent::WireSelectModel{std::move(tid), std::move(model)});
    });
    return AGENTXX_OK;
}

int FfiAgentRuntime::setPermission(std::string_view path, int allow, int op, std::string& err) {
    if (!stateUsable(state())) {
        err = "状态错误: 未启动或已停止";
        return AGENTXX_ERR_STATE;
    }
    if (path.empty()) {
        err = "路径为空";
        return AGENTXX_ERR_INVALID;
    }
    auto clientIO = clientIO_;
    auto tid      = threadId_;
    auto pathStr  = std::string{path};
    asio::post(
        *ioCtx_,
        [clientIO, tid = std::move(tid), path = std::move(pathStr), allow, op]() mutable {
            clientIO->sendToPeer(agent::WireSetPermission{
                std::move(tid),
                std::move(path),
                allow != 0,
                static_cast<size_t>(op > 0 ? 1 : 0),
            });
        }
    );
    return AGENTXX_OK;
}

int FfiAgentRuntime::switchSession(std::string_view threadId, std::string& err) {
    if (!stateUsable(state())) {
        err = "状态错误: 未启动或已停止";
        return AGENTXX_ERR_STATE;
    }
    if (threadId.empty()) {
        err = "thread_id 为空";
        return AGENTXX_ERR_INVALID;
    }
    auto clientIO = clientIO_;
    auto newTid   = std::string{threadId};
    asio::post(*ioCtx_, [clientIO, newTid = std::move(newTid)]() mutable {
        clientIO->sendToPeer(agent::WireSwitchSession{std::move(newTid)});
    });
    return AGENTXX_OK;
}

// ---------------------------------------------------------------------------
// 同步查询
// ---------------------------------------------------------------------------

void FfiAgentRuntime::onSyncReplyOnIoThread(FfiClientAgentIO::SyncKind kind, neograph::json j) {
    std::shared_ptr<SyncWait> waiter;
    {
        std::lock_guard<std::mutex> lock(syncMutex_);
        auto&                       q = syncWaits_[kind];
        if (!q.empty()) {
            waiter = q.front();
            q.pop_front();
        }
    }
    if (waiter) {
        waiter->promise.set_value(j.dump());
    }
}

std::string FfiAgentRuntime::syncQuery(
    FfiClientAgentIO::SyncKind kind,
    std::function<void()>      send,
    std::string&               err
) {
    if (!stateUsable(state())) {
        err = "状态错误: 未启动或已停止";
        return {};
    }
    auto waiter = std::make_shared<SyncWait>();
    {
        std::lock_guard<std::mutex> lock(syncMutex_);
        syncWaits_[kind].push_back(waiter);
    }
    asio::post(*ioCtx_, std::move(send));

    constexpr auto kTimeout = std::chrono::seconds{10};
    auto           fut      = waiter->promise.get_future();
    if (fut.wait_for(kTimeout) != std::future_status::ready) {
        // 超时: 移除本等待器 (防止后续应答错配)
        std::lock_guard<std::mutex> lock(syncMutex_);
        auto&                       q = syncWaits_[kind];
        for (auto it = q.begin(); it != q.end(); ++it) {
            if (*it == waiter) {
                q.erase(it);
                break;
            }
        }
        err = "同步查询等待服务端响应超时 (10s)";
        return {};
    }
    return fut.get();
}

std::string FfiAgentRuntime::getModelInfo(std::string& err) {
    auto clientIO = clientIO_;
    auto tid      = threadId_;
    return syncQuery(
        FfiClientAgentIO::SyncKind::ModelInfo,
        [clientIO, tid = std::move(tid)]() mutable {
            clientIO->sendToPeer(agent::WireGetModel{std::move(tid)});
        },
        err
    );
}

std::string FfiAgentRuntime::getContextMessages(std::string& err) {
    auto clientIO = clientIO_;
    auto tid      = threadId_;
    return syncQuery(
        FfiClientAgentIO::SyncKind::ContextMessages,
        [clientIO, tid = std::move(tid)]() mutable {
            clientIO->sendToPeer(agent::WireGetContext{std::move(tid)});
        },
        err
    );
}

std::string FfiAgentRuntime::listSessions(std::string& err) {
    auto clientIO = clientIO_;
    return syncQuery(
        FfiClientAgentIO::SyncKind::SessionList,
        [clientIO]() mutable {
            clientIO->sendToPeer(agent::WireListSessions{});
        },
        err
    );
}

// ---------------------------------------------------------------------------
// HIL 中断
// ---------------------------------------------------------------------------

bool FfiAgentRuntime::hasPendingInterrupt(int64_t interruptId) const {
    return clientIO_ && clientIO_->hasPendingInterrupt(interruptId);
}

int FfiAgentRuntime::interruptRespond(
    int64_t      interruptId,
    const char*  valuesJson,
    std::string& err
) {
    if (!stateUsable(state())) {
        err = "状态错误: 未启动或已停止";
        return AGENTXX_ERR_STATE;
    }
    if (valuesJson == nullptr || *valuesJson == '\0') {
        err = "values_json 为空";
        return AGENTXX_ERR_INVALID;
    }
    if (!hasPendingInterrupt(interruptId)) {
        err = "中断 id 不存在或已应答/已过期";
        return AGENTXX_ERR_INTERRUPT;
    }
    neograph::json values;
    try {
        values = neograph::json::parse(valuesJson);
    } catch (const std::exception& e) {
        err = fmt::format("values_json 非法 JSON: {}", e.what());
        return AGENTXX_ERR_JSON;
    }
    if (!values.is_array()) {
        err = "values_json 须为 JSON 数组 (与 inputs 顺序对应)";
        return AGENTXX_ERR_JSON;
    }
    auto clientIO = clientIO_;
    asio::post(*ioCtx_, [clientIO, interruptId, values = std::move(values)]() mutable {
        clientIO->submitInterruptResponse(interruptId, std::move(values));
    });
    return AGENTXX_OK;
}

} // namespace ffi
} // namespace agentxx