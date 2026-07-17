#pragma once

// https://agentclientprotocol.com/protocol/schema.md

#include "agentxx/middlewares/middleware.h"
#include "agentxx/nodes/agentcall.h"
#include "agentxx/nodes/modelcall.h"
#include "agentxx/nodes/toolcall.h"
#include "neograph/acp/server.h"
#include "neograph/neograph.h"

#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace agentxx {
namespace server {

std::shared_ptr<neograph::graph::GraphEngine> build_engine() {
  neograph::json def = {
      {"name", "uppercase-acp-agent"},
      {
          "channels",
          {
              {"messages", {{"reducer", "append"}}},
              {
                  agentxx::middleware::MiddlewareContext::
                      channel_savedGraphData,
                  {{"reducer", "overwrite"}},
              },
          },
      },
      {
          "nodes",
          {
              {
                  "agent_start",
                  {{
                      "type",
                      agentxx::nodes::AgentStartCallWrapNode::defNodeType,
                  }},
              },
              {
                  "agent_end",
                  {{
                      "type",
                      agentxx::nodes::MiddlewareWrapAgentEndCallNode::
                          defNodeType,
                  }},
              },
              {
                  "llm",
                  {{
                      "type",
                      agentxx::nodes::ModelCallWrapNode::defNodeType,
                  }},
              },
          },
      },
      {
          "edges",
          neograph::json::array({
              {{"from", "__start__"}, {"to", "agent_start"}},
              {{"from", "agent_start"}, {"to", "agent_end"}},
              {{"from", "llm"}, {"to", "agent_end"}},
              {{"from", "agent_end"}, {"to", "__end__"}},
          }),
      },
  };
  neograph::graph::NodeContext ctx;
  auto engine = neograph::graph::GraphEngine::compile(def, ctx);
  return std::shared_ptr<neograph::graph::GraphEngine>(std::move(engine));
}

class AcpServer {
public:
  inline static int run_test(neograph::acp::ACPServer &server) {
    std::cerr << "[*] self-test — driving the server in-process\n";

    // session/prompt is async-dispatched: we have to wait on the sink
    // for the response envelope to arrive instead of relying on
    // handle_message's return value.
    std::mutex mu;
    std::condition_variable cv;
    bool saw_response = false;
    int notif_count = 0;
    server.set_notification_sink([&](const neograph::json &env) {
      std::cerr << "    [notif] " << env.dump() << "\n";
      std::lock_guard lk(mu);
      if (env.value("method", std::string()) == "session/update") {
        ++notif_count;
      } else if (!env.contains("method") && env.contains("id") &&
                 env["id"].is_number_integer() && env["id"].get<int>() == 3) {
        saw_response = true;
        cv.notify_all();
      }
    });

    {
      neograph::json env;
      env["jsonrpc"] = "2.0";
      env["id"] = 1;
      env["method"] = "initialize";
      env["params"] = {{"protocolVersion", 1},
                       {"clientCapabilities", neograph::json::object()}};
      auto resp = server.handle_message(env);
      std::cerr << "    initialize -> " << resp.dump() << "\n";
    }

    std::string sid;
    {
      neograph::json env;
      env["jsonrpc"] = "2.0";
      env["id"] = 2;
      env["method"] = "session/new";
      env["params"] = {{"cwd", "/tmp"},
                       {"mcpServers", neograph::json::array()}};
      auto resp = server.handle_message(env);
      std::cerr << "    session/new -> " << resp.dump() << "\n";
      sid = resp["result"].value("sessionId", std::string());
    }

    {
      neograph::json env;
      env["jsonrpc"] = "2.0";
      env["id"] = 3;
      env["method"] = "session/prompt";
      neograph::json prompt = neograph::json::array();
      prompt.push_back({{"type", "text"}, {"text", "hello acp"}});
      env["params"] = {{"sessionId", sid}, {"prompt", std::move(prompt)}};
      auto immediate = server.handle_message(env);
      std::cerr << "    session/prompt dispatched (async) -> "
                << (immediate.is_null() ? std::string("null")
                                        : immediate.dump())
                << "\n";
    }

    {
      std::unique_lock lk(mu);
      if (!cv.wait_for(lk, std::chrono::seconds(5),
                       [&] { return saw_response; })) {
        std::cerr << "[!] timed out waiting for session/prompt response\n";
        return 1;
      }
    }
    if (notif_count == 0) {
      std::cout << "[!] expected at least one session/update notification\n";
      return 1;
    }
    std::cout << "[*] OK — " << notif_count << " update(s) emitted, "
              << "response received\n";
    return 0;
  }

  inline static int
  run_server(std::shared_ptr<neograph::graph::GraphEngine> engine,
             bool test = false) {
    neograph::json info{
        {"name", "agentxx-server"},
        {"version", "0.1.0"},
    };

    auto server = neograph::acp::ACPServer{build_engine(), info};
    server.capabilities().session.close = false; // baseline-only
    server.capabilities().prompt.image = false;

    if (test) {
      return run_test(server);
    }

    std::cerr << "[*] ACP agent reading NDJSON on stdin (Ctrl-D to exit)\n";
    server.run(); // blocks on std::cin / std::cout
    return 0;
  }
};

} // namespace server
} // namespace agentxx
