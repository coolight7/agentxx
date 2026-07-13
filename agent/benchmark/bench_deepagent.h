#pragma once

#include "agentxx/agent/deepagent.h"
#include "agentxx/util/http_server.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "bench_util.h"
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace agentxx {
namespace bench {

// ===========================================================================
// Local LLM Simulator — an OpenAI-compatible HTTP server
// ===========================================================================
//
// Configure response content via these globals BEFORE calling
// startLlmSimServer(). The server runs on 127.0.0.1:0 (OS-assigned port) and
// supports both streaming (SSE) and non-streaming POST /v1/chat/completions.
//
// Usage:
//   g_llm_sim_response_content = "your custom response";
//   auto sim = startLlmSimServer();
//   // ... use sim.port (e.g. http://127.0.0.1:<port>) as modelOpenAIBaseUrl
//   ... sim.stop();
// ===========================================================================
inline std::string g_llm_sim_response_content =
    "Hello! I am a simulated LLM response for benchmarking purposes. "
    "This content is fully configurable via the global variable.";
inline int g_llm_sim_prompt_tokens = 100;
inline int g_llm_sim_completion_tokens = 50;
/// Tool calls JSON array.  Leave empty for a content-only response.
/// Format:
/// [{"index":0,"id":"call_1","type":"function","function":{"name":"tool","arguments":"{}"}}]
inline neograph::json g_llm_sim_tool_calls = neograph::json::array();

struct LlmSimServer {
  std::unique_ptr<agentxx::util::HttpServer> svr;
  std::thread thr;
  uint16_t port = 0;

  LlmSimServer() = default;

  LlmSimServer(LlmSimServer &&o) noexcept
      : svr(std::move(o.svr)), thr(std::move(o.thr)), port(o.port) {
    o.port = 0;
  }

  LlmSimServer &operator=(LlmSimServer &&o) noexcept {
    if (this != &o) {
      stop();
      svr = std::move(o.svr);
      thr = std::move(o.thr);
      port = o.port;
      o.port = 0;
    }
    return *this;
  }

  LlmSimServer(const LlmSimServer &) = delete;
  LlmSimServer &operator=(const LlmSimServer &) = delete;

  ~LlmSimServer() { stop(); }

  void stop() {
    if (svr)
      svr->stop();
    if (thr.joinable())
      thr.join();
    svr.reset();
    port = 0;
  }
};

inline LlmSimServer startLlmSimServer() {
  LlmSimServer sim;

  agentxx::util::HttpServer::Config cfg;
  cfg.address = "127.0.0.1";
  cfg.port = 0;
  cfg.ioThreads = 1;
  cfg.accessLogEnabled = false;
  cfg.maxConnections = 128;
  cfg.maxRequestBody = 1024 * 1024;

  sim.svr = std::make_unique<agentxx::util::HttpServer>(cfg);
  auto *rawSvr = sim.svr.get();

  // POST /v1/chat/completions
  rawSvr->router().add(
      "/v1/chat/completions", 2,
      std::make_shared<agentxx::util::HttpServer::Handler>(
          [](agentxx::util::HttpServer::Request &req,
             agentxx::util::HttpServer::Response &resp,
             const std::string &) -> hical::Awaitable<void> {
            namespace http = boost::beast::http;

            auto j = neograph::json::parse(req.body());
            bool stream = j.value("stream", false);
            bool hasToolCalls = !g_llm_sim_tool_calls.empty();

            if (stream) {
              std::string sseBody;
              auto append = [&](const neograph::json &delta,
                                const std::string &finishReason) {
                auto ev = neograph::json::object();
                ev["id"] = "chatcmpl-bench-sim";
                ev["object"] = "chat.completion.chunk";
                ev["created"] = 1234567890;
                ev["model"] = "bench-sim";

                auto choice = neograph::json::object();
                choice["index"] = 0;
                choice["delta"] = delta;
                if (finishReason.empty())
                  choice["finish_reason"] = nullptr;
                else
                  choice["finish_reason"] = finishReason;
                ev["choices"] = neograph::json::array({choice});

                sseBody += "data: " + ev.dump() + "\n\n";
              };

              {
                auto d = neograph::json::object();
                d["role"] = "assistant";
                if (hasToolCalls)
                  d["content"] = nullptr;
                else
                  d["content"] = "";
                append(d, "");
              }

              if (hasToolCalls) {
                auto d = neograph::json::object();
                d["tool_calls"] = g_llm_sim_tool_calls;
                append(d, "");
                append(neograph::json::object(), "tool_calls");
              } else {
                auto &content = g_llm_sim_response_content;
                std::string acc;
                for (size_t i = 0; i < content.size(); ++i) {
                  acc += content[i];
                  if (content[i] == ' ' || acc.size() >= 10 ||
                      i == content.size() - 1) {
                    auto d = neograph::json::object();
                    d["content"] = acc;
                    append(d, "");
                    acc.clear();
                  }
                }
                if (!acc.empty()) {
                  auto d = neograph::json::object();
                  d["content"] = acc;
                  append(d, "");
                }
                append(neograph::json::object(), "stop");
              }

              sseBody += "data: [DONE]\n\n";

              resp.result(http::status::ok);
              resp.set(http::field::content_type, "text/event-stream");
              resp.set(http::field::cache_control, "no-cache");
              resp.body() = std::move(sseBody);
              resp.prepare_payload();
            } else {
              auto msg = neograph::json::object();
              msg["role"] = "assistant";
              if (hasToolCalls) {
                msg["content"] = nullptr;
                msg["tool_calls"] = g_llm_sim_tool_calls;
              } else {
                msg["content"] = g_llm_sim_response_content;
              }

              auto choice = neograph::json::object();
              choice["index"] = 0;
              choice["message"] = msg;
              choice["finish_reason"] = hasToolCalls ? "tool_calls" : "stop";

              auto usage = neograph::json::object();
              usage["prompt_tokens"] = g_llm_sim_prompt_tokens;
              usage["completion_tokens"] = g_llm_sim_completion_tokens;
              usage["total_tokens"] =
                  g_llm_sim_prompt_tokens + g_llm_sim_completion_tokens;

              auto respBody = neograph::json::object();
              respBody["id"] = "chatcmpl-bench-sim";
              respBody["object"] = "chat.completion";
              respBody["created"] = 1234567890;
              respBody["model"] = "bench-sim";
              respBody["choices"] = neograph::json::array({choice});
              respBody["usage"] = usage;

              resp.result(http::status::ok);
              resp.set(http::field::content_type, "application/json");
              resp.body() = respBody.dump();
              resp.prepare_payload();
            }
            co_return;
          }));

  // Start server thread
  sim.thr = std::thread([rawSvr]() { rawSvr->start(); });

  for (int i = 0; i < 100; ++i) {
    sim.port = rawSvr->port();
    if (sim.port != 0)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  return sim;
}

// ===========================================================================
// Benchmark result helpers
// ===========================================================================
namespace detail {

inline std::shared_ptr<agentxx::agent::AgentConfig>
makeAgentConfig(const std::string &baseUrl, const std::string &apiKey,
                const std::string &modelName,
                const std::string &systemPrompt) {
  auto cfg = std::make_shared<agentxx::agent::AgentConfig>();
  cfg->modelOpenAIBaseUrl = baseUrl;
  cfg->modelOpenAIApiKey = apiKey;
  cfg->modelOpenAIModelName = modelName;
  cfg->prompt.systemPrompt = systemPrompt;
  return cfg;
}

inline void reportBenchResult(const std::string &name,
                              std::vector<double> &durations) {
  if (durations.empty()) {
    std::cout << "  [ERROR] No successful iterations for [" << name << "]"
              << std::endl;
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
  r.name = name;
  r.iterations = durations.size();
  r.total_ns = total_ns;
  r.mean_ns = mean_ns;
  r.min_ns = min_ns;
  r.max_ns = max_ns;
  r.stddev_ns = stddev_ns;
  r.median_ns = median_ns;
  BenchReporter::instance().addResult(r);
  printResult(r);
}

} // namespace detail

struct DeepAgentBenchConfig {
  std::string openAIBaseUrl;
  std::string openAIApiKey;
  std::string openAIModelName;
  std::string systemPrompt;
  std::string userInput;
  size_t iterations = 5;
};

inline void
benchDeepAgentRunConversationTurnAsync(const DeepAgentBenchConfig &cfg) {
  std::cout << "\n=== DeepAgent::runConversationTurnAsync Benchmarks ==="
            << std::endl;

  std::string baseUrl = cfg.openAIBaseUrl;
  std::string apiKey = cfg.openAIApiKey;
  std::string modelName = cfg.openAIModelName;
  LlmSimServer simGuard;

  if (baseUrl.empty()) {
    simGuard = startLlmSimServer();
    baseUrl = "http://127.0.0.1:" + std::to_string(simGuard.port);
    apiKey = "EMPTY";
    modelName = "bench-sim";
    std::cout << "  [INFO] Using local LLM simulator on 127.0.0.1:"
              << simGuard.port << std::endl;
  }

  auto agentConfig = std::make_shared<agentxx::agent::AgentConfig>();
  agentConfig->modelOpenAIBaseUrl = baseUrl;
  agentConfig->modelOpenAIApiKey = apiKey;
  agentConfig->modelOpenAIModelName = modelName;
  agentConfig->prompt.systemPrompt = cfg.systemPrompt;

  std::vector<double> durations;
  durations.reserve(cfg.iterations);

  for (size_t i = 0; i < cfg.iterations; ++i) {
    asio::io_context ioCtx;
    agentxx::agent::DeepAgent agent(agentConfig);

    asio::co_spawn(
        ioCtx,
        [&]() -> asio::awaitable<void> {
          co_await agent.init();

          auto start = std::chrono::high_resolution_clock::now();

          neograph::json messages = neograph::json::array();
          auto turnResult = co_await agent.runConversationTurnAsync(
              "bench_session_" + std::to_string(i), cfg.userInput, true,
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
  BenchReporter::instance().addResult(r);
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
    // init() does not make HTTP calls, a dummy URL suffices
    config.openAIBaseUrl = "http://127.0.0.1:1";
    config.openAIApiKey = "EMPTY";
    config.openAIModelName = "bench-sim";
    std::cout << "  [INFO] Using local LLM simulator (init only)" << std::endl;
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
  BenchReporter::instance().addResult(r);
  printResult(r);
}

// -----------------------------------------------------------------------
// benchmark: deepagent runSingleInputAsync (simple completion, no tool loop)
// Measures the overhead of a single LLM call through the agent framework.
// -----------------------------------------------------------------------
inline void benchDeepAgentSimpleCompletion(const DeepAgentBenchConfig &cfg) {
  std::cout << "\n=== DeepAgent::runSingleInputAsync [simple completion] ==="
            << std::endl;

  std::string baseUrl = cfg.openAIBaseUrl;
  std::string apiKey = cfg.openAIApiKey;
  std::string modelName = cfg.openAIModelName;
  LlmSimServer simGuard;

  if (baseUrl.empty()) {
    simGuard = startLlmSimServer();
    baseUrl = "http://127.0.0.1:" + std::to_string(simGuard.port);
    apiKey = "EMPTY";
    modelName = "bench-sim";
    std::cout << "  [INFO] Using local LLM simulator on 127.0.0.1:"
              << simGuard.port << std::endl;
  }

  auto agentConfig = detail::makeAgentConfig(baseUrl, apiKey, modelName,
                                              cfg.systemPrompt);
  std::vector<double> durations;
  durations.reserve(cfg.iterations);

  for (size_t i = 0; i < cfg.iterations; ++i) {
    asio::io_context ioCtx;
    agentxx::agent::DeepAgent agent(agentConfig);

    asio::co_spawn(
        ioCtx,
        [&]() -> asio::awaitable<void> {
          co_await agent.init();
          auto start = std::chrono::high_resolution_clock::now();
          auto result = co_await agent.runSingleInputAsync(
              "bench_simple_" + std::to_string(i), cfg.userInput,
              cfg.systemPrompt);
          auto end = std::chrono::high_resolution_clock::now();
          double ns =
              std::chrono::duration<double, std::nano>(end - start).count();
          durations.push_back(ns);
          std::cout << "    [INFO] Iteration " << i << " completed, "
                    << result.size() << " chars" << std::endl;
        },
        asio::detached);

    ioCtx.run();
  }

  detail::reportBenchResult("DeepAgent::runSingleInputAsync [completion]", durations);
}

// -----------------------------------------------------------------------
// benchmark: deepagent multi-turn conversation
// Runs multiple turns sequentially, passing messages between each turn.
// -----------------------------------------------------------------------
inline void benchDeepAgentMultiTurn(const DeepAgentBenchConfig &cfg) {
  std::cout << "\n=== DeepAgent::runConversationTurnAsync [multi-turn] ==="
            << std::endl;

  std::string baseUrl = cfg.openAIBaseUrl;
  std::string apiKey = cfg.openAIApiKey;
  std::string modelName = cfg.openAIModelName;
  LlmSimServer simGuard;

  if (baseUrl.empty()) {
    simGuard = startLlmSimServer();
    baseUrl = "http://127.0.0.1:" + std::to_string(simGuard.port);
    apiKey = "EMPTY";
    modelName = "bench-sim";
    std::cout << "  [INFO] Using local LLM simulator on 127.0.0.1:"
              << simGuard.port << std::endl;
  }

  auto agentConfig = detail::makeAgentConfig(baseUrl, apiKey, modelName,
                                              cfg.systemPrompt);

  constexpr size_t turnsPerIteration = 3;
  std::vector<double> durations;
  durations.reserve(cfg.iterations);

  for (size_t i = 0; i < cfg.iterations; ++i) {
    asio::io_context ioCtx;
    agentxx::agent::DeepAgent agent(agentConfig);

    asio::co_spawn(
        ioCtx,
        [&]() -> asio::awaitable<void> {
          co_await agent.init();

          neograph::json messages = neograph::json::array();
          auto start = std::chrono::high_resolution_clock::now();

          for (size_t turn = 0; turn < turnsPerIteration; ++turn) {
            auto turnResult = co_await agent.runConversationTurnAsync(
                "bench_multi_" + std::to_string(i), cfg.userInput, true,
                std::move(messages),
                [](const neograph::graph::GraphEvent &) {});
            if (turnResult.hasError) {
              std::cerr << "    [ERROR] Turn " << turn << " of iteration " << i
                        << " failed: " << turnResult.errorMessage << std::endl;
              co_return;
            }
            messages = std::move(turnResult.messages);
          }

          auto end = std::chrono::high_resolution_clock::now();
          double ns =
              std::chrono::duration<double, std::nano>(end - start).count();
          durations.push_back(ns);

          std::cout << "    [INFO] Iteration " << i << " completed ("
                    << turnsPerIteration << " turns, "
                    << messages.size() << " total messages)" << std::endl;
        },
        asio::detached);

    ioCtx.run();
  }

  detail::reportBenchResult("DeepAgent::runConversationTurnAsync [multi-turn x"
                                + std::to_string(turnsPerIteration) + "]",
                            durations);
}

// -----------------------------------------------------------------------
// benchmark: deepagent with large conversation history
// Prepares a long message history then runs one final turn.
// -----------------------------------------------------------------------
inline void benchDeepAgentLargeHistory(const DeepAgentBenchConfig &cfg) {
  std::cout << "\n=== DeepAgent::runConversationTurnAsync [large history] ==="
            << std::endl;

  std::string baseUrl = cfg.openAIBaseUrl;
  std::string apiKey = cfg.openAIApiKey;
  std::string modelName = cfg.openAIModelName;
  LlmSimServer simGuard;

  if (baseUrl.empty()) {
    simGuard = startLlmSimServer();
    baseUrl = "http://127.0.0.1:" + std::to_string(simGuard.port);
    apiKey = "EMPTY";
    modelName = "bench-sim";
    std::cout << "  [INFO] Using local LLM simulator on 127.0.0.1:"
              << simGuard.port << std::endl;
  }

  auto agentConfig = detail::makeAgentConfig(baseUrl, apiKey, modelName,
                                              cfg.systemPrompt);

  constexpr size_t historyMessages = 20; // 10 turns back-and-forth
  std::vector<double> durations;
  durations.reserve(cfg.iterations);

  for (size_t i = 0; i < cfg.iterations; ++i) {
    asio::io_context ioCtx;
    agentxx::agent::DeepAgent agent(agentConfig);

    asio::co_spawn(
        ioCtx,
        [&]() -> asio::awaitable<void> {
          co_await agent.init();

          // Build a large message history
          neograph::json messages = neograph::json::array();
          for (size_t h = 0; h < historyMessages; ++h) {
            messages.push_back(neograph::json{
                {"role", (h % 2 == 0) ? "user" : "assistant"},
                {"content",
                 "This is a historical message number " + std::to_string(h) +
                     ". It contains some text to simulate a real "
                     "conversation history for benchmarking purposes."},
            });
          }

          auto start = std::chrono::high_resolution_clock::now();
          auto turnResult = co_await agent.runConversationTurnAsync(
              "bench_history_" + std::to_string(i), cfg.userInput, true,
              std::move(messages),
              [](const neograph::graph::GraphEvent &) {});
          auto end = std::chrono::high_resolution_clock::now();

          double ns =
              std::chrono::duration<double, std::nano>(end - start).count();

          if (turnResult.hasError) {
            std::cerr << "    [ERROR] Iteration " << i
                      << " failed: " << turnResult.errorMessage << std::endl;
          } else {
            durations.push_back(ns);
            std::cout << "    [INFO] Iteration " << i << " completed ("
                      << historyMessages << " history msgs + new turn)"
                      << std::endl;
          }
        },
        asio::detached);

    ioCtx.run();
  }

  detail::reportBenchResult(
      "DeepAgent::runConversationTurnAsync [history="
          + std::to_string(historyMessages) + "]",
      durations);
}

// -----------------------------------------------------------------------
// benchmark: agent init warm (re-init with same config)
// Measures whether repeated init benefits from any internal caching.
// -----------------------------------------------------------------------
inline void benchDeepAgentInitWarm() {
  std::cout << "\n=== DeepAgent::init [warm — same config] ===" << std::endl;

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
    config.openAIBaseUrl = "http://127.0.0.1:1";
    config.openAIApiKey = "EMPTY";
    config.openAIModelName = "bench-sim";
    std::cout << "  [INFO] Using local LLM simulator (init only)" << std::endl;
  }

  auto agentConfig = detail::makeAgentConfig(
      config.openAIBaseUrl, config.openAIApiKey, config.openAIModelName, "");

  // One cold init to warm up caches
  {
    asio::io_context ioCtx;
    agentxx::agent::DeepAgent agent(agentConfig);
    asio::co_spawn(ioCtx,
                   [&]() -> asio::awaitable<void> {
                     co_await agent.init();
                   },
                   asio::detached);
    ioCtx.run();
  }

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

  detail::reportBenchResult("DeepAgent::init [warm re-init]", durations);
}
} // namespace bench
} // namespace agentxx