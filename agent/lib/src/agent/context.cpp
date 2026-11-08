#include "agentxx/agent/context.h"
#include "agentxx/agent/io/agent_io.h"
#include "agentxx/agent/model_registry.h"
#include "agentxx/agent/session_store.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/tools/subagent.h"
#include "agentxx/util/neograph_json_bridge.h"
#include "agentxx/util/observability.h"
#include "neograph/graph/registry.h"
#include "utilxx/async_offload.h"
#include "utilxx_base/container_util.h"
#include "utilxx_base/log.h"
#include <chrono>
#include <filesystem>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <optional>

namespace agentxx {
namespace agent {

namespace {

/// 当前 steady 时钟毫秒数 (节流时间戳用, 单调不受系统时钟调整影响)
int64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()
    )
        .count();
}

/// 持久化写入失败时提示一次
///
/// - 仅在"同一失败原因尚未提示过"时推送一条 `WireDelta::MessageUITip`
///   (客户端插入提示消息), 避免每次写失败都刷屏
/// - 提示走增量通道而不改写会话历史: 该函数由持久化回调内部调用, 若再往
///   历史里追加消息会与持久化回调相互递归
/// - io 线程调用 (与 Session 写入同一线程)
void reportPersistFailure(
    const std::shared_ptr<Session>&      session,
    const std::shared_ptr<SessionStore>& store
) {
    if (!session || !store) {
        return;
    }
    const auto err = store->lastWriteError();
    if (err.empty() || err == session->lastPersistWarning()) {
        return;
    }
    session->setLastPersistWarning(err);
    XX_LOGW("Session: persistence degraded ({}): {}", session->lastPersistWarning(), err);
    if (!session->io) {
        return;
    }
    auto delta = WireDelta{
        .text    = fmt::format("Persistence degraded: {} (changes may not be saved)", err),
        .type    = WireDelta::Type::MessageUITip,
        .tipType = WireDelta::TipType::Warning,
    };
    delta.seq = session->nextDeltaSeq();
    session->io->sendToPeer(std::move(delta));
}

/// 工具行参数自愈 (展示历史加载/分页时使用): 只处理 Tool 角色且有参数的行
/// - 回调由 BaseAgent 注入 (按工具 schema 修正参数类型), 未注入/无 schema/无变化时
///   行保持原样
void normalizeToolViewArgsIn(
    const std::function<std::string(std::string_view, std::string_view)>& normalizer,
    std::vector<ViewMessage>&                                            rows
) {
    if (!normalizer) {
        return;
    }
    for (auto& row : rows) {
        if (row.role != ViewMessage::Role::Tool || !row.tool || row.text.empty()) {
            continue;
        }
        auto fixed = normalizer(std::string_view{row.tool->toolName}, std::string_view{row.text});
        if (fixed != row.text) {
            row.text = std::move(fixed);
        }
    }
}

/// 组装会话持久化回调 (每次写入后检查写失败并提示一次)
/// - 捕获 sessionId 副本, 回调生命周期随 session, 无悬垂风险;
///   session 用 weak_ptr 避免回调与 session 互相持有
SessionStoreHooks makeSessionStoreHooks(
    const std::shared_ptr<SessionStore>& store,
    std::string                          sessionId,
    const std::shared_ptr<Session>&      session,
    std::function<std::string(std::string_view, std::string_view)> toolArgsNormalizer
) {
    std::weak_ptr<Session> weakSession = session;
    return SessionStoreHooks{
        .onAppendViewMessage =
            [store,
             sessionId,
             weakSession](const ViewMessage& msg, uint64_t counter, uint64_t seq) {
                store->appendViewMessage(sessionId, msg, counter, seq);
                // 自动标题 (首条用户消息预览) 上一步已随消息写入会话库: 同步到会话统计,
                // 使界面 (信息侧边栏) 无需等下次从库里取就能显示标题
                // (已有标题时不覆盖: 改名优先, 且缓存已非空)
                // 事件投递的输入同样算作输入消息, 否则事件开头的会话永远没有标题
                if ((msg.role == ViewMessage::Role::User
                     || msg.role == ViewMessage::Role::Event)
                    && !msg.text.empty()) {
                    if (auto sessionPtr = weakSession.lock();
                        sessionPtr && sessionPtr->contextStats
                        && sessionPtr->contextStats->title.empty()) {
                        auto title = sessionTitlePreview(msg.text);
                        if (!title.empty()) {
                            sessionPtr->contextStats->title = std::move(title);
                        }
                    }
                }
                reportPersistFailure(weakSession.lock(), store);
            },
        .onUpdateViewMessage =
            [store, sessionId, weakSession](const ViewMessage& msg) {
                store->updateViewMessage(sessionId, msg);
                reportPersistFailure(weakSession.lock(), store);
            },
        .onLoadViewPage =
            [store, sessionId, toolArgsNormalizer = std::move(toolArgsNormalizer)](
                uint64_t start,
                uint32_t count
            ) {
                auto page = store->loadViewMessagesPage(sessionId, start, count);
                // 分页读出的工具行与恢复尾窗同一处理: 更早的历史也按当前工具 schema
                // 修正参数类型 (库内旧行是模型原文)
                normalizeToolViewArgsIn(toolArgsNormalizer, page);
                return page;
            },
        .onSaveLlmMessages =
            [store, sessionId, weakSession](std::string_view msgsText) {
                store->saveLlmMessages(sessionId, msgsText);
                reportPersistFailure(weakSession.lock(), store);
            },
    };
}

/// 拼接提示词段 (空段跳过; 段间用一个空行分隔)
void appendPromptSegment(std::string& combined, const std::string& segment) {
    if (segment.empty()) {
        return;
    }
    if (!combined.empty()) {
        if (combined.back() != (char)0x0a) {
            combined += "\n";
        }
        if (combined.size() < 2 || combined.compare(combined.size() - 2, 2, "\n\n") != 0) {
            combined += "\n";
        }
    }
    combined += segment;
}

/// Json 边界形态 -> typed 上下文 (逐条 ChatMessage JSON 反序列化)
std::vector<neograph::ChatMessage> messagesFromJson(const utilxx_base::Json& msgs) {
    std::vector<neograph::ChatMessage> typed;
    if (!msgs.is_array()) {
        return typed;
    }
    typed.reserve(msgs.size());
    for (const auto& item : msgs) {
        neograph::ChatMessage msg;
        auto                  neoItem = agentxx::util::toNeographJson(item);
        neograph::from_json(neoItem, msg);
        typed.push_back(std::move(msg));
    }
    return typed;
}

} // namespace

