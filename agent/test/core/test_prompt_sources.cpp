// 运行时提示词来源注册表 (PromptSourceRegistry) 测试
//
// 设计要点 (见 [prompt_sources.h](/agent/lib/include/agentxx/agent/prompt_sources.h)):
// - 唯一的刷新入口是 model call 的 start 钩子, 注册表本身不做文件监听;
//   本模块直接调用 ensureFresh, 逐个验证"什么时候重取、什么时候不动"
// - 内容未变化时不制造段落变化 (否则每次刷新都会让 system 前缀失效)
// - 单来源失败/超时/没有意见 → 保留上次文本, 只记失败次数
// - perSession 来源按会话各存一份, 会话回收时释放

#include "agentxx-test/core/test_prompt_sources.h"

#include "agentxx/agent/prompt_sources.h"
#include "asio/this_coro.hpp"
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_ps_passed = 0;
int g_ps_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_ps_passed
#define XX_TEST_FAILED g_ps_failed

namespace agentxx {
namespace test {

namespace {

using agentxx::agent::PromptRefreshContext;
using agentxx::agent::PromptRefreshReason;
using agentxx::agent::PromptSectionSlot;
using agentxx::agent::PromptSource;
using agentxx::agent::PromptSourceRegistry;

/// 构造一个"返回固定文本"的来源; buildCount 记录取文本次数
PromptSource makeTextSource(
    std::string              key,
    std::string              owner,
    int                      order,
    std::string              text,
    std::shared_ptr<size_t>  buildCount,
    bool                     perSession = false,
    bool                     refreshOnCompaction = true,
    PromptSectionSlot        slot = PromptSectionSlot::Stable
) {
    PromptSource src;
    src.key                 = std::move(key);
    src.owner               = std::move(owner);
    src.order               = order;
    src.slot                = slot;
    src.perSession          = perSession;
    src.refreshOnCompaction = refreshOnCompaction;
    src.timeout             = std::chrono::milliseconds{500};
    src.build               = [text = std::move(text), buildCount](const PromptRefreshContext&)
        -> asio::awaitable<std::optional<std::string>> {
        ++(*buildCount);
        co_return text;
    };
    return src;
}

/// 取某个会话可见的稳定段文本 (按 key 查找; 没有该段时返回空串)
std::string sectionOf(const PromptSourceRegistry& registry, std::string_view key, std::string_view session)
{
    for (const auto& view : registry.stableSections(session)) {
        if (view.key == key) {
            return std::string{view.text};
        }
    }
    return {};
}

} // namespace

asio::awaitable<TestResult> run_prompt_source_tests() {
    const int passedBefore = g_ps_passed;
    const int failedBefore = g_ps_failed;

    // ================= T1. 首次读取 + 内容未变不重复读盘 (A1/A2) =================
    {
        PromptSourceRegistry   registry;
        auto                   builds = std::make_shared<size_t>(0);
        PromptRefreshContext   ctx;
        ctx.sessionId = "s1";

        registry.registerSource(
            makeTextSource("alpha", "core:alpha", 10, "ALPHA-TEXT", builds)
        );
        XX_TEST_EXPECT_EQ(registry.sourceCount(), size_t{1});
        XX_TEST_EXPECT_TRUE(registry.isDirty("s1"));

        co_await registry.ensureFresh("s1", ctx);
        XX_TEST_EXPECT_EQ(*builds, size_t{1});
        XX_TEST_EXPECT_EQ(sectionOf(registry, "alpha", "s1"), std::string{"ALPHA-TEXT"});
        XX_TEST_EXPECT_FALSE(registry.isDirty("s1"));

        // 没有标记 → 一次布尔判断后直接返回, 不读盘
        co_await registry.ensureFresh("s1", ctx);
        XX_TEST_EXPECT_EQ(*builds, size_t{1});

        // 压缩完成 + 内容未变 → 重取了文本, 但段落没有变化 (changed 仍为 0)
        auto stats = registry.takeRefreshStats();
        (void)stats;
        registry.markDirty("s1", PromptRefreshReason::AfterCompaction);
        co_await registry.ensureFresh("s1", ctx);
        XX_TEST_EXPECT_EQ(*builds, size_t{2});
        stats = registry.takeRefreshStats();
        XX_TEST_EXPECT_EQ(stats.refreshes, uint64_t{1});
        XX_TEST_EXPECT_EQ(stats.changedSections, uint64_t{0});
        XX_TEST_EXPECT_EQ(sectionOf(registry, "alpha", "s1"), std::string{"ALPHA-TEXT"});
    }

    // ================= T2. 内容变化 → 段落变化恰好一次 (A3) =================
    {
        PromptSourceRegistry registry;
        auto                 builds = std::make_shared<size_t>(0);
        PromptRefreshContext ctx;
        ctx.sessionId = "s2";

        // 文本由外部变量驱动: 模拟"文件内容变了"
        auto text = std::make_shared<std::string>("V1");
        PromptSource src;
        src.key     = "mem";
        src.owner   = "core:mem";
        src.order   = 100;
        src.timeout = std::chrono::milliseconds{500};
        src.build   = [text, builds](const PromptRefreshContext&)
            -> asio::awaitable<std::optional<std::string>> {
            ++(*builds);
            co_return *text;
        };
        registry.registerSource(std::move(src));

        co_await registry.ensureFresh("s2", ctx);
        XX_TEST_EXPECT_EQ(sectionOf(registry, "mem", "s2"), std::string{"V1"});
        (void)registry.takeRefreshStats();

        // 未标记时不刷新 (即使内容已在磁盘上变了)
        *text = "V2";
        co_await registry.ensureFresh("s2", ctx);
        XX_TEST_EXPECT_EQ(sectionOf(registry, "mem", "s2"), std::string{"V1"});

        // 压缩完成 → 重取 → 段落更新, 变化计数 +1
        registry.markDirty("s2", PromptRefreshReason::AfterCompaction);
        co_await registry.ensureFresh("s2", ctx);
        XX_TEST_EXPECT_EQ(sectionOf(registry, "mem", "s2"), std::string{"V2"});
        auto stats = registry.takeRefreshStats();
        XX_TEST_EXPECT_EQ(stats.changedSections, uint64_t{1});
    }

    // ================= T3. refreshOnCompaction=false / 资源变更强制重取 =================
    {
        PromptSourceRegistry registry;
        auto                 builds = std::make_shared<size_t>(0);
        PromptRefreshContext ctx;
        ctx.sessionId = "s3";
        registry.registerSource(
            makeTextSource("static", "core:static", 10, "S", builds, false, /*refreshOnCompaction=*/false)
        );

        co_await registry.ensureFresh("s3", ctx);
        XX_TEST_EXPECT_EQ(*builds, size_t{1});

        registry.markDirty("s3", PromptRefreshReason::AfterCompaction);
        co_await registry.ensureFresh("s3", ctx);
        XX_TEST_EXPECT_EQ(*builds, size_t{1}); // 压缩后不刷新

        registry.markDirty("s3", PromptRefreshReason::ResourcesChanged);
        co_await registry.ensureFresh("s3", ctx);
        XX_TEST_EXPECT_EQ(*builds, size_t{2});

        registry.markDirty("s3", PromptRefreshReason::Explicit);
        co_await registry.ensureFresh("s3", ctx);
        XX_TEST_EXPECT_EQ(*builds, size_t{3});
    }

    // ================= T4. 失败 / 没有意见 / 空串 =================
    {
        PromptSourceRegistry registry;
        PromptRefreshContext ctx;
        ctx.sessionId = "s4";

        auto mode     = std::make_shared<int>(0); // 0=正常 1=抛异常 2=nullopt 3=空串
        PromptSource src;
        src.key     = "flaky";
        src.owner   = "core:flaky";
        src.timeout = std::chrono::milliseconds{500};
        src.build   = [mode](const PromptRefreshContext&) -> asio::awaitable<std::optional<std::string>> {
            if (*mode == 1) {
                throw std::runtime_error("boom");
            }
            if (*mode == 2) {
                co_return std::nullopt;
            }
            if (*mode == 3) {
                co_return std::string{};
            }
            co_return std::string{"OK"};
        };
        registry.registerSource(std::move(src));

        co_await registry.ensureFresh("s4", ctx);
        XX_TEST_EXPECT_EQ(sectionOf(registry, "flaky", "s4"), std::string{"OK"});

        // 抛异常: 保留旧文本 + 计一次失败
        *mode = 1;
        registry.markDirty("s4", PromptRefreshReason::Explicit);
        co_await registry.ensureFresh("s4", ctx);
        XX_TEST_EXPECT_EQ(sectionOf(registry, "flaky", "s4"), std::string{"OK"});
        XX_TEST_EXPECT_EQ(registry.takeRefreshStats().failures, uint64_t{1});

        // nullopt (没有意见): 保留旧文本, 不算失败
        *mode = 2;
        registry.markDirty("s4", PromptRefreshReason::Explicit);
        co_await registry.ensureFresh("s4", ctx);
        XX_TEST_EXPECT_EQ(sectionOf(registry, "flaky", "s4"), std::string{"OK"});
        XX_TEST_EXPECT_EQ(registry.takeRefreshStats().failures, uint64_t{0});

        // 空串 (该段落没有内容): 清空段落
        *mode = 3;
        registry.markDirty("s4", PromptRefreshReason::Explicit);
        co_await registry.ensureFresh("s4", ctx);
        XX_TEST_EXPECT_EQ(sectionOf(registry, "flaky", "s4"), std::string{});
    }

    // ================= T5. 按会话隔离 + 会话回收 (A7/A8) =================
    {
        PromptSourceRegistry registry;
        auto                 builds = std::make_shared<size_t>(0);

        // 文本随会话工作目录变化 (与记忆文件的相对路径解析同构)
        PromptSource src;
        src.key        = "memory";
        src.owner      = "core:memory";
        src.order      = 100;
        src.perSession = true;
        src.timeout    = std::chrono::milliseconds{500};
        src.build      = [builds](const PromptRefreshContext& rc)
            -> asio::awaitable<std::optional<std::string>> {
            ++(*builds);
            co_return std::string{"workdir="} + rc.workDir;
        };
        registry.registerSource(std::move(src));

        PromptRefreshContext ctxA;
        ctxA.sessionId = "A";
        ctxA.workDir   = "/proj/a";
        PromptRefreshContext ctxB;
        ctxB.sessionId = "B";
        ctxB.workDir   = "/proj/b";

        co_await registry.ensureFresh("A", ctxA);
        co_await registry.ensureFresh("B", ctxB);
        XX_TEST_EXPECT_EQ(sectionOf(registry, "memory", "A"), std::string{"workdir=/proj/a"});
        XX_TEST_EXPECT_EQ(sectionOf(registry, "memory", "B"), std::string{"workdir=/proj/b"});
        XX_TEST_EXPECT_EQ(*builds, size_t{2});
        XX_TEST_EXPECT_EQ(registry.sessionCount(), size_t{2});

        // 会话回收: 该会话的状态与文本都释放 (共享来源不受影响)
        registry.dropSession("A");
        XX_TEST_EXPECT_EQ(registry.sessionCount(), size_t{1});
        XX_TEST_EXPECT_EQ(sectionOf(registry, "memory", "A"), std::string{});
        XX_TEST_EXPECT_EQ(sectionOf(registry, "memory", "B"), std::string{"workdir=/proj/b"});
    }

    // ================= T6. 排序 / 撤销 / 快照 =================
    {
        PromptSourceRegistry registry;
        auto                 builds = std::make_shared<size_t>(0);
        PromptRefreshContext ctx;
        ctx.sessionId = "s6";

        registry.registerSource(makeTextSource("zeta", "core:zeta", 300, "Z", builds));
        registry.registerSource(makeTextSource("alpha", "core:alpha", 100, "A", builds));
        registry.registerSource(makeTextSource("beta", "core:beta", 100, "B", builds));
        // 动态段不参与稳定段视图
        registry.registerSource(
            makeTextSource("dyn", "test:dyn", 50, "D", builds, false, true, PromptSectionSlot::Dynamic)
        );

        co_await registry.ensureFresh("s6", ctx);
        auto stable = registry.stableSections("s6");
        XX_TEST_EXPECT_EQ(stable.size(), size_t{3});
        if (stable.size() == 3) {
            // 按 (order, 键名) 排序: alpha(100) < beta(100) < zeta(300)
            XX_TEST_EXPECT_EQ(std::string{stable[0].key}, std::string{"alpha"});
            XX_TEST_EXPECT_EQ(std::string{stable[1].key}, std::string{"beta"});
            XX_TEST_EXPECT_EQ(std::string{stable[2].key}, std::string{"zeta"});
            XX_TEST_EXPECT_EQ(std::string{stable[0].owner}, std::string{"core:alpha"});
            XX_TEST_EXPECT_EQ(stable[0].scope, agentxx::agent::PromptSectionScope::Session);
        }
        auto dynamic = registry.dynamicSections("s6");
        XX_TEST_EXPECT_EQ(dynamic.size(), size_t{1});
        if (!dynamic.empty()) {
            XX_TEST_EXPECT_EQ(std::string{dynamic[0].key}, std::string{"dyn"});
            XX_TEST_EXPECT_EQ(dynamic[0].slot, PromptSectionSlot::Dynamic);
        }

        // 撤销: owner 不符拒绝, 匹配则删除
        XX_TEST_EXPECT_FALSE(registry.removeSource("alpha", "core:other"));
        XX_TEST_EXPECT_TRUE(registry.removeSource("alpha", "core:alpha"));
        XX_TEST_EXPECT_EQ(registry.sourceCount(), size_t{3});
        XX_TEST_EXPECT_EQ(sectionOf(registry, "alpha", "s6"), std::string{});

        // 快照: 含来源状态, 不含正文
        const auto snapshot = registry.snapshot();
        XX_TEST_EXPECT_EQ(snapshot.value("count", 0), 3);
        XX_TEST_EXPECT_TRUE(snapshot.dump().find("\"Z\"") == std::string::npos);
        XX_TEST_EXPECT_TRUE(snapshot.dump().find("zeta") != std::string::npos);
    }

    co_return TestResult{g_ps_passed - passedBefore, g_ps_failed - failedBefore};
}

} // namespace test
} // namespace agentxx
