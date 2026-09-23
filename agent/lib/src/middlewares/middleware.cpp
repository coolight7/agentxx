#include "agentxx/middlewares/middleware.h"
#include "agentxx/agent/session_store.h"
#include "agentxx/tools/tool.h"
#include "agentxx/util/neograph_json_bridge.h"
#include "utilxx_base/container_util.h"
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

utilxx_base::Json
    BaseMiddlewareHandleInterface::getLastMessageJson(const neograph::graph::NodeInput& in) {
    auto messages = in.state.get("messages");
    if (messages.is_array() && messages.size() > 0) {
        return agentxx::util::fromNeographJson(messages.back());
    }
    return utilxx_base::Json(nullptr);
}

std::optional<neograph::ChatMessage>
    BaseMiddlewareHandleInterface::getLastMessage(const neograph::graph::NodeInput& in) {
    auto lastMsgJson = getLastMessageJson(in);
    if (false == lastMsgJson.is_object()) {
        return std::nullopt;
    }
    auto result  = neograph::ChatMessage{};
    auto neoJson = agentxx::util::toNeographJson(lastMsgJson);
    neograph::from_json(neoJson, result);
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

bool InterruptHandleArg::isAccordingFormat(const utilxx_base::Json& data) {
    return data.is_object() && data["name"].is_string();
}

std::optional<InterruptHandleArg> InterruptHandleArg::fromJson(const utilxx_base::Json& data) {
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
        if (data["ui"].is_object()) {
            result.ui = InterruptUi::fromJson(data["ui"]);
        }
    }
    return result;
}

utilxx_base::Json InterruptHandleArg::toJson() const {
    // 描述原样下发 (生产者负责构造; 缺失 = 该中断不进入客户端渲染路径,
    // 客户端按契约错误处理并输出诊断行)
    auto j = utilxx_base::Json{
        {"name",     name       },
        {"arg",      arg        },
        {"resultId", resultId   },
        {"ui",       ui.toJson()},
    };
    return j;
}

