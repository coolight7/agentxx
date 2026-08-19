#include "agentxx/middlewares/middleware.h"
#include "agentxx/agent/session_persistence.h"
#include "agentxx/tools/tool.h"
#include <algorithm>
#include <charconv>

agentxx::middleware::BaseMiddlewareHandleInterface::BaseMiddlewareHandleInterface(
    std::string_view                            in_name,
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    name(in_name),
    agentContext(in_agentContext) {}

agentxx::middleware::BaseMiddlewareHandleInterface::~BaseMiddlewareHandleInterface() = default;

namespace agentxx {
namespace middleware {

neograph::json
    BaseMiddlewareHandleInterface::getLastMessageJson(const neograph::graph::NodeInput& in) {
    auto messages = in.state.get("messages");
    if (messages.is_array() && messages.size() > 0) {
        return messages.back();
    }
    return neograph::json(nullptr);
}

std::optional<neograph::ChatMessage>
    BaseMiddlewareHandleInterface::getLastMessage(const neograph::graph::NodeInput& in) {
    auto lastMsgJson = getLastMessageJson(in);
    if (false == lastMsgJson.is_object()) {
        return std::nullopt;
    }
    auto result = neograph::ChatMessage{};
    neograph::from_json(lastMsgJson, result);
    return result;
}

const neograph::ChatMessage* BaseMiddlewareHandleInterface::getLastAssistantToolcallMessage(
    std::vector<neograph::ChatMessage>& messages
) {
    const neograph::ChatMessage* assistant_msg = nullptr;
    if (false == messages.empty()) {
        for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
            if (it->role == "assistant" && !it->tool_calls.empty()) {
                assistant_msg = &(*it);
                break;
            }
        }
    }
    return assistant_msg;
}

const neograph::ChatMessage* BaseMiddlewareHandleInterface::getLastToolcallResultMessage(
    std::vector<neograph::ChatMessage>& messages
) {
    const neograph::ChatMessage* tool_msg = nullptr;
    if (false == messages.empty()) {
        for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
            if (it->role == "tool") {
                tool_msg = &(*it);
                break;
            }
        }
    }
    return tool_msg;
}

void BaseMiddlewareHandleInterface::printMessage(const neograph::ChatMessage& msg, size_t index) {
    std::string toollist;
    if (false == msg.tool_calls.empty()) {
        toollist += "┣━ Toolcall: \n";
        for (const auto& tool : msg.tool_calls) {
            toollist += fmt::format(
                R"(  - {}/{}
    {}
)",
                tool.name,
                tool.id,
                tool.arguments
            );
        }
    }
    XX_OUT(
        R"(
┏━━━━━━ Message/{} ━━━━━━┓
┣━ Role: {}
{}
┣━ Think: {}
┣━ Content: {}
┗━━━━━━ Message/{} ━━━━━━┛
)",
        index,
        msg.role,
        toollist,
        msg.reasoning_content,
        msg.content,
        index
    );
}

void BaseMiddlewareHandleInterface::printMessages(
    const std::vector<neograph::ChatMessage>& messages,
    bool                                      printSystemMsg
) {
    size_t index = 0;
    for (const auto& msg : messages) {
        ++index;
        if (false == printSystemMsg && msg.role == "system") {
            continue;
        }
        printMessage(msg, index);
    }
}

InterruptHandleArg::InterruptHandleInputItem
    InterruptHandleArg::InterruptHandleInputItem::fromJson(const neograph::json& data) {
    auto result = InterruptHandleInputItem{};
    if (data.is_object()) {
        if (data["label"].is_string()) {
            result.label = data["label"].get<std::string>();
        }
        if (data["depict"].is_string()) {
            result.depict = data["depict"].get<std::string>();
        }
        if (data["type"].is_string()) {
            result.type = data["type"].get<std::string>();
        }
        if (data["enumValues"].is_array()) {
            result.enumValues = data["enumValues"].get<std::vector<std::string>>();
        }
        if (data["defaultValue"].is_string()) {
            result.defaultValue = data["defaultValue"].get<std::string>();
        }
    }
    return result;
}

