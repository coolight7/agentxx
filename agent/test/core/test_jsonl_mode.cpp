#include "agentxx-test/core/test_jsonl_mode.h"

#include "agentxx-test/core/test_agent.h" // 本地 LLM 模拟器 startDaSimServer/g_da_sim_*
#include "agentxx/agent/code_agent.h"
#include "agentxx/agent/config.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/io/jsonl_io_transport.h"
#include "agentxx/agent/io/session_server_agent_io.h"
#include "agentxx/agent/io/wire_protocol.h"
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见)
int g_jl_passed = 0;
int g_jl_failed = 0;
} // namespace

#define XX_TEST_PASSED g_jl_passed
#define XX_TEST_FAILED g_jl_failed

namespace asio = ::boost::asio;

namespace agentxx {
namespace test {

namespace {

using agentxx::agent::JsonlAgentIOTransport;
using agentxx::agent::SessionServerAgentIO;
using agentxx::agent::WireMessage;

/// 内存行缓冲: 作为传输的读入行来源与输出行去处 (替代真实 stdin/stdout)
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

    size_t outputCount() {
        std::lock_guard<std::mutex> lock(mu);
        return output.size();
    }

    /// 注入用行读写回调: 读完全部输入行后返回 false (= stdin EOF)
    JsonlAgentIOTransport::LineIo lineIo() {
        JsonlAgentIOTransport::LineIo io;
        io.readLine = [this](std::string& line) -> bool {
            std::lock_guard<std::mutex> lock(mu);
            if (input.empty()) {
                return false;
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

/// 输出侧快照: 原始行 + 解析出的消息 + 无法解析的行
struct Collected {
    std::vector<std::string>  rawLines;
    std::vector<WireMessage>  messages;
    std::vector<std::string>  unparsed;
};

/// 取一行消息文本 (去掉结尾换行符)
std::string_view stripNewline(std::string_view line) {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.remove_suffix(1);
    }
    return line;
}

Collected collect(const std::shared_ptr<MemoryLines>& lines) {
    Collected out;
    for (auto& raw : lines->takeOutput()) {
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
        out.rawLines.push_back(std::string{text});
        out.messages.push_back(std::move(*msg));
    }
    return out;
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

/// 累积流式正文: TextToken 增量的文本拼接 (模型回复常被切成多片), 附带完整消息文本
std::string streamedText(const Collected& c) {
    std::string out;
    for (const auto& m : c.messages) {
        if (const auto* d = std::get_if<agentxx::agent::WireDelta>(&m)) {
            if (d->type == agentxx::agent::WireDelta::Type::TextToken) {
                out += d->text;
            }
            if (d->message) {
                out += d->message->text;
            }
        }
    }
    return out;
}

template<typename T>
std::vector<T> allOfType(const Collected& c) {
    std::vector<T> out;
    for (const auto& m : c.messages) {
        if (const auto* v = std::get_if<T>(&m)) {
            out.push_back(*v);
        }
    }
    return out;
}

/// 某类消息在输出序列里的最早位置; 未出现返回 -1
template<typename T>
int indexOf(const Collected& c) {
    for (size_t i = 0; i < c.messages.size(); ++i) {
        if (std::holds_alternative<T>(c.messages[i])) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

/// 等待条件成立 (轮转 io 线程)
asio::awaitable<bool> waitFor(std::function<bool()> cond, int timeoutMs = 10000, int pollMs = 10) {
    auto ex       = co_await asio::this_coro::executor;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{timeoutMs};
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) {
            co_return true;
        }
        asio::steady_timer t(ex);
        t.expires_after(std::chrono::milliseconds{pollMs});
        utilxx_base::AsioErrorCode ec;
        co_await t.async_wait(asio::redirect_error(asio::use_awaitable, ec));
    }
    co_return cond();
}

std::string helloLine(std::string_view sessionId) {
    agentxx::agent::WireHello hello;
    hello.sessionId       = std::string{sessionId};
    hello.language        = "zh-cn";
    hello.protocolVersion = agentxx::agent::WireProtocol::kVersion;
    return agentxx::agent::io::jsonlEncodeMessage(WireMessage{std::move(hello)});
}

std::string userInputLine(
    std::string_view sessionId,
    std::string_view text,
    uint64_t         requestId,
    std::string_view delivery = {}
) {
    agentxx::agent::WireUserInput input;
    input.sessionId = std::string{sessionId};
    input.text      = std::string{text};
    input.requestId = requestId;
    input.delivery  = std::string{delivery};
    return agentxx::agent::io::jsonlEncodeMessage(WireMessage{std::move(input)});
}

} // namespace

// ---------------------------------------------------------------------------
// 1. 分帧编解码 (计划 PRO-8: 只加 JSONL 分帧, 报文 JSON 与 WS 传输一致)
// ---------------------------------------------------------------------------

TestResult testJsonlFraming() {
    const int passedBefore = g_jl_passed;
    const int failedBefore = g_jl_failed;

    const auto encode = [](const WireMessage& m) {
        return agentxx::agent::io::jsonlEncodeMessage(m);
    };
    const auto decode = [](std::string_view line) {
        return agentxx::agent::io::jsonlDecodeLine(line);
    };

    // 编码: 一条消息一行 (结尾恰好一个换行符, JSON 字符串内的换行已转义)
    const auto line = encode(WireMessage{agentxx::agent::WireHello{.sessionId = "s1"}});
    XX_TEST_EXPECT_TRUE(line.ends_with('\n'));
    XX_TEST_EXPECT_EQ(line.find('\n'), line.size() - 1);
    XX_TEST_EXPECT_TRUE(line.find("\"type\":\"hello\"") != std::string::npos);

    // 解码: 合法行回到同一条消息
    {
        auto frame = decode(line);
        XX_TEST_EXPECT_TRUE(frame.error.empty());
        XX_TEST_EXPECT_HAS_VALUE(frame.message);
        if (frame.message.has_value()) {
            XX_TEST_EXPECT_TRUE(std::holds_alternative<agentxx::agent::WireHello>(*frame.message));
            const auto* hello = std::get_if<agentxx::agent::WireHello>(&*frame.message);
            XX_TEST_EXPECT_EQ(hello->sessionId, std::string{"s1"});
        }
    }

    // 容忍一行前后空白与 Windows 换行 (\r\n)
    {
        auto frame = decode("  " + std::string{stripNewline(line)} + "\r\n");
        XX_TEST_EXPECT_TRUE(frame.error.empty());
        XX_TEST_EXPECT_HAS_VALUE(frame.message);
    }

    // 空行: 既无消息也无错误 (调用方跳过)
    {
        auto frame = decode("   \r\n");
        XX_TEST_EXPECT_TRUE(frame.error.empty());
        XX_TEST_EXPECT_FALSE(frame.message.has_value());
        XX_TEST_EXPECT_FALSE(decode("").message.has_value());
    }

    // 非法 JSON: 给出可读原因, 不抛异常
    {
        auto frame = decode(R"({"type":"hello")");
        XX_TEST_EXPECT_FALSE(frame.message.has_value());
        XX_TEST_EXPECT_TRUE(frame.error.find("invalid") != std::string::npos);
    }

    // 根节点不是对象
    {
        auto frame = decode("[1,2,3]");
        XX_TEST_EXPECT_FALSE(frame.message.has_value());
        XX_TEST_EXPECT_TRUE(frame.error.find("JSON object") != std::string::npos);
    }

    // 缺 type 字段
    {
        auto frame = decode(R"({"sessionId":"s1"})");
        XX_TEST_EXPECT_FALSE(frame.message.has_value());
        XX_TEST_EXPECT_TRUE(frame.error.find("type") != std::string::npos);
    }

    // 未知消息类型 (报出对端写错的类型名)
    {
        auto frame = decode(R"({"type":"not_a_message"})");
        XX_TEST_EXPECT_FALSE(frame.message.has_value());
        XX_TEST_EXPECT_TRUE(frame.error.find("not_a_message") != std::string::npos);
    }

    // 超长行: 长度上限拦住, 不进入 JSON 解析
    {
        std::string huge(agentxx::agent::io::kJsonlMaxLineBytes + 16, 'a');
        auto        frame = decode(huge);
        XX_TEST_EXPECT_FALSE(frame.message.has_value());
        XX_TEST_EXPECT_TRUE(frame.error.find("too long") != std::string::npos);
    }

    return TestResult{g_jl_passed - passedBefore, g_jl_failed - failedBefore};
}

// ---------------------------------------------------------------------------
// 2. 传输与真实 agent 联调 (传输层 → 会话端点 → 执行引擎)
// ---------------------------------------------------------------------------

namespace {

/// 联调夹具: 本地 LLM 模拟器 + CodeAgent + 会话端点 + JSONL 传输 (注入内存行缓冲)
struct JsonlFixture {
    std::shared_ptr<DaSimServer>                       sim;
    std::shared_ptr<agentxx::agent::AgentConfig>       cfg;
    std::shared_ptr<agentxx::agent::CodeAgent>         agent;
    std::shared_ptr<SessionServerAgentIO>              endpoint;
    std::shared_ptr<JsonlAgentIOTransport>             transport;
    std::shared_ptr<MemoryLines>                       lines;
    std::string                                        sessionId;

    ~JsonlFixture() {
        if (transport) {
            transport->close();
        }
        if (endpoint) {
            endpoint->stop();
        }
        if (sim) {
            sim->stop();
        }
    }
};

/// 建立 "JSONL 传输 ↔ 会话端点 ↔ CodeAgent" 的联调夹具
/// - 模拟器与 agent 都活到夹具析构 (用例结束时)
/// - **输入行必须在创建夹具前填好**: 注入的读取器在缓冲为空时立即返回 false,
///   按 stdin EOF 处理 (与真实管道一致, EOF 之后不会再有输入)
/// - 夹具建立后, 输出行落在 `lines->output`
asio::awaitable<std::shared_ptr<JsonlFixture>>
    makeJsonlFixture(std::string sessionId, std::shared_ptr<MemoryLines> lines) {
    auto fx  = std::make_shared<JsonlFixture>();
    fx->sim  = std::make_shared<DaSimServer>(startDaSimServer());
    fx->lines = std::move(lines);

    g_da_sim_response_content     = "jsonl-mode-response";
    g_da_sim_tool_calls           = utilxx_base::Json::array();
    g_da_sim_fail_count           = 0;
    g_da_sim_delay_ms             = 0;
    g_da_sim_tool_calls_remaining = -1;

    fx->cfg                  = std::make_shared<agentxx::agent::AgentConfig>();
    fx->cfg->model.baseUrl   = "http://127.0.0.1:" + std::to_string(fx->sim->port);
    fx->cfg->model.apiKey    = "EMPTY";
    fx->cfg->model.modelName = "default-model";
    fx->cfg->llmMaxRetry     = 1;

    fx->agent = std::make_shared<agentxx::agent::CodeAgent>(fx->cfg);
    co_await fx->agent->init();

    auto ex       = co_await asio::this_coro::executor;
    fx->transport = std::make_shared<JsonlAgentIOTransport>(ex, fx->lines->lineIo());
    fx->sessionId = std::move(sessionId);

    SessionServerAgentIO::Config scCfg;
    scCfg.sessionId = fx->sessionId;
    fx->endpoint    = std::make_shared<SessionServerAgentIO>(ex, fx->agent, scCfg);
    fx->endpoint->setTransport(fx->transport);

    asio::co_spawn(
        ex,
        [ep = fx->endpoint, tr = fx->transport]() -> asio::awaitable<void> {
            co_await ep->runTransportLoop(tr);
            co_return;
        },
        asio::detached
    );
    asio::co_spawn(
        ex,
        [ep = fx->endpoint]() -> asio::awaitable<void> {
            co_await ep->run();
            co_return;
        },
        asio::detached
    );
    co_return fx;
}

} // namespace

asio::awaitable<void> testJsonlTransportWithAgent() {
    // 输入: 握手 → 用户输入 → 一行非法 JSON → 一行空行 → 第二条用户输入
    // 全部预先填好 (读取器缓冲空即视为 stdin EOF, 与真实管道一致)
    auto lines = std::make_shared<MemoryLines>();
    lines->pushLine(helloLine("jsonl-transport"));
    lines->pushLine(userInputLine("jsonl-transport", "hello jsonl", 7));
    lines->pushLine(R"({"type":"user_input",)"); // 非法 JSON
    lines->pushLine("   ");                      // 空行 (跳过)
    lines->pushLine(userInputLine("jsonl-transport", "after bad line", 8));

    auto fx = co_await makeJsonlFixture("jsonl-transport", lines);

    // 模型延迟: 让"回执先到、轮次结果后到"这一点可被稳定观察;
    // 第二条输入在第一条轮次进行中到达, 因此会走排队路径 (queued)
    g_da_sim_delay_ms = 400;

    // 握手回执: 先到, 且带会话 id 与服务端能力声明
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return countOfType<agentxx::agent::WireHelloAck>(collect(fx->lines)) > 0;
    }));
    {
        auto             c    = collect(fx->lines);
        auto             acks = allOfType<agentxx::agent::WireHelloAck>(c);
        XX_TEST_EXPECT_EQ(acks.size(), size_t{1});
        if (!acks.empty()) {
            XX_TEST_EXPECT_TRUE(acks[0].ok);
            XX_TEST_EXPECT_EQ(acks[0].sessionId, std::string{"jsonl-transport"});
            XX_TEST_EXPECT_FALSE(acks[0].capabilities.empty());
        }
        // 第一条输出行就是握手回执 (对端按序解析)
        XX_TEST_EXPECT_TRUE(!c.messages.empty());
        XX_TEST_EXPECT_TRUE(
            std::holds_alternative<agentxx::agent::WireHelloAck>(c.messages.front())
        );
    }

