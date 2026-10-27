/// 插件运行时: 根 / 通道 / 输入与查询 (接口表 `agentxx.agent.runtime`)
///
/// 三层的职责分工 (与设计文档 §1 一致):
/// - **根**: 挂到进程容器上的 agent, 各有一份 `AgentConfig` (提示词/工具白名单/插件挂载);
///   建根只能发生在 `start` 这类进程级时机 (在 `mount` 回调里建会被拒, 否则无限嵌套);
/// - **通道**: 一个 `sessionId` 的驱动壳 —— 把 [AgentSessionRuntime] 的两端接起来,
///   把服务端事件翻成通道事件交给插件, 并把插件的输入/取消/应答转回 wire;
/// - **触发器 / 定时器**: 把事件或时间转成通道输入 (实现见 plugin_manager_runtime_trigger.cpp)。
///
/// 线程约定: 本文件的全部入口都只在主程序 IO 线程执行 (vtable 层经 `ioCallSyncKeep`
/// 投递), 因此记录表不需要锁。事件回调也在同一条线程 (通道的 client 端就用主程序 io
/// 执行器), 插件回调里不得同步阻塞。
///
/// 生命周期: 对象的归属者是插件实例名 (`owner`)。插件禁用/卸载/关闭时按 owner 整批
/// 回收 (见 [PluginManager::releaseRuntimeOfOwner]) —— 不依赖插件自己记得关。
#include "agentxx/plugin/plugin_manager.h"

#include "agentxx/agent/agent_host.h"
#include "agentxx/agent/base_agent.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/io/embedded_client_agent_io.h"
#include "agentxx/agent/io/user_input_json.h"
#include "agentxx/agent/session_store.h"
#include "agentxx/util/exception.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/post.hpp"
#include "fmt/format.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"

#include "plugin_channel_agent_io.h"

#include <algorithm>
#include <filesystem>