AgentContext::AgentContext() = default;

asio::awaitable<bool> AgentContext::shutdownPluginsAsync(std::chrono::milliseconds timeout) {
    if (!pluginManager) {
        co_return true;
    }
    // 先拆本 agent 的挂载 (驱动插件 unmount + 摘 agent 级注册 + 从主程序的挂载表移除)
    if (pluginMount) {
        co_await pluginManager->unmountAgentAsync(pluginMount->agentId());
        pluginMount.reset();
    }
    // 管理器是共享的 (容器/主程序注入, pluginManagerOwned=false) 时到此为止:
    // `shutdownAsync` 是把管理器里的**全部**插件实例卸掉, 一旦对共享管理器调用,
    // 销毁任何一个 agent (包括插件自己建的人设根) 都会把创建它的插件一起卸掉。
    // 共享管理器的关闭由它的主人负责 (AgentHost / 主程序调用 shutdownAsync)。
    if (!pluginManagerOwned) {
        co_return true;
    }
    co_return co_await pluginManager->shutdownAsync(timeout);
}

AgentContext::~AgentContext() {
    // 挂载必须随 agent 一起拆: 残留的挂载会让插件以为这个 agent 还在
    if (pluginMount) {
        XX_LOGW(
            "AgentContext destroyed with plugin mount '{}' still attached; "
            "the owner should await agent->shutdownAsync() first",
            pluginMount->agentId()
        );
        pluginMount.reset();
    }
    // 插件管理器先卸载全部插件, 断开中间件↔实例循环引用
    // (handles 由 middlewareHandleContext 持有, 其析构晚于 pluginManager)
    // - 只处理自建的管理器: 共享管理器 (AgentHost 注入) 属于主程序,
    //   其它 agent 还在用它, 不能随本上下文卸掉全部插件
    if (pluginManager && pluginManagerOwned) {
        pluginManager->shutdownAll();
        if (pluginManager->hasPendingClose()) {
            // 析构无法等待异步 stop 事务: 这些实例保持 CloseFailed 并保留
            // 上下文/动态库。owner 应在停止 IO executor 前 await shutdownAsync()。
            XX_LOGW("AgentContext destroyed with plugins still closing; owner should await "
                    "agent->shutdownAsync() before stopping the agent IO executor");
        }
    }
}

Session::~Session() {
    // 析构线程安全守卫:
    // - pendingViewOps_ 与 hooks_ 严格约定仅在 bound io 线程访问
    // - 若 Session 在非 io 线程析构 (如 SessionsManager::remove 或 AgentHost::destroyAgent
    //   在其他线程释放最后引用), 禁止跨线程触发 SQLite 落库或并发读写 pendingViewOps_;
    //   此时主程序/进程正在拆卸, 直接丢弃待落盘操作并记录警告
    if (isIoThread()) {
        flushPendingViewOps();
    } else {
        if (!pendingViewOps_.empty()) {
            XX_LOGW(
                "Session destructor called off io thread; discarding {} pending view ops",
                pendingViewOps_.size()
            );
            pendingViewOps_.clear();
        }
    }
}

std::string Session::appendViewMessage(ViewMessage msg, std::string_view reuseId) {
    // 强制校验: 展示历史窗口/msgIdCounter_ 仅允许 io 线程写入
    assertIoThread();

    std::string id;
    if (reuseId.empty()) {
        id = fmt::format("msg_{:06d}", ++msgIdCounter_);
    } else {
        // 定稿路径: 沿用直播槽预留的 id (计数器在 beginStreamingView 已推进)
        id = std::string{reuseId};
    }
    msg.id = id;
    // 展示历史持久化序号: 会话内单调递增, 与库里 view_message.seq 一致
    // (绝对展示下标 = seq - 1)
    const auto viewSeq = ++viewSeqCounter_;
    viewWindow_.push_back(std::move(msg));
    // 维护 msgId → 绝对下标索引 (updateViewMessage 的 O(1) 定位用)
    msgIndex_.insert_or_assign(id, viewSeq - 1);
    // 持久化 (节流, 尽力而为): 压入待落盘队列 — 首次立即落库, 节流窗口内合并,
    // 待下次触发或轮末 flushViewMessages() 补存 (消息 + 追加后计数同事务)
    if (hooks_.onAppendViewMessage) {
        enqueueViewPersist(PendingViewOp{
            .isAppend = true,
            .absIndex = viewSeq - 1,
            .counter  = msgIdCounter_,
            .seq      = viewSeq,
        });
    }
    // 窗口淘汰 (只在待落盘队列为空时真正删除, 见 maybeEvictViewWindow)
    maybeEvictViewWindow();
    return id;
}

std::string Session::beginStreamingView(ViewMessage skeleton) {
    assertIoThread();
    // 预留 id (不推 viewSeqCounter_: 那段还没进历史)
    const auto id = fmt::format("msg_{:06d}", ++msgIdCounter_);
    skeleton.id   = id;
    streamingView = std::move(skeleton);
    return id;
}

