#pragma once

#include "agentxx/agent/context.h"
#include "agentxx/util/log.h"
#include "asio/io_context.hpp"
#include "fmt/format.h"
#include <any>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <neograph/llm/rate_limited_provider.h>
#include <neograph/llm/schema_provider.h>
#include <neograph/neograph.h>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace asio = ::boost::asio;

namespace agentxx {
namespace tools {
class XXToolBase;
class XXToolWarp;
} // namespace tools

namespace middleware {

using onGraphNodeBeforeCallFunc =
    std::function<asio::awaitable<void>(neograph::graph::NodeInput &in)>;
using onGraphNodeAfterCallFunc = std::function<asio::awaitable<void>(
    const neograph::graph::NodeInput &in, neograph::graph::NodeOutput &result)>;

class MiddlewareContext;
class InterruptHandleArg;

class BaseMiddlewareState {
public:
  BaseMiddlewareState() {}

  virtual ~BaseMiddlewareState() {}
};

template <typename T>
concept BaseMiddlewareStateType = std::same_as<T, BaseMiddlewareState> ||
                                  std::derived_from<T, BaseMiddlewareState>;

/// 接口类型
/// - 主要用于接收多种泛型参数, 见
/// [MiddlewareContext::handles]，handles
///   需要接收多种不同继承后的模版类型
///   BaseMiddlewareHandle<BaseMiddlewareStateType>，当 state
///   被继承时编译会失败，因此拉出 [BaseMiddlewareHandleInterface] 无 state
///   模版参数作为基本类型
class BaseMiddlewareHandleInterface {
protected:
public:
  inline static const std::string channelKey_interruptMessages{
      "xx_interruptMessages"};
  inline static const std::string channelKey_interruptToolcallCache{
      "xx_interruptToolcallCache"};
  inline static const std::string channelKey_interruptArgs{"xx_interruptArg"};
  inline static const std::string channelKey_interruptResult{
      "xx_interruptResults"};
  /// 谨慎存储/修改 middleware 中的变量，
  /// 这是一个agent中所有会话共享的，单会话变量应该放 state 内

  /// 名称
  std::string name;
  std::weak_ptr<agentxx::agent::AgentContext> agentContext;
  /// 会被添加移动到 agent 中，完成后此处留空数组
  std::vector<std::unique_ptr<agentxx::tools::XXToolBase>> toolcalls{};
  /// 每个 [Middleware] 全局共享，按会话ID 取值 <thread_id, state>
  std::map<std::string, std::shared_ptr<BaseMiddlewareState>> states{};

