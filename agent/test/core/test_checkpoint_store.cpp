#include "test_checkpoint_store.h"

#include "agentxx/agent/checkpoint_store.h"
#include "neograph/graph/checkpoint.h"
#include "neograph/json.h"
#include <string>

namespace agentxx {
namespace test {

int g_cs_passed = 0;
int g_cs_failed = 0;

namespace ngcp = neograph::graph;

/// 构造一个带 channel_values 的测试 checkpoint
/// 注意: 必须用括号构造 json 标量 —— `neograph::json{msg}` 会被
/// json(std::initializer_list<json>) 劫持构造为数组而非字符串
static ngcp::Checkpoint makeCp(const std::string& threadId,
                               const std::string& id,
                               int64_t            step,
                               const std::string& msg) {
    ngcp::Checkpoint cp;
    cp.id              = id;
    cp.thread_id       = threadId;
    cp.step            = step;
    cp.timestamp       = 1000 + step;
    cp.current_node    = "node_" + id;
    cp.next_nodes      = {"next_" + id};
    cp.interrupt_phase = ngcp::CheckpointPhase::Completed;

    neograph::json messages = neograph::json::object();
    messages["version"] = step;
    messages["value"]   = neograph::json(msg);
    neograph::json channels = neograph::json::object();
    channels["messages"] = messages;
    cp.channel_values           = neograph::json::object();
    cp.channel_values["channels"] = channels;
    return cp;
}

/// 构造一条 pending write
static ngcp::PendingWrite makeWrite(const std::string& taskId, int64_t step) {
    ngcp::PendingWrite w;
    w.task_id   = taskId;
    w.task_path = taskId;
    w.node_name = "node_" + taskId;
    w.writes    = neograph::json::array();
    w.command   = neograph::json{};
    w.sends     = neograph::json::array();
    w.step      = step;
    w.timestamp = 2000 + step;
    return w;
}

asio::awaitable<TestResult> run_checkpoint_store_tests() {
    g_cs_passed = 0;
    g_cs_failed = 0;

    // ── 1. 基础保存与读取 ───────────────────────────────────────────────
    {
        agentxx::agent::InMemorySingleCheckpointStore store;
        XX_TEST_EXPECT_EQ(store.size(), (size_t)0);
        XX_TEST_EXPECT_FALSE(store.load_latest("t1").has_value());
        XX_TEST_EXPECT_EQ(store.list("t1").size(), (size_t)0);

        auto cp1 = makeCp("t1", "cp1", 1, "hello");
        store.save(cp1);

        XX_TEST_EXPECT_EQ(store.size(), (size_t)1);
        auto latest = store.load_latest("t1");
        XX_TEST_EXPECT_TRUE(latest.has_value());
        XX_TEST_EXPECT_EQ(latest->id, std::string("cp1"));
        XX_TEST_EXPECT_EQ(latest->step, (int64_t)1);
        // channel_values 完整往返
        XX_TEST_EXPECT_EQ(
            latest->channel_values["channels"]["messages"]["value"].get<std::string>(),
            std::string("hello")
        );
    }

    // ── 2. 同一 thread 重复 save: 只保留最新 ────────────────────────────
    {
        agentxx::agent::InMemorySingleCheckpointStore store;
        store.save(makeCp("t1", "cp1", 1, "v1"));
        store.save(makeCp("t1", "cp2", 2, "v2"));
        store.save(makeCp("t1", "cp3", 3, "v3"));

        XX_TEST_EXPECT_EQ(store.size(), (size_t)1);

        auto latest = store.load_latest("t1");
        XX_TEST_EXPECT_TRUE(latest.has_value());
        XX_TEST_EXPECT_EQ(latest->id, std::string("cp3"));
        XX_TEST_EXPECT_EQ(latest->step, (int64_t)3);
        XX_TEST_EXPECT_EQ(
            latest->channel_values["channels"]["messages"]["value"].get<std::string>(),
            std::string("v3")
        );

        // list 最多返回最新一条
        XX_TEST_EXPECT_EQ(store.list("t1").size(), (size_t)1);
        XX_TEST_EXPECT_EQ(store.list("t1")[0].id, std::string("cp3"));
        // limit <= 0 返回空
        XX_TEST_EXPECT_EQ(store.list("t1", 0).size(), (size_t)0);

        // load_by_id 只命中最新, 历史 id 已被淘汰
        XX_TEST_EXPECT_TRUE(store.load_by_id("cp3").has_value());
        XX_TEST_EXPECT_FALSE(store.load_by_id("cp1").has_value());
        XX_TEST_EXPECT_FALSE(store.load_by_id("cp2").has_value());
        XX_TEST_EXPECT_FALSE(store.load_by_id("nonexistent").has_value());
    }

    // ── 3. 多 thread 独立保留 ───────────────────────────────────────────
    {
        agentxx::agent::InMemorySingleCheckpointStore store;
        store.save(makeCp("tA", "a1", 1, "A"));
        store.save(makeCp("tB", "b1", 1, "B"));
        store.save(makeCp("tA", "a2", 2, "A2"));

        XX_TEST_EXPECT_EQ(store.size(), (size_t)2);
        XX_TEST_EXPECT_EQ(store.load_latest("tA")->id, std::string("a2"));
        XX_TEST_EXPECT_EQ(store.load_latest("tB")->id, std::string("b1"));
        XX_TEST_EXPECT_TRUE(store.load_by_id("b1").has_value());
        XX_TEST_EXPECT_FALSE(store.load_by_id("a1").has_value());
    }

    // ── 4. pending writes: 随历史 checkpoint 淘汰, 最新的保留 ───────────
    {
        agentxx::agent::InMemorySingleCheckpointStore store;

        // cp1 上挂载 writes
        store.save(makeCp("t1", "cp1", 1, "v1"));
        store.put_writes("t1", "cp1", makeWrite("w1", 1));
        store.put_writes("t1", "cp1", makeWrite("w2", 1));
        XX_TEST_EXPECT_EQ(store.pending_writes_count("t1", "cp1"), (size_t)2);
        XX_TEST_EXPECT_EQ(store.get_writes("t1", "cp1").size(), (size_t)2);

        // 保存 cp2 (新 super-step): cp1 及其 pending writes 应被淘汰
        store.save(makeCp("t1", "cp2", 2, "v2"));
        XX_TEST_EXPECT_EQ(store.pending_writes_count("t1", "cp1"), (size_t)0);
        XX_TEST_EXPECT_EQ(store.get_writes("t1", "cp1").size(), (size_t)0);

        // cp2 (当前最新) 上挂载 writes: 必须保留
        store.put_writes("t1", "cp2", makeWrite("w3", 2));
        XX_TEST_EXPECT_EQ(store.pending_writes_count("t1", "cp2"), (size_t)1);
        auto ws = store.get_writes("t1", "cp2");
        XX_TEST_EXPECT_EQ(ws.size(), (size_t)1);
        XX_TEST_EXPECT_EQ(ws[0].task_id, std::string("w3"));

        // 再次 save cp2 (如 update_state 覆盖同一 cp id 时不应误删其 writes):
        // saveImpl 覆盖 latest_, evictImpl 只清理非 keepId 的 writes
        store.put_writes("t1", "cp2", makeWrite("w4", 2));
        store.save(makeCp("t1", "cp2", 2, "v2-updated"));
        XX_TEST_EXPECT_EQ(store.pending_writes_count("t1", "cp2"), (size_t)2);

        // clear_writes
        store.clear_writes("t1", "cp2");
        XX_TEST_EXPECT_EQ(store.pending_writes_count("t1", "cp2"), (size_t)0);
    }

    // ── 5. 跨 thread 的 pending writes 不受影响 ─────────────────────────
    {
        agentxx::agent::InMemorySingleCheckpointStore store;
        store.save(makeCp("tA", "a1", 1, "A"));
        store.save(makeCp("tB", "b1", 1, "B"));
        store.put_writes("tA", "a1", makeWrite("wa", 1));
        store.put_writes("tB", "b1", makeWrite("wb", 1));

        // tA 推进到 a2: 只清理 tA 的旧 writes, tB 的不动
        store.save(makeCp("tA", "a2", 2, "A2"));
        XX_TEST_EXPECT_EQ(store.pending_writes_count("tA", "a1"), (size_t)0);
        XX_TEST_EXPECT_EQ(store.pending_writes_count("tB", "b1"), (size_t)1);
    }

    // ── 6. delete_thread ────────────────────────────────────────────────
    {
        agentxx::agent::InMemorySingleCheckpointStore store;
        store.save(makeCp("t1", "cp1", 1, "v1"));
        store.save(makeCp("t2", "cp2", 1, "v2"));
        store.put_writes("t1", "cp1", makeWrite("w1", 1));
        store.put_writes("t2", "cp2", makeWrite("w2", 1));

        store.delete_thread("t1");
        XX_TEST_EXPECT_EQ(store.size(), (size_t)1);
        XX_TEST_EXPECT_FALSE(store.load_latest("t1").has_value());
        XX_TEST_EXPECT_FALSE(store.load_by_id("cp1").has_value());
        XX_TEST_EXPECT_EQ(store.pending_writes_count("t1", "cp1"), (size_t)0);
        // t2 未受影响
        XX_TEST_EXPECT_TRUE(store.load_latest("t2").has_value());
        XX_TEST_EXPECT_EQ(store.pending_writes_count("t2", "cp2"), (size_t)1);
    }

    // ── 7. async 接口 (engine 实际调用路径) ─────────────────────────────
    {
        agentxx::agent::InMemorySingleCheckpointStore store;

        co_await store.save_async(makeCp("t1", "cp1", 1, "v1"));
        co_await store.save_async(makeCp("t1", "cp2", 2, "v2"));

        auto latest = co_await store.load_latest_async("t1");
        XX_TEST_EXPECT_TRUE(latest.has_value());
        XX_TEST_EXPECT_EQ(latest->id, std::string("cp2"));

        auto byId = co_await store.load_by_id_async("cp2");
        XX_TEST_EXPECT_TRUE(byId.has_value());
        auto oldById = co_await store.load_by_id_async("cp1");
        XX_TEST_EXPECT_FALSE(oldById.has_value());

        auto ls = co_await store.list_async("t1");
        XX_TEST_EXPECT_EQ(ls.size(), (size_t)1);

        co_await store.put_writes_async("t1", "cp2", makeWrite("w-async", 2));
        auto ws = co_await store.get_writes_async("t1", "cp2");
        XX_TEST_EXPECT_EQ(ws.size(), (size_t)1);
        XX_TEST_EXPECT_EQ(ws[0].task_id, std::string("w-async"));

        // save cp3: 通过 async 路径淘汰 cp2 的 writes
        co_await store.save_async(makeCp("t1", "cp3", 3, "v3"));
        auto ws2 = co_await store.get_writes_async("t1", "cp2");
        XX_TEST_EXPECT_EQ(ws2.size(), (size_t)0);
        auto ws3 = co_await store.get_writes_async("t1", "cp3");
        XX_TEST_EXPECT_EQ(ws3.size(), (size_t)0); // cp3 本身未挂载 writes

        co_await store.clear_writes_async("t1", "cp3");
        co_await store.delete_thread_async("t1");
        XX_TEST_EXPECT_EQ(store.size(), (size_t)0);
    }

    co_return TestResult{g_cs_passed, g_cs_failed};
}

} // namespace test
} // namespace agentxx
