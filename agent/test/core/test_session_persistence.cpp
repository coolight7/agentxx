#include "agentxx-test/core/test_session_persistence.h"
#include "agentxx-test/core/test_agent.h"
#include "agentxx-test/test_coro_util.h"

#include "agentxx/agent/code_agent.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/io/session_server_agent_io.h"
#include "agentxx/agent/session_store.h"
#include "agentxx/middlewares/interrupt_presets.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/util/sqlite.h"
#include "agentxx/util/tool_args_fix.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <chrono>
#include <filesystem>
#include <fmt/format.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_sp_passed = 0;
int g_sp_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_sp_passed
#define XX_TEST_FAILED g_sp_failed

namespace agentxx {
namespace test {

namespace fs = std::filesystem;

namespace {

/// 创建唯一临时目录 (测试根目录), 返回路径; 由调用方在测试结束经 [removeTempRoot] 清理
std::string makeTempRoot() {
    auto dir = fs::temp_directory_path()
               / fmt::format(
                   "agentxx_sp_test_{}",
                   std::chrono::steady_clock::now().time_since_epoch().count()
               );
    fs::create_directories(dir);
    return dir.string();
}

/// 删除测试临时目录 (目录 + 内容)
/// - Windows 上 SQLite 文件句柄可能比对象析构晚一点释放, 直接删除会失败并抛出
///   异常 (整个模块会被判为失败); 这里短暂重试, 仍失败时仅记日志, 不影响用例结论
void removeTempRoot(const std::string& root) {
    for (int attempt = 0; attempt < 40; ++attempt) {
        std::error_code ec;
        fs::remove_all(utilxx_base::utf8ToPath(root), ec);
        if (!ec) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    std::error_code ec;
    fs::remove_all(utilxx_base::utf8ToPath(root), ec);
    if (ec) {
        XX_LOGW("清理测试临时目录失败: {} ({})", root, ec.message());
    }
}

/// 构造一条带角色专属字段的测试消息
agentxx::agent::ViewMessage makeMsg(agentxx::agent::ViewMessage::Role role, std::string text) {
    using V = agentxx::agent::ViewMessage;
    V msg;
    msg.role        = role;
    msg.text        = std::move(text);
    msg.startTimeMs = 1700000000000LL;
    msg.durationMs  = 123;
    switch (role) {
        case V::Role::Tool: {
            V::ToolData t;
            t.toolName     = "agentxx_share_store";
            t.toolCallId   = "call_001";
            t.toolResult   = "ok";
            t.diff         = "--- a\n+++ b";
            t.toolFinished = true;
            msg.tool       = std::move(t);
            break;
        }
        case V::Role::Tip: {
            V::TipData s;
            s.tipLevel = V::TipLevel::Warning;
            // 运行中标记 (界面显示加载动画): 随 JSON 往返保留, 但加载历史时清掉
            s.running = true;
            msg.tip   = std::move(s);
            break;
        }
        case V::Role::Interrupt: {
            V::InterruptData it;
            it.interruptId = 7;
            // 中断 UI 描述 (声明式表单: 一条消息 = 一份表单; 预设模板生成)
            agentxx::middleware::preset::InputSpec spec;
            spec.label         = "pick one";
            spec.type          = "enum";
            spec.defaultValue  = "b";
            spec.enumValues    = {"a", "b", "c"};
            auto ui            = agentxx::middleware::preset::inputForm({spec});
            it.ui              = ui.toJson();
            it.interruptStatus = V::InterruptStatus::Confirmed;
            it.interruptResult = "b";
            msg.interrupt      = std::move(it);
            break;
        }
        default:
            break;
    }
    return msg;
}

} // namespace

// ---------------------------------------------------------------------------
// SessionStore 单测: viewMessages / llmMessages / meta
// ---------------------------------------------------------------------------

static TestResult testViewMessagesRoundtrip() {
    using agentxx::agent::MediaAttachment;
    using agentxx::agent::MediaType;
    using agentxx::agent::SessionStore;
    using V = agentxx::agent::ViewMessage;

    auto root = makeTempRoot();
    {
        // 写入: user/assistant/tool/system/thinking/interrupt 各一条
        auto p = std::make_shared<SessionStore>(root);
        // 只读访问不创建目录/空文件
        auto fresh = p->loadSession("not-exist-yet");
        XX_TEST_EXPECT_TRUE(fresh.viewMessages.empty());
        XX_TEST_EXPECT_TRUE(!fs::exists(fs::path(root) / "not-exist-yet"));

        std::vector<V> msgs{
            makeMsg(V::Role::User, "hello"),
            makeMsg(V::Role::Assistant, "hi there"),
            makeMsg(V::Role::Tool, R"({"tool":"x"})"),
            makeMsg(V::Role::Tip, "tip"),
            makeMsg(V::Role::Think, "think"),
            makeMsg(V::Role::Interrupt, "interrupt-payload"),
        };
        for (size_t i = 0; i < msgs.size(); ++i) {
            p->appendViewMessage("t1", msgs[i], i + 1);
        }
        // running 标记随消息序列化往返 (wire 与落库共用 toJson/fromJson)
        {
            auto back = V::fromJson(msgs[3].toJson());
            XX_TEST_EXPECT_TRUE(back.tip.has_value());
            if (back.tip) {
                XX_TEST_EXPECT_TRUE(back.tip->running);
            }
        }
        // 同一实例读回
        auto loaded = p->loadSession("t1");
        XX_TEST_EXPECT_EQ(loaded.viewMessages.size(), msgs.size());
        if (loaded.viewMessages.size() == msgs.size()) {
            for (size_t i = 0; i < msgs.size(); ++i) {
                XX_TEST_EXPECT_EQ(loaded.viewMessages[i].role, msgs[i].role);
                XX_TEST_EXPECT_EQ(loaded.viewMessages[i].text, msgs[i].text);
                XX_TEST_EXPECT_EQ(loaded.viewMessages[i].startTimeMs, msgs[i].startTimeMs);
                XX_TEST_EXPECT_EQ(loaded.viewMessages[i].durationMs, msgs[i].durationMs);
            }
        }
        // 角色专属字段往返
        const auto& toolMsg = loaded.viewMessages[2];
        XX_TEST_EXPECT_TRUE(toolMsg.tool.has_value());
        if (toolMsg.tool) {
            XX_TEST_EXPECT_EQ(toolMsg.tool->toolName, "agentxx_share_store");
            XX_TEST_EXPECT_EQ(toolMsg.tool->toolCallId, "call_001");
            XX_TEST_EXPECT_EQ(toolMsg.tool->toolResult, "ok");
            XX_TEST_EXPECT_EQ(toolMsg.tool->diff, "--- a\n+++ b");
            XX_TEST_EXPECT_TRUE(toolMsg.tool->toolFinished);
        }
        const auto& sysMsg = loaded.viewMessages[3];
        XX_TEST_EXPECT_TRUE(sysMsg.tip.has_value());
        if (sysMsg.tip) {
            XX_TEST_EXPECT_EQ(sysMsg.tip->tipLevel, V::TipLevel::Warning);
            // 读取历史时清掉运行中标记: 发起方若在工作结束前退出, 库里留下的是
            // "运行中"副本, 重开后没有对应工作在跑, 加载动画不该一直转
            XX_TEST_EXPECT_TRUE(!sysMsg.tip->running);
        }
        const auto& intMsg = loaded.viewMessages[5];
        XX_TEST_EXPECT_TRUE(intMsg.interrupt.has_value());
        if (intMsg.interrupt) {
            XX_TEST_EXPECT_EQ(intMsg.interrupt->interruptId, int64_t{7});
            // 中断 UI 描述随消息持久化往返 (控件自包含: 候选值/默认值在块上)
            const auto ui = agentxx::middleware::InterruptUi::fromJson(intMsg.interrupt->ui);
            size_t     controlCount = 0;
            for (const auto& block : ui.blocks) {
                if (block.kind != "control") {
                    continue;
                }
                ++controlCount;
                XX_TEST_EXPECT_EQ(block.control, std::string("select"));
                XX_TEST_EXPECT_EQ(block.id, std::string("value"));
                XX_TEST_EXPECT_EQ(block.options.size(), size_t{3});
            }
            XX_TEST_EXPECT_EQ(controlCount, size_t{1});
            XX_TEST_EXPECT_EQ(intMsg.interrupt->interruptStatus, V::InterruptStatus::Confirmed);
            XX_TEST_EXPECT_EQ(intMsg.interrupt->interruptResult, "b");
        }
        // msgIdCounter 恢复
        XX_TEST_EXPECT_EQ(loaded.msgIdCounter, uint64_t{6});

        // 目录布局: {root}/{sanitizedThreadId}/session.db (单库, 含 item)
        auto dir = fs::path(root) / SessionStore::sanitizeSessionId("t1");
        XX_TEST_EXPECT_TRUE(fs::exists(dir / "session.db"));
        XX_TEST_EXPECT_TRUE(!fs::exists(dir / "share_store.db"));

        // 模拟重启: 新实例读同一目录
        auto p2 = std::make_shared<SessionStore>(root);
        auto l2 = p2->loadSession("t1");
        XX_TEST_EXPECT_EQ(l2.viewMessages.size(), msgs.size());
        XX_TEST_EXPECT_EQ(l2.msgIdCounter, uint64_t{6});
        XX_TEST_EXPECT_EQ(l2.viewMessages[0].text, "hello");
    }
    // 清理
    removeTempRoot(root);

    // ---- 视觉为空的消息 (零宽占位符) 照样占位 ----
    // 窗口化后读取期不再过滤它们: 每一行都占一个绝对下标, 读取时过滤会让后续
    // 下标整体前移, 分页的页长与请求就对不上; 表格里不显示由客户端决定
    {
        auto root3 = makeTempRoot();
        auto p     = std::make_shared<SessionStore>(root3);
        // U+200B 零宽空格
        p->appendViewMessage("t-blank", makeMsg(V::Role::Assistant, "\xE2\x80\x8B"), 1);
        p->appendViewMessage("t-blank", makeMsg(V::Role::Assistant, "hello"), 2);
        // 只有空格的思考消息 (且无耗时可展示) 同样不展示
        {
            V think           = makeMsg(V::Role::Think, " ");
            think.collapsed   = true;
            think.durationMs  = 0; // makeMsg 默认带 123ms, 这里置 0 才是真的没信息
            p->appendViewMessage("t-blank", think, 3);
        }
        // 只有耗时、没有任何思考正文的空思考消息也不展示: 它渲染出来只剩一个
        // `[思考]` 头部行, 看上去就是一条空消息并和后面的消息粘在一起
        {
            V think          = makeMsg(V::Role::Think, " ");
            think.collapsed  = true;
            think.durationMs = 10900;
            p->appendViewMessage("t-blank", think, 4);
        }
        // 加密思考 (无明文但有 token 统计) 保留: 头部会展示这些信息
        {
            V think          = makeMsg(V::Role::Think, "");
            think.collapsed  = true;
            think.durationMs = 1200;
            think.think      = V::ThinkData{.reasoningTokens = 42, .isEncrypted = true};
            p->appendViewMessage("t-blank", think, 5);
        }
        // 有正文的思考消息保留 (带耗时+正文)
        {
            V think           = makeMsg(V::Role::Think, "real thought");
            think.collapsed   = true;
            think.durationMs  = 3300;
            p->appendViewMessage("t-blank", think, 6);
        }

        auto loaded = std::make_shared<SessionStore>(root3)->loadSession("t-blank");
        XX_TEST_EXPECT_EQ(loaded.viewMessages.size(), size_t{6});
        if (loaded.viewMessages.size() == 6) {
            XX_TEST_EXPECT_EQ(loaded.viewMessages[0].text, "\xE2\x80\x8B");
            XX_TEST_EXPECT_EQ(loaded.viewMessages[1].text, "hello");
            XX_TEST_EXPECT_EQ(loaded.viewMessages[4].think->reasoningTokens, 42);
            XX_TEST_EXPECT_EQ(loaded.viewMessages[5].text, "real thought");
            XX_TEST_EXPECT_EQ(loaded.viewMessages[5].durationMs, int64_t{3300});
        }
        XX_TEST_EXPECT_TRUE(loaded.msgIdCounter >= uint64_t{6});

        // 增量补拉 (翻页) 与按页读取走同一规则: 不过滤
        auto rows = std::make_shared<SessionStore>(root3)->loadViewMessagesAfter("t-blank", 0);
        XX_TEST_EXPECT_EQ(rows.size(), size_t{6});

        auto page = std::make_shared<SessionStore>(root3)->loadViewMessagesPage("t-blank", 0, 6);
        XX_TEST_EXPECT_EQ(page.size(), size_t{6});
        if (page.size() == 6) {
            XX_TEST_EXPECT_EQ(page[1].text, "hello");
            XX_TEST_EXPECT_EQ(page[5].text, "real thought");
        }

        removeTempRoot(root3);
    }

    // ---- 多模态附件: 落库剥离 dataUrl, 元数据保留 ----
    {
        auto            root2 = makeTempRoot();
        auto            p     = std::make_shared<SessionStore>(root2);
        V               msg   = makeMsg(V::Role::User, "see chart");
        MediaAttachment att;
        att.type        = MediaType::Image;
        att.displayName = "chart.png";
        att.mimeType    = "image/png";
        att.pathOrUrl   = "/tmp/chart.png";
        att.dataUrl     = "data:image/png;base64,AAAABBBB";
        att.sizeBytes   = 42;
        msg.attachments.push_back(att);
        p->appendViewMessage("t-attach", msg, 1);
        auto loaded = p->loadSession("t-attach");
        XX_TEST_EXPECT_EQ(loaded.viewMessages.size(), size_t{1});
        if (!loaded.viewMessages.empty()) {
            const auto& atts = loaded.viewMessages[0].attachments;
            XX_TEST_EXPECT_EQ(atts.size(), size_t{1});
            if (!atts.empty()) {
                XX_TEST_EXPECT_EQ(atts[0].displayName, "chart.png");
                XX_TEST_EXPECT_EQ(atts[0].mimeType, "image/png");
                XX_TEST_EXPECT_EQ(atts[0].pathOrUrl, "/tmp/chart.png");
                XX_TEST_EXPECT_EQ(atts[0].sizeBytes, uint64_t{42});
                // Base64 已剥离, 避免数据库膨胀
                XX_TEST_EXPECT_TRUE(atts[0].dataUrl.empty());
            }
        }
        // wire 往返保留 dataUrl (内存/Sync 路径不受剥离影响)
        auto j  = msg.toJson();
        auto rt = V::fromJson(j);
        XX_TEST_EXPECT_EQ(rt.attachments.size(), size_t{1});
        if (!rt.attachments.empty()) {
            XX_TEST_EXPECT_EQ(rt.attachments[0].dataUrl, "data:image/png;base64,AAAABBBB");
        }
        // 先释放会话库 (Windows 上 SQLite 文件句柄未关闭时目录无法删除)
        p.reset();
        removeTempRoot(root2);
    }
    return TestResult{};
}

static TestResult testLlmMessagesRoundtrip() {
    using agentxx::agent::SessionStore;

    auto root = makeTempRoot();
    {
        auto p = std::make_shared<SessionStore>(root);

        // 空上下文
        p->saveLlmMessages("t2", "[]");
        auto l1 = p->loadSession("t2");
        XX_TEST_EXPECT_TRUE(l1.llmMessages.is_array());
        XX_TEST_EXPECT_EQ(l1.llmMessages.size(), size_t{0});

        // 带 system/user/assistant/tool 的上下文
        utilxx_base::Json ctx = utilxx_base::Json::array();
        ctx.push_back(utilxx_base::Json{
            {"role",    "system"},
            {"content", "sys"   },
        });
        ctx.push_back(utilxx_base::Json{
            {"role",    "user"},
            {"content", "u1"  },
        });
        ctx.push_back(utilxx_base::Json{
            {"role",    "assistant"},
            {"content", "a1"       },
        });
        ctx.push_back(utilxx_base::Json{
            {"role",         "tool"      },
            {"tool_call_id", "call_1"    },
            {"content",      R"({"r":1})"},
        });
        p->saveLlmMessages("t2", ctx.dump());

        // 重启后恢复
        auto p2 = std::make_shared<SessionStore>(root);
        auto l2 = p2->loadSession("t2");
        XX_TEST_EXPECT_TRUE(l2.llmMessages.is_array());
        XX_TEST_EXPECT_EQ(l2.llmMessages.size(), ctx.size());
        if (l2.llmMessages.size() == ctx.size()) {
            XX_TEST_EXPECT_EQ(
                l2.llmMessages[0].value("role", std::string{}),
                std::string{"system"}
            );
            XX_TEST_EXPECT_EQ(
                l2.llmMessages[3].value("tool_call_id", std::string{}),
                std::string{"call_1"}
            );
        }
    }
    removeTempRoot(root);
    return TestResult{};
}

/// `Session::llmMessagesJsonText()` 与 `llmMessagesJson().dump()` 逐字节一致的卡口
///
/// - 落盘走文本路径 (不构造整段数组 DOM), 请求体组装 / WireGetContext / 插件查询
///   仍走 DOM 路径; 两条路径必须给出**同一个 JSON**, 否则"省内存"会把落库内容改掉
/// - 覆盖: 空上下文 / 各类消息形态 (system 含转义字符 / user 带图片 / assistant 带
///   tool_calls + 思考 + history + flags + extra / tool) / 追加与截断后的缓存刷新
static TestResult testLlmMessagesTextMirror() {
    auto s = std::make_shared<agentxx::agent::Session>();

    // 空上下文: 文本形态就是 "[]"
    XX_TEST_EXPECT_EQ(s->llmMessagesJsonText(), std::string{"[]"});
    XX_TEST_EXPECT_EQ(s->llmMessagesJsonText(), s->llmMessagesJson().dump());

    neograph::ChatMessage sys;
    sys.role    = "system";
    sys.content = "sys \"quoted\" \\ 反斜杠\n换行\t制表";

    neograph::ChatMessage user;
    user.role    = "user";
    user.content = "hello 中文 emoji 🙂";
    user.image_urls.push_back("data:image/png;base64,iVBORw0KGgo=");

    neograph::ChatMessage asst;
    asst.role              = "assistant";
    asst.content           = "";
    asst.reasoning_content = "thinking...";
    asst.tool_calls.push_back(
        neograph::ToolCall{.id = "call_1", .name = "read", .arguments = R"({"path":"a\nb"})"}
    );
    asst.tool_calls.push_back(neograph::ToolCall{.id = "call_2", .name = "grep", .arguments = "{}"}
    );
    asst.history_contents.push_back("earlier content");
    asst.flags = neograph::MessageFlag::AutoInserted | neograph::MessageFlag::Summarized;
    asst.extra["share_id"] = 7;

    neograph::ChatMessage tool;
    tool.role         = "tool";
    tool.content      = R"({"r":1})";
    tool.tool_call_id = "call_1";
    tool.tool_name    = "read";
    tool.tool_status  = "completed";

    std::vector<neograph::ChatMessage> msgs;
    msgs.push_back(sys);
    msgs.push_back(user);
    msgs.push_back(asst);
    msgs.push_back(tool);
    s->appendMessages(std::move(msgs), /*persistThrottled=*/false);

    const std::string domDump = s->llmMessagesJson().dump();
    XX_TEST_EXPECT_EQ(s->llmMessagesJsonText(), domDump);
    XX_TEST_EXPECT_TRUE(domDump.find("call_2") != std::string::npos);
    XX_TEST_EXPECT_TRUE(domDump.find("share_id") != std::string::npos);

    // 再追加一条: 文本镜像要跟着脏标记刷新
    neograph::ChatMessage more;
    more.role    = "user";
    more.content = "again";
    s->appendMessages({more}, /*persistThrottled=*/false);
    XX_TEST_EXPECT_TRUE(s->llmMessagesJsonText().find("again") != std::string::npos);
    XX_TEST_EXPECT_EQ(s->llmMessagesJsonText(), s->llmMessagesJson().dump());

    // 截断: 同样刷新, 且两条路径一致
    s->truncateMessages(2, /*persistThrottled=*/false);
    XX_TEST_EXPECT_TRUE(s->llmMessagesJsonText().find("again") == std::string::npos);
    XX_TEST_EXPECT_EQ(s->llmMessagesJsonText(), s->llmMessagesJson().dump());
    XX_TEST_EXPECT_EQ(s->llmMessagesJson().size(), size_t{2});

    // 整批替换 (边界形态入口) 后仍一致
    utilxx_base::Json fromJson = utilxx_base::Json::array();
    fromJson.push_back(utilxx_base::Json{
        {"role",    "user"    },
        {"content", "replaced"}
    });
    s->replaceMessagesFromJson(fromJson);
    XX_TEST_EXPECT_EQ(s->llmMessagesJsonText(), s->llmMessagesJson().dump());

    return TestResult{};
}

// ---------------------------------------------------------------------------
// SessionStore 单测: share store
// ---------------------------------------------------------------------------

static TestResult testShareStoreRoundtrip() {
    using agentxx::agent::SessionStore;

    auto root = makeTempRoot();
    {
        auto p = std::make_shared<SessionStore>(root);

        // 空存储: 自增 id 为 0, 无条目
        XX_TEST_EXPECT_EQ(p->shareStoreLastId("s1"), size_t{0});
        XX_TEST_EXPECT_NULLOPT(p->getShareStoreItem("s1", 1));

        // add: id 从 1 递增
        auto id1 = p->addShareStoreItem("s1", "first");
        auto id2 = p->addShareStoreItem("s1", "second");
        XX_TEST_EXPECT_EQ(id1, size_t{1});
        XX_TEST_EXPECT_EQ(id2, size_t{2});
        XX_TEST_EXPECT_EQ(p->shareStoreLastId("s1"), size_t{2});
        auto v1 = p->getShareStoreItem("s1", id1);
        XX_TEST_EXPECT_HAS_VALUE(v1);
        if (v1) {
            XX_TEST_EXPECT_EQ(*v1, std::string{"first"});
        }

        // set 覆盖已有 id
        p->setShareStoreItem("s1", id1, "first-updated");
        auto v1b = p->getShareStoreItem("s1", id1);
        XX_TEST_EXPECT_HAS_VALUE(v1b);
        if (v1b) {
            XX_TEST_EXPECT_EQ(*v1b, std::string{"first-updated"});
        }

        // 库层不禁止显式写高位 id (工具/中间件层已按自增 id 拦截, 见
        // testMiddlewareShareStorePersistence); 此处保证自增 id 分配不会与该高位
        // id 冲突: 下一个 id 取现有最大 id + 1
        p->setShareStoreItem("s1", 100, "far-id");
        XX_TEST_EXPECT_EQ(p->shareStoreLastId("s1"), size_t{100});
        XX_TEST_EXPECT_EQ(p->getShareStoreItem("s1", 100).value_or(""), std::string{"far-id"});
        auto id3 = p->addShareStoreItem("s1", "third");
        XX_TEST_EXPECT_EQ(id3, size_t{101});
        XX_TEST_EXPECT_EQ(p->shareStoreLastId("s1"), size_t{101});

        // 合法范围内但库中没有的 id (空洞): 返回空, 不报错
        XX_TEST_EXPECT_NULLOPT(p->getShareStoreItem("s1", 50));

        // 模拟重启: 条目与自增 id 延续
        auto p2 = std::make_shared<SessionStore>(root);
        XX_TEST_EXPECT_EQ(p2->shareStoreLastId("s1"), size_t{101});
        XX_TEST_EXPECT_EQ(p2->getShareStoreItem("s1", 100).value_or(""), std::string{"far-id"});
        auto id4 = p2->addShareStoreItem("s1", "fourth");
        XX_TEST_EXPECT_EQ(id4, size_t{102});

        // 多 thread 隔离 (各自独立的自增 id)
        p->addShareStoreItem("s2", "other");
        XX_TEST_EXPECT_EQ(p2->shareStoreLastId("s1"), size_t{102});
        XX_TEST_EXPECT_EQ(p2->shareStoreLastId("s2"), size_t{1});
        XX_TEST_EXPECT_EQ(p2->getShareStoreItem("s2", 1).value_or(""), std::string{"other"});
    }
    removeTempRoot(root);
    return TestResult{};
}

// ---------------------------------------------------------------------------
// Session::updateViewMessage + 持久化: tool 结果回填后重启恢复仍为已完成
// ---------------------------------------------------------------------------

static TestResult testUpdateHistoryPersistence() {
    using agentxx::agent::SessionStore;
    using agentxx::agent::ViewMessage;
    using V = ViewMessage;

    auto root = makeTempRoot();
    {
        auto sessionStore   = std::make_shared<SessionStore>(root);
        auto store          = std::make_shared<agentxx::agent::SessionsManager>();
        store->sessionStore = sessionStore;

        auto s1 = store->getOrCreate("thread-upd");
        s1->appendViewMessage(ViewMessage::makeText(V::Role::User, "u1"));
        // 追加一条未完成的 Tool 消息 (模拟 assistant tool_calls 展开)
        V toolMsg;
        toolMsg.role        = V::Role::Tool;
        toolMsg.text        = R"({"path":"/x"})";
        toolMsg.startTimeMs = 1700000000000LL;
        toolMsg.collapsed   = true;
        V::ToolData t;
        t.toolName     = "agentxx_filesystem_read";
        t.toolCallId   = "call_x";
        t.toolFinished = false; // 尚未收到结果
        toolMsg.tool   = std::move(t);
        auto toolId    = s1->appendViewMessage(std::move(toolMsg));

        // 模拟 tool 结果回填: 走 Session::updateViewMessage (触发 onUpdateViewMessage 落库)
        V stored                  = s1->viewWindow().back();
        stored.tool->toolResult   = "file content";
        stored.tool->toolFinished = true;
        stored.collapsed          = true;
        s1->updateViewMessage(stored);

        // 内存中已完成
        XX_TEST_EXPECT_TRUE(s1->viewWindow().back().tool->toolFinished);
        // 模拟轮末统一补存 (节流窗口内的 append/update 落库)
        s1->flushViewMessages();

        // ---- 模拟重启: 新实例恢复, tool 消息应保持已完成 ----
        auto p2              = std::make_shared<SessionStore>(root);
        auto store2          = std::make_shared<agentxx::agent::SessionsManager>();
        store2->sessionStore = p2;
        auto s2              = store2->getOrCreate("thread-upd");
        XX_TEST_EXPECT_EQ(s2->viewWindow().size(), size_t{2});
        const auto& toolMsg2 = s2->viewWindow()[1];
        XX_TEST_EXPECT_EQ(toolMsg2.id, toolId);
        XX_TEST_EXPECT_TRUE(toolMsg2.tool.has_value());
        if (toolMsg2.tool) {
            XX_TEST_EXPECT_TRUE(toolMsg2.tool->toolFinished);
            XX_TEST_EXPECT_EQ(toolMsg2.tool->toolResult, std::string{"file content"});
        }

        // 回填后内存里就已是完整行, 重启不需要再靠哈希对账
        XX_TEST_EXPECT_EQ(s2->viewMessageCount(), size_t{2});

        // 不存在的 id: 仅记日志, 不崩溃
        V bogus  = ViewMessage::makeText(V::Role::User, "x");
        bogus.id = "msg_999999";
        s2->updateViewMessage(bogus);
    }
    removeTempRoot(root);
    return TestResult{};
}

// ---------------------------------------------------------------------------
// Session + SessionStore 集成: 重启恢复语义
// ---------------------------------------------------------------------------

static TestResult testSessionStoreIntegration() {
    using agentxx::agent::SessionStore;
    using agentxx::agent::ViewMessage;
    using V = ViewMessage;

    auto root = makeTempRoot();
    {
        // ---- 第一次"运行": 写入历史 + LLM 上下文 ----
        auto sessionStore   = std::make_shared<SessionStore>(root);
        auto store          = std::make_shared<agentxx::agent::SessionsManager>();
        store->sessionStore = sessionStore;

        auto s1 = store->getOrCreate("thread-a");
        s1->appendViewMessage(ViewMessage::makeText(V::Role::User, "u1"));
        s1->appendViewMessage(ViewMessage::makeText(V::Role::Assistant, "a1"));
        s1->appendViewMessage(makeMsg(V::Role::Tool, R"({"tool":"x"})"));
        s1->replaceMessagesFromJson(utilxx_base::Json::array({
            utilxx_base::Json{{"role", "system"}, {"content", "sys"}},
            utilxx_base::Json{{"role", "user"},   {"content", "u1"} },
        }));
        s1->saveLlmMessages();
        // 模拟轮末统一补存 (节流窗口内的 append 落库)
        s1->flushViewMessages();
        XX_TEST_EXPECT_EQ(s1->viewMessageCount(), size_t{3});
        XX_TEST_EXPECT_EQ(s1->lastViewSeq(), uint64_t{3});

        // ---- 第二次"运行" (模拟重启): 新建 store + sessionStore ----
        auto p2              = std::make_shared<SessionStore>(root);
        auto store2          = std::make_shared<agentxx::agent::SessionsManager>();
        store2->sessionStore = p2;

        auto s2 = store2->getOrCreate("thread-a");
        XX_TEST_EXPECT_EQ(s2->viewWindow().size(), size_t{3});
        XX_TEST_EXPECT_EQ(s2->viewWindow()[0].id, std::string{"msg_000001"});
        XX_TEST_EXPECT_EQ(s2->viewWindow()[1].id, std::string{"msg_000002"});
        XX_TEST_EXPECT_EQ(s2->viewWindow()[2].tool.has_value(), true);
        // 恢复后总数与最后序号一致 (没有链式哈希了, 直接对序号)
        XX_TEST_EXPECT_EQ(s2->viewMessageCount(), size_t{3});
        XX_TEST_EXPECT_EQ(s2->lastViewSeq(), uint64_t{3});
        XX_TEST_EXPECT_EQ(s2->viewWindowBase(), size_t{0});
        // LLM 上下文恢复
        XX_TEST_EXPECT_TRUE(s2->llmMessagesJson().is_array());
        XX_TEST_EXPECT_EQ(s2->llmMessagesJson().size(), size_t{2});
        // msg id 延续: 新消息不冲突
        auto newId = s2->appendViewMessage(ViewMessage::makeText(V::Role::Assistant, "a2"));
        XX_TEST_EXPECT_EQ(newId, std::string{"msg_000004"});

        // 追加后再"重启", 历史含新消息
        auto p3              = std::make_shared<SessionStore>(root);
        auto store3          = std::make_shared<agentxx::agent::SessionsManager>();
        store3->sessionStore = p3;
        auto s3              = store3->getOrCreate("thread-a");
        XX_TEST_EXPECT_EQ(s3->viewWindow().size(), size_t{4});
        XX_TEST_EXPECT_EQ(s3->viewWindow()[3].text, std::string{"a2"});
        XX_TEST_EXPECT_EQ(s3->viewMessageCount(), size_t{4});
        // 计数延续
        auto newId2 = s3->appendViewMessage(ViewMessage::makeText(V::Role::User, "u3"));
        XX_TEST_EXPECT_EQ(newId2, std::string{"msg_000005"});

        // ---- 不同 thread 互不影响 ----
        auto sOther = store3->getOrCreate("thread-b");
        XX_TEST_EXPECT_EQ(sOther->viewWindow().size(), size_t{0});
        XX_TEST_EXPECT_EQ(sOther->viewMessageCount(), size_t{0});
    }
    removeTempRoot(root);
    return TestResult{};
}

// ---------------------------------------------------------------------------
// 持久化节流: 首次触发立即落盘, 窗口内合并, 轮末强制补存收尾
// ---------------------------------------------------------------------------

static TestResult testPersistThrottle() {
    using agentxx::agent::SessionsManager;
    using agentxx::agent::SessionStore;
    using V = agentxx::agent::ViewMessage;

    auto root = makeTempRoot();
    {
        auto sessionStore   = std::make_shared<SessionStore>(root);
        auto store          = std::make_shared<SessionsManager>();
        store->sessionStore = sessionStore;
        auto s1             = store->getOrCreate("throttle");

        // ---- view 节流: 第 1 条窗口未开启 → 立即落盘 ----
        s1->appendViewMessage(V::makeText(V::Role::User, "u1"));
        {
            SessionStore probe(root);
            XX_TEST_EXPECT_EQ(probe.loadSession("throttle").viewMessages.size(), size_t{1});
        }
        // ---- 第 2/3 条: 节流窗口内 (连续触发) → 合并未落盘 ----
        s1->appendViewMessage(V::makeText(V::Role::Assistant, "a1"));
        s1->appendViewMessage(V::makeText(V::Role::User, "u2"));
        {
            SessionStore probe(root);
            auto         loaded = probe.loadSession("throttle");
            XX_TEST_EXPECT_EQ(loaded.viewMessages.size(), size_t{1});
            // msgIdCounter 与已落库条数一致 (回退恢复语义)
            XX_TEST_EXPECT_EQ(loaded.msgIdCounter, uint64_t{1});
        }
        // ---- llm 节流: 首次结算立即落盘 ----
        utilxx_base::Json ctx = utilxx_base::Json::array();
        ctx.push_back(utilxx_base::Json{
            {"role",    "user"},
            {"content", "u1"  },
        });
        s1->replaceMessagesFromJson(ctx);
        s1->requestSaveLlmMessages();
        {
            SessionStore probe(root);
            XX_TEST_EXPECT_EQ(probe.loadSession("throttle").llmMessages.size(), size_t{1});
        }
        // ---- 第二次结算 (窗口内): 内存增长, 未落盘 ----
        s1->appendSettledLlmMessages(utilxx_base::Json::array({
            utilxx_base::Json{
                              {"role", "assistant"},
                              {"content", "a1"},
                              },
        }));
        XX_TEST_EXPECT_EQ(s1->llmMessagesJson().size(), size_t{2});
        {
            SessionStore probe(root);
            XX_TEST_EXPECT_EQ(probe.loadSession("throttle").llmMessages.size(), size_t{1});
        }
        // ---- 强制补存后全部结束 (轮末 saveLlmMessages + flushViewMessages 语义) ----
        s1->flushViewMessages();
        s1->saveLlmMessages();
        {
            SessionStore probe(root);
            auto         loaded = probe.loadSession("throttle");
            XX_TEST_EXPECT_EQ(loaded.viewMessages.size(), size_t{3});
            XX_TEST_EXPECT_EQ(loaded.viewMessages[1].text, std::string{"a1"});
            XX_TEST_EXPECT_EQ(loaded.viewMessages[2].text, std::string{"u2"});
            XX_TEST_EXPECT_EQ(loaded.msgIdCounter, uint64_t{3});
            XX_TEST_EXPECT_TRUE(loaded.llmMessages.is_array());
            XX_TEST_EXPECT_EQ(loaded.llmMessages.size(), size_t{2});
        }
    }
    removeTempRoot(root);
    return TestResult{};
}

// ---------------------------------------------------------------------------
// MiddlewareContext + share store 写穿持久化
// ---------------------------------------------------------------------------

static TestResult testMiddlewareShareStorePersistence() {
    using agentxx::agent::SessionStore;
    using agentxx::middleware::MiddlewareContext;

    auto root = makeTempRoot();
    {
        auto sessionStore = std::make_shared<SessionStore>(root);

        // ---- 第一次运行: 写穿落库 ----
        auto ctx = std::make_shared<MiddlewareContext>(sessionStore);
        auto id1 = ctx->addShareStoreItemValue("m1", "v1");
        auto id2 = ctx->addShareStoreItemValue("m1", "v2");
        XX_TEST_EXPECT_EQ(id1, size_t{1});
        XX_TEST_EXPECT_EQ(id2, size_t{2});
        XX_TEST_EXPECT_EQ(ctx->getShareStoreItemValue("m1", id1).value_or(""), std::string{"v1"});
        ctx->setShareStoreItemValue("m1", id1, "v1-updated");
        XX_TEST_EXPECT_EQ(
            ctx->getShareStoreItemValue("m1", id1).value_or(""),
            std::string{"v1-updated"}
        );

        // ---- 模拟重启: 新 MiddlewareContext 从 DB 恢复 ----
        auto ctx2 = std::make_shared<MiddlewareContext>(sessionStore);
        XX_TEST_EXPECT_EQ(
            ctx2->getShareStoreItemValue("m1", id1).value_or(""),
            std::string{"v1-updated"}
        );
        XX_TEST_EXPECT_EQ(ctx2->getShareStoreItemValue("m1", id2).value_or(""), std::string{"v2"});

        // 未分配的 id (大于自增 id) → 抛异常
        bool threwUnused = false;
        try {
            (void)ctx2->getShareStoreItemValue("m1", 999);
        } catch (const std::invalid_argument& e) {
            threwUnused = true;
            XX_TEST_EXPECT_TRUE(
                std::string_view{e.what()}.find("out of range") != std::string::npos
            );
        }
        XX_TEST_EXPECT_TRUE(threwUnused);

        // 未用过的 thread: 自增 id 为 0, 任何 id 都未分配 → 抛异常
        bool threwEmptyThread = false;
        try {
            (void)ctx2->getShareStoreItemValue("m-new", 1);
        } catch (const std::invalid_argument&) {
            threwEmptyThread = true;
        }
        XX_TEST_EXPECT_TRUE(threwEmptyThread);

        // set 同样要求 id 已分配 (不能凭空写入新 id)
        bool threwSetUnused = false;
        try {
            ctx2->setShareStoreItemValue("m1", 999, "x");
        } catch (const std::invalid_argument&) {
            threwSetUnused = true;
        }
        XX_TEST_EXPECT_TRUE(threwSetUnused);

        // 合法范围内但库中无此行的 id (旧数据空洞) → nullopt, 不抛异常
        sessionStore->setShareStoreItem("m-sparse", 5, "only5");
        XX_TEST_EXPECT_NULLOPT(ctx2->getShareStoreItemValue("m-sparse", 2));
        XX_TEST_EXPECT_EQ(
            ctx2->getShareStoreItemValue("m-sparse", 5).value_or(""),
            std::string{"only5"}
        );

        // id 延续 (以库中最大 id 为准)
        auto id3 = ctx2->addShareStoreItemValue("m1", "v3");
        XX_TEST_EXPECT_EQ(id3, size_t{3});

        // 重启后仍可读取 (内容以库为准, 与内存缓存无关)
        auto ctx3 = std::make_shared<MiddlewareContext>(sessionStore);
        XX_TEST_EXPECT_EQ(ctx3->getShareStoreItemValue("m1", id3).value_or(""), std::string{"v3"});
        XX_TEST_EXPECT_EQ(
            ctx3->getShareStoreItemValue("m1", id1).value_or(""),
            std::string{"v1-updated"}
        );
        XX_TEST_EXPECT_EQ(ctx3->getShareStoreItemValue("m1", id2).value_or(""), std::string{"v2"});

        // ---- 内存缓存容量: 只保留最近使用的 3 条, 其余条目回库读取 ----
        // 连续写入 6 条, 缓存始终不超过容量 (内存占用与条目数量无关)
        auto   ctx4   = std::make_shared<MiddlewareContext>(sessionStore);
        size_t lastId = 0;
        for (int i = 0; i < 6; ++i) {
            lastId = ctx4->addShareStoreItemValue("m2", fmt::format("item{}", i + 1));
        }
        XX_TEST_EXPECT_EQ(lastId, size_t{6});
        auto cacheIt = ctx4->shareStore.find("m2");
        XX_TEST_EXPECT_TRUE(ctx4->shareStore.end() != cacheIt);
        if (ctx4->shareStore.end() != cacheIt) {
            XX_TEST_EXPECT_EQ(cacheIt->second.cache.capacity(), size_t{3});
            XX_TEST_EXPECT_TRUE(cacheIt->second.cache.size() <= size_t{3});
            XX_TEST_EXPECT_EQ(cacheIt->second.lastId, size_t{6});
        }
        // 早已被淘汰的第 1 条: 缓存未命中, 回库读取仍能取到原值
        XX_TEST_EXPECT_EQ(ctx4->getShareStoreItemValue("m2", 1).value_or(""), std::string{"item1"});
        XX_TEST_EXPECT_EQ(ctx4->getShareStoreItemValue("m2", 3).value_or(""), std::string{"item3"});
        XX_TEST_EXPECT_EQ(ctx4->getShareStoreItemValue("m2", 6).value_or(""), std::string{"item6"});
        // 回读后仍不超过容量
        if (ctx4->shareStore.end() != cacheIt) {
            XX_TEST_EXPECT_TRUE(cacheIt->second.cache.size() <= size_t{3});
        }
    }
    removeTempRoot(root);
    return TestResult{};
}

// ---------------------------------------------------------------------------
// sessionId 清洗
// ---------------------------------------------------------------------------

static TestResult testSanitizeThreadId() {
    using agentxx::agent::SessionStore;

    auto dir = [](std::string_view tid) {
        return SessionStore::sanitizeSessionId(tid);
    };

    // 常规 sessionId 保持不变 (目录可读)
    XX_TEST_EXPECT_EQ(dir("session"), std::string{"session"});
    XX_TEST_EXPECT_EQ(dir("test-thread_1"), std::string{"test-thread_1"});

    // 空串 → default
    XX_TEST_EXPECT_EQ(dir(""), std::string{"default"});

    // 清洗结果不含路径分隔符 / 非法字符
    for (const auto& tid : {"a/b", "a\\b", "../evil", "x:y", "x?y", "x*y", "..", "."}) {
        auto d = dir(tid);
        XX_TEST_EXPECT_TRUE(d.find('/') == std::string::npos);
        XX_TEST_EXPECT_TRUE(d.find('\\') == std::string::npos);
        XX_TEST_EXPECT_TRUE(d.find(':') == std::string::npos);
        XX_TEST_EXPECT_TRUE(!d.empty());
        // 发生过改写的 sessionId 附带 8 位 hex 尾缀 (与原始 sessionId 区分)
        if (tid != d) {
            XX_TEST_EXPECT_TRUE(d.size() >= 9);
            auto suffix = d.substr(d.size() - 9);
            XX_TEST_EXPECT_EQ(suffix[0], '_');
            for (size_t i = 1; i < suffix.size(); ++i) {
                XX_TEST_EXPECT_TRUE(
                    (suffix[i] >= '0' && suffix[i] <= '9') || (suffix[i] >= 'a' && suffix[i] <= 'f')
                );
            }
        }
    }

    // 不同 sessionId 绝不映射到同一目录 (含清洗碰撞规避)
    {
        std::vector<std::string> ids{"a/b", "a_b", "../x", "..", "x:y", "x*y", "x?y", "CON"};
        for (size_t i = 0; i < ids.size(); ++i) {
            for (size_t j = i + 1; j < ids.size(); ++j) {
                XX_TEST_EXPECT_TRUE(dir(ids[i]) != dir(ids[j]));
            }
        }
        // ".." 与 "." 不能映射为父级/自身目录
        XX_TEST_EXPECT_TRUE(dir("..") != std::string{".."});
        XX_TEST_EXPECT_TRUE(dir(".") != std::string{"."});
    }

    // 确定性: 同输入同输出
    XX_TEST_EXPECT_EQ(dir("a/b"), dir("a/b"));

    // 超长 sessionId 截断 (总长受控)
    {
        std::string longTid(300, 'x');
        auto        d = dir(longTid);
        XX_TEST_EXPECT_TRUE(d.size() <= 96 + 9);
        XX_TEST_EXPECT_TRUE(d.rfind("xxxx", d.size() - 10) != std::string::npos);
    }
    return TestResult{};
}

// ---------------------------------------------------------------------------
// 会话列表 keyset 分页列举 (listSessionsPage: 按最近活动降序, 游标续取)
// ---------------------------------------------------------------------------

static TestResult testSessionListPagination() {
    using agentxx::agent::SessionStore;
    using V = agentxx::agent::ViewMessage;

    auto root = makeTempRoot();
    {
        SessionStore store(root);

        // 写入 7 个会话, lastActiveMs 交错; 目录创建顺序与时间顺序不同,
        // 保证排序/分页不依赖目录迭代序。"t5"/"t6" 同为 400ms 平局对,
        // 按 sessionId 升序稳定排列 (t5 在前)
        auto addSession = [&store](std::string_view sid, int64_t tsMs) {
            auto msg        = V::makeText(V::Role::User, fmt::format("msg-{}", sid));
            msg.startTimeMs = tsMs;
            store.appendViewMessage(sid, msg, 1);
        };
        addSession("t1", 700);
        addSession("t2", 600);
        addSession("t3", 500);
        addSession("t5", 400);
        addSession("t4", 300);
        addSession("t6", 400);
        addSession("t7", 200);

        // ---- 完整基准: 降序 + 同毫秒按 id 升序 ----
        const auto all = store.listSessions();
        XX_TEST_EXPECT_EQ(all.size(), size_t{7});
        if (all.size() == 7) {
            XX_TEST_EXPECT_EQ(all[0].sessionId, std::string{"t1"});
            XX_TEST_EXPECT_EQ(all[1].sessionId, std::string{"t2"});
            XX_TEST_EXPECT_EQ(all[2].sessionId, std::string{"t3"});
            XX_TEST_EXPECT_EQ(all[3].sessionId, std::string{"t5"});
            XX_TEST_EXPECT_EQ(all[4].sessionId, std::string{"t6"});
            XX_TEST_EXPECT_EQ(all[5].sessionId, std::string{"t4"});
            XX_TEST_EXPECT_EQ(all[6].sessionId, std::string{"t7"});
        }

        // ---- 首页 limit=3: 最新三条 + hasMore + 总数 ----
        auto p1 = store.listSessionsPage(0, "", 3);
        XX_TEST_EXPECT_EQ(p1.totalCount, uint64_t{7});
        XX_TEST_EXPECT_TRUE(p1.hasMore);
        XX_TEST_EXPECT_EQ(p1.sessions.size(), size_t{3});
        if (p1.sessions.size() == 3) {
            XX_TEST_EXPECT_EQ(p1.sessions[0].sessionId, std::string{"t1"});
            XX_TEST_EXPECT_EQ(p1.sessions[2].sessionId, std::string{"t3"});
        }
        // title 来自首条用户消息预览
        if (!p1.sessions.empty()) {
            XX_TEST_EXPECT_EQ(p1.sessions[0].title, std::string{"msg-t1"});
        }

        // ---- 游标续取至尽: 全序无重复无遗漏 ----
        std::vector<std::string> seen;
        auto                     cursor = p1;
        while (!cursor.sessions.empty()) {
            for (const auto& s : cursor.sessions) {
                seen.push_back(s.sessionId);
            }
            if (!cursor.hasMore) {
                break;
            }
            const auto& last = cursor.sessions.back();
            cursor           = store.listSessionsPage(last.lastActiveMs, last.sessionId, 3);
        }
        XX_TEST_EXPECT_EQ(seen.size(), size_t{7});
        if (seen.size() == 7) {
            XX_TEST_EXPECT_EQ(seen[0], std::string{"t1"});
            XX_TEST_EXPECT_EQ(seen[1], std::string{"t2"});
            XX_TEST_EXPECT_EQ(seen[2], std::string{"t3"});
            XX_TEST_EXPECT_EQ(seen[3], std::string{"t5"});
            XX_TEST_EXPECT_EQ(seen[4], std::string{"t6"});
            XX_TEST_EXPECT_EQ(seen[5], std::string{"t4"});
            XX_TEST_EXPECT_EQ(seen[6], std::string{"t7"});
        }

        // ---- 游标平局边界: beforeMs 恰为某会话时间戳时排除该会话本身,
        //      但包含同毫秒、sessionId 更大的会话 ----
        // 游标 = (400, "t5") → 之后为 t6(400, id 更大)、t4(300)、t7(200)
        auto ptie = store.listSessionsPage(400, "t5", 10);
        XX_TEST_EXPECT_FALSE(ptie.hasMore);
        XX_TEST_EXPECT_EQ(ptie.totalCount, uint64_t{7});
        XX_TEST_EXPECT_EQ(ptie.sessions.size(), size_t{3});
        if (ptie.sessions.size() == 3) {
            XX_TEST_EXPECT_EQ(ptie.sessions[0].sessionId, std::string{"t6"});
            XX_TEST_EXPECT_EQ(ptie.sessions[1].sessionId, std::string{"t4"});
            XX_TEST_EXPECT_EQ(ptie.sessions[2].sessionId, std::string{"t7"});
        }
        // 页大小恰好在平局对中间截断: 首页收满后同毫秒的 t6 排在游标之后的
        // 后续页, 游标携带 sessionId 保证不重不漏
        auto phalf = store.listSessionsPage(0, "", 4);
        if (phalf.sessions.size() == 4) {
            XX_TEST_EXPECT_EQ(phalf.sessions[3].sessionId, std::string{"t5"});
            XX_TEST_EXPECT_TRUE(phalf.hasMore);
            auto ptail = store.listSessionsPage(
                phalf.sessions.back().lastActiveMs,
                phalf.sessions.back().sessionId,
                4
            );
            XX_TEST_EXPECT_FALSE(ptail.hasMore);
            if (ptail.sessions.size() == 3) {
                XX_TEST_EXPECT_EQ(ptail.sessions[0].sessionId, std::string{"t6"});
                XX_TEST_EXPECT_EQ(ptail.sessions[1].sessionId, std::string{"t4"});
                XX_TEST_EXPECT_EQ(ptail.sessions[2].sessionId, std::string{"t7"});
            }
        }

        // ---- limit=0 全部返回路径等价 listSessions ----
        auto pall = store.listSessionsPage(0, "", 0);
        XX_TEST_EXPECT_FALSE(pall.hasMore);
        XX_TEST_EXPECT_EQ(pall.totalCount, uint64_t{7});
        XX_TEST_EXPECT_EQ(pall.sessions.size(), all.size());

        // ---- 空根目录 (从未持久化): 空页无更多 ----
        auto emptyRoot = makeTempRoot();
        {
            SessionStore emptyStore(emptyRoot);
            auto         pe = emptyStore.listSessionsPage(0, "", 5);
            XX_TEST_EXPECT_TRUE(pe.sessions.empty());
            XX_TEST_EXPECT_FALSE(pe.hasMore);
            XX_TEST_EXPECT_EQ(pe.totalCount, uint64_t{0});
        }
        removeTempRoot(emptyRoot);

        // ---- 追加新会话后排最前, 已有游标序列不受影响 ----
        addSession("t0-new", 800);
        auto pn = store.listSessionsPage(0, "", 2);
        XX_TEST_EXPECT_EQ(pn.totalCount, uint64_t{8});
        if (pn.sessions.size() == 2) {
            XX_TEST_EXPECT_EQ(pn.sessions[0].sessionId, std::string{"t0-new"});
            XX_TEST_EXPECT_EQ(pn.sessions[1].sessionId, std::string{"t1"});
        }
        XX_TEST_EXPECT_TRUE(pn.hasMore);
    }
    removeTempRoot(root);
    return TestResult{};
}

// ---------------------------------------------------------------------------
// 全流程: 真实 BaseAgent (模拟 LLM Server) + 持久化, 重启后恢复会话
// ---------------------------------------------------------------------------

static asio::awaitable<void> testSessionPersistenceE2E() {
    using agentxx::agent::CodeAgent;

    auto root = makeTempRoot();
    {
        auto sim     = startDaSimServer();
        auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

        auto makeCfg = [&]() {
            auto cfg                   = std::make_shared<agentxx::agent::AgentConfig>();
            cfg->model.baseUrl         = baseUrl;
            cfg->model.apiKey          = "EMPTY";
            cfg->model.modelName       = "test-sim";
            cfg->prompt.systemPrompt   = "You are a helpful assistant.";
            cfg->enableSessionStore    = true;
            cfg->sessionStoreDirectory = root;
            return cfg;
        };

        // 收尾请求返回真实文本 (provider 层已把完全空响应当作生成失败,
        // 模拟器/用例遵循同一契约: 回合以带内容的 assistant 消息结束)
        g_da_sim_response_content = "E2E final answer";
        g_da_sim_tool_calls       = utilxx_base::Json::array({
            utilxx_base::Json{
                              {"index", 0},
                              {"id", "call_e2e_1"},
                              {"type", "function"},
                              {
                    "function",
                    utilxx_base::Json{
                              {"name", "agentxx_filesystem_list"},
                              {"arguments", "{}"},
                    },
                }, },
        });
        // 模拟 thinking 模型: 首个请求携带 reasoning_content + tool_calls,
        // 验证展开出的 Think 历史消息可持久化并在重启后恢复
        g_da_sim_reasoning_content = "E2E reasoning before tool call";

        // ---- 第一次运行: 一轮对话 + share store 写入 ----
        {
            CodeAgent agent(makeCfg());
            co_await agent.init();

            auto result = co_await agent.runTurnAsync("e2e-thread", "Hello", nullptr);
            XX_TEST_EXPECT_FALSE(result.hasError);
            XX_TEST_EXPECT_FALSE(result.interrupted);

            // 内存 viewMessages: user + Think(tool_calls 展开) + Tool
            auto sess = agent.agentContext->getSession("e2e-thread");
            XX_TEST_EXPECT_TRUE(sess->viewWindow().size() >= size_t{3});
            bool thinkingInMemory = false;
            for (const auto& vm : sess->viewWindow()) {
                if (vm.role == agentxx::agent::ViewMessage::Role::Think
                    && vm.text == "E2E reasoning before tool call") {
                    thinkingInMemory = true;
                }
            }
            XX_TEST_EXPECT_TRUE(thinkingInMemory);

            // share store 写穿
            auto id = agent.agentContext->middlewareHandleContext->addShareStoreItemValue(
                "e2e-thread",
                "stored-value"
            );
            XX_TEST_EXPECT_EQ(id, size_t{1});

            // 落盘检查: 目录布局 {root}/{sessionId}/session.db (单库)
            auto dir
                = fs::path(root) / agentxx::agent::SessionStore::sanitizeSessionId("e2e-thread");
            XX_TEST_EXPECT_TRUE(fs::exists(dir / "session.db"));
            XX_TEST_EXPECT_TRUE(!fs::exists(dir / "share_store.db"));
        }

        // ---- 模拟重启: 新 Agent 实例恢复会话 ----
        {
            CodeAgent agent(makeCfg());
            co_await agent.init();

            auto sess = agent.agentContext->getSession("e2e-thread");
            // 展示历史恢复: user + Think + Tool + Assistant(收尾文本);
            // Think 是本次修复的核心断言, 修复前 tool_calls 分支不展开 Think,
            // 重启后 Think 丢失
            XX_TEST_EXPECT_EQ(sess->viewWindow().size(), size_t{4});
            XX_TEST_EXPECT_EQ(sess->viewWindow()[0].id, std::string{"msg_000001"});
            XX_TEST_EXPECT_EQ(sess->viewWindow()[0].text, std::string{"Hello"});
            XX_TEST_EXPECT_TRUE(
                sess->viewWindow()[1].role == agentxx::agent::ViewMessage::Role::Think
            );
            XX_TEST_EXPECT_EQ(
                sess->viewWindow()[1].text,
                std::string{"E2E reasoning before tool call"}
            );
            XX_TEST_EXPECT_TRUE(sess->viewWindow()[1].collapsed);
            XX_TEST_EXPECT_TRUE(sess->viewWindow()[1].startTimeMs > 0);
            XX_TEST_EXPECT_TRUE(sess->viewWindow()[1].durationMs > 0);
            XX_TEST_EXPECT_TRUE(
                sess->viewWindow()[2].role == agentxx::agent::ViewMessage::Role::Tool
            );
            // 收尾 assistant 消息 (带内容) 同样可持久化恢复
            XX_TEST_EXPECT_TRUE(
                sess->viewWindow()[3].role == agentxx::agent::ViewMessage::Role::Assistant
            );
            XX_TEST_EXPECT_EQ(sess->viewWindow()[3].text, std::string{"E2E final answer"});
            // LLM 上下文恢复 (system + user + assistant(tool_calls) + tool + assistant)
            XX_TEST_EXPECT_TRUE(sess->llmMessagesJson().is_array());
            XX_TEST_EXPECT_TRUE(sess->llmMessagesJson().size() >= size_t{2});
            // msg id 延续
            auto newId = sess->appendViewMessage(agentxx::agent::ViewMessage::makeText(
                agentxx::agent::ViewMessage::Role::Assistant,
                "extra"
            ));
            XX_TEST_EXPECT_EQ(newId, std::string{"msg_000005"});

            // share store 恢复 (懒加载自 DB)
            auto v = agent.agentContext->middlewareHandleContext->getShareStoreItemValue(
                "e2e-thread",
                1
            );
            XX_TEST_EXPECT_HAS_VALUE(v);
            if (v) {
                XX_TEST_EXPECT_EQ(*v, std::string{"stored-value"});
            }
            // 新条目 id 延续
            auto id2 = agent.agentContext->middlewareHandleContext->addShareStoreItemValue(
                "e2e-thread",
                "second"
            );
            XX_TEST_EXPECT_EQ(id2, size_t{2});
        }

        sim.stop();
    }
    removeTempRoot(root);
    co_return;
}

static asio::awaitable<void> testTurnEndTipPersistenceRoundtrip() {
    using agentxx::agent::CodeAgent;
    using agentxx::agent::SessionServerAgentIO;

    auto root = makeTempRoot();
    {
        auto sim     = startDaSimServer();
        auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

        auto makeCfg = [&]() {
            auto cfg                   = std::make_shared<agentxx::agent::AgentConfig>();
            cfg->model.baseUrl         = baseUrl;
            cfg->model.apiKey          = "EMPTY";
            cfg->model.modelName       = "test-model-xyz";
            cfg->prompt.systemPrompt   = "You are a helpful assistant.";
            cfg->enableSessionStore    = true;
            cfg->sessionStoreDirectory = root;
            return cfg;
        };

        g_da_sim_response_content  = "Turn result text";
        g_da_sim_tool_calls        = utilxx_base::Json::array();
        g_da_sim_reasoning_content = "";

        // 第一次运行: 传入 server-io 驱动会话, 验证轮次结束时插入 Tip 消息并落盘
        {
            auto agent = std::make_shared<CodeAgent>(makeCfg());
            co_await agent->init();

            SessionServerAgentIO::Config ioCfg;
            ioCfg.sessionId = "turn-tip-thread";
            auto serverIO   = std::make_shared<SessionServerAgentIO>(
                agent->ioCtx->get_executor(),
                agent,
                std::move(ioCfg)
            );

            auto result = co_await agent->runTurnAsync("turn-tip-thread", "Hello", serverIO);
            XX_TEST_EXPECT_FALSE(result.hasError);

            auto sess = agent->agentContext->getSession("turn-tip-thread");
            XX_TEST_EXPECT_TRUE(sess != nullptr);
            if (sess) {
                // 内存中消息: User + Assistant + Tip (轮次统计: 模型名 · 耗时 · 时间点)
                XX_TEST_EXPECT_EQ(sess->viewWindow().size(), size_t{3});
                if (sess->viewWindow().size() >= 3) {
                    const auto& tipMsg = sess->viewWindow().back();
                    XX_TEST_EXPECT_EQ(tipMsg.role, agentxx::agent::ViewMessage::Role::Tip);
                    XX_TEST_EXPECT_TRUE(tipMsg.text.find("test-model-xyz") != std::string::npos);
                    XX_TEST_EXPECT_TRUE(tipMsg.tip.has_value());
                    if (tipMsg.tip) {
                        XX_TEST_EXPECT_EQ(
                            tipMsg.tip->tipLevel,
                            agentxx::agent::ViewMessage::TipLevel::Info
                        );
                    }
                }
            }
        }

        // 模拟重启: 新 Agent 实例从 SQLite 恢复会话, 验证轮次完成提示消息成功落盘且完整恢复
        {
            CodeAgent agent(makeCfg());
            co_await agent.init();

            auto sess = agent.agentContext->getSession("turn-tip-thread");
            XX_TEST_EXPECT_TRUE(sess != nullptr);
            if (sess) {
                XX_TEST_EXPECT_EQ(sess->viewWindow().size(), size_t{3});
                if (sess->viewWindow().size() >= 3) {
                    XX_TEST_EXPECT_EQ(
                        sess->viewWindow()[0].role,
                        agentxx::agent::ViewMessage::Role::User
                    );
                    XX_TEST_EXPECT_EQ(
                        sess->viewWindow()[1].role,
                        agentxx::agent::ViewMessage::Role::Assistant
                    );
                    const auto& tipMsg = sess->viewWindow()[2];
                    XX_TEST_EXPECT_EQ(tipMsg.role, agentxx::agent::ViewMessage::Role::Tip);
                    XX_TEST_EXPECT_TRUE(tipMsg.text.find("test-model-xyz") != std::string::npos);
                    XX_TEST_EXPECT_TRUE(tipMsg.tip.has_value());
                    if (tipMsg.tip) {
                        XX_TEST_EXPECT_EQ(
                            tipMsg.tip->tipLevel,
                            agentxx::agent::ViewMessage::TipLevel::Info
                        );
                    }
                }
            }
        }

        sim.stop();
    }
    removeTempRoot(root);
    co_return;
}

/// 持久化健壮性 (崩溃/脏数据后尽量少丢数据):
/// ① 正文含非法 UTF-8 (工具输出的 GBK/二进制文本) 时入库前修复, 读取不失败
/// ② 单行历史脏数据 (非 JSON) 只跳过该行, 其余历史正常恢复 (不整体丢弃)
/// ③ LLM 上下文脏数据按空上下文恢复, 展示历史不受影响
static void testPersistenceResilience() {
    using agentxx::agent::SessionStore;
    using V = agentxx::agent::ViewMessage;

    auto root = makeTempRoot();
    {
        auto p = std::make_shared<SessionStore>(root);

        // ① 非法 UTF-8 正文: 落库前按 U+FFFD 修复 (Json::dump 原样转发非法字节,
        //    读取端 simdjson 解析会抛异常 -> 原实现表现为会话整体丢失)
        std::string dirty = "GBK: ";
        dirty.push_back(static_cast<char>(0xB2));
        dirty.push_back(static_cast<char>(0xE2));
        dirty.push_back(static_cast<char>(0xCA));
        dirty.push_back(static_cast<char>(0xD4));
        dirty += " end";
        XX_TEST_EXPECT_FALSE(utilxx_base::utf8IsAvail(dirty));

        p->appendViewMessage("r_utf8", makeMsg(V::Role::User, "before"), 1);
        p->appendViewMessage("r_utf8", makeMsg(V::Role::Assistant, dirty), 2);
        p->appendViewMessage("r_utf8", makeMsg(V::Role::User, "after"), 3);

        auto loaded = p->loadSession("r_utf8");
        XX_TEST_EXPECT_EQ(loaded.viewMessages.size(), size_t{3});
        if (loaded.viewMessages.size() == 3) {
            XX_TEST_EXPECT_EQ(loaded.viewMessages[0].text, std::string{"before"});
            XX_TEST_EXPECT_EQ(loaded.viewMessages[2].text, std::string{"after"});
            // 非法字节被替换, 合法文本保留
            XX_TEST_EXPECT_TRUE(loaded.viewMessages[1].text.starts_with("GBK: "));
            XX_TEST_EXPECT_TRUE(loaded.viewMessages[1].text.find(" end") != std::string::npos);
            // 修复后的文本含 U+FFFD: 合法 UTF-8, 所以带 allowBackstop 判定
            XX_TEST_EXPECT_TRUE(
                utilxx_base::utf8IsAvail(loaded.viewMessages[1].text, /*allowBackstop=*/true)
            );
        }

        // ②③ 脏数据注入: 直接经 SQLite 写入无法解析的行 (模拟历史脏数据)
        p->appendViewMessage("r_dirty", makeMsg(V::Role::User, "keep-me"), 1);
        p->saveLlmMessages(
            "r_dirty",
            utilxx_base::Json::array({
                                         utilxx_base::Json{{"role", "user"}, {"content", "ctx"}}
        }
            ).dump()
        );
        {
            auto dir = fs::path(root) / SessionStore::sanitizeSessionId("r_dirty");
            agentxx::util::SqliteDb db;
            db.open((dir / "session.db").string());
            db.exec("INSERT INTO view_message(json) VALUES ('{not valid json')");
            db.exec("DELETE FROM llm_context");
            db.exec("INSERT INTO llm_context(id, json) VALUES (1, '{oops')");
        }

        auto dirtyLoaded = p->loadSession("r_dirty");
        // 可解析的历史行仍恢复 (坏行被跳过); 脏 LLM 上下文按空上下文恢复
        XX_TEST_EXPECT_EQ(dirtyLoaded.viewMessages.size(), size_t{1});
        if (!dirtyLoaded.viewMessages.empty()) {
            XX_TEST_EXPECT_EQ(dirtyLoaded.viewMessages[0].text, std::string{"keep-me"});
        }
        XX_TEST_EXPECT_TRUE(dirtyLoaded.llmMessages.is_array());
        XX_TEST_EXPECT_TRUE(dirtyLoaded.llmMessages.empty());
    }
    removeTempRoot(root);
}

/// 回归: Session 析构函数线程安全
/// - 绑定的 io 线程析构时正常 flushPendingViewOps
/// - 非 io 线程析构时安全丢弃 pendingViewOps, 禁止跨线程调用 SQLite hooks
static void testSessionDestructorThreadSafety() {
    // Case 1: 在绑定的 io 线程析构 -> 正常回放
    {
        bool hookCalled = false;
        {
            auto sess = std::make_shared<agentxx::agent::Session>();
            sess->bindIoThread();
            XX_TEST_EXPECT_TRUE(sess->isIoThread());

            sess->setStoreHooks(agentxx::agent::SessionStoreHooks{
                .onAppendViewMessage =
                    [&](const agentxx::agent::ViewMessage&, uint64_t, uint64_t) {
                        hookCalled = true;
                    },
            });

            // 首次立即落库, 将 viewLastPersistMs 置为当前时间
            sess->appendViewMessage(makeMsg(agentxx::agent::ViewMessage::Role::User, "msg1"));
            hookCalled = false;

            // 节流窗口内第二条: 进入 pendingViewOps_
            sess->appendViewMessage(makeMsg(agentxx::agent::ViewMessage::Role::User, "msg2"));
            XX_TEST_EXPECT_FALSE(hookCalled);

            // sess 随作用域在当前 (io) 线程析构
        }
        // 绑定的 io 线程析构应触发 flushPendingViewOps, 回放未落盘操作
        XX_TEST_EXPECT_TRUE(hookCalled);
    }

    // Case 2: 在非 io 线程析构 -> 安全丢弃, 禁止跨线程调用 hook
    {
        bool hookCalledOffThread = false;
        auto sess                = std::make_shared<agentxx::agent::Session>();
        sess->bindIoThread(); // 绑定到当前线程

        sess->setStoreHooks(agentxx::agent::SessionStoreHooks{
            .onAppendViewMessage =
                [&](const agentxx::agent::ViewMessage&, uint64_t, uint64_t) {
                    hookCalledOffThread = true;
                },
        });

        // 首次立即落库
        sess->appendViewMessage(makeMsg(agentxx::agent::ViewMessage::Role::User, "msg1"));
        hookCalledOffThread = false;
        // 节流窗口内第二条: 进入 pendingViewOps_
        sess->appendViewMessage(makeMsg(agentxx::agent::ViewMessage::Role::User, "msg2"));
        XX_TEST_EXPECT_FALSE(hookCalledOffThread);

        // 将 sess 移交给其他工作线程析构
        std::thread workerThread([s = std::move(sess)]() mutable {
            XX_TEST_EXPECT_FALSE(s->isIoThread());
            // s 在 workerThread 析构: 触发 Session::~Session()
            s.reset();
        });
        workerThread.join();

        // 非 io 线程析构不应调用 hook
        XX_TEST_EXPECT_FALSE(hookCalledOffThread);
    }
}

// ---------------------------------------------------------------------------
// SessionStore 连接缓存 LRU 淘汰
// ---------------------------------------------------------------------------

/// 当前进程打开的 fd 数量 (仅 Linux/Android 可统计; 其他平台返回 0 跳过断言)
static size_t countOpenFileDescriptors() {
#if XX_IS_LINUX_D || XX_IS_ANDROID_D
    std::error_code ec;
    size_t          n = 0;
    for (auto it = fs::directory_iterator("/proc/self/fd", ec);
         false == static_cast<bool>(ec) && it != fs::directory_iterator();
         it.increment(ec)) {
        ++n;
    }
    return n;
#else
    return 0;
#endif
}

static TestResult testStoreConnectionLruEviction() {
    using agentxx::agent::SessionStore;
    using V = agentxx::agent::ViewMessage;

    auto      root      = makeTempRoot();
    const int kSessions = 200; // 远大于连接上限 (32)

    auto fdBefore = countOpenFileDescriptors();
    {
        auto p = std::make_shared<SessionStore>(root);
        for (int i = 0; i < kSessions; ++i) {
            auto name = "lru_" + std::to_string(i);
            p->appendViewMessage(name, makeMsg(V::Role::User, "hello-" + std::to_string(i)), 1);
        }
        auto fdAfter = countOpenFileDescriptors();
        if (fdBefore > 0 && fdAfter > 0) {
            // 打开连接数受上限约束: fd 增量必须远小于"每会话一个连接"
            // (未淘汰时每会话至少 1~3 个 fd, 200 个会话将增长数百)
            XX_TEST_EXPECT_TRUE(fdAfter - fdBefore < static_cast<size_t>(kSessions));
        }
        // 被淘汰的会话 (最早写入) 再次读取: 自动重开连接且数据仍在
        auto reloaded = p->loadSession("lru_0");
        XX_TEST_EXPECT_EQ(reloaded.viewMessages.size(), size_t{1});
        if (reloaded.viewMessages.size() == 1) {
            XX_TEST_EXPECT_EQ(reloaded.viewMessages[0].role, V::Role::User);
        }
    }
    // 新实例逐个读回: 全部会话数据完整 (无丢失/错位)
    {
        auto p = std::make_shared<SessionStore>(root);
        for (int i = 0; i < kSessions; ++i) {
            auto name = "lru_" + std::to_string(i);
            // 顺序访问会自动触发反复的打开/淘汰 (每次只保留最近使用的连接)
            auto loaded = p->loadSession(name);
            XX_TEST_EXPECT_EQ(loaded.viewMessages.size(), size_t{1});
        }
    }

    removeTempRoot(root);
    return TestResult{};
}

// ---------------------------------------------------------------------------
// view_message.msg_id 列迁移:
// - 老库 (只有 seq/json 两列) 首次经 SessionStore 打开时: ALTER 增加 msg_id
//   列 → 从 json 回填 → 建索引, 原有数据与 update 语义不受影响
// - 幂等: 再次打开不再 ALTER
// ---------------------------------------------------------------------------
static TestResult testViewMessageMsgIdMigration() {
    using agentxx::agent::SessionStore;
    using agentxx::util::SqliteDb;
    using V = agentxx::agent::ViewMessage;

    auto root = makeTempRoot();
    auto sid  = std::string{"legacy"};
    auto dir  = fs::path(root) / SessionStore::sanitizeSessionId(sid);
    fs::create_directories(dir);

    // 1) 用"老 schema"建库并写入两行 (无 msg_id 列)
    {
        SqliteDb db;
        db.open((dir / "session.db").string());
        db.exec(
            "CREATE TABLE view_message (seq INTEGER PRIMARY KEY AUTOINCREMENT, json TEXT NOT NULL)"
        );
        db.exec(
            "CREATE TABLE llm_context (id INTEGER PRIMARY KEY CHECK (id = 1), json TEXT NOT NULL)"
        );
        db.exec("CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)");
        db.exec("CREATE TABLE store (id INTEGER PRIMARY KEY, value TEXT NOT NULL)");

        auto m0 = makeMsg(V::Role::User, "legacy hello");
        m0.id   = "msg_000001";
        auto m1 = makeMsg(V::Role::Assistant, "legacy reply");
        m1.id   = "msg_000002";
        for (const auto& m : {m0, m1}) {
            auto st = db.prepare("INSERT INTO view_message(json) VALUES (?)");
            st.bindText(1, m.toJson().dump());
            st.step();
        }
        auto meta = db.prepare("INSERT INTO meta(key, value) VALUES (?, ?)");
        meta.bindText(1, "msgIdCounter");
        meta.bindInt64(2, 2);
        meta.step();
    }

    // 2) 经 SessionStore 打开 (触发迁移) 并读回历史
    {
        auto p      = std::make_shared<SessionStore>(root);
        auto loaded = p->loadSession(sid);
        XX_TEST_EXPECT_EQ(loaded.viewMessages.size(), size_t{2});
        if (loaded.viewMessages.size() == 2) {
            XX_TEST_EXPECT_EQ(loaded.viewMessages[0].id, std::string{"msg_000001"});
            XX_TEST_EXPECT_EQ(loaded.viewMessages[1].id, std::string{"msg_000002"});

            // 3) 更新历史行: 走 msg_id 索引路径 (迁移已回填 msg_id)
            auto updated = loaded.viewMessages[0];
            updated.text = "legacy updated";
            p->updateViewMessage(sid, updated);
        }
    }

    // 4) 校验迁移结果: msg_id 已回填 + 内容已更新 + 索引存在
    {
        SqliteDb db;
        db.open((dir / "session.db").string());
        {
            auto st = db.prepare("SELECT msg_id FROM view_message WHERE seq = 1");
            XX_TEST_EXPECT_TRUE(st.step());
            XX_TEST_EXPECT_EQ(st.columnText(0), std::string{"msg_000001"});
        }
        {
            auto st = db.prepare("SELECT json FROM view_message WHERE seq = 1");
            XX_TEST_EXPECT_TRUE(st.step());
            XX_TEST_EXPECT_TRUE(st.columnText(0).find("legacy updated") != std::string::npos);
        }
        {
            auto st = db.prepare("SELECT name FROM sqlite_master WHERE type = 'index' "
                                 "AND name = 'idx_view_message_msg_id'");
            XX_TEST_EXPECT_TRUE(st.step());
        }
    }

    // 5) 再次打开 (已迁移): 幂等, 数据不变
    {
        auto p       = std::make_shared<SessionStore>(root);
        auto loaded2 = p->loadSession(sid);
        XX_TEST_EXPECT_EQ(loaded2.viewMessages.size(), size_t{2});
        if (loaded2.viewMessages.size() == 2) {
            XX_TEST_EXPECT_TRUE(
                loaded2.viewMessages[0].text.find("legacy updated") != std::string::npos
            );
        }
    }

    removeTempRoot(root);
    return TestResult{};
}

/// 待落盘队列按下标回放 (不持有消息副本):
/// - 节流窗口内对同一消息的多次更新, 回放时写入"当下内容" (与逐条深拷贝的
///   最终落库结果一致), 且 append 先于其 update 入队
/// - Session::restore 整体替换 viewMessages 时丢弃队列 (旧下标失效), 之后
///   不会再回放陈旧操作
// ---------------------------------------------------------------------------
// 展示历史按页读取 / 只读尾窗加载
// - 页区间 `[startIndex, startIndex + count)` 对应 seq `(startIndex, startIndex+count]`
// - 只读尾窗时总数/窗口基准/msgIdCounter 仍要正确 (行数不等于总条数)
// ---------------------------------------------------------------------------
static TestResult testViewMessagesPageRead() {
    using V = agentxx::agent::ViewMessage;
    using agentxx::agent::SessionStore;

    auto root = makeTempRoot();
    {
        auto store = std::make_shared<SessionStore>(root);
        for (int i = 0; i < 12; ++i) {
            store->appendViewMessage(
                "pg",
                V::makeText(V::Role::User, fmt::format("p{}", i)),
                static_cast<uint64_t>(i + 1),
                static_cast<uint64_t>(i + 1)
            );
        }

        // ---- 按页: 左开右闭, 恰好在同一绝对下标空间 ----
        auto page = store->loadViewMessagesPage("pg", 4, 3);
        XX_TEST_EXPECT_EQ(page.size(), size_t{3});
        if (page.size() == 3) {
            XX_TEST_EXPECT_EQ(page[0].text, std::string{"p4"});
            XX_TEST_EXPECT_EQ(page[1].text, std::string{"p5"});
            XX_TEST_EXPECT_EQ(page[2].text, std::string{"p6"});
        }
        // 页尾到顶 (超出现有行) 时只返回实际存在的条数
        auto tailPage = store->loadViewMessagesPage("pg", 10, 100);
        XX_TEST_EXPECT_EQ(tailPage.size(), size_t{2});
        // 超出范围: 空 (不是错误)
        XX_TEST_EXPECT_TRUE(store->loadViewMessagesPage("pg", 12, 10).empty());
        XX_TEST_EXPECT_TRUE(store->loadViewMessagesPage("pg", 0, 0).empty());

        // ---- 只读尾窗 ----
        auto tail = store->loadSession("pg", 5);
        XX_TEST_EXPECT_EQ(tail.viewMessages.size(), size_t{5});
        XX_TEST_EXPECT_EQ(tail.viewWindowBase, uint64_t{7});
        XX_TEST_EXPECT_EQ(tail.lastViewSeq, uint64_t{12});
        XX_TEST_EXPECT_EQ(tail.msgIdCounter, uint64_t{12});
        if (tail.viewMessages.size() == 5) {
            XX_TEST_EXPECT_EQ(tail.viewMessages.front().text, std::string{"p7"});
            XX_TEST_EXPECT_EQ(tail.viewMessages.back().text, std::string{"p11"});
        }
        // 尾窗大于历史: 退化为全量 (base = 0)
        auto over = store->loadSession("pg", 100);
        XX_TEST_EXPECT_EQ(over.viewMessages.size(), size_t{12});
        XX_TEST_EXPECT_EQ(over.viewWindowBase, uint64_t{0});

        // ---- 全量加载 (`tailCount = 0`, 诊断/测试用) ----
        auto all = store->loadSession("pg");
        XX_TEST_EXPECT_EQ(all.viewMessages.size(), size_t{12});
        XX_TEST_EXPECT_EQ(all.viewWindowBase, uint64_t{0});
    }
    removeTempRoot(root);
    return TestResult{};
}

// ---------------------------------------------------------------------------
// 展示历史窗口化: 只常驻尾部窗口 + 更早的页回库读
// - 绝对下标 absIndex = seq - 1 (view_message 不删行、seq 单调)
// - 淘汰只在待落盘队列为空时发生 (队列按下标引用内存)
// - 取段内存优先, 缺口回库; 内存里的行比库里新 (tool 回填)
// ---------------------------------------------------------------------------
static void testViewWindowPaging();

static TestResult testPendingViewOpsIndexReplay() {
    using V = agentxx::agent::ViewMessage;

    // Case 1: 窗口内 append + 两次 update 同一消息 → 回放写的都是最新内容
    {
        auto sess = std::make_shared<agentxx::agent::Session>();
        sess->bindIoThread();

        std::vector<std::pair<bool, std::string>> replayed; // (isAppend, text)
        sess->setStoreHooks(agentxx::agent::SessionStoreHooks{
            .onAppendViewMessage =
                [&](const V& m, uint64_t, uint64_t) {
                    replayed.emplace_back(true, m.text);
                },
            .onUpdateViewMessage =
                [&](const V& m) {
                    replayed.emplace_back(false, m.text);
                },
        });

        // 首次触发立即落库
        auto id1 = sess->appendViewMessage(makeMsg(V::Role::User, "first"));
        XX_TEST_EXPECT_EQ(replayed.size(), size_t{1});
        XX_TEST_EXPECT_TRUE(replayed[0].first);
        XX_TEST_EXPECT_EQ(replayed[0].second, std::string{"first"});

        // 节流窗口内: 追加第二条 + 两次更新 (均进入队列, 不立即落库)
        auto        id2 = sess->appendViewMessage(makeMsg(V::Role::Tool, "tool-running"));
        const auto* m2  = [&]() -> const V* {
            for (const auto& m : sess->viewWindow()) {
                if (m.id == id2) {
                    return &m;
                }
            }
            return nullptr;
        }();
        XX_TEST_EXPECT_TRUE(m2 != nullptr);
        auto updated = m2 ? *m2 : makeMsg(V::Role::Tool, "");
        updated.text = "tool-step1";
        sess->updateViewMessage(updated);
        updated.text = "tool-done";
        sess->updateViewMessage(updated);
        XX_TEST_EXPECT_EQ(replayed.size(), size_t{1}); // 仍只有首次那一条

        // 轮末补存: 回放队列 (append + 一次 update)。同一行的连续覆盖只留一条
        // (回放时读的是行的当下内容, 中间状态没有落库价值; 流式文本反复追加到
        //  同一行时不会把队列撞大)
        sess->flushViewMessages();
        XX_TEST_EXPECT_EQ(replayed.size(), size_t{3});
        if (replayed.size() == 3) {
            XX_TEST_EXPECT_TRUE(replayed[1].first);
            XX_TEST_EXPECT_EQ(replayed[1].second, std::string{"tool-done"});
            XX_TEST_EXPECT_FALSE(replayed[2].first);
            XX_TEST_EXPECT_EQ(replayed[2].second, std::string{"tool-done"});
        }
        XX_TEST_EXPECT_EQ(id1.empty(), false);
    }

    // Case 1b: 不同行各自的覆盖不会被合并 (去重只按下标)
    {
        auto sess = std::make_shared<agentxx::agent::Session>();
        sess->bindIoThread();

        int updateCalls = 0;
        sess->setStoreHooks(agentxx::agent::SessionStoreHooks{
            .onAppendViewMessage = [&](const V&, uint64_t, uint64_t) {},
            .onUpdateViewMessage = [&](const V&) { ++updateCalls; },
        });
        auto idA = sess->appendViewMessage(makeMsg(V::Role::Tool, "a"));
        auto idB = sess->appendViewMessage(makeMsg(V::Role::Tool, "b"));
        for (const auto& id : {idA, idB, idA, idB}) {
            for (const auto& m : sess->viewWindow()) {
                if (m.id == id) {
                    auto copy = m;
                    sess->updateViewMessage(copy);
                    break;
                }
            }
        }
        sess->flushViewMessages();
        XX_TEST_EXPECT_EQ(updateCalls, 2); // 每条行各回放一次
    }

    // Case 2: restore 丢弃队列 → 不落库陈旧操作
    {
        auto sess = std::make_shared<agentxx::agent::Session>();
        sess->bindIoThread();

        int appendCalls = 0;
        int updateCalls = 0;
        sess->setStoreHooks(agentxx::agent::SessionStoreHooks{
            .onAppendViewMessage =
                [&](const V&, uint64_t, uint64_t) {
                    ++appendCalls;
                },
            .onUpdateViewMessage =
                [&](const V&) {
                    ++updateCalls;
                },
        });

        sess->appendViewMessage(makeMsg(V::Role::User, "first")); // 立即落库
        sess->appendViewMessage(makeMsg(V::Role::User, "queued"));
        XX_TEST_EXPECT_EQ(appendCalls, 1);

        // 还原完整历史 (模拟会话重载): 队列被丢弃
        sess->restore({makeMsg(V::Role::User, "restored")}, 1);
        sess->flushViewMessages();
        XX_TEST_EXPECT_EQ(appendCalls, 1); // 未回放被丢弃的排队操作
        XX_TEST_EXPECT_EQ(updateCalls, 0);
        XX_TEST_EXPECT_EQ(sess->viewWindow().size(), size_t{1});
    }

    return TestResult{};
}

/// 展示历史窗口化 (会话 + 存储层)
/// - 超过上限后只常驻尾部: 总数不变、旧下标不可取、msgId 索引只留窗口内
/// - 待落盘队列非空时不淘汰 (否则队列里的行就没了), flush 之后才删
/// - 跨窗口边界取段能拼出完整序列, 且内存里的行覆盖库内旧版本
/// - 已淘汰的行被更新时只写库, 不改内存
/// - 重启后 base/总数/msgIdCounter 与重启前一致, 继续分页仍正确
/// - 内存模式 (无会话库) 不淘汰; 库不可读时取段明确失败 (调用方回空页降级)
static void testViewWindowPaging() {
    using V = agentxx::agent::ViewMessage;
    using agentxx::agent::SessionStore;

    // ---- Case 1: 超过上限后只常驻尾部窗口 ----
    {
        auto root             = makeTempRoot();
        auto store            = std::make_shared<SessionStore>(root);
        auto mgr              = std::make_shared<agentxx::agent::SessionsManager>();
        mgr->sessionStore     = store;
        mgr->viewWindowMessages = 10;
        auto sess             = mgr->getOrCreate("win-cap");

        for (int i = 0; i < 40; ++i) {
            sess->appendViewMessage(V::makeText(V::Role::User, fmt::format("m{}", i)));
        }
        XX_TEST_EXPECT_EQ(sess->viewMessageCount(), size_t{40});
        // 待落盘队列非空 → 淘汰被推迟 (节流窗口内只首批落库)
        XX_TEST_EXPECT_TRUE(sess->viewWindow().size() > size_t{10});
        sess->flushViewMessages();

        // 成批淘汰 (至少 cap/4 条): 窗口不一定刚好等于 cap, 但一定不超
        XX_TEST_EXPECT_TRUE(sess->viewWindow().size() <= size_t{10});
        XX_TEST_EXPECT_EQ(
            sess->viewWindowBase() + sess->viewWindow().size(),
            size_t{40}
        );
        XX_TEST_EXPECT_EQ(sess->viewWindowCap(), size_t{10});

        // 旧下标不在内存: 按下标/按 id 都取不到
        XX_TEST_EXPECT_TRUE(sess->viewMessageAt(0) == nullptr);
        XX_TEST_EXPECT_TRUE(sess->viewMessageById("msg_000001") == nullptr);
        // 窗口内仍可直接取
        const auto* last = sess->viewMessageAt(39);
        XX_TEST_EXPECT_TRUE(last != nullptr);
        if (last != nullptr) {
            XX_TEST_EXPECT_EQ(last->text, std::string{"m39"});
        }
        XX_TEST_EXPECT_TRUE(sess->viewMessageById("msg_000040") != nullptr);

        // ---- 跨窗口边界取段: 前半段回库, 后半段来自内存 ----
        std::vector<V> range;
        const auto     base = static_cast<uint64_t>(sess->viewWindowBase());
        XX_TEST_EXPECT_TRUE(sess->collectViewRange(base - 5, base + 5, range));
        XX_TEST_EXPECT_EQ(range.size(), size_t{10});
        if (range.size() == 10) {
            for (size_t i = 0; i < range.size(); ++i) {
                XX_TEST_EXPECT_EQ(range[i].text, fmt::format("m{}", base - 5 + i));
                XX_TEST_EXPECT_EQ(range[i].id, fmt::format("msg_{:06d}", base - 5 + i + 1));
            }
        }

        // 整段历史都能取回 (完整同步走的就是这条路径)
        std::vector<V> all;
        XX_TEST_EXPECT_TRUE(sess->collectViewRange(0, 40, all));
        XX_TEST_EXPECT_EQ(all.size(), size_t{40});
        if (all.size() == 40) {
            XX_TEST_EXPECT_EQ(all.front().text, std::string{"m0"});
            XX_TEST_EXPECT_EQ(all.back().text, std::string{"m39"});
        }

        // ---- 已淘汰的行被更新: 只写库, 不改内存 ----
        V oldRow = V::makeText(V::Role::Tool, "old-result");
        oldRow.id = "msg_000002";
        sess->updateViewMessage(oldRow);
        std::vector<V> head;
        XX_TEST_EXPECT_TRUE(sess->collectViewRange(1, 2, head));
        XX_TEST_EXPECT_EQ(head.size(), size_t{1});
        if (head.size() == 1) {
            XX_TEST_EXPECT_EQ(head[0].text, std::string{"old-result"});
            XX_TEST_EXPECT_TRUE(head[0].role == V::Role::Tool);
        }

        // ---- 重启: base / 总数 / msgIdCounter 一致 ----
        auto mgr2              = std::make_shared<agentxx::agent::SessionsManager>();
        mgr2->sessionStore     = std::make_shared<SessionStore>(root);
        mgr2->viewWindowMessages = 10;
        auto sess2             = mgr2->getOrCreate("win-cap");
        XX_TEST_EXPECT_EQ(sess2->viewMessageCount(), size_t{40});
        XX_TEST_EXPECT_EQ(sess2->viewWindowBase(), size_t{30});
        XX_TEST_EXPECT_EQ(sess2->viewWindow().size(), size_t{10});
        XX_TEST_EXPECT_EQ(sess2->viewWindow().front().text, std::string{"m30"});
        // id 计数延续 (新消息不冲突)
        XX_TEST_EXPECT_EQ(
            sess2->appendViewMessage(V::makeText(V::Role::User, "m40")),
            std::string{"msg_000041"}
        );
        XX_TEST_EXPECT_EQ(sess2->viewMessageCount(), size_t{41});
        store.reset();
        mgr.reset();
        mgr2.reset();
        removeTempRoot(root);
    }

    // ---- Case 2: 连续追加 3×cap 不返回控制权: 全部落库, 一条不丢 ----
    {
        auto root           = makeTempRoot();
        auto store          = std::make_shared<SessionStore>(root);
        auto mgr            = std::make_shared<agentxx::agent::SessionsManager>();
        mgr->sessionStore   = store;
        mgr->viewWindowMessages = 8;
        auto sess           = mgr->getOrCreate("win-burst");

        for (int i = 0; i < 24; ++i) {
            sess->appendViewMessage(V::makeText(V::Role::User, fmt::format("b{}", i)));
        }
        // 淘汰没跟上 (队列非空), 但总量正确
        XX_TEST_EXPECT_EQ(sess->viewMessageCount(), size_t{24});
        sess->flushViewMessages();
        XX_TEST_EXPECT_TRUE(sess->viewWindow().size() <= size_t{8});

        // 库里必须是完整 24 行 (没有因淘汰丢消息)
        auto all = std::make_shared<SessionStore>(root)->loadSession("win-burst");
        XX_TEST_EXPECT_EQ(all.viewMessages.size(), size_t{24});
        XX_TEST_EXPECT_EQ(all.lastViewSeq, uint64_t{24});
        if (all.viewMessages.size() == 24) {
            XX_TEST_EXPECT_EQ(all.viewMessages.front().text, std::string{"b0"});
            XX_TEST_EXPECT_EQ(all.viewMessages.back().text, std::string{"b23"});
        }
        store.reset();
        mgr.reset();
        removeTempRoot(root);
    }

    // ---- Case 3: 内存模式 (无会话库) 永不淘汰 ----
    {
        auto mgr            = std::make_shared<agentxx::agent::SessionsManager>();
        mgr->viewWindowMessages = 4;
        auto sess           = mgr->getOrCreate("win-memory");
        for (int i = 0; i < 20; ++i) {
            sess->appendViewMessage(V::makeText(V::Role::User, fmt::format("n{}", i)));
        }
        XX_TEST_EXPECT_EQ(sess->viewWindowCap(), size_t{0});
        XX_TEST_EXPECT_EQ(sess->viewWindow().size(), size_t{20});
        XX_TEST_EXPECT_EQ(sess->viewWindowBase(), size_t{0});
    }

    // ---- Case 4: 内存窗口装不下、又读不到库时取段明确失败 ----
    // 场景: 会话有库但读页拿不到完整区间 (库不可读/页被删/短页), 调用方要回空页
    // 降级而不是把不连续的内容当合法页返回
    {
        auto sess = std::make_shared<agentxx::agent::Session>();
        sess->bindIoThread();
        sess->setViewWindowCap(4);
        for (int i = 0; i < 12; ++i) {
            sess->appendViewMessage(V::makeText(V::Role::User, fmt::format("d{}", i)));
        }
        // 没有绑定会话库 (无 onLoadViewPage): 淘汰照做, 但更早的行就取不回来了
        XX_TEST_EXPECT_TRUE(sess->viewWindowBase() > size_t{0});
        XX_TEST_EXPECT_TRUE(sess->viewWindow().size() <= size_t{4});
        XX_TEST_EXPECT_EQ(sess->viewMessageCount(), size_t{12});

        std::vector<V> gap;
        XX_TEST_EXPECT_FALSE(sess->collectViewRange(0, 12, gap));
        XX_TEST_EXPECT_TRUE(gap.empty());
        // 窗口内的区间仍可取 (尾窗同步/追加快照不需要库)
        std::vector<V> tailOnly;
        XX_TEST_EXPECT_TRUE(sess->collectViewRange(sess->viewWindowBase(), 12, tailOnly));
        XX_TEST_EXPECT_EQ(tailOnly.size(), sess->viewWindow().size());
    }
}

/// 恢复历史时工具行参数按注入的回调自愈 (BaseAgent 注入的 normalizer)
/// - 尾窗恢复与分页读取两条路径都要生效: 老会话库里存的是模型原文
/// - 未注入回调 / 工具不匹配时行保持原样
static TestResult testToolViewArgsNormalizeOnLoad() {
    using agentxx::agent::SessionStore;
    using agentxx::agent::SessionsManager;
    using agentxx::agent::ViewMessage;
    using V = ViewMessage;

    // 模拟 grep 的参数 schema (file_patterns 是字符串数组, max_files 是整数)
    const auto grepParams = utilxx_base::Json{
        {"type",       "object"                                                    },
        {"properties",
         utilxx_base::Json{
             {"file_patterns",
              utilxx_base::Json{
                  {"type", "array"},
                  {"items", utilxx_base::Json{{"type", "string"}}},
              }},
             {"max_files", utilxx_base::Json{{"type", "integer"}}},
         }                                                                          },
    };
    const std::string rawArgs = R"({"file_patterns":"*.cpp","max_files":"5"})";
    const std::string fixedArgs = R"({"file_patterns":["*.cpp"],"max_files":5})";

    auto root = makeTempRoot();
    {
        // ---- 第一次"运行": 库里写的是模型原文的工具行 ----
        auto sessionStore   = std::make_shared<SessionStore>(root);
        auto mgr            = std::make_shared<SessionsManager>();
        mgr->sessionStore   = sessionStore;
        auto s1             = mgr->getOrCreate("thread-args");
        s1->appendViewMessage(V::makeText(V::Role::User, "u1"));
        V toolMsg;
        toolMsg.role      = V::Role::Tool;
        toolMsg.text      = rawArgs;
        toolMsg.collapsed = true;
        V::ToolData t;
        t.toolName     = "grep";
        t.toolCallId   = "call_args";
        t.toolFinished = true;
        toolMsg.tool   = std::move(t);
        s1->appendViewMessage(std::move(toolMsg));
        s1->flushViewMessages();
    }
    {
        // ---- 第二次"运行": 注入 normalizer, 尾窗恢复要自愈 ----
        auto sessionStore = std::make_shared<SessionStore>(root);
        auto mgr          = std::make_shared<SessionsManager>();
        mgr->sessionStore = sessionStore;
        mgr->toolArgsNormalizer
            = [grepParams](std::string_view toolName, std::string_view argsJson) {
                  if (toolName != "grep") {
                      return std::string{argsJson};
                  }
                  return agentxx::util::autoFixedToolArgsJson(grepParams, argsJson);
              };
        auto s = mgr->getOrCreate("thread-args");
        XX_TEST_EXPECT_EQ(s->viewWindow().size(), size_t{2});
        if (s->viewWindow().size() == 2) {
            XX_TEST_EXPECT_EQ(s->viewWindow()[1].text, fixedArgs);
        }
    }
    {
        // ---- 第三次"运行": 尾窗只留 1 条, 分页读出的更早行同样自愈 ----
        auto sessionStore       = std::make_shared<SessionStore>(root);
        auto mgr                = std::make_shared<SessionsManager>();
        mgr->sessionStore       = sessionStore;
        mgr->viewWindowMessages = 1;
        mgr->toolArgsNormalizer
            = [grepParams](std::string_view toolName, std::string_view argsJson) {
                  if (toolName != "grep") {
                      return std::string{argsJson};
                  }
                  return agentxx::util::autoFixedToolArgsJson(grepParams, argsJson);
              };
        auto s = mgr->getOrCreate("thread-args");
        // 内存窗口只有最后一条 (工具行被淘汰), 取全区间会走 onLoadViewPage
        XX_TEST_EXPECT_EQ(s->viewWindow().size(), size_t{1});
        std::vector<V> all;
        XX_TEST_EXPECT_TRUE(s->collectViewRange(0, 2, all));
        XX_TEST_EXPECT_EQ(all.size(), size_t{2});
        if (all.size() == 2) {
            XX_TEST_EXPECT_EQ(all[1].role, V::Role::Tool);
            XX_TEST_EXPECT_EQ(all[1].text, fixedArgs);
        }
    }
    {
        // ---- 未注入 normalizer: 恢复的历史保持原文 (不改变现有行为) ----
        auto sessionStore = std::make_shared<SessionStore>(root);
        auto mgr          = std::make_shared<SessionsManager>();
        mgr->sessionStore = sessionStore;
        auto s            = mgr->getOrCreate("thread-args");
        XX_TEST_EXPECT_EQ(s->viewWindow().size(), size_t{2});
        if (s->viewWindow().size() == 2) {
            XX_TEST_EXPECT_EQ(s->viewWindow()[1].text, rawArgs);
        }
    }
    removeTempRoot(root);
    return TestResult{};
}

asio::awaitable<TestResult> run_session_persistence_tests() {
    g_sp_passed = 0;
    g_sp_failed = 0;

    testViewMessagesRoundtrip();
    testViewMessagesPageRead();
    testLlmMessagesRoundtrip();
    testLlmMessagesTextMirror();
    testShareStoreRoundtrip();
    testUpdateHistoryPersistence();
    testSessionStoreIntegration();
    testPersistThrottle();
    testMiddlewareShareStorePersistence();
    testSanitizeThreadId();
    testSessionListPagination();
    testSessionDestructorThreadSafety();
    testPersistenceResilience();
    testStoreConnectionLruEviction();
    testViewMessageMsgIdMigration();
    testPendingViewOpsIndexReplay();
    testViewWindowPaging();
    testToolViewArgsNormalizeOnLoad();

    // E2E 需要独立 io_context (BaseAgent 内部有自身的 io 循环)
    asio::io_context io;
    asio::co_spawn(
        io,
        keepAliveRun([&]() -> asio::awaitable<void> {
            co_await testSessionPersistenceE2E();
            co_await testTurnEndTipPersistenceRoundtrip();
        }),
        asio::detached
    );
    io.run();

    co_return TestResult{g_sp_passed, g_sp_failed};
}

} // namespace test
} // namespace agentxx
