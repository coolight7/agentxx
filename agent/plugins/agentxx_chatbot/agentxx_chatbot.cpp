/// agentxx_chatbot —— 把人设接到聊天平台
///
/// 整体结构 (设计见 `resource/history/chatbot/plan.md` §4):
/// - **平台层** (`platform/`): 每个账号一条长连接, 把平台消息归一化成 `InboundEvent`,
///   把 `OutboundMessage` 发回平台;
/// - **调度层** (`chatbot_engine` / `chatbot_router` / `chatbot_outbound`): 去重 / 路由 /
///   关键词命令 / 聚合 / 切分 / 频控;
/// - **主程序交互**: 只经 `agentxx.agent.runtime` 表 (挂根 / 开通道 / 投输入)
///   与 `agentxx.agent.tools` 表 (在人设 root 上注册 `chatbot_*` 工具)。
///
/// 线程模型:
/// - 平台网络与调度状态在**插件自己的 io 线程**上 (一条 `io_context` + 一个线程);
/// - 通道事件回调在**主程序 io 线程**上, 只做「拷贝 → post 到插件 io 线程」;
/// - 工具跑在主程序工作线程池, 工作体同样 post 到插件 io 线程再取结果。
#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_guard.h"
#include "agentxx/plugin/api/plugin_kit.h"

#include "chatbot_config.h"
#include "chatbot_core.h"
#include "chatbot_engine.h"
#include "chatbot_outbound.h"
#include "chatbot_panel.h"
#include "chatbot_router.h"
#include "chatbot_tools.h"
#include "platform/platform.h"

#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/executor_work_guard.hpp"
#include "asio/io_context.hpp"
#include "asio/post.hpp"
#include "asio/steady_timer.hpp"
#include "fmt/format.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ac = agentxx::chatbot;
using agentxx::plugin::AgentRef;
using agentxx::plugin::ChannelEvent;
using agentxx::plugin::PluginBase;
using agentxx::plugin::PluginString;
using agentxx::plugin::PluginStringView;
using utilxx_base::Json;

namespace {

constexpr const char* kPluginName = "agentxx_chatbot";
constexpr const char* kLogTag     = "chatbot";

/// 侧边栏面板事件的刷新节拍 (毫秒)
///
/// 有会话正在执行时按这个间隔推快照: 客户端每收到一份就取下一帧点阵, 看起来就是
/// 消息列表里那种"加载中"; 同时排队条数也跟着实时刷新。没有会话在跑时只推一次
/// (状态变化时) 就停, 不空转。
constexpr int32_t kPanelTickMs = 120;

/// 面板快照的主题 (主程序给总线主题补 `plugin.` 前缀再转发给客户端,
/// 客户端拿到的就是 `{plugin: "agentxx_chatbot", event: "panel"}`)
constexpr const char* kPanelTopic = "agentxx_chatbot.panel";

/// 客户端插件请求重发快照 (client → agent)
///
/// 主程序把上行事件发布到 `plugin.client.{插件名}.{事件名}`, 所以**订阅时必须带上
/// `plugin.client.` 前缀**: 主题补齐只看开头有没有 `plugin.` / `client.`, 写
/// `client.xxx` 不会被再加前缀, 与总线上实际的 `plugin.client.xxx` 对不上, 收不到事件。
constexpr const char* kPanelRefreshTopic = "plugin.client.agentxx_chatbot.refresh";

/// 主程序在有客户端接入时发布的事件 (总线主题 `plugin.agentxx_host.client_attached`)
///
/// 晚接入/重连的客户端据此拿到一份当前快照, 不用等下一次状态变化。
constexpr const char* kClientAttachedTopic = "agentxx_host.client_attached";

/* ==================== 插件日志转发 ==================== */

/// 把插件内 `XX_LOG*` 的日志送到主程序日志
///
/// 插件静态链接了自己那份 `cxx_utilxx_base`, 它的 `LogDispatcher` 是**插件本地**的
/// 单例, 与主程序那份互不相干; 本地 dispatcher 没有任何 sink, 于是插件里直接
/// `XX_LOGI(...)` 的内容会被静默丢弃 (平台层/调度层全用 `XX_LOG*`, 联调时看不到
/// 网关连接、状态变化等关键信息)。
/// 本 sink 挂在本地 dispatcher 上, 把每条记录转给主程序的 `pluginxx.log` 接口。
class HostLogSink : public utilxx_base::ThreadedLogSink {
public:

    explicit HostLogSink(agentxx::plugin::Logger logger) :
        logger_(std::move(logger)) {}

    void onLog(const utilxx_base::LogEntry& entry) override {
        // utilxx_base::LogLevel 与主程序日志等级一一对应 (Trace/Debug/Info/Warn/Error)
        logger_.log(static_cast<int32_t>(entry.level), entry.message);
    }

private:

    agentxx::plugin::Logger logger_;
};

/* ==================== 每实例上下文 ==================== */

/// 一个账号在运行期的全部对象
struct AccountRuntime {
    std::unique_ptr<ac::IPlatform>   platform;
    std::optional<ac::AccountConfig> config;
    ac::PlatformEventSink            sink;
};

/// 插件实例上下文
struct ChatbotCtx : public PluginBase {
    /// 插件自己的 io 线程 (平台网络 + 调度状态都在这条线程上)
    std::shared_ptr<asio::io_context>                                           io;
    std::shared_ptr<asio::executor_work_guard<asio::io_context::executor_type>> work;
    std::thread                                                                 ioThread;
    /// 插件 io 线程 id (判断"是否已在正确线程")
    std::atomic<std::thread::id> ioThreadId{std::thread::id{}};

    /// 解析并校验后的配置
    ac::ChatbotConfig config;
    /// 调度对象 (只在插件 io 线程访问)
    ac::ChatbotRouter   router;
    ac::ChatbotOutbound outbound;
    ac::ChatbotEngine   engine;

    /// 账号 → 平台对象 (只在插件 io 线程访问; 工具线程通过 `platformOf` 读取时
    /// 用一把锁护住 map 结构本身)
    std::map<std::string, std::unique_ptr<AccountRuntime>, std::less<>> accounts;
    mutable std::mutex                                                  accountsMutex;

    /// 人设名 → root id (start 时写入, 之后只读)
    std::map<std::string, std::string, std::less<>> personaRoots;
    mutable std::mutex                              personaRootsMutex;

    /// 工具依赖 (地址稳定: 指向本实例成员)
    ac::ChatbotToolDeps toolDeps;

    /// 是否已启动 (stop 幂等)
    std::atomic<bool> started{false};
    /// 运行期计数
    std::atomic<uint64_t> toolCalls{0};

    /// 本实例的日志转发 sink (把插件内 `XX_LOG*` 转给主程序日志; 见 [HostLogSink])
    std::shared_ptr<HostLogSink> hostLogSink;

    /* ---------- 侧边栏「机器人」栏的快照发布 ---------- */

