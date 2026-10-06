/// test_permission —— 权限判定理由与执行前目标复验 (计划 SEC-2 / SEC-5 / TST-10)
///
/// 覆盖:
/// - SEC-2 判定理由 (PermissionReason): 同一套判定口径下每个分支都给出理由 ——
///   未解析的目标、工作区隔离写拒绝、配置显式拒绝(优先于完全授权)、完全授权、
///   命中规则、未命中规则按模式默认兜底; 理由文本与命中规则/目标一并给出
/// - SEC-5 执行前目标复验: 判定阶段记录"已批准目标", 执行前用当前参数按同一口径
///   复验 —— 参数被改写时拒绝执行; 未声明权限的工具无约束
/// - 门禁正确性 (TST-10 收窄用例集): 配置拒绝优先于完全授权、工作区隔离写拒绝
///   优先于白名单、未声明权限的工具直接放行
#include "agentxx-test/core/test_permission.h"

#include "agentxx/agent/context.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/middlewares/permission.h"
#include "asio/io_context.hpp"
#include "asio/this_coro.hpp"
#include "utilxx_base/json.h"
#include <memory>
#include <string>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_perm_passed = 0;
int g_perm_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_perm_passed
#define XX_TEST_FAILED g_perm_failed

namespace agentxx {
namespace test {

using namespace agentxx::middleware;

namespace {

/// 造一个只含权限中间件所需字段的 AgentContext (无需会话/插件)
std::shared_ptr<agentxx::agent::AgentContext> makePermContext() {
    auto ctx                     = std::make_shared<agentxx::agent::AgentContext>();
    ctx->agentConfig             = std::make_shared<agentxx::agent::AgentConfig>();
    ctx->middlewareHandleContext = std::make_shared<agentxx::middleware::MiddlewareContext>();
    return ctx;
}

/// 声明一个"路径目标 + 读作用域"的测试工具权限
void declareReadTool(
    const std::shared_ptr<PermissionMiddlewareHandle>& perm,
    std::string_view                                   toolName
) {
    ToolPermissionSpec spec;
    spec.scope      = PermissionMiddlewareHandle::FilesystemPermissionREAD;
    spec.targetKind = ToolPermissionTargetKind::Path;
    spec.targetArgs = {"path"};
    spec.category   = "read";
    perm->registerToolPermission(toolName, std::move(spec));
}

} // namespace

asio::awaitable<TestResult> run_permission_tests() {
    g_perm_passed = 0;
    g_perm_failed = 0;

    auto ctx  = makePermContext();
    auto perm = std::make_shared<PermissionMiddlewareHandle>(ctx);

    const auto readScope  = PermissionMiddlewareHandle::FilesystemPermissionREAD;
    const auto writeScope = PermissionMiddlewareHandle::FilesystemPermissionWRITE;

    // 规范化基准 (工作目录): 用会话工作目录下的路径做判定
    const auto baseDir     = perm->normalizePermissionPath("./perm_test_base/", "");
    const auto allowedFile = perm->normalizePermissionPath("./perm_test_base/allowed.txt", "");
    const auto deniedFile  = perm->normalizePermissionPath("./perm_test_base/secret.txt", "");
    const auto otherFile   = perm->normalizePermissionPath("./perm_test_other/file.txt", "");
    const auto worktreeDir = perm->normalizePermissionPath("./perm_test_base/.wt/", "");
    XX_TEST_EXPECT_FALSE(baseDir.empty());

    // ---------------- SEC-2: 未解析的目标 ----------------

    {
        const auto d = perm->explainTarget("", readScope, "");
        XX_TEST_EXPECT_TRUE(d.decision == PathDecision::Ask);
        XX_TEST_EXPECT_TRUE(d.reason == PermissionReason::Unresolved);
        XX_TEST_EXPECT_TRUE(d.describe().find("could not be resolved") != std::string::npos);
    }

    // ---------------- SEC-2: 未命中规则 → 按模式默认兜底 ----------------

    {
        perm->noRuleOperator = PermissionOperator::ALLOW;
        auto d               = perm->explainTarget(otherFile, readScope, "");
        XX_TEST_EXPECT_TRUE(d.decision == PathDecision::Allow);
        XX_TEST_EXPECT_TRUE(d.reason == PermissionReason::NoRuleDefault);
        XX_TEST_EXPECT_TRUE(d.rule.empty());

        perm->noRuleOperator = PermissionOperator::INTERRUPT;
        d                    = perm->explainTarget(otherFile, readScope, "");
        XX_TEST_EXPECT_TRUE(d.decision == PathDecision::Ask);
        XX_TEST_EXPECT_TRUE(d.reason == PermissionReason::NoRuleDefault);

        perm->noRuleOperator = PermissionOperator::DENY;
        d                    = perm->explainTarget(otherFile, readScope, "");
        XX_TEST_EXPECT_TRUE(d.decision == PathDecision::Deny);
        XX_TEST_EXPECT_TRUE(d.reason == PermissionReason::NoRuleDefault);
        perm->noRuleOperator = PermissionOperator::ALLOW;
    }

    // ---------------- SEC-2: 命中规则 (允许 / 拒绝 / 询问) ----------------

    {
        perm->setFilesystemPermission(allowedFile, PermissionOperator::ALLOW, readScope);
        const auto d = perm->explainTarget(allowedFile, readScope, "");
        XX_TEST_EXPECT_TRUE(d.decision == PathDecision::Allow);
        XX_TEST_EXPECT_TRUE(d.reason == PermissionReason::Rule);
        XX_TEST_EXPECT_FALSE(d.rule.empty());
        XX_TEST_EXPECT_EQ(d.target, allowedFile);
        XX_TEST_EXPECT_TRUE(d.describe().find("matched a registered permission rule") != std::string::npos);
    }
    {
        perm->setFilesystemPermission(deniedFile, PermissionOperator::DENY, readScope);
        const auto d = perm->explainTarget(deniedFile, readScope, "");
        XX_TEST_EXPECT_TRUE(d.decision == PathDecision::Deny);
        XX_TEST_EXPECT_TRUE(d.reason == PermissionReason::Rule);

        perm->setFilesystemPermission(otherFile, PermissionOperator::INTERRUPT, readScope);
        const auto ask = perm->explainTarget(otherFile, readScope, "");
        XX_TEST_EXPECT_TRUE(ask.decision == PathDecision::Ask);
        XX_TEST_EXPECT_TRUE(ask.reason == PermissionReason::Rule);
    }

    // ---------------- SEC-2: 完全授权 ----------------

    {
        perm->setFullAuthorized(true);
        const auto d = perm->explainTarget(otherFile, readScope, "");
        XX_TEST_EXPECT_TRUE(d.decision == PathDecision::Allow);
        XX_TEST_EXPECT_TRUE(d.reason == PermissionReason::FullAuth);
        perm->setFullAuthorized(false);
    }

    // ---------------- SEC-2 + TST-10: 配置显式拒绝优先于完全授权 ----------------

    {
        perm->addConfigDenyPath(deniedFile);
        perm->setFullAuthorized(true);
        const auto d = perm->explainTarget(deniedFile, readScope, "");
        XX_TEST_EXPECT_TRUE(d.decision == PathDecision::Deny);
        XX_TEST_EXPECT_TRUE(d.reason == PermissionReason::ConfigDeny);
        XX_TEST_EXPECT_TRUE(d.describe().find("configuration deny") != std::string::npos);
        perm->setFullAuthorized(false);
    }

    // ---------------- SEC-2 + TST-10: 工作区隔离写拒绝优先于白名单 ----------------

    {
        // worktree 子树内允许写, 主检出其余位置拒绝写 (读不受限)
        perm->setFilesystemPermission(baseDir, PermissionOperator::ALLOW, writeScope);
        SessionFsIsolation iso;
        iso.allowPath     = worktreeDir;
        iso.denyWritePath = baseDir;
        perm->setSessionIsolation("sess-wt", iso);

        const auto writeMain = perm->explainTarget(allowedFile, writeScope, "sess-wt");
        XX_TEST_EXPECT_TRUE(writeMain.decision == PathDecision::Deny);
        XX_TEST_EXPECT_TRUE(writeMain.reason == PermissionReason::WorktreeIsolation);

        const auto writeWt = perm->explainTarget(
            perm->normalizePermissionPath("./perm_test_base/.wt/a.txt", ""),
            writeScope,
            "sess-wt"
        );
        XX_TEST_EXPECT_TRUE(writeWt.decision == PathDecision::Allow);
        XX_TEST_EXPECT_TRUE(writeWt.reason == PermissionReason::Rule);

        // 读不受隔离约束 (同一路径读仍是白名单允许)
        const auto readMain = perm->explainTarget(allowedFile, readScope, "sess-wt");
        XX_TEST_EXPECT_TRUE(readMain.decision == PathDecision::Allow);

        perm->clearSessionIsolation("sess-wt");
        const auto writeAfterClear = perm->explainTarget(allowedFile, writeScope, "sess-wt");
        XX_TEST_EXPECT_TRUE(writeAfterClear.decision == PathDecision::Allow);
    }

    // ---------------- SEC-5: 执行前目标复验 ----------------

    {
        declareReadTool(perm, "perm_test_read");

        utilxx_base::Json args = utilxx_base::Json::object();
        args["sessionId"]      = "sess-reverify";
        args["tool_call_id"]   = "call-1";
        args["path"]           = "./perm_test_base/allowed.txt";

        std::string reason;
        const bool  allow = co_await perm->checkToolPermission("perm_test_read", args, &reason);
        XX_TEST_EXPECT_TRUE(allow);

        // 参数未变: 复验通过
        auto rv = perm->reverifyApprovedTargets("perm_test_read", "call-1", args);
        XX_TEST_EXPECT_TRUE(rv.ok);
        XX_TEST_EXPECT_TRUE(rv.reason.empty());

        // 参数被改写 (换成另一个目标): 复验拒绝, 理由带批准与执行两边的目标
        auto tampered          = args;
        tampered["path"]       = "./perm_test_other/other.txt";
        const auto rvChanged   = perm->reverifyApprovedTargets("perm_test_read", "call-1", tampered);
        XX_TEST_EXPECT_FALSE(rvChanged.ok);
        XX_TEST_EXPECT_TRUE(rvChanged.reason.find("permission target changed") != std::string::npos);
        XX_TEST_EXPECT_TRUE(rvChanged.reason.find("perm_test_other") != std::string::npos);

        // 未经过判定的工具调用 (未声明权限): 无约束, 复验通过
        const auto rvUnconstrained = perm->reverifyApprovedTargets("no_such_tool", "call-x", args);
        XX_TEST_EXPECT_TRUE(rvUnconstrained.ok);

        // 未声明权限的工具: 直接放行 (TST-10 收窄用例)
        utilxx_base::Json unknownArgs = utilxx_base::Json::object();
        unknownArgs["path"]           = "./perm_test_other/other.txt";
        std::string unknownReason;
        XX_TEST_EXPECT_TRUE(
            co_await perm->checkToolPermission("no_such_tool", unknownArgs, &unknownReason)
        );

        // 声明过权限但被规则拒绝的工具: 判定与复验都不成立 (未记录批准)
        utilxx_base::Json deniedArgs = utilxx_base::Json::object();
        deniedArgs["sessionId"]      = "sess-reverify";
        deniedArgs["tool_call_id"]   = "call-2";
        deniedArgs["path"]           = "./perm_test_base/secret.txt";
        std::string deniedReason;
        XX_TEST_EXPECT_FALSE(
            co_await perm->checkToolPermission("perm_test_read", deniedArgs, &deniedReason)
        );
        XX_TEST_EXPECT_FALSE(deniedReason.empty());

        // 批量路径查询 (check_paths 同口径): 明确允许 / 明确拒绝 / 未获批准
        const std::vector<std::string> batch{
            "./perm_test_base/allowed.txt",
            "./perm_test_base/secret.txt",
            "./perm_test_other/file.txt",
        };
        const auto decisions = perm->decidePaths(batch, readScope, "sess-reverify");
        XX_TEST_EXPECT_EQ(decisions.size(), size_t{3});
        if (decisions.size() == 3) {
            XX_TEST_EXPECT_TRUE(decisions[0] == PathDecision::Allow);
            XX_TEST_EXPECT_TRUE(decisions[1] == PathDecision::Deny);
            XX_TEST_EXPECT_TRUE(decisions[2] == PathDecision::Ask);
        }
    }

    co_return TestResult{g_perm_passed, g_perm_failed};
}

} // namespace test
} // namespace agentxx