neograph::json InterruptHandleArg::InterruptHandleInputItem::toJson() const {
    return neograph::json{
        {"label",        label       },
        {"depict",       depict      },
        {"type",         type        },
        {"enumValues",   enumValues  },
        {"defaultValue", defaultValue},
    };
}

bool InterruptHandleArg::isAccordingFormat(const neograph::json& data) {
    return data.is_object() && data["name"].is_string();
}

std::optional<InterruptHandleArg> InterruptHandleArg::fromJson(const neograph::json& data) {
    if (false == isAccordingFormat(data)) {
        return std::nullopt;
    }
    auto result = InterruptHandleArg{};
    if (data.is_object()) {
        if (data["name"].is_string()) {
            result.name = data["name"].get<std::string>();
        }
        result.arg      = data["arg"];
        result.resultId = data.value("resultId", std::string{});
        if (data["inputs"].is_array()) {
            for (const auto& input : data["inputs"]) {
                result.inputs.push_back(InterruptHandleInputItem::fromJson(input));
            }
        }
    }
    return result;
}

neograph::json InterruptHandleArg::toJson() const {
    auto inputsJson = neograph::json::array();
    for (const auto& item : inputs) {
        inputsJson.push_back(item.toJson());
    }
    return neograph::json{
        {"name",     name      },
        {"arg",      arg       },
        {"inputs",   inputsJson},
        {"resultId", resultId  },
    };
}

std::vector<InterruptHandleArg> InterruptHandleArg::listFromJson(const neograph::json& data) {
    auto relist = std::vector<InterruptHandleArg>{};
    if (data.is_array()) {
        for (const auto& item : data) {
            auto arg = fromJson(item);
            if (arg.has_value()) {
                relist.push_back(arg.value());
            }
        }
    }
    return relist;
}

neograph::json InterruptHandleArg::listToJson(const std::vector<InterruptHandleArg>& data) {
    auto relist = neograph::json::array();
    for (const auto& item : data) {
        relist.push_back(item.toJson());
    }
    return relist;
}

neograph::json MiddlewareContext::anyToJson(const std::any& val) {
    if (!val.has_value()) {
        return nullptr;
    }
    auto& t = val.type();
    if (t == typeid(neograph::json)) {
        return std::any_cast<neograph::json>(val);
    }
    if (t == typeid(std::nullptr_t)) {
        return nullptr;
    }
    if (t == typeid(bool)) {
        return std::any_cast<bool>(val);
    }
    if (t == typeid(int)) {
        return std::any_cast<int>(val);
    }
    if (t == typeid(int64_t)) {
        return std::any_cast<int64_t>(val);
    }
    if (t == typeid(uint64_t)) {
        return std::any_cast<uint64_t>(val);
    }
    if (t == typeid(size_t)) {
        return static_cast<uint64_t>(std::any_cast<size_t>(val));
    }
    if (t == typeid(float)) {
        return static_cast<double>(std::any_cast<float>(val));
    }
    if (t == typeid(double)) {
        return std::any_cast<double>(val);
    }
    if (t == typeid(std::string)) {
        return std::any_cast<std::string>(val);
    }
    if (t == typeid(const char*)) {
        return std::string(std::any_cast<const char*>(val));
    }
    if (t == typeid(std::string_view)) {
        return std::string(std::any_cast<std::string_view>(val));
    }
    if (t == typeid(std::vector<std::string>)) {
        return std::any_cast<const std::vector<std::string>&>(val);
    }
    if (t == typeid(std::vector<neograph::ChatMessage>)) {
        auto& msgs = std::any_cast<const std::vector<neograph::ChatMessage>&>(val);
        auto  arr  = neograph::json::array();
        for (const auto& msg : msgs) {
            neograph::json j;
            neograph::to_json(j, msg);
            arr.push_back(std::move(j));
        }
        return arr;
    }
    if (t == typeid(std::vector<InterruptHandleArg>)) {
        return InterruptHandleArg::listToJson(
            std::any_cast<const std::vector<InterruptHandleArg>&>(val)
        );
    }
    // 未覆盖类型: 记录错误并返回 null，避免静默丢值导致 checkpoint 恢复丢失
    XX_LOGE("MiddlewareContext::anyToJson: unsupported type `{}`, checkpoint value lost", t.name());
    return nullptr;
}

