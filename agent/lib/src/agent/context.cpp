#include "agentxx/agent/context.h"
#include "agentxx/agent/model_registry.h"
#include "agentxx/agent/session_store.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/util/log.h"
#include <fmt/format.h>

namespace agentxx {
namespace agent {

AgentContext::~AgentContext() {
    // 插件系统先卸载全部插件, 断开中间件↔实例循环引用
    // (handles 由 middlewareHandleContext 持有, 其析构晚于 pluginManager)
    if (pluginManager) {
        pluginManager->shutdownAll();
    }
}

std::string Session::appendViewMessage(ViewMessage msg) {
    // 强制校验: viewMessages/chainHash/msgIdCounter_ 仅允许 io 线程写入
    assertIoThread();

    // 链式哈希对消息内容 (不含 id): 与旧实现 (json 内容 dump) 语义一致
    chainHash.append(msg.toJson().dump());
    auto id = fmt::format("msg_{:06d}", ++msgIdCounter_);
    msg.id  = id;
    viewMessages.push_back(std::move(msg));
    // 持久化 (尽力而为): 消息 + 追加后计数一起落库, 供重启恢复
    if (hooks_.onAppendViewMessage) {
        hooks_.onAppendViewMessage(viewMessages.back(), msgIdCounter_);
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
    // 定位同 id 消息 (历史 append-only, 顺序线性扫描即可; 低频操作)
    for (auto& m : viewMessages) {
        if (m.id == msg.id) {
            m = std::move(msg);
            // 持久化 (尽力而为): 覆盖库内对应行, 供重启恢复
            if (hooks_.onUpdateViewMessage) {
                hooks_.onUpdateViewMessage(m);
            }
            return;
        }
    }
    XX_LOGW("Session::updateViewMessage: msg id {} not found in history", msg.id);
}

void Session::setStoreHooks(SessionStoreHooks hooks) {
    assertIoThread();
    hooks_ = std::move(hooks);
}

void Session::restore(std::vector<ViewMessage> messages, uint64_t msgIdCounter) {
    assertIoThread();

    // 重建链式哈希: 与 appendViewMessage 一致, 对不含 id 的消息内容哈希
    chainHash.reset();
    for (const auto& m : messages) {
        auto content = m;
        content.id.clear();
        chainHash.append(content.toJson().dump());
    }
    viewMessages  = std::move(messages);
    msgIdCounter_ = msgIdCounter;
}

void Session::saveLlmMessages() {
    assertIoThread();
    if (hooks_.onSaveLlmMessages) {
        hooks_.onSaveLlmMessages(llmMessages);
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
        session->restore(std::move(loaded.viewMessages), loaded.msgIdCounter);
        session->llmMessages = std::move(loaded.llmMessages);
        // 捕获 sessionId 副本, 回调生命周期随 session, 无悬垂风险
        auto tid = std::string{sessionId};
        session->setStoreHooks(SessionStoreHooks{
            .onAppendViewMessage =
                [sessionStore, tid](const ViewMessage& msg, uint64_t counter) {
                    sessionStore->appendViewMessage(tid, msg, counter);
                },
            .onUpdateViewMessage =
                [sessionStore, tid](const ViewMessage& msg) {
                    sessionStore->updateViewMessage(tid, msg);
                },
            .onSaveLlmMessages =
                [sessionStore, tid](const neograph::json& msgs) {
                    sessionStore->saveLlmMessages(tid, msgs);
                },
        });
    }
    sessions_.emplace(sessionId, session);
    return session;
}

std::shared_ptr<Session> SessionsManager::get(std::string_view sessionId) {
    auto it = sessions_.find(sessionId);
    return it == sessions_.end() ? nullptr : it->second;
}

void SessionsManager::remove(std::string_view sessionId) {
    sessions_.erase(sessionId);
}

std::shared_ptr<Session> AgentContext::getSession(std::string_view sessionId) {
    return sessions->getOrCreate(sessionId);
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

} // namespace agent
} // namespace agentxx
