/// HTTP 录制/回放夹具测试 (计划 LLM-13)
///
/// 覆盖:
/// - 摘要与固定装置往返 (JSON / 文件)
/// - 端到端: 真实 provider 经录制器打本地 LLM 模拟器 → 固定装置 → 回放器再跑一次,
///   两次结果一致; 固定装置无凭据取值 (脱敏)
/// - 请求与固定装置不符时的拒绝路径 (400 + 可读原因), 以及固定装置用尽后的额外请求
/// - 脱敏细节: 头白名单 / 凭据存在标记 / 正文 secrets 与通用凭据形态
#include "agentxx-test/core/test_http_recorder.h"

#include "agentxx-test/core/http_recorder.h"
#include "agentxx-test/core/test_agent.h" // 本地 LLM 模拟器
#include "agentxx/protocol/openai_provider.h"
#include "utilxx/http_client.h"
#include <chrono>
#include <filesystem>
#include <fmt/format.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见)
int g_hr_passed = 0;
int g_hr_failed = 0;
} // namespace

#define XX_TEST_PASSED g_hr_passed
#define XX_TEST_FAILED g_hr_failed

namespace agentxx {
namespace test {

namespace fs = std::filesystem;

namespace {

std::string makeTempDir() {
    auto dir = fs::temp_directory_path()
               / fmt::format(
                   "agentxx_hr_test_{}",
                   std::chrono::steady_clock::now().time_since_epoch().count()
               );
    fs::create_directories(dir);
    return dir.string();
}

void removeTempDir(const std::string& dir) {
    for (int attempt = 0; attempt < 40; ++attempt) {
        std::error_code ec;
        fs::remove_all(utilxx_base::utf8ToPath(dir), ec);
        if (!ec) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
}

/// 构造一个指向本地 baseUrl 的 OpenAI provider 配置
agentxx::agent::ModelConfig makeProviderCfg(std::string baseUrl, std::string apiKey) {
    agentxx::agent::ModelConfig mc;
    mc.name                    = "http-recorder-test";
    mc.type                    = "openai";
    mc.baseUrl                 = std::move(baseUrl);
    mc.apiKey                  = std::move(apiKey);
    mc.modelName               = "sim-model";
    mc.connectTimeoutSeconds   = 5;
    mc.readChunkTimeoutSeconds = 10;
    return mc;
}

neograph::CompletionParams makeParams(std::string text) {
    neograph::CompletionParams params;
    params.model    = "sim-model";
    params.messages = {
        neograph::ChatMessage{.role = "user", .content = std::move(text)}
    };
    return params;
}

} // namespace

// ---------------------------------------------------------------------------
// 1. 摘要与固定装置往返
// ---------------------------------------------------------------------------

TestResult testHttpFixtureBasics() {
    const int passedBefore = g_hr_passed;
    const int failedBefore = g_hr_failed;

    // 摘要: 相同输入稳定, 任一要素变化即不同
    const auto d1 = agentxx::test::httpRequestDigest("POST", "/v1/chat/completions", "{\"a\":1}");
    const auto d2 = agentxx::test::httpRequestDigest("POST", "/v1/chat/completions", "{\"a\":1}");
    XX_TEST_EXPECT_EQ(d1, d2);
    XX_TEST_EXPECT_EQ(d1.size(), size_t{16});
    XX_TEST_EXPECT_FALSE(
        d1 == agentxx::test::httpRequestDigest("POST", "/v1/chat/completions", "{\"a\":2}")
    );
    XX_TEST_EXPECT_FALSE(
        d1 == agentxx::test::httpRequestDigest("GET", "/v1/chat/completions", "{\"a\":1}")
    );
    XX_TEST_EXPECT_FALSE(
        d1 == agentxx::test::httpRequestDigest("POST", "/v1/messages", "{\"a\":1}")
    );

    // JSON 往返
    HttpFixture fixture;
    fixture.note         = "往返用例";
    HttpInteraction one;
    one.method        = "POST";
    one.path          = "/v1/chat/completions";
    one.requestBody   = "{\"model\":\"m\"}";
    one.requestDigest = agentxx::test::httpRequestDigest(one.method, one.path, one.requestBody);
    one.requestBytes  = one.requestBody.size();
    one.requestHeaders = utilxx_base::Json{{"content-type", "application/json"}};
    one.status         = 200;
    one.contentType    = "application/json";
    one.responseBody   = "{\"ok\":true}";
    fixture.interactions.push_back(one);

    const auto json     = fixture.toJson();
    const auto restored = HttpFixture::fromJson(json);
    XX_TEST_EXPECT_EQ(restored.note, std::string{"往返用例"});
    XX_TEST_EXPECT_EQ(restored.interactions.size(), size_t{1});
    if (restored.interactions.size() == 1) {
        const auto& it = restored.interactions[0];
        XX_TEST_EXPECT_EQ(it.method, one.method);
        XX_TEST_EXPECT_EQ(it.path, one.path);
        XX_TEST_EXPECT_EQ(it.requestDigest, one.requestDigest);
        XX_TEST_EXPECT_EQ(it.requestBody, one.requestBody);
        XX_TEST_EXPECT_EQ(it.requestBytes, one.requestBytes);
        XX_TEST_EXPECT_EQ(it.status, one.status);
        XX_TEST_EXPECT_EQ(it.contentType, one.contentType);
        XX_TEST_EXPECT_EQ(it.responseBody, one.responseBody);
        XX_TEST_EXPECT_EQ(
            it.requestHeaders.value("content-type", std::string{}),
            std::string{"application/json"}
        );
    }

    // 文件往返
    const auto dir  = makeTempDir();
    const auto file = (fs::path{dir} / "fixture" / "http.json").string();
    std::string err;
    XX_TEST_EXPECT_TRUE(fixture.saveToFile(file, &err));
    XX_TEST_EXPECT_TRUE(err.empty());
    XX_TEST_EXPECT_TRUE(fs::exists(utilxx_base::utf8ToPath(file)));
    {
        const auto loaded = HttpFixture::loadFromFile(file);
        XX_TEST_EXPECT_EQ(loaded.interactions.size(), size_t{1});
        if (!loaded.interactions.empty()) {
            XX_TEST_EXPECT_EQ(loaded.interactions[0].responseBody, one.responseBody);
        }
    }
    // 不存在的文件: 明确抛错而不是静默返回空装置
    bool threw = false;
    try {
        (void)HttpFixture::loadFromFile((fs::path{dir} / "nope.json").string());
    } catch (const std::exception&) {
        threw = true;
    }
    XX_TEST_EXPECT_TRUE(threw);
    removeTempDir(dir);

    return TestResult{g_hr_passed - passedBefore, g_hr_failed - failedBefore};
}

// ---------------------------------------------------------------------------
// 2. 脱敏 (头白名单 + 凭据存在标记 + 正文 secrets)
// ---------------------------------------------------------------------------

TestResult testHttpFixtureRedaction() {
    const int passedBefore = g_hr_passed;
    const int failedBefore = g_hr_failed;

    utilxx::HttpServer::Request req;
    req.method(boost::beast::http::verb::post);
    req.target("/v1/chat/completions");
    req.set("Content-Type", "application/json");
    req.set("Anthropic-Version", "2023-06-01");
    req.set("Authorization", "Bearer sk-ant-super-secret");
    req.set("x-api-key", "sk-ant-super-secret");
    req.set("Cookie", "session=abc");

    const auto headers = detail::sanitizeHeaders(req);
    const auto text    = headers.dump();
    XX_TEST_EXPECT_EQ(
        headers.value("content-type", std::string{}),
        std::string{"application/json"}
    );
    XX_TEST_EXPECT_EQ(
        headers.value("anthropic-version", std::string{}),
        std::string{"2023-06-01"}
    );
    XX_TEST_EXPECT_TRUE(headers.value("authorization_present", false));
    XX_TEST_EXPECT_TRUE(headers.value("x-api-key_present", false));
    XX_TEST_EXPECT_TRUE(headers.value("cookie_present", false));
    // 取值一律不落盘
    XX_TEST_EXPECT_TRUE(text.find("sk-ant-super-secret") == std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("session=abc") == std::string::npos);

    // 正文脱敏: 调用方给的 secrets 与通用凭据形态都被屏蔽
    const std::string body
        = R"({"api_key":"sk-live-1234567890","note":"Authorization: Bearer abcdef123456"})";
    const auto sanitized = detail::sanitizeText(body, {"sk-live-1234567890"});
    XX_TEST_EXPECT_TRUE(sanitized.find("sk-live-1234567890") == std::string::npos);
    XX_TEST_EXPECT_TRUE(sanitized.find("abcdef123456") == std::string::npos);
    // 不含凭据的正文原样保留 (避免"什么都屏蔽"让装置失去价值)
    XX_TEST_EXPECT_TRUE(
        detail::sanitizeText("{\"model\":\"sim-model\"}", {}).find("sim-model")
        != std::string::npos
    );

    return TestResult{g_hr_passed - passedBefore, g_hr_failed - failedBefore};
}

// ---------------------------------------------------------------------------
// 3. 端到端: 录制 → 回放
// ---------------------------------------------------------------------------

asio::awaitable<void> testRecordThenReplay() {
    auto sim = std::make_shared<DaSimServer>(startDaSimServer());
    g_da_sim_response_content     = "http-recorder answer";
    g_da_sim_tool_calls           = utilxx_base::Json::array();
    g_da_sim_fail_count           = 0;
    g_da_sim_delay_ms             = 0;
    g_da_sim_tool_calls_remaining = -1;

    const std::string upstream = fmt::format("http://127.0.0.1:{}", sim->port);
    const std::string apiKey   = "sk-live-should-not-be-recorded";

    // ---- 录制: provider → 录制器 → 模拟器 -------------------------------------
    HttpRecorder::Options recOpts;
    recOpts.upstreamOrigin = upstream;
    recOpts.secrets        = {apiKey};
    auto recorder          = HttpRecorder::start(recOpts);
    XX_TEST_EXPECT_TRUE(recorder != nullptr);
    if (!recorder) {
        sim->stop();
        co_return;
    }

    {
        auto provider = agentxx::protocol::OpenAIProvider::create(
            makeProviderCfg(recorder->baseUrl(), apiKey)
        );
        auto result = co_await provider->invoke(makeParams("录制一轮"), nullptr);
        XX_TEST_EXPECT_TRUE(
            result.message.content.find("http-recorder answer") != std::string::npos
        );
        XX_TEST_EXPECT_EQ(recorder->count(), size_t{1});
        const auto fixture = recorder->fixture();
        XX_TEST_EXPECT_EQ(fixture.interactions.size(), size_t{1});
        if (!fixture.interactions.empty()) {
            const auto& it = fixture.interactions[0];
            XX_TEST_EXPECT_EQ(it.method, std::string{"POST"});
            XX_TEST_EXPECT_EQ(it.path, std::string{"/chat/completions"});
            XX_TEST_EXPECT_EQ(it.status, 200);
            XX_TEST_EXPECT_TRUE(
                it.responseBody.find("http-recorder answer") != std::string::npos
            );
            XX_TEST_EXPECT_TRUE(it.requestBytes > 0);
            XX_TEST_EXPECT_EQ(it.requestDigest.size(), size_t{16});
            // 凭据: 只记"出现过", 不记取值
            XX_TEST_EXPECT_TRUE(it.requestHeaders.value("authorization_present", false));
            const auto dumped = it.requestHeaders.dump();
            XX_TEST_EXPECT_TRUE(dumped.find(apiKey) == std::string::npos);
        }
        // 整个装置文本里也没有凭据取值
        XX_TEST_EXPECT_TRUE(fixture.toJson().dump().find(apiKey) == std::string::npos);
    }

    // 固定装置落盘 (入库形态)
    const auto dir  = makeTempDir();
    const auto file = (fs::path{dir} / "recorded.json").string();
    XX_TEST_EXPECT_TRUE(recorder->fixture().saveToFile(file));
    recorder->stop();
    sim->stop();

    // ---- 回放: provider → 回放器 (上游已停) -----------------------------------
    {
        auto loaded = HttpFixture::loadFromFile(file);
        XX_TEST_EXPECT_EQ(loaded.interactions.size(), size_t{1});

        auto player = HttpPlayer::start(loaded);
        XX_TEST_EXPECT_TRUE(player != nullptr);
        if (player) {
            auto provider = agentxx::protocol::OpenAIProvider::create(
                makeProviderCfg(player->baseUrl(), "EMPTY")
            );
            auto result = co_await provider->invoke(makeParams("录制一轮"), nullptr);
            XX_TEST_EXPECT_TRUE(
                result.message.content.find("http-recorder answer") != std::string::npos
            );
            XX_TEST_EXPECT_EQ(player->servedCount(), size_t{1});
            XX_TEST_EXPECT_TRUE(player->allServed());
            XX_TEST_EXPECT_TRUE(player->mismatches().empty());
            player->stop();
        }
    }

    removeTempDir(dir);
    co_return;
}

// ---------------------------------------------------------------------------
// 4. 回放的请求校验: 与装置不符 / 装置用尽
// ---------------------------------------------------------------------------

asio::awaitable<void> testReplayMismatchAndExhaustion() {
    auto sim = std::make_shared<DaSimServer>(startDaSimServer());
    g_da_sim_response_content     = "mismatch-probe answer";
    g_da_sim_tool_calls           = utilxx_base::Json::array();
    g_da_sim_fail_count           = 0;
    g_da_sim_delay_ms             = 0;
    g_da_sim_tool_calls_remaining = -1;

    // 直接构造一条"只对应别的内容"的交互: 真实请求的摘要必然不同
    HttpFixture fixture;
    HttpInteraction expected;
    expected.method        = "POST";
    expected.path          = "/chat/completions";
    expected.requestDigest = agentxx::test::httpRequestDigest(
        expected.method,
        expected.path,
        "{\"model\":\"other\"}"
    );
    expected.requestBytes   = 20;
    expected.status         = 200;
    expected.contentType    = "application/json";
    expected.responseBody   = "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"x\"}}]}";
    fixture.interactions.push_back(expected);

    auto player = HttpPlayer::start(fixture);
    XX_TEST_EXPECT_TRUE(player != nullptr);
    if (!player) {
        sim->stop();
        co_return;
    }

    // ① 请求体与装置不符 → 回放器回 400, provider 抛错, 不推进游标
    {
        auto   provider = agentxx::protocol::OpenAIProvider::create(
            makeProviderCfg(player->baseUrl(), "EMPTY")
        );
        bool   threw = false;
        std::string errText;
        try {
            (void)co_await provider->invoke(makeParams("不一样的内容"), nullptr);
        } catch (const std::exception& e) {
            threw   = true;
            errText = e.what();
        }
        XX_TEST_EXPECT_TRUE(threw);
        XX_TEST_EXPECT_TRUE(errText.find("400") != std::string::npos);
        XX_TEST_EXPECT_EQ(player->servedCount(), size_t{0});
        // 注意: mismatches() 按值返回, 必须先把快照取出来再引用其元素
        const auto mismatches = player->mismatches();
        XX_TEST_EXPECT_EQ(mismatches.size(), size_t{1});
        if (!mismatches.empty()) {
            const auto& why = mismatches[0];
            XX_TEST_EXPECT_TRUE(why.find("mismatch") != std::string::npos);
            XX_TEST_EXPECT_TRUE(why.find("request #0") != std::string::npos);
            XX_TEST_EXPECT_TRUE(why.find(expected.requestDigest) != std::string::npos);
        }
    }

    // ② 更换装置后正常回放一条, 再来一次即"额外的请求" → 同样被拒
    {
        player->stop();
        auto player2 = HttpPlayer::start(fixture);
        XX_TEST_EXPECT_TRUE(player2 != nullptr);
        if (!player2) {
            sim->stop();
            co_return;
        }
        // 用与装置一致的摘要构造请求: 直接按装置内容发一次原始 HTTP 调用
        auto sent = co_await utilxx::HttpClient::postAsync(
            player2->baseUrl() + "/chat/completions",
            std::string{"{\"model\":\"other\"}"},
            std::string_view{"application/json"},
            utilxx::HeaderMap{},
            utilxx::HttpClient::RequestConfig{}
        );
        XX_TEST_EXPECT_TRUE(sent.has_value());
        if (sent.has_value()) {
            XX_TEST_EXPECT_EQ(sent->status, 200);
            XX_TEST_EXPECT_EQ(sent->body, expected.responseBody);
        }
        XX_TEST_EXPECT_EQ(player2->servedCount(), size_t{1});
        XX_TEST_EXPECT_TRUE(player2->allServed());

        auto extra = co_await utilxx::HttpClient::postAsync(
            player2->baseUrl() + "/chat/completions",
            std::string{"{\"model\":\"other\"}"},
            std::string_view{"application/json"},
            utilxx::HeaderMap{},
            utilxx::HttpClient::RequestConfig{}
        );
        XX_TEST_EXPECT_TRUE(extra.has_value());
        if (extra.has_value()) {
            XX_TEST_EXPECT_EQ(extra->status, 400);
            XX_TEST_EXPECT_TRUE(extra->body.find("no more recorded interaction") != std::string::npos);
        }
        const auto mismatches = player2->mismatches();
        XX_TEST_EXPECT_EQ(mismatches.size(), size_t{1});
        if (!mismatches.empty()) {
            XX_TEST_EXPECT_TRUE(mismatches[0].find("extra request") != std::string::npos);
        }
        player2->stop();
    }

    sim->stop();
    co_return;
}

// ---------------------------------------------------------------------------
// 5. 流式响应也能录制回放 (SSE 原样保存与回放)
// ---------------------------------------------------------------------------

asio::awaitable<void> testStreamingReplay() {
    auto sim = std::make_shared<DaSimServer>(startDaSimServer());
    g_da_sim_response_content     = "stream replay text";
    g_da_sim_tool_calls           = utilxx_base::Json::array();
    g_da_sim_fail_count           = 0;
    g_da_sim_delay_ms             = 0;
    g_da_sim_tool_calls_remaining = -1;

    HttpRecorder::Options recOpts;
    recOpts.upstreamOrigin = fmt::format("http://127.0.0.1:{}", sim->port);
    auto recorder          = HttpRecorder::start(recOpts);
    XX_TEST_EXPECT_TRUE(recorder != nullptr);
    if (!recorder) {
        sim->stop();
        co_return;
    }

    std::string firstText;
    std::string chunkedDelta;
    {
        auto provider = agentxx::protocol::OpenAIProvider::create(
            makeProviderCfg(recorder->baseUrl(), "EMPTY")
        );
        neograph::StreamCallback onChunk = [&](const std::string& chunk) { chunkedDelta += chunk; };
        auto result = co_await provider->invoke(makeParams("流式一轮"), onChunk);
        firstText   = result.message.content;
        XX_TEST_EXPECT_TRUE(firstText.find("stream replay text") != std::string::npos);
        XX_TEST_EXPECT_TRUE(chunkedDelta.find("stream replay text") != std::string::npos);
    }

    const auto fixture = recorder->fixture();
    XX_TEST_EXPECT_EQ(fixture.interactions.size(), size_t{1});
    if (!fixture.interactions.empty()) {
        // 流式响应按原样保存 (SSE 文本), 回放时同样按原样回给 provider
        XX_TEST_EXPECT_TRUE(
            fixture.interactions[0].responseBody.find("data:") != std::string::npos
        );
    }
    recorder->stop();
    sim->stop();

    auto player = HttpPlayer::start(fixture);
    XX_TEST_EXPECT_TRUE(player != nullptr);
    if (!player) {
        co_return;
    }
    {
        auto provider = agentxx::protocol::OpenAIProvider::create(
            makeProviderCfg(player->baseUrl(), "EMPTY")
        );
        std::string          replayed;
        neograph::StreamCallback onChunk = [&](const std::string& chunk) { replayed += chunk; };
        auto result = co_await provider->invoke(makeParams("流式一轮"), onChunk);
        XX_TEST_EXPECT_EQ(result.message.content, firstText);
        XX_TEST_EXPECT_EQ(replayed, chunkedDelta);
        XX_TEST_EXPECT_EQ(player->servedCount(), size_t{1});
        XX_TEST_EXPECT_TRUE(player->mismatches().empty());
    }
    player->stop();
    co_return;
}

// ---------------------------------------------------------------------------
// 模块入口
// ---------------------------------------------------------------------------

asio::awaitable<TestResult> run_http_recorder_tests() {
    (void)testHttpFixtureBasics();
    (void)testHttpFixtureRedaction();
    co_await testRecordThenReplay();
    co_await testReplayMismatchAndExhaustion();
    co_await testStreamingReplay();
    // 收尾: 释放 provider 侧 keep-alive 连接池里指向录制器/回放器的空闲连接
    // (服务已停, 连接留着只会拖慢后续用例/进程退出)
    utilxx::HttpClient::clearConnectionPool();
    co_return TestResult{g_hr_passed, g_hr_failed};
}

} // namespace test
} // namespace agentxx