  BaseMiddlewareHandleInterface(
      std::string_view in_name,
      std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

  /// ================ warp call ================
  virtual asio::awaitable<void>
  onAgentcallStartFunc(neograph::graph::NodeInput &in) = 0;

  virtual asio::awaitable<void>
  onAgentcallEndFunc(const neograph::graph::NodeInput &in,
                     neograph::graph::NodeOutput &result) = 0;

  virtual asio::awaitable<void>
  onModelcallStartFunc(neograph::graph::NodeInput &in) = 0;

  virtual asio::awaitable<void>
  onModelcallRunFunc(neograph::graph::NodeInput &in) = 0;

  virtual asio::awaitable<void>
  onModelcallEndFunc(const neograph::graph::NodeInput &in,
                     neograph::graph::NodeOutput &result) = 0;

  virtual asio::awaitable<void>
  onToolcallStartFunc(neograph::graph::NodeInput &in) = 0;

  virtual asio::awaitable<void>
  onToolcallEndFunc(const neograph::graph::NodeInput &in,
                    neograph::graph::NodeOutput &result) = 0;

  virtual ~BaseMiddlewareHandleInterface() = default;

  inline static neograph::json
  getLastMessageJson(const neograph::graph::NodeInput &in) {
    auto messages = in.state.get("messages");
    if (messages.is_array() && messages.size() > 0) {
      return messages.back();
    }
    return neograph::json(nullptr);
  }

  inline static std::optional<neograph::ChatMessage>
  getLastMessage(const neograph::graph::NodeInput &in) {
    auto lastMsgJson = getLastMessageJson(in);
    if (false == lastMsgJson.is_object()) {
      return std::nullopt;
    }
    auto result = neograph::ChatMessage{};
    neograph::from_json(lastMsgJson, result);
    return result;
  }

  inline static const neograph::ChatMessage *getLastAssistantToolcallMessage(
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

  inline static const neograph::ChatMessage *
  getLastToolcallResultMessage(std::vector<neograph::ChatMessage> &messages) {
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

  inline static void
  printMessages(const std::vector<neograph::ChatMessage> &messages,
                bool printSystemMsg = true) {
    size_t index = 0;
    for (const auto &msg : messages) {
      ++index;
      if (false == printSystemMsg && msg.role == "system") {
        continue;
      }
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
  }
};

template <BaseMiddlewareStateType T>
class BaseMiddlewareHandle : public BaseMiddlewareHandleInterface {
protected:
public:
  BaseMiddlewareHandle(
      std::string_view in_name,
      std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
      : BaseMiddlewareHandleInterface(in_name, in_agentContext) {}

  asio::awaitable<void>
  onAgentcallStartFunc(neograph::graph::NodeInput &in) override {
    co_return;
  }

  asio::awaitable<void>
  onAgentcallEndFunc(const neograph::graph::NodeInput &in,
                     neograph::graph::NodeOutput &result) override {
    co_return;
  }

  asio::awaitable<void>
  onModelcallStartFunc(neograph::graph::NodeInput &in) override {
    co_return;
  }

  asio::awaitable<void>
  onModelcallRunFunc(neograph::graph::NodeInput &in) override {
    co_return;
  }

  asio::awaitable<void>
  onModelcallEndFunc(const neograph::graph::NodeInput &in,
                     neograph::graph::NodeOutput &result) override {
    co_return;
  }

  asio::awaitable<void>
  onToolcallStartFunc(neograph::graph::NodeInput &in) override {
    co_return;
  }

  asio::awaitable<void>
  onToolcallEndFunc(const neograph::graph::NodeInput &in,
                    neograph::graph::NodeOutput &result) override {
    co_return;
  }

  /// ================ state ================
  virtual asio::awaitable<void>
  stateReadBlock(const std::function<asio::awaitable<void>()> &func) {
    if (nullptr != func) {
      co_await func();
    }
  }

  virtual asio::awaitable<void>
  stateWriteBlock(const std::function<asio::awaitable<void>()> &func) {
    if (nullptr != func) {
      co_await func();
    }
  }

  /// 延迟加载 state
  /// - 如果 thread 很多，可以等需要时从硬盘加载进内存
  virtual asio::awaitable<std::shared_ptr<T>>
  loadStateItem(const std::string &thread_id) {
    // TODO: 从磁盘读取
    auto ptr = std::make_shared<T>();
    states[thread_id] = ptr;
    co_return ptr;
  }

  virtual asio::awaitable<std::shared_ptr<T>>
  getStateItem(const std::string &thread_id) {
    {
      auto it = states.find(thread_id);
      if (it != states.end()) {
        co_return (std::static_pointer_cast<T>(it->second));
      }
    }
    co_return co_await loadStateItem(thread_id);
  }

  virtual asio::awaitable<void> saveStateItem(const std::string &thread_id,
                                              bool offload = true) {
    std::shared_ptr<agentxx::middleware::BaseMiddlewareState> oldEntity =
        nullptr;
    bool doSave = false;
    if (offload) {
      {
        auto it = states.find(thread_id);
        if (it != states.end()) {
          doSave = true;
          oldEntity = states.erase(it)->second;
        }
      }
    } else {
      auto it = states.find(thread_id);
      if (it != states.end()) {
        doSave = true;
        oldEntity = it->second;
      }
    }
    if (doSave && nullptr != oldEntity) {
      // TODO: old 写入磁盘
    }
    co_return;
  }

  virtual bool containsItem(const std::string &thread_id) {
    return states.contains(thread_id);
  }
};

template <BaseMiddlewareStateType T>
class MiddlewareWarpHandle : public BaseMiddlewareHandle<T> {
public:
  onGraphNodeBeforeCallFunc onAgentcallStart;
  onGraphNodeAfterCallFunc onAgentcallEnd;
  onGraphNodeBeforeCallFunc onModelcallStart;
  onGraphNodeBeforeCallFunc onModelcallRun;
  onGraphNodeAfterCallFunc onModelcallEnd;
  onGraphNodeBeforeCallFunc onToolcallStart;
  onGraphNodeAfterCallFunc onToolcallEnd;

  MiddlewareWarpHandle(
      std::string_view in_name,
      std::weak_ptr<agentxx::agent::AgentContext> in_agentContext,
      const onGraphNodeBeforeCallFunc &in_onAgentcallStart = nullptr,
      const onGraphNodeAfterCallFunc &in_onAgentcallEnd = nullptr,
      const onGraphNodeBeforeCallFunc &in_onModelcallStart = nullptr,
      const onGraphNodeBeforeCallFunc &in_onModelcallRun = nullptr,
      const onGraphNodeAfterCallFunc &in_onModelcallEnd = nullptr,
      const onGraphNodeBeforeCallFunc &in_onToolcallStart = nullptr,
      const onGraphNodeAfterCallFunc &in_onToolcallEnd = nullptr)
      : BaseMiddlewareHandle<T>(in_name, in_agentContext),
        onAgentcallStart(in_onAgentcallStart),
        onAgentcallEnd(in_onAgentcallEnd),
        onModelcallStart(in_onModelcallStart),
        onModelcallRun(in_onModelcallRun), onModelcallEnd(in_onModelcallEnd),
        onToolcallStart(in_onToolcallStart), onToolcallEnd(in_onToolcallEnd) {}

  asio::awaitable<void>
  onAgentcallStartFunc(neograph::graph::NodeInput &in) override {
    if (nullptr != onAgentcallStart) {
      co_await onAgentcallStart(in);
    }
  }

  asio::awaitable<void>
  onAgentcallEndFunc(const neograph::graph::NodeInput &in,
                     neograph::graph::NodeOutput &result) override {
    if (nullptr != onAgentcallEnd) {
      co_await onAgentcallEnd(in, result);
    }
  }

  asio::awaitable<void>
  onModelcallStartFunc(neograph::graph::NodeInput &in) override {
    if (nullptr != onModelcallStart) {
      co_await onModelcallStart(in);
    }
  }

  asio::awaitable<void>
  onModelcallRunFunc(neograph::graph::NodeInput &in) override {
    if (nullptr != onModelcallRun) {
      co_await onModelcallRun(in);
    }
  }

  asio::awaitable<void>
  onModelcallEndFunc(const neograph::graph::NodeInput &in,
                     neograph::graph::NodeOutput &result) override {
    if (nullptr != onModelcallEnd) {
      co_await onModelcallEnd(in, result);
    }
  }

  asio::awaitable<void>
  onToolcallStartFunc(neograph::graph::NodeInput &in) override {
    if (nullptr != onToolcallStart) {
      co_await onToolcallStart(in);
    }
  }

  asio::awaitable<void>
  onToolcallEndFunc(const neograph::graph::NodeInput &in,
                    neograph::graph::NodeOutput &result) override {
    if (nullptr != onToolcallEnd) {
      co_await onToolcallEnd(in, result);
    }
  }
};

class MiddlewareContext {
public:
  class ThreadShareStore {
  public:
    std::map<size_t, std::string> store{};
    size_t storeId = 1;

    size_t getNextId() { return storeId++; }
  };

  inline static const std::string interruptHandleName_default = "default";

  inline static const std::string graphDataKey_systemMessage{"systemMessage"};
  inline static const std::string graphDataKey_tempLLMMessage{
      "xx_ModelCallWrap_tempLLMMessage"};
  inline static const std::string graphDataKey_LLMTokenUsage{
      "xx_ModelCallWrap_LLMTokenUsage"};

  /// <thread_id, <id, value>>
  /// - 存储变量内容，留出 id 到 上下文中，llm 需要时可以通过
  /// toolcall/share_store 读取
  /// - 如: 压缩上下文时会将部分长文本存入这里替换为 id
  std::map<std::string, ThreadShareStore, std::less<>> shareStore{};

  /// <thread_id, itemData>
  /// [会话独立] 每次执行的临时数据，在 [AgentStartCall] 时刷新，在
  /// [AgentEndCall] 时清理
  std::map<std::string, std::map<std::string, std::any, std::less<>>,
           std::less<>>
      graphData{};

  /// 用基类声明类型，以便支持插入不同子类
  /// - 中间的指针是必要的，直接写 std::vector<BaseMiddlewareHandleInterface>
  /// 的话元素大小是 固定为基类大小，插入子类时内存会被截断，导致后续异常
  std::vector<std::shared_ptr<BaseMiddlewareHandleInterface>> handles{};

  /// <name, handle>
  std::map<std::string, std::function<asio::awaitable<neograph::json>(
                            const InterruptHandleArg &)>>
      interruptHandles{};

  MiddlewareContext() { registerInterruptHandles(); }

  void registerInterruptHandles();

  std::optional<std::string> getShareStoreItemValue(std::string_view thread_id,
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

  void setShareStoreItemValue(const std::string &thread_id, const int id,
                              std::string_view value) {
    shareStore[thread_id].store[id] = value;
  }

  size_t addShareStoreItemValue(const std::string &thread_id,
                                std::string_view value) {
    auto store = shareStore[thread_id];
    auto id = store.getNextId();
    store.store[id] = value;
    return id;
  }

  void removeShareStoreItemValue(std::string_view thread_id, const int id) {
    auto it = shareStore.find(thread_id);
    if (shareStore.end() != it) {
      auto reslutIt = it->second.store.find(id);
      if (it->second.store.end() != reslutIt) {
        it->second.store.erase(reslutIt);
      }
    }
  }

  void removeGraphDataItem(const std::string &thread_id, std::string_view key) {
    auto it = graphData.find(thread_id);
    if (graphData.end() != it) {
      auto reslutIt = it->second.find(key);
      if (it->second.end() != reslutIt) {
        it->second.erase(reslutIt);
      }
    }
  }

  template <typename T>
  T &getGraphDataItemValue(const std::string &thread_id, std::string_view key) {
    auto &itemGraphData = graphData[thread_id];
    auto it = itemGraphData.find(key);
    if (it == itemGraphData.end()) {
      auto [insertIt, _] =
          itemGraphData.insert(std::pair<std::string, std::any>{key, T{}});
      it = insertIt;
    }
    return std::any_cast<T &>(it->second);
  }

  template <typename T>
  void setGraphDataItemValue(const std::string &thread_id, std::string_view key,
                             const T &value) {
    auto &itemGraphData = graphData[thread_id];
    auto it = itemGraphData.find(key);
    if (it == itemGraphData.end()) {
      itemGraphData.insert(std::pair<std::string, std::any>{key, value});
    } else {
      it->second = value;
    }
  }

  template <typename T>
  void modifyGraphDataItemValue(const std::string &thread_id,
                                std::string_view key,
                                std::function<void(T &)> &&modify) {
    auto &itemGraphData = graphData[thread_id];
    auto it = itemGraphData.find(key);
    if (it == itemGraphData.end()) {
      auto value = T{};
      modify(value);
      itemGraphData.insert(
          std::pair<std::string, std::any>{key, std::move(value)});
    } else {
      modify(std::any_cast<T &>((it->second)));
    }
  }

  asio::awaitable<std::optional<neograph::json>>
  execInterruptHandle(std::string_view name,
                      const agentxx::middleware::InterruptHandleArg &arg);
};

class SummarizationToolHandle {
public:
  /// 根据 tool call 参数生成去重 key。
  /// 返回 std::nullopt 表示该次调用不需要去重。
  std::function<std::optional<std::string>(const neograph::json &args)>
      generateDeduplicationKey;

  /// 当发现重复（旧数据已被新数据覆盖）时，截断旧的 toolcall request
  std::function<void(neograph::ToolCall &)> truncateRequest;

  /// 当发现重复（旧数据已被新数据覆盖）时，截断旧的 toolcall response
  std::function<void(neograph::ChatMessage &)> truncateResponse;
};

class InterruptHandleArg {
public:
  class InterruptHandleInputItem {
  public:
    std::string label;
    std::string depict;
    /// bool / int / double / string / enum
    std::string type;
    std::string defaultValue;
    std::vector<std::string> enumValues;

    inline static InterruptHandleInputItem
    fromJson(const neograph::json &data) {
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
          result.enumValues =
              data["enumValues"].get<std::vector<std::string>>();
        }
        if (data["defaultValue"].is_string()) {
          result.defaultValue = data["defaultValue"].get<std::string>();
        }
      }
      return result;
    }

    neograph::json toJson() const {
      return neograph::json{
          {"label", label},
          {"depict", depict},
          {"type", type},
          {"enumValues", enumValues},
          {"defaultValue", defaultValue},
      };
    }
  };

  std::string name;
  neograph::json arg;
  std::vector<InterruptHandleInputItem> inputs;
  std::string resultId;

  /// 一般用于捕获到 NodeInterrupt 后重新抛出，而不能作为首次抛出使用
  inline static void throwNodeInterruptBase() {
    throw neograph::graph::NodeInterrupt{"xx-NodeInterrupt"};
  }

  inline static void throwInterrupt(
      const std::vector<InterruptHandleArg> &args,
      neograph::graph::GraphState &state,
      const neograph::json &toolcallCache = neograph::json{nullptr}) {
    state.overwrite(BaseMiddlewareHandleInterface::channelKey_interruptMessages,
                    state.get("messages"));
    auto arglist = neograph::json::array();
    for (const auto &arg : args) {
      arglist.push_back(arg.toJson());
    }
    // 添加，而非覆盖，以便支持多个 toolcall 中断
    state.write(BaseMiddlewareHandleInterface::channelKey_interruptArgs,
                arglist);
    state.remove(BaseMiddlewareHandleInterface::channelKey_interruptResult);
    throwNodeInterruptBase();
  }

  /// - 一般应当在 Node/toolcall 中触发,中断恢复会重新执行这个
  /// Node,中断前的代码会重复执行!
  inline static neograph::json
  getInterruptResult(neograph::graph::GraphState &state,
                     std::function<InterruptHandleArg(void)> onCreateArg) {
    auto result =
        state.get(BaseMiddlewareHandleInterface::channelKey_interruptResult);
    state.remove(BaseMiddlewareHandleInterface::channelKey_interruptResult);
    if (false == result.is_null()) {
      return result;
    }
    auto arg = onCreateArg();
    throwInterrupt({arg}, state);
    return result;
  }

  inline static bool isAccordingFormat(const neograph::json &data) {
    return data.is_object() && data["name"].is_string();
  }

  inline static std::optional<InterruptHandleArg>
  fromJson(const neograph::json &data) {
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

  inline static std::vector<InterruptHandleArg>
  listFromJson(const neograph::json &data) {
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

  neograph::json toJson() const {
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
};

} // namespace middleware
} // namespace agentxx