///
/// 覆盖 (对应 `resource/history/async-work/plan.md` §11 的 chatbot 接入):
/// - 提交: 成功回执 / 空正文拒绝 / 没有运行时入口拒绝 / 每会话上限
/// - 事件状态机: queued (`queueAhead`) → started → done (结果摘要) / failed / canceled,
///   以及"不是本实例提交的 id"不认
/// - 取消: 单条 / 本会话全部 / 已结束不再取消
/// - 文本: `formatTaskElapsed` / list / status / 结果通知
/// - 任务记录回收: 过期终态清理与 `maxRecords` 挤掉最旧
/// - 面板: 「后台任务」行文本、状态中文标记、快照 JSON 往返 (空数组不写)
///
/// 提交/取消是协程 (它们背后是表调用): 用例内用本地 `io_context` 跑完再取结果
/// (见 `runAwaitable`), 除此之外这一层不碰网络也不碰主程序。
#include "agentxx-test/core/test_chatbot_task.h"

#include "agentxx_chatbot/chatbot_panel.h"
#include "agentxx_chatbot/chatbot_panel_ui.h"
#include "agentxx_chatbot/chatbot_task.h"

#include "utilxx_base/json.h"

#include "asio/awaitable.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"

#include <optional>
#include <string>
#include <vector>

namespace {
int g_ct_passed = 0;
int g_ct_failed = 0;
} // namespace

#define XX_TEST_PASSED g_ct_passed
#define XX_TEST_FAILED g_ct_failed

namespace agentxx {
namespace test {

namespace {

namespace asio = ::boost::asio;

using namespace agentxx::chatbot;
using utilxx_base::Json;

/// 协程体: 参数按值复制进帧, 不会比协程先死
///
/// 直接写 `co_spawn(io, [&]{...}(), detached)` 是错的: lambda 协程帧只存闭包指针,
/// 临时闭包在语句结束就析构 (规则同 `test_coro_util.h` 的 `keepAliveRun`)。
template<typename T>
asio::awaitable<void> runInto(asio::awaitable<T> call, std::optional<T>* out) {
    *out = co_await std::move(call);
}

/// 在本地 io_context 上把一个协程跑完并取回结果
///
/// 用例仍是同步函数: 假运行时的提交/取消都不挂起, 所以 `run()` 会直接跑完。
template<typename T>
T runAwaitable(asio::awaitable<T> call) {
    asio::io_context io;
    std::optional<T> out;
    asio::co_spawn(io, runInto(std::move(call), &out), asio::detached);
    io.run();
    return std::move(*out);
}

/// 提交一条任务并等结果
TaskSubmitResult submitOf(ChatbotTaskManager& manager, const TaskSubmit& request) {
    return runAwaitable(manager.submit(request));
}

/// 取消并等结果
std::string
    cancelOf(ChatbotTaskManager& manager, std::string_view chatKey, std::string_view asyncId) {
    return runAwaitable(manager.cancel(std::string{chatKey}, std::string{asyncId}));
}

constexpr int32_t kPhaseQueued   = 0;
constexpr int32_t kPhaseStarted  = 1;
constexpr int32_t kPhaseFinished = 2;
constexpr int32_t kPhaseCanceled = 3;

bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

/// 假的主程序侧 (记提交/取消, 返回固定 id 或任务记录 JSON)
struct FakeHost {
    int64_t     clockMs      = 1000;
    int         submitCalls  = 0;
    int         cancelCalls  = 0;
    std::string lastPrompt;
    std::string lastPersona;
    std::string lastChatKey;
    /// 依次返回的 id (每提交一次取一个; 空了 = 提交失败)
    std::vector<std::string> ids{};
    std::string              submitError = "后端拒绝";
    std::vector<std::string> canceled{};

    TaskRuntime runtime() {
        TaskRuntime rt;
        // 真实运行时里这两步都是表调用 (协程); 假实现直接完成, 不挂起
        rt.submit = [this](const TaskSubmit& request, std::string& err)
            -> asio::awaitable<std::string> {
            ++submitCalls;
            lastPrompt  = request.prompt;
            lastPersona = request.persona;
            lastChatKey = request.chatKey;
            if (ids.empty()) {
                err = submitError;
                co_return std::string{};
            }
            const auto id = ids.front();
            ids.erase(ids.begin());
            co_return id;
        };
        rt.cancel = [this](std::string_view asyncId) -> asio::awaitable<int32_t> {
            ++cancelCalls;
            canceled.emplace_back(asyncId);
            co_return 0;
        };
        return rt;
    }

