///
/// 覆盖 (展示历史副本 = agent 端下发增量的唯一接收方):
/// - 消息级 op 语义: `insert_message` / `update_message` 都是 by-id upsert
///   (未知 id 的 insert 追加、已知 id 的 insert 等价整条替换 —— 重连重放幂等;
///   未知 id 的 update 忽略 —— 多半是本地条数上限清掉的老行)
/// - 流式项: `stream_begin` 定角色/思考元数据, token 按 id 追加正文/思考;
///   `stream_end` 丢弃; 定稿行 (同 id 的 insert) 同时把流式项收掉
/// - 不可见的行 (只有空白/零宽占位符) 一律不进来
/// - `reset` 整体替换 (保留副本里已有的本地行 + 灌入 `payload.streaming`)、
///   `appendIncremental` 只追加去重、`prependRows` 前插去重、`trimHead` 丢头部
/// - 本地行 (`local_` 前缀): 只接受本地 id, agent 的 op 永不命中它们
/// - 行对象改写: `replaceRow` / `setRowCollapsed` 每次换新对象 (渲染缓存以指针失效)
#include "agentxx-test/core/test_view_replica.h"

#include "agentxx/agent/io/view_replica.h"
#include <memory>
#include <string>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_vr_passed = 0;
int g_vr_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_vr_passed
#define XX_TEST_FAILED g_vr_failed

