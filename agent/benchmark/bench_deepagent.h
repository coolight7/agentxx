#pragma once

#include "agentxx/agent/deepagent.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "bench_util.h"
#include <chrono>
#include <iostream>
#include <string>

namespace agentxx {
namespace bench {

struct DeepAgentBenchConfig {
  std::string openAIBaseUrl;
  std::string openAIApiKey;
  std::string openAIModelName;
  std::string systemPrompt;
  std::string userInput;
  size_t iterations = 5;
};

inline void
benchDeepAgentRunConversationTurnAsync(const DeepAgentBenchConfig &config) {
  std::cout << "\n=== DeepAgent::runConversationTurnAsync Benchmarks ==="
            << std::endl;

  if (config.openAIBaseUrl.empty()) {
    std::cout << "  [SKIP] No OpenAI base URL provided. "
              << "Set env AGENTXX_BENCH_LLM_BASE_URL to enable." << std::endl;
    return;
  }

  auto agentConfig = std::make_shared<agentxx::agent::AgentConfig>();
  agentConfig->modelOpenAIBaseUrl = config.openAIBaseUrl;
  agentConfig->modelOpenAIApiKey = config.openAIApiKey;
  agentConfig->modelOpenAIModelName = config.openAIModelName;
  agentConfig->prompt.systemPrompt = config.systemPrompt;

  std::vector<double> durations;
  durations.reserve(config.iterations);

  for (size_t i = 0; i < config.iterations; ++i) {
    asio::io_context ioCtx;
    agentxx::agent::DeepAgent agent(agentConfig);

    asio::co_spawn(
        ioCtx,
        [&]() -> asio::awaitable<void> {
          co_await agent.init();

          auto start = std::chrono::high_resolution_clock::now();

          neograph::json messages = neograph::json::array();
          auto turnResult = co_await agent.runConversationTurnAsync(
              "bench_session_" + std::to_string(i), config.userInput, true,
              std::move(messages),
              [](const neograph::graph::GraphEvent &event) {
                switch (event.type) {
                case neograph::graph::GraphEvent::Type::LLM_TOKEN:
                  break;
                default:
                  break;
                }
              });

          auto end = std::chrono::high_resolution_clock::now();
          double ns =
              std::chrono::duration<double, std::nano>(end - start).count();
          durations.push_back(ns);

          if (turnResult.hasError) {
            std::cerr << "    [ERROR] Iteration " << i
                      << " failed: " << turnResult.errorMessage << std::endl;
          } else {
            std::cout << "    [INFO] Iteration " << i << " completed, "
                      << turnResult.messages.size() << " messages" << std::endl;
          }
        },
        asio::detached);

    ioCtx.run();
  }

  if (durations.empty()) {
    std::cout << "  [ERROR] No successful iterations" << std::endl;
    return;
  }

  double total_ns = 0;
  double min_ns = durations[0];
  double max_ns = durations[0];
  for (auto d : durations) {
    total_ns += d;
    if (d < min_ns)
      min_ns = d;
    if (d > max_ns)
      max_ns = d;
  }
  double mean_ns = total_ns / static_cast<double>(durations.size());

  double variance = 0;
  for (auto d : durations) {
    double diff = d - mean_ns;
    variance += diff * diff;
  }
  double stddev_ns =
      std::sqrt(variance / static_cast<double>(durations.size()));

  std::sort(durations.begin(), durations.end());
  double median_ns = durations[durations.size() / 2];

  BenchResult r;
  r.name = "DeepAgent::runConversationTurnAsync [LLM end-to-end]";
  r.iterations = durations.size();
  r.total_ns = total_ns;
  r.mean_ns = mean_ns;
  r.min_ns = min_ns;
  r.max_ns = max_ns;
  r.stddev_ns = stddev_ns;
  r.median_ns = median_ns;
  printResult(r);
}

inline void benchDeepAgentInit() {
  std::cout << "\n=== DeepAgent::init Benchmarks ===" << std::endl;

  DeepAgentBenchConfig config;
  config.openAIBaseUrl = std::getenv("AGENTXX_BENCH_LLM_BASE_URL")
                             ? std::getenv("AGENTXX_BENCH_LLM_BASE_URL")
                             : "";
  config.openAIApiKey = std::getenv("AGENTXX_BENCH_LLM_API_KEY")
                            ? std::getenv("AGENTXX_BENCH_LLM_API_KEY")
                            : "EMPTY";
  config.openAIModelName = std::getenv("AGENTXX_BENCH_LLM_MODEL_NAME")
                               ? std::getenv("AGENTXX_BENCH_LLM_MODEL_NAME")
                               : "Agentxx";

  if (config.openAIBaseUrl.empty()) {
    std::cout << "  [SKIP] No OpenAI base URL provided. "
              << "Set env AGENTXX_BENCH_LLM_BASE_URL to enable." << std::endl;
    return;
  }

  auto agentConfig = std::make_shared<agentxx::agent::AgentConfig>();
  agentConfig->modelOpenAIBaseUrl = config.openAIBaseUrl;
  agentConfig->modelOpenAIApiKey = config.openAIApiKey;
  agentConfig->modelOpenAIModelName = config.openAIModelName;

  constexpr size_t iterations = 10;
  std::vector<double> durations;
  durations.reserve(iterations);

  for (size_t i = 0; i < iterations; ++i) {
    asio::io_context ioCtx;
    agentxx::agent::DeepAgent agent(agentConfig);

    asio::co_spawn(
        ioCtx,
        [&]() -> asio::awaitable<void> {
          auto start = std::chrono::high_resolution_clock::now();
          co_await agent.init();
          auto end = std::chrono::high_resolution_clock::now();
          double ns =
              std::chrono::duration<double, std::nano>(end - start).count();
          durations.push_back(ns);
        },
        asio::detached);

    ioCtx.run();
  }

  if (durations.empty()) {
    return;
  }

  double total_ns = 0;
  double min_ns = durations[0];
  double max_ns = durations[0];
  for (auto d : durations) {
    total_ns += d;
    if (d < min_ns)
      min_ns = d;
    if (d > max_ns)
      max_ns = d;
  }
  double mean_ns = total_ns / static_cast<double>(durations.size());

  double variance = 0;
  for (auto d : durations) {
    double diff = d - mean_ns;
    variance += diff * diff;
  }
  double stddev_ns =
      std::sqrt(variance / static_cast<double>(durations.size()));

  std::sort(durations.begin(), durations.end());
  double median_ns = durations[durations.size() / 2];

  BenchResult r;
  r.name = "DeepAgent::init [graph engine compilation]";
  r.iterations = durations.size();
  r.total_ns = total_ns;
  r.mean_ns = mean_ns;
  r.min_ns = min_ns;
  r.max_ns = max_ns;
  r.stddev_ns = stddev_ns;
  r.median_ns = median_ns;
  printResult(r);
}

} // namespace bench
} // namespace agentxx