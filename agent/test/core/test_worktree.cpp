#include "test_worktree.h"

#include "agentxx/util/string_util.h"
#include "agentxx/util/worktree.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace agentxx {
namespace test {

int g_wt_passed = 0;
int g_wt_failed = 0;

} // namespace test
} // namespace agentxx

// 断言计数宏覆盖: 放在全局作用域定义 (本文件后续所有 XX_TEST_EXPECT_* 生效),
// 不经头文件导出, 避免泄漏到其他测试模块
#define XX_TEST_PASSED agentxx::test::g_wt_passed
#define XX_TEST_FAILED agentxx::test::g_wt_failed

namespace agentxx {
namespace test {

namespace {

/// 递归删除目录 (忽略错误)
void rmRf(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
}

/// 创建唯一临时目录
std::filesystem::path makeTempDir(const std::string& tag) {
    static std::mt19937 rng{std::random_device{}()};
    auto                base = std::filesystem::temp_directory_path();
    for (int i = 0; i < 64; ++i) {
        auto            candidate = base / fmt::format("agentxx_wt_{}_{}", tag, rng() % 100000000);
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec)
            && std::filesystem::create_directories(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

/// 初始化测试仓库 (init + main 分支 + 首次提交); 返回仓库根, 失败返回空
std::string initTestRepo(const std::filesystem::path& dir) {
    namespace fw = agentxx::util::worktree;
    if (!fw::runGit({"init"}, dir.generic_string(), 30).ok()) {
        return {};
    }
    // 兼容默认分支名差异 (master/main): 统一切到 main
    fw::runGit({"symbolic-ref", "HEAD", "refs/heads/main"}, dir.generic_string(), 30);
    fw::runGit({"config", "user.email", "agentxx-test@example.com"}, dir.generic_string(), 30);
    fw::runGit({"config", "user.name", "agentxx-test"}, dir.generic_string(), 30);
    {
        std::ofstream f{dir / "README.md"};
        f << "# test repo\n";
    }
    if (!fw::runGit({"add", "-A"}, dir.generic_string(), 30).ok()) {
        return {};
    }
    if (!fw::runGit({"commit", "-m", "initial"}, dir.generic_string(), 30).ok()) {
        return {};
    }
    return dir.generic_string();
}

} // namespace

asio::awaitable<TestResult> run_worktree_tests() {
    namespace fw = agentxx::util::worktree;

    // ---- 前置: git 可用性 ----
    if (!fw::runGit({"--version"}, {}, 30).ok()) {
        TEST_SKIP << "git not available in PATH, worktree tests skipped" << std::endl;
        co_return TestResult{g_wt_passed, g_wt_failed};
    }

    // ==================== 名称清洗 ====================
    TEST_INFO << "---- sanitizeWorktreeName / isValidWorktreeName ----" << std::endl;
    {
        struct Case {
            const char* in;
            const char* expect;
        };

        static const std::vector<Case> cases{
            {"feature-a",        "feature-a"  },
            {"Fix_Bug.123",      "Fix_Bug.123"},
            {"../../etc/passwd", "etc-passwd" },
            {"a/b\\c:d*e?f",     "a-b-c-d-e-f"},
            {".hidden",          "hidden"     },
            {"-lead",            "lead"       },
            {"con",              "_con"       },
            {"aux.txt",          "_aux-txt"   },
            {"",                 ""           },
            {"!!!",              ""           },
        };
        int okCount = 0;
        for (const auto& c : cases) {
            auto got = fw::sanitizeWorktreeName(c.in);
            if (got == c.expect) {
                ++okCount;
            } else {
                XX_TEST_EXPECT_EQ(got, std::string{c.expect});
            }
        }
        XX_TEST_EXPECT_EQ(okCount, (int)cases.size());
        XX_TEST_EXPECT_TRUE(fw::isValidWorktreeName(fw::sanitizeWorktreeName("../../x")));
        XX_TEST_EXPECT_FALSE(fw::isValidWorktreeName("-bad"));
        XX_TEST_EXPECT_FALSE(fw::isValidWorktreeName(""));
        // 长名截断到 64
        XX_TEST_EXPECT_EQ(fw::sanitizeWorktreeName(std::string(80, 'a')).size(), (size_t)64);
    }

    // ==================== 生命周期 (真实 git) ====================
    auto tmpDir = makeTempDir("repo");
    if (tmpDir.empty()) {
        XX_TEST_FAILED++;
        TEST_FAIL << "cannot create temp dir" << std::endl;
        co_return TestResult{g_wt_passed, g_wt_failed};
    }
    auto repo = initTestRepo(tmpDir);
    if (repo.empty()) {
        TEST_WARN << "git init/commit failed in temp repo, lifecycle tests skipped" << std::endl;
        rmRf(tmpDir);
        co_return TestResult{g_wt_passed, g_wt_failed};
    }

    try {
        TEST_INFO << "---- repo detection ----" << std::endl;
        XX_TEST_EXPECT_TRUE(fw::isInsideWorkTree(repo));
        auto rootOpt = fw::repoRoot(repo);
        XX_TEST_EXPECT_HAS_VALUE(rootOpt);

        TEST_INFO << "---- create worktree ----" << std::endl;
        auto cr = fw::createWorktree(repo, "t-one");
        if (!cr.ok()) {
            XX_TEST_FAILED++;
            TEST_FAIL << "createWorktree failed: " << cr.err << std::endl;
        }
        XX_TEST_EXPECT_TRUE(cr.ok());
        auto wtPath = (std::filesystem::path{fw::worktreesRoot(repo)} / "t-one").generic_string();
        XX_TEST_EXPECT_TRUE(std::filesystem::exists(wtPath));
        // 分支校验
        auto br = fw::runGit({"rev-parse", "--abbrev-ref", "HEAD"}, wtPath, 30);
        XX_TEST_EXPECT_TRUE(br.ok() && br.out.find("agentxx/wt-t-one") != std::string::npos);

        TEST_INFO << "---- duplicate name rejected ----" << std::endl;
        auto dup = fw::createWorktree(repo, "t-one");
        XX_TEST_EXPECT_FALSE(dup.ok());

        TEST_INFO << "---- listWorktrees ----" << std::endl;
        auto entries = fw::listWorktrees(repo);
        bool found   = false;
        for (const auto& e : entries) {
            if (std::filesystem::path{e.path}.filename().generic_string() == "t-one"
                && e.branch == "agentxx/wt-t-one") {
                found = true;
            }
        }
        XX_TEST_EXPECT_TRUE(found);
        XX_TEST_EXPECT_TRUE(entries.size() >= 2);

        TEST_INFO << "---- .git/info/exclude idempotent ----" << std::endl;
        fw::ensureInfoExcluded(repo, ".agentxx/");
        fw::ensureInfoExcluded(repo, ".agentxx/");
        {
            std::ifstream in{std::filesystem::path{repo} / ".git" / "info" / "exclude"};
            std::string   content{
                std::istreambuf_iterator<char>(in),
                std::istreambuf_iterator<char>{}
            };
            size_t cnt = 0;
            for (size_t p = content.find(".agentxx/"); p != std::string::npos;
                 p        = content.find(".agentxx/", p + 1)) {
                ++cnt;
            }
            XX_TEST_EXPECT_EQ(cnt, (size_t)1);
        }

        TEST_INFO << "-- statusSummary: clean -> dirty -> committed(unmerged) --";
        auto stClean = fw::statusSummary(wtPath);
        XX_TEST_EXPECT_HAS_VALUE(stClean);
        if (stClean) {
            XX_TEST_EXPECT_FALSE(stClean->dirtyFiles());
            XX_TEST_EXPECT_FALSE(stClean->hasWork());
        }
        {
            std::ofstream f{wtPath + "/new_file.txt"};
            f << "hello\n";
        }
        auto stDirty = fw::statusSummary(wtPath);
        XX_TEST_EXPECT_HAS_VALUE(stDirty);
        if (stDirty) {
            XX_TEST_EXPECT_TRUE(stDirty->dirtyFiles());
        }
        // 提交后无上游: unmerged 应回退为整分支提交数 (>0), 保护已提交工作
        fw::runGit({"add", "-A"}, wtPath, 30);
        auto cm = fw::runGit({"commit", "-m", "wip feature"}, wtPath, 30);
        if (!cm.ok()) {
            XX_TEST_FAILED++;
            TEST_FAIL << "commit in worktree failed: " << cm.err << std::endl;
        }
        auto stCommitted = fw::statusSummary(wtPath);
        XX_TEST_EXPECT_HAS_VALUE(stCommitted);
        if (stCommitted) {
            XX_TEST_EXPECT_TRUE(stCommitted->hasWork());
            XX_TEST_EXPECT_TRUE(stCommitted->unmerged > 0);
        }

        TEST_INFO << "---- remove protection ----" << std::endl;
        auto refuse = fw::removeWorktreeByName(repo, "t-one", false);
        XX_TEST_EXPECT_FALSE(refuse.ok());
        auto forced = fw::removeWorktreeByName(repo, "t-one", true);
        if (!forced.ok()) {
            XX_TEST_FAILED++;
            TEST_FAIL << "force remove failed: " << forced.err << std::endl;
        }
        XX_TEST_EXPECT_FALSE(std::filesystem::exists(wtPath));
        // 空父链清理: .agentxx/agent/worktrees 应被回收 (best effort, 允许保留)
        if (std::filesystem::exists(std::filesystem::path{fw::worktreesRoot(repo)})) {
            TEST_INFO << "empty worktrees root kept (acceptable)" << std::endl;
        }

        TEST_INFO << "---- non-git dir detection ----" << std::endl;
        auto nogit = makeTempDir("nogit");
        XX_TEST_EXPECT_FALSE(fw::isInsideWorkTree(nogit.generic_string()));
        rmRf(nogit);
    } catch (const std::exception& ex) {
        XX_TEST_FAILED++;
        TEST_FAIL << "unexpected exception: " << ex.what() << std::endl;
    }

    rmRf(tmpDir);
    co_return TestResult{g_wt_passed, g_wt_failed};
}

} // namespace test
} // namespace agentxx
