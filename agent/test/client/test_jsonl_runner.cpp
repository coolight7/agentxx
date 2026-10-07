/// JSONL stdio 一次性运行模式 (client 运行器) 测试 (计划 PRO-8)
///
/// 覆盖运行器级别的语义 (与传输级用例互补, 见 core 模块 `jsonl_mode`):
/// - 输入结束 (stdin EOF) 后仍把轮次结果写出去, 轮次跑完自动收尾返回
/// - 输出里只有协议行 (逐行可解析), 受理回执排在轮次结果之前
/// - 完全没有输入时直接收尾返回, 不空转
#include "agentxx-test/client/test_jsonl_runner.h"

#include "agentxx-test/core/test_agent.h" // 本地 LLM 模拟器
#include "agentxx-client/io/jsonl/jsonl_mode.h"
#include "agentxx/agent/code_agent.h"
#include "agentxx/agent/config.h"
#include "agentxx/agent/io/jsonl_io_transport.h"
#include "agentxx/agent/io/wire_protocol.h"
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见)
int g_jr_passed = 0;
int g_jr_failed = 0;
} // namespace

#define XX_TEST_PASSED g_jr_passed
#define XX_TEST_FAILED g_jr_failed

namespace agentxx {
namespace test {

namespace {

using agentxx::agent::JsonlAgentIOTransport;
using agentxx::agent::WireMessage;

/// 内存行缓冲 (注入运行器的 LineIo: 不碰真实 stdin/stdout)
struct MemoryLines {
    std::mutex               mu;
    std::deque<std::string>  input;
    std::vector<std::string> output;

    void pushLine(std::string line) {
        std::lock_guard<std::mutex> lock(mu);
        input.push_back(std::move(line));
    }

    std::vector<std::string> takeOutput() {
        std::lock_guard<std::mutex> lock(mu);
        return output;
    }