void Session::appendStreamingText(std::string_view text) {
    assertIoThread();
    if (!streamingView.has_value() || text.empty()) {
        return;
    }
    streamingView->text.append(text);
}

void Session::mergeStreamingThink(const ViewMessage::ThinkData& data) {
    assertIoThread();
    if (!streamingView.has_value()) {
        return;
    }
    if (!streamingView->think.has_value()) {
        streamingView->think = data;
        return;
    }
    auto& think         = *streamingView->think;
    think.isEncrypted   = think.isEncrypted || data.isEncrypted;
    think.reasoningTokens = std::max(think.reasoningTokens, data.reasoningTokens);
}

std::string Session::finalizeStreamingView(ViewMessage finalMsg) {
    assertIoThread();
    if (!streamingView.has_value()) {
        return appendViewMessage(std::move(finalMsg));
    }
    const auto id = streamingView->id;
    streamingView.reset();
    finalMsg.id = id;
    return appendViewMessage(std::move(finalMsg), id);
}

void Session::updateViewMessage(ViewMessage msg) {
    // 强制校验: 展示历史窗口/msgIdCounter_ 仅允许 io 线程写入
    assertIoThread();

    if (msg.id.empty()) {
        XX_LOGW("Session::updateViewMessage: empty msg id, skipped");
        return;
    }
    // 定位同 id 消息: 先走 msgId 索引 (O(1)); 索引缺失或指向的消息已变
    // 则回退线性扫描并修复索引 (窗口/索引仅本类维护, 回退属防御)
    const auto absIndex = absoluteIndexOfId(msg.id);
    if (absIndex.has_value()) {
        viewWindow_[*absIndex - viewWindowBase_] = std::move(msg);
        // 持久化 (节流, 尽力而为): 覆盖库内对应行, 供重启恢复
        if (hooks_.onUpdateViewMessage) {
            enqueueViewPersist(PendingViewOp{
                .isAppend = false,
                .absIndex = *absIndex,
                .counter  = 0,
            });
        }
        return;
    }
    // 已淘汰 (不在内存窗口): 只写库, 不改内存
    // - 调用方持有的是淘汰前读到的副本, 不能据此重建整行;
    //   客户端的显示由定稿的整条 upsert 收敛
    XX_LOGD("Session::updateViewMessage: msg id {} is outside the window, write to store only", msg.id);
    if (hooks_.onUpdateViewMessage) {
        hooks_.onUpdateViewMessage(msg);
    }
}

void Session::setStoreHooks(SessionStoreHooks hooks) {
    assertIoThread();
    hooks_ = std::move(hooks);
}

void Session::restore(
    std::vector<ViewMessage> messages,
    uint64_t                 msgIdCounter,
    uint64_t                 lastViewSeq
) {
    const auto total
        = std::max<uint64_t>(lastViewSeq, static_cast<uint64_t>(messages.size()));
    restoreWindow(std::move(messages), 0, total, msgIdCounter);
}

void Session::restoreWindow(
    std::vector<ViewMessage> messages,
    uint64_t                 base,
    uint64_t                 total,
    uint64_t                 msgIdCounter
) {
    assertIoThread();

    // 整体替换窗口前丢弃待落盘队列: 队列按绝对下标引用消息, 替换后
    // 旧下标不再有效; 恢复发生在会话加载阶段, 此时不应有未落盘增量
    pendingViewOps_.clear();
    viewEvictPending_ = false;

    // 进程重启/切会话后没有半截行: 直播槽一并清空
    streamingView.reset();

    viewWindow_     = std::move(messages);
    viewWindowBase_ = base;
    msgIdCounter_   = msgIdCounter;
    // 总数: 库内记录的优先; 老数据无记录时按"窗口首条下标 + 窗口条数"推算,
    // 保证后续追加的序号严格大于已存在的历史 (重连增量补拉不会漏消息)
    viewSeqCounter_ = std::max<uint64_t>(total, base + viewWindow_.size());
    // 重建 msgId → 绝对下标索引 (只含窗口内的 id)
    msgIndex_.clear();
    msgIndex_.reserve(viewWindow_.size());
    for (size_t i = 0; i < viewWindow_.size(); ++i) {
        if (false == viewWindow_[i].id.empty()) {
            msgIndex_.insert_or_assign(viewWindow_[i].id, base + i);
        }
    }
}

std::optional<uint64_t> Session::absoluteIndexOfId(std::string_view id) const {
    if (id.empty()) {
        return std::nullopt;
    }
    const auto windowEnd = viewWindowBase_ + viewWindow_.size();
    // 异构查找未开 (std::hash<std::string> 不是透明哈希): 一次 string 构造换 O(1) 定位
    const std::string key{id};
    if (auto idxIt = msgIndex_.find(key); idxIt != msgIndex_.end()) {
        const auto absIndex = idxIt->second;
        if (absIndex >= viewWindowBase_ && absIndex < windowEnd
            && viewWindow_[static_cast<size_t>(absIndex - viewWindowBase_)].id == id) {
            return absIndex;
        }
    }
    // 索引缺失/不一致: 线性扫描窗口并修复索引 (旧下标可能因淘汰而失效)
    for (size_t i = 0; i < viewWindow_.size(); ++i) {
        if (viewWindow_[i].id == id) {
            msgIndex_.insert_or_assign(key, viewWindowBase_ + i);
            return viewWindowBase_ + i;
        }
    }
    return std::nullopt;
}

const ViewMessage* Session::viewMessageAt(uint64_t absIndex) const {
    if (absIndex < viewWindowBase_) {
        return nullptr;
    }
    const auto offset = absIndex - viewWindowBase_;
    if (offset >= viewWindow_.size()) {
        return nullptr;
    }
    return &viewWindow_[static_cast<size_t>(offset)];
}

const ViewMessage* Session::viewMessageById(std::string_view id) const {
    const auto absIndex = absoluteIndexOfId(id);
    return absIndex.has_value() ? viewMessageAt(*absIndex) : nullptr;
}

