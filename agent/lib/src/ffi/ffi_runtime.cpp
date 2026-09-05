#include "ffi_runtime.h"

#include "agentxx/agent/agent_host.h"
#include "agentxx/agent/code_agent.h"
#include "agentxx/agent/config.h"
#include "agentxx/agent/io/channel_io_transport.h"
#include "agentxx/agent/io/session_server_agent_io.h"
#include "agentxx/agent/io/wire_protocol.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/post.hpp"
#include "fmt/format.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
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

std::string_view toSv(const AgentxxStringView* sv) {
    if (sv == nullptr || sv->data == nullptr || sv->size == 0) {
        return {};
    }
    return std::string_view{sv->data, static_cast<size_t>(sv->size)};
}

} // namespace

// ---------------------------------------------------------------------------
// 日志
// ---------------------------------------------------------------------------

void FfiAgentRuntime::FfiLogSink::onLog(const util::LogEntry& entry) {
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

std::string FfiAgentRuntime::drainLogs() {
    std::deque<LogItem> drained;
    {
        std::lock_guard<std::mutex> lock(logMutex_);
        drained.swap(logRing_);
    }
    neograph::json arr = neograph::json::array();
    for (const auto& item : drained) {
        neograph::json entry;
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
    neograph::json cfgJ;
    auto           cfgSv = toSv(config_json);
    if (!cfgSv.empty()) {
        try {
            cfgJ = neograph::json::parse(cfgSv);
        } catch (const std::exception& e) {
            err = fmt::format("config_json 非法 JSON: {}", e.what());
            return false;
        }
        config->dataDir = jsonStr(cfgJ, "dataDir", "");
        // 会话工作目录: 相对路径/`~` 在此按进程 cwd 展开为绝对路径
        // (嵌入多实例场景下各句柄可绑定独立项目目录, 见 AgentConfig::workDir)
        {
            auto workDir = agentxx::util::expandUserHomePath(jsonStr(cfgJ, "workDir", ""));
            if (!workDir.empty()) {
                std::filesystem::path wp{workDir};
                config->workDir = wp.is_absolute() ? wp.lexically_normal().generic_string()
                                                   : (std::filesystem::current_path() / wp)
                                                         .lexically_normal()
                                                         .generic_string();
            }
        }
        config->enableSessionStore    = jsonBool(cfgJ, "enableSessionStore", false);
        config->sessionStoreDirectory = jsonStr(cfgJ, "sessionStoreDirectory", "");
        config->agentName             = jsonStr(cfgJ, "agentName", config->agentName);
        config->llmMaxRetry           = jsonInt(cfgJ, "llmMaxRetry", config->llmMaxRetry);
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
    auto           modelSv = toSv(model_json);
    if (!modelSv.empty()) {
        try {
            mj = neograph::json::parse(modelSv);
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

FfiAgentRuntime::~FfiAgentRuntime() {
    // 兜底: 若未显式 stop, 保证内部线程与所有资源安全退出与释放
    if (state() != State::Stopped && state() != State::Created) {
        if (!isOnAnyIoThread()) {
            stopInternal();
        }
    } else {
        // Created 状态直接清理对象
        clientIO_.reset();
        serverIO_.reset();
        host_.reset();
        agent_.reset();
    }
}

bool FfiAgentRuntime::isOnAgentThread() const {
    const auto tid = serverThread_.get_id();
    return tid != std::thread::id{} && std::this_thread::get_id() == tid;
}

bool FfiAgentRuntime::isOnClientThread() const {
    const auto tid = clientThread_.get_id();
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

    clientIoCtx_        = std::make_shared<asio::io_context>();
    const auto agentEx  = serverIoCtx_->get_executor();
    const auto clientEx = clientIoCtx_->get_executor();

    // 进程内传输对 (跨 Client-IO 线程与 Server-IO 线程)
    auto [clientTrans, serverTrans] = agent::ChannelAgentIOTransport::makePair(clientEx, agentEx);

    // FFI client 端点 (绑定 clientEx)
    clientIO_ = std::make_shared<FfiClientAgentIO>(clientEx, callbacks_);
    clientIO_->setSessionId(sessionId_);
    clientIO_->setTransport(std::move(clientTrans));

    // 服务端点 (绑定 agentEx, 被 BaseAgent 驱动)
    agent::SessionServerAgentIO::Config scCfg;
    scCfg.sessionId        = sessionId_;
    scCfg.interruptTimeout = interruptTimeout_;
    serverIO_              = std::make_shared<agent::SessionServerAgentIO>(agentEx, agent_, scCfg);
    serverIO_->setTransport(std::move(serverTrans));

    // 启动进度 → 日志环形缓冲
    agent_->agentContext->initNotifier = [this](std::string_view step) {
        pushLogItem(LogItem{2, fmt::format("[startup] {}", step)});
    };

    // 同步应答路由: client io 线程收到 Wire 响应时完成对应 promise
    auto weakSelf          = std::weak_ptr<FfiAgentRuntime>{shared_from_this()};
    clientIO_->onSyncReply = [weakSelf](FfiClientAgentIO::SyncKind kind, neograph::json j) {
        if (auto sp = weakSelf.lock()) {
            sp->onSyncReplyOnClientThread(kind, std::move(j));
        }
    };

    // 接入日志分发器
    util::LogDispatcher::instance().addSink(logSink_);

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

    // 1) Server-IO 线程协程: server 接收循环 + init / main
    asio::post(*serverIoCtx_, [self = shared_from_this()]() {
        // 先启动 server 接收循环 (init 期间请求如 WireHello/WireGetModel 不排队)
        asio::co_spawn(*self->serverIoCtx_, self->serverIO_->runTransportLoop(), asio::detached);
        // 主协程: init → host → ready → serverIO->run()
        asio::co_spawn(
            *self->serverIoCtx_,
            [self]() -> asio::awaitable<void> {
                co_await self->runAgentMain();
            },
            asio::detached
        );
    });

    // 2) Client-IO 线程协程: client 接收循环 + hello
    asio::post(*clientIoCtx_, [self = shared_from_this()]() {
        asio::co_spawn(*self->clientIoCtx_, self->clientIO_->runTransportLoop(), asio::detached);
        self->clientIO_->sendToPeer(agent::WireHello{self->sessionId_, "", 0, ""});
        self->clientIO_->sendToPeer(agent::WireGetModel{self->sessionId_});
    });

    return AGENTXX_FFI_OK;
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
                AGENTXX_FFI_ERR_INIT,
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

    // 启动组件信息 (跨线程请求 Client 端点发送 WireAppendComponentInfo)
    asio::post(*clientIoCtx_, [clientIO = clientIO_, sid = sessionId_]() {
        clientIO->requestAppendComponentInfo(sid);
    });

    // 通知客户端: 服务端就绪 (EVT_READY; 先置 Ready 状态避免回调内操作遇到 ERR_STATE)
    state_ = State::Ready;
    clientIO_->notifyServerReady();

    // 会话驱动循环: 处理用户输入 → runTurnAsync → 推送事件
    co_await serverIO_->run();
}

void FfiAgentRuntime::stopInternal() {
    const State st = state_.load();
    if (st == State::Stopped) {
        return;
    }
    if (st == State::Created) {
        state_ = State::Stopped;
        return;
    }
    state_ = State::Stopping;

    // 1) Client 侧: 中止挂起中断并关闭 client transport
    if (clientIO_) {
        clientIO_->failAllPendingInterrupts();
        if (clientIO_->transport()) {
            clientIO_->transport()->close();
        }
    }

    // 2) Server 侧: 停止服务端点 (dispatch 到 agent io 线程执行 stopImpl)
    if (serverIO_) {
        serverIO_->stop();
    }

    // 3) 等待 serverIO run() 退出 (最长 20s)
    if (serverIO_) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
        while (serverIO_->running() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (serverIO_->running()) {
            XX_LOGW("[ffi] stop: serverIO run loop did not exit within 20s, forcing stop");
        }
    }

    // 4) 摘除日志 sink
    util::LogDispatcher::instance().removeSink(logSink_);

    // 5) 停止并 join Server-IO 线程
    if (serverIoCtx_) {
        serverWorkGuard_.reset();
        serverIoCtx_->stop();
    }
    if (serverThread_.joinable()) {
        serverThread_.join();
    }

    // 6) 停止并 join Client-IO 线程
    if (clientIoCtx_) {
        clientWorkGuard_.reset();
        clientIoCtx_->stop();
    }
    if (clientThread_.joinable()) {
        clientThread_.join();
    }

    // 7) 取消所有残留的同步查询等待
    {
        std::lock_guard<std::mutex> lock(syncMutex_);
        for (auto& [kind, q] : syncWaits_) {
            for (auto& w : q) {
                try {
                    w->promise.set_value("{}");
                } catch (...) {
                }
            }
            q.clear();
        }
    }

    // 8) 显式释放所有持有 transport/channel 的对象, 确保它们在 ioCtx 析构前完成清理
    clientIO_.reset();
    serverIO_.reset();
    host_.reset();
    agent_.reset();

    state_ = State::Stopped;
}

int FfiAgentRuntime::stop(std::string& err) {
    if (isOnAnyIoThread()) {
        err = "不能在 agent/client io 线程 (事件回调) 内调用 stop; 请从宿主线程调用";
        return AGENTXX_FFI_ERR_STATE;
    }
    stopInternal();
    return AGENTXX_FFI_OK;
}

int FfiAgentRuntime::destroy(std::string& err) {
    if (isOnAnyIoThread()) {
        err = "不能在 agent/client io 线程 (事件回调) 内调用 destroy; 请从宿主线程调用";
        return AGENTXX_FFI_ERR_STATE;
    }
    stopInternal();
    return AGENTXX_FFI_OK;
}

// ---------------------------------------------------------------------------
// 会话交互 (投递 client io 线程)
// ---------------------------------------------------------------------------

namespace {

bool stateUsable(FfiAgentRuntime::State s) {
    return s == FfiAgentRuntime::State::Starting || s == FfiAgentRuntime::State::Ready;
}

} // namespace

int FfiAgentRuntime::sendInput(std::string_view text, std::string& err) {
    if (!stateUsable(state())) {
        err = "状态错误: 未启动或已停止";
        return AGENTXX_FFI_ERR_STATE;
    }
    if (text.empty()) {
        err = "输入文本为空";
        return AGENTXX_FFI_ERR_INVALID;
    }
    auto clientIO = clientIO_;
    auto tid      = sessionId_;
    auto textStr  = std::string{text};
    asio::post(
        *clientIoCtx_,
        [clientIO, tid = std::move(tid), text = std::move(textStr)]() mutable {
            clientIO->sendToPeer(agent::WireUserInput{std::move(tid), std::move(text)});
        }
    );
    return AGENTXX_FFI_OK;
}

int FfiAgentRuntime::cancel(std::string& err) {
    if (!stateUsable(state())) {
        err = "状态错误: 未启动或已停止";
        return AGENTXX_FFI_ERR_STATE;
    }
    auto clientIO = clientIO_;
    auto tid      = sessionId_;
    asio::post(*clientIoCtx_, [clientIO, tid = std::move(tid)]() mutable {
        clientIO->sendToPeer(agent::WireCancel{std::move(tid)});
    });
    return AGENTXX_FFI_OK;
}

int FfiAgentRuntime::selectModel(std::string_view modelName, std::string& err) {
    if (!stateUsable(state())) {
        err = "状态错误: 未启动或已停止";
        return AGENTXX_FFI_ERR_STATE;
    }
    if (modelName.empty()) {
        err = "模型名为空";
        return AGENTXX_FFI_ERR_INVALID;
    }
    auto clientIO = clientIO_;
    auto tid      = sessionId_;
    auto model    = std::string{modelName};
    asio::post(*clientIoCtx_, [clientIO, tid = std::move(tid), model = std::move(model)]() mutable {
        clientIO->sendToPeer(agent::WireSelectModel{std::move(tid), std::move(model)});
    });
    return AGENTXX_FFI_OK;
}

int FfiAgentRuntime::setPermission(std::string_view path, int allow, int op, std::string& err) {
    if (!stateUsable(state())) {
        err = "状态错误: 未启动或已停止";
        return AGENTXX_FFI_ERR_STATE;
    }
    if (path.empty()) {
        err = "路径为空";
        return AGENTXX_FFI_ERR_INVALID;
    }
    auto clientIO = clientIO_;
    auto tid      = sessionId_;
    auto pathStr  = std::string{path};
    asio::post(
        *clientIoCtx_,
        [clientIO, tid = std::move(tid), path = std::move(pathStr), allow, op]() mutable {
            clientIO->sendToPeer(agent::WireSetPermission{
                std::move(tid),
                std::move(path),
                allow != 0,
                static_cast<size_t>(op > 0 ? 1 : 0),
            });
        }
    );
    return AGENTXX_FFI_OK;
}

int FfiAgentRuntime::switchSession(std::string_view sessionId, std::string& err) {
    if (!stateUsable(state())) {
        err = "状态错误: 未启动或已停止";
        return AGENTXX_FFI_ERR_STATE;
    }
    if (sessionId.empty()) {
        err = "sessionId 为空";
        return AGENTXX_FFI_ERR_INVALID;
    }
    auto clientIO = clientIO_;
    auto newTid   = std::string{sessionId};
    asio::post(*clientIoCtx_, [this, clientIO, newTid]() mutable {
        sessionId_ = newTid;
        clientIO->setSessionId(newTid);
        clientIO->sendToPeer(agent::WireSwitchSession{std::move(newTid)});
    });
    return AGENTXX_FFI_OK;
}

// ---------------------------------------------------------------------------
// 同步查询
// ---------------------------------------------------------------------------

void FfiAgentRuntime::onSyncReplyOnClientThread(FfiClientAgentIO::SyncKind kind, neograph::json j) {
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
    asio::post(*clientIoCtx_, std::move(send));

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
    auto tid      = sessionId_;
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
    auto tid      = sessionId_;
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
    if (!clientIO_) {
        return false;
    }
    return clientIO_->hasPendingInterrupt(interruptId);
}

int FfiAgentRuntime::interruptRespond(
    int64_t                  interruptId,
    const AgentxxStringView* valuesJson,
    std::string&             err
) {
    if (!stateUsable(state())) {
        err = "状态错误: 未启动或已停止";
        return AGENTXX_FFI_ERR_STATE;
    }
    if (!hasPendingInterrupt(interruptId)) {
        err = fmt::format("中断 #{} 不存在、已应答或已过期", interruptId);
        return AGENTXX_FFI_ERR_INTERRUPT;
    }
    neograph::json val   = neograph::json::array();
    auto           valSv = toSv(valuesJson);
    if (!valSv.empty()) {
        try {
            val = neograph::json::parse(valSv);
        } catch (const std::exception& e) {
            err = fmt::format("valuesJson 非法 JSON: {}", e.what());
            return AGENTXX_FFI_ERR_JSON;
        }
    }
    auto clientIO = clientIO_;
    asio::post(*clientIoCtx_, [clientIO, interruptId, val = std::move(val)]() mutable {
        clientIO->submitInterruptResponse(interruptId, std::move(val));
    });
    return AGENTXX_FFI_OK;
}

} // namespace ffi
} // namespace agentxx