void MiddlewareContext::ensureShareStoreLoaded(std::string_view thread_id) {
    if (!persistence_ || shareStore.contains(thread_id)) {
        return;
    }
    // 已加载过直接返回, 避免空存储反复查询 (O(1))
    if (shareStoreLoaded_.contains(std::string{thread_id})) {
        return;
    }
    // 首次访问: 从 SQLite 恢复全部条目与 id 计数器
    auto loaded = persistence_->loadShareStore(thread_id);
    shareStore.emplace(
        std::string{thread_id},
        ThreadShareStore{.store = std::move(loaded.items), .storeId = loaded.nextId}
    );
    shareStoreLoaded_.insert(std::string{thread_id});
}

std::optional<std::string>
    MiddlewareContext::getShareStoreItemValue(std::string_view thread_id, const size_t id) {
    ensureShareStoreLoaded(thread_id);
    auto it = shareStore.find(thread_id);
    if (shareStore.end() != it) {
        auto result = it->second.store.find(id);
        if (it->second.store.end() != result) {
            return result->second;
        }
    }
    return std::nullopt;
}

void MiddlewareContext::setShareStoreItemValue(
    std::string_view thread_id,
    const size_t     id,
    std::string_view value
) {
    ensureShareStoreLoaded(thread_id);
    // shareStore[thread_id].store[id] = value;

    auto it = shareStore.find(thread_id);
    if (it != shareStore.end()) {
        it->second.store[id] = value;
    } else {
        shareStore.emplace(
            std::string{thread_id},
            MiddlewareContext::ThreadShareStore{
                .store = std::map<size_t, std::string>{{id, std::string{value}}}
            }
        );
    }
    if (persistence_) {
        persistence_->setShareStoreItem(thread_id, id, value);
    }
}

size_t
    MiddlewareContext::addShareStoreItemValue(std::string_view thread_id, std::string_view value) {
    ensureShareStoreLoaded(thread_id);

    // 分配 id:
    // - 注入持久化时由数据库分配 (取现有最大 id + 1, 重启后延续); 落库失败
    //   退回内存分配, 保证本次会话内功能可用
    // - 无持久化时保持原语义: 新 thread 首条为 1, 其后 storeId 递增
    size_t id = 0;
    bool   dbOk = false;
    if (persistence_) {
        id   = persistence_->addShareStoreItem(thread_id, value);
        dbOk = (id != 0);
    }
    if (!dbOk) {
        auto it = shareStore.find(thread_id);
        if (it != shareStore.end()) {
            id = it->second.getNextId();
        } else {
            id = 1;
        }
    }

    auto it = shareStore.find(thread_id);
    if (it != shareStore.end()) {
        it->second.store[id] = std::string{value};
        // 同步内存计数器: 仅 DB 成功时同步到 DB 值, 回退路径已由 getNextId 递增, 不覆盖
        if (dbOk && it->second.storeId <= id) {
            it->second.storeId = id;
        }
    } else {
        shareStore.emplace(
            std::string{thread_id},
            MiddlewareContext::ThreadShareStore{
                .store   = std::map<size_t, std::string>{{id, std::string{value}}},
                .storeId = id,
            }
        );
    }
    return id;
}