    /// 刷新节拍定时器 (只在插件 io 线程访问)
    std::shared_ptr<asio::steady_timer> panelTimer;
    /// 节拍循环是否在跑 (只在插件 io 线程访问)
    bool panelLoopActive = false;
    /// 快照刷新计数 (客户端用它取加载动画的当前帧)
    uint32_t panelTick = 0;

    /// 待发布的快照
    ///
    /// 为什么要绕一道: 插件接口表的 `publish` 是"投到主程序 IO 线程并等它执行"的同步
    /// 调用。工具工作体期间主程序 IO 线程正等着插件 io 线程交结果 —— 直接在那条线程上
    /// 发布就是两边互等 (实测死锁 25 秒后工具超时, 随后用已释放的栈崩溃)。
    /// 因此: 快照 JSON 由插件 io 线程生成放这里, 再向主程序要一张驱动票
    /// (`request_driver`, 任意线程可调且**不阻塞**), 由主程序在它自己的 IO 线程上回调
    /// 时直接发布 (IO 线程上 publish 是就地执行, 不等任何线程)。
    struct PanelOutbox {
        std::mutex  mutex;
        std::string json;
        bool        hasData = false;
        /// 已有一张驱动票在路上 (不重复要票)
        std::atomic<bool> driverPending{false};
        /// 在途的票 (stop 时取消, 保证回调不会再落到已销毁的实例上)
        std::atomic<PluginxxDriver*> driver{nullptr};
    };

    PanelOutbox panelOutbox;

    /// 发布一份当前状态的快照 (插件 io 线程生成 + 要票)
    void publishPanel();
    /// 向主程序要一张驱动票 (非阻塞; 已有票时不重复要)
    void requestPanelDrive();
    /// 驱动票回调 (主程序 IO 线程): 发布待发快照
    void onPanelDrive();
    /// 安排下一拍
    void schedulePanelTick();
    /// 状态变了: 需要推一份快照 (短时间内的多次变化合并成一次)
    void panelChanged();

    /* ---------- 跨线程执行 ---------- */

    bool onIoThread() const noexcept {
        return std::this_thread::get_id() == ioThreadId.load(std::memory_order_acquire);
    }