bool Session::collectViewRange(uint64_t start, uint64_t end, std::vector<ViewMessage>& out) {
    assertIoThread();
    out.clear();
    if (end > viewSeqCounter_) {
        end = viewSeqCounter_;
    }
    if (start >= end) {
        return true;
    }
    const auto memBase = viewWindowBase_;
    const auto memEnd  = viewWindowBase_ + viewWindow_.size();
    if (start < memBase) {
        // 前半段不在内存: 回库读 (页区间与 seq 一一对应)
        const uint64_t headEnd = std::min(end, memBase);
        const uint64_t want    = headEnd - start;
        std::vector<ViewMessage> page;
        if (hooks_.onLoadViewPage) {
            page = hooks_.onLoadViewPage(start, static_cast<uint32_t>(want));
            if (auto m = metrics_.lock(); m) {
                m->noteViewPageRead(true);
            }
        }
        if (page.size() != want) {
            // 库不可用/短页: 不能把不连续的内容当成合法页返回 (客户端会按
            // startIndex 对账), 直接回空页由调用方降级
            XX_LOGW(
                "Session::collectViewRange: page [{}, {}) from store is incomplete ({} of {}); "
                "older history unavailable",
                start,
                headEnd,
                page.size(),
                want
            );
            if (auto m = metrics_.lock(); m) {
                m->noteViewCollectPartial();
            }
            return false;
        }
        out = std::move(page);
    } else if (auto m = metrics_.lock(); m) {
        // 整段命中内存 (尾窗/追加快照的常规路径)
        m->noteViewPageRead(false);
    }
    // 内存覆盖的部分 (窗口比库新: tool 结果回填只在内存里)
    const uint64_t from = std::max(start, memBase);
    if (from < end && from < memEnd) {
        const uint64_t to = std::min(end, memEnd);
        out.insert(
            out.end(),
            viewWindow_.begin() + static_cast<ptrdiff_t>(from - memBase),
            viewWindow_.begin() + static_cast<ptrdiff_t>(to - memBase)
        );
    }
    return true;
}

void Session::maybeEvictViewWindow() {
    assertIoThread();
    if (viewWindowCap_ == 0 || viewWindow_.size() <= viewWindowCap_) {
        viewEvictPending_ = false;
        return;
    }
    // 待落盘队列按下标引用内存: 队列非空时不删, 等 flush 后再试
    // (既不在 io 线程强做批量写库, 也不会出现"窗口里删了、库里还没写")
    if (!pendingViewOps_.empty()) {
        viewEvictPending_ = true;
        return;
    }
    viewEvictPending_ = false;
    // 成批删除: 至少清到 cap 以下, 且一次至少删 cap/4 条 —— 逐条 erase(begin())
    // 在长会话里是 O(n) 搬移, 分批后摊还成本可忽略
    const size_t batch = std::max<size_t>(viewWindowCap_ / 4, 1);
    size_t       drop  = viewWindow_.size() - viewWindowCap_;
    drop               = std::max(drop, batch);
    drop               = std::min(drop, viewWindow_.size());
    if (drop == 0) {
        return;
    }
    for (size_t i = 0; i < drop; ++i) {
        if (!viewWindow_[i].id.empty()) {
            msgIndex_.erase(viewWindow_[i].id);
        }
    }
    viewWindow_.erase(
        viewWindow_.begin(),
        viewWindow_.begin() + static_cast<ptrdiff_t>(drop)
    );
    viewWindowBase_ += drop;
    if (auto m = metrics_.lock(); m) {
        m->noteViewEvict(drop);
    }
    XX_LOGD(
        "Session: view window evicted {} message(s), window=[{}, {}) cap={}",
        drop,
        viewWindowBase_,
        viewWindowBase_ + viewWindow_.size(),
        viewWindowCap_
    );
}

void Session::saveLlmMessages() {
    assertIoThread();
    if (hooks_.onSaveLlmMessages) {
        hooks_.onSaveLlmMessages(llmMessagesJsonText());
        // 记录落盘时刻供节流判定 (轮末权威保存同样刷新窗口)
        llmLastSaveMs_ = steadyNowMs();
    }
}

void Session::markMessagesChanged() {
    ++messagesVersion_;
    llmMessagesJsonDirty_     = true;
    llmMessagesJsonTextDirty_ = true;
}

void Session::appendMessages(std::vector<neograph::ChatMessage> msgs, bool persistThrottled) {
    assertIoThread();
    if (msgs.empty()) {
        return;
    }
    messages_.insert(
        messages_.end(),
        std::make_move_iterator(msgs.begin()),
        std::make_move_iterator(msgs.end())
    );
    markMessagesChanged();
    if (persistThrottled) {
        requestSaveLlmMessages();
    }
}

void Session::replaceMessages(std::vector<neograph::ChatMessage> msgs, bool persistThrottled) {
    assertIoThread();
    messages_ = std::move(msgs);
    markMessagesChanged();
    if (persistThrottled) {
        requestSaveLlmMessages();
    }
}

void Session::replaceMessagesFromJson(const utilxx_base::Json& msgs) {
    assertIoThread();
    replaceMessages(messagesFromJson(msgs), false);
}

void Session::appendSettledLlmMessages(const utilxx_base::Json& settledMsgs) {
    assertIoThread();
    appendMessages(messagesFromJson(settledMsgs));
}

void Session::setSystemMessage(neograph::ChatMessage msg, bool persistThrottled) {
    assertIoThread();
    if (!messages_.empty() && messages_.front().role == "system") {
        // 内容相同 (每轮重建的系统提示词大多数时候不变) 就不动版本号,
        // 避免无谓的缓存失效与落盘
        if (messages_.front().content == msg.content) {
            return;
        }
        messages_.front() = std::move(msg);
    } else {
        messages_.insert(messages_.begin(), std::move(msg));
    }
    markMessagesChanged();
    if (persistThrottled) {
        requestSaveLlmMessages();
    }
}