namespace agentxx {
namespace test {

namespace {

using agentxx::agent::ViewMessage;
using agentxx::agent::ViewReplica;
using agentxx::agent::WireDelta;
using agentxx::agent::WireSyncPayload;
using Role = ViewMessage::Role;

namespace {

/// 一条普通文本行
ViewMessage textRow(std::string id, Role role, std::string text) {
    auto m  = ViewMessage::makeText(role, std::move(text));
    m.id    = std::move(id);
    return m;
}

/// insert / update 增量 (message 必须带 id)
WireDelta rowDelta(WireDelta::Type type, ViewMessage row) {
    WireDelta d;
    d.type    = type;
    d.msgId   = row.id;
    d.message = std::make_shared<ViewMessage>(std::move(row));
    return d;
}

/// 行集合的 id 序列 (`a,b,c`; 便于一条断言看清顺序)
std::string idsOf(const ViewReplica& replica) {
    std::string out;
    for (const auto& row : replica.rows()) {
        if (!out.empty()) {
            out += ",";
        }
        out += row->id;
    }
    return out;
}

} // namespace

} // namespace

TestResult testViewReplica() {
    g_vr_passed = 0;
    g_vr_failed = 0;

    // ---- 1. insert_message: 新 id 追加, 已存在的 id 整条替换 (重放幂等) ----
    {
        ViewReplica r;
        XX_TEST_EXPECT_TRUE(r.rows().empty());
        XX_TEST_EXPECT_FALSE(r.hasStream());
        XX_TEST_EXPECT_TRUE(r.applyDelta(rowDelta(WireDelta::Type::InsertMessage, textRow("m1", Role::User, "hi"))));
        XX_TEST_EXPECT_EQ(idsOf(r), std::string{"m1"});
        XX_TEST_EXPECT_TRUE(r.applyDelta(rowDelta(WireDelta::Type::InsertMessage, textRow("m2", Role::Assistant, "hello"))));
        XX_TEST_EXPECT_EQ(idsOf(r), std::string{"m1,m2"});
        // 同 id 重放: 不追加, 内容按整条替换
        XX_TEST_EXPECT_TRUE(r.applyDelta(rowDelta(WireDelta::Type::InsertMessage, textRow("m1", Role::User, "hi again"))));
        XX_TEST_EXPECT_EQ(idsOf(r), std::string{"m1,m2"});
        XX_TEST_EXPECT_EQ(r.rows()[0]->text, std::string{"hi again"});
        // 无 id / 空消息: 直接忽略
        XX_TEST_EXPECT_FALSE(r.applyDelta(rowDelta(WireDelta::Type::InsertMessage, textRow("", Role::User, "x"))));
        {
            WireDelta d;
            d.type = WireDelta::Type::InsertMessage;
            XX_TEST_EXPECT_FALSE(r.applyDelta(d));
        }
        XX_TEST_EXPECT_EQ(r.rows().size(), size_t{2});
    }

    // ---- 2. update_message: 未知 id 忽略 (行已被本地条数上限清掉), 已知 id 整条替换 ----
    {
        ViewReplica r;
        r.applyDelta(rowDelta(WireDelta::Type::InsertMessage, textRow("m1", Role::Tool, "{}")));
        XX_TEST_EXPECT_FALSE(r.applyDelta(rowDelta(WireDelta::Type::UpdateMessage, textRow("m404", Role::Tool, "{}"))));
        XX_TEST_EXPECT_EQ(r.rows().size(), size_t{1});
        XX_TEST_EXPECT_TRUE(r.applyDelta(rowDelta(WireDelta::Type::UpdateMessage, textRow("m1", Role::Tool, "done"))));
        XX_TEST_EXPECT_EQ(r.rows()[0]->text, std::string{"done"});
        XX_TEST_EXPECT_EQ(r.rows().size(), size_t{1});
    }

    // ---- 3. 不可见的行两路都不进来 (只有空白/零宽占位符) ----
    {
        ViewReplica r;
        XX_TEST_EXPECT_FALSE(r.applyDelta(rowDelta(WireDelta::Type::InsertMessage, textRow("m1", Role::Assistant, "\xE2\x80\x8B \n"))));
        XX_TEST_EXPECT_TRUE(r.rows().empty());
        // 带附件的消息恒为有内容 (附件卡片本身可见)
        auto m = ViewMessage::makeText(Role::User, "\xE2\x80\x8B");
        m.id   = "m2";
        m.attachments.push_back(agentxx::agent::MediaAttachment{});
        XX_TEST_EXPECT_TRUE(r.applyDelta(rowDelta(WireDelta::Type::InsertMessage, std::move(m))));
        XX_TEST_EXPECT_EQ(idsOf(r), std::string{"m2"});
    }

    // ---- 4. 流式项: stream_begin 定角色与元数据, token 逐段追加 ----
    {
        ViewReplica r;
        WireDelta   begin;
        begin.type       = WireDelta::Type::StreamBegin;
        begin.msgId      = "s1";
        begin.streamRole = Role::Think;
        begin.startTimeMs = 1000;
        begin.think      = ViewMessage::ThinkData{.reasoningTokens = 12, .isEncrypted = true};
        XX_TEST_EXPECT_TRUE(r.applyDelta(begin));
        XX_TEST_EXPECT_TRUE(r.hasStream());
        XX_TEST_EXPECT_EQ(r.stream().id, std::string{"s1"});
        XX_TEST_EXPECT_TRUE(r.stream().role == Role::Think);
        XX_TEST_EXPECT_TRUE(r.stream().text->empty());
        XX_TEST_EXPECT_TRUE(r.stream().think.has_value());
        // 同一个 stream_begin 重放: 集合没变化 -> 返回 false
        XX_TEST_EXPECT_FALSE(r.applyDelta(begin));

        WireDelta tok;
        tok.type  = WireDelta::Type::ThinkToken;
        tok.msgId = "s1";
        tok.text  = "abc";
        XX_TEST_EXPECT_TRUE(r.applyDelta(tok));
        tok.text = "def";
        XX_TEST_EXPECT_TRUE(r.applyDelta(tok));
        XX_TEST_EXPECT_EQ(*r.stream().text, std::string{"abcdef"});
        // 空文本 token 不算改动
        tok.text = "";
        XX_TEST_EXPECT_FALSE(r.applyDelta(tok));
        // 正文 token 打到同一条流上也只追加 (角色由 stream_begin 决定)
        WireDelta body;
        body.type  = WireDelta::Type::TextToken;
        body.msgId = "s1";
        body.text  = "gh";
        XX_TEST_EXPECT_TRUE(r.applyDelta(body));
        XX_TEST_EXPECT_EQ(*r.stream().text, std::string{"abcdefgh"});

        // stream_end: id 不符视为 no-op, 相符才丢弃
        WireDelta end;
        end.type  = WireDelta::Type::StreamEnd;
        end.msgId = "other";
        XX_TEST_EXPECT_FALSE(r.applyDelta(end));
        XX_TEST_EXPECT_TRUE(r.hasStream());
        end.msgId = "s1";
        XX_TEST_EXPECT_TRUE(r.applyDelta(end));
        XX_TEST_EXPECT_FALSE(r.hasStream());
        XX_TEST_EXPECT_TRUE(r.stream().text->empty());
    }

    // ---- 5. 定稿: 同 id 的 insert 收掉流式项并落成正式行 ----
    {
        ViewReplica r;
        WireDelta   begin;
        begin.type       = WireDelta::Type::StreamBegin;
        begin.msgId      = "s1";
        begin.streamRole = Role::Assistant;
        r.applyDelta(begin);
        WireDelta tok;
        tok.type  = WireDelta::Type::TextToken;
        tok.msgId = "s1";
        tok.text  = "half";
        r.applyDelta(tok);

        auto row = textRow("s1", Role::Assistant, "half");
        XX_TEST_EXPECT_TRUE(r.applyDelta(rowDelta(WireDelta::Type::InsertMessage, row)));
        XX_TEST_EXPECT_FALSE(r.hasStream());
        XX_TEST_EXPECT_EQ(idsOf(r), std::string{"s1"});
        XX_TEST_EXPECT_EQ(r.rows()[0]->text, std::string{"half"});
        // 定稿行之后的 token (迟到) 自建一条新流 (id 与已有行同名, 但流式项是独立的)
        XX_TEST_EXPECT_TRUE(r.applyDelta(tok));
        XX_TEST_EXPECT_TRUE(r.hasStream());
        XX_TEST_EXPECT_EQ(*r.stream().text, std::string{"half"});
        XX_TEST_EXPECT_EQ(r.rows().size(), size_t{1});
    }

    // ---- 6. token 先于 stream_begin 到达 (丢包): 按 token 自带 id 自建流 ----
    {
        ViewReplica r;
        WireDelta   tok;
        tok.type  = WireDelta::Type::ThinkToken;
        tok.msgId = "s9";
        tok.text  = "x";
        XX_TEST_EXPECT_TRUE(r.applyDelta(tok));
        XX_TEST_EXPECT_TRUE(r.hasStream());
        XX_TEST_EXPECT_EQ(r.stream().id, std::string{"s9"});
        XX_TEST_EXPECT_TRUE(r.stream().role == Role::Think); // 思考 token 自建为 Think
        // 无 id 的 token 无法定位 (协议违规): 忽略
        WireDelta bad;
        bad.type = WireDelta::Type::TextToken;
        bad.text = "y";
        XX_TEST_EXPECT_FALSE(r.applyDelta(bad));
        XX_TEST_EXPECT_EQ(*r.stream().text, std::string{"x"});
    }

    // ---- 7. reset: 整体替换 + 保留副本里已有的本地行 + 灌入直播槽 ----
    {
        ViewReplica r;
        r.applyDelta(rowDelta(WireDelta::Type::InsertMessage, textRow("m1", Role::User, "old")));
        ViewMessage form;
        form.id                     = "local_interrupt_7";
        form.role                   = Role::Interrupt;
        form.interrupt              = ViewMessage::InterruptData{};
        form.interrupt->interruptId = 7;
        XX_TEST_EXPECT_TRUE(r.upsertLocalRow(form));

        WireSyncPayload p;
        p.messages      = {textRow("m1", Role::User, "new"), textRow("m2", Role::Assistant, "a2")};
        p.totalMessages = p.messages.size();
        p.lastViewSeq   = 42;
        p.streaming     = textRow("s5", Role::Assistant, "streaming half");
        r.reset(p);

        XX_TEST_EXPECT_EQ(idsOf(r), std::string{"m1,m2,local_interrupt_7"});
        XX_TEST_EXPECT_EQ(r.rows()[0]->text, std::string{"new"});
        XX_TEST_EXPECT_EQ(r.lastViewSeq(), uint64_t{42});
        XX_TEST_EXPECT_TRUE(r.hasStream());
        XX_TEST_EXPECT_EQ(r.stream().id, std::string{"s5"});
        XX_TEST_EXPECT_EQ(*r.stream().text, std::string{"streaming half"});

        // 没有流式段的整体替换: 上一份的直播槽被清掉
        WireSyncPayload p2;
        p2.messages  = {textRow("m1", Role::User, "new")};
        p2.lastViewSeq = 50;
        r.reset(p2);
        XX_TEST_EXPECT_FALSE(r.hasStream());
        XX_TEST_EXPECT_EQ(idsOf(r), std::string{"m1,local_interrupt_7"});
        XX_TEST_EXPECT_EQ(r.lastViewSeq(), uint64_t{50});
    }

    // ---- 8. appendIncremental: 只追加未知 id, 不动直播槽与本地行 ----
    {
        ViewReplica r;
        r.applyDelta(rowDelta(WireDelta::Type::InsertMessage, textRow("m1", Role::User, "a")));
        WireDelta begin;
        begin.type       = WireDelta::Type::StreamBegin;
        begin.msgId      = "s1";
        begin.streamRole = Role::Assistant;
        r.applyDelta(begin);

        WireSyncPayload p;
        p.messages    = {textRow("m1", Role::User, "a"), textRow("m2", Role::Assistant, "b")};
        p.lastViewSeq = 7;
        XX_TEST_EXPECT_TRUE(r.appendIncremental(p));
        XX_TEST_EXPECT_EQ(idsOf(r), std::string{"m1,m2"});
        XX_TEST_EXPECT_TRUE(r.hasStream()); // 直播槽不受增量补拉影响
        XX_TEST_EXPECT_EQ(r.lastViewSeq(), uint64_t{7});
        // 全部已存在: 没有改动, 序号不倒退
        WireSyncPayload p2;
        p2.messages    = {textRow("m2", Role::Assistant, "b")};
        p2.lastViewSeq = 3;
        XX_TEST_EXPECT_FALSE(r.appendIncremental(p2));
        XX_TEST_EXPECT_EQ(r.lastViewSeq(), uint64_t{7});
    }

    // ---- 9. prependRows: 前插一页更早的历史 (按 id 去重, 保持页内顺序) ----
    {
        ViewReplica r;
        r.applyDelta(rowDelta(WireDelta::Type::InsertMessage, textRow("m3", Role::User, "c")));
        XX_TEST_EXPECT_TRUE(r.prependRows({
            textRow("m1", Role::User, "a"),
            textRow("m2", Role::Assistant, "b"),
        }));
        XX_TEST_EXPECT_EQ(idsOf(r), std::string{"m1,m2,m3"});
        // 页内有重复 / 已存在的 id: 只加新的
        XX_TEST_EXPECT_TRUE(r.prependRows({textRow("m0", Role::User, "z")}));
        XX_TEST_EXPECT_EQ(idsOf(r), std::string{"m0,m1,m2,m3"});
        XX_TEST_EXPECT_FALSE(r.prependRows({textRow("m1", Role::User, "a")}));
        XX_TEST_EXPECT_EQ(idsOf(r), std::string{"m0,m1,m2,m3"});
        // 前插后 id → 下标索引仍可用
        XX_TEST_EXPECT_TRUE(r.rowById("m1") != nullptr);
        XX_TEST_EXPECT_EQ(r.rowById("m1")->text, std::string{"a"});
    }

    // ---- 10. trimHead: 丢头部, 返回实际丢弃条数, 索引同步 ----
    {
        ViewReplica r;
        for (int i = 0; i < 5; ++i) {
            r.applyDelta(rowDelta(
                WireDelta::Type::InsertMessage,
                textRow("m" + std::to_string(i), Role::User, "x")
            ));
        }
        XX_TEST_EXPECT_EQ(r.trimHead(2), size_t{2});
        XX_TEST_EXPECT_EQ(idsOf(r), std::string{"m2,m3,m4"});
        XX_TEST_EXPECT_TRUE(r.rowById("m0") == nullptr);
        XX_TEST_EXPECT_TRUE(r.rowById("m4") != nullptr);
        XX_TEST_EXPECT_EQ(r.trimHead(99), size_t{3}); // 超出: 全部丢掉
        XX_TEST_EXPECT_TRUE(r.rows().empty());
        XX_TEST_EXPECT_EQ(r.trimHead(1), size_t{0});
    }

    // ---- 11. 本地行: 只接受 `local_` 前缀, 且必须有可见内容 ----
    {
        ViewReplica r;
        ViewMessage tip;
        tip.id       = "local_tip_1";
        tip.role     = Role::Tip;
        tip.text     = "retrying";
        tip.tip      = ViewMessage::TipData{};
        XX_TEST_EXPECT_TRUE(r.upsertLocalRow(tip));
        XX_TEST_EXPECT_EQ(idsOf(r), std::string{"local_tip_1"});

        // agent 的 id 不许走本地行入口 (避免与 agent 的消息级 op 撞车)
        auto agentRow = textRow("m1", Role::User, "hi");
        XX_TEST_EXPECT_FALSE(r.upsertLocalRow(agentRow));
        XX_TEST_EXPECT_EQ(r.rows().size(), size_t{1});

        // 同 id 再次 upsert: 整条替换 (折叠/结果回写)
        tip.text = "retrying again";
        XX_TEST_EXPECT_TRUE(r.upsertLocalRow(tip));
        XX_TEST_EXPECT_EQ(r.rows().size(), size_t{1});
        XX_TEST_EXPECT_EQ(r.rows()[0]->text, std::string{"retrying again"});

        // 不可见的本地行直接忽略
        ViewMessage emptyTip;
        emptyTip.id   = "local_tip_2";
        emptyTip.role = Role::Tip;
        emptyTip.text = "   ";
        XX_TEST_EXPECT_FALSE(r.upsertLocalRow(emptyTip));

        // agent 的 insert/update 用同名 id 也打不中本地行 (id 命名空间隔离由 agent 保证,
        // 客户端这里只确认两边的行互不覆盖)
        r.applyDelta(rowDelta(WireDelta::Type::InsertMessage, textRow("m1", Role::User, "hi")));
        XX_TEST_EXPECT_EQ(idsOf(r), std::string{"local_tip_1,m1"});
    }

    // ---- 12. 行对象改写: replaceRow / setRowCollapsed 每次换新对象 ----
    {
        ViewReplica r;
        r.applyDelta(rowDelta(WireDelta::Type::InsertMessage, textRow("m1", Role::Think, "t")));
        const auto before = r.rows()[0];
        XX_TEST_EXPECT_TRUE(r.setRowCollapsed("m1", true));
        const auto after = r.rows()[0];
        XX_TEST_EXPECT_TRUE(before != after); // 指针变化 -> 渲染缓存失效
        XX_TEST_EXPECT_TRUE(after->collapsed);
        // 状态没变化: 不产生新对象
        XX_TEST_EXPECT_FALSE(r.setRowCollapsed("m1", true));
        XX_TEST_EXPECT_FALSE(r.setRowCollapsed("m404", false));

        auto  replacement = std::make_shared<ViewMessage>(textRow("m1", Role::Think, "t2"));
        XX_TEST_EXPECT_TRUE(r.replaceRow(0, replacement));
        XX_TEST_EXPECT_EQ(r.rows()[0]->text, std::string{"t2"});
        XX_TEST_EXPECT_FALSE(r.replaceRow(9, replacement)); // 越界忽略
        XX_TEST_EXPECT_FALSE(r.replaceRow(0, nullptr));
        XX_TEST_EXPECT_TRUE(r.rowById("m1") == replacement);
    }

    // ---- 13. 非消息级 op 一律不动副本 (界面状态类增量由端点自己处理) ----
    {
        ViewReplica r;
        WireDelta   turn;
        turn.type      = WireDelta::Type::TurnStart;
        turn.startTimeMs = 1;
        XX_TEST_EXPECT_FALSE(r.applyDelta(turn));
        WireDelta gen;
        gen.type     = WireDelta::Type::GenStatus;
        gen.genPhase = WireDelta::GenPhase::Waiting;
        XX_TEST_EXPECT_FALSE(r.applyDelta(gen));
        WireDelta node;
        node.type     = WireDelta::Type::NodeStart;
        node.nodeName = "llm";
        XX_TEST_EXPECT_FALSE(r.applyDelta(node));
        XX_TEST_EXPECT_TRUE(r.rows().empty());
        XX_TEST_EXPECT_FALSE(r.hasStream());
    }

    return TestResult{g_vr_passed, g_vr_failed};
}

} // namespace test
} // namespace agentxx