    ChatbotTaskManager::Options options() {
        ChatbotTaskManager::Options opt;
        opt.clock = [this]() {
            return clockMs;
        };
        return opt;
    }
};

/* ==================== 时长文本 ==================== */

void testElapsedText() {
    XX_TEST_EXPECT_EQ(formatTaskElapsed(0), std::string{"0s"});
    XX_TEST_EXPECT_EQ(formatTaskElapsed(-5), std::string{"0s"});
    XX_TEST_EXPECT_EQ(formatTaskElapsed(300), std::string{"1s"});
    XX_TEST_EXPECT_EQ(formatTaskElapsed(12'000), std::string{"12s"});
    XX_TEST_EXPECT_EQ(formatTaskElapsed(60'000), std::string{"1m00s"});
    XX_TEST_EXPECT_EQ(formatTaskElapsed(200'000), std::string{"3m20s"});
    XX_TEST_EXPECT_EQ(formatTaskElapsed(3'600'000), std::string{"1h00m"});
    XX_TEST_EXPECT_EQ(formatTaskElapsed(2 * 3'600'000 + 5 * 60'000), std::string{"2h05m"});
}

/* ==================== 提交 ==================== */

void testSubmit() {
    FakeHost            host;
    ChatbotTaskManager  manager;
    host.ids = {"async-1", "async-2", "async-3"};
    manager.init(host.runtime(), host.options());

    TaskSubmit request;
    request.chatKey = "qq_main|group|G1";
    request.persona = "assistant";
    request.prompt  = "查一下今天的汇率";
    const auto first = submitOf(manager, request);
    XX_TEST_EXPECT_TRUE(first.ok);
    XX_TEST_EXPECT_EQ(first.asyncId, std::string{"async-1"});
    XX_TEST_EXPECT_TRUE(contains(first.message, "async-1"));
    XX_TEST_EXPECT_EQ(host.submitCalls, 1);
    XX_TEST_EXPECT_EQ(host.lastPersona, std::string{"assistant"});
    XX_TEST_EXPECT_EQ(host.lastChatKey, std::string{"qq_main|group|G1"});

    const auto* record = manager.recordOf("async-1");
    XX_TEST_EXPECT_TRUE(record != nullptr);
    if (record != nullptr) {
        XX_TEST_EXPECT_EQ(record->state, TaskState::Queued);
        XX_TEST_EXPECT_EQ(record->title, std::string{"查一下今天的汇率"});
        XX_TEST_EXPECT_EQ(record->submittedAtMs, int64_t{1000});
        XX_TEST_EXPECT_TRUE(record->resultHead.empty());
    }
    XX_TEST_EXPECT_EQ(manager.pendingCount("qq_main|group|G1"), size_t{1});
    XX_TEST_EXPECT_TRUE(manager.hasPending("qq_main|group|G1"));
    XX_TEST_EXPECT_FALSE(manager.hasPending("qq_main|group|G2"));
    XX_TEST_EXPECT_EQ(manager.pendingTotal(), size_t{1});

    // 标题显式给的时候用它
    TaskSubmit titled;
    titled.chatKey = "qq_main|group|G1";
    titled.prompt  = "很长的正文内容";
    titled.title   = "汇率";
    const auto second = submitOf(manager, titled);
    XX_TEST_EXPECT_TRUE(second.ok);
    const auto* titledRecord = manager.recordOf("async-2");
    XX_TEST_EXPECT_TRUE(titledRecord != nullptr);
    if (titledRecord != nullptr) {
        XX_TEST_EXPECT_EQ(titledRecord->title, std::string{"汇率"});
    }

    // 正文会被压成一行并截断存档
    TaskSubmit multiline;
    multiline.chatKey = "qq_main|group|G2";
    multiline.prompt  = "第一行\n第二行\t带缩进";
    XX_TEST_EXPECT_TRUE(submitOf(manager, multiline).ok);
    const auto* flat = manager.recordOf("async-3");
    XX_TEST_EXPECT_TRUE(flat != nullptr);
    if (flat != nullptr) {
        XX_TEST_EXPECT_EQ(flat->promptHead, std::string{"第一行 第二行 带缩进"});
    }
}

void testSubmitRejects() {
    FakeHost           host;
    ChatbotTaskManager manager;
    host.ids = {"async-1"};
    manager.init(host.runtime(), host.options());

    TaskSubmit empty;
    empty.chatKey = "qq_main|group|G1";
    empty.prompt  = "   ";
    const auto emptyResult = submitOf(manager, empty);
    XX_TEST_EXPECT_FALSE(emptyResult.ok);
    XX_TEST_EXPECT_TRUE(contains(emptyResult.message, "正文为空"));
    XX_TEST_EXPECT_EQ(host.submitCalls, 0);

    // 没有运行时入口 (老主程序): 明说不支持, 并给出替代做法
    ChatbotTaskManager noRuntime;
    noRuntime.init(TaskRuntime{}, {});
    TaskSubmit ok;
    ok.chatKey = "qq_main|group|G1";
    ok.prompt  = "做点事";
    const auto noRuntimeResult = submitOf(noRuntime, ok);
    XX_TEST_EXPECT_FALSE(noRuntimeResult.ok);
    XX_TEST_EXPECT_TRUE(contains(noRuntimeResult.message, "submit_async_work"));

    // 后端拒绝: 把原因带回给模型
    FakeHost rejecting;
    ChatbotTaskManager rejectManager;
    rejectManager.init(rejecting.runtime(), rejecting.options());
    TaskSubmit request;
    request.chatKey = "qq_main|group|G1";
    request.prompt  = "做点事";
    const auto rejected = submitOf(rejectManager, request);
    XX_TEST_EXPECT_FALSE(rejected.ok);
    XX_TEST_EXPECT_TRUE(contains(rejected.message, "后端拒绝"));
    XX_TEST_EXPECT_EQ(rejectManager.allRecords().size(), size_t{0});
}

void testPendingLimit() {
    FakeHost           host;
    ChatbotTaskManager manager;
    host.ids = {"async-1", "async-2", "async-3", "async-4"};
    auto options             = host.options();
    options.maxPendingPerChat = 2;
    manager.init(host.runtime(), options);

    TaskSubmit request;
    request.chatKey = "qq_main|group|G1";
    request.prompt  = "做点事";
    XX_TEST_EXPECT_TRUE(submitOf(manager, request).ok);
    XX_TEST_EXPECT_TRUE(submitOf(manager, request).ok);
    const auto third = submitOf(manager, request);
    XX_TEST_EXPECT_FALSE(third.ok);
    XX_TEST_EXPECT_TRUE(contains(third.message, "上限 2"));

    // 别的会话不受影响
    request.chatKey = "qq_main|group|G2";
    XX_TEST_EXPECT_TRUE(submitOf(manager, request).ok);

    // 结束一条后又可以提交
    host.clockMs = 2000;
    XX_TEST_EXPECT_TRUE(manager.onEvent("async-1", kPhaseFinished, R"({"hasError":false})"));
    request.chatKey = "qq_main|group|G1";
    XX_TEST_EXPECT_TRUE(submitOf(manager, request).ok);
}

/* ==================== 事件状态机 ==================== */

void testEventStateMachine() {
    FakeHost           host;
    ChatbotTaskManager manager;
    host.ids = {"async-7", "async-8", "async-9", "async-10"};
    manager.init(host.runtime(), host.options());

    TaskSubmit request;
    request.chatKey = "qq_main|group|G1";
    request.persona = "assistant";
    request.prompt  = "巡一遍仓库";
    XX_TEST_EXPECT_TRUE(submitOf(manager, request).ok);

    // 排队 → 起跑 → 结果 (任务记录里有 result 字段)
    XX_TEST_EXPECT_TRUE(manager.onEvent("async-7", kPhaseQueued, R"({"queueAhead":2})"));
    XX_TEST_EXPECT_EQ(manager.recordOf("async-7")->queueAhead, 2);

    host.clockMs = 4000;
    XX_TEST_EXPECT_TRUE(manager.onEvent("async-7", kPhaseStarted, R"({"asyncId":"async-7"})"));
    const auto* running = manager.recordOf("async-7");
    XX_TEST_EXPECT_EQ(running->state, TaskState::Running);
    XX_TEST_EXPECT_EQ(running->queueAhead, 0);
    XX_TEST_EXPECT_EQ(running->startedAtMs, int64_t{4000});

    host.clockMs = 9500;
    XX_TEST_EXPECT_TRUE(manager.onEvent("async-7", kPhaseFinished, R"({"hasError":false})"));
    // 结果正文不在事件载荷里: 入口在终态向主程序任务表要过一次后补上 (本层不调表)
    manager.setResultHead("async-7", "上游三个提交, 没有异常");
    const auto* done = manager.recordOf("async-7");
    XX_TEST_EXPECT_EQ(done->state, TaskState::Done);
    XX_TEST_EXPECT_EQ(done->finishedAtMs, int64_t{9500});
    XX_TEST_EXPECT_TRUE(contains(done->resultHead, "没有异常"));
    XX_TEST_EXPECT_FALSE(done->hasError);
    XX_TEST_EXPECT_FALSE(manager.hasPending("qq_main|group|G1"));

    // 不是本实例提交的 id: 不认
    XX_TEST_EXPECT_FALSE(manager.onEvent("async-404", kPhaseFinished, "{}"));
    // 未知阶段: 不认
    XX_TEST_EXPECT_FALSE(manager.onEvent("async-7", 99, "{}"));

    // 失败: 错误正文进任务记录
    TaskSubmit failing;
    failing.chatKey = "qq_main|group|G1";
    failing.prompt  = "再跑一次";
    XX_TEST_EXPECT_TRUE(submitOf(manager, failing).ok);
    XX_TEST_EXPECT_TRUE(manager.onEvent(
        "async-8",
        kPhaseFinished,
        R"({"hasError":true,"error":"模型没有返回内容"})"
    ));
    const auto* failed = manager.recordOf("async-8");
    XX_TEST_EXPECT_EQ(failed->state, TaskState::Failed);
    XX_TEST_EXPECT_TRUE(failed->hasError);
    XX_TEST_EXPECT_EQ(failed->errorText, std::string{"模型没有返回内容"});

    // 取消 (排队中): 没有起跑时间时用提交时间兜底
    TaskSubmit queuedCancel;
    queuedCancel.chatKey = "qq_main|group|G1";
    queuedCancel.prompt  = "排队就被撤";
    XX_TEST_EXPECT_TRUE(submitOf(manager, queuedCancel).ok);
    host.clockMs = 20'000;
    XX_TEST_EXPECT_TRUE(manager.onEvent("async-9", kPhaseCanceled, R"({"canceled":true})"));
    const auto* canceled = manager.recordOf("async-9");
    XX_TEST_EXPECT_EQ(canceled->state, TaskState::Canceled);
    XX_TEST_EXPECT_EQ(canceled->startedAtMs, canceled->submittedAtMs);

    // 载荷不是合法 JSON: 也要能落到终态 (不能被异常带走)
    XX_TEST_EXPECT_TRUE(submitOf(manager, queuedCancel).ok);
    XX_TEST_EXPECT_TRUE(manager.onEvent("async-10", kPhaseFinished, "not json"));
    XX_TEST_EXPECT_EQ(manager.recordOf("async-10")->state, TaskState::Done);
}

/* ==================== 取消 ==================== */

void testCancel() {
    FakeHost           host;
    ChatbotTaskManager manager;
    host.ids = {"async-1", "async-2"};
    manager.init(host.runtime(), host.options());

    TaskSubmit request;
    request.chatKey = "qq_main|group|G1";
    request.prompt  = "做点事";
    XX_TEST_EXPECT_TRUE(submitOf(manager, request).ok);
    XX_TEST_EXPECT_TRUE(submitOf(manager, request).ok);

    // 单条
    const auto one = cancelOf(manager, "qq_main|group|G1", "async-2");
    XX_TEST_EXPECT_TRUE(contains(one, "已取消 1 个"));
    XX_TEST_EXPECT_EQ(host.canceled.size(), size_t{1});
    XX_TEST_EXPECT_EQ(host.canceled[0], std::string{"async-2"});

    // 别的会话里的 id: 拒绝 (防止跨群取消)
    const auto cross = cancelOf(manager, "qq_main|group|G2", "async-1");
    XX_TEST_EXPECT_TRUE(contains(cross, "没有任务"));

    // 已结束的: 明确说不用取消
    XX_TEST_EXPECT_TRUE(
        manager.onEvent("async-1", kPhaseFinished, R"({"hasError":false})")
    );
    const auto finished = cancelOf(manager, "qq_main|group|G1", "async-1");
    XX_TEST_EXPECT_TRUE(contains(finished, "已经结束"));

    // 全部: 只剩一条进行中 (async-2 还在排队)
    const auto all = cancelOf(manager, "qq_main|group|G1", "");
    XX_TEST_EXPECT_TRUE(contains(all, "已取消 1 个"));
    // 终态由主程序的回调带回来: 补一条之后就没有进行中的了
    XX_TEST_EXPECT_TRUE(manager.onEvent("async-2", kPhaseCanceled, R"({"canceled":true})"));
    const auto again = cancelOf(manager, "qq_main|group|G1", "");
    XX_TEST_EXPECT_TRUE(contains(again, "没有进行中"));
}

void testCancelWithoutRuntime() {
    ChatbotTaskManager manager;
    manager.init(TaskRuntime{}, {});
    XX_TEST_EXPECT_TRUE(contains(cancelOf(manager, "qq_main|group|G1", "async-1"), "没有后台任务入口"));
}

/* ==================== 文本渲染 ==================== */

void testTexts() {
    FakeHost           host;
    ChatbotTaskManager manager;
    host.ids = {"async-1", "async-2"};
    manager.init(host.runtime(), host.options());

    TaskSubmit request;
    request.chatKey = "qq_main|group|G1";
    request.persona = "assistant";
    request.prompt  = "查一下今天的汇率";
    XX_TEST_EXPECT_TRUE(submitOf(manager, request).ok);

    // 空清单
    XX_TEST_EXPECT_TRUE(contains(manager.listText("qq_main|group|G2"), "还没有后台任务"));
    XX_TEST_EXPECT_TRUE(contains(manager.statusText("qq_main|group|G2", ""), "没有后台任务"));
    XX_TEST_EXPECT_TRUE(
        contains(manager.statusText("qq_main|group|G1", "async-404"), "没有任务 async-404")
    );

    // 运行中: 带已进行时长
    host.clockMs = 2000;
    XX_TEST_EXPECT_TRUE(manager.onEvent("async-1", kPhaseStarted, "{}"));
    host.clockMs = 2000 + 75'000;
    const auto list = manager.listText("qq_main|group|G1");
    XX_TEST_EXPECT_TRUE(contains(list, "进行中 1"));
    XX_TEST_EXPECT_TRUE(contains(list, "async-1 [running]"));
    XX_TEST_EXPECT_TRUE(contains(list, "1m15s"));
    XX_TEST_EXPECT_TRUE(contains(list, "查一下今天的汇率"));

    const auto status = manager.statusText("qq_main|group|G1", "async-1");
    XX_TEST_EXPECT_TRUE(contains(status, "async-1 [running]"));
    XX_TEST_EXPECT_TRUE(contains(status, "人设: assistant"));
    XX_TEST_EXPECT_TRUE(contains(status, "已进行: 1m15s"));
    XX_TEST_EXPECT_TRUE(contains(status, "内容: 查一下今天的汇率"));

    // 不给 id 时取本会话最近一条 (进行中优先)
    XX_TEST_EXPECT_TRUE(contains(manager.statusText("qq_main|group|G1", ""), "async-1"));

    // 终态: 带耗时与结果开头, 通知正文能直接发给人
    host.clockMs = 200'000;
    XX_TEST_EXPECT_TRUE(manager.onEvent("async-1", kPhaseFinished, R"({"hasError":false})"));
    // 主程序给的结果是数组时入口会拼成多行文本, 这里直接给拼接后的正文
    manager.setResultHead("async-1", "一条\n两条\n三条");
    const auto* record = manager.recordOf("async-1");
    XX_TEST_EXPECT_TRUE(record != nullptr);
    if (record != nullptr) {
        const auto note = ChatbotTaskManager::finishedText(*record);
        XX_TEST_EXPECT_TRUE(contains(note, "已完成"));
        XX_TEST_EXPECT_TRUE(contains(note, "查一下今天的汇率"));

        const auto systemNote = ChatbotTaskManager::resultNote(*record);
        XX_TEST_EXPECT_TRUE(contains(systemNote, "[系统提示]"));
        XX_TEST_EXPECT_TRUE(contains(systemNote, "不是群成员发言"));

        const auto json = taskRecordJson(*record);
        const auto parsed = Json::parse(json);
        XX_TEST_EXPECT_EQ(parsed.value("id", std::string{}), std::string{"async-1"});
        XX_TEST_EXPECT_EQ(parsed.value("state", std::string{}), std::string{"done"});
        XX_TEST_EXPECT_EQ(parsed.value("chatKey", std::string{}), std::string{"qq_main|group|G1"});
    }

    // 取消的消息不进 finishedText 的"已完成"分支
    TaskSubmit second;
    second.chatKey = "qq_main|group|G1";
    second.prompt  = "算了";
    XX_TEST_EXPECT_TRUE(submitOf(manager, second).ok);
    XX_TEST_EXPECT_TRUE(manager.onEvent("async-2", kPhaseCanceled, R"({"canceled":true})"));
    const auto canceledNote = ChatbotTaskManager::finishedText(*manager.recordOf("async-2"));
    XX_TEST_EXPECT_TRUE(contains(canceledNote, "已取消"));
}

/* ==================== 任务记录回收 ==================== */

void testPrune() {
    FakeHost           host;
    ChatbotTaskManager manager;
    host.ids = {"async-1", "async-2", "async-3"};
    auto options              = host.options();
    options.keepFinishedMs    = 1000;
    options.maxRecords        = 1;
    manager.init(host.runtime(), options);
    TaskSubmit request;
    request.chatKey = "qq_main|group|G1";
    request.prompt  = "做点事";

    // 过期终态记录在下次提交时被清掉
    XX_TEST_EXPECT_TRUE(submitOf(manager, request).ok);
    host.clockMs = 5000;
    XX_TEST_EXPECT_TRUE(manager.onEvent("async-1", kPhaseFinished, "{}"));
    host.clockMs = 7000; // 超过 keepFinishedMs
    XX_TEST_EXPECT_TRUE(submitOf(manager, request).ok); // async-2
    XX_TEST_EXPECT_TRUE(manager.recordOf("async-1") == nullptr);
    XX_TEST_EXPECT_TRUE(manager.recordOf("async-2") != nullptr);

    // maxRecords: 超出后丢最旧的已结束记录 (进行中的不丢)
    host.clockMs = 8000;
    XX_TEST_EXPECT_TRUE(manager.onEvent("async-2", kPhaseFinished, "{}"));
    XX_TEST_EXPECT_TRUE(submitOf(manager, request).ok); // async-3
    // async-2 已结束, 名额满 (1 条) → 它被挤掉
    XX_TEST_EXPECT_TRUE(manager.recordOf("async-2") == nullptr);
    XX_TEST_EXPECT_TRUE(manager.recordOf("async-3") != nullptr);
    XX_TEST_EXPECT_EQ(manager.allRecords().size(), size_t{1});

    manager.clear();
    XX_TEST_EXPECT_EQ(manager.allRecords().size(), size_t{0});
}

/* ==================== 面板 ==================== */

void testPanel() {
    // 行文本: 运行中带时长, 终态带中文标记
    PanelTaskView running;
    running.asyncId    = "async-7";
    running.title      = "查汇率";
    running.state      = "running";
    running.elapsedMs  = 95'000;
    XX_TEST_EXPECT_TRUE(contains(panelTaskText(running, true), "async-7 1m35s 查汇率"));

    PanelTaskView queued;
    queued.asyncId    = "async-8";
    queued.title      = "巡仓库";
    queued.state      = "queued";
    queued.queueAhead = 2;
    XX_TEST_EXPECT_TRUE(contains(panelTaskText(queued, true), "排队第 2 位"));

    PanelTaskView done;
    done.asyncId   = "async-9";
    done.title     = "算完了";
    done.state     = "done";
    done.elapsedMs = 4000;
    XX_TEST_EXPECT_TRUE(contains(panelTaskText(done, false), "async-9 完成 算完了"));

    XX_TEST_EXPECT_EQ(panelTaskStateLabel("queued"), std::string{"排队"});
    XX_TEST_EXPECT_EQ(panelTaskStateLabel("running"), std::string{"运行"});
    XX_TEST_EXPECT_EQ(panelTaskStateLabel("done"), std::string{"完成"});
    XX_TEST_EXPECT_EQ(panelTaskStateLabel("failed"), std::string{"失败"});
    XX_TEST_EXPECT_EQ(panelTaskStateLabel("canceled"), std::string{"取消"});

    // 快照往返: 有任务时写进 JSON, 读完一致
    PanelSnapshot snapshot;
    snapshot.tick = 3;
    // 面板需要至少一段数据才会渲染 (会话/人设/账号都没有时只给一行占位)
    PanelPersonaView persona;
    persona.name       = "assistant";
    persona.chatBudget = 64;
    persona.mounted    = true;
    snapshot.personas.push_back(persona);
    PanelTaskView view;
    view.asyncId    = "async-3";
    view.title      = "查汇率";
    view.state      = "running";
    view.chatKey    = "qq_main|group|G1";
    view.elapsedMs  = 12'000;
    view.queueAhead = 0;
    snapshot.tasks.push_back(view);

    PanelSnapshot parsed;
    XX_TEST_EXPECT_TRUE(parsePanelSnapshot(panelSnapshotJson(snapshot), parsed));
    XX_TEST_EXPECT_EQ(parsed.tasks.size(), size_t{1});
    if (!parsed.tasks.empty()) {
        XX_TEST_EXPECT_EQ(parsed.tasks[0].asyncId, std::string{"async-3"});
        XX_TEST_EXPECT_EQ(parsed.tasks[0].state, std::string{"running"});
        XX_TEST_EXPECT_EQ(parsed.tasks[0].elapsedMs, int64_t{12'000});
        XX_TEST_EXPECT_EQ(parsed.tasks[0].chatKey, std::string{"qq_main|group|G1"});
    }

    // 空数组不写进 JSON (老客户端行为不变)
    PanelSnapshot empty;
    XX_TEST_EXPECT_FALSE(contains(panelSnapshotJson(empty), "\"tasks\""));
    // 老 agent 不发 tasks 字段: 解析后是空数组, 不报错
    PanelSnapshot legacy;
    XX_TEST_EXPECT_TRUE(parsePanelSnapshot(R"({"v":1,"tick":1})", legacy));
    XX_TEST_EXPECT_TRUE(legacy.tasks.empty());

    // 组件树: 有任务时出现「后台任务」段, 没有时不出现
    const auto items = panelItemsOf(snapshot);
    bool       hasSection = false;
    for (const auto& item : items) {
        // 段标题是 Text 组件; 递归找一次
        std::vector<const pluginxx::ui::Item*> stack{&item};
        while (!stack.empty()) {
            const auto* node = stack.back();
            stack.pop_back();
            if (node->kind == "Text" && node->text.resolve().rfind("后台任务", 0) == 0) {
                hasSection = true;
                break;
            }
            for (const auto& child : node->children) {
                stack.push_back(&child);
            }
        }
        if (hasSection) {
            break;
        }
    }
    XX_TEST_EXPECT_TRUE(hasSection);
}

} // namespace

TestResult testChatbotTask() {
    g_ct_passed = 0;
    g_ct_failed = 0;

    testElapsedText();
    testSubmit();
    testSubmitRejects();
    testPendingLimit();
    testEventStateMachine();
    testCancel();
    testCancelWithoutRuntime();
    testTexts();
    testPrune();
    testPanel();

    return TestResult{g_ct_passed, g_ct_failed};
}

} // namespace test
} // namespace agentxx
