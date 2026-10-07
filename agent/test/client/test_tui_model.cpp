/// test_tui_model —— 客户端模型层 (计划 UI-1)
///
/// 背景: 历史分页窗口与消息队列镜像原先写在 TUI 端点内 (混着渲染/线程代码),
/// 只能靠真实终端交互验证。抽成无 FTXUI 依赖的模型之后, 边界条件 (迟到页、
/// 不连续页、空页、重复请求、序号断号、回执重复) 可以在这里穷举。
///
/// 覆盖:
/// 1. 历史分页窗口:
///    - 全量/尾窗同步后的窗口边界与"上方是否还有更早历史"
///    - 分页请求的去重 (请求未完成期间不重复发) 与边界 (上方无历史时不发)
///    - 页响应判定: 接受 / 空页 / 迟到会话 / 不连续页 / 无未完成的请求
///    - 前插与尾部追加对窗口起点/条数的影响
///    - 尾部序号连续性: 首个 / 连续 / 断号 / 重复
///    - 切换会话后旧响应被丢弃
/// 2. 消息队列镜像:
///    - 快照应用 (条目 + 队列状态), 未知状态文本与空状态的处理
///    - 按 id 删除 / 查找 / 清空
///    - 投递回执记账: 重复 requestId 不重复记账, 容量上限按最旧淘汰
#include "agentxx-test/client/test_tui_model.h"

#include "agentxx-client/io/tui/model/history_window.h"
#include "agentxx-client/io/tui/model/queue_mirror.h"
#include <string>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_tm_passed = 0;
int g_tm_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_tm_passed
#define XX_TEST_FAILED g_tm_failed

