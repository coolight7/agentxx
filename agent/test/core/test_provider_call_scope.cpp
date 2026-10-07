/// test_provider_call_scope —— 单次 provider 调用的取消域 (计划 LLM-7)
///
/// 验证 [agentxx::nodes::ProviderCallScope] 的语义:
/// - 父令牌取消 → 级联到调用域 (运行取消仍然中止进行中的调用)
/// - 调用域单独取消 → 父令牌不受影响 (只中止本次调用, 不取消整轮)
/// - `markDone()` 后析构不取消 (调用已正常收尾)
/// - 未 `markDone()` 就析构 (消费方放弃) → 调用域被取消
/// - 父令牌为空时使用独立令牌 (不被外部取消)
#include "agentxx-test/core/test_provider_call_scope.h"

#include "agentxx/nodes/provider_call_scope.h"
#include "neograph/graph/cancel.h"
#include <memory>
#include <string>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_pcs_passed = 0;
int g_pcs_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_pcs_passed
#define XX_TEST_FAILED g_pcs_failed

namespace agentxx {
namespace test {

namespace {

using agentxx::nodes::ProviderCallScope;

/// 作用域: 取消级联与隔离
void testScopeCancelSemantics() {
    // 1) 运行取消 (父令牌) 级联到调用域
    {
        auto run = std::make_shared<neograph::graph::CancelToken>();
        ProviderCallScope scope{run};
        XX_TEST_EXPECT_FALSE(scope.cancelled());
        XX_TEST_EXPECT_TRUE(scope.token() != run); // 不是运行令牌本身
        run->cancel();
        XX_TEST_EXPECT_TRUE(scope.cancelled());
        XX_TEST_EXPECT_TRUE(run->is_cancelled());
    }

    // 2) 调用域单独取消不影响父令牌 (cancel 只中止本次调用)
    {
        auto              run = std::make_shared<neograph::graph::CancelToken>();
        ProviderCallScope scope{run};
        scope.token()->cancel();
        XX_TEST_EXPECT_TRUE(scope.cancelled());
        XX_TEST_EXPECT_FALSE(run->is_cancelled());
    }

    // 3) 每个调用域互不影响 (同一轮次内的多次调用各自独立)
    {
        auto              run = std::make_shared<neograph::graph::CancelToken>();
        ProviderCallScope first{run};
        ProviderCallScope second{run};
        XX_TEST_EXPECT_TRUE(first.token() != second.token());
        first.token()->cancel();
        XX_TEST_EXPECT_TRUE(first.cancelled());
        XX_TEST_EXPECT_FALSE(second.cancelled());
        XX_TEST_EXPECT_FALSE(run->is_cancelled());
    }
}

/// 作用域: RAII 收尾语义
void testScopeRaiiSemantics() {
    // 1) markDone 后析构: 不取消
    {
        auto run = std::make_shared<neograph::graph::CancelToken>();
        auto child = std::shared_ptr<neograph::graph::CancelToken>{};
        {
            ProviderCallScope scope{run};
            child = scope.token();
            scope.markDone();
        }
        XX_TEST_EXPECT_FALSE(child->is_cancelled());
        XX_TEST_EXPECT_FALSE(run->is_cancelled());
    }

    // 2) 未 markDone 就析构 (消费方放弃): 调用域被取消, 父令牌不受影响
    {
        auto run = std::make_shared<neograph::graph::CancelToken>();
        auto child = std::shared_ptr<neograph::graph::CancelToken>{};
        {
            ProviderCallScope scope{run};
            child = scope.token();
        }
        XX_TEST_EXPECT_TRUE(child->is_cancelled());
        XX_TEST_EXPECT_FALSE(run->is_cancelled());
    }

    // 3) 父令牌为空: 独立令牌, 外部无从取消
    {
        auto child = std::shared_ptr<neograph::graph::CancelToken>{};
        {
            ProviderCallScope scope{nullptr};
            child = scope.token();
            XX_TEST_EXPECT_TRUE(child != nullptr);
            XX_TEST_EXPECT_FALSE(scope.cancelled());
            scope.markDone();
        }
        XX_TEST_EXPECT_FALSE(child->is_cancelled());
    }

    // 4) 父令牌已取消时新建调用域: 立即处于取消状态 (eager 传播), 不抛异常
    {
        auto run = std::make_shared<neograph::graph::CancelToken>();
        run->cancel();
        ProviderCallScope scope{run};
        XX_TEST_EXPECT_TRUE(scope.cancelled());
    }
}

} // namespace

TestResult testProviderCallScope() {
    g_pcs_passed = 0;
    g_pcs_failed = 0;
    testScopeCancelSemantics();
    testScopeRaiiSemantics();
    return TestResult{g_pcs_passed, g_pcs_failed};
}

} // namespace test
} // namespace agentxx
