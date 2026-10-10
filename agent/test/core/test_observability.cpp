/// test_observability —— 关键指标与诊断包 (计划 OBS-3 / OBS-4)
///
/// 覆盖:
/// 1. KeyMetrics: 轮次起止与终态分类、首 token 延迟只记一次、模型调用用量与错误
///    分类、工具终态 (成功/失败/取消/中断)、压缩、JSON/summary 字段、reset
/// 2. 凭据脱敏: key=value / Bearer / 裸 token / URL userinfo / 查询参数
/// 3. 日志捕获: 环形缓冲容量、启停、清空
/// 4. 诊断包: 段落齐全 (版本/环境、指标、装配、会话、日志尾部), 不含 API Key 取值,
///    默认不含消息正文、显式打开时含截断正文, 日志尾部经脱敏
#include "agentxx-test/core/test_observability.h"

#include "agentxx/agent/context.h"
#include "agentxx/agent/session_store.h"
#include "agentxx/feature/points.h"
#include "agentxx/feature/registry.h"
#include "agentxx/util/diagnostics.h"
#include "agentxx/util/observability.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_ob_passed = 0;
int g_ob_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_ob_passed
#define XX_TEST_FAILED g_ob_failed

namespace agentxx {
namespace test {

namespace fs = std::filesystem;

namespace {

using agentxx::util::KeyMetrics;

void testKeyMetrics() {
    KeyMetrics m;
    XX_TEST_EXPECT_EQ(m.turns(), uint64_t{0});
    XX_TEST_EXPECT_EQ(m.toolOk(), uint64_t{0});

    // ---- 轮次: 正常结束 ----
    m.noteTurnStart();
    m.noteFirstToken();
    m.noteTurnEnd(KeyMetrics::TurnOutcome::Completed, 123);
    XX_TEST_EXPECT_EQ(m.turns(), uint64_t{1});
    XX_TEST_EXPECT_EQ(m.turnsCompleted(), uint64_t{1});
    XX_TEST_EXPECT_EQ(m.firstTokenSamples(), uint64_t{1});
    XX_TEST_EXPECT_EQ(m.turnTotalMs(), int64_t{123});
    XX_TEST_EXPECT_EQ(m.turnMaxMs(), int64_t{123});

    // 首 token 延迟: 同一轮重复调用只记一次 (流式 token 很密集)
    m.noteFirstToken();
    m.noteFirstToken();
    XX_TEST_EXPECT_EQ(m.firstTokenSamples(), uint64_t{1});

    // ---- 轮次: 其它终态分别计数 ----
    m.noteTurnStart();
    m.noteTurnEnd(KeyMetrics::TurnOutcome::Failed, 200);
    m.noteTurnStart();
    m.noteTurnEnd(KeyMetrics::TurnOutcome::Cancelled, 50);
    m.noteTurnStart();
    m.noteTurnEnd(KeyMetrics::TurnOutcome::Interrupted, 10);
    XX_TEST_EXPECT_EQ(m.turns(), uint64_t{4});
    XX_TEST_EXPECT_EQ(m.turnsFailed(), uint64_t{1});
    XX_TEST_EXPECT_EQ(m.turnsCancelled(), uint64_t{1});
    XX_TEST_EXPECT_EQ(m.turnsInterrupted(), uint64_t{1});
    XX_TEST_EXPECT_EQ(m.turnTotalMs(), int64_t{383});
    XX_TEST_EXPECT_EQ(m.turnMaxMs(), int64_t{200}); ///< 最大值只增不减

    // ---- 模型调用 ----
    m.noteModelCall(100, 20, 30);
    m.noteModelCall(0, 0, 0); ///< provider 未上报用量: 只计次数
    m.noteModelError("rate limit");
    XX_TEST_EXPECT_EQ(m.modelCalls(), uint64_t{2});
    XX_TEST_EXPECT_EQ(m.modelErrors(), uint64_t{1});
    XX_TEST_EXPECT_EQ(m.promptTokens(), int64_t{100});
    XX_TEST_EXPECT_EQ(m.completionTokens(), int64_t{20});
    XX_TEST_EXPECT_EQ(m.cachedTokens(), int64_t{30});
    XX_TEST_EXPECT_EQ(m.lastModelErrorKind(), std::string{"rate limit"});

    // ---- 工具终态 ----
    m.noteToolCall(KeyMetrics::ToolOutcome::Ok);
    m.noteToolCall(KeyMetrics::ToolOutcome::Ok);
    m.noteToolCall(KeyMetrics::ToolOutcome::Failed);
    m.noteToolCall(KeyMetrics::ToolOutcome::Cancelled);
    m.noteToolCall(KeyMetrics::ToolOutcome::Interrupted);
    XX_TEST_EXPECT_EQ(m.toolOk(), uint64_t{2});
    XX_TEST_EXPECT_EQ(m.toolFailed(), uint64_t{1});
    XX_TEST_EXPECT_EQ(m.toolCancelled(), uint64_t{1});
    XX_TEST_EXPECT_EQ(m.toolInterrupted(), uint64_t{1});

    // ---- 压缩 ----
    m.noteCompaction(1000, 300);
    XX_TEST_EXPECT_EQ(m.compactions(), uint64_t{1});
    XX_TEST_EXPECT_EQ(m.compactionTokensBefore(), int64_t{1000});
    XX_TEST_EXPECT_EQ(m.compactionTokensAfter(), int64_t{300});

    // ---- 导出 ----
    const auto json = m.toJson();
    XX_TEST_EXPECT_EQ(json["turns"]["total"].get<int>(), 4);
    XX_TEST_EXPECT_EQ(json["turns"]["completed"].get<int>(), 1);
    XX_TEST_EXPECT_EQ(json["turns"]["max_ms"].get<int64_t>(), int64_t{200});
    XX_TEST_EXPECT_TRUE(json["turns"].contains("avg_ms"));
    XX_TEST_EXPECT_EQ(json["ttft"]["samples"].get<int>(), 1);
    XX_TEST_EXPECT_EQ(json["model"]["calls"].get<int>(), 2);
    XX_TEST_EXPECT_EQ(json["model"]["last_error_kind"].get<std::string>(), std::string{"rate limit"});
    XX_TEST_EXPECT_EQ(json["tools"]["total"].get<int>(), 5);
    XX_TEST_EXPECT_EQ(json["tools"]["failed"].get<int>(), 1);
    XX_TEST_EXPECT_EQ(json["compaction"]["count"].get<int>(), 1);
    const auto summary = m.summary();
    XX_TEST_EXPECT_TRUE(summary.find("turns=4") != std::string::npos);
    XX_TEST_EXPECT_TRUE(summary.find("compactions=1") != std::string::npos);

    // ---- reset ----
    m.reset();
    XX_TEST_EXPECT_EQ(m.turns(), uint64_t{0});
    XX_TEST_EXPECT_EQ(m.modelCalls(), uint64_t{0});
    XX_TEST_EXPECT_EQ(m.toolOk(), uint64_t{0});
    XX_TEST_EXPECT_EQ(m.lastModelErrorKind(), std::string{});
    XX_TEST_EXPECT_EQ(m.toJson()["tools"]["total"].get<int>(), 0);
}

void testRedactSecrets() {
    // key=value / key: value (含引号与大小写)
    {
        const auto out = agentxx::util::redactSecrets("api_key=sk-abcdef1234567890 some text");
        XX_TEST_EXPECT_TRUE(out.find("sk-abcdef1234567890") == std::string::npos);
        XX_TEST_EXPECT_TRUE(out.find("some text") != std::string::npos);
    }
    {
        const auto out = agentxx::util::redactSecrets(R"({"apiKey":"secret-value-123"})");
        XX_TEST_EXPECT_TRUE(out.find("secret-value-123") == std::string::npos);
    }
    {
        const auto out = agentxx::util::redactSecrets("Authorization: Bearer abcdefghijklmnop");
        XX_TEST_EXPECT_TRUE(out.find("abcdefghijklmnop") == std::string::npos);
    }
    {
        const auto out = agentxx::util::redactSecrets("password : hunter2 token: zzz");
        XX_TEST_EXPECT_TRUE(out.find("hunter2") == std::string::npos);
        XX_TEST_EXPECT_TRUE(out.find("zzz") == std::string::npos);
    }
    // 裸 token (常见前缀)
    {
        const auto out = agentxx::util::redactSecrets("call failed with sk-proj-ABCDEFGH12345678");
        XX_TEST_EXPECT_TRUE(out.find("sk-proj-ABCDEFGH12345678") == std::string::npos);
    }
    // URL userinfo 与查询参数
    {
        const auto out = agentxx::util::redactSecrets("GET https://user:p4ssw0rd@example.com/x");
        XX_TEST_EXPECT_TRUE(out.find("p4ssw0rd") == std::string::npos);
    }
    {
        const auto out = agentxx::util::redactSecrets("https://example.com/v1?api_key=topsecret&x=1");
        XX_TEST_EXPECT_TRUE(out.find("topsecret") == std::string::npos);
        XX_TEST_EXPECT_TRUE(out.find("x=1") != std::string::npos);
    }
    // 普通文本不受影响 (避免"什么都屏蔽"导致诊断无用)
    {
        const auto out = agentxx::util::redactSecrets("plugin loaded in 12 ms: tools=5 hooks=1");
        XX_TEST_EXPECT_EQ(out, std::string{"plugin loaded in 12 ms: tools=5 hooks=1"});
    }
}

void testLogCapture() {
    agentxx::util::disableLogCapture();
    XX_TEST_EXPECT_TRUE(agentxx::util::recentLogLines().empty());

    // 未启用捕获时读不到 (不隐式安装, 避免占用内存)
    XX_LOGI("[observability-test] before capture");
    XX_TEST_EXPECT_TRUE(agentxx::util::recentLogLines().empty());

    agentxx::util::enableLogCapture(3);
    XX_LOGI("[observability-test] line-1");
    XX_LOGI("[observability-test] line-2");
    XX_LOGI("[observability-test] line-3");
    XX_LOGI("[observability-test] line-4");
    const auto lines = agentxx::util::recentLogLines();
    XX_TEST_EXPECT_EQ(lines.size(), size_t{3}); ///< 容量上限 3, 最旧的被淘汰
    XX_TEST_EXPECT_TRUE(lines[0].find("line-2") != std::string::npos);
    XX_TEST_EXPECT_TRUE(lines[2].find("line-4") != std::string::npos);
    XX_TEST_EXPECT_TRUE(lines[0].find("[I]") != std::string::npos); ///< 级别前缀

    // limit 生效 (取最近 N 条)
    const auto tail = agentxx::util::recentLogLines(1);
    XX_TEST_EXPECT_EQ(tail.size(), size_t{1});
    if (!tail.empty()) {
        XX_TEST_EXPECT_TRUE(tail[0].find("line-4") != std::string::npos);
    }

    agentxx::util::clearCapturedLogs();
    XX_TEST_EXPECT_TRUE(agentxx::util::recentLogLines().empty());

    // 重新安装 (容量变更) 与关闭
    agentxx::util::enableLogCapture(10);
    XX_LOGI("[observability-test] again");
    XX_TEST_EXPECT_EQ(agentxx::util::recentLogLines().size(), size_t{1});
    agentxx::util::disableLogCapture();
    XX_TEST_EXPECT_TRUE(agentxx::util::recentLogLines().empty());
}

void testDiagnosticsPackage() {
    // ---- 夹具: 配置 (含 API Key) + 指标 + 持久化会话 ----
    auto dir = fs::temp_directory_path()
               / fmt::format(
                   "agentxx_obs_test_{}",
                   std::chrono::steady_clock::now().time_since_epoch().count()
               );
    fs::create_directories(dir);

    auto ctx            = std::make_shared<agentxx::agent::AgentContext>();
    ctx->agentConfig    = std::make_shared<agentxx::agent::AgentConfig>();
    ctx->metrics        = std::make_shared<agentxx::util::KeyMetrics>();
    ctx->agentConfig->model.apiKey    = "sk-live-SECRET0123456789";
    ctx->agentConfig->model.baseUrl   = "https://api.example.com/v1";
    ctx->agentConfig->model.modelName = "test-model";
    ctx->agentConfig->dataDir         = dir.string();
    ctx->agentConfig->enableSessionStore = true;
    // 功能点: 装配上下文两个核心点 (与 BaseAgent::init 同一顺序), 让诊断包的
    // featurePoints 段有真实内容可断言
    ctx->features = std::make_shared<agentxx::feature::Registry>();
    agentxx::feature::registerContextPoints(*ctx->features, agentxx::feature::ContextPointOptions{});

    const std::string sessionId = "obs-session";
    auto store = std::make_shared<agentxx::agent::SessionStore>(
        (dir / "sqlite" / "sessions").string(),
        /*enableWriterLease=*/false
    );
    {
        agentxx::agent::SessionsManager mgr;
        mgr.sessionStore = store;
        auto session     = mgr.getOrCreate(sessionId);
        agentxx::agent::ViewMessage msg;
        msg.id          = "m1";
        msg.role        = agentxx::agent::ViewMessage::Role::User;
        // 正文用独立标记: 与标题分开断言"默认不导出正文"
        msg.text        = "BODY-MARKER-42 this is a user message";
        msg.startTimeMs = 1700000000000LL;
        session->appendViewMessage(msg);
        session->persistNow("test");
        // 固定标题 (避免自动标题取首条消息预览, 使"正文不外泄"的断言失焦)
        store->setSessionTitle(sessionId, "diagnostics fixture");
    }
    ctx->sessions = std::make_shared<agentxx::agent::SessionsManager>();
    ctx->sessions->sessionStore = store;

    agentxx::agent::SessionStore::UsageRecord usage;
    usage.model            = "test-model";
    usage.promptTokens     = 1234;
    usage.completionTokens = 56;
    usage.totalTokens      = 1290;
    usage.ok               = true;
    store->addUsage(sessionId, usage);

    ctx->metrics->noteTurnStart();
    ctx->metrics->noteFirstToken();
    ctx->metrics->noteTurnEnd(agentxx::util::KeyMetrics::TurnOutcome::Completed, 321);
    ctx->metrics->noteToolCall(agentxx::util::KeyMetrics::ToolOutcome::Ok);
    ctx->metrics->noteModelCall(1234, 56, 10);

    agentxx::util::enableLogCapture(20);
    XX_LOGI("[observability-test] diagnostics sample api_key=sk-should-be-hidden");

    // ---- 生成 (默认: 不含消息正文) ----
    const auto text = agentxx::util::buildDiagnosticsText(*ctx, sessionId);
    XX_TEST_EXPECT_TRUE(text.find("# agentxx diagnostics") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("- version:") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("- platform:") != std::string::npos);

    // 指标段
    XX_TEST_EXPECT_TRUE(text.find("## metrics (this process)") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("turns=1") != std::string::npos);

    // 装配段 (与 --dump-config 同源): 含模型/工具/插件/持久化小节
    XX_TEST_EXPECT_TRUE(text.find("## assembly") != std::string::npos);
    XX_TEST_EXPECT_TRUE(
        text.find("model.default: test-model (https://api.example.com/v1)") != std::string::npos
    );
    XX_TEST_EXPECT_TRUE(text.find("tools:") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("plugins[") != std::string::npos);
    // 功能点清单也进装配段 (插件作者与排障看得到"哪个实现现在生效")
    XX_TEST_EXPECT_TRUE(text.find("featurePoints[") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("agentxx.context.countTokens") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("agentxx.context.summarize") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("config_json:") != std::string::npos);

    // 会话段: 计数与用量 (不含正文)
    XX_TEST_EXPECT_TRUE(text.find("## session") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("- session id: obs-session") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("calls=1") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("prompt=1234") != std::string::npos);
    // 标题属于"会话标识" (报障需要知道是哪条会话), 默认导出; 消息正文默认不导出。
    // "不含正文"针对会话段 (日志尾部可能带消息片段, 那是日志本身的语义)
    XX_TEST_EXPECT_TRUE(text.find("- title: diagnostics fixture") != std::string::npos);
    {
        const auto sessionBegin = text.find("## session");
        const auto logBegin     = text.find("## log tail");
        XX_TEST_EXPECT_TRUE(sessionBegin != std::string::npos);
        XX_TEST_EXPECT_TRUE(logBegin != std::string::npos);
        const auto sessionPart = (sessionBegin != std::string::npos && logBegin != std::string::npos
                                  && logBegin > sessionBegin)
                                     ? text.substr(sessionBegin, logBegin - sessionBegin)
                                     : std::string{};
        XX_TEST_EXPECT_FALSE(sessionPart.find("BODY-MARKER-42") != std::string::npos);
    }

    // 日志尾部段: 存在且已脱敏
    XX_TEST_EXPECT_TRUE(text.find("## log tail") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("diagnostics sample") != std::string::npos);
    XX_TEST_EXPECT_FALSE(text.find("sk-should-be-hidden") != std::string::npos);

    // 凭据不外泄: 配置段只标记"已设置", 不出现 Key 取值
    XX_TEST_EXPECT_FALSE(text.find("sk-live-SECRET0123456789") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("\"api_key_set\":true") != std::string::npos
                        || text.find("api_key_set") != std::string::npos);

    // ---- 打开消息正文: 出现截断后的正文 (且经脱敏) ----
    {
        agentxx::util::DiagnosticsOptions opts;
        opts.includeMessages  = true;
        opts.logTailLines     = 0; ///< 不带日志 (验证该开关)
        opts.maxMessageChars  = 8;
        const auto withBody = agentxx::util::buildDiagnosticsText(*ctx, sessionId, opts);
        XX_TEST_EXPECT_TRUE(withBody.find("recent view messages") != std::string::npos);
        XX_TEST_EXPECT_TRUE(withBody.find("BODY-MAR") != std::string::npos); ///< 正文前 8 字符
        XX_TEST_EXPECT_FALSE(withBody.find("this is a user message") != std::string::npos);
        XX_TEST_EXPECT_FALSE(withBody.find("## log tail") != std::string::npos);
    }

    // ---- 无会话 / 无存储 / 无配置的降级路径不崩 ----
    {
        auto bare = std::make_shared<agentxx::agent::AgentContext>();
        const auto bareText = agentxx::util::buildDiagnosticsText(*bare, "nope");
        XX_TEST_EXPECT_TRUE(bareText.find("# agentxx diagnostics") != std::string::npos);
        XX_TEST_EXPECT_FALSE(bareText.find("## metrics") != std::string::npos); ///< 无指标对象
        XX_TEST_EXPECT_TRUE(bareText.find("## session") != std::string::npos); ///< 只给 id
    }

    agentxx::util::disableLogCapture();
    for (int attempt = 0; attempt < 40; ++attempt) {
        std::error_code ec;
        fs::remove_all(dir, ec);
        if (!ec) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
}

} // namespace

TestResult testObservability() {
    testKeyMetrics();
    testRedactSecrets();
    testLogCapture();
    testDiagnosticsPackage();
    return TestResult{g_ob_passed, g_ob_failed};
}

} // namespace test
} // namespace agentxx
