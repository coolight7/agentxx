#include "test_a2a.h"

#include "agentxx/protocol/a2a_client.h"
#include "agentxx/protocol/a2a_server.h"
#include "agentxx/util/log.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "asio/use_awaitable.hpp"
#include <chrono>
#include <thread>

namespace agentxx {
namespace test {

int g_a2a_passed = 0;
int g_a2a_failed = 0;

using agentxx::server::A2aClient;
using agentxx::server::A2aServer;
using agentxx::server::A2aTaskState;
using agentxx::server::taskStateFromString;
using agentxx::server::taskStateToString;
using json = neograph::json;

// ---------------------------------------------------------------------------
// Unit tests: data model helpers
// ---------------------------------------------------------------------------

static void test_task_state_conversion() {
    XX_TEST_EXPECT_EQ(taskStateToString(A2aTaskState::Submitted), "TASK_STATE_SUBMITTED");
    XX_TEST_EXPECT_EQ(taskStateToString(A2aTaskState::Working), "TASK_STATE_WORKING");
    XX_TEST_EXPECT_EQ(taskStateToString(A2aTaskState::Completed), "TASK_STATE_COMPLETED");
    XX_TEST_EXPECT_EQ(taskStateToString(A2aTaskState::Failed), "TASK_STATE_FAILED");
    XX_TEST_EXPECT_EQ(taskStateToString(A2aTaskState::Canceled), "TASK_STATE_CANCELED");
    XX_TEST_EXPECT_EQ(taskStateToString(A2aTaskState::InputRequired), "TASK_STATE_INPUT_REQUIRED");
    XX_TEST_EXPECT_EQ(taskStateToString(A2aTaskState::Rejected), "TASK_STATE_REJECTED");
    XX_TEST_EXPECT_EQ(taskStateToString(A2aTaskState::AuthRequired), "TASK_STATE_AUTH_REQUIRED");
    XX_TEST_EXPECT_EQ(taskStateToString(A2aTaskState::Unspecified), "TASK_STATE_UNSPECIFIED");

    XX_TEST_EXPECT_TRUE(taskStateFromString("TASK_STATE_SUBMITTED") == A2aTaskState::Submitted);
    XX_TEST_EXPECT_TRUE(taskStateFromString("TASK_STATE_WORKING") == A2aTaskState::Working);
    XX_TEST_EXPECT_TRUE(taskStateFromString("TASK_STATE_COMPLETED") == A2aTaskState::Completed);
    XX_TEST_EXPECT_TRUE(taskStateFromString("TASK_STATE_FAILED") == A2aTaskState::Failed);
    XX_TEST_EXPECT_TRUE(taskStateFromString("TASK_STATE_CANCELED") == A2aTaskState::Canceled);
    XX_TEST_EXPECT_TRUE(
        taskStateFromString("TASK_STATE_INPUT_REQUIRED") == A2aTaskState::InputRequired
    );
    XX_TEST_EXPECT_TRUE(taskStateFromString("TASK_STATE_REJECTED") == A2aTaskState::Rejected);
    XX_TEST_EXPECT_TRUE(
        taskStateFromString("TASK_STATE_AUTH_REQUIRED") == A2aTaskState::AuthRequired
    );
    XX_TEST_EXPECT_TRUE(taskStateFromString("invalid") == A2aTaskState::Unspecified);
    XX_TEST_EXPECT_TRUE(taskStateFromString("") == A2aTaskState::Unspecified);
}

static void test_terminal_state() {
    using agentxx::server::isTerminalState;
    XX_TEST_EXPECT_TRUE(isTerminalState(A2aTaskState::Completed));
    XX_TEST_EXPECT_TRUE(isTerminalState(A2aTaskState::Failed));
    XX_TEST_EXPECT_TRUE(isTerminalState(A2aTaskState::Canceled));
    XX_TEST_EXPECT_TRUE(isTerminalState(A2aTaskState::Rejected));
    XX_TEST_EXPECT_FALSE(isTerminalState(A2aTaskState::Submitted));
    XX_TEST_EXPECT_FALSE(isTerminalState(A2aTaskState::Working));
    XX_TEST_EXPECT_FALSE(isTerminalState(A2aTaskState::InputRequired));
    XX_TEST_EXPECT_FALSE(isTerminalState(A2aTaskState::AuthRequired));
    XX_TEST_EXPECT_FALSE(isTerminalState(A2aTaskState::Unspecified));
}

static void test_json_rpc_helpers() {
    auto result = A2aServer::jsonRpcResult(
        json(1),
        json{
            {"key", "value"}
    }
    );
    XX_TEST_EXPECT_EQ(result["jsonrpc"].get<std::string>(), "2.0");
    XX_TEST_EXPECT_EQ(result["id"].get<int>(), 1);
    XX_TEST_EXPECT_TRUE(result.contains("result"));
    XX_TEST_EXPECT_EQ(result["result"]["key"].get<std::string>(), "value");

    auto err = A2aServer::jsonRpcError(json(2), -32601, "Method not found");
    XX_TEST_EXPECT_EQ(err["jsonrpc"].get<std::string>(), "2.0");
    XX_TEST_EXPECT_EQ(err["id"].get<int>(), 2);
    XX_TEST_EXPECT_TRUE(err.contains("error"));
    XX_TEST_EXPECT_EQ(err["error"]["code"].get<int>(), -32601);
    XX_TEST_EXPECT_EQ(err["error"]["message"].get<std::string>(), "Method not found");
}

static void test_make_text_part() {
    auto part = A2aServer::makeTextPart("hello");
    XX_TEST_EXPECT_TRUE(part.contains("text"));
    XX_TEST_EXPECT_EQ(part["text"].get<std::string>(), "hello");
}

static void test_make_message() {
    auto msg = A2aServer::makeMessage("ROLE_AGENT", "response text");
    XX_TEST_EXPECT_TRUE(msg.contains("messageId"));
    XX_TEST_EXPECT_EQ(msg["role"].get<std::string>(), "ROLE_AGENT");
    XX_TEST_EXPECT_TRUE(msg["parts"].is_array());
    XX_TEST_EXPECT_EQ(msg["parts"][0]["text"].get<std::string>(), "response text");
}

static void test_make_task() {
    auto task = A2aServer::makeTask("task-1", "ctx-1", A2aTaskState::Working);
    XX_TEST_EXPECT_EQ(task["id"].get<std::string>(), "task-1");
    XX_TEST_EXPECT_EQ(task["contextId"].get<std::string>(), "ctx-1");
    XX_TEST_EXPECT_TRUE(task.contains("status"));
    XX_TEST_EXPECT_EQ(task["status"]["state"].get<std::string>(), "TASK_STATE_WORKING");
    XX_TEST_EXPECT_TRUE(task["status"].contains("timestamp"));
}

static void test_extract_text_from_parts() {
    json parts = json::array({
        json{{"text", "hello"}},
        json{{"text", "world"}},
        json{{"data", json{{"key", "val"}}}},
    });
    auto text  = A2aServer::extractTextFromParts(parts);
    XX_TEST_EXPECT_EQ(text, "hello\nworld");

    json emptyParts = json::array();
    XX_TEST_EXPECT_EQ(A2aServer::extractTextFromParts(emptyParts), "");

    json nonArray = json("not array");
    XX_TEST_EXPECT_EQ(A2aServer::extractTextFromParts(nonArray), "");
}

static void test_generate_id() {
    auto id1 = A2aServer::generateId();
    auto id2 = A2aServer::generateId();
    XX_TEST_EXPECT_FALSE(id1.empty());
    XX_TEST_EXPECT_FALSE(id2.empty());
    XX_TEST_EXPECT_TRUE(id1 != id2);
}

static void test_current_timestamp() {
    auto ts = A2aServer::currentTimestamp();
    XX_TEST_EXPECT_FALSE(ts.empty());
    XX_TEST_EXPECT_TRUE(ts.size() >= 20);
    XX_TEST_EXPECT_TRUE(ts.back() == 'Z');
    XX_TEST_EXPECT_TRUE(ts.find('T') != std::string::npos);
}

static void test_error_helpers() {
    auto notFound = A2aServer::makeTaskNotFound(json(1), "task-xyz");
    XX_TEST_EXPECT_EQ(notFound["error"]["code"].get<int>(), -32001);
    XX_TEST_EXPECT_TRUE(
        notFound["error"]["message"].get<std::string>().find("task-xyz") != std::string::npos
    );

    auto unsupported = A2aServer::makeUnsupportedOperation(json(2), "not supported");
    XX_TEST_EXPECT_EQ(unsupported["error"]["code"].get<int>(), -32004);

    auto versionErr = A2aServer::makeVersionNotSupported(json(3), "0.5");
    XX_TEST_EXPECT_EQ(versionErr["error"]["code"].get<int>(), -32009);
    XX_TEST_EXPECT_TRUE(
        versionErr["error"]["message"].get<std::string>().find("0.5") != std::string::npos
    );
}

// ---------------------------------------------------------------------------
// Client helper tests
// ---------------------------------------------------------------------------

static void test_client_build_text_message() {
    auto msg = A2aClient::buildTextMessage("hello agent");
    XX_TEST_EXPECT_EQ(msg["role"].get<std::string>(), "ROLE_USER");
    XX_TEST_EXPECT_TRUE(msg["parts"].is_array());
    XX_TEST_EXPECT_EQ(msg["parts"][0]["text"].get<std::string>(), "hello agent");
    XX_TEST_EXPECT_FALSE(msg.contains("taskId"));
    XX_TEST_EXPECT_FALSE(msg.contains("contextId"));

    auto msgWithIds = A2aClient::buildTextMessage("hi", "task-1", "ctx-1");
    XX_TEST_EXPECT_EQ(msgWithIds["taskId"].get<std::string>(), "task-1");
    XX_TEST_EXPECT_EQ(msgWithIds["contextId"].get<std::string>(), "ctx-1");
}

static void test_client_extract_helpers() {
    json sendResult;
    sendResult["task"]["id"] = "task-123";
    XX_TEST_EXPECT_EQ(A2aClient::extractTaskId(sendResult), "task-123");

    json noTask = json::object();
    XX_TEST_EXPECT_EQ(A2aClient::extractTaskId(noTask), "");

    json task;
    task["status"]["state"] = "TASK_STATE_COMPLETED";
    XX_TEST_EXPECT_EQ(A2aClient::extractTaskState(task), "TASK_STATE_COMPLETED");

    json noStatus = json::object();
    XX_TEST_EXPECT_EQ(A2aClient::extractTaskState(noStatus), "TASK_STATE_UNSPECIFIED");

    json taskWithArtifacts;
    taskWithArtifacts["artifacts"] = json::array({
        json{{"artifactId", "a1"}, {"parts", json::array({json{{"text", "result text"}}})}},
    });
    XX_TEST_EXPECT_EQ(A2aClient::extractArtifactText(taskWithArtifacts), "result text");

    json noArtifacts = json::object();
    XX_TEST_EXPECT_EQ(A2aClient::extractArtifactText(noArtifacts), "");
}

// ---------------------------------------------------------------------------
// Integration test: A2A server + client over HTTP
// ---------------------------------------------------------------------------

static uint16_t startServerThread(A2aServer& server, std::thread& th) {
    th = std::thread([&server]() {
        server.start();
    });
    while (server.port() == 0 && !server.isStopped()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return server.port();
}

static asio::awaitable<void> test_a2a_server_integration() {
    A2aServer::Config cfg;
    cfg.httpConfig.address = "127.0.0.1";
    cfg.httpConfig.port    = 0;
    cfg.serverName         = "test-a2a-agent";
    cfg.description        = "Test A2A Agent";
    cfg.skills             = {
        {"echo", "Echo Skill", "Echoes input back", {"echo", "test"}, {"say hello"}},
    };

    A2aServer   server(nullptr, std::move(cfg));
    std::thread serverThread;
    auto        port = startServerThread(server, serverThread);
    XX_TEST_EXPECT_TRUE(port > 0);

    auto baseUrl = "http://127.0.0.1:" + std::to_string(port);

    A2aClient::Config clientCfg;
    clientCfg.baseUrl = baseUrl;
    A2aClient client(std::move(clientCfg));

    // --- Agent Card ---
    {
        auto cardResult = co_await client.fetchAgentCard();
        XX_TEST_EXPECT_TRUE(cardResult.has_value());
        if (cardResult.has_value()) {
            auto& card = cardResult.value();
            XX_TEST_EXPECT_EQ(card["name"].get<std::string>(), "test-a2a-agent");
            XX_TEST_EXPECT_EQ(card["description"].get<std::string>(), "Test A2A Agent");
            XX_TEST_EXPECT_TRUE(card.contains("supportedInterfaces"));
            XX_TEST_EXPECT_TRUE(card["supportedInterfaces"].is_array());
            XX_TEST_EXPECT_TRUE(card["supportedInterfaces"].size() > 0);
            XX_TEST_EXPECT_EQ(
                card["supportedInterfaces"][0]["protocolBinding"].get<std::string>(),
                "JSONRPC"
            );
            XX_TEST_EXPECT_EQ(
                card["supportedInterfaces"][0]["protocolVersion"].get<std::string>(),
                "1.0"
            );
            XX_TEST_EXPECT_TRUE(card.contains("capabilities"));
            XX_TEST_EXPECT_TRUE(card["capabilities"]["streaming"].get<bool>());
            XX_TEST_EXPECT_TRUE(card.contains("skills"));
            XX_TEST_EXPECT_TRUE(card["skills"].is_array());
            XX_TEST_EXPECT_EQ(card["skills"].size(), size_t(1));
            XX_TEST_EXPECT_EQ(card["skills"][0]["id"].get<std::string>(), "echo");
            XX_TEST_EXPECT_TRUE(card.contains("defaultInputModes"));
            XX_TEST_EXPECT_TRUE(card.contains("defaultOutputModes"));
        }
    }

    // --- JSON-RPC: method not found ---
    {
        auto result = co_await client.rpcCall("NonExistentMethod", json::object());
        XX_TEST_EXPECT_FALSE(result.has_value());
        if (!result.has_value()) {
            XX_TEST_EXPECT_TRUE(result.error().find("-32601") != std::string::npos);
        }
    }

    // --- JSON-RPC: invalid request (no message) ---
    {
        auto result = co_await client.rpcCall("SendMessage", json::object());
        XX_TEST_EXPECT_FALSE(result.has_value());
        if (!result.has_value()) {
            XX_TEST_EXPECT_TRUE(result.error().find("-32602") != std::string::npos);
        }
    }

    // --- GetTask: not found ---
    {
        auto result = co_await client.getTask("nonexistent-task");
        XX_TEST_EXPECT_FALSE(result.has_value());
        if (!result.has_value()) {
            XX_TEST_EXPECT_TRUE(result.error().find("-32001") != std::string::npos);
        }
    }

    // --- CancelTask: not found ---
    {
        auto result = co_await client.cancelTask("nonexistent-task");
        XX_TEST_EXPECT_FALSE(result.has_value());
        if (!result.has_value()) {
            XX_TEST_EXPECT_TRUE(result.error().find("-32001") != std::string::npos);
        }
    }

    // --- ListTasks: empty ---
    {
        auto result = co_await client.listTasks();
        XX_TEST_EXPECT_TRUE(result.has_value());
        if (result.has_value()) {
            auto& r = result.value();
            XX_TEST_EXPECT_TRUE(r.contains("tasks"));
            XX_TEST_EXPECT_TRUE(r["tasks"].is_array());
            XX_TEST_EXPECT_EQ(r["totalSize"].get<int>(), 0);
        }
    }

    // --- SendMessage: missing parts ---
    {
        json params;
        params["message"] = json{
            {"role", "ROLE_USER"}
        };
        auto result = co_await client.rpcCall("SendMessage", std::move(params));
        XX_TEST_EXPECT_FALSE(result.has_value());
    }

    // --- SendMessage: wrong role ---
    {
        json params;
        params["message"] = json{
            {"role",  "ROLE_AGENT"                          },
            {"parts", json::array({json{{"text", "hello"}}})},
        };
        auto result = co_await client.rpcCall("SendMessage", std::move(params));
        XX_TEST_EXPECT_FALSE(result.has_value());
    }

    // --- Version negotiation: unsupported version ---
    {
        auto            url = baseUrl + "/a2a";
        util::HeaderMap headers;
        headers.set("A2A-Version", "99.0");
        json request;
        request["jsonrpc"] = "2.0";
        request["id"]      = 1;
        request["method"]  = "GetTask";
        request["params"]  = json{
             {"id", "x"}
        };

        auto resp = co_await util::HttpClient::postAsync(
            url,
            request.dump(),
            "application/json",
            headers,
            util::RequestConfig{}
        );
        XX_TEST_EXPECT_TRUE(resp.has_value());
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp->status, 400);
            auto body = resp->bodyJson();
            XX_TEST_EXPECT_TRUE(body.has_value());
            if (body.has_value()) {
                XX_TEST_EXPECT_TRUE(body->contains("error"));
                XX_TEST_EXPECT_EQ((*body)["error"]["code"].get<int>(), -32009);
            }
        }
    }