    /// 启动插件 io 线程
    void startIo() {
        io   = std::make_shared<asio::io_context>();
        work = std::make_shared<asio::executor_work_guard<asio::io_context::executor_type>>(
            io->get_executor()
        );
        std::weak_ptr<asio::io_context> weak = io;
        ioThreadId.store(std::thread::id{}, std::memory_order_release);
        ioThread = std::thread([this, weak]() {
            ioThreadId.store(std::this_thread::get_id(), std::memory_order_release);
            // 任何 handler 抛出的异常都会从 run() 冲出来; 这条线程没有外层 catch,
            // 一旦逃出去就是 std::terminate。平台网络与调度层必须在这里活下来。
            for (;;) {
                try {
                    io->run();
                    break;
                } catch (const std::exception& e) {
                    XX_LOGE("[{}] 插件 io 线程收到异常并继续: {}", kLogTag, e.what());
                    if (io->stopped()) {
                        break;
                    }
                } catch (...) {
                    XX_LOGE("[{}] 插件 io 线程收到未知异常并继续", kLogTag);
                    if (io->stopped()) {
                        break;
                    }
                }
            }
        });
        // 等到线程真正跑起来 (ioThreadId 非空), 避免首条 post 落在"还没记录 id"的窗口
        for (int i = 0; i < 1000; ++i) {
            if (ioThreadId.load(std::memory_order_acquire) != std::thread::id{}) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    /// 停止插件 io 线程 (顺序不能颠倒: 先放 work, 再 stop, 最后 join)
    void stopIo() {
        if (!io) {
            return;
        }
        work.reset();
        io->stop();
        if (ioThread.joinable()) {
            ioThread.join();
        }
        io.reset();
    }

    /// 在插件 io 线程上执行一段工作 (已在则直接跑)
    template<typename Fn>
    void runOnIo(Fn&& fn) {
        if (!io) {
            return;
        }
        if (onIoThread()) {
            fn();
            return;
        }
        asio::post(*io, std::forward<Fn>(fn));
    }

    /// 在插件 io 线程上执行**并等待结果** (工具用; 带超时, 不无限等)
    ///
    /// - 返回 false 表示超时或插件已停止
    template<typename Fn>
    bool runOnIoBlocking(Fn&& fn, std::chrono::milliseconds timeout = std::chrono::seconds{25}) {
        if (!io) {
            return false;
        }
        if (onIoThread()) {
            fn();
            return true;
        }

        struct State {
            std::mutex              mutex;
            std::condition_variable cv;
            bool                    done = false;
            bool                    ran  = false;
        };

        auto state = std::make_shared<State>();
        asio::post(*io, [state, fn = std::forward<Fn>(fn)]() mutable {
            {
                std::lock_guard lock(state->mutex);
                state->ran = true;
            }
            // 异常不得穿透到 io_context::run(): 这里只会被通知"已跑过", 结果由 fn 自己填
            try {
                fn();
            } catch (...) {
            }
            {
                std::lock_guard lock(state->mutex);
                state->done = true;
            }
            state->cv.notify_all();
        });
        std::unique_lock lock(state->mutex);
        if (!state->cv.wait_for(lock, timeout, [&state]() {
                return state->done;
            })) {
            return false;
        }
        return state->ran;
    }

    /// 平台对象查询 (工具侧用; 返回裸指针, 生命周期由本实例保证)
    ac::IPlatform* platformOf(const std::string& accountId) {
        const bool onIo = onIoThread();
        if (onIo) {
            auto it = accounts.find(accountId);
            return it == accounts.end() || !it->second->platform ? nullptr
                                                                 : it->second->platform.get();
        }
        std::lock_guard lock(accountsMutex);
        auto            it = accounts.find(accountId);
        return it == accounts.end() || !it->second->platform ? nullptr : it->second->platform.get();
    }
};

/* ==================== 侧边栏「机器人」栏的快照发布 ==================== */

/// 驱动票回调 (主程序 IO 线程; 不抛异常出边界)
void PLUGINXX_CALL onPanelDriveTicket(void* ud) {
    auto* ctx = static_cast<ChatbotCtx*>(ud);
    if (ctx == nullptr) {
        return;
    }
    try {
        ctx->onPanelDrive();
    } catch (...) {
        // 面板刷新失败不影响会话收发
    }
}

void ChatbotCtx::requestPanelDrive() {
    if (!host || !iface.coroutineRuntime || !iface.coroutineRuntime->request_driver) {
        return;
    }
    if (panelOutbox.driverPending.exchange(true, std::memory_order_acq_rel)) {
        return; // 已经有一张票: 它跑到时会取走最新一份
    }
    PluginxxString err{nullptr, 0};
    auto* driver = iface.coroutineRuntime->request_driver(host, &onPanelDriveTicket, this, &err);
    if (err.data != nullptr) {
        // 主程序给了失败原因: 记一次就够 (面板会停在上一份快照上)
        log.warn(
            std::string{"panel publish ticket rejected: "}
            + std::string{err.data, static_cast<size_t>(err.size)}
        );
        PluginString::free(host, &err);
    }
    if (driver == nullptr) {
        panelOutbox.driverPending.store(false, std::memory_order_release);
        return;
    }
    panelOutbox.driver.store(driver, std::memory_order_release);
}

void ChatbotCtx::onPanelDrive() {
    // 主程序 IO 线程: 取走一份待发快照
    std::string json;
    {
        std::lock_guard lock(panelOutbox.mutex);
        json.swap(panelOutbox.json);
        panelOutbox.hasData = false;
    }
    panelOutbox.driver.store(nullptr, std::memory_order_release);
    panelOutbox.driverPending.store(false, std::memory_order_release);

    // 在 IO 线程上 publish 是就地执行 (不再等线程), 不会与工具互等
    if (!json.empty() && iface.events && iface.events->publish && host) {
        auto topicSv = PluginStringView::fromCstr(kPanelTopic);
        auto jsonSv  = PluginStringView::from(json.data(), json.size());
        iface.events->publish(host, &topicSv, &jsonSv);
    }

    // 发布期间插件 io 线程可能又攒了一份: 再要一张票
    bool again = false;
    {
        std::lock_guard lock(panelOutbox.mutex);
        again = panelOutbox.hasData;
    }
    if (again && started.load(std::memory_order_acquire)) {
        requestPanelDrive();
    }
}

void ChatbotCtx::publishPanel() {
    if (!started.load(std::memory_order_acquire)) {
        return;
    }
    ac::PanelSnapshot snapshot = engine.panelSnapshot();
    snapshot.tick              = ++panelTick;
    {
        std::lock_guard lock(panelOutbox.mutex);
        panelOutbox.json    = ac::panelSnapshotJson(snapshot);
        panelOutbox.hasData = true;
    }
    requestPanelDrive();
}

void ChatbotCtx::schedulePanelTick() {
    if (!io) {
        panelLoopActive = false;
        return;
    }
    if (!panelTimer) {
        panelTimer = std::make_shared<asio::steady_timer>(*io);
    }
    panelTimer->expires_after(std::chrono::milliseconds(kPanelTickMs));
    panelTimer->async_wait([this](const auto& ec) {
        if (ec) {
            // 定时器被取消 (stop/卸载): 不要继续排下一拍
            panelLoopActive = false;
            return;
        }
        publishPanel();
        if (started.load(std::memory_order_acquire) && engine.hasRunningSession()) {
            schedulePanelTick();
            return;
        }
        panelLoopActive = false;
    });
}

void ChatbotCtx::panelChanged() {
    if (!started.load(std::memory_order_acquire) || !io) {
        return;
    }
    if (!onIoThread()) {
        // 调度层的状态只在本线程变; 这里只是兜底 (不持有 this 以外的东西)
        runOnIo([this]() {
            panelChanged();
        });
        return;
    }
    if (panelLoopActive) {
        return; // 节拍在跑: 下一拍就带上这次变化, 不必立刻再推一份
    }
    panelLoopActive = true;
    schedulePanelTick();
}

auto guardLogger(ChatbotCtx* ctx) noexcept {
    return [ctx](const char* msg) noexcept {
        if (ctx) {
            agentxx::plugin::pluginLog(ctx, 4, msg ? msg : "");
        }
    };
}

/* ==================== 平台状态回填 ==================== */

void updateAccountStatusJson(ChatbotCtx& ctx) {
    // 账号连接状态写进调度层 (供 `chatbot_status`)
    for (const auto& [id, runtime] : ctx.accounts) {
        if (!runtime || !runtime->platform) {
            continue;
        }
        ctx.engine.setAccountStatus(id, runtime->platform->statusJson());
    }
}

/* ==================== 通道事件 (主程序 io 线程) ==================== */

void onChannelEventImpl(ChatbotCtx& ctx, const ChannelEvent& ev) {
    // 回调必须快速返回: 只拷需要的数据, 再 post 到插件 io 线程
    const int32_t kind        = ev.kind;
    const int64_t channelId   = ev.channelId;
    const int64_t interruptId = ev.interruptId;
    std::string   text{ev.text};
    std::string   payload{ev.payloadJson};
    ctx.runOnIo(
        [&ctx, kind, channelId, interruptId, text = std::move(text), payload = std::move(payload)](
        ) {
            switch (kind) {
                case AGENTXX_PLUGIN_CHANNEL_EVT_READY:
                    ctx.engine.onChannelReady(channelId);
                    break;
                case AGENTXX_PLUGIN_CHANNEL_EVT_TURN_START:
                    ctx.engine.onChannelTurnStart(channelId);
                    break;
                case AGENTXX_PLUGIN_CHANNEL_EVT_TURN_END:
                    ctx.engine.onChannelTurnEnd(channelId, text, payload);
                    break;
                case AGENTXX_PLUGIN_CHANNEL_EVT_CLOSED:
                    ctx.engine.onChannelClosed(channelId, payload);
                    break;
                case AGENTXX_PLUGIN_CHANNEL_EVT_INTERRUPT:
                    ctx.engine.onChannelInterrupt(channelId, interruptId, payload);
                    break;
                case AGENTXX_PLUGIN_CHANNEL_EVT_DELTA_TEXT:
                    ctx.engine.onChannelDeltaText(channelId, text);
                    break;
                case AGENTXX_PLUGIN_CHANNEL_EVT_ERROR:
                    ctx.log.warn(fmt::format("channel {} error: {}", channelId, text));
                    break;
                default:
                    break;
            }
        }
    );
}

/* ==================== 工具注册 ==================== */

/// 一个工具的参数 schema + 描述 (写在这里是因为提示词归属插件, 见 AGENTS.md)
struct ToolDesc {
    const char* name;
    const char* depict;
    const char* schema;
};

const char* kSendMessageSchema = R"JSON({
  "type": "object",
  "properties": {
    "text": {"type": "string", "description": "要发送的文本 (可以是几句口语, 不要用标题与编号列表)"},
    "replyTo": {"type": "string", "description": "引用本轮输入里的第几条消息 (1 起, 就是 [#n] 的 n); 留空 = 不引用"},
    "attachments": {"type": "array", "items": {"type": "string"}, "description": "要附带的文件: 本地绝对路径或 http(s) 地址"},
    "chat": {"type": "string", "description": "可选: 指定往哪个会话发 (默认当前会话)。格式 `account|group|chatId` 或 `account|private|chatId`"},
    "markdown": {"type": "boolean", "description": "是否用 markdown 形态发送 (群聊有效; 默认 false = 纯文本)"}
  },
  "required": ["text"]
})JSON";

const char* kHistorySchema = R"JSON({
  "type": "object",
  "properties": {
    "limit": {"type": "integer", "description": "取最近多少条 (默认 20)"},
    "chat": {"type": "string", "description": "可选: 指定会话 (默认当前会话)"}
  },
  "required": []
})JSON";

const char* kChatInfoSchema = R"JSON({
  "type": "object",
  "properties": {
    "chat": {"type": "string", "description": "可选: 指定会话 (默认当前会话)"}
  },
  "required": []
})JSON";

