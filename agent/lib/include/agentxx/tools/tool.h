#pragma once

#include "agentxx/middlewares/middleware.h"
#include "asio/io_context.hpp"
#include "fmt/base.h"
#include "fmt/format.h"
#include <functional>
#include <neograph/llm/rate_limited_provider.h>
#include <neograph/llm/schema_provider.h>
#include <neograph/neograph.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace asio = ::boost::asio;

namespace agentxx {
namespace tools {

class XXToolBase : public neograph::AsyncTool {
protected:
  const std::string name;
  std::weak_ptr<agentxx::agent::AgentContext> agentContext;

public:
  /// - 自动压缩 tool 输出，当长度超过限制值
  /// [agentxx::agent::AgentConfig::toolcallSummaryLimitOutputLength] 时，且该
  /// tool 启用 [autoSummaryOutput] 则进行压缩
  const bool autoSummaryOutput;
  /// - 延迟加载
  /// - `true`: 该 tool 在初始时仅记录名称等简短信息在 system prompt，由
  /// `tool_skill_search` 检索查找合适的 tool 后才加载全量信息并支持LLM调用
  const bool canDelayLoad;
  /// - 最大重试次数
  /// - 如果 [maxRetry] > 0，当 tool 执行抛出异常时，进行重试
  /// - 最多执行 1 + maxRetry(retry) 次
  const size_t maxRetry;

  XXToolBase(std::string_view in_name,
             std::weak_ptr<agentxx::agent::AgentContext> in_agentContext,
             bool in_autoSummaryOutput = false, bool in_canDelayLoad = true,
             size_t in_maxRetry = 0)
      : name(in_name), agentContext(in_agentContext),
        autoSummaryOutput(in_autoSummaryOutput), canDelayLoad(in_canDelayLoad),
        maxRetry(in_maxRetry) {
    extra["autoSummaryOutput"] = autoSummaryOutput ? "true" : "false";
    extra["canDelayLoad"] = canDelayLoad ? "true" : "false";
    extra["maxRetry"] = std::to_string(maxRetry);
  }

  std::string get_name() const override { return name; }

  virtual std::optional<agentxx::middleware::SummarizationToolHandle>
  createSummarizationToolHandle() const {
    return std::nullopt;
    // return agentxx::middleware::SummarizationToolHandle{
    //     .generateDeduplicationKey =
    //         [](const neograph::json &args) -> std::optional<std::string> {
    //           return "tool_name:unique_key";
    //         },
    //     .truncateRequest =
    //         [](neograph::ToolCall &toolcall) {
    //           toolcall.arguments =
    //               R"({"tip":"[Outdated Message Truncated]"})";
    //         },
    //     .truncateResponse =
    //         [](neograph::ChatMessage &msg) {
    //           msg.content = "[Outdated Content truncated]";
    //         },
    // };
  }
};

/// - 封装原始的 [neograph::Tool] 类型，添加额外功能
/// - 部分函数 (如 MCP) 返回的 tool 类型是原始的 [neograph::Tool]，可以用
/// [XXToolWarp] 进行封装扩展功能
class XXToolWarp : public XXToolBase {
protected:
  std::unique_ptr<neograph::Tool> inner;
  std::optional<agentxx::middleware::SummarizationToolHandle>
      summarizationHandle;

public:
  XXToolWarp(std::unique_ptr<neograph::Tool> &&in_inner,
             std::weak_ptr<agentxx::agent::AgentContext> in_agentContext,
             bool in_autoSummaryOutput = false, bool in_canDelayLoad = false,
             size_t in_maxRetry = 0,
             std::optional<agentxx::middleware::SummarizationToolHandle>
                 in_summarizationHandle = std::nullopt)
      : XXToolBase(in_inner->get_name(), in_agentContext, in_autoSummaryOutput,
                   in_canDelayLoad, in_maxRetry),
        inner(std::move(in_inner)),
        summarizationHandle(in_summarizationHandle) {}

  std::string get_name() const override { return inner->get_name(); }

  neograph::ChatTool get_definition() const override {
    return inner->get_definition();
  }

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override {
    co_return co_await inner->real_execute_async(arguments);
  }
};

} // namespace tools
} // namespace agentxx