    // --- Version negotiation: valid version 0.3 ---
    {
        auto            url = baseUrl + "/a2a";
        util::HeaderMap headers;
        headers.set("A2A-Version", "0.3");
        json request;
        request["jsonrpc"] = "2.0";
        request["id"]      = 1;
        request["method"]  = "ListTasks";
        request["params"]  = json::object();

        auto resp = co_await util::HttpClient::postAsync(
            url,
            request.dump(),
            "application/json",
            headers,
            util::RequestConfig{}
        );
        XX_TEST_EXPECT_TRUE(resp.has_value());
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp->status, 200);
            auto body = resp->bodyJson();
            XX_TEST_EXPECT_TRUE(body.has_value());
            if (body.has_value()) {
                XX_TEST_EXPECT_TRUE(body->contains("result"));
            }
        }
    }

    // --- Invalid JSON body ---
    {
        auto url  = baseUrl + "/a2a";
        auto resp = co_await util::HttpClient::postAsync(
            url,
            std::string_view("not valid json{{{"),
            "application/json",
            util::HeaderMap{},
            util::RequestConfig{}
        );
        XX_TEST_EXPECT_TRUE(resp.has_value());
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp->status, 200);
            auto body = resp->bodyJson();
            XX_TEST_EXPECT_TRUE(body.has_value());
            if (body.has_value()) {
                XX_TEST_EXPECT_TRUE(body->contains("error"));
                XX_TEST_EXPECT_EQ((*body)["error"]["code"].get<int>(), -32700);
            }
        }
    }

    // --- Invalid jsonrpc version ---
    {
        auto url = baseUrl + "/a2a";
        json request;
        request["jsonrpc"] = "1.0";
        request["id"]      = 1;
        request["method"]  = "ListTasks";
        request["params"]  = json::object();

        auto resp = co_await util::HttpClient::postAsync(
            url,
            request.dump(),
            "application/json",
            util::HeaderMap{},
            util::RequestConfig{}
        );
        XX_TEST_EXPECT_TRUE(resp.has_value());
        if (resp.has_value()) {
            XX_TEST_EXPECT_EQ(resp->status, 200);
            auto body = resp->bodyJson();
            XX_TEST_EXPECT_TRUE(body.has_value());
            if (body.has_value()) {
                XX_TEST_EXPECT_TRUE(body->contains("error"));
                XX_TEST_EXPECT_EQ((*body)["error"]["code"].get<int>(), -32600);
            }
        }
    }

    // --- GetTask: missing id param ---
    {
        auto result = co_await client.rpcCall("GetTask", json::object());
        XX_TEST_EXPECT_FALSE(result.has_value());
        if (!result.has_value()) {
            XX_TEST_EXPECT_TRUE(result.error().find("-32602") != std::string::npos);
        }
    }

    // --- CancelTask: missing id param ---
    {
        auto result = co_await client.rpcCall("CancelTask", json::object());
        XX_TEST_EXPECT_FALSE(result.has_value());
        if (!result.has_value()) {
            XX_TEST_EXPECT_TRUE(result.error().find("-32602") != std::string::npos);
        }
    }

    server.stop();
    serverThread.join();
    co_return;
}