void Session::truncateMessages(size_t count, bool persistThrottled) {
    assertIoThread();
    if (count >= messages_.size()) {
        return;
    }
    messages_.resize(count);
    markMessagesChanged();
    if (persistThrottled) {
        requestSaveLlmMessages();
    }
}

const utilxx_base::Json& Session::llmMessagesJson() const {
    assertIoThread();
    if (llmMessagesJsonDirty_) {
        // 逐条转图边界 JSON 再转业务 JSON: 复用 neograph 的 ChatMessage 序列化,
        // 保证与落库/传输的历史形态完全一致
        neograph::json arr = neograph::json::array();
        for (const auto& m : messages_) {
            neograph::json one;
            neograph::to_json(one, m);
            arr.push_back(std::move(one));
        }
        llmMessagesJsonCache_ = agentxx::util::fromNeographJson(arr);
        llmMessagesJsonDirty_ = false;
    }
    return llmMessagesJsonCache_;
}

const std::string& Session::llmMessagesJsonText() const {
    assertIoThread();
    if (llmMessagesJsonTextDirty_) {
        // 逐条消息序列化后单独 dump 再拼成数组: 与"先构造整段数组 DOM 再 dump"
        // 的输出逐字节一致 (JSON 数组就是 `[` + 元素以 `,` 连接 + `]`),
        // 但不需要在内存里同时持有整段数组 DOM
        std::string& out = llmMessagesJsonTextCache_;
        out.clear();
        if (messages_.empty()) {
            out = "[]";
        } else {
            out.push_back('[');
            for (size_t i = 0; i < messages_.size(); ++i) {
                if (i > 0) {
                    out.push_back(',');
                }
                // 每条消息一个临时 json: 只保留单条消息的转换结果, 不构造整段数组
                neograph::json one;
                neograph::to_json(one, messages_[i]);
                out += agentxx::util::fromNeographJson(one).dump();
            }
            out.push_back(']');
        }
        llmMessagesJsonTextDirty_ = false;
    }
    return llmMessagesJsonTextCache_;
}

void Session::requestSaveLlmMessages() {
    assertIoThread();
    if (!hooks_.onSaveLlmMessages) {
        return;
    }
    const auto nowMs = steadyNowMs();
    if (llmLastSaveMs_ == 0 || nowMs - llmLastSaveMs_ >= kPersistThrottleMs) {
        // 首次触发 / 距上次落盘已超窗口: 立即保存
        saveLlmMessages();
    }
    // 窗口内: 仅更新内存 (llm 内容本身在 llmMessages 中, 无需单独排队),
    // 待下次结算触发或轮末 saveLlmMessages() 统一落盘
}

void Session::flushViewMessages() {
    assertIoThread();
    flushPendingViewOps();
}

void Session::persistNow(std::string_view reason) {
    assertIoThread();
    if (!hooks_.onSaveLlmMessages && !hooks_.onAppendViewMessage && !hooks_.onUpdateViewMessage) {
        // 内存模式 (未启用持久化): 无落盘内容, 也不产生日志噪音
        return;
    }
    // 空会话不落盘: 只连接/切换过、还没产生任何消息的会话落盘会建出空的会话库,
    // 会话列表里随后多出一条无内容条目; 第一条消息写入时 (appendViewMessage /
    // saveLlmMessages) 才建立会话库。进程退出时的整体刷盘也走本入口
    if (viewWindow_.empty() && messages_.empty() && pendingViewOps_.empty()) {
        return;
    }
    XX_LOGD("Session: persist now ({})", reason);
    saveLlmMessages();
    flushViewMessages();
}

void Session::persistThrottled(std::string_view reason) {
    assertIoThread();
    if (!hooks_.onSaveLlmMessages) {
        return;
    }
    XX_LOGT("Session: persist throttled ({})", reason);
    // 上下文按既有节流窗口 (首次/超窗口立即写)
    requestSaveLlmMessages();
    // 展示历史: 追加时已入待落盘队列, 这里只在窗口外补一次刷出
    if (!pendingViewOps_.empty() && viewLastPersistMs_ != 0
        && steadyNowMs() - viewLastPersistMs_ >= kPersistThrottleMs) {
        flushPendingViewOps();
    }
}

void Session::enqueueViewPersist(PendingViewOp op) {
    // 同一条行的连续覆盖只留一条: PendingViewOp 只记绝对下标, 回放时读的是行
    // 当前内容, 中间状态没有落库价值 (流式文本反复追加到同一条行时会调用很多次)
    if (false == op.isAppend) {
        for (const auto& pending : pendingViewOps_) {
            if (false == pending.isAppend && pending.absIndex == op.absIndex) {
                return;
            }
        }
    }
    pendingViewOps_.push_back(std::move(op));
    const auto nowMs = steadyNowMs();
    if (viewLastPersistMs_ == 0 || nowMs - viewLastPersistMs_ >= kPersistThrottleMs) {
        // 首次触发 / 距上次落盘已超窗口: 立即回放全部待落盘操作 (含本条)
        flushPendingViewOps();
    }
}