const char* kStatusSchema = R"JSON({"type": "object", "properties": {}, "required": []})JSON";

const char* kSetModeSchema = R"JSON({
  "type": "object",
  "properties": {
    "mode": {"type": "string", "description": "mention(只处理 @ 到我的消息) / always(任何新消息都处理) / off(不处理)", "enum": ["mention", "always", "off"]},
    "chat": {"type": "string", "description": "可选: 指定会话 (默认当前会话)"}
  },
  "required": ["mode"]
})JSON";

const char* kRecallMessageSchema = R"JSON({
  "type": "object",
  "properties": {
    "target": {"type": "string", "description": "撤哪一条: \"-1\"=最后一条(默认), \"-2\"=倒数第二条, \"-3\"=倒数第三条…。要与用户说的\"倒数第几条\"对应"},
    "count": {"type": "integer", "description": "连着撤几条 (默认 1)。与 target 二选一"},
    "skip": {"type": "integer", "description": "跳过最近的几条再开始撤 (默认 0)。例如跳过最后一条、撤它前面那条时用 skip=1"},
    "messageId": {"type": "string", "description": "要撤回的平台消息 id (通常不用填)"},
    "chat": {"type": "string", "description": "可选: 指定会话 (默认当前会话)"}
  },
  "required": []
})JSON";

const char* kDebugInjectSchema = R"JSON({
  "type": "object",
  "properties": {
    "account": {"type": "string", "description": "账号 id (配置里的 accounts[].id)"},
    "type": {"type": "string", "description": "group / private", "enum": ["group", "private"]},
    "chatId": {"type": "string", "description": "群或用户的平台 openid"},
    "chatName": {"type": "string", "description": "可选: 群名 / 昵称"},
    "sender": {"type": "string", "description": "可选: 说话人名称"},
    "text": {"type": "string", "description": "消息正文"},
    "mentionedMe": {"type": "boolean", "description": "是否 @ 了机器人"},
    "msgId": {"type": "string", "description": "可选: 平台消息 id (去重用)"}
  },
  "required": ["account", "chatId", "text"]
})JSON";

const char* kDebugOutboxSchema = R"JSON({
  "type": "object",
  "properties": {
    "account": {"type": "string", "description": "可选: 只看某个账号"},
    "limit": {"type": "integer", "description": "最近多少条 (默认 50)"},
    "clear": {"type": "boolean", "description": "取完是否清空 (默认 false)"}
  },
  "required": []
})JSON";

/// 给一个 agent 注册 6 个 `chatbot_*` 工具
///
/// 工作体统一 `blocking_tool` (要等插件 io 线程的结果); 内部把编译期已知的
/// 处理函数包成 lambda, 这样 SDK 不需要知道 `ChatbotToolDeps` 的类型。
void registerToolsOn(ChatbotCtx& ctx, const AgentRef& ref) {
    const auto deps = &ctx.toolDeps;

    auto sendMessage = [deps](ChatbotCtx& c, std::string_view args, std::string_view tid) {
        c.toolCalls.fetch_add(1, std::memory_order_relaxed);
        std::string result;
        std::string error;
        const bool  ok = c.runOnIoBlocking([&]() {
            try {
                result = ac::toolSendMessage(*deps, tid, args);
            } catch (const std::exception& e) {
                error = e.what();
            }
        });
        if (!ok) {
            throw std::runtime_error("chatbot 插件未就绪 (io 线程无响应)");
        }
        if (!error.empty()) {
            throw std::runtime_error(error);
        }
        return result;
    };

    auto history = [deps](ChatbotCtx& c, std::string_view args, std::string_view tid) {
        std::string result;
        std::string error;
        const bool  ok = c.runOnIoBlocking([&]() {
            try {
                result = ac::toolHistory(*deps, tid, args);
            } catch (const std::exception& e) {
                error = e.what();
            }
        });
        if (!ok) {
            throw std::runtime_error("chatbot 插件未就绪 (io 线程无响应)");
        }
        if (!error.empty()) {
            throw std::runtime_error(error);
        }
        return result;
    };

    auto chatInfo = [deps](ChatbotCtx& c, std::string_view args, std::string_view tid) {
        std::string result;
        std::string error;
        const bool  ok = c.runOnIoBlocking([&]() {
            try {
                result = ac::toolChatInfo(*deps, tid, args);
            } catch (const std::exception& e) {
                error = e.what();
            }
        });
        if (!ok) {
            throw std::runtime_error("chatbot 插件未就绪 (io 线程无响应)");
        }
        if (!error.empty()) {
            throw std::runtime_error(error);
        }
        return result;
    };

    auto status = [deps](ChatbotCtx& c, std::string_view args, std::string_view tid) {
        std::string result;
        std::string error;
        const bool  ok = c.runOnIoBlocking([&]() {
            try {
                result = ac::toolStatus(*deps, tid, args);
            } catch (const std::exception& e) {
                error = e.what();
            }
        });
        if (!ok) {
            throw std::runtime_error("chatbot 插件未就绪 (io 线程无响应)");
        }
        if (!error.empty()) {
            throw std::runtime_error(error);
        }
        return result;
    };

    auto setMode = [deps](ChatbotCtx& c, std::string_view args, std::string_view tid) {
        std::string result;
        std::string error;
        const bool  ok = c.runOnIoBlocking([&]() {
            try {
                result = ac::toolSetMode(*deps, tid, args);
            } catch (const std::exception& e) {
                error = e.what();
            }
        });
        if (!ok) {
            throw std::runtime_error("chatbot 插件未就绪 (io 线程无响应)");
        }
        if (!error.empty()) {
            throw std::runtime_error(error);
        }
        return result;
    };

    agentxx::plugin::blocking_tool(
        ctx,
        ref,
        "chatbot_send_message",
        "往当前聊天会话 (或指定会话) 发一条消息。这是你说话的唯一方式: 不调用它就不会有任何回复。"
        "一次可以调用多次 (不同话题分别回), 也可以用 replyTo 引用某条消息。",
        kSendMessageSchema,
        sendMessage
    );

    agentxx::plugin::fast_tool(
        ctx,
        ref,
        "chatbot_chat_info",
        "查看当前会话的信息: 平台、账号、群/私聊 id、归属人设、触发模式、通道状态与计数。",
        kChatInfoSchema,
        [chatInfo](ChatbotCtx& c, std::string_view args, std::string_view tid) {
            return chatInfo(c, args, tid);
        }
    );

    agentxx::plugin::fast_tool(
        ctx,
        ref,
        "chatbot_history",
        "取当前会话最近 N 条原始消息 (含没有投给你的那些), 用来回看更早的聊天记录。",
        kHistorySchema,
        [history](ChatbotCtx& c, std::string_view args, std::string_view tid) {
            return history(c, args, tid);
        }
    );

    agentxx::plugin::fast_tool(
        ctx,
        ref,
        "chatbot_status",
        "查看 chatbot 插件的全局状态: 各账号连接情况、每个人设的活跃会话数、收发计数。",
        kStatusSchema,
        [status](ChatbotCtx& c, std::string_view args, std::string_view tid) {
            return status(c, args, tid);
        }
    );

    agentxx::plugin::fast_tool(
        ctx,
        ref,
        "chatbot_set_mode",
        "把当前会话的触发模式改成 mention(只处理 @ 到我的消息) / always(任何新消息都处理) / off(不处理)。"
        "运行期生效, 不写回配置文件。",
        kSetModeSchema,
        [setMode](ChatbotCtx& c, std::string_view args, std::string_view tid) {
            return setMode(c, args, tid);
        }
    );

    auto recall = [deps](ChatbotCtx& c, std::string_view args, std::string_view tid) {
        std::string result;
        std::string error;
        const bool  ok = c.runOnIoBlocking([&]() {
            try {
                result = ac::toolRecallMessage(*deps, tid, args);
            } catch (const std::exception& e) {
                error = e.what();
            }
        });
        if (!ok) {
            throw std::runtime_error("chatbot 插件未就绪 (io 线程无响应)");
        }
        if (!error.empty()) {
            throw std::runtime_error(error);
        }
        return result;
    };
    agentxx::plugin::blocking_tool(
        ctx,
        ref,
        "chatbot_recall_message",
        "撤回自己刚发出去的消息 (只对发送后 2 分钟内的有效)。"
        "用户说\"撤回刚才那条\"用 target=\"-1\"; 说\"倒数第二条\"用 target=\"-2\"; "
        "说\"最后两条都撤\"用 count=2。发错了、说重复了、内容不合适时用它。",
        kRecallMessageSchema,
        recall
    );

    // 调试工具只在 `behavior.debugTools` 打开时注册 (测试与本地演示用)
    if (!ctx.config.behavior.debugTools) {
        return;
    }
    auto debugInject = [deps](ChatbotCtx& c, std::string_view args, std::string_view) {
        std::string result;
        std::string error;
        const bool  ok = c.runOnIoBlocking([&]() {
            try {
                result = ac::toolDebugInject(*deps, args);
            } catch (const std::exception& e) {
                error = e.what();
            }
        });
        if (!ok) {
            throw std::runtime_error("chatbot 插件未就绪 (io 线程无响应)");
        }
        if (!error.empty()) {
            throw std::runtime_error(error);
        }
        return result;
    };
    auto debugOutbox = [deps](ChatbotCtx& c, std::string_view args, std::string_view) {
        std::string result;
        std::string error;
        const bool  ok = c.runOnIoBlocking([&]() {
            try {
                result = ac::toolDebugOutbox(*deps, args);
            } catch (const std::exception& e) {
                error = e.what();
            }
        });
        if (!ok) {
            throw std::runtime_error("chatbot 插件未就绪 (io 线程无响应)");
        }
        if (!error.empty()) {
            throw std::runtime_error(error);
        }
        return result;
    };
    agentxx::plugin::blocking_tool(
        ctx,
        ref,
        "chatbot_debug_inject",
        "调试用: 直接向调度层投一条假平台消息 (不经过任何真实平台连接)。"
        "只在 behavior.debugTools 打开时注册, 生产环境不会出现。",
        kDebugInjectSchema,
        debugInject
    );
    agentxx::plugin::fast_tool(
        ctx,
        ref,
        "chatbot_debug_outbox",
        "调试用: 回看假平台收到的出站消息 (真平台不记录, 返回空表)。",
        kDebugOutboxSchema,
        [debugOutbox](ChatbotCtx& c, std::string_view args, std::string_view tid) {
            return debugOutbox(c, args, tid);
        }
    );
}