// ---------------------------------------------------------------------------
// Test: SendMessage creates task (without DeepAgent, task stays in Working)
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_a2a_send_message_creates_task() {
    A2aServer::Config cfg;
    cfg.httpConfig.address = "127.0.0.1";
    cfg.httpConfig.port    = 0;
    cfg.serverName         = "task-test-agent";

    A2aServer   server(nullptr, std::move(cfg));
    std::thread serverThread;
    auto        port = startServerThread(server, serverThread);

    auto              baseUrl = "http://127.0.0.1:" + std::to_string(port);
    A2aClient::Config clientCfg;
    clientCfg.baseUrl = baseUrl;
    A2aClient client(std::move(clientCfg));

    // SendMessage should create a task (will fail in execution since no DeepAgent,
    // but the task record should exist)
    auto sendResult = co_await client.sendMessage("hello world");
    XX_TEST_EXPECT_TRUE(sendResult.has_value());
    std::string taskId;
    if (sendResult.has_value()) {
        taskId = A2aClient::extractTaskId(sendResult.value());
        XX_TEST_EXPECT_FALSE(taskId.empty());
        auto state = A2aClient::extractTaskState(sendResult.value()["task"]);
        XX_TEST_EXPECT_TRUE(state == "TASK_STATE_WORKING" || state == "TASK_STATE_SUBMITTED");
    }

    // Wait a bit for the execution thread to finish (it will fail since no DeepAgent)
    co_await asio::steady_timer(co_await asio::this_coro::executor, std::chrono::milliseconds(200))
        .async_wait(asio::use_awaitable);

    // GetTask should find the task
    if (!taskId.empty()) {
        auto getResult = co_await client.getTask(taskId);
        XX_TEST_EXPECT_TRUE(getResult.has_value());
        if (getResult.has_value()) {
            auto& task = getResult.value();
            XX_TEST_EXPECT_EQ(task["id"].get<std::string>(), taskId);
            XX_TEST_EXPECT_TRUE(task.contains("contextId"));
            XX_TEST_EXPECT_TRUE(task.contains("status"));
        }
    }

    // ListTasks should include the task
    {
        auto listResult = co_await client.listTasks();
        XX_TEST_EXPECT_TRUE(listResult.has_value());
        if (listResult.has_value()) {
            XX_TEST_EXPECT_TRUE(listResult.value()["totalSize"].get<int>() >= 1);
        }
    }

    server.stop();
    serverThread.join();
    co_return;
}

