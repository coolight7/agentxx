#include "agentxx/middlewares/middleware.h"
#include "agentxx/tools/tool.h"
#include <charconv>

agentxx::middleware::BaseMiddlewareHandleInterface::
    BaseMiddlewareHandleInterface(
        std::string_view in_name,
        std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
    : name(in_name), agentContext(in_agentContext) {}

agentxx::middleware::BaseMiddlewareHandleInterface::
    ~BaseMiddlewareHandleInterface() = default;

namespace agentxx {
namespace middleware {

neograph::json BaseMiddlewareHandleInterface::getLastMessageJson(
    const neograph::graph::NodeInput &in) {
  auto messages = in.state.get("messages");
  if (messages.is_array() && messages.size() > 0) {
    return messages.back();
  }
  return neograph::json(nullptr);
}

std::optional<neograph::ChatMessage> BaseMiddlewareHandleInterface::getLastMessage(
    const neograph::graph::NodeInput &in) {
  auto lastMsgJson = getLastMessageJson(in);
  if (false == lastMsgJson.is_object()) {
    return std::nullopt;
  }
  auto result = neograph::ChatMessage{};
  neograph::from_json(lastMsgJson, result);
  return result;
}

const neograph::ChatMessage *
BaseMiddlewareHandleInterface::getLastAssistantToolcallMessage(
    std::vector<neograph::ChatMessage> &messages) {
  const neograph::ChatMessage *assistant_msg = nullptr;
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

const neograph::ChatMessage *
BaseMiddlewareHandleInterface::getLastToolcallResultMessage(
    std::vector<neograph::ChatMessage> &messages) {
  const neograph::ChatMessage *tool_msg = nullptr;
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

void BaseMiddlewareHandleInterface::printMessage(const neograph::ChatMessage &msg,
                                                 size_t index) {
  std::string toollist;
  if (false == msg.tool_calls.empty()) {
    toollist += "┣━ Toolcall: \n";
    for (const auto &tool : msg.tool_calls) {
      toollist += fmt::format(R"(  - {}/{}
    {}
)",
                              tool.name, tool.id, tool.arguments);
    }
  }
  XX_OUT(R"(
┏━━━━━━ Message/{} ━━━━━━┓
┣━ Role: {}
{}
┣━ Content: {}
┗━━━━━━ Message/{} ━━━━━━┛
)",
         index, msg.role, toollist, msg.content, index);
}

void BaseMiddlewareHandleInterface::printMessages(
    const std::vector<neograph::ChatMessage> &messages, bool printSystemMsg) {
  size_t index = 0;
  for (const auto &msg : messages) {
    ++index;
    if (false == printSystemMsg && msg.role == "system") {
      continue;
    }
    printMessage(msg, index);
  }
}

InterruptHandleArg::InterruptHandleInputItem
InterruptHandleArg::InterruptHandleInputItem::fromJson(
    const neograph::json &data) {
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
      {"label", label},
      {"depict", depict},
      {"type", type},
      {"enumValues", enumValues},
      {"defaultValue", defaultValue},
  };
}

bool InterruptHandleArg::isAccordingFormat(const neograph::json &data) {
  return data.is_object() && data["name"].is_string();
}

std::optional<InterruptHandleArg>
InterruptHandleArg::fromJson(const neograph::json &data) {
  if (false == isAccordingFormat(data)) {
    return std::nullopt;
  }
  auto result = InterruptHandleArg{};
  if (data.is_object()) {
    if (data["name"].is_string()) {
      result.name = data["name"].get<std::string>();
    }
    result.arg = data["arg"];
    result.resultId = data["resultId"].get<std::string>();
    if (data["inputs"].is_array()) {
      for (const auto &input : data["inputs"]) {
        result.inputs.push_back(InterruptHandleInputItem::fromJson(input));
      }
    }
  }
  return result;
}

neograph::json InterruptHandleArg::toJson() const {
  auto inputsJson = neograph::json::array();
  for (const auto &item : inputs) {
    inputsJson.push_back(item.toJson());
  }
  return neograph::json{
      {"name", name},
      {"arg", arg},
      {"inputs", inputsJson},
      {"resultId", resultId},
  };
}

std::vector<InterruptHandleArg>
InterruptHandleArg::listFromJson(const neograph::json &data) {
  auto relist = std::vector<InterruptHandleArg>{};
  if (data.is_array()) {
    for (const auto &item : data) {
      auto arg = fromJson(item);
      if (arg.has_value()) {
        relist.push_back(arg.value());
      }
    }
  }
  return relist;
}

neograph::json
InterruptHandleArg::listToJson(const std::vector<InterruptHandleArg> &data) {
  auto relist = neograph::json::array();
  for (const auto &item : data) {
    relist.push_back(item.toJson());
  }
  return relist;
}

neograph::json MiddlewareContext::anyToJson(const std::any &val) {
  if (!val.has_value())
    return nullptr;
  auto &t = val.type();
  if (t == typeid(neograph::json))
    return std::any_cast<neograph::json>(val);
  if (t == typeid(std::nullptr_t))
    return nullptr;
  if (t == typeid(bool))
    return std::any_cast<bool>(val);
  if (t == typeid(int))
    return std::any_cast<int>(val);
  if (t == typeid(int64_t))
    return std::any_cast<int64_t>(val);
  if (t == typeid(uint64_t))
    return std::any_cast<uint64_t>(val);
  if (t == typeid(float))
    return static_cast<double>(std::any_cast<float>(val));
  if (t == typeid(double))
    return std::any_cast<double>(val);
  if (t == typeid(std::string))
    return std::any_cast<std::string>(val);
  if (t == typeid(const char *))
    return std::string(std::any_cast<const char *>(val));
  if (t == typeid(std::string_view))
    return std::string(std::any_cast<std::string_view>(val));
  if (t == typeid(std::vector<std::string>)) {
    return std::any_cast<const std::vector<std::string> &>(val);
  }
  if (t == typeid(std::vector<neograph::ChatMessage>)) {
    auto &msgs = std::any_cast<const std::vector<neograph::ChatMessage> &>(val);
    auto arr = neograph::json::array();
    for (const auto &msg : msgs) {
      neograph::json j;
      neograph::to_json(j, msg);
      arr.push_back(std::move(j));
    }
    return arr;
  }
  if (t == typeid(std::vector<InterruptHandleArg>)) {
    return InterruptHandleArg::listToJson(
        std::any_cast<const std::vector<InterruptHandleArg> &>(val));
  }
  return nullptr;
}

std::optional<std::string>
MiddlewareContext::getShareStoreItemValue(std::string_view thread_id,
                                          const int id) {
  auto it = shareStore.find(thread_id);
  if (shareStore.end() != it) {
    auto reslut = it->second.store.find(id);
    if (it->second.store.end() != reslut) {
      return reslut->second;
    }
  }
  return std::nullopt;
}

void MiddlewareContext::setShareStoreItemValue(const std::string &thread_id,
                                               const int id,
                                               std::string_view value) {
  shareStore[thread_id].store[id] = value;
}

size_t MiddlewareContext::addShareStoreItemValue(const std::string &thread_id,
                                                 std::string_view value) {
  auto &store = shareStore[thread_id];
  auto id = store.getNextId();
  store.store[id] = value;
  return id;
}

void MiddlewareContext::removeShareStoreItemValue(std::string_view thread_id,
                                                  const int id) {
  auto it = shareStore.find(thread_id);
  if (shareStore.end() != it) {
    auto reslutIt = it->second.store.find(id);
    if (it->second.store.end() != reslutIt) {
      it->second.store.erase(reslutIt);
    }
  }
}

void MiddlewareContext::removeGraphDataItem(const std::string &thread_id,
                                            std::string_view key) {
  auto it = graphData.find(thread_id);
  if (graphData.end() != it) {
    auto reslutIt = it->second.find(key);
    if (it->second.end() != reslutIt) {
      it->second.erase(reslutIt);
    }
  }
}

void MiddlewareContext::throwNodeInterruptBase(const std::string &thread_id,
                                               const neograph::json &msgs) {
  if (msgs.is_array()) {
    setGraphDataItemValue(thread_id,
                          MiddlewareContext::graphDataKey_interruptMessages,
                          msgs);
  }
  throw neograph::graph::NodeInterrupt{"xx-NodeInterrupt"};
}

asio::awaitable<neograph::json> MiddlewareContext::requestInterrupt(
    const std::string &thread_id,
    const std::function<InterruptHandleArg()> &onCreateArg,
    const neograph::json &msgs) {
  auto &result = getGraphDataItemValue<neograph::json>(
      thread_id, MiddlewareContext::graphDataKey_interruptResult);
  removeGraphDataItem(thread_id,
                      MiddlewareContext::graphDataKey_interruptResult);
  if (false == result.is_null()) {
    co_return result;
  }

  auto arg = onCreateArg();
  modifyGraphDataItemValue<std::vector<InterruptHandleArg>>(
      thread_id, MiddlewareContext::graphDataKey_interruptArgs,
      [&](std::vector<InterruptHandleArg> &args) { args.push_back(arg); });
  throwNodeInterruptBase(thread_id, msgs);
}

neograph::json
MiddlewareContext::getGraphDataToState(neograph::graph::GraphState &state,
                                       const std::string &thread_id) {
  neograph::json saved = neograph::json::object();
  auto it = graphData.find(thread_id);
  if (it != graphData.end()) {
    for (const auto &[key, val] : it->second) {
      saved[key] = anyToJson(val);
    }
  }
  return saved;
}

void MiddlewareContext::setGraphDataFromState(
    neograph::graph::GraphState &state, const std::string &thread_id) {
  setGraphDataFromState(state.get(channel_savedGraphData), thread_id);
}

void MiddlewareContext::setGraphDataFromState(const neograph::json &j,
                                              const std::string &thread_id) {
  if (j.is_object()) {
    auto data = std::map<std::string, std::any, std::less<>>{};
    for (auto it = j.begin(); it != j.end(); ++it) {
      data[it.key()] = it.value();
    }
    graphData[thread_id] = std::move(data);
  }
}

} // namespace middleware
} // namespace agentxx