void Session::flushPendingViewOps() {
    if (pendingViewOps_.empty()) {
        return;
    }
    const auto windowEnd = viewWindowBase_ + viewWindow_.size();
    for (const auto& op : pendingViewOps_) {
        // 绝对下标越界防御: restore 整体替换窗口时会先清空队列, 且淘汰只在队列
        // 为空时发生, 正常路径不会命中; 此处先做一次判断, 避免异常路径把错误数据写库
        if (op.absIndex < viewWindowBase_ || op.absIndex >= windowEnd) {
            XX_LOGW(
                "Session::flushPendingViewOps: stale view index {} (window=[{}, {}))",
                op.absIndex,
                viewWindowBase_,
                windowEnd
            );
            continue;
        }
        const auto& msg = viewWindow_[static_cast<size_t>(op.absIndex - viewWindowBase_)];
        if (op.isAppend) {
            if (hooks_.onAppendViewMessage) {
                hooks_.onAppendViewMessage(msg, op.counter, op.seq);
            }
        } else {
            if (hooks_.onUpdateViewMessage) {
                hooks_.onUpdateViewMessage(msg);
            }
        }
    }
    pendingViewOps_.clear();
    viewLastPersistMs_ = steadyNowMs();
    // 队列表清空了: 之前因"待落盘不为空"而推迟的淘汰现在可以做
    if (viewEvictPending_) {
        maybeEvictViewWindow();
    }
}

void Session::setCancelToken(std::shared_ptr<neograph::graph::CancelToken> token) {
    assertIoThread();
    cancelToken_ = std::move(token);
}

std::shared_ptr<neograph::graph::CancelToken> Session::getCancelToken() {
    assertIoThread();
    return cancelToken_;
}

void Session::setModelName(std::string_view name) {
    assertIoThread();
    modelName_ = name;
}

std::string Session::getModelName() const {
    assertIoThread();
    return modelName_;
}

void Session::enqueuePendingInput(SessionPendingInput input) {
    assertIoThread();
    if (input.delivery.empty()) {
        input.delivery = std::string{InputDelivery::NextTurn};
    }
    pendingInputs_.push_back(std::move(input));
}

std::vector<SessionPendingInput> Session::takePendingInputs(std::string_view delivery) {
    assertIoThread();
    std::vector<SessionPendingInput> out;
    auto                             it = pendingInputs_.begin();
    while (it != pendingInputs_.end()) {
        if (it->delivery == delivery) {
            out.push_back(std::move(*it));
            it = pendingInputs_.erase(it);
        } else {
            ++it;
        }
    }
    return out;
}

/// 用量台账聚合 → 会话累计用量 (界面展示"已消耗 token"用)
static SessionUsageTotals usageTotalsOf(const SessionStore::UsageSummary& summary) {
    SessionUsageTotals out;
    out.promptTokens       = summary.promptTokens;
    out.completionTokens   = summary.completionTokens;
    out.totalTokens        = summary.totalTokens;
    out.cachedPromptTokens = summary.cachedPromptTokens;
    return out;
}

std::shared_ptr<Session> SessionsManager::getOrCreate(std::string_view sessionId) {
    auto it = sessions_.find(sessionId);
    if (it != sessions_.end()) {
        return it->second;
    }
    auto session = std::make_shared<Session>();
    // 拷贝到局部: 供 lambda 按值捕获 (成员无法直接捕获)
    auto sessionStore = this->sessionStore;
    if (sessionStore) {
        // 从 SQLite 恢复该 session 的展示历史尾窗/LLM 上下文, 并绑定持久化回调
        auto loaded = sessionStore->loadSession(sessionId, viewTailCount());
        // 恢复的工具行参数按当前工具 schema 修正 (库里存的是模型原文)
        normalizeToolViewArgs(loaded.viewMessages);
        session->setViewWindowCap(viewWindowMessages);
        session->restoreWindow(
            std::move(loaded.viewMessages),
            loaded.viewWindowBase,
            loaded.lastViewSeq,
            loaded.msgIdCounter
        );
        session->replaceMessagesFromJson(loaded.llmMessages);
        session->setStoreHooks(
            makeSessionStoreHooks(sessionStore, std::string{sessionId}, session, toolArgsNormalizer)
        );
        // 累计用量同样从台账恢复 (之后每次模型调用累加, 见 nodes/modelcall.cpp):
        // 界面显示"本会话已消耗多少 token"跨进程重启连续
        if (session->contextStats) {
            session->contextStats->usage = usageTotalsOf(loaded.usage);
            // 会话标题一并恢复: 界面 (信息侧边栏) 启动即有标题可显示
            session->contextStats->title = loaded.title;
        }
    }
    session->setMetrics(metrics);
    utilxx_base::insertHeterogeneous(sessions_, std::string{sessionId}, session);
    return session;
}

asio::awaitable<std::shared_ptr<Session>>
    SessionsManager::getOrCreateAsync(std::string_view sessionId, asio::thread_pool* pool) {
    auto it = sessions_.find(sessionId);
    if (it != sessions_.end()) {
        co_return it->second;
    }

    auto                        sessionStore = this->sessionStore;
    const size_t                tailCount    = viewTailCount();
    SessionStore::LoadedSession loaded;
    if (sessionStore) {
        if (pool) {
            loaded = co_await utilxx::offloadAsync<SessionStore::LoadedSession>(
                *pool,
                [sessionStore, tailCount, sid = std::string(sessionId)]()
                    -> asio::awaitable<SessionStore::LoadedSession> {
                    co_return sessionStore->loadSession(sid, tailCount);
                }
            );
        } else {
            loaded = sessionStore->loadSession(sessionId, tailCount);
        }
    }

    it = sessions_.find(sessionId);
    if (it != sessions_.end()) {
        co_return it->second;
    }

    auto session = std::make_shared<Session>();
    if (sessionStore) {
        session->setViewWindowCap(viewWindowMessages);
        // 恢复的工具行参数按当前工具 schema 修正 (与 getOrCreate 同一处理)
        normalizeToolViewArgs(loaded.viewMessages);
        session->restoreWindow(
            std::move(loaded.viewMessages),
            loaded.viewWindowBase,
            loaded.lastViewSeq,
            loaded.msgIdCounter
        );
        session->replaceMessagesFromJson(loaded.llmMessages);
        session->setStoreHooks(
            makeSessionStoreHooks(sessionStore, std::string{sessionId}, session, toolArgsNormalizer)
        );
        // 累计用量从台账恢复 (与 getOrCreate 同一处理)
        if (session->contextStats) {
            session->contextStats->usage = usageTotalsOf(loaded.usage);
            session->contextStats->title = loaded.title;
        }
    }
    session->setMetrics(metrics);
    utilxx_base::insertHeterogeneous(sessions_, std::string{sessionId}, session);
    co_return session;
}