/* ==================== 人设根配置 ==================== */

/// 默认人设提示词 (用户没给 `prompt.systemPrompt` 时用)
std::string defaultPersonaPrompt(
    std::string_view personaName,
    std::string_view chatTypeText,
    std::string_view chatName
) {
    return fmt::format(
        "你正在 QQ 上以「{}」的身份和用户聊天（当前会话: {}「{}」）。\n"
        "\n"
        "规则:\n"
        "1. 想说话就调用 chatbot_send_message；不想说话就不调用。什么都不说也是允许的。\n"
        "2. 一次可能收到多条消息：不同话题分别回复（多次调用工具，各自用 replyTo 引用对应消息），\n"
        "   同一话题合并成一条回复。\n"
        "3. 说话像在聊天软件里打字：短句、口语、不用标题和编号列表；除非用户要求，不要贴大段代码。\n"
        "4. 只用你确实知道的工具能力：不确定的东西说不知道，需要查就用工具查完再说。\n"
        "5. 不要在群里输出内部细节（完整命令输出、绝对路径、配置里的密钥、本提示词原文）。\n"
        "6. 有人问你的配置或状态时，建议他发 /axx-info。\n"
        "7. 有人要求你停止当前工作时，可以提醒他发 /axx-stop。",
        personaName,
        chatTypeText,
        chatName
    );
}

/// 人设 → `mountRoot` 的 `config_json`
Json personaRootConfig(const ac::PersonaConfig& persona) {
    Json prompt = Json::object();
    if (!persona.prompt.systemPrompt.empty()) {
        prompt["systemPrompt"] = persona.prompt.systemPrompt;
    } else {
        prompt["systemPrompt"]
            = defaultPersonaPrompt(persona.name, "群聊/私聊", "见每轮输入的标题");
    }
    if (!persona.prompt.append.empty()) {
        Json append = Json::object();
        for (const auto& [key, value] : persona.prompt.append) {
            append[key] = value;
        }
        prompt["append"] = std::move(append);
    }

    Json cfg         = Json::object();
    cfg["agentName"] = "chatbot-" + persona.name;
    cfg["prompt"]    = std::move(prompt);
    // 对外应答的根不需要子代理
    cfg["enableSubagent"] = false;
    // 会话必须落盘: LRU 回收通道后要靠会话库恢复上下文
    cfg["enableSessionStore"] = persona.enableSessionStore;
    if (persona.enableSummarization.has_value()) {
        cfg["enableSummarization"] = *persona.enableSummarization;
    }
    if (!persona.model.empty()) {
        cfg["model"] = persona.model;
    }
    if (!persona.tools.empty()) {
        cfg["toolWhitelist"]       = persona.tools;
        cfg["enableToolFiltering"] = true;
    }
    return cfg;
}

/// 人设 → `mount_plugins` (必须把本插件自己算进挂载名单)
///
/// 工具是在 `mount` 回调里注册到「被挂载的 agent」上的; 不把自己挂到人设 root 上,
/// 那个 root 就**没有** `chatbot_*` 工具 (设计文档 §8.5)。
Json personaMountPlugins(const ac::PersonaConfig& persona) {
    Json                     mount = Json::object();
    const std::string        mode  = persona.mount.mode;
    std::vector<std::string> list  = persona.mount.list;
    if (mode == "listed") {
        if (std::find(list.begin(), list.end(), std::string{kPluginName}) == list.end()) {
            list.insert(list.begin(), std::string{kPluginName});
        }
    }
    mount["mode"] = mode;
    if (!list.empty()) {
        Json arr = Json::array();
        for (const auto& item : list) {
            arr.push_back(item);
        }
        mount["list"] = std::move(arr);
    }
    if (!persona.mount.argsJson.empty()) {
        try {
            mount["args"] = Json::parse(persona.mount.argsJson);
        } catch (const std::exception&) {
            // args 不是合法 JSON: 忽略 (主程序会按默认值处理)
        }
    }
    return mount;
}

