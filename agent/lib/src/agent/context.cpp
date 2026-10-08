#include "agentxx/agent/context.h"
#include "agentxx/agent/io/agent_io.h"
#include "agentxx/agent/model_registry.h"
#include "agentxx/agent/session_store.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/tools/subagent.h"
#include "agentxx/util/neograph_json_bridge.h"
#include "neograph/graph/registry.h"
#include "utilxx/async_offload.h"
#include "utilxx_base/container_util.h"
#include "utilxx_base/log.h"
#include <chrono>
#include <filesystem>
#include <fmt/format.h>
#include <fmt/ranges.h>

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

/// 持久化写入失败时提示一次 (计划 STO-9)
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

/// 组装会话持久化回调 (计划 STO-9: 每次写入后检查写失败并提示一次)
/// - 捕获 sessionId 副本, 回调生命周期随 session, 无悬垂风险;
///   session 用 weak_ptr 避免回调与 session 互相持有
SessionStoreHooks makeSessionStoreHooks(
    const std::shared_ptr<SessionStore>& store,
    std::string                          sessionId,
    const std::shared_ptr<Session>&      session
) {
    std::weak_ptr<Session> weakSession = session;
    return SessionStoreHooks{
        .onAppendViewMessage =
            [store, sessionId, weakSession](const ViewMessage& msg, uint64_t counter, uint64_t seq) {
                store->appendViewMessage(sessionId, msg, counter, seq);
                reportPersistFailure(weakSession.lock(), store);
            },
        .onUpdateViewMessage =
            [store, sessionId, weakSession](const ViewMessage& msg) {
                store->updateViewMessage(sessionId, msg);
                reportPersistFailure(weakSession.lock(), store);
            },
        .onSaveLlmMessages =
            [store, sessionId, weakSession](const utilxx_base::Json& msgs) {
                store->saveLlmMessages(sessionId, msgs);
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
    co_return co_await pluginManager->shutdownAsync(timeout);
}

AgentContext::~AgentContext() {
    // 插件系统先卸载全部插件, 断开中间件↔实例循环引用
    // (handles 由 middlewareHandleContext 持有, 其析构晚于 pluginManager)
    if (pluginManager) {
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
    //   此时宿主/进程正在拆卸, 直接丢弃待落盘操作并记录警告
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

std::string Session::appendViewMessage(ViewMessage msg) {
    // 强制校验: viewMessages/chainHash/msgIdCounter_ 仅允许 io 线程写入
    assertIoThread();

    // 链式哈希对消息内容 (不含 id)
    chainHash.append(msg.toJson().dump());
    auto id = fmt::format("msg_{:06d}", ++msgIdCounter_);
    msg.id  = id;
    // 展示历史持久化序号 (计划 STO-4): 会话内单调递增, 与库里 view_message.seq 一致
    const auto viewSeq = ++viewSeqCounter_;
    viewMessages.push_back(std::move(msg));
    // 维护 msgId → 下标索引 (updateViewMessage 的 O(1) 定位用)
    msgIndex_.insert_or_assign(id, viewMessages.size() - 1);
    // 持久化 (节流, 尽力而为): 压入待落盘队列 — 首次立即落库, 节流窗口内合并,
    // 待下次触发或轮末 flushViewMessages() 补存 (消息 + 追加后计数同事务)
    if (hooks_.onAppendViewMessage) {
        enqueueViewPersist(PendingViewOp{
            .isAppend = true,
            .index    = viewMessages.size() - 1,
            .counter  = msgIdCounter_,
            .seq      = viewSeq,
        });
    }
    return id;
}

void Session::updateViewMessage(ViewMessage msg) {
    // 强制校验: viewMessages/chainHash/msgIdCounter_ 仅允许 io 线程写入
    assertIoThread();

    if (msg.id.empty()) {
        XX_LOGW("Session::updateViewMessage: empty msg id, skipped");
        return;
    }
    // 定位同 id 消息: 先走 msgId 索引 (O(1)); 索引缺失或指向的消息已变
    // 则回退线性扫描并修复索引 (viewMessages/索引仅本类维护, 回退属防御)
    size_t index = viewMessages.size();
    if (auto idxIt = msgIndex_.find(msg.id); idxIt != msgIndex_.end()) {
        if (idxIt->second < viewMessages.size() && viewMessages[idxIt->second].id == msg.id) {
            index = idxIt->second;
        }
    }
    if (index == viewMessages.size()) {
        for (size_t i = 0; i < viewMessages.size(); ++i) {
            if (viewMessages[i].id == msg.id) {
                index = i;
                msgIndex_.insert_or_assign(msg.id, i); // 修复索引
                break;
            }
        }
    }
    if (index < viewMessages.size()) {
        auto& m = viewMessages[index];
        m       = std::move(msg);
        // 持久化 (节流, 尽力而为): 覆盖库内对应行, 供重启恢复
        if (hooks_.onUpdateViewMessage) {
            enqueueViewPersist(PendingViewOp{
                .isAppend = false,
                .index    = index,
                .counter  = 0,
            });
        }
        return;
    }
    XX_LOGW("Session::updateViewMessage: msg id {} not found in history", msg.id);
}

void Session::setStoreHooks(SessionStoreHooks hooks) {
    assertIoThread();
    hooks_ = std::move(hooks);
}

void Session::restore(std::vector<ViewMessage> messages, uint64_t msgIdCounter, uint64_t lastViewSeq) {
    assertIoThread();

    // 整体替换 viewMessages 前丢弃待落盘队列: 队列按下标引用消息, 替换后
    // 旧下标不再有效; 恢复发生在会话加载阶段, 此时不应有未落盘增量
    pendingViewOps_.clear();

    // 重建链式哈希: 与 appendViewMessage 一致, 对不含 id 的消息内容哈希
    chainHash.reset();
    for (const auto& m : messages) {
        auto content = m;
        content.id.clear();
        chainHash.append(content.toJson().dump());
    }
    viewMessages  = std::move(messages);
    msgIdCounter_ = msgIdCounter;
    // 展示历史序号: 库内最大值优先; 老数据无记录时按历史条数兜底, 保证后续
    // 追加的序号严格大于已存在的历史 (重连增量补拉不会漏消息)
    viewSeqCounter_ = std::max(lastViewSeq, static_cast<uint64_t>(viewMessages.size()));
    // 重建 msgId → 下标索引 (与 viewMessages 同步; 恢复的历史全量建索引)
    msgIndex_.clear();
    msgIndex_.reserve(viewMessages.size());
    for (size_t i = 0; i < viewMessages.size(); ++i) {
        if (false == viewMessages[i].id.empty()) {
            msgIndex_.insert_or_assign(viewMessages[i].id, i);
        }
    }
}

void Session::saveLlmMessages() {
    assertIoThread();
    if (hooks_.onSaveLlmMessages) {
        hooks_.onSaveLlmMessages(llmMessagesJson());
        // 记录落盘时刻供节流判定 (轮末权威保存同样刷新窗口)
        llmLastSaveMs_ = steadyNowMs();
    }
}

void Session::markMessagesChanged() {
    ++messagesVersion_;
    llmMessagesJsonDirty_ = true;
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
    if (viewMessages.empty() && messages_.empty() && pendingViewOps_.empty()) {
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
    for (const auto& op : pendingViewOps_) {
        // 下标越界防御: restore() 整体替换 viewMessages 时会先清空队列, 正常路径
        // 不会命中; 此处兜底避免异常路径把错误数据写库
        if (op.index >= viewMessages.size()) {
            XX_LOGW(
                "Session::flushPendingViewOps: stale view index {} (size={})",
                op.index,
                viewMessages.size()
            );
            continue;
        }
        const auto& msg = viewMessages[op.index];
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

std::shared_ptr<Session> SessionsManager::getOrCreate(std::string_view sessionId) {
    auto it = sessions_.find(sessionId);
    if (it != sessions_.end()) {
        return it->second;
    }
    auto session = std::make_shared<Session>();
    // 拷贝到局部: 供 lambda 按值捕获 (成员无法直接捕获)
    auto sessionStore = this->sessionStore;
    if (sessionStore) {
        // 从 SQLite 恢复该 session 的历史消息/LLM 上下文, 并绑定持久化回调
        auto loaded = sessionStore->loadSession(sessionId);
        session->restore(
            std::move(loaded.viewMessages),
            loaded.msgIdCounter,
            loaded.lastViewSeq
        );
        session->replaceMessagesFromJson(loaded.llmMessages);
        session->setStoreHooks(makeSessionStoreHooks(sessionStore, std::string{sessionId}, session));
    }
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
    SessionStore::LoadedSession loaded;
    if (sessionStore) {
        if (pool) {
            loaded = co_await utilxx::offloadAsync<SessionStore::LoadedSession>(
                *pool,
                [sessionStore,
                 sid = std::string(sessionId)]() -> asio::awaitable<SessionStore::LoadedSession> {
                    co_return sessionStore->loadSession(sid);
                }
            );
        } else {
            loaded = sessionStore->loadSession(sessionId);
        }
    }

    it = sessions_.find(sessionId);
    if (it != sessions_.end()) {
        co_return it->second;
    }

    auto session = std::make_shared<Session>();
    if (sessionStore) {
        session->restore(
            std::move(loaded.viewMessages),
            loaded.msgIdCounter,
            loaded.lastViewSeq
        );
        session->replaceMessagesFromJson(loaded.llmMessages);
        session->setStoreHooks(makeSessionStoreHooks(sessionStore, std::string{sessionId}, session));
    }
    utilxx_base::insertHeterogeneous(sessions_, std::string{sessionId}, session);
    co_return session;
}

std::shared_ptr<Session> SessionsManager::get(std::string_view sessionId) {
    auto it = sessions_.find(sessionId);
    return it == sessions_.end() ? nullptr : it->second;
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
    // 回退 agent 级配置 (yaml work_dir / 进程 cwd), 保持旧行为兜底
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
    // 本方法约定在 io 线程调用 —— 插件宿主侧经 ioCallSync 投递)
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
    auto        session = sessions->get(sessionId);
    if (session) {
        selected = session->getModelName();
    }
    if (modelRegistry) {
        return modelRegistry->resolveModelName(selected);
    } else {
        return agentConfig->model.modelName;
    }
}

const ModelConfig& AgentContext::getSessionCurrentModelConfig(std::string_view sessionId) const {
    if (modelRegistry) {
        return modelRegistry->getModelConfig(getSessionCurrentModelName(sessionId));
    }
    // 未初始化 registry 时(测试/嵌入场景)回退主模型
    return agentConfig ? agentConfig->model : ModelConfig::defaultModelConfig;
}

std::vector<std::pair<std::string, std::string>>
    AgentContext::buildDynamicContextSections(std::string_view sessionId) const {
    std::vector<std::pair<std::string, std::string>> out;
    if (!middlewareHandleContext || sessionId.empty()) {
        return out;
    }
    // 键排序的 map: 同一来源每轮覆盖写入 (不累积), 顺序稳定 (计划 PRM-1);
    // 旧实现用 vector 追加, 每轮都会把上一轮的片段再拼一次 (系统提示词逐轮变长)
    const auto& sections
        = middlewareHandleContext->getGraphDataItemValue<
            std::map<std::string, std::string, std::less<>>>(
            sessionId,
            agentxx::middleware::MiddlewareContext::graphDataKey_appendSystemMessage
        );
    out.reserve(sections.size());
    for (const auto& kv : sections) {
        if (!kv.second.empty()) {
            out.emplace_back(kv.first, kv.second);
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
    vars.platform  = "Windows";
#elif XX_IS_LINUX_D
    vars.platform  = "Linux";
#elif XX_IS_MACOS_D
    vars.platform  = "macOS";
#elif XX_IS_ANDROID_D
    vars.platform  = "Android";
#elif XX_IS_IOS_D
    vars.platform  = "iOS";
#else
    vars.platform  = "Unknown";
#endif
    return AgentPrompt::renderVars(text, vars);
}

std::string AgentContext::buildSystemPromptStable(std::string_view sessionId) const {
    if (!agentConfig) {
        return "";
    }
    std::string combined;
    appendPromptSegment(combined, agentConfig->prompt.systemPrompt);
    // 静态附加段: 按段落 order 排序 (计划 PRM-2), 同 order 按键名字典序稳定排列
    for (const auto& section : agentConfig->prompt.orderedAppendSections()) {
        appendPromptSegment(combined, std::string{section.text});
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
    // 动态段 (记忆 / 技能清单): 完整提示词里保留在末尾 (供 UI 查看);
    // 请求装配使用 buildSystemPromptStable + 末尾动态消息 (见 modelcall)
    for (const auto& [source, text] : buildDynamicContextSections(sessionId)) {
        (void)source;
        appendPromptSegment(combined, text);
    }
    return renderPromptVars(std::move(combined), sessionId);
}

} // namespace agent
} // namespace agentxx