namespace agentxx {
namespace plugin {

namespace {

/// 会话 id 用的安全片段: 只保留字母数字与 `_-.`, 其余换成 `_`
std::string sanitizeIdPart(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        const auto uc = static_cast<unsigned char>(c);
        if ((uc >= 'a' && uc <= 'z') || (uc >= 'A' && uc <= 'Z') || (uc >= '0' && uc <= '9')
            || c == '_' || c == '-' || c == '.') {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }
    return out;
}

/// 运行时错误码对应的可读文本 (日志与事件载荷用)
const char* runtimeErrorText(int32_t code) {
    switch (code) {
        case AGENTXX_PLUGIN_RUNTIME_ERR_ARG:
            return "参数非法";
        case AGENTXX_PLUGIN_RUNTIME_ERR_STATE:
            return "状态错误";
        case AGENTXX_PLUGIN_RUNTIME_ERR_NOT_FOUND:
            return "对象不存在";
        case AGENTXX_PLUGIN_RUNTIME_ERR_BUDGET:
            return "超出资源上限";
        case AGENTXX_PLUGIN_RUNTIME_ERR_CONFLICT:
            return "会话已被占用";
        case AGENTXX_PLUGIN_RUNTIME_ERR_UNSUPPORTED:
            return "主程序未提供运行时能力";
        case AGENTXX_PLUGIN_RUNTIME_ERR_FAILED:
            return "执行失败";
        default:
            return "未知错误";
    }
}

} // namespace

// =====================================================================
// 容器与记录查找
// =====================================================================

std::shared_ptr<agent::AgentHost> PluginManager::runtimeHost() const {
    auto primary = primaryAgent_.lock();
    if (!primary) {
        return nullptr;
    }
    return primary->host.lock();
}

RuntimeRoot* PluginManager::runtimeRootOf(const PluginInstance* inst, std::string_view rootId) {
    if (rootId.empty()) {
        return nullptr;
    }
    for (const auto& root : runtimeRoots_) {
        if (!root || root->rootId != rootId) {
            continue;
        }
        // 别的实例的根一律当"不存在": 既不暴露信息, 也不允许跨实例操作
        if (inst != nullptr && root->owner != inst->name) {
            return nullptr;
        }
        return root.get();
    }
    return nullptr;
}

RuntimeChannel* PluginManager::runtimeChannelOf(const PluginInstance* inst, int64_t channelId) {
    if (channelId <= 0) {
        return nullptr;
    }
    for (const auto& channel : runtimeChannels_) {
        if (!channel || channel->channelId != channelId) {
            continue;
        }
        if (inst != nullptr && channel->owner != inst->name) {
            return nullptr;
        }
        return channel.get();
    }
    return nullptr;
}

// =====================================================================
// 通道事件交付
// =====================================================================

void PluginManager::deliverChannelEvent(
    const RuntimeChannel&    channel,
    int32_t                  kind,
    int64_t                  interruptId,
    std::string_view         text,
    const utilxx_base::Json* payload
) {
    if (!channel.onEvent || channel.closed) {
        return;
    }
    // 载荷文本要在回调期间有效: 放在本栈帧里的局部字符串
    std::string payloadText;
    if (payload != nullptr) {
        payloadText = payload->dump();
    }
    AgentxxPluginChannelEvent event{};
    event.struct_size    = sizeof(AgentxxPluginChannelEvent);
    event.kind           = kind;
    event.channel_id     = channel.channelId;
    event.interrupt_id   = interruptId;
    event.text           = PluginStringView::from(text.data(), text.size());
    event.payload_json   = PluginStringView::from(payloadText.data(), payloadText.size());
    try {
        channel.onEvent(channel.userData, &event);
    } catch (const std::exception& e) {
        XX_LOGE(
            "[runtime] channel {} event callback threw: {}",
            channel.channelId,
            e.what()
        );
    } catch (...) {
        XX_LOGE("[runtime] channel {} event callback threw unknown exception", channel.channelId);
    }
}

// =====================================================================
// 根: 挂载
// =====================================================================

int32_t PluginManager::runtimeMountRoot(
    PluginInstance*                   inst,
    const AgentxxPluginMountRootSpec* spec,
    std::string&                      outRootId,
    std::string&                      err
) {
    if (inst == nullptr || spec == nullptr) {
        err = "mount_root: 参数为空";
        return AGENTXX_PLUGIN_RUNTIME_ERR_ARG;
    }
    outRootId.clear();
    // 建根只能在进程级时机: 在插件 mount 回调里建根会变成"每个 agent 再建一个根",
    // 递归没有终点 —— 这是第一道防护
    if (mountingDepth_ > 0) {
        err = "mount_root: 不能在插件 mount 回调里调用 (请在 start 里建根)";
        return AGENTXX_PLUGIN_RUNTIME_ERR_STATE;
    }
    auto host = runtimeHost();
    if (!host) {
        err = "mount_root: 本进程没有 agent 容器 (插件管理器未挂到 AgentHost 上)";
        return AGENTXX_PLUGIN_RUNTIME_ERR_UNSUPPORTED;
    }
    if (host->rootCount() >= host->maxRoots()) {
        err = fmt::format(
            "mount_root: 根数已达上限 {} (AgentHost::Config::maxRoots)",
            host->maxRoots()
        );
        return AGENTXX_PLUGIN_RUNTIME_ERR_BUDGET;
    }

    // ---- 根 id ----
    // 根 id 就是容器的 agentId (插件拿它 open_channel, 也当 agentId 用)。
    // 但容器只能在 agent init 之后登记, 而派生配置又需要 id (拼会话目录), 因此这里
    // 先按同一公式算一个**不重复**的 id, 再把它显式传给 MountSpec.agentId —— 容器
    // 用的就是我们算出的那个 —— 于是落取录与容器登记永远一致。
    const std::string owner = inst->name.empty() ? std::string{"plugin"} : inst->name;
    std::string       rootId;
    for (int attempt = 0; attempt < 1024 && rootId.empty(); ++attempt) {
        const std::string candidate
            = fmt::format("ext:{}:{}", sanitizeIdPart(owner), nextRuntimeHandle_++);
        if (host->registry().contains(candidate)) {
            continue; // 容器里已有同名节点 (别的实例/子代理)
        }
        bool used = false;
        for (const auto& item : runtimeRoots_) {
            if (item && item->rootId == candidate) {
                used = true;
                break;
            }
        }
        if (!used) {
            rootId = candidate;
        }
    }
    if (rootId.empty()) {
        err = "mount_root: 无法分配新的根 id";
        return AGENTXX_PLUGIN_RUNTIME_ERR_FAILED;
    }

    // 派生配置 (可能失败: JSON 非法 / 字段越界 / 模型不存在)
    auto cfg = deriveRootConfig(*spec, rootId, *inst, err);
    if (!cfg) {
        return AGENTXX_PLUGIN_RUNTIME_ERR_ARG;
    }
    // 创建者是否也挂到自己根上: 默认不挂 (切断递归的第二道防护)
    if (spec->include_creator == 0
        && cfg->pluginMountPolicy != agentxx::agent::PluginMountPolicy::None) {
        // 从挂载名单里剔掉自己 (mode=all 时改成 listed 并把自己去掉)
        const auto self = inst->name;
        if (cfg->pluginMountPolicy == agentxx::agent::PluginMountPolicy::All) {
            // all 里无法排除单个插件: 退化成"除它以外全部"的显式名单
            std::vector<std::string> names;
            for (const auto& [name, other] : this->plugins_) {
                (void)other;
                if (name != self) {
                    names.push_back(name);
                }
            }
            cfg->pluginMountPolicy = agentxx::agent::PluginMountPolicy::Listed;
            cfg->pluginMountList   = std::move(names);
            if (cfg->pluginMountList.empty()) {
                cfg->pluginMountPolicy = agentxx::agent::PluginMountPolicy::None;
            }
        } else if (cfg->pluginMountPolicy == agentxx::agent::PluginMountPolicy::Listed) {
            std::erase(cfg->pluginMountList, self);
            if (cfg->pluginMountList.empty()) {
                cfg->pluginMountPolicy = agentxx::agent::PluginMountPolicy::None;
            }
        }
    }

    auto agent = host->createAgent(cfg);
    if (!agent) {
        err = "mount_root: 容器未能创建 agent 实例";
        return AGENTXX_PLUGIN_RUNTIME_ERR_FAILED;
    }
    auto ctx = agent->getContext();
    if (!ctx) {
        err = "mount_root: 新建 agent 没有上下文";
        return AGENTXX_PLUGIN_RUNTIME_ERR_FAILED;
    }
    // 挂载前先把容器这份插件管理器注入: init 的 plugin_mount 步骤要按策略挂载
    if (auto pluginHost = host->pluginHost()) {
        ctx->pluginManager      = pluginHost;
        ctx->pluginManagerOwned = false;
    }

    // ---- 记录先建出来 (init 期间的状态与失败都要能查到) ----
    auto record      = std::make_shared<RuntimeRoot>();
    record->rootId   = rootId;
    record->owner    = owner;
    record->name     = svToStr(spec->name);
    record->config   = cfg;
    record->agent    = agent;
    record->storeRoot = cfg->sessionStoreDirectory;
    record->mountSummary = utilxx_base::Json{
        {"policy", agentxx::agent::PluginMountPolicy::Listed == cfg->pluginMountPolicy
                       ? "listed"
                       : (agentxx::agent::PluginMountPolicy::All == cfg->pluginMountPolicy ? "all"
                                                                                          : "none")},
        {"list",   cfg->pluginMountList                                  },
    }
                               .dump();
    runtimeRoots_.push_back(record);

    // ---- 异步 init + 登记 (入口不能 co_await: 发起后立即返回 id) ----
    std::weak_ptr<PluginManager> weakSelf = weak_from_this();
    auto                         ioEx     = ioExecutor();
    auto task = [weakSelf, host, agent, record, owner, rootId]() -> asio::awaitable<void> {
        const bool ok = co_await agentxx::util::catchErrorAsync<bool>(
            [agent]() -> asio::awaitable<bool> {
                co_await agent->init();
                co_return true;
            },
            [record](std::string errmsg) -> asio::awaitable<bool> {
                record->failed = true;
                record->error  = errmsg;
                XX_LOGE("[runtime] root '{}' init failed: {}", record->rootId, errmsg);
                co_return false;
            }
        );
        auto self = weakSelf.lock();
        if (!self) {
            co_return;
        }
        if (!ok) {
            // init 失败: 该根下等待中的通道各自收到错误并关闭
            for (auto channelId : record->waitingChannels) {
                if (auto* channel = self->runtimeChannelOf(nullptr, channelId); channel != nullptr) {
                    self->runtimeCloseChannelNow(
                        *channel,
                        fmt::format("root init failed: {}", record->error)
                    );
                }
            }
            record->waitingChannels.clear();
            self->runtimeDropRoot(*record, false);
            co_return;
        }

        // 登记到容器 (节点 + 子代理服务); id 用我们算好的那一个
        const std::string mountedId = host->mountRoot(
            agent,
            {.agentId = rootId, .owner = owner, .registerSubagentService = true}
        );
        if (mountedId.empty()) {
            record->failed = true;
            record->error  = "会话容器拒绝挂载该根 (见日志)";
            for (auto channelId : record->waitingChannels) {
                if (auto* channel = self->runtimeChannelOf(nullptr, channelId); channel != nullptr) {
                    self->runtimeCloseChannelNow(*channel, record->error);
                }
            }
            record->waitingChannels.clear();
            self->runtimeDropRoot(*record, false);
            co_return;
        }
        record->ready = true;
        XX_LOGI(
            "[runtime] root '{}' ready (owner='{}', channels waiting={})",
            rootId,
            owner,
            record->waitingChannels.size()
        );
        self->runtimeFlushWaitingChannels(*record);
    };

    if (ioEx) {
        asio::post(ioEx, [ioEx, task = std::move(task)]() mutable {
            asio::co_spawn(ioEx, std::move(task), asio::detached);
        });
    } else {
        // 没有 io 执行器: 直接在容器 ioCtx 上发起
        auto ex = host->ioCtx()->get_executor();
        asio::post(ex, [ex, task = std::move(task)]() mutable {
            asio::co_spawn(ex, std::move(task), asio::detached);
        });
    }

    // 插件面看到的就是这个 id (容器登记时也用同一个值)
    outRootId = rootId;
    return AGENTXX_PLUGIN_RUNTIME_OK;
}

int32_t PluginManager::runtimeUnmountRoot(PluginInstance* inst, std::string_view rootId) {
    if (inst == nullptr || rootId.empty()) {
        return AGENTXX_PLUGIN_RUNTIME_ERR_ARG;
    }
    // 内置根与"别人的根"都按不存在处理
    auto* record = runtimeRootOf(inst, rootId);
    if (record == nullptr) {
        XX_LOGW("[runtime] unmount_root: root '{}' 不存在或不属于本实例", rootId);
        return AGENTXX_PLUGIN_RUNTIME_ERR_NOT_FOUND;
    }
    auto host = runtimeHost();
    if (!host || !record->ready) {
        // 尚未挂进容器: 只回收记录 (init 协程会因记录失效而自行收尾)
        runtimeDropRoot(*record, false);
        return AGENTXX_PLUGIN_RUNTIME_OK;
    }
    if (host->findRoot(rootId) == nullptr) {
        // 容器里已经没有它 (自己关了): 记录一并清掉
        runtimeDropRoot(*record, false);
        return AGENTXX_PLUGIN_RUNTIME_OK;
    }
    // 先关通道再摘根: 通道的驱动壳持有该 agent, 反过来不行
    std::vector<int64_t> channels = record->channels;
    for (auto channelId : channels) {
        if (auto* channel = runtimeChannelOf(nullptr, channelId); channel != nullptr) {
            runtimeCloseChannelNow(*channel, "root_unmounted");
        }
    }
    runtimeDropRoot(*record, true);
    return AGENTXX_PLUGIN_RUNTIME_OK;
}

void PluginManager::runtimeDropRoot(RuntimeRoot& root, bool unmountFromHost) {
    if (unmountFromHost && !root.rootId.empty()) {
        if (auto host = runtimeHost()) {
            if (host->findRoot(root.rootId) != nullptr) {
                host->unmountRoot(root.rootId);
            }
        }
    }
    std::erase_if(runtimeRoots_, [&root](const std::shared_ptr<RuntimeRoot>& item) {
        return item.get() == &root;
    });
}

// =====================================================================
// 通道: 打开 / 启动 / 关闭
// =====================================================================

int32_t PluginManager::runtimeOpenChannel(
    PluginInstance*                 inst,
    const AgentxxPluginChannelSpec* spec,
    int64_t*                        outChannelId,
    std::string&                    err
) {
    if (inst == nullptr || spec == nullptr || outChannelId == nullptr) {
        err = "open_channel: 参数为空";
        return AGENTXX_PLUGIN_RUNTIME_ERR_ARG;
    }
    if (spec->on_event == nullptr) {
        err = "open_channel: on_event 回调必填";
        return AGENTXX_PLUGIN_RUNTIME_ERR_ARG;
    }
    const std::string rootId = svToStr(PluginxxStringView{spec->root_id.data, spec->root_id.size});
    auto*             root   = runtimeRootOf(inst, rootId);
    if (root == nullptr) {
        err = fmt::format("open_channel: root '{}' 不存在或不属于本实例", rootId);
        return AGENTXX_PLUGIN_RUNTIME_ERR_NOT_FOUND;
    }
    if (root->failed) {
        err = fmt::format("open_channel: root '{}' 已失败: {}", rootId, root->error);
        return AGENTXX_PLUGIN_RUNTIME_ERR_STATE;
    }
    if (root->channels.size() + root->waitingChannels.size() >= kMaxChannelsPerRoot) {
        err = fmt::format(
            "open_channel: 根 '{}' 的通道数已达上限 {}",
            rootId,
            kMaxChannelsPerRoot
        );
        return AGENTXX_PLUGIN_RUNTIME_ERR_BUDGET;
    }

    auto channel           = std::make_shared<RuntimeChannel>();
    channel->channelId     = nextRuntimeHandle_++;
    channel->owner         = inst->name;
    channel->rootId        = rootId;
    channel->sessionId     = svToStr(PluginxxStringView{spec->session_id.data, spec->session_id.size});
    channel->title         = svToStr(PluginxxStringView{spec->title.data, spec->title.size});
    channel->model         = svToStr(PluginxxStringView{spec->model.data, spec->model.size});
    channel->interruptTimeout = std::chrono::milliseconds(spec->interrupt_timeout_ms);
    channel->onEvent          = spec->on_event;
    channel->userData         = spec->user_data;
    if (channel->sessionId.empty()) {
        // 框架生成: 会话 id 决定上下文与持久化目录, 必须可读且唯一
        channel->sessionId = fmt::format(
            "{}_{}_{}",
            sanitizeIdPart(inst->name),
            sanitizeIdPart(rootId),
            channel->channelId
        );
    }

    // (存储根, sessionId) 进程内独占: 两个通道驱动同一个会话会静默互盖数据
    //
    // 会话定位键对"内存会话"要补上根 id: 没有会话存储时 `storeRoot` 是空串,
    // 只用 (空串, sessionId) 会把不同根下的同名会话误判成同一份 —— 而内存会话
    // 恰恰是按 agent 各自存放的, 不同根同名本来就可以共存。
    if (auto host = runtimeHost()) {
        const std::string ownerTag = fmt::format("plugin:{}#{}", inst->name, channel->channelId);
        if (!host->claimSession(root->sessionKey(), channel->sessionId, ownerTag)) {
            err = fmt::format(
                "open_channel: 会话 '{}' 已被别的通道驱动 (存储根 '{}')",
                channel->sessionId,
                root->sessionKey()
            );
            return AGENTXX_PLUGIN_RUNTIME_ERR_CONFLICT;
        }
    }

    runtimeChannels_.push_back(channel);
    root->channels.push_back(channel->channelId);

    // 根还没就绪: 先排队, 就绪后自动启动
    if (!root->ready) {
        channel->waiting = true;
        root->waitingChannels.push_back(channel->channelId);
        XX_LOGI(
            "[runtime] channel {} (session '{}') 已排队, 等待 root '{}' 就绪",
            channel->channelId,
            channel->sessionId,
            rootId
        );
        *outChannelId = channel->channelId;
        return AGENTXX_PLUGIN_RUNTIME_OK;
    }

    const int32_t rc = runtimeStartChannel(*root, *channel, err);
    if (rc != AGENTXX_PLUGIN_RUNTIME_OK) {
        // 启动失败: 释放会话占用与记录, 不留下半成品
        if (auto host = runtimeHost()) {
            host->releaseSession(
                root->sessionKey(),
                channel->sessionId,
                fmt::format("plugin:{}#{}", inst->name, channel->channelId)
            );
        }
        std::erase(root->channels, channel->channelId);
        std::erase_if(runtimeChannels_, [&channel](const std::shared_ptr<RuntimeChannel>& item) {
            return item.get() == channel.get();
        });
        return rc;
    }
    *outChannelId = channel->channelId;
    return AGENTXX_PLUGIN_RUNTIME_OK;
}

int32_t PluginManager::runtimeStartChannel(
    RuntimeRoot&    root,
    RuntimeChannel& channel,
    std::string&    err
) {
    auto host = runtimeHost();
    if (!host || !root.agent) {
        err = "open_channel: 根不可用";
        return AGENTXX_PLUGIN_RUNTIME_ERR_STATE;
    }
    channel.waiting = false;

    // 会话标题: 写入会话库 meta (界面/会话列表用; 无持久化时是空操作)
    if (!channel.title.empty()) {
        if (auto host = runtimeHost(); host != nullptr) {
            if (auto ctx = root.agent->getContext(); ctx && ctx->sessions
                && ctx->sessions->sessionStore) {
                ctx->sessions->sessionStore->setSessionTitle(channel.sessionId, channel.title);
            }
        }
    }

    // client 端点: 事件翻成通道事件; 存活由驱动壳持有
    auto channelId = channel.channelId;
    auto clientIO  = std::make_shared<PluginChannelAgentIO>(host->ioCtx()->get_executor(), channelId);
    clientIO->setSessionId(channel.sessionId);
    // 正文按时间窗合批: 逐 token 回调对插件 (往往要 push 到聊天平台) 太密
    clientIO->setDeltaBatch(60);
    std::weak_ptr<PluginManager> weakSelf = weak_from_this();
    clientIO->setEventFn(
        [weakSelf, channelId](
            int32_t                  kind,
            int64_t                  interruptId,
            std::string_view         text,
            const utilxx_base::Json* payload
        ) {
            auto self = weakSelf.lock();
            if (!self) {
                return;
            }
            auto* live = self->runtimeChannelOf(nullptr, channelId);
            if (live == nullptr) {
                return;
            }
            if (kind == AGENTXX_PLUGIN_CHANNEL_EVT_TURN_START) {
                live->turnBusy = true;
            } else if (kind == AGENTXX_PLUGIN_CHANNEL_EVT_TURN_END) {
                live->turnBusy = false;
            }
            self->deliverChannelEvent(*live, kind, interruptId, text, payload);
        }
    );
    clientIO->initSelf(clientIO);
    channel.clientIO = clientIO;

    agent::AgentSessionRuntime::Config cfg;
    cfg.sessionId              = channel.sessionId;
    cfg.clientExecutor         = host->ioCtx()->get_executor();
    cfg.interruptTimeout       = channel.interruptTimeout;
    cfg.deltaBufferCap         = 0; ///< 通道不重连重放: 不占缓冲
    cfg.initialSyncTailCount   = 0;
    cfg.autoRequestComponents  = false; ///< 通道不要启动清单 (插件不看那一段)
    cfg.forwardInitProgress    = false; ///< 根已经 init 过 (skipInit)
    cfg.loadPluginsInHost      = false;
    cfg.host                   = host;
    cfg.skipInit               = true; ///< 根在 mount_root 时已 init 并登记
    cfg.onServerReady = [clientIO, channelId, rootId = root.rootId, weakSelf]() {
        auto self = weakSelf.lock();
        if (!self) {
            return;
        }
        auto* live = self->runtimeChannelOf(nullptr, channelId);
        if (live == nullptr) {
            return;
        }
        live->ready = true;
        // 就绪事件交给插件: 带上根 id 与会话 id, 插件据此记录"这个通道已可用"
        utilxx_base::Json payload = utilxx_base::Json::object();
        payload["rootId"]         = rootId;
        payload["sessionId"]      = live->sessionId;
        payload["model"]          = live->model;
        self->deliverChannelEvent(
            *live,
            AGENTXX_PLUGIN_CHANNEL_EVT_READY,
            0,
            {},
            &payload
        );
    };
    cfg.onInitError = [clientIO](std::string_view errmsg) {
        clientIO->notifyError("init_failed", fmt::format("agent init failed: {}", errmsg));
    };
    cfg.onStopped = [clientIO, channelId, weakSelf]() {
        // 驱动循环退出: 通知插件并释放记录 (端点最后一条事件固定是 CLOSED)
        clientIO->notifyClosed("closed");
        auto self = weakSelf.lock();
        if (!self) {
            return;
        }
        if (auto* live = self->runtimeChannelOf(nullptr, channelId); live != nullptr) {
            live->closed = true;
        }
        clientIO->clearEventFn();
    };

    auto runtime = agent::AgentSessionRuntime::create(root.agent, clientIO, std::move(cfg));
    if (!runtime) {
        err = "open_channel: 驱动壳装配失败 (参数不完整)";
        return AGENTXX_PLUGIN_RUNTIME_ERR_FAILED;
    }
    channel.runtime = runtime;
    // 初始模型 (空 = 用根配置的当前模型)
    if (!channel.model.empty()) {
        runtime->selectModel(channel.model);
    }
    runtime->start();
    XX_LOGI(
        "[runtime] channel {} started (root='{}', session='{}')",
        channel.channelId,
        root.rootId,
        channel.sessionId
    );
    return AGENTXX_PLUGIN_RUNTIME_OK;
}

void PluginManager::runtimeFlushWaitingChannels(RuntimeRoot& root) {
    std::vector<int64_t> waiting = root.waitingChannels;
    root.waitingChannels.clear();
    for (auto channelId : waiting) {
        auto* channel = runtimeChannelOf(nullptr, channelId);
        if (channel == nullptr) {
            continue;
        }
        std::string err;
        if (runtimeStartChannel(root, *channel, err) != AGENTXX_PLUGIN_RUNTIME_OK) {
            XX_LOGE("[runtime] channel {} 启动失败: {}", channelId, err);
            runtimeCloseChannelNow(*channel, err);
        }
    }
}

void PluginManager::runtimeCloseChannelNow(RuntimeChannel& channel, std::string_view reason) {
    if (channel.closed) {
        return;
    }
    // 先交最后一条事件再把通道标成已关闭: [deliverChannelEvent] 对已关闭的通道
    // 直接丢弃, 顺序颠倒的话插件永远收不到 CLOSED
    utilxx_base::Json payload = utilxx_base::Json::object();
    payload["reason"]         = reason;
    deliverChannelEvent(channel, AGENTXX_PLUGIN_CHANNEL_EVT_CLOSED, 0, {}, &payload);
    channel.closed = true;
    if (channel.clientIO) {
        channel.clientIO->clearEventFn();
        // 失败挂起的中断并冲刷合批: 关闭之后这些事件再也发不出去
        channel.clientIO->failAllPendingInterrupts();
    }
    if (channel.runtime) {
        channel.runtime->stop();
    }
    // 会话占用释放: 之后同 sessionId 可以再开一个通道
    if (auto sessionHost = runtimeHost()) {
        if (auto* sessionRoot = runtimeRootOf(nullptr, channel.rootId); sessionRoot != nullptr) {
            sessionHost->releaseSession(
                sessionRoot->sessionKey(),
                channel.sessionId,
                fmt::format("plugin:{}#{}", channel.owner, channel.channelId)
            );
        }
    }
    // 触发器/定时器指向这个通道: 一并注销 (它们的投递目标已经不存在)
    std::erase_if(runtimeTriggers_, [&channel](const std::shared_ptr<RuntimeTrigger>& item) {
        return item && item->channelId == channel.channelId;
    });
    std::erase_if(runtimeTimers_, [&channel](const std::shared_ptr<RuntimeTimer>& item) {
        return item && item->channelId == channel.channelId;
    });
    // 从根记录里摘掉
    if (auto* ownerRoot = runtimeRootOf(nullptr, channel.rootId); ownerRoot != nullptr) {
        std::erase(ownerRoot->channels, channel.channelId);
    }
    const auto channelId = channel.channelId;
    std::erase_if(runtimeChannels_, [channelId](const std::shared_ptr<RuntimeChannel>& item) {
        return item && item->channelId == channelId;
    });
    XX_LOGI("[runtime] channel {} closed ({})", channelId, reason);
}

int32_t PluginManager::runtimeCloseChannel(PluginInstance* inst, int64_t channelId) {
    if (inst == nullptr || channelId <= 0) {
        return AGENTXX_PLUGIN_RUNTIME_ERR_ARG;
    }
    auto* channel = runtimeChannelOf(inst, channelId);
    if (channel == nullptr) {
        return AGENTXX_PLUGIN_RUNTIME_ERR_NOT_FOUND;
    }
    runtimeCloseChannelNow(*channel, "closed");
    return AGENTXX_PLUGIN_RUNTIME_OK;
}

// =====================================================================
// 输入与控制
// =====================================================================

int32_t PluginManager::runtimeSendInput(
    PluginInstance*  inst,
    int64_t          channelId,
    std::string_view inputJson,
    std::string&     err
) {
    // inst 为空 = 主程序内部投递 (触发器/定时器): 不做归属校验, 但仍要求通道存在
    if (channelId <= 0) {
        err = "send_input: channel_id 非法";
        return AGENTXX_PLUGIN_RUNTIME_ERR_ARG;
    }
    auto* channel = runtimeChannelOf(inst, channelId);
    if (channel == nullptr) {
        err = fmt::format(
            "send_input: 通道 {} 不存在{}",
            channelId,
            inst != nullptr ? "或不属于本实例" : ""
        );
        return AGENTXX_PLUGIN_RUNTIME_ERR_NOT_FOUND;
    }
    if (channel->closed || !channel->runtime) {
        err = fmt::format("send_input: 通道 {} 已关闭", channelId);
        return AGENTXX_PLUGIN_RUNTIME_ERR_STATE;
    }
    // 与 FFI 同一份输入解析: text/attachments 至少一个非空, 条数与大小先拦一次
    auto parsed = agent::parseUserInputJson(inputJson);
    if (!parsed.has_value()) {
        err = parsed.error().message;
        return AGENTXX_PLUGIN_RUNTIME_ERR_ARG;
    }
    auto input = std::move(*parsed);
    if (!input.delivery.empty() && !agent::InputDelivery::known(input.delivery)) {
        err = fmt::format(
            "send_input: delivery 取值非法 '{}' (支持 next-turn/next-step/inject/collect)",
            input.delivery
        );
        return AGENTXX_PLUGIN_RUNTIME_ERR_ARG;
    }
    channel->runtime->sendInput(std::move(input));
    return AGENTXX_PLUGIN_RUNTIME_OK;
}

int32_t PluginManager::runtimeCancel(PluginInstance* inst, int64_t channelId) {
    if (inst == nullptr || channelId <= 0) {
        return AGENTXX_PLUGIN_RUNTIME_ERR_ARG;
    }
    auto* channel = runtimeChannelOf(inst, channelId);
    if (channel == nullptr) {
        return AGENTXX_PLUGIN_RUNTIME_ERR_NOT_FOUND;
    }
    if (channel->closed || !channel->runtime) {
        return AGENTXX_PLUGIN_RUNTIME_ERR_STATE;
    }
    channel->runtime->cancel();
    return AGENTXX_PLUGIN_RUNTIME_OK;
}

int32_t
    PluginManager::runtimeSelectModel(PluginInstance* inst, int64_t channelId, std::string_view model) {
    if (inst == nullptr || channelId <= 0 || model.empty()) {
        return AGENTXX_PLUGIN_RUNTIME_ERR_ARG;
    }
    auto* channel = runtimeChannelOf(inst, channelId);
    if (channel == nullptr) {
        return AGENTXX_PLUGIN_RUNTIME_ERR_NOT_FOUND;
    }
    if (channel->closed || !channel->runtime) {
        return AGENTXX_PLUGIN_RUNTIME_ERR_STATE;
    }
    channel->model = std::string{model};
    channel->runtime->selectModel(std::string{model});
    return AGENTXX_PLUGIN_RUNTIME_OK;
}

int32_t PluginManager::runtimeRespondInterrupt(
    PluginInstance*  inst,
    int64_t          channelId,
    int64_t          interruptId,
    std::string_view valuesJson,
    std::string&     err
) {
    if (inst == nullptr || channelId <= 0 || interruptId <= 0) {
        err = "respond_interrupt: 参数为空";
        return AGENTXX_PLUGIN_RUNTIME_ERR_ARG;
    }
    auto* channel = runtimeChannelOf(inst, channelId);
    if (channel == nullptr) {
        err = fmt::format("respond_interrupt: 通道 {} 不存在或不属于本实例", channelId);
        return AGENTXX_PLUGIN_RUNTIME_ERR_NOT_FOUND;
    }
    if (channel->closed || !channel->clientIO) {
        err = fmt::format("respond_interrupt: 通道 {} 已关闭", channelId);
        return AGENTXX_PLUGIN_RUNTIME_ERR_STATE;
    }
    // 应答形状与界面一致: {"values": {...}} (中断结果由值对象表达, 空对象 = 取消)
    utilxx_base::Json values = utilxx_base::Json::object();
    if (!valuesJson.empty()) {
        try {
            values = utilxx_base::Json::parse(std::string{valuesJson});
        } catch (const std::exception& e) {
            err = fmt::format("respond_interrupt: values_json 不是合法 JSON: {}", e.what());
            return AGENTXX_PLUGIN_RUNTIME_ERR_ARG;
        }
    }
    if (!values.is_object()) {
        err = "respond_interrupt: values_json 必须是对象 ({\"values\":{...}})";
        return AGENTXX_PLUGIN_RUNTIME_ERR_ARG;
    }
    if (!values.contains("values")) {
        // 允许直接给值对象 (插件写 {"name":"answer"} 也能工作)
        values = utilxx_base::Json{{"values", std::move(values)}};
    }
    if (!channel->clientIO->submitInterrupt(interruptId, std::move(values))) {
        err = fmt::format("respond_interrupt: 中断 {} 已过期或已应答", interruptId);
        return AGENTXX_PLUGIN_RUNTIME_ERR_STATE;
    }
    return AGENTXX_PLUGIN_RUNTIME_OK;
}

// =====================================================================
// 查询
// =====================================================================

utilxx_base::Json channelStateJson(const RuntimeChannel& channel, const agent::AgentConfig* cfg) {
    utilxx_base::Json out = utilxx_base::Json::object();
    out["channelId"]        = channel.channelId;
    out["rootId"]           = channel.rootId;
    out["sessionId"]        = channel.sessionId;
    out["title"]            = channel.title;
    out["model"]            = channel.model;
    out["ready"]            = channel.ready;
    out["closed"]           = channel.closed;
    out["waiting"]          = channel.waiting;
    out["busy"]             = channel.turnBusy;
    out["droppedInputs"]    = channel.droppedInputs;
    out["interruptPending"] = channel.clientIO ? channel.clientIO->pendingInterruptId() : 0;
    if (cfg != nullptr) {
        out["workDir"] = cfg->resolvedWorkDir();
        out["language"] = cfg->language;
    }
    return out;
}

std::string PluginManager::runtimeChannelStateJson(PluginInstance* inst, int64_t channelId) const {
    // 只读入口 (允许关闭中调用): 记录表本身只在 IO 线程变动, 这里已在该线程
    auto* self    = const_cast<PluginManager*>(this);
    auto* channel = self->runtimeChannelOf(inst, channelId);
    if (channel == nullptr) {
        return {};
    }
    const agent::AgentConfig* cfg = nullptr;
    if (auto* root = self->runtimeRootOf(inst, channel->rootId); root != nullptr && root->config) {
        cfg = root->config.get();
    }
    return channelStateJson(*channel, cfg).dump();
}

std::string
    PluginManager::channelContextMessages(PluginInstance* inst, int64_t channelId) const {
    auto* self    = const_cast<PluginManager*>(this);
    auto* channel = self->runtimeChannelOf(inst, channelId);
    if (channel == nullptr) {
        return {};
    }
    auto* root = self->runtimeRootOf(inst, channel->rootId);
    if (root == nullptr || !root->agent) {
        return {};
    }
    auto ctx = root->agent->getContext();
    if (!ctx) {
        return {};
    }
    auto session = ctx->getSession(channel->sessionId);
    if (!session) {
        return {};
    }
    return session->llmMessagesJson().dump();
}

std::string PluginManager::listRuntimeJson(PluginInstance* inst) const {
    utilxx_base::Json roots = utilxx_base::Json::array();
    for (const auto& root : runtimeRoots_) {
        if (!root) {
            continue;
        }
        // inst 非空时只列本实例的; 空 = 全部 (诊断)
        if (inst != nullptr && root->owner != inst->name) {
            continue;
        }
        utilxx_base::Json item = utilxx_base::Json::object();
        item["rootId"]         = root->rootId;
        item["name"]           = root->name;
        item["owner"]          = root->owner;
        item["ready"]          = root->ready;
        item["failed"]         = root->failed;
        item["error"]          = root->error;
        const std::string mode
            = root->config
                  ? (root->config->pluginMountPolicy == agentxx::agent::PluginMountPolicy::All
                         ? "all"
                         : (root->config->pluginMountPolicy == agentxx::agent::PluginMountPolicy::Listed
                                ? "listed"
                                : "none"))
                  : "none";
        utilxx_base::Json summary = utilxx_base::Json::object();
        summary["pluginMount"]    = mode;
        summary["plugins"]        = root->config ? root->config->pluginMountList
                                                 : std::vector<std::string>{};
        summary["agentName"]      = root->config ? root->config->agentName : std::string{};
        summary["workDir"]        = root->config ? root->config->resolvedWorkDir() : std::string{};
        summary["language"]       = root->config ? root->config->language : std::string{};
        summary["model"]          = root->config ? root->config->currentModelName : std::string{};
        summary["sessionStore"]   = root->config ? root->config->enableSessionStore : false;
        item["configSummary"]     = std::move(summary);
        utilxx_base::Json channels = utilxx_base::Json::array();
        for (auto channelId : root->channels) {
            for (const auto& channel : runtimeChannels_) {
                if (!channel || channel->channelId != channelId) {
                    continue;
                }
                channels.push_back(
                    channelStateJson(*channel, root->config ? root->config.get() : nullptr)
                );
                break;
            }
        }
        item["channels"] = std::move(channels);
        roots.push_back(std::move(item));
    }
    // 触发器与定时器一并列出 (诊断"为什么没有投递"的第一现场)
    utilxx_base::Json triggers = utilxx_base::Json::array();
    for (const auto& trigger : runtimeTriggers_) {
        if (!trigger || (inst != nullptr && trigger->owner != inst->name)) {
            continue;
        }
        triggers.push_back(utilxx_base::Json{
            {"triggerId",  trigger->triggerId },
            {"topic",      trigger->topic     },
            {"channelId",  trigger->channelId },
            {"policy",     trigger->policy    },
            {"debounceMs", trigger->debounceMs},
            {"pending",    trigger->pending.size()},
            {"dropped",    trigger->dropped   },
            {"merged",     trigger->merged    },
        });
    }
    utilxx_base::Json timers = utilxx_base::Json::array();
    for (const auto& timer : runtimeTimers_) {
        if (!timer || (inst != nullptr && timer->owner != inst->name)) {
            continue;
        }
        timers.push_back(utilxx_base::Json{
            {"timerId",    timer->timerId           },
            {"channelId",  timer->channelId         },
            {"policy",     timer->policy            },
            {"intervalMs", timer->interval.count()  },
            {"fired",      timer->fired             },
        });
    }
    utilxx_base::Json out = utilxx_base::Json::object();
    out["roots"]          = std::move(roots);
    out["triggers"]       = std::move(triggers);
    out["timers"]         = std::move(timers);
    return out.dump();
}

// =====================================================================
// 回收
// =====================================================================

void PluginManager::releaseRuntimeChannelsOf(PluginInstance* inst) {
    if (inst == nullptr) {
        return;
    }
    std::vector<int64_t> channels;
    for (const auto& channel : runtimeChannels_) {
        if (channel && channel->owner == inst->name) {
            channels.push_back(channel->channelId);
        }
    }
    for (auto channelId : channels) {
        if (auto* channel = runtimeChannelOf(nullptr, channelId); channel != nullptr) {
            runtimeCloseChannelNow(*channel, "plugin_detached");
        }
    }
}

void PluginManager::releaseRuntimeOfOwner(std::string_view owner) {
    // 顺序: 触发器/定时器 → 通道 → 根。前两项都指向通道或根, 反过来会留下悬空目标。
    std::erase_if(runtimeTriggers_, [owner](const std::shared_ptr<RuntimeTrigger>& item) {
        return item && item->owner == owner;
    });
    std::erase_if(runtimeTimers_, [owner](const std::shared_ptr<RuntimeTimer>& item) {
        return item && item->owner == owner;
    });
    std::vector<int64_t> channels;
    for (const auto& channel : runtimeChannels_) {
        if (channel && channel->owner == owner) {
            channels.push_back(channel->channelId);
        }
    }
    for (auto channelId : channels) {
        if (auto* channel = runtimeChannelOf(nullptr, channelId); channel != nullptr) {
            runtimeCloseChannelNow(*channel, "plugin_stopped");
        }
    }
    std::vector<std::string> roots;
    for (const auto& root : runtimeRoots_) {
        if (root && root->owner == owner) {
            roots.push_back(root->rootId);
        }
    }
    for (const auto& rootId : roots) {
        auto* root = runtimeRootOf(nullptr, rootId);
        if (root == nullptr) {
            continue;
        }
        if (root->ready) {
            if (auto host = runtimeHost(); host && host->findRoot(rootId) != nullptr) {
                host->unmountRoot(rootId);
            }
        }
        runtimeDropRoot(*root, false);
    }
}

} // namespace plugin
} // namespace agentxx