std::shared_ptr<Session> SessionsManager::get(std::string_view sessionId) {
    auto it = sessions_.find(sessionId);
    return it == sessions_.end() ? nullptr : it->second;
}

void SessionsManager::normalizeToolViewArgs(std::vector<ViewMessage>& rows) const {
    normalizeToolViewArgsIn(toolArgsNormalizer, rows);
}

void SessionsManager::remove(std::string_view sessionId) {
    // 异构查找删除, 免除 string_view→string 拷贝 (libc++ 无 C++23 异构 erase)
    utilxx_base::eraseHeterogeneous(sessions_, sessionId);
}

std::shared_ptr<Session> AgentContext::getSession(std::string_view sessionId) {
    return sessions->getOrCreate(sessionId);
}

asio::awaitable<std::shared_ptr<Session>> AgentContext::getSessionAsync(std::string_view sessionId
) {
    if (sessions) {
        co_return co_await sessions->getOrCreateAsync(sessionId, threadPool.get());
    }
    co_return nullptr;
}

void AgentContext::setSessionWorkDir(std::string_view sessionId, std::string_view absWorkDir) {
    if (sessionId.empty()) {
        return;
    }
    // mutex 保护: 端点线程 (如 ACP HTTP handler) 与 io 线程并发读写安全
    std::lock_guard lk(sessionWorkDirMu_);
    if (absWorkDir.empty()) {
        utilxx_base::eraseHeterogeneous(sessionWorkDirs_, sessionId);
        return;
    }
    utilxx_base::insertOrAssignHeterogeneous(
        sessionWorkDirs_,
        std::string{sessionId},
        std::string{absWorkDir}
    );
}

void AgentContext::clearSessionWorkDir(std::string_view sessionId) {
    std::lock_guard lk(sessionWorkDirMu_);
    utilxx_base::eraseHeterogeneous(sessionWorkDirs_, sessionId);
}

std::string AgentContext::getSessionBaseWorkDir(std::string_view sessionId) const {
    // 会话工作目录覆写 (各会话独立, 如 ACP 客户端注入的 cwd;
    // mutex 保护, 任意线程可读)
    {
        std::lock_guard lk(sessionWorkDirMu_);
        if (auto it = sessionWorkDirs_.find(sessionId); it != sessionWorkDirs_.end()) {
            return it->second;
        }
    }
    // 回退 agent 级配置 (yaml work_dir / 进程 cwd), 沿用旧行为
    if (agentConfig) {
        auto wd = agentConfig->resolvedWorkDir();
        if (!wd.empty()) {
            return wd;
        }
    }
    return {};
}

std::string AgentContext::getSessionWorkDir(std::string_view sessionId) const {
    // worktree 绑定优先 (worktree 模式; Session 可变状态仅 io 线程读写,
    // 本方法约定在 io 线程调用 —— 插件主程序侧经 ioCallSync 投递)
    auto session = sessions->get(sessionId);
    if (session) {
        const auto& wb = session->getWorktreeBinding();
        if (!wb.path.empty()) {
            return wb.path;
        }
    }
    return getSessionBaseWorkDir(sessionId);
}

std::string AgentContext::sessionTempDir(std::string_view sessionId) {
    std::error_code ec;
    auto            tempRoot = std::filesystem::temp_directory_path(ec);
    if (ec) {
        XX_LOGW("AgentContext::sessionTempDir: temp dir unavailable: {}", ec.message());
        return {};
    }
    // 目录段与会话数据目录同一套清洗规则: 空 ID 记为 "default", 非法字符替换,
    // 超长截断, 避免不同会话落到同一目录或路径穿越
    auto dir = tempRoot / "agentxx" / SessionStore::sanitizeSessionId(sessionId);
    return dir.generic_string();
}

std::string AgentContext::getSessionCurrentModelName(std::string_view sessionId) const {
    std::string selected;
    auto        session = sessions ? sessions->get(sessionId) : nullptr;
    if (session) {
        selected = session->getModelName();
    }
    // 表里已有模型时由它做权威解析: 会话没选模型 / 选了已下架的模型 → 默认模型
    // - 判定必须是 size() > 0 而不是"指针非空": 进程共享的注册表在 `AgentHost::create`
    //   里就挂到上下文上了 (插件 `start` 里挂的根也要能查模型), 但模型要到 `init()`
    //   才注册。客户端握手后立刻发 `WireGetModel` (见 mode_runners 的
    //   onTransportStarted), 那会儿表是空的, 当成"有表但在里面找不到"会解析出空串,
    //   状态栏于是显示"未选择", 而配置里的 `model.use.default` 明明有值
    if (modelRegistry && modelRegistry->size() > 0) {
        return modelRegistry->resolveModelName(selected);
    }
    // 表还没建 (init 之前 / 测试嵌入场景): 回退到配置选定的模型
    // - 会话已选过模型 (从会话库恢复): 直接用该名称 (selectModel 只记 registry 内的条目名)
    // - 否则用配置决定的默认条目名 (与 init 建表后定下的默认模型同一规则), 客户端启动时
    //   显示的模型名就是用户在 yaml 里写的那一个, 而不是上游 model_name
    if (!selected.empty()) {
        return selected;
    }
    if (agentConfig) {
        return agentConfig->defaultModelName();
    }
    return {};
}