// ---------------------------------------------------------------------------
// Test: CancelTask on terminal state returns error
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_a2a_cancel_terminal_task() {
    A2aServer::Config cfg;
    cfg.httpConfig.address = "127.0.0.1";
    cfg.httpConfig.port    = 0;

    A2aServer   server(nullptr, std::move(cfg));
    std::thread serverThread;
    auto        port = startServerThread(server, serverThread);

    auto              baseUrl = "http://127.0.0.1:" + std::to_string(port);
    A2aClient::Config clientCfg;
    clientCfg.baseUrl = baseUrl;
    A2aClient client(std::move(clientCfg));

    // Create a task
    auto sendResult = co_await client.sendMessage("test cancel");
    XX_TEST_EXPECT_TRUE(sendResult.has_value());
    std::string taskId;
    if (sendResult.has_value()) {
        taskId = A2aClient::extractTaskId(sendResult.value());
    }

    // Wait for execution to finish (will fail since no DeepAgent -> terminal state)
    co_await asio::steady_timer(co_await asio::this_coro::executor, std::chrono::milliseconds(200))
        .async_wait(asio::use_awaitable);

    // Cancel should fail on terminal state
    if (!taskId.empty()) {
        auto cancelResult = co_await client.cancelTask(taskId);
        XX_TEST_EXPECT_FALSE(cancelResult.has_value());
        if (!cancelResult.has_value()) {
            XX_TEST_EXPECT_TRUE(cancelResult.error().find("-32002") != std::string::npos);
        }
    }

    server.stop();
    serverThread.join();
    co_return;
}

