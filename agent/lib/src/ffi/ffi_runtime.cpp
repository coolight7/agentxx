#include "ffi_runtime.h"

#include "agentxx/agent/agent_host.h"
#include "agentxx/agent/code_agent.h"
#include "agentxx/agent/config.h"
#include "agentxx/agent/io/channel_io_transport.h"
#include "agentxx/agent/io/session_server_agent_io.h"
#include "agentxx/agent/io/wire_protocol.h"
#include "agentxx/util/exception.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/post.hpp"
#include "asio/use_future.hpp"
#include "fmt/format.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"
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

std::string FfiAgentRuntime::drainLogs() {
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
        config->languageExplicit      = cfgJ.contains("language");
        // 开发者模式 (配置 JSON `devMode`): 只控制"记录数据"的收集
        config->devMode               = cfgJ.value("devMode", false);
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

        // HIL 中断等待宿主应答超时 (秒; 0=不限)
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
    mc.modelContextMaxToken    = mj.value("modelContextMaxToken", size_t{0});
    mc.sendThinking             = mj.value("sendThinking", false);
    // 多模态输入能力 (与 yaml 的 image_input/audio_input/video_input 同义):
    // 决定宿主能否上传图片/音频/视频, 经 WireModelInfo.capabilities 下发
    // (见 agentxx_ffi_get_model_info 与 EVT_MODEL_INFO); 字段名兼容驼峰与下划线
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
    clientIO_->onSyncReply = [weakSelf](FfiClientAgentIO::SyncKind kind, utilxx_base::Json j) {
        if (auto sp = weakSelf.lock()) {
            sp->onSyncReplyOnClientThread(kind, std::move(j));
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
        std::string curLang;
        {
            std::lock_guard<std::mutex> lock(self->langMutex_);
            curLang = self->language_;
        }
        self->clientIO_->sendToPeer(agent::WireHello{
            .sessionId = self->sessionId_,
            .token     = "",
            .lastSeq   = 0,
            .tailHash  = "",
            .model     = "",
            .language  = curLang,
        });
        self->clientIO_->sendToPeer(agent::WireGetModel{self->sessionId_});
    });

    return AGENTXX_FFI_OK;
}

asio::awaitable<void> FfiAgentRuntime::runAgentMain() {
    // init (含启动组件加载; 失败经 EVT_ERROR 上报并置 Failed 状态)
    const bool initOk = co_await agentxx::util::catchErrorAsync<bool>(
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
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (serverIO_->running()) {
            XX_LOGW("[ffi] stop: serverIO run loop did not exit within 20s, forcing stop");
        }
    }

    // 4) 摘除日志 sink
    utilxx_base::LogDispatcher::instance().removeSink(logSink_);

    // 5) 停止并 join Server-IO 线程
    //    插件关闭必须先于此完成: shutdownAsync 在 agent IO 线程上 stop →
    //    等 lease 归零 → destroy/dlclose。executor 一旦停止就只能走同步析构
    //    兜底 (实例保留 CloseFailed, 动态库不卸载)。
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

/// 检查附件是否超出单文件限额; 超限时返回可读原因 (哪一项、多大、上限多少)
std::string attachmentLimitError(const agentxx::agent::MediaAttachment& att) {
    const uint64_t maxSize = agentxx::agent::maxBytesForMediaType(att.type);
    const uint64_t estSize = agentxx::agent::estimateAttachmentSizeBytes(att);
    if (estSize <= maxSize) {
        return {};
    }
    std::string name = !att.displayName.empty()
                           ? att.displayName
                           : (!att.pathOrUrl.empty() ? att.pathOrUrl : "attachment");
    return fmt::format(
        "附件 '{}' 超出 {} 类型的大小上限 ({} 字节 > {} 字节)",
        name,
        agentxx::agent::mediaTypeToString(att.type),
        estSize,
        maxSize
    );
}

} // namespace

