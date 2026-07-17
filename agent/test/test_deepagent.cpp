#include "test_deepagent.h"

namespace agentxx {
namespace test {

int g_da_passed = 0;
int g_da_failed = 0;

std::string g_da_sim_response_content =
    "Hello! I am a simulated LLM response for testing.";
int g_da_sim_prompt_tokens = 100;
int g_da_sim_completion_tokens = 50;
neograph::json g_da_sim_tool_calls = neograph::json::array();

DaSimServer::DaSimServer(DaSimServer &&o) noexcept
    : svr(std::move(o.svr)), thr(std::move(o.thr)), port(o.port) {
  o.port = 0;
}

DaSimServer &DaSimServer::operator=(DaSimServer &&o) noexcept {
  if (this != &o) {
    stop();
    svr = std::move(o.svr);
    thr = std::move(o.thr);
    port = o.port;
    o.port = 0;
  }
  return *this;
}

DaSimServer::~DaSimServer() { stop(); }

void DaSimServer::stop() {
  if (svr)
    svr->stop();
  if (thr.joinable())
    thr.join();
  svr.reset();
  port = 0;
}

DaSimServer startDaSimServer() {
  DaSimServer sim;

  agentxx::util::HttpServer::Config cfg;
  cfg.address = "127.0.0.1";
  cfg.port = 0;
  cfg.ioThreads = 1;
  cfg.accessLogEnabled = false;
  cfg.maxConnections = 128;
  cfg.maxRequestBody = 1024 * 1024;

  sim.svr = std::make_unique<agentxx::util::HttpServer>(cfg);
  auto *rawSvr = sim.svr.get();

  rawSvr->router().add(
      "/v1/chat/completions", 2,
      std::make_shared<agentxx::util::HttpServer::Handler>(
          [](agentxx::util::HttpServer::Request &req,
             agentxx::util::HttpServer::Response &resp,
             const std::string &) -> asio::awaitable<void> {
            namespace http = boost::beast::http;

            auto j = neograph::json::parse(req.body());
            bool stream = j.value("stream", false);
            bool hasToolCalls = !g_da_sim_tool_calls.empty();

            if (stream) {
              std::string sseBody;
              auto append = [&](const neograph::json &delta,
                                const std::string &finishReason) {
                auto ev = neograph::json::object();
                ev["id"] = "chatcmpl-test-sim";
                ev["object"] = "chat.completion.chunk";
                ev["created"] = 1234567890;
                ev["model"] = "test-sim";

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
                d["tool_calls"] = g_da_sim_tool_calls;
                append(d, "");
                append(neograph::json::object(), "tool_calls");
              } else {
                auto &content = g_da_sim_response_content;
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
                msg["tool_calls"] = g_da_sim_tool_calls;
              } else {
                msg["content"] = g_da_sim_response_content;
              }

              auto choice = neograph::json::object();
              choice["index"] = 0;
              choice["message"] = msg;
              choice["finish_reason"] = hasToolCalls ? "tool_calls" : "stop";

              auto usage = neograph::json::object();
              usage["prompt_tokens"] = g_da_sim_prompt_tokens;
              usage["completion_tokens"] = g_da_sim_completion_tokens;
              usage["total_tokens"] =
                  g_da_sim_prompt_tokens + g_da_sim_completion_tokens;

              auto respBody = neograph::json::object();
              respBody["id"] = "chatcmpl-test-sim";
              respBody["object"] = "chat.completion";
              respBody["created"] = 1234567890;
              respBody["model"] = "test-sim";
              respBody["choices"] = neograph::json::array({choice});
              respBody["usage"] = usage;

              resp.result(http::status::ok);
              resp.set(http::field::content_type, "application/json");
              resp.body() = respBody.dump();
              resp.prepare_payload();
            }

            g_da_sim_tool_calls = neograph::json::array();
            co_return;
          }));

  sim.thr = std::thread([rawSvr]() { rawSvr->start(); });

  for (int i = 0; i < 100; ++i) {
    sim.port = rawSvr->port();
    if (sim.port != 0)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  return sim;
}

asio::awaitable<void> test_deepagent_init() {
  auto sim = startDaSimServer();
  auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

  auto cfg = std::make_shared<agentxx::agent::AgentConfig>();
  cfg->modelOpenAIBaseUrl = baseUrl;
  cfg->modelOpenAIApiKey = "EMPTY";
  cfg->modelOpenAIModelName = "test-sim";

  agentxx::agent::DeepAgent agent(cfg);
  co_await agent.init();

  XX_TEST_EXPECT_TRUE(agent.engine != nullptr);
  XX_TEST_EXPECT_TRUE(agent.agentContext != nullptr);

  co_return;
}

asio::awaitable<void> test_deepagent_single_input() {
  auto sim = startDaSimServer();
  auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

  auto cfg = std::make_shared<agentxx::agent::AgentConfig>();
  cfg->modelOpenAIBaseUrl = baseUrl;
  cfg->modelOpenAIApiKey = "EMPTY";
  cfg->modelOpenAIModelName = "test-sim";
  cfg->prompt.systemPrompt = "You are a helpful assistant.";

  g_da_sim_response_content = "This is the test response content.";
  g_da_sim_tool_calls = neograph::json::array();

  agentxx::agent::DeepAgent agent(cfg);
  co_await agent.init();

  auto result = co_await agent.runSingleInputAsync("test_thread", "Hello");

  XX_TEST_EXPECT_FALSE(result.empty());
  XX_TEST_EXPECT_TRUE(result.find("test response") != std::string::npos);

  co_return;
}

asio::awaitable<void> test_deepagent_conversation_turn() {
  auto sim = startDaSimServer();
  auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

  auto cfg = std::make_shared<agentxx::agent::AgentConfig>();
  cfg->modelOpenAIBaseUrl = baseUrl;
  cfg->modelOpenAIApiKey = "EMPTY";
  cfg->modelOpenAIModelName = "test-sim";
  cfg->prompt.systemPrompt = "You are a helpful assistant.";

  g_da_sim_response_content = "Hello from the simulated LLM!";
  g_da_sim_tool_calls = neograph::json::array();

  agentxx::agent::DeepAgent agent(cfg);
  co_await agent.init();

  neograph::json messages = neograph::json::array();
  auto result = co_await agent.runConversationTurnAsync(
      "conv_test", "What is the weather?", true, std::move(messages),
      [](const neograph::graph::GraphEvent &) {});

  XX_TEST_EXPECT_FALSE(result.hasError);
  XX_TEST_EXPECT_FALSE(result.interrupted);
  XX_TEST_EXPECT_TRUE(result.messages.is_array());
  XX_TEST_EXPECT_TRUE(result.messages.size() > 0);

  co_return;
}

asio::awaitable<void> test_deepagent_tool_calls() {
  auto sim = startDaSimServer();
  auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

  auto cfg = std::make_shared<agentxx::agent::AgentConfig>();
  cfg->modelOpenAIBaseUrl = baseUrl;
  cfg->modelOpenAIApiKey = "EMPTY";
  cfg->modelOpenAIModelName = "test-sim";
  cfg->prompt.systemPrompt = "You are a helpful assistant.";

  g_da_sim_response_content = "";
  g_da_sim_tool_calls = neograph::json::array({
      neograph::json{
          {"index", 0},
          {"id", "call_test_1"},
          {"type", "function"},
          {"function",
           neograph::json{
               {"name", "filesystem_list"},
               {"arguments", "{}"},
           }},
      },
  });

  agentxx::agent::DeepAgent agent(cfg);
  co_await agent.init();

  neograph::json messages = neograph::json::array();
  auto result = co_await agent.runConversationTurnAsync(
      "tool_test", "List files", true, std::move(messages),
      [](const neograph::graph::GraphEvent &) {});

  XX_TEST_EXPECT_FALSE(result.hasError);

  co_return;
}

asio::awaitable<void> test_deepagent_multi_turn() {
  auto sim = startDaSimServer();
  auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

  auto cfg = std::make_shared<agentxx::agent::AgentConfig>();
  cfg->modelOpenAIBaseUrl = baseUrl;
  cfg->modelOpenAIApiKey = "EMPTY";
  cfg->modelOpenAIModelName = "test-sim";
  cfg->prompt.systemPrompt = "You are a helpful assistant.";

  g_da_sim_response_content = "Response for turn ";
  g_da_sim_tool_calls = neograph::json::array();

  agentxx::agent::DeepAgent agent(cfg);
  co_await agent.init();

  neograph::json messages = neograph::json::array();

  for (int turn = 0; turn < 3; ++turn) {
    auto input = "Turn " + std::to_string(turn) + " input";
    auto result = co_await agent.runConversationTurnAsync(
        "multi_turn_test", input, turn == 0, std::move(messages),
        [](const neograph::graph::GraphEvent &) {});

    XX_TEST_EXPECT_FALSE(result.hasError);
    XX_TEST_EXPECT_FALSE(result.interrupted);
    XX_TEST_EXPECT_TRUE(result.messages.is_array());
    XX_TEST_EXPECT_TRUE(result.messages.size() > 0);

    messages = std::move(result.messages);
  }

  co_return;
}

asio::awaitable<void> test_deepagent_large_history() {
  auto sim = startDaSimServer();
  auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

  auto cfg = std::make_shared<agentxx::agent::AgentConfig>();
  cfg->modelOpenAIBaseUrl = baseUrl;
  cfg->modelOpenAIApiKey = "EMPTY";
  cfg->modelOpenAIModelName = "test-sim";
  cfg->prompt.systemPrompt = "You are a helpful assistant.";

  g_da_sim_response_content = "Final response after long history.";
  g_da_sim_tool_calls = neograph::json::array();

  agentxx::agent::DeepAgent agent(cfg);
  co_await agent.init();

  neograph::json messages = neograph::json::array();
  for (int h = 0; h < 20; ++h) {
    messages.push_back(neograph::json{
        {"role", (h % 2 == 0) ? "user" : "assistant"},
        {"content", "Historical message " + std::to_string(h)},
    });
  }

  auto result = co_await agent.runConversationTurnAsync(
      "history_test", "Final question", true, std::move(messages),
      [](const neograph::graph::GraphEvent &) {});

  XX_TEST_EXPECT_FALSE(result.hasError);
  XX_TEST_EXPECT_TRUE(result.messages.is_array());

  co_return;
}

asio::awaitable<void> test_deepagent_nonstream() {
  auto sim = startDaSimServer();
  auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

  auto cfg = std::make_shared<agentxx::agent::AgentConfig>();
  cfg->modelOpenAIBaseUrl = baseUrl;
  cfg->modelOpenAIApiKey = "EMPTY";
  cfg->modelOpenAIModelName = "test-sim";

  g_da_sim_response_content = "Non-stream test response.";
  g_da_sim_tool_calls = neograph::json::array();

  agentxx::agent::DeepAgent agent(cfg);
  co_await agent.init();

  std::vector<neograph::ChatMessage> msgs;
  msgs.push_back(neograph::ChatMessage{
      .role = "system",
      .content = "You are helpful.",
  });
  msgs.push_back(neograph::ChatMessage{
      .role = "user",
      .content = "Hello",
  });

  auto result = co_await agent.runNonStreamAsync("nonstream_test", msgs);

  XX_TEST_EXPECT_FALSE(result.empty());
  XX_TEST_EXPECT_TRUE(result.find("Non-stream") != std::string::npos);

  co_return;
}

asio::awaitable<TestResult> run_deepagent_tests() {
  g_da_passed = 0;
  g_da_failed = 0;

  try {
    co_await test_deepagent_init();
    co_await test_deepagent_single_input();
    co_await test_deepagent_conversation_turn();
    co_await test_deepagent_tool_calls();
    co_await test_deepagent_multi_turn();
    co_await test_deepagent_large_history();
    co_await test_deepagent_nonstream();
  } catch (const std::exception &e) {
    TEST_FAIL << "deepagent suite exception: " << e.what() << std::endl;
    g_da_failed++;
  }

  co_return TestResult{g_da_passed, g_da_failed};
}

} // namespace test
} // namespace agentxx