    // 输入受理回执: 空闲时状态为 started; 此时轮次尚未结束 (回执 ≠ 轮次完成)
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return countOfType<agentxx::agent::WireInputAck>(collect(fx->lines)) > 0;
    }));
    {
        auto             c    = collect(fx->lines);
        auto             acks = allOfType<agentxx::agent::WireInputAck>(c);
        XX_TEST_EXPECT_GE(acks.size(), size_t{1});
        if (!acks.empty()) {
            XX_TEST_EXPECT_EQ(acks[0].requestId, uint64_t{7});
            XX_TEST_EXPECT_EQ(acks[0].status, std::string{agentxx::agent::InputStatus::Started});
        }
        // 回执已经收到时轮次结果还没有出现 (证明"响应不等于轮次完成")
        XX_TEST_EXPECT_EQ(countOfType<agentxx::agent::WireTurnResult>(c), size_t{0});
    }

    // 非法行: 回一条 WireError(InvalidArgs) 后会话继续可用 (不中断)
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return countOfType<agentxx::agent::WireError>(collect(fx->lines)) > 0;
    }));
    {
        auto c      = collect(fx->lines);
        auto errors = allOfType<agentxx::agent::WireError>(c);
        XX_TEST_EXPECT_EQ(errors.size(), size_t{1});
        if (!errors.empty()) {
            XX_TEST_EXPECT_EQ(errors[0].code, agentxx::agent::WireErrorCode::InvalidArgs);
            XX_TEST_EXPECT_TRUE(errors[0].message.find("invalid") != std::string::npos);
        }
        // 空行与非法行都计入读入行数; 共 5 行 = 握手 + 两条输入 + 非法行 + 空行
        XX_TEST_EXPECT_EQ(fx->transport->stats().linesRead, uint64_t{5});
        XX_TEST_EXPECT_EQ(fx->transport->stats().invalidLines, uint64_t{1});
    }

    // 两条输入各跑完一轮 (非法行之后会话仍然可用)
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return countOfType<agentxx::agent::WireTurnResult>(collect(fx->lines)) >= 2;
    }));
    {
        auto c = collect(fx->lines);
        XX_TEST_EXPECT_EQ(c.unparsed.size(), size_t{0}); // stdout 只有协议行
        XX_TEST_EXPECT_GE(countOfType<agentxx::agent::WireDelta>(c), size_t{2});
        XX_TEST_EXPECT_EQ(countOfType<agentxx::agent::WireTurnResult>(c), size_t{2});
        for (const auto& t : allOfType<agentxx::agent::WireTurnResult>(c)) {
            XX_TEST_EXPECT_FALSE(t.hasError);
            XX_TEST_EXPECT_FALSE(t.interrupted);
        }
        auto acks = allOfType<agentxx::agent::WireInputAck>(c);
        XX_TEST_EXPECT_EQ(acks.size(), size_t{2});
        if (acks.size() >= 2) {
            XX_TEST_EXPECT_EQ(acks[1].requestId, uint64_t{8});
            // 第二条输入在第一条轮次进行中到达 → 排队等待
            XX_TEST_EXPECT_EQ(acks[1].status, std::string{agentxx::agent::InputStatus::Queued});
        }
        // 轮次结果排在两条受理回执之后 (响应 ≠ 轮次完成)
        const int ackIndex = indexOf<agentxx::agent::WireInputAck>(c);
        const int turnIndex = indexOf<agentxx::agent::WireTurnResult>(c);
        XX_TEST_EXPECT_TRUE(turnIndex > ackIndex);

        bool sawText = streamedText(c).find("jsonl-mode-response") != std::string::npos;
        XX_TEST_EXPECT_TRUE(sawText);

        // 输入已结束 (读入行耗尽), 但传输仍可写出 (EOF 不等于关闭)
        XX_TEST_EXPECT_TRUE(fx->transport->inputEnded());
        XX_TEST_EXPECT_TRUE(fx->transport->alive());
        XX_TEST_EXPECT_GE(fx->transport->stats().linesWritten, uint64_t{4});
    }

    // 会话上下文里确实有这两轮内容 (协议行只是外表, 状态落在会话上)
    if (auto session = fx->agent->agentContext->sessions->get("jsonl-transport")) {
        size_t matched = 0;
        for (const auto& m : session->messages()) {
            if (m.content.find("hello jsonl") != std::string::npos
                || m.content.find("after bad line") != std::string::npos) {
                ++matched;
            }
        }
        XX_TEST_EXPECT_EQ(matched, size_t{2});
    } else {
        XX_TEST_EXPECT_TRUE(false);
    }

    g_da_sim_delay_ms = 0;
    co_return;
}