namespace agentxx {
namespace test {

using agentxx::client::HistoryWindow;
using agentxx::client::MessageQueueMirror;

namespace {

/// 1. 历史分页窗口: 同步/分页/序号
void testHistoryWindow() {
    HistoryWindow win;
    win.resetForSession("sess-1");

    // 装载前: 无历史, 不触发分页
    XX_TEST_EXPECT_EQ(win.windowStart(), uint64_t{0});
    XX_TEST_EXPECT_FALSE(win.hasMore());
    XX_TEST_EXPECT_FALSE(win.beginOlderPageRequest());

    // 尾窗同步: 上方还有更早历史 (fromIndex > 0)
    win.reset(/*windowStart=*/120, /*totalCount=*/200, /*loadedCount=*/80);
    XX_TEST_EXPECT_EQ(win.windowStart(), uint64_t{120});
    XX_TEST_EXPECT_EQ(win.totalCount(), uint64_t{200});
    XX_TEST_EXPECT_EQ(win.loadedCount(), uint64_t{80});
    XX_TEST_EXPECT_TRUE(win.hasMore());
    XX_TEST_EXPECT_FALSE(win.loading());

    // 分页请求: 第一次成功并置加载中, 第二次被去重
    XX_TEST_EXPECT_TRUE(win.beginOlderPageRequest());
    XX_TEST_EXPECT_TRUE(win.loading());
    XX_TEST_EXPECT_FALSE(win.beginOlderPageRequest());

    // 不连续页 (页尾 200 != 窗口起点 120) 被丢弃, 加载标志复位
    XX_TEST_EXPECT_EQ(
        win.applyPage("sess-1", 140, 60, 200),
        HistoryWindow::PageOutcome::NonContiguous
    );
    XX_TEST_EXPECT_FALSE(win.loading());
    XX_TEST_EXPECT_EQ(win.windowStart(), uint64_t{120}); ///< 窗口未被改动

    // 迟到会话的页被丢弃
    XX_TEST_EXPECT_TRUE(win.beginOlderPageRequest());
    XX_TEST_EXPECT_EQ(
        win.applyPage("sess-2", 40, 80, 200),
        HistoryWindow::PageOutcome::StaleSession
    );

    // 连续页被接受, 但接受本身不改窗口 (由调用方前插后调 notePrepended)
    XX_TEST_EXPECT_TRUE(win.beginOlderPageRequest());
    XX_TEST_EXPECT_EQ(win.applyPage("sess-1", 40, 80, 200), HistoryWindow::PageOutcome::Accepted);
    XX_TEST_EXPECT_EQ(win.windowStart(), uint64_t{120});
    win.notePrepended(40, 78); ///< 78 = 界面过滤掉 2 条空消息后的实际前插条数
    XX_TEST_EXPECT_EQ(win.windowStart(), uint64_t{40});
    XX_TEST_EXPECT_EQ(win.loadedCount(), uint64_t{158});

    // 空页 = 上方没有更早历史 (窗口起点归零, 后续不再请求)
    XX_TEST_EXPECT_TRUE(win.beginOlderPageRequest());
    XX_TEST_EXPECT_EQ(win.applyPage("sess-1", 0, 0, 200), HistoryWindow::PageOutcome::Empty);
    XX_TEST_EXPECT_EQ(win.windowStart(), uint64_t{0});
    XX_TEST_EXPECT_FALSE(win.hasMore());
    XX_TEST_EXPECT_FALSE(win.beginOlderPageRequest());

    // 全量同步: windowStart == 0, 总数与条数一并更新
    win.reset(0, 30, 30);
    XX_TEST_EXPECT_EQ(win.windowStart(), uint64_t{0});
    XX_TEST_EXPECT_EQ(win.totalCount(), uint64_t{30});
    XX_TEST_EXPECT_FALSE(win.hasMore());

    // 尾部追加 (断线增量补拉): 条数增加, 窗口起点不变; 总数 0 时保持原值
    win.noteTailAppended(5, 35);
    XX_TEST_EXPECT_EQ(win.loadedCount(), uint64_t{35});
    XX_TEST_EXPECT_EQ(win.totalCount(), uint64_t{35});
    win.noteTailAppended(2, 0);
    XX_TEST_EXPECT_EQ(win.loadedCount(), uint64_t{37});
    XX_TEST_EXPECT_EQ(win.totalCount(), uint64_t{35}); ///< 0 = 未知, 不覆盖

    // 尾部序号连续性
    XX_TEST_EXPECT_TRUE(!win.hasSeq());
    XX_TEST_EXPECT_EQ(win.observeTailSeq(10), HistoryWindow::SeqOutcome::First);
    XX_TEST_EXPECT_TRUE(win.hasSeq());
    XX_TEST_EXPECT_FALSE(win.hasGap());
    XX_TEST_EXPECT_EQ(win.lastSeq(), uint64_t{10});
    XX_TEST_EXPECT_EQ(win.observeTailSeq(11), HistoryWindow::SeqOutcome::Ok);
    XX_TEST_EXPECT_EQ(win.observeTailSeq(11), HistoryWindow::SeqOutcome::Repeat); ///< 重复投递
    XX_TEST_EXPECT_EQ(win.observeTailSeq(9), HistoryWindow::SeqOutcome::Repeat);  ///< 旧序号
    XX_TEST_EXPECT_EQ(win.observeTailSeq(15), HistoryWindow::SeqOutcome::Gap);
    XX_TEST_EXPECT_TRUE(win.hasGap());
    XX_TEST_EXPECT_EQ(win.lastSeq(), uint64_t{15}); ///< 基线推进到新值
    XX_TEST_EXPECT_EQ(win.observeTailSeq(16), HistoryWindow::SeqOutcome::Ok);
    XX_TEST_EXPECT_FALSE(win.hasGap()); ///< 恢复连续后空洞标记清除

    // 窗口整体替换后序号基线失效 (新窗口重新观察)
    win.reset(0, 40, 40);
    XX_TEST_EXPECT_FALSE(win.hasSeq());
    XX_TEST_EXPECT_EQ(win.observeTailSeq(3), HistoryWindow::SeqOutcome::First);

    // 切换会话: 会话标识更新, 窗口与序号全部归零 (旧会话迟到页被丢弃)
    win.resetForSession("sess-3");
    XX_TEST_EXPECT_EQ(win.sessionId(), std::string{"sess-3"});
    XX_TEST_EXPECT_EQ(win.windowStart(), uint64_t{0});
    XX_TEST_EXPECT_EQ(win.totalCount(), uint64_t{0});
    XX_TEST_EXPECT_EQ(win.loadedCount(), uint64_t{0});
    XX_TEST_EXPECT_FALSE(win.hasSeq());
    XX_TEST_EXPECT_FALSE(win.hasMore());
    XX_TEST_EXPECT_EQ(win.observeTailSeq(5), HistoryWindow::SeqOutcome::First);
    XX_TEST_EXPECT_EQ(win.applyPage("sess-1", 170, 30, 200), HistoryWindow::PageOutcome::StaleSession);
}

/// 2. 消息队列镜像: 快照/删除/状态/回执记账
void testQueueMirror() {
    MessageQueueMirror q;
    XX_TEST_EXPECT_TRUE(q.empty());
    XX_TEST_EXPECT_EQ(q.size(), size_t{0});
    XX_TEST_EXPECT_TRUE(q.idle()); ///< 初始即空闲 (idle)
    XX_TEST_EXPECT_EQ(q.state(), std::string{"idle"});
    XX_TEST_EXPECT_FALSE(q.paused());

    auto makeEntry = [](std::string id, std::string text, int64_t createdMs) {
        MessageQueueMirror::Entry e;
        e.id          = std::move(id);
        e.text        = std::move(text);
        e.model       = "m1";
        e.createdAtMs = createdMs;
        return e;
    };

    // 快照应用: 整体替换 (含条目顺序) + 队列状态
    q.applySnapshot(
        {makeEntry("q-2", "second", 2000), makeEntry("q-1", "first", 1000)},
        "running"
    );
    XX_TEST_EXPECT_EQ(q.size(), size_t{2});
    XX_TEST_EXPECT_EQ(q.state(), std::string{"running"});
    XX_TEST_EXPECT_FALSE(q.idle());
    XX_TEST_EXPECT_FALSE(q.paused());
    XX_TEST_EXPECT_EQ(q.entries()[0].id, std::string{"q-2"}); ///< 顺序以服务端为准
    XX_TEST_EXPECT_EQ(q.entries()[0].model, std::string{"m1"});
    XX_TEST_EXPECT_EQ(q.entries()[0].createdAtMs, int64_t{2000});

    // 按 id 查找
    XX_TEST_EXPECT_TRUE(q.find("q-1") != nullptr);
    if (const auto* e = q.find("q-1"); e != nullptr) {
        XX_TEST_EXPECT_EQ(e->text, std::string{"first"});
        XX_TEST_EXPECT_EQ(e->createdAtMs, int64_t{1000});
    }
    XX_TEST_EXPECT_TRUE(q.find("nope") == nullptr);

    // 删除一条 (本地乐观更新)
    XX_TEST_EXPECT_TRUE(q.removeEntry("q-2"));
    XX_TEST_EXPECT_FALSE(q.removeEntry("q-2")); ///< 再删返回 false
    XX_TEST_EXPECT_EQ(q.size(), size_t{1});
    XX_TEST_EXPECT_TRUE(q.find("q-2") == nullptr);

    // 暂停状态 + 清空
    q.applySnapshot({makeEntry("q-9", "nine", 9000)}, "paused");
    XX_TEST_EXPECT_TRUE(q.paused());
    XX_TEST_EXPECT_EQ(q.size(), size_t{1});
    q.clear();
    XX_TEST_EXPECT_TRUE(q.empty());
    XX_TEST_EXPECT_EQ(q.state(), std::string{"idle"}); ///< 清空同时回到 idle

    // 状态文本: 未知取值原样保存 (界面按"非 idle"保守展示); 空值按 idle
    q.applySnapshot({}, "draining");
    XX_TEST_EXPECT_EQ(q.state(), std::string{"draining"});
    XX_TEST_EXPECT_FALSE(q.idle());
    q.applySnapshot({}, "");
    XX_TEST_EXPECT_EQ(q.state(), std::string{"idle"});
    XX_TEST_EXPECT_TRUE(q.idle());

    // 投递回执记账: 同一 requestId 只记账一次 (界面据此只提示一次)
    MessageQueueMirror        qa;
    MessageQueueMirror::AckRecord ack;
    ack.requestId = 7;
    ack.status    = "rejected";
    ack.reason    = "empty_content";
    ack.detail    = "内容为空";
    XX_TEST_EXPECT_TRUE(qa.noteInputAck(ack));
    XX_TEST_EXPECT_FALSE(qa.noteInputAck(ack)); ///< 重复回执不再记账
    XX_TEST_EXPECT_EQ(qa.ackCount(), size_t{1});
    XX_TEST_EXPECT_TRUE(qa.lastAck(7) != nullptr);
    if (const auto* rec = qa.lastAck(7); rec != nullptr) {
        XX_TEST_EXPECT_EQ(rec->status, std::string{"rejected"});
        XX_TEST_EXPECT_EQ(rec->reason, std::string{"empty_content"});
    }
    XX_TEST_EXPECT_TRUE(qa.lastAck(8) == nullptr);
    // requestId == 0 (老客户端不回执) 不记账
    MessageQueueMirror::AckRecord zeroAck;
    zeroAck.requestId = 0;
    zeroAck.status    = "started";
    XX_TEST_EXPECT_FALSE(qa.noteInputAck(zeroAck));
    XX_TEST_EXPECT_EQ(qa.ackCount(), size_t{1});

    // 容量上限: 超过后按最旧淘汰, 且淘汰项不再命中
    for (uint64_t i = 1; i <= MessageQueueMirror::kAckHistoryLimit + 5; ++i) {
        MessageQueueMirror::AckRecord a;
        a.requestId = 100 + i;
        a.status    = "queued";
        XX_TEST_EXPECT_TRUE(qa.noteInputAck(a));
    }
    XX_TEST_EXPECT_EQ(qa.ackCount(), MessageQueueMirror::kAckHistoryLimit);
    XX_TEST_EXPECT_TRUE(qa.lastAck(7) == nullptr);    ///< 最旧的一条已被淘汰
    XX_TEST_EXPECT_TRUE(qa.lastAck(100 + MessageQueueMirror::kAckHistoryLimit) != nullptr);
    // 被淘汰的 requestId 可以重新记账 (不会被永久记为"已提示")
    XX_TEST_EXPECT_TRUE(qa.noteInputAck(ack));
}

} // namespace

TestResult testTuiModels() {
    testHistoryWindow();
    testQueueMirror();
    return TestResult{g_tm_passed, g_tm_failed};
}

} // namespace test
} // namespace agentxx