void MiddlewareContext::removeShareStoreItemValue(std::string_view thread_id, const size_t id) {
    ensureShareStoreLoaded(thread_id);
    auto it = shareStore.find(thread_id);
    if (shareStore.end() != it) {
        auto resultIt = it->second.store.find(id);
        if (it->second.store.end() != resultIt) {
            it->second.store.erase(resultIt);
        }
    }
    if (persistence_) {
        persistence_->removeShareStoreItem(thread_id, id);
    }
}

void MiddlewareContext::removeGraphDataItem(std::string_view thread_id, std::string_view key) {
    auto it = graphData.find(thread_id);
    if (graphData.end() != it) {
        auto resultIt = it->second.find(key);
        if (it->second.end() != resultIt) {
            it->second.erase(resultIt);
        }
    }
}

void MiddlewareContext::cleanupThread(std::string_view thread_id) {
    graphData.erase(thread_id);
    shareStore.erase(thread_id);
    // 移除"已从持久化加载过"标记, 避免该 thread 再次出现时跳过加载 (O(1))
    shareStoreLoaded_.erase(std::string{thread_id});
    // 各中间件按 thread 的 state
    for (auto& handle : handles) {
        if (handle) {
            handle->states.erase(thread_id);
        }
    }
}

void MiddlewareContext::throwNodeInterruptBase(
    std::string_view      thread_id,
    const neograph::json& msgs
) {
    // if (msgs.is_array()) {
    // 直接抛异常到 neograph::engine 的话会丢失本轮 session 上下文，因此需要临时保存，这里改为交由
    // wrap_handle 保存此时的 上下文 setGraphDataItemValue(thread_id,
    // MiddlewareContext::graphDataKey_tempMessages, msgs);
    // }
    throw neograph::graph::NodeInterrupt{"xx-NodeInterrupt"};
}

asio::awaitable<neograph::json> MiddlewareContext::requestInterrupt(
    std::string_view                           thread_id,
    const std::function<InterruptHandleArg()>& onCreateArg,
    const neograph::json&                      msgs
) {
    auto result = std::move(getGraphDataItemValue<neograph::json>(
        thread_id,
        MiddlewareContext::graphDataKey_interruptResult
    ));
    removeGraphDataItem(thread_id, MiddlewareContext::graphDataKey_interruptResult);
    if (false == result.is_null()) {
        co_return result;
    }

    auto arg = onCreateArg();
    modifyGraphDataItemValue<std::vector<InterruptHandleArg>>(
        thread_id,
        MiddlewareContext::graphDataKey_interruptArgs,
        [&](std::vector<InterruptHandleArg>& args) {
            args.push_back(arg);
        }
    );
    throwNodeInterruptBase(thread_id, msgs);
}

neograph::json MiddlewareContext::getGraphDataToState(
    neograph::graph::GraphState& state,
    std::string_view             thread_id
) {
    neograph::json saved = neograph::json::object();
    auto           it    = graphData.find(thread_id);
    if (it != graphData.end()) {
        for (const auto& [key, val] : it->second) {
            saved[key] = anyToJson(val);
        }
    }
    return saved;
}

void MiddlewareContext::setGraphDataFromState(
    neograph::graph::GraphState& state,
    std::string_view             thread_id
) {
    setGraphDataFromState(state.get(channel_savedGraphData), thread_id);
}

void MiddlewareContext::setGraphDataFromState(neograph::json j, std::string_view thread_id) {
    if (j.is_object()) {
        auto data = std::map<std::string, std::any, std::less<>>{};
        for (auto it = j.begin(); it != j.end(); ++it) {
            data[it.key()] = it.value();
        }
        auto it = graphData.find(thread_id); // find 支持异构查找（使用透明比较器）
        if (it != graphData.end()) {
            it->second = std::move(data);
        } else {
            graphData.emplace(std::string{thread_id}, std::move(data));
        }
    }
}

} // namespace middleware
} // namespace agentxx