// ---------------------------------------------------------------------------
// Test: ListTasks with filters
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_a2a_list_tasks_filters() {
    A2aServer::Config cfg;
    cfg.httpConfig.address = "127.0.0.1";
    cfg.httpConfig.port    = 0;

    A2aServer   server(nullptr, std::move(cfg));
    std::thread serverThread;
    auto        port = startServerThread(server, serverThread);

    auto              baseUrl = "http://127.0.0.1:" + std::to_string(port);
    A2aClient::Config clientCfg;
    clientCfg.baseUrl = baseUrl;
    A2aClient client(std::move(clientCfg));

    // Create tasks with specific context
    auto sendResult = co_await client.sendMessage("task in context", "", "ctx-filter-test");
    XX_TEST_EXPECT_TRUE(sendResult.has_value());

    // Wait for execution
    co_await asio::steady_timer(co_await asio::this_coro::executor, std::chrono::milliseconds(200))
        .async_wait(asio::use_awaitable);

    // Filter by contextId
    {
        auto result = co_await client.listTasks("ctx-filter-test");
        XX_TEST_EXPECT_TRUE(result.has_value());
        if (result.has_value()) {
            XX_TEST_EXPECT_TRUE(result.value()["totalSize"].get<int>() >= 1);
        }
    }

    // Filter by non-existent context
    {
        auto result = co_await client.listTasks("nonexistent-context");
        XX_TEST_EXPECT_TRUE(result.has_value());
        if (result.has_value()) {
            XX_TEST_EXPECT_EQ(result.value()["totalSize"].get<int>(), 0);
        }
    }

    // Filter by status
    {
        auto result = co_await client.listTasks("", "TASK_STATE_FAILED");
        XX_TEST_EXPECT_TRUE(result.has_value());
        if (result.has_value()) {
            XX_TEST_EXPECT_TRUE(result.value()["totalSize"].get<int>() >= 1);
        }
    }

    server.stop();
    serverThread.join();
    co_return;
}

// ---------------------------------------------------------------------------
// Test: Agent Card structure validation
// ---------------------------------------------------------------------------

static void test_agent_card_structure() {
    A2aServer::Config cfg;
    cfg.httpConfig.address = "127.0.0.1";
    cfg.httpConfig.port    = 0;
    cfg.serverName         = "card-test";
    cfg.description        = "Card structure test";
    cfg.serverVersion      = "2.0.0";
    cfg.inputModes         = {"text/plain", "application/json"};
    cfg.outputModes        = {"text/plain"};
    cfg.skills             = {
        {"skill-1", "Skill One", "First skill",  {"tag1", "tag2"}, {"example1"}},
        {"skill-2", "Skill Two", "Second skill", {"tag3"},         {}          },
    };

    A2aServer server(nullptr, std::move(cfg));
    auto      card = server.agentCard();

    XX_TEST_EXPECT_EQ(card["name"].get<std::string>(), "card-test");
    XX_TEST_EXPECT_EQ(card["description"].get<std::string>(), "Card structure test");
    XX_TEST_EXPECT_EQ(card["version"].get<std::string>(), "2.0.0");
    XX_TEST_EXPECT_TRUE(card["supportedInterfaces"].is_array());
    XX_TEST_EXPECT_TRUE(card["supportedInterfaces"].size() >= 1);
    XX_TEST_EXPECT_TRUE(card["defaultInputModes"].is_array());
    XX_TEST_EXPECT_EQ(card["defaultInputModes"].size(), size_t(2));
    XX_TEST_EXPECT_TRUE(card["defaultOutputModes"].is_array());
    XX_TEST_EXPECT_EQ(card["defaultOutputModes"].size(), size_t(1));
    XX_TEST_EXPECT_TRUE(card["skills"].is_array());
    XX_TEST_EXPECT_EQ(card["skills"].size(), size_t(2));
    XX_TEST_EXPECT_EQ(card["skills"][0]["id"].get<std::string>(), "skill-1");
    XX_TEST_EXPECT_EQ(card["skills"][0]["tags"].size(), size_t(2));
    XX_TEST_EXPECT_TRUE(card["skills"][0].contains("examples"));
    XX_TEST_EXPECT_FALSE(card["skills"][1].contains("examples"));
    XX_TEST_EXPECT_TRUE(card.contains("capabilities"));
    XX_TEST_EXPECT_TRUE(card["capabilities"]["streaming"].get<bool>());
}