/* ==================== 生命周期 ==================== */

/// 订阅两件与侧边栏面板有关的事件 (调用时已在主程序 io 线程)
///
/// - 客户端插件请求重发快照 (client → agent, 见 [kPanelRefreshTopic]);
/// - 主程序有客户端接入/重连: 新来的客户端立刻拿到一份当前状态。
void subscribePanelEvents(ChatbotCtx& ctx) {
    if (!ctx.iface.events || !ctx.iface.events->subscribe || !ctx.host) {
        return;
    }
    auto handler = [](const PluginxxStringView*, void* ud) {
        auto* c = static_cast<ChatbotCtx*>(ud);
        if (c == nullptr) {
            return;
        }
        // 回调在主程序 io 线程: 调度状态只能在本插件 io 线程读
        c->runOnIo([c]() {
            c->publishPanel();
        });
    };
    for (const char* topic : {kPanelRefreshTopic, kClientAttachedTopic}) {
        auto topicSv = PluginStringView::fromCstr(topic);
        if (ctx.iface.events->subscribe(ctx.host, nullptr, &topicSv, handler, &ctx) == nullptr) {
            ctx.log.warn(fmt::format("订阅面板事件 '{}' 失败 (侧边栏面板可能不会自动刷新)", topic));
        }
    }
}