    JsonlAgentIOTransport::LineIo lineIo() {
        JsonlAgentIOTransport::LineIo io;
        io.readLine = [this](std::string& line) -> bool {
            std::lock_guard<std::mutex> lock(mu);
            if (input.empty()) {
                return false; // = stdin EOF
            }
            line = std::move(input.front());
            input.pop_front();
            return true;
        };
        io.writeLine = [this](std::string_view line) {
            std::lock_guard<std::mutex> lock(mu);
            output.emplace_back(line);
        };
        return io;
    }
};

/// 收集输出行并解析 (无法解析的行计入 unparsed)
struct Collected {
    std::vector<WireMessage> messages;
    std::vector<std::string> unparsed;
};

std::string_view stripNewline(std::string_view line) {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.remove_suffix(1);
    }
    return line;
}

Collected collect(const std::shared_ptr<MemoryLines>& lines) {
    Collected out;
    for (const auto& raw : lines->takeOutput()) {
        auto text = stripNewline(raw);
        if (text.empty()) {
            out.unparsed.push_back(raw);
            continue;
        }
        auto msg = agentxx::agent::io::deserialize(text);
        if (!msg.has_value()) {
            out.unparsed.push_back(std::string{text});
            continue;
        }
        out.messages.push_back(std::move(*msg));
    }
    return out;
}

/// 某条消息在输出序列里的位置; 未出现返回 -1
template<typename T>
int indexOf(const Collected& c) {
    for (size_t i = 0; i < c.messages.size(); ++i) {
        if (std::holds_alternative<T>(c.messages[i])) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

template<typename T>
size_t countOfType(const Collected& c) {
    size_t n = 0;
    for (const auto& m : c.messages) {
        if (std::holds_alternative<T>(m)) {
            ++n;
        }
    }
    return n;
}

/// 累积流式正文: TextToken 增量的文本拼接 (模型回复常被切成多片)
std::string streamedText(const Collected& c) {
    std::string out;
    for (const auto& m : c.messages) {
        if (const auto* d = std::get_if<agentxx::agent::WireDelta>(&m)) {
            if (d->type == agentxx::agent::WireDelta::Type::TextToken) {
                out += d->text;
            }
        }
    }
    return out;
}

std::shared_ptr<agentxx::agent::CodeAgent> makeCodeAgent(
    std::shared_ptr<DaSimServer> sim,
    std::string_view            response
) {
    g_da_sim_response_content     = std::string{response};
    g_da_sim_tool_calls           = utilxx_base::Json::array();
    g_da_sim_fail_count           = 0;
    g_da_sim_delay_ms             = 0;
    g_da_sim_tool_calls_remaining = -1;

    auto cfg                  = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl        = "http://127.0.0.1:" + std::to_string(sim->port);
    cfg->model.apiKey         = "EMPTY";
    cfg->model.modelName      = "default-model";
    cfg->llmMaxRetry          = 1;
    return std::make_shared<agentxx::agent::CodeAgent>(cfg);
}

std::string helloLine(std::string_view sessionId) {
    agentxx::agent::WireHello hello;
    hello.sessionId       = std::string{sessionId};
    hello.language        = "zh-cn";
    hello.protocolVersion = agentxx::agent::WireProtocol::kVersion;
    return agentxx::agent::io::jsonlEncodeMessage(WireMessage{std::move(hello)});
}

std::string userInputLine(std::string_view sessionId, std::string_view text, uint64_t requestId) {
    agentxx::agent::WireUserInput input;
    input.sessionId = std::string{sessionId};
    input.text      = std::string{text};
    input.requestId = requestId;
    return agentxx::agent::io::jsonlEncodeMessage(WireMessage{std::move(input)});
}

} // namespace

// ---------------------------------------------------------------------------
// 用例 1: hello + 一条输入, 输入随即结束 → 轮次跑完自动收尾
// ---------------------------------------------------------------------------

void testJsonlRunnerOneShot() {
    auto sim   = std::make_shared<DaSimServer>(startDaSimServer());
    auto agent = makeCodeAgent(sim, "runner-response");

    auto lines = std::make_shared<MemoryLines>();
    lines->pushLine(helloLine("jsonl-runner"));
    lines->pushLine(userInputLine("jsonl-runner", "one shot input", 21));
    // 输入到此为止: 读取器返回 false (= stdin EOF), 之后不再有新输入

    const auto begin = std::chrono::steady_clock::now();
    // runJsonlStdio 阻塞直到一次性运行收尾完成
    agentxx::client::runJsonlStdio(agent, "jsonl-runner", lines->lineIo());
    const auto costMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - begin
    )
                            .count();

    // 收尾返回: 既不是挂住 (超过 60 秒), 也不是还没跑完就退出
    XX_TEST_EXPECT_TRUE(costMs < 60000);

    auto c = collect(lines);
    // stdout 只有协议行 (每一行都能解析回 Wire 消息)
    XX_TEST_EXPECT_EQ(c.unparsed.size(), size_t{0});

    // 握手回执 / 受理回执 / 轮次结束各一条; 正文经 delta 流式送出
    XX_TEST_EXPECT_EQ(countOfType<agentxx::agent::WireHelloAck>(c), size_t{1});
    XX_TEST_EXPECT_EQ(countOfType<agentxx::agent::WireInputAck>(c), size_t{1});
    XX_TEST_EXPECT_EQ(countOfType<agentxx::agent::WireTurnResult>(c), size_t{1});
    XX_TEST_EXPECT_GE(countOfType<agentxx::agent::WireDelta>(c), size_t{1});

    // 顺序: 握手 → 受理回执 → 轮次结果 (响应不等于轮次完成)
    const int ackIndex  = indexOf<agentxx::agent::WireInputAck>(c);
    const int turnIndex = indexOf<agentxx::agent::WireTurnResult>(c);
    XX_TEST_EXPECT_EQ(indexOf<agentxx::agent::WireHelloAck>(c), 0);
    XX_TEST_EXPECT_TRUE(ackIndex > 0);
    XX_TEST_EXPECT_TRUE(turnIndex > ackIndex);

    // 轮次正常结束, 且模型正文确实出现在流式增量里 (EOF 之后仍能写出结果)
    for (const auto& m : c.messages) {
        if (const auto* t = std::get_if<agentxx::agent::WireTurnResult>(&m)) {
            XX_TEST_EXPECT_FALSE(t->hasError);
            XX_TEST_EXPECT_FALSE(t->interrupted);
        }
    }
    XX_TEST_EXPECT_TRUE(streamedText(c).find("runner-response") != std::string::npos);

    // 会话上下文里有这轮输入 (协议行只是外表)
    if (auto session = agent->agentContext->sessions->get("jsonl-runner")) {
        bool found = false;
        for (const auto& m : session->messages()) {
            if (m.content.find("one shot input") != std::string::npos) {
                found = true;
            }
        }
        XX_TEST_EXPECT_TRUE(found);
    } else {
        XX_TEST_EXPECT_TRUE(false);
    }
}

// ---------------------------------------------------------------------------
// 用例 2: 完全没有输入 → 直接收尾返回, 不空转
// ---------------------------------------------------------------------------

void testJsonlRunnerEmptyInput() {
    auto sim   = std::make_shared<DaSimServer>(startDaSimServer());
    auto agent = makeCodeAgent(sim, "unused-response");

    auto lines = std::make_shared<MemoryLines>();
    // 没有任何输入行: 读取器第一次调用就返回 false

    const auto begin = std::chrono::steady_clock::now();
    agentxx::client::runJsonlStdio(agent, "jsonl-empty", lines->lineIo());
    const auto costMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - begin
    )
                            .count();

    // 无输入时不应长时间空转 (init 后立即收尾)
    XX_TEST_EXPECT_TRUE(costMs < 30000);
    auto c = collect(lines);
    XX_TEST_EXPECT_EQ(c.unparsed.size(), size_t{0});
    XX_TEST_EXPECT_EQ(c.messages.size(), size_t{0});
}

TestResult testJsonlRunner() {
    const int passedBefore = g_jr_passed;
    const int failedBefore = g_jr_failed;

    // 运行器自己持有 io_context 并在其中运行: 用例直接同步调用 (返回即收尾完成)
    testJsonlRunnerOneShot();
    testJsonlRunnerEmptyInput();

    return TestResult{g_jr_passed - passedBefore, g_jr_failed - failedBefore};
}

} // namespace test
} // namespace agentxx
