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

  inline static neograph::json
  listToJson(const std::vector<InterruptHandleArg> &data) {
    auto relist = neograph::json::array();
    for (const auto &item : data) {
      relist.push_back(item.toJson());
    }
    return relist;
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
  /// graphData 需要跨 checkpoint 存储时使用该 state channel key
  inline static const std::string channel_savedGraphData{"xx_savedGraphData"};

  inline static const std::string graphDataKey_systemMessage{"systemMessage"};
  inline static const std::string graphDataKey_tempLLMMessage{
      "xx_ModelCallWrap_tempLLMMessage"};
  inline static const std::string graphDataKey_LLMTokenUsage{
      "xx_ModelCallWrap_LLMTokenUsage"};
  inline static const std::string graphDataKey_interruptMessages{
      "xx_interruptMessages"};
  inline static const std::string graphDataKey_interruptArgs{
      "xx_interruptArgs"};
  inline static const std::string graphDataKey_interruptResult{
      "xx_interruptResult"};
  inline static const std::string graphDataKey_interruptToolcallCache{
      "xx_interruptToolcallCache"};

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

  MiddlewareContext() {}

  /// 将 std::any 转为 neograph::json（用于序列化到 state）
  static neograph::json anyToJson(const std::any &val) {
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
      auto &msgs =
          std::any_cast<const std::vector<neograph::ChatMessage> &>(val);
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

  /// 将 neograph::json 转为 T（用于从 state 恢复后按需转换）
  template <typename T> static T jsonToValue(const neograph::json &j) {
    if constexpr (std::is_same_v<T, neograph::json>) {
      return j;
    } else if constexpr (std::is_same_v<T, std::string>) {
      if (j.is_string())
        return j.get<std::string>();
      if (j.is_number())
        return j.dump();
      return {};
    } else if constexpr (std::is_same_v<T, int>) {
      if (j.is_number_integer())
        return j.get<int>();
      return {};
    } else if constexpr (std::is_same_v<T, int64_t>) {
      if (j.is_number_integer())
        return j.get<int64_t>();
      return {};
    } else if constexpr (std::is_same_v<T, double>) {
      if (j.is_number())
        return j.get<double>();
      return {};
    } else if constexpr (std::is_same_v<T, bool>) {
      if (j.is_boolean())
        return j.get<bool>();
      return {};
    } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
      if (j.is_array())
        return j.get<std::vector<std::string>>();
      return {};
    } else if constexpr (std::is_same_v<T,
                                        std::vector<neograph::ChatMessage>>) {
      std::vector<neograph::ChatMessage> msgs;
      if (j.is_array()) {
        for (const auto &item : j) {
          neograph::ChatMessage msg;
          neograph::from_json(item, msg);
          msgs.push_back(std::move(msg));
        }
      }
      return msgs;
    } else if constexpr (std::is_same_v<T, std::vector<InterruptHandleArg>>) {
      std::vector<InterruptHandleArg> msgs;
      if (j.is_array()) {
        msgs = InterruptHandleArg::listFromJson(j);
      }
      return msgs;
    } else {
      static_assert(sizeof(T) == 0, "jsonToValue: unsupported type T");
    }
  }

  /// 确保 std::any 中的值类型为 T，支持双向自动转换:
  ///   - json → T  (从 state 恢复后按需转换回原始类型)
  ///   - 任意类型 → json  (读为 json 格式)
  template <typename T> static void ensureAnyType(std::any &val) {
    if (!val.has_value() || val.type() == typeid(T)) {
      return;
    }
    if (val.type() == typeid(neograph::json)) {
      auto j = std::any_cast<neograph::json>(std::move(val));
      val = jsonToValue<T>(j);
      return;
    }
    if constexpr (std::is_same_v<T, neograph::json>) {
      val = anyToJson(val);
    }
  }

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
    auto &store = shareStore[thread_id];
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
    } else {
      ensureAnyType<T>(it->second);
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
      ensureAnyType<T>(it->second);
      modify(std::any_cast<T &>((it->second)));
    }
  }

  /// 一般用于捕获到 NodeInterrupt 后重新抛出，而不能作为首次抛出使用
  void throwNodeInterruptBase(const std::string &thread_id,
                              const neograph::json &msgs) {
    if (msgs.is_array()) {
      setGraphDataItemValue(
          thread_id, MiddlewareContext::graphDataKey_interruptMessages, msgs);
    }
    throw neograph::graph::NodeInterrupt{"xx-NodeInterrupt"};
  }

  /// 工具请求中断：检查已有结果（resume 后）或存储参数并抛异常
  asio::awaitable<neograph::json>
  requestInterrupt(const std::string &thread_id,
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

  /// 将 graphData 中 JSON 兼容条目序列化到 state channel
  inline neograph::json getGraphDataToState(neograph::graph::GraphState &state,
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

  /// 从 state channel 恢复 graphData (用于中断 resume)
  inline void setGraphDataFromState(neograph::graph::GraphState &state,
                                    const std::string &thread_id) {
    setGraphDataFromState(state.get(channel_savedGraphData), thread_id);
  }

  inline void setGraphDataFromState(const neograph::json &j,
                                    const std::string &thread_id) {
    if (j.is_object()) {
      auto data = std::map<std::string, std::any, std::less<>>{};
      for (auto it = j.begin(); it != j.end(); ++it) {
        data[it.key()] = it.value();
      }
      graphData[thread_id] = std::move(data);
    }
  }
};

} // namespace middleware
} // namespace agentxx