// ---------------------------------------------------------------------------
// Test: SendMessage to existing task in terminal state
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_a2a_send_to_terminal_task() {
    A2aServer::Config cfg;
    cfg.httpConfig.address = "127.0.0.1";
    cfg.httpConfig.port    = 0;

    A2aServer   server(nullptr, std::move(cfg));
    std::thread serverThread;
    auto        port = startServerThread(server, serverThread);

    auto              baseUrl = "http://127.0.0.1:" + std::to_string(port);
    A2aClient::Config clientCfg;
    clientCfg.baseUrl = baseUrl;
    A2aClient client(std::move(clientCfg));

    // Create a task and wait for it to reach terminal state
    auto sendResult = co_await client.sendMessage("first message");
    XX_TEST_EXPECT_TRUE(sendResult.has_value());
    std::string taskId;
    if (sendResult.has_value()) {
        taskId = A2aClient::extractTaskId(sendResult.value());
    }

    co_await asio::steady_timer(co_await asio::this_coro::executor, std::chrono::milliseconds(200))
        .async_wait(asio::use_awaitable);

    // Try to send another message to the same task (should fail - terminal state)
    if (!taskId.empty()) {
        auto result = co_await client.sendMessage("second message", taskId);
        XX_TEST_EXPECT_FALSE(result.has_value());
        if (!result.has_value()) {
            XX_TEST_EXPECT_TRUE(result.error().find("-32004") != std::string::npos);
        }
    }

    server.stop();
    serverThread.join();
    co_return;
}