int FfiAgentRuntime::sendInput(std::string_view inputJson, std::string& err) {
    if (!stateUsable(state())) {
        err = "状态错误: 未启动或已停止";
        return AGENTXX_FFI_ERR_STATE;
    }

    // 输入 JSON 解析: text 与 attachments 至少有一个非空 (附件走服务端加载/编码路径)
    utilxx_base::Json j;
    try {
        j = utilxx_base::Json::parse(inputJson);
    } catch (const std::exception& e) {
        err = fmt::format("input_json JSON 解析失败: {}", e.what());
        return AGENTXX_FFI_ERR_JSON;
    }
    if (!j.is_object()) {
        err = "input_json 必须是 JSON 对象, 形如 {\"text\":\"...\",\"attachments\":[...]}";
        return AGENTXX_FFI_ERR_INVALID;
    }

    agent::WireUserInput input;
    input.text      = j.value("text", std::string{});
    input.model     = j.value("model", std::string{});
    input.delivery  = j.value("delivery", std::string{});
    input.sessionId = sessionId_;
    // 受理回执照样经 EVT_WIRE 透出: 服务端只在 requestId > 0 时回 input_ack
    input.requestId = nextInputRequestId_.fetch_add(1, std::memory_order_relaxed) + 1;

    if (j.contains("attachments") && j["attachments"].is_array()) {
        for (const auto& item : j["attachments"]) {
            // fromJson 同时认下划线与驼峰/别名写法 (与 wire user_input.attachments 一致)
            input.attachments.push_back(agent::MediaAttachment::fromJson(item));
        }
    } else if (j.contains("attachments") && !j["attachments"].is_null()) {
        err = "input_json.attachments 必须是数组";
        return AGENTXX_FFI_ERR_INVALID;
    }

    if (input.text.empty() && input.attachments.empty()) {
        err = "输入为空: text 与 attachments 至少需要一个非空";
        return AGENTXX_FFI_ERR_INVALID;
    }
    // 条数与大小在本地先拦一次 (与 wire 服务端同一套约定): 参数写错立刻同步报错,
    // 不让宿主等到 input_ack(rejected) 才发现
    if (input.attachments.size() > agent::kMaxAttachmentsPerMessage) {
        err = fmt::format(
            "附件条数超出上限: {} > {} (单条消息)",
            input.attachments.size(),
            agent::kMaxAttachmentsPerMessage
        );
        return AGENTXX_FFI_ERR_INVALID;
    }
    for (const auto& att : input.attachments) {
        auto limitErr = attachmentLimitError(att);
        if (!limitErr.empty()) {
            err = std::move(limitErr);
            return AGENTXX_FFI_ERR_INVALID;
        }
    }

    auto clientIO = clientIO_;
    asio::post(
        *clientIoCtx_,
        [clientIO, input = std::move(input)]() mutable {
            clientIO->sendToPeer(std::move(input));
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

int FfiAgentRuntime::setLanguage(std::string_view language, std::string& err) {
    if (!stateUsable(state())) {
        err = "状态错误: 未启动或已停止";
        return AGENTXX_FFI_ERR_STATE;
    }
    auto norm = agent::normalizeLanguage(language);
    {
        std::lock_guard<std::mutex> lock(langMutex_);
        language_ = norm;
    }
    if (agent_ && serverIoCtx_) {
        auto ag  = agent_;
        auto sid = sessionId_;
        asio::post(*serverIoCtx_, [ag, sid, norm]() {
            ag->setLanguage(norm, sid);
        });
    }
    return AGENTXX_FFI_OK;
}

std::string FfiAgentRuntime::getLanguage(std::string& err) {
    std::lock_guard<std::mutex> lock(langMutex_);
    return language_.empty() ? "en" : language_;
}

// ---------------------------------------------------------------------------
// 同步查询
// ---------------------------------------------------------------------------

void FfiAgentRuntime::onSyncReplyOnClientThread(
    FfiClientAgentIO::SyncKind kind,
    utilxx_base::Json          j
) {
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

// -------------------------------------------------------------------
// 能力清单 (A6)
// -------------------------------------------------------------------

std::string FfiAgentRuntime::getCapabilities() {
    utilxx_base::Json j = {
        {"apiVersion",     AGENTXX_FFI_API_VERSION},
        {"libraryVersion", "0.4.0"},
        {"capabilities",   {
            AGENTXX_FFI_CAP_ADD_MODEL,
            AGENTXX_FFI_CAP_REMOVE_MODEL,
            AGENTXX_FFI_CAP_LIST_MODELS,
            AGENTXX_FFI_CAP_MESSAGE_QUEUE,
            AGENTXX_FFI_CAP_VIEW_MESSAGES,
            AGENTXX_FFI_CAP_HOST_TOOLS,
            AGENTXX_FFI_CAP_PLUGIN_DATA_UP,
            AGENTXX_FFI_CAP_WIRE_PASSTHROUGH,
            AGENTXX_FFI_CAP_DELTA_BATCH,
        }}
    };
    return j.dump();
}

// -------------------------------------------------------------------
// 模型管理 (A1)
// -------------------------------------------------------------------

int FfiAgentRuntime::addModel(std::string_view modelJson, std::string& err) {
    if (!stateUsable(state())) {
        err = "状态错误: 未启动或已停止";
        return AGENTXX_FFI_ERR_STATE;
    }
    utilxx_base::Json j;
    try {
        j = utilxx_base::Json::parse(modelJson);
    } catch (const std::exception& e) {
        err = fmt::format("model_json JSON 解析失败: {}", e.what());
        return AGENTXX_FFI_ERR_JSON;
    }
    if (!j.is_object() || !j.contains("name") || !j["name"].is_string()
        || j["name"].get<std::string>().empty()) {
        err = "model_json 格式错误: 缺少必填字段 name";
        return AGENTXX_FFI_ERR_CONFIG;
    }

    auto req      = agentxx::agent::io::addModelFromJson(j);
    req.sessionId = sessionId_;

    auto resStr = syncQuery(
        FfiClientAgentIO::SyncKind::AddModelResult,
        [this, req = std::move(req)]() mutable {
            clientIO_->sendToPeer(std::move(req));
        },
        err
    );
    if (resStr.empty()) {
        return AGENTXX_FFI_ERR_TIMEOUT;
    }
    try {
        auto resJ = utilxx_base::Json::parse(resStr);
        if (!resJ.value("ok", false)) {
            err = resJ.value("error", "添加模型失败");
            return AGENTXX_FFI_ERR_CONFIG;
        }
    } catch (...) {
        err = "解析添加模型回执失败";
        return AGENTXX_FFI_ERR_INTERNAL;
    }
    return AGENTXX_FFI_OK;
}

int FfiAgentRuntime::removeModel(std::string_view modelName, std::string& err) {
    if (!stateUsable(state())) {
        err = "状态错误: 未启动或已停止";
        return AGENTXX_FFI_ERR_STATE;
    }
    if (modelName.empty()) {
        err = "model_name 不能为空";
        return AGENTXX_FFI_ERR_INVALID;
    }

    agent::WireRemoveModel req{
        .sessionId = std::string(sessionId_),
        .name      = std::string(modelName),
    };

    auto resStr = syncQuery(
        FfiClientAgentIO::SyncKind::RemoveModelResult,
        [this, req = std::move(req)]() mutable {
            clientIO_->sendToPeer(std::move(req));
        },
        err
    );
    if (resStr.empty()) {
        return AGENTXX_FFI_ERR_TIMEOUT;
    }
    try {
        auto resJ = utilxx_base::Json::parse(resStr);
        if (!resJ.value("ok", false)) {
            err = resJ.value("error", "删除模型失败");
            return AGENTXX_FFI_ERR_CONFIG;
        }
    } catch (...) {
        err = "解析删除模型回执失败";
        return AGENTXX_FFI_ERR_INTERNAL;
    }
    return AGENTXX_FFI_OK;
}

std::string FfiAgentRuntime::listModels(std::string& err) {
    return getModelInfo(err);
}

// -------------------------------------------------------------------
// Wire 透传 (A2)
// -------------------------------------------------------------------

int FfiAgentRuntime::sendWire(std::string_view wireJson, std::string& err) {
    if (!stateUsable(state())) {
        err = "状态错误: 未启动或已停止";
        return AGENTXX_FFI_ERR_STATE;
    }
    utilxx_base::Json j;
    try {
        j = utilxx_base::Json::parse(wireJson);
    } catch (const std::exception& e) {
        err = fmt::format("wire_json JSON 解析失败: {}", e.what());
        return AGENTXX_FFI_ERR_JSON;
    }
    if (!j.is_object() || !j.contains("type") || !j["type"].is_string()) {
        err = "wire_json 格式错误: 缺少必填字段 type";
        return AGENTXX_FFI_ERR_INVALID;
    }

    const std::string msgType = j["type"].get<std::string>();

    // 黑名单拦截
    if (msgType == "hello" || msgType == "ping" || msgType == "pong") {
        err = "核心连接消息禁止由 send_wire 发送";
        return AGENTXX_FFI_ERR_INVALID;
    }
    if (msgType == "user_input") {
        err = "发送用户输入请使用专用接口 agentxx_ffi_send_input";
        return AGENTXX_FFI_ERR_INVALID;
    }
    if (msgType == "cancel") {
        err = "取消当前轮次请使用专用接口 agentxx_ffi_cancel";
        return AGENTXX_FFI_ERR_INVALID;
    }
    if (msgType == "select_model") {
        err = "切换当前模型请使用专用接口 agentxx_ffi_select_model";
        return AGENTXX_FFI_ERR_INVALID;
    }
    if (msgType == "switch_session") {
        err = "切换当前会话请使用专用接口 agentxx_ffi_switch_session";
        return AGENTXX_FFI_ERR_INVALID;
    }
    if (msgType == "interrupt_response") {
        err = "提交中断应答请使用专用接口 agentxx_ffi_interrupt_respond";
        return AGENTXX_FFI_ERR_INVALID;
    }
    if (msgType == "add_model") {
        err = "添加模型配置请使用专用接口 agentxx_ffi_add_model";
        return AGENTXX_FFI_ERR_INVALID;
    }
    if (msgType == "remove_model") {
        err = "删除模型配置请使用专用接口 agentxx_ffi_remove_model";
        return AGENTXX_FFI_ERR_INVALID;
    }
    if (msgType == "get_model" || msgType == "get_context" || msgType == "list_sessions") {
        err = "同步查询请使用对应的专用查询符号 (如 agentxx_ffi_get_model_info 等)";
        return AGENTXX_FFI_ERR_INVALID;
    }

    // 白名单放行校验
    static const std::set<std::string, std::less<>> s_whitelist = {
        "compact_context",
        "get_view_messages",
        "list_dir",
        "rename_session",
        "set_full_auth",
        "get_permission_state",
        "clear_message_queue",
        "remove_queue_item",
        "interrupt_and_run_next",
        "plugin_data_up",
        "host_tool_register",
        "host_tool_unregister",
        "host_tool_result",
    };

    if (!s_whitelist.contains(msgType)) {
        err = fmt::format("消息类型 '{}' 不在白名单允许范围内", msgType);
        return AGENTXX_FFI_ERR_INVALID;
    }

    // 如果未带 sessionId, 自动补齐为当前绑定 sessionId
    if (!j.contains("sessionId") || j["sessionId"].is_null()
        || (j["sessionId"].is_string() && j["sessionId"].get<std::string>().empty())) {
        j["sessionId"] = sessionId_;
    }

    auto wireMsg = agentxx::agent::io::deserialize(j.dump());
    if (!wireMsg.has_value()) {
        err = fmt::format("线消息反序列化失败: type={}", msgType);
        return AGENTXX_FFI_ERR_INVALID;
    }

    asio::post(*clientIoCtx_, [self = shared_from_this(), msg = std::move(*wireMsg)]() mutable {
        self->clientIO_->sendToPeer(std::move(msg));
    });
    return AGENTXX_FFI_OK;
}

// -------------------------------------------------------------------
// 宿主工具 (A3)
// -------------------------------------------------------------------

int FfiAgentRuntime::toolRegister(std::string_view toolJson, std::string& err) {
    if (!stateUsable(state())) {
        err = "状态错误: 未启动或已停止";
        return AGENTXX_FFI_ERR_STATE;
    }
    utilxx_base::Json j;
    try {
        j = utilxx_base::Json::parse(toolJson);
    } catch (const std::exception& e) {
        err = fmt::format("tool_json JSON 解析失败: {}", e.what());
        return AGENTXX_FFI_ERR_JSON;
    }

    agent::WireHostToolRegister reg;
    reg.sessionId = sessionId_;

    auto parseOneTool = [](const utilxx_base::Json& item) -> std::optional<agent::WireHostToolInfo> {
        if (!item.is_object() || !item.contains("name") || !item["name"].is_string()) {
            return std::nullopt;
        }
        agent::WireHostToolInfo info;
        info.name        = item["name"].get<std::string>();
        info.description = item.value("description", std::string{});
        if (item.contains("inputSchema")) {
            info.inputSchema = item["inputSchema"];
        }
        info.timeoutSec    = item.value("timeoutSec", uint32_t{0});
        info.maxConcurrent = item.value("maxConcurrent", uint32_t{0});
        return info;
    };

    if (j.is_array()) {
        for (const auto& item : j) {
            if (auto t = parseOneTool(item)) {
                reg.tools.push_back(std::move(*t));
            }
        }
    } else if (j.is_object()) {
        if (j.contains("tools") && j["tools"].is_array()) {
            for (const auto& item : j["tools"]) {
                if (auto t = parseOneTool(item)) {
                    reg.tools.push_back(std::move(*t));
                }
            }
        } else {
            if (auto t = parseOneTool(j)) {
                reg.tools.push_back(std::move(*t));
            }
        }
    }

    if (reg.tools.empty()) {
        err = "tool_json 未包含有效的工具定义 (需 name 字段)";
        return AGENTXX_FFI_ERR_INVALID;
    }

    asio::post(*clientIoCtx_, [self = shared_from_this(), req = std::move(reg)]() mutable {
        self->clientIO_->sendToPeer(std::move(req));
    });
    return AGENTXX_FFI_OK;
}

int FfiAgentRuntime::toolUnregister(std::string_view name, std::string& err) {
    if (!stateUsable(state())) {
        err = "状态错误: 未启动或已停止";
        return AGENTXX_FFI_ERR_STATE;
    }
    if (name.empty()) {
        err = "工具名称不能为空";
        return AGENTXX_FFI_ERR_INVALID;
    }
    agent::WireHostToolUnregister unreg{
        .sessionId = std::string(sessionId_),
        .names     = {std::string(name)},
    };
    asio::post(*clientIoCtx_, [self = shared_from_this(), req = std::move(unreg)]() mutable {
        self->clientIO_->sendToPeer(std::move(req));
    });
    return AGENTXX_FFI_OK;
}

int FfiAgentRuntime::toolRespond(
    int64_t          callId,
    int32_t          isError,
    std::string_view resultJson,
    std::string&     err
) {
    if (!stateUsable(state())) {
        err = "状态错误: 未启动或已停止";
        return AGENTXX_FFI_ERR_STATE;
    }
    auto str = std::string(resultJson);
    asio::post(
        *clientIoCtx_,
        [self = shared_from_this(), callId, isError, str = std::move(str)]() mutable {
            self->clientIO_->submitHostToolResponse(callId, isError, std::move(str));
        }
    );
    return AGENTXX_FFI_OK;
}

// -------------------------------------------------------------------
// 文本合批 (A11)
// -------------------------------------------------------------------

int FfiAgentRuntime::setDeltaBatch(int32_t maxDelayMs, std::string& err) {
    if (!stateUsable(state())) {
        err = "状态错误: 未启动或已停止";
        return AGENTXX_FFI_ERR_STATE;
    }
    asio::post(*clientIoCtx_, [self = shared_from_this(), maxDelayMs]() {
        self->clientIO_->setDeltaBatch(maxDelayMs);
    });
    return AGENTXX_FFI_OK;
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
    utilxx_base::Json val   = utilxx_base::Json::object();
    auto              valSv = toSv(valuesJson);
    if (!valSv.empty()) {
        try {
            val = utilxx_base::Json::parse(valSv);
        } catch (const std::exception& e) {
            err = fmt::format("valuesJson 非法 JSON: {}", e.what());
            return AGENTXX_FFI_ERR_JSON;
        }
    }
    // 应答载荷恒为对象形态 {"values": {"<控件 id>": 值}} (见
    // agentxx::middleware::makeInterruptResult); 非对象形态直接拒绝,
    // 避免契约外的载荷被静默当成"空应答"导致权限被误判为拒绝。
    // (参数校验先于中断 id 校验: 载荷错误与 id 状态无关, 报错更确定)
    if (!val.is_object() || !val.contains("values") || !val["values"].is_object()) {
        err = "valuesJson 须为 {\"values\":{\"<控件 id>\":值}} 对象形态";
        return AGENTXX_FFI_ERR_INVALID;
    }
    if (!hasPendingInterrupt(interruptId)) {
        err = fmt::format("中断 #{} 不存在、已应答或已过期", interruptId);
        return AGENTXX_FFI_ERR_INTERRUPT;
    }
    auto clientIO = clientIO_;
    asio::post(*clientIoCtx_, [clientIO, interruptId, val = std::move(val)]() mutable {
        clientIO->submitInterruptResponse(interruptId, std::move(val));
    });
    return AGENTXX_FFI_OK;
}

} // namespace ffi
} // namespace agentxx