std::vector<InterruptHandleArg> InterruptHandleArg::listFromJson(const utilxx_base::Json& data) {
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

utilxx_base::Json InterruptHandleArg::listToJson(const std::vector<InterruptHandleArg>& data) {
    auto relist = utilxx_base::Json::array();
    for (const auto& item : data) {
        relist.push_back(item.toJson());
    }
    return relist;
}

utilxx_base::Json MiddlewareContext::anyToJson(const std::any& val) {
    if (!val.has_value()) {
        return nullptr;
    }
    auto& t = val.type();
    if (t == typeid(utilxx_base::Json)) {
        return std::any_cast<utilxx_base::Json>(val);
    }
    // 兼容: 历史路径可能仍存入 neograph::json (如未迁移的调用点),
    // 经桥接转为业务 Json, 避免 checkpoint 落盘丢值
    if (t == typeid(neograph::json)) {
        return agentxx::util::fromNeographJson(std::any_cast<neograph::json>(val));
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
        auto  arr  = utilxx_base::Json::array();
        for (const auto& msg : msgs) {
            neograph::json nj;
            neograph::to_json(nj, msg);
            arr.push_back(agentxx::util::fromNeographJson(nj));
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

MiddlewareContext::SessionShareStore& MiddlewareContext::shareStoreState(std::string_view sessionId
) {
    auto it = shareStore.find(sessionId);
    if (shareStore.end() != it) {
        return it->second;
    }
    // 首次访问该会话: 只取回自增 id 计数 (max(id)), 不读条目内容 ——
    // 内容按 id 在取值时单独读取, 内存占用与条目总数无关
    SessionShareStore state;
    if (persistence_) {
        state.lastId = persistence_->shareStoreLastId(sessionId);
    }
    auto [insertIt, _]
        = utilxx_base::insertHeterogeneous(shareStore, std::string{sessionId}, std::move(state));
    return insertIt->second;
}

void MiddlewareContext::checkShareStoreId(
    const SessionShareStore& state,
    std::string_view         sessionId,
    const size_t             id
) {
    // id 从 1 开始分配: 0 与大于自增 id 的值都表示"从未分配过的条目"
    if (0 == id || id > state.lastId) {
        XX_LOGD(
            "MiddlewareContext: share_store id out of range (session={}, id={}, max={})",
            sessionId,
            id,
            state.lastId
        );
        throw std::invalid_argument{fmt::format(
            "Share store id({}) is out of range, max allocated id is {}",
            id,
            state.lastId
        )};
    }
}

void MiddlewareContext::ensureShareStoreItemCached(
    SessionShareStore& state,
    std::string_view   sessionId,
    const size_t       id
) {
    if (!persistence_ || state.cache.exists(id)) {
        return;
    }
    auto value = persistence_->getShareStoreItem(sessionId, id);
    if (value.has_value()) {
        state.cache.put(id, std::move(value.value()));
    }
}

std::optional<std::string>
    MiddlewareContext::getShareStoreItemValue(std::string_view sessionId, const size_t id) {
    auto& state = shareStoreState(sessionId);
    checkShareStoreId(state, sessionId, id);
    if (persistence_) {
        // 命中缓存直接返回; 未命中则回库读取并填入缓存 (下次访问即命中)
        ensureShareStoreItemCached(state, sessionId, id);
        return state.cache.get(id);
    }
    // 无持久化: 内存是唯一副本, 全部条目都在 map 中
    auto it = state.items.find(id);
    if (state.items.end() != it) {
        return it->second;
    }
    return std::nullopt;
}

void MiddlewareContext::setShareStoreItemValue(
    std::string_view sessionId,
    const size_t     id,
    std::string_view value
) {
    auto& state = shareStoreState(sessionId);
    checkShareStoreId(state, sessionId, id);
    if (persistence_) {
        // 缓存未命中时先按 id 回库读取填入 (与其他读取路径一致), 再覆盖新值
        ensureShareStoreItemCached(state, sessionId, id);
        persistence_->setShareStoreItem(sessionId, id, value);
        state.cache.put(id, std::string{value});
        return;
    }
    state.items[id] = std::string{value};
}

size_t
    MiddlewareContext::addShareStoreItemValue(std::string_view sessionId, std::string_view value) {
    auto& state = shareStoreState(sessionId);

    // 分配 id:
    // - 注入持久化时由数据库分配 (取现有最大 id + 1, 重启后延续); 落库失败
    //   退回内存计数递增, 保证本次会话内功能可用
    // - 无持久化时按内存计数递增: 新 session 首条为 1
    size_t id = persistence_ ? persistence_->addShareStoreItem(sessionId, value) : size_t{0};
    if (0 == id) {
        id = state.lastId + 1;
    }
    if (id > state.lastId) {
        state.lastId = id;
    }
    // 新条目写入内存副本 (最近使用, 后续读取直接命中)
    if (persistence_) {
        state.cache.put(id, std::string{value});
    } else {
        state.items[id] = std::string{value};
    }
    return id;
}

void MiddlewareContext::removeGraphDataItem(std::string_view sessionId, std::string_view key) {
    auto it = graphData.find(sessionId);
    if (graphData.end() != it) {
        auto resultIt = it->second.find(key);
        if (it->second.end() != resultIt) {
            it->second.erase(resultIt);
        }
    }
}

void MiddlewareContext::cleanupSession(std::string_view sessionId) {
    // 异构查找删除, 免除 string_view→string 拷贝 (libc++ 无 C++23 异构 erase)
    utilxx_base::eraseHeterogeneous(graphData, sessionId);
    utilxx_base::eraseHeterogeneous(shareStore, sessionId);
    // 各中间件按 session 的 state
    for (auto& handle : handles) {
        if (handle) {
            utilxx_base::eraseHeterogeneous(handle->states, sessionId);
        }
    }
}

void MiddlewareContext::throwNodeInterruptBase(
    std::string_view         sessionId,
    const utilxx_base::Json& msgs
) {
    // if (msgs.is_array()) {
    // 直接抛异常到 neograph::engine 的话会丢失本轮 session 上下文，因此需要临时保存，这里改为交由
    // wrap_handle 保存此时的 上下文 setGraphDataItemValue(sessionId,
    // MiddlewareContext::graphDataKey_tempMessages, msgs);
    // }
    throw neograph::graph::NodeInterrupt{"xx-NodeInterrupt"};
}

asio::awaitable<utilxx_base::Json> MiddlewareContext::requestInterrupt(
    std::string_view                           sessionId,
    const std::function<InterruptHandleArg()>& onCreateArg,
    const utilxx_base::Json&                   msgs
) {
    auto result = std::move(getGraphDataItemValue<utilxx_base::Json>(
        sessionId,
        MiddlewareContext::graphDataKey_interruptResult
    ));
    removeGraphDataItem(sessionId, MiddlewareContext::graphDataKey_interruptResult);
    if (false == result.is_null()) {
        co_return result;
    }

    auto arg = onCreateArg();
    modifyGraphDataItemValue<std::vector<InterruptHandleArg>>(
        sessionId,
        MiddlewareContext::graphDataKey_interruptArgs,
        [&](std::vector<InterruptHandleArg>& args) {
            args.push_back(arg);
        }
    );
    throwNodeInterruptBase(sessionId, msgs);
}

neograph::json MiddlewareContext::getGraphDataToState(
    neograph::graph::GraphState& state,
    std::string_view             sessionId
) {
    neograph::json saved = neograph::json::object();
    auto           it    = graphData.find(sessionId);
    if (it != graphData.end()) {
        for (const auto& [key, val] : it->second) {
            saved[key] = agentxx::util::toNeographJson(anyToJson(val));
        }
    }
    return saved;
}

void MiddlewareContext::setGraphDataFromState(
    neograph::graph::GraphState& state,
    std::string_view             sessionId
) {
    setGraphDataFromState(
        agentxx::util::fromNeographJson(state.get(channel_savedGraphData)),
        sessionId
    );
}

void MiddlewareContext::setGraphDataFromState(utilxx_base::Json j, std::string_view sessionId) {
    if (j.is_object()) {
        auto data = std::map<std::string, std::any, std::less<>>{};
        for (auto it = j.begin(); it != j.end(); ++it) {
            utilxx_base::insertOrAssignHeterogeneous(data, it.key(), it.value());
        }
        utilxx_base::insertOrAssignHeterogeneous(graphData, sessionId, std::move(data));
    }
}

} // namespace middleware
} // namespace agentxx