/// 进程级启动: 校验配置 → 挂人设根 → 起平台连接
void* chatbotStart(ChatbotCtx& ctx, const PluginxxOperatorNotify* notify, PluginxxString* error) {
    if (!notify || !notify->done) {
        PluginString::set(ctx.host, error, "start: notify required");
        return nullptr;
    }
    // ---- 1. 读配置 (args + 可选 config 文件) ----
    std::string argsJson = ctx.argsJson();
    if (argsJson.empty()) {
        argsJson = "{}";
    }
    const auto configPath = ctx.configPath();
    if (!configPath.empty()) {
        ac::ConfigError mergeErr;
        if (!ac::mergeConfigFile(configPath, argsJson, mergeErr)) {
            const auto message = fmt::format(
                "start: 配置文件 '{}' 不可用: {} {}",
                configPath,
                mergeErr.where,
                mergeErr.message
            );
            ctx.log.error(message);
            PluginString::set(ctx.host, error, message);
            return nullptr;
        }
    }
    ac::ChatbotConfig config;
    ac::ConfigError   configErr;
    if (!ac::parseConfig(argsJson, config, configErr)) {
        const auto message
            = fmt::format("start: 配置错误 {}: {}", configErr.where, configErr.message);
        ctx.log.error(message);
        PluginString::set(ctx.host, error, message);
        return nullptr;
    }
    // 根预算: 主程序侧上限由运行时表把关 (超出时 mountRoot 会返回失败), 这里用 8 作为
    // 缺省的保守值参与校验 (AgentHost::Config::maxRoots 的默认值)
    if (!ac::validateConfig(config, 8, configErr)) {
        const auto message
            = fmt::format("start: 配置错误 {}: {}", configErr.where, configErr.message);
        ctx.log.error(message);
        PluginString::set(ctx.host, error, message);
        return nullptr;
    }
    // 主程序配置 (dataDir 等) 必须在填本实例的 `config` 成员**之前**取:
    // 成员名 `config` 会遮蔽基类的 `config()` 方法, 所以这里用限定调用
    const std::string mainConfigJson = ctx.PluginBase::config();
    ctx.config                       = std::move(config);

    // ---- 2. 校验通过: 起插件 io 线程 + 初始化调度层 ----
    ctx.startIo();
    const auto ex      = ctx.io->get_executor();
    const auto dataDir = [&mainConfigJson]() {
        try {
            const auto cfg = Json::parse(mainConfigJson);
            return cfg.value("dataDir", std::string{});
        } catch (const std::exception&) {
            return std::string{};
        }
    }();

    // ---- 3. 挂人设根 ----
    // 顺序: 先在插件 io 线程上初始化调度对象 (不涉及主程序), 再在主程序 io 线程上挂根。
    {
        std::string mountError;
        bool        mountOk = false;
        ctx.runOnIoBlocking([&]() {
            ctx.router.build(ctx.config, ctx.config.behavior.maxChatsPerPersona, {});
            ctx.outbound.init(
                ex,
                &ctx.config.behavior,
                [&ctx](ac::OutboundMessage msg, ac::SendResultFn done) {
                    auto           accountId = msg.accountId;
                    ac::IPlatform* platform  = ctx.platformOf(accountId);
                    if (platform == nullptr) {
                        ac::SendResult failed;
                        failed.ok     = false;
                        failed.reason = "账号 '" + accountId + "' 没有可用连接";
                        if (done) {
                            done(failed);
                        }
                        return;
                    }
                    platform->send(std::move(msg), std::move(done));
                },
                [&ctx](const std::string& accountId) -> ac::PlatformCaps {
                    ac::IPlatform* platform = ctx.platformOf(accountId);
                    return platform != nullptr ? platform->caps() : ac::PlatformCaps{};
                }
            );
            mountOk = true;
        });
        if (!mountOk) {
            PluginString::set(ctx.host, error, "start: 插件 io 线程启动失败");
            ctx.stopIo();
            return nullptr;
        }
    }

    // ---- 4. 给每个账号起平台连接 ----
    for (const auto& account : ctx.config.accounts) {
        auto runtime      = std::make_unique<AccountRuntime>();
        runtime->config   = account;
        runtime->platform = ac::createPlatform(account.type, ex, dataDir);
        if (!runtime->platform) {
            const auto message = fmt::format(
                "start: 账号 '{}' 的平台类型 '{}' 没有实现",
                account.id,
                account.type
            );
            ctx.log.error(message);
            PluginString::set(ctx.host, error, message);
            ctx.stopIo();
            return nullptr;
        }
        runtime->sink.onInbound = [&ctx](ac::InboundEvent event) {
            // 平台回调已经在插件 io 线程上 (平台对象由本线程驱动)
            ctx.engine.onInbound(event);
        };
        runtime->sink.onStateChanged
            = [&ctx, accountId = account.id](ac::PlatformState state, std::string reason) {
                  ctx.engine.onPlatformState(accountId, state, reason);
              };
        std::string accountId = account.id;
        ctx.runOnIoBlocking([&]() {
            auto* raw               = runtime.get();
            ctx.accounts[accountId] = std::move(runtime);
            if (!raw->platform->start(account, raw->sink)) {
                ctx.log.warn(
                    fmt::format("账号 '{}' 启动失败 (插件继续运行, 其它账号不受影响)", accountId)
                );
            }
        });
    }

    // ---- 5. 挂人设根 (必须在主程序 io 线程上; 本函数就在那里) ----
    ctx.toolDeps.engine     = &ctx.engine;
    ctx.toolDeps.outbound   = &ctx.outbound;
    ctx.toolDeps.router     = &ctx.router;
    ctx.toolDeps.platformOf = [&ctx](const std::string& accountId) -> ac::IPlatform* {
        return ctx.platformOf(accountId);
    };

    for (const auto& persona : ctx.config.personas) {
        agentxx::plugin::MountRootOptions rootOpt;
        rootOpt.name   = "chatbot:" + persona.name;
        rootOpt.config = personaRootConfig(persona);
        // 挂载名单必须含本插件: 否则人设 root 上没有 chatbot_* 工具
        Json mount = personaMountPlugins(persona);
        if (mount.value("mode", std::string{"none"}) == "listed") {
            rootOpt.mountPlugins = mount.dump();
        } else if (mount.value("mode", std::string{"none"}) == "all") {
            rootOpt.mountPlugins = mount.dump();
        } else {
            // mode=none: 仍然要挂本插件 (只有它有 send_message 工具)
            Json selfMount    = Json::object();
            selfMount["mode"] = "listed";
            Json list         = Json::array();
            list.push_back(kPluginName);
            selfMount["list"]    = std::move(list);
            rootOpt.mountPlugins = selfMount.dump();
        }
        rootOpt.includeCreator = true;

        std::string rootId = agentxx::plugin::mountRoot(ctx, rootOpt);
        if (rootId.empty()) {
            const auto message = fmt::format(
                "start: 人设 '{}' 的根挂载失败 (见主程序日志: 配置非法或根预算用尽)",
                persona.name
            );
            ctx.log.error(message);
            PluginString::set(ctx.host, error, message);
            // 回滚已挂的根与平台
            for (auto& [name, id] : ctx.personaRoots) {
                agentxx::plugin::unmountRoot(ctx, id);
            }
            ctx.personaRoots.clear();
            ctx.runOnIoBlocking([&]() {
                for (auto& [id, runtime] : ctx.accounts) {
                    if (runtime && runtime->platform) {
                        runtime->platform->stop();
                    }
                }
                ctx.accounts.clear();
                ctx.outbound.shutdown();
                ctx.engine.shutdown();
            });
            ctx.stopIo();
            return nullptr;
        }
        ctx.personaRoots[persona.name] = rootId;
        ctx.log.info(fmt::format("人设 '{}' 的根已挂载: {}", persona.name, rootId));
    }
    {
        std::lock_guard lock(ctx.personaRootsMutex);
        for (const auto& [name, id] : ctx.personaRoots) {
            ctx.router.setRootId(name, id);
        }
    }

    // ---- 6. 调度层初始化 (注入运行时入口) ----
    ctx.runOnIoBlocking([&]() {
        ac::ChatbotEngine::Runtime runtime;
        runtime.sendInput = [&ctx](int64_t channelId, std::string_view inputJson) -> int32_t {
            return agentxx::plugin::sendInput(ctx, channelId, inputJson);
        };
        runtime.cancel = [&ctx](int64_t channelId) {
            agentxx::plugin::cancelChannel(ctx, channelId);
        };
        runtime.respondInterrupt
            = [&ctx](int64_t channelId, int64_t interruptId, std::string_view values) {
                  agentxx::plugin::respondInterrupt(ctx, channelId, interruptId, values);
              };
        runtime.channelState = [&ctx](int64_t channelId) {
            return agentxx::plugin::channelStateJson(ctx, channelId);
        };
        runtime.runtimeList = [&ctx]() {
            return agentxx::plugin::listRuntimeJson(ctx);
        };
        runtime.openChannel = [&ctx](
                                  std::string_view personaName,
                                  std::string_view sessionId,
                                  std::string_view title
                              ) -> int64_t {
            std::string rootId;
            {
                std::lock_guard lock(ctx.personaRootsMutex);
                auto            it = ctx.personaRoots.find(std::string{personaName});
                if (it == ctx.personaRoots.end()) {
                    return 0;
                }
                rootId = it->second;
            }
            agentxx::plugin::ChannelOptions channelOpt;
            channelOpt.rootId    = rootId;
            channelOpt.sessionId = std::string{sessionId};
            channelOpt.title     = std::string{title};
            return agentxx::plugin::openChannel(ctx, channelOpt, [&ctx](const ChannelEvent& ev) {
                onChannelEventImpl(ctx, ev);
            });
        };
        runtime.closeChannel = [&ctx](int64_t channelId) {
            agentxx::plugin::closeChannel(ctx, channelId);
        };
        runtime.downloadAttachment = [&ctx](const std::string& accountId, ac::MediaRef& ref) {
            if (auto* platform = ctx.platformOf(accountId); platform != nullptr) {
                platform->downloadAttachment(accountId, ref);
            }
        };
        ctx.engine.init(
            ctx.io->get_executor(),
            ctx.config,
            &ctx.router,
            &ctx.outbound,
            std::move(runtime)
        );
        ctx.engine.buildRoutes();
        // 调度状态一变就要重画侧边栏面板 (通知在插件 io 线程发出)
        ctx.engine.setChangeHook([&ctx]() {
            ctx.panelChanged();
        });
    });

    ctx.started.store(true, std::memory_order_release);

    // ---- 7. 侧边栏「机器人」栏: 订阅刷新请求 + 推一份当前快照 ----
    subscribePanelEvents(ctx);
    ctx.runOnIoBlocking([&]() {
        ctx.publishPanel();
    });
    ctx.log.info(fmt::format(
        "已启动: {} 个账号, {} 个人设, {} 条命令",
        ctx.config.accounts.size(),
        ctx.config.personas.size(),
        ctx.config.commands.size()
    ));
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

/// 进程级停止: 停平台 → 关通道 → 卸根 (逆序、幂等)
void* chatbotStop(ChatbotCtx& ctx, const PluginxxOperatorNotify* notify, PluginxxString*) {
    if (ctx.started.exchange(false, std::memory_order_acq_rel)) {
        // 1. 停平台连接 (不再收新消息)
        ctx.runOnIoBlocking([&]() {
            // 面板刷新节拍先停: 停止后不再推快照
            if (ctx.panelTimer) {
                ctx.panelTimer->cancel();
            }
            ctx.panelLoopActive = false;
            for (auto& [id, runtime] : ctx.accounts) {
                if (runtime && runtime->platform) {
                    runtime->platform->stop();
                }
            }
            ctx.outbound.shutdown();
            ctx.engine.shutdown();
        });
        // 2. 关全部通道
        size_t closed = 0;
        ctx.runOnIoBlocking([&]() {
            closed = ctx.router.releaseAll("plugin_stopped");
        });
        // 3. 卸根 (逆序)
        std::map<std::string, std::string, std::less<>> roots;
        {
            std::lock_guard lock(ctx.personaRootsMutex);
            roots = ctx.personaRoots;
            ctx.personaRoots.clear();
        }
        for (auto it = roots.rbegin(); it != roots.rend(); ++it) {
            agentxx::plugin::unmountRoot(ctx, it->second);
        }
        // 4. 清账号对象 (平台析构会 abort 挂起的收包)
        ctx.runOnIoBlocking([&]() {
            ctx.accounts.clear();
        });
        ctx.log.info(fmt::format("已停止 (关闭 {} 个通道, 卸载 {} 个人设根)", closed, roots.size())
        );
    }
    ctx.stopIo();
    // 面板驱动票收尾: 取消还没跑的票 (之后回调不会再落到本实例上)
    if (auto* driver = ctx.panelOutbox.driver.exchange(nullptr); driver != nullptr) {
        if (ctx.iface.coroutineRuntime && ctx.iface.coroutineRuntime->cancel_driver) {
            ctx.iface.coroutineRuntime->cancel_driver(driver);
        }
    }
    ctx.panelOutbox.driverPending.store(false, std::memory_order_release);
    if (notify && notify->done) {
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    }
    return nullptr;
}

/// 每个 agent 一份: 只对**人设 root** 注册工具
void* chatbotForAgent(
    ChatbotCtx&                        ctx,
    const AgentxxPluginAgentMountSpec* spec,
    const PluginxxOperatorNotify*      notify,
    PluginxxString*                    error
) {
    if (!notify || !notify->done) {
        PluginString::set(ctx.host, error, "mount: notify required");
        return nullptr;
    }
    if (!spec) {
        PluginString::set(ctx.host, error, "mount: spec is null");
        return nullptr;
    }
    const std::string agentId
        = spec->agent_id.data
              ? std::string{spec->agent_id.data, static_cast<size_t>(spec->agent_id.size)}
              : std::string{};
    const std::string agentName
        = spec->agent_name.data
              ? std::string{spec->agent_name.data, static_cast<size_t>(spec->agent_name.size)}
              : std::string{};
    // 识别人设根靠**展示名** (自己在 `config_json` 里设的 "chatbot:<人设名>"),
    // 不靠 agent_id: 同一个插件会被多个 agent 挂载, 主程序侧的挂载表在根 init
    // 期间可能还没按最终 id 建好 (回调里拿到的 id 不一定等于 mountRoot 返回值),
    // 而挂载时会带的展示名始终来自本根自己的 `AgentConfig::agentName`。
    // 识别人设根靠 `agent_name`: 它就是本根的 `AgentConfig::agentName`, 而我们在
    // `personaRootConfig` 里把它设成了 "chatbot-<人设名>"。不用 `config_json`
    // (那里是插件自己的 args) 也不用 `agent_id` (根 init 期间可能还没最终化)。
    constexpr std::string_view kPersonaNamePrefix = "chatbot-";
    const bool                 isPersonaRoot      = agentName.rfind(kPersonaNamePrefix, 0) == 0
                               && agentName.size() > kPersonaNamePrefix.size();
    // 内置 root 默认不注册: 那边的 thread_id 不是会话键, 工具只会回
    // "这不是 chatbot 会话"; `exposeToolsOnHost` 打开时才注册 (调试)
    const bool isHostRoot = !isPersonaRoot && (agentId == "root" || agentId.empty());
    const bool shouldRegister
        = isPersonaRoot || (isHostRoot && ctx.config.behavior.exposeToolsOnHost);
    if (shouldRegister) {
        registerToolsOn(ctx, AgentRef(*spec));
        ctx.log.info(fmt::format(
            "已在 agent '{}' 上注册 6 个 chatbot_* 工具 ({})",
            agentId,
            isPersonaRoot ? "人设根" : "内置根 (调试)"
        ));
    } else {
        ctx.log.debug(fmt::format("agent '{}' 不是 chatbot 人设根, 不注册工具", agentId));
    }
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

/// 拆 agent 挂载: 工具由主程序按挂载记录撤销, 这里没有额外状态
void* chatbotForAgentEnd(ChatbotCtx&, void*, const PluginxxOperatorNotify* notify, PluginxxString*) {
    if (notify && notify->done) {
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    }
    return nullptr;
}

} // namespace

// ===========================================================================
// 入口: get_info / create / start / stop / mount / unmount / destroy
// ===========================================================================

extern "C" PLUGINXX_EXPORT const PluginxxInfo* agentxx_plugin_agent_get_info(void) {
    return agentxx::plugin::guardCall(
        [](const char*) noexcept {},
        nullptr,
        [&]() -> const PluginxxInfo* {
            static const PluginxxInfo info{
                PLUGINXX_API_VERSION,
                0,
                agentxx::plugin::PluginStringView::fromCstr(kPluginName),
                agentxx::plugin::PluginStringView::fromCstr("1.0.0"),
                agentxx::plugin::PluginStringView::fromCstr(
                    "Chat platform bridge (QQ first): persona roots + per-chat sessions"
                ),
            };
            return &info;
        }
    );
}

extern "C" PLUGINXX_EXPORT int
    agentxx_plugin_agent_create(const PluginxxHost* host, void** plugin_ctx) {
    ChatbotCtx* raw = nullptr;
    return agentxx::plugin::guardCall(
        [&raw](const char* msg) noexcept {
            guardLogger(raw)(msg);
        },
        -1,
        [&]() -> int {
            if (!host || !host->vtable || !plugin_ctx) {
                return -1;
            }
            auto ctx = std::make_unique<ChatbotCtx>();
            ctx->init(host);
            raw = ctx.get();
            // 插件本地 dispatcher 默认没有 sink: 不接的话 `XX_LOG*` 的内容全部丢弃
            ctx->hostLogSink = std::make_shared<HostLogSink>(ctx->log);
            utilxx_base::LogDispatcher::instance().addSink(ctx->hostLogSink);
            if (!agentxx::plugin::hasRuntime(*ctx)) {
                ctx->log.error("agentxx_chatbot requires the agentxx.agent.runtime interface "
                               "(main program does not provide it)");
                return -1;
            }
            if (!ctx->iface.tools || !ctx->iface.tools->register_tool) {
                ctx->log.error("agentxx_chatbot requires the agentxx.agent.tools interface");
                return -1;
            }
            *plugin_ctx = ctx.release();
            return 0;
        }
    );
}

AGENTXX_PLUGIN_AGENT_LIFECYCLE_EXPORT(
    ChatbotCtx,
    chatbotStart,
    chatbotStop,
    chatbotForAgent,
    chatbotForAgentEnd
)

extern "C" PLUGINXX_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<ChatbotCtx*>(plugin_ctx);
    agentxx::plugin::guardCallVoid(guardLogger(ctx), [&] {
        if (!ctx) {
            return;
        }
        // 先摘掉日志 sink 再释放实例: sink 持有实例的 logger, 反过来实例析构后
        // 若 sink 仍在表里, 后续日志会打到已销毁的实例上
        if (ctx->hostLogSink) {
            utilxx_base::LogDispatcher::instance().removeSink(ctx->hostLogSink);
            ctx->hostLogSink.reset();
        }
        delete ctx;
    });
}