// ---------------------------------------------------------------------------
// 3. 未握手时只接受 hello (复用端点连接阶段校验)
// ---------------------------------------------------------------------------

asio::awaitable<void> testJsonlRequiresHello() {
    // 输入预先填好: ① 握手前的业务消息 ② 握手 ③ 握手后的业务消息
    // (端点按序处理, 因此可以断言"拒绝排在受理之前")
    auto lines = std::make_shared<MemoryLines>();
    lines->pushLine(userInputLine("jsonl-hello", "too early", 1));
    lines->pushLine(helloLine("jsonl-hello"));
    lines->pushLine(userInputLine("jsonl-hello", "after hello", 2));

    auto fx = co_await makeJsonlFixture("jsonl-hello", lines);

    // 握手前的业务消息被拒 (回 InvalidState), 且拒绝排在受理回执之前
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return countOfType<agentxx::agent::WireError>(collect(fx->lines)) > 0;
    }));
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return countOfType<agentxx::agent::WireInputAck>(collect(fx->lines)) > 0;
    }));
    {
        auto c      = collect(fx->lines);
        auto errors = allOfType<agentxx::agent::WireError>(c);
        XX_TEST_EXPECT_EQ(errors.size(), size_t{1});
        if (!errors.empty()) {
            XX_TEST_EXPECT_EQ(errors[0].code, agentxx::agent::WireErrorCode::InvalidState);
        }
        auto acks = allOfType<agentxx::agent::WireInputAck>(c);
        XX_TEST_EXPECT_EQ(acks.size(), size_t{1});
        if (!acks.empty()) {
            XX_TEST_EXPECT_EQ(acks[0].requestId, uint64_t{2}); // 只有握手后的那条被受理
        }
        XX_TEST_EXPECT_TRUE(
            indexOf<agentxx::agent::WireError>(c) < indexOf<agentxx::agent::WireInputAck>(c)
        );
    }
    // 握手之后传输被推进到 ready
    XX_TEST_EXPECT_TRUE(fx->transport->stage() == agentxx::agent::WireConnectionStage::Ready);
    // 握手后的那条输入正常跑完一轮 (等它结束再收尾, 避免测试退出时留下进行中的轮次)
    XX_TEST_EXPECT_TRUE(co_await waitFor([&] {
        return countOfType<agentxx::agent::WireTurnResult>(collect(fx->lines)) > 0;
    }));
    {
        auto c = collect(fx->lines);
        XX_TEST_EXPECT_EQ(countOfType<agentxx::agent::WireTurnResult>(c), size_t{1});
        for (const auto& t : allOfType<agentxx::agent::WireTurnResult>(c)) {
            XX_TEST_EXPECT_FALSE(t.hasError);
        }
        XX_TEST_EXPECT_EQ(c.unparsed.size(), size_t{0});
    }
    co_return;
}

// ---------------------------------------------------------------------------
// 模块入口
// ---------------------------------------------------------------------------

asio::awaitable<TestResult> run_jsonl_mode_tests() {
    const int passedBefore = g_jl_passed;
    const int failedBefore = g_jl_failed;

    (void)testJsonlFraming();
    co_await testJsonlTransportWithAgent();
    co_await testJsonlRequiresHello();

    co_return TestResult{g_jl_passed - passedBefore, g_jl_failed - failedBefore};
}

} // namespace test
} // namespace agentxx