// ---------------------------------------------------------------------------
// Test: GetTask with historyLength
// ---------------------------------------------------------------------------

static asio::awaitable<void> test_a2a_get_task_history() {
    A2aServer::Config cfg;
    cfg.httpConfig.address = "127.0.0.1";
    cfg.httpConfig.port    = 0;

    A2aServer   server(nullptr, std::move(cfg));
    std::thread serverThread;
    auto        port = startServerThread(server, serverThread);

    auto              baseUrl = "http://127.0.0.1:" + std::to_string(port);
    A2aClient::Config clientCfg;
    clientCfg.baseUrl = baseUrl;
    A2aClient client(std::move(clientCfg));

    auto sendResult = co_await client.sendMessage("history test");
    XX_TEST_EXPECT_TRUE(sendResult.has_value());
    std::string taskId;
    if (sendResult.has_value()) {
        taskId = A2aClient::extractTaskId(sendResult.value());
    }

    co_await asio::steady_timer(co_await asio::this_coro::executor, std::chrono::milliseconds(200))
        .async_wait(asio::use_awaitable);

    if (!taskId.empty()) {
        // Get with history
        auto result = co_await client.getTask(taskId, 10);
        XX_TEST_EXPECT_TRUE(result.has_value());
        if (result.has_value()) {
            auto& task = result.value();
            XX_TEST_EXPECT_TRUE(task.contains("history"));
            XX_TEST_EXPECT_TRUE(task["history"].is_array());
            XX_TEST_EXPECT_TRUE(task["history"].size() >= 1);
            // First message should be from user
            XX_TEST_EXPECT_EQ(task["history"][0]["role"].get<std::string>(), "ROLE_USER");
        }

        // Get without history (historyLength=0)
        auto result2 = co_await client.getTask(taskId, 0);
        XX_TEST_EXPECT_TRUE(result2.has_value());
        if (result2.has_value()) {
            XX_TEST_EXPECT_FALSE(result2.value().contains("history"));
        }
    }

    server.stop();
    serverThread.join();
    co_return;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

asio::awaitable<TestResult> run_a2a_tests() {
    TEST_INFO << "=== A2A Protocol Tests ===" << std::endl;

    // Unit tests (synchronous)
    test_task_state_conversion();
    test_terminal_state();
    test_json_rpc_helpers();
    test_make_text_part();
    test_make_message();
    test_make_task();
    test_extract_text_from_parts();
    test_generate_id();
    test_current_timestamp();
    test_error_helpers();
    test_client_build_text_message();
    test_client_extract_helpers();
    test_agent_card_structure();

    // Integration tests (async)
    co_await test_a2a_server_integration();
    co_await test_a2a_send_message_creates_task();
    co_await test_a2a_cancel_terminal_task();
    co_await test_a2a_list_tasks_filters();
    co_await test_a2a_send_to_terminal_task();
    co_await test_a2a_get_task_history();

    co_return TestResult{g_a2a_passed, g_a2a_failed};
}

} // namespace test
} // namespace agentxx