const ModelConfig& AgentContext::getSessionCurrentModelConfig(std::string_view sessionId) const {
    // 与 getSessionCurrentModelName 同一处判定: 表空着 (init 之前) 不算"有表",
    // 否则会拿到 ModelConfig::defaultModelConfig (空配置) 而不是配置里的模型
    if (modelRegistry && modelRegistry->size() > 0) {
        return modelRegistry->getModelConfig(getSessionCurrentModelName(sessionId));
    }
    // 表还没建 (init 之前 / 测试嵌入场景): 用配置里同一条目的配置, 找不到再用主模型
    if (agentConfig) {
        auto it = agentConfig->availableModels.find(getSessionCurrentModelName(sessionId));
        if (it != agentConfig->availableModels.end()) {
            return it->second;
        }
        return agentConfig->model;
    }
    return ModelConfig::defaultModelConfig;
}

std::vector<std::pair<std::string, std::string>>
    AgentContext::buildStableContextSections(std::string_view sessionId) const {
    std::vector<std::pair<std::string, std::string>> out;
    if (!promptSources) {
        return out;
    }
    // 运行时段由来源注册表持有 (不再经 graphData 中转): 顺序 (order, 键名) 稳定,
    // 内容只在 model call 的 start 钩子里按标记重取
    for (const auto& section : promptSources->stableSections(sessionId)) {
        out.emplace_back(std::string{section.key}, std::string{section.text});
    }
    return out;
}

std::vector<std::pair<std::string, std::string>>
    AgentContext::buildDynamicContextSections(std::string_view sessionId) const {
    std::vector<std::pair<std::string, std::string>> out;
    if (!promptSources) {
        return out;
    }
    for (const auto& section : promptSources->dynamicSections(sessionId)) {
        out.emplace_back(std::string{section.key}, std::string{section.text});
    }
    return out;
}

std::vector<PromptSectionView> AgentContext::promptSectionViews(std::string_view sessionId) const {
    std::vector<PromptSectionView> out;
    // 配置作用域静态段 (只读引用 AgentPrompt 内部存储, 只在本次调用内有效)
    if (agentConfig) {
        for (const auto& section : agentConfig->prompt.orderedAppendSections()) {
            out.push_back(PromptSectionView{
                .key   = section.key,
                .text  = section.text,
                .order = section.order,
                .owner = section.source,
                .scope = PromptSectionScope::Config,
                .slot  = PromptSectionSlot::Stable,
            });
        }
    }
    // 接下来是运行时段 (会话作用域)
    if (promptSources) {
        for (const auto& section : promptSources->stableSections(sessionId)) {
            out.push_back(section);
        }
        for (const auto& section : promptSources->dynamicSections(sessionId)) {
            out.push_back(section);
        }
    }
    return out;
}

std::string AgentContext::renderPromptVars(std::string text, std::string_view sessionId) const {
    // - 拼装完成后统一替换, 自定义 systemPrompt 与各附加段都生效
    // - 工作目录取基准值 (不含 worktree 绑定): 进出 worktree 由工具在会话内切换,
    //   模型从工具结果得知, 系统提示词不跟着变化
    // - 会话 ID 为空时按 "default" 参与替换, 与 sessionTempDir 的目录名一致
    PromptSessionVars vars;
    vars.sessionId = sessionId.empty() ? std::string{"default"} : std::string{sessionId};
    vars.workDir   = getSessionBaseWorkDir(sessionId);
    vars.tempDir   = sessionTempDir(sessionId);
#if XX_IS_WIN_D
    vars.platform = "Windows";
#elif XX_IS_LINUX_D
    vars.platform = "Linux";
#elif XX_IS_MACOS_D
    vars.platform = "macOS";
#elif XX_IS_ANDROID_D
    vars.platform = "Android";
#elif XX_IS_IOS_D
    vars.platform = "iOS";
#else
    vars.platform = "Unknown";
#endif
    return AgentPrompt::renderVars(text, vars);
}

std::string AgentContext::buildSystemPromptStable(std::string_view sessionId) const {
    if (!agentConfig) {
        return "";
    }
    std::string combined;
    appendPromptSegment(combined, agentConfig->prompt.systemPrompt);
    // 静态附加段: 按段落 order 排序, 同 order 按键名字典序稳定排列
    for (const auto& section : agentConfig->prompt.orderedAppendSections()) {
        appendPromptSegment(combined, std::string{section.text});
    }
    // 运行时稳定附加段 (记忆文件内容 / 技能清单): 随 system 消息一起进稳定前缀,
    // 上游的 KV/前缀缓存能命中; 文本由来源注册表持有, 只在 model call 的 start
    // 钩子里按标记重取 (首次 / 压缩完成 / 资源变更 / 显式请求)
    for (const auto& [source, text] : buildStableContextSections(sessionId)) {
        (void)source;
        appendPromptSegment(combined, text);
    }
    return renderPromptVars(std::move(combined), sessionId);
}

std::string AgentContext::buildSystemPrompt(std::string_view sessionId) const {
    if (!agentConfig) {
        return "";
    }
    std::string combined;
    appendPromptSegment(combined, agentConfig->prompt.systemPrompt);
    for (const auto& section : agentConfig->prompt.orderedAppendSections()) {
        appendPromptSegment(combined, std::string{section.text});
    }
    // 稳定附加段与动态段都保留在末尾 (完整提示词仅供 UI 查看): 请求加载时稳定
    // 附加段随 system 消息下发, 动态段作为请求末尾的独立消息附加 (见 modelcall)
    for (const auto& [source, text] : buildStableContextSections(sessionId)) {
        (void)source;
        appendPromptSegment(combined, text);
    }
    for (const auto& [source, text] : buildDynamicContextSections(sessionId)) {
        (void)source;
        appendPromptSegment(combined, text);
    }
    return renderPromptVars(std::move(combined), sessionId);
}

} // namespace agent
} // namespace agentxx
