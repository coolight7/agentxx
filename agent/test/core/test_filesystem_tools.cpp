#include "agentxx-test/core/test_filesystem_tools.h"
#include "agentxx/agent/context.h"
#include "agentxx/util/env.h"
#include <neograph/types.h>
// 原 lib 内置工具已迁移至 agentxx_filesystem 插件 (同名同行为); 测试直测
// 插件同一实现 (filesystem_impl.h), 保证插件行为与测试覆盖一致
#include "agentxx/event/event_stream.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/util/string_util.h"
#include "agentxx_filesystem/filesystem_impl.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_fs_passed = 0;
int g_fs_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_fs_passed
#define XX_TEST_FAILED g_fs_failed

namespace agentxx {
namespace tools {

/// 会话工作目录解析 (原工具经 AgentContext; 与插件 get_session_work_dir 同源):
/// ctx 未装配时回退进程 cwd, 相对路径用例语义不变
inline std::string testResolvedWorkDir(const std::weak_ptr<agentxx::agent::AgentContext>& ctx) {
    if (auto p = ctx.lock()) {
        if (p->agentConfig) {
            auto dir = p->agentConfig->resolvedWorkDir();
            if (!dir.empty()) {
                return dir;
            }
        }
    }
    return std::filesystem::current_path().generic_string();
}

/// 测试适配: 原工具类的同名薄包装 (execute_async 直调插件实现;
/// impl 的取消回调传 nullptr 等价无取消支持, 与原单测语义一致)
#define AGENTXX_TEST_FS_TOOL(NAME, IMPL_FN, TOOL_NAME, DEPICT)                               \
    struct NAME {                                                                            \
        std::weak_ptr<agentxx::agent::AgentContext> ctx;                                     \
        explicit NAME(std::weak_ptr<agentxx::agent::AgentContext> c) :                       \
            ctx(std::move(c)) {}                                                             \
        neograph::ChatTool get_definition() const {                                          \
            return {TOOL_NAME, DEPICT, {}};                                                  \
        }                                                                                    \
        asio::awaitable<std::string> execute_async(const agentxx::util::Json& args) const {  \
            co_return ::agentxx_fs_plugin::IMPL_FN(args, testResolvedWorkDir(ctx), nullptr); \
        }                                                                                    \
    };

/// 协程版测试适配 (read/write/edit): 直调插件 *ExecuteAsync 协程执行体 ——
/// 插件运行时经 poll 寄生驱动在实例 PollLoop 上 spawn (asio stream_file 真
/// 异步文件 I/O), 测试在宿主 io_context 上直接 co_await 同一实现, 覆盖一致
#define AGENTXX_TEST_FS_TOOL_ASYNC(NAME, IMPL_ASYNC_FN, TOOL_NAME, DEPICT)                         \
    struct NAME {                                                                                  \
        std::weak_ptr<agentxx::agent::AgentContext> ctx;                                           \
        explicit NAME(std::weak_ptr<agentxx::agent::AgentContext> c) :                             \
            ctx(std::move(c)) {}                                                                   \
        neograph::ChatTool get_definition() const {                                                \
            return {TOOL_NAME, DEPICT, {}};                                                        \
        }                                                                                          \
        asio::awaitable<std::string> execute_async(const agentxx::util::Json& args) const {        \
            co_return co_await ::agentxx_fs_plugin::IMPL_ASYNC_FN(args, testResolvedWorkDir(ctx)); \
        }                                                                                          \
    };

AGENTXX_TEST_FS_TOOL(
    FileSystemListTool,
    fileListExecute,
    "agentxx_filesystem_list",
    R"(List files and directories at a given path, output is multi-line text similar to `ls -l`, one entry per line: `type size last-modified-time path`.)"
)
AGENTXX_TEST_FS_TOOL_ASYNC(
    FilesystemReadTextFileTool,
    fileReadExecuteAsync,
    "agentxx_filesystem_read",
    R"(Read a text file (e.g. .txt, .md, .json, .log, source code) and return its contents with line numbers.)"
)
AGENTXX_TEST_FS_TOOL_ASYNC(
    FilesystemWriteFileTool,
    fileWriteExecuteAsync,
    "agentxx_filesystem_write",
    "Create a new file or overwrite an existing file with the given content."
)
AGENTXX_TEST_FS_TOOL_ASYNC(
    FilesystemEditTextFileTool,
    fileEditExecuteAsync,
    "agentxx_filesystem_edit",
    R"(Perform exact string replacement in a text file (e.g. *.txt, *.md, *.cpp, *.h).)"
)
AGENTXX_TEST_FS_TOOL(
    FilesystemGlobTool,
    fileGlobExecute,
    "agentxx_filesystem_glob",
    "Find files and directories matching glob patterns."
)
AGENTXX_TEST_FS_TOOL(
    FilesystemGrepTool,
    fileGrepExecute,
    "agentxx_filesystem_grep",
    R"(Search file contents using text or regular expressions.)"
)

#undef AGENTXX_TEST_FS_TOOL
#undef AGENTXX_TEST_FS_TOOL_ASYNC

} // namespace tools
} // namespace agentxx

namespace agentxx {
namespace test {

const std::string testDir
    = (std::filesystem::temp_directory_path() / "agentxx_test_filesystem").generic_string();

/// 计算 testDir 相对当前工作目录的路径 (供"相对路径解析"测试使用)。
/// 注意: Windows 上 temp 目录 (通常 C:) 与测试进程工作目录可能跨盘,
/// 此时 MSVC 的 std::filesystem::relative 会返回空路径 (跨盘无相对路径)。
/// 空则回退为绝对路径 (工具对绝对路径同样支持, 不影响验证目标),
/// 避免测试在跨盘环境下误报失败。
std::string testDirRelativeToCwd() {
    auto rel    = std::filesystem::relative(testDir, std::filesystem::current_path());
    auto relStr = rel.generic_string();
    return relStr.empty() ? testDir : relStr;
}

void setupTestDir() {
    namespace fs = std::filesystem;
    if (fs::exists(testDir)) {
        fs::remove_all(testDir);
    }
    fs::create_directories(testDir);

    std::ofstream f1(testDir + "/test1.txt");
    f1 << "line1\nline2\nline3\nline4\nline5\n";
    f1.close();

    std::ofstream f2(testDir + "/test2.txt");
    f2 << "hello world\nthis is a test file\n";
    f2.close();

    fs::create_directory(testDir + "/subdir");
    std::ofstream f3(testDir + "/subdir/subtest.txt");
    f3 << "subdir file content\n";
    f3.close();
}

void cleanupTestDir() {
    std::filesystem::remove_all(testDir);
}

asio::awaitable<void>
    test_list_file_get_definition(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FileSystemListTool{agentContext};
    auto def  = tool.get_definition();
    if (def.name == "agentxx_filesystem_list") {
        g_fs_passed++;
        TEST_PASS << "FileSystemListTool::get_definition() name correct" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FileSystemListTool::get_definition() name incorrect" << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_list_file_empty_path(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FileSystemListTool{agentContext};
    auto args = agentxx::util::Json{
        {"path", ""}
    };
    auto result = co_await tool.execute_async(args);
    if (agentxx::util::isIgnoreCaseContains(result, "error")) {
        g_fs_passed++;
        TEST_PASS << "FileSystemListTool returns error for empty path" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FileSystemListTool should return error for empty "
                     "path, got: "
                  << result << std::endl;
    }
    co_return;
}

asio::awaitable<void> test_list_file_basic(std::weak_ptr<agentxx::agent::AgentContext> agentContext
) {
    auto tool = agentxx::tools::FileSystemListTool{agentContext};
    auto args = agentxx::util::Json{
        {"path", testDir}
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("test1.txt") != std::string::npos
        && result.find("test2.txt") != std::string::npos
        && result.find("subdir") != std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FileSystemListTool lists directory contents" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FileSystemListTool listing failed, got: " << result << " path: " << testDir
                  << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_list_file_recursive(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FileSystemListTool{agentContext};
    auto args = agentxx::util::Json{
        {"path",      testDir},
        {"recursive", true   },
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("subtest.txt") != std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FileSystemListTool recursive lists subdirectories" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FileSystemListTool recursive listing failed, got: " << result
                  << " path: " << testDir << std::endl;
    }
    co_return;
}

asio::awaitable<void> test_list_file_limit(std::weak_ptr<agentxx::agent::AgentContext> agentContext
) {
    auto tool = agentxx::tools::FileSystemListTool{agentContext};
    auto args = agentxx::util::Json{
        {"path",  testDir},
        {"limit", 1      },
    };
    auto result = co_await tool.execute_async(args);
    // ls 风格多行文本: 限制条目数即限制行数
    size_t lineCount = 0;
    for (size_t i = 0; i < result.size(); i++) {
        if (result[i] == '\n') {
            lineCount++;
        }
    }
    if (lineCount <= 1) {
        g_fs_passed++;
        TEST_PASS << "FileSystemListTool respects limit parameter" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FileSystemListTool limit failed, got: " << result << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_list_file_info_fields(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FileSystemListTool{agentContext};
    auto args = agentxx::util::Json{
        {"path", testDir}
    };
    auto result = co_await tool.execute_async(args);
    // ls 风格: 每行应包含类型标识 (drwx/-rw-/lrwx/??????), 大小列, 时间列, 路径
    if (result.find("test1.txt") != std::string::npos
        && (result.find("-rw-") != std::string::npos || result.find("drwx") != std::string::npos
            || result.find("lrwx") != std::string::npos
            || result.find("??????") != std::string::npos)
        && result.find("20") != std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FileSystemListTool returns ls-style lines with type, size, time, path"
                  << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FileSystemListTool ls-style output missing fields, got: " << result
                  << std::endl;
    }
    co_return;
}

/// 相对路径应自动转换为绝对路径 (基于进程当前工作目录) 再处理。
/// 不修改进程 CWD: 计算 testDir 相对当前工作目录的路径传入, 工具内部解析后应能正确列出。
asio::awaitable<void>
    test_list_file_relative_path(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool    = agentxx::tools::FileSystemListTool{agentContext};
    auto relPath = testDirRelativeToCwd();
    auto args    = agentxx::util::Json{
           {"path", relPath}
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("test1.txt") != std::string::npos
        && result.find("test2.txt") != std::string::npos
        && result.find("subdir") != std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FileSystemListTool resolves relative path to absolute" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FileSystemListTool relative path failed, got: " << result
                  << " relPath: " << relPath << std::endl;
    }
    co_return;
}

/// 构造绑定指定会话工作目录的独立 AgentContext (work_dir 相关测试专用)
/// - 不依赖共享 agentContext: 验证工具以 AgentConfig::resolvedWorkDir 为
///   相对路径基准时, 结果与进程 cwd 无关 (嵌入场景单进程多 agent 实例
///   各自绑定独立项目目录的核心语义)
static std::shared_ptr<agentxx::agent::AgentContext> makeWorkDirContext(const std::string& dir) {
    auto ctx                  = std::make_shared<agentxx::agent::AgentContext>();
    ctx->agentConfig          = std::make_shared<agentxx::agent::AgentConfig>();
    ctx->agentConfig->workDir = dir;
    return ctx;
}

/// resolvedWorkDir 纯函数校验: workDir 非空原样返回; 为空回退进程 cwd
asio::awaitable<void> test_resolved_workdir(std::weak_ptr<agentxx::agent::AgentContext>) {
    auto cfgWith     = std::make_shared<agentxx::agent::AgentConfig>();
    cfgWith->workDir = "/tmp/proj-a";
    XX_TEST_EXPECT_EQ(cfgWith->resolvedWorkDir(), std::string{"/tmp/proj-a"});

    auto cfgEmpty = std::make_shared<agentxx::agent::AgentConfig>();
    XX_TEST_EXPECT_EQ(
        cfgEmpty->resolvedWorkDir(),
        std::filesystem::current_path().generic_string()
    );
    co_return;
}

/// AgentContext::getSessionWorkDir 统一入口的会话级优先级链:
/// worktree 绑定 > 会话工作目录覆写 > AgentConfig::workDir / 进程 cwd
/// - 各会话覆写彼此独立 (session 引用 workdir 独立的核心语义);
///   清除后回退 agent 级配置
asio::awaitable<void> test_session_workdir_priority(std::weak_ptr<agentxx::agent::AgentContext>) {
    auto ctx         = std::make_shared<agentxx::agent::AgentContext>();
    ctx->agentConfig = std::make_shared<agentxx::agent::AgentConfig>();

    // 未做任何会话级设置: 回退 agent 配置 (workDir 非空原样返回)
    ctx->agentConfig->workDir = "/tmp/proj-a";
    XX_TEST_EXPECT_EQ(ctx->getSessionWorkDir("s1"), std::string{"/tmp/proj-a"});

    // 会话覆写生效且会话间独立
    ctx->setSessionWorkDir("s1", "/tmp/proj-b");
    XX_TEST_EXPECT_EQ(ctx->getSessionWorkDir("s1"), std::string{"/tmp/proj-b"});
    XX_TEST_EXPECT_EQ(ctx->getSessionWorkDir("s2"), std::string{"/tmp/proj-a"});

    // 空串覆写等价清除; 清除后回退 agent 级配置
    ctx->setSessionWorkDir("s1", "");
    XX_TEST_EXPECT_EQ(ctx->getSessionWorkDir("s1"), std::string{"/tmp/proj-a"});
    ctx->setSessionWorkDir("s1", "/tmp/proj-c");
    ctx->clearSessionWorkDir("s1");
    XX_TEST_EXPECT_EQ(ctx->getSessionWorkDir("s1"), std::string{"/tmp/proj-a"});

    // worktree 绑定优先于覆写与配置 (Session 未绑定 io 线程时允许初始化写入)
    auto session = ctx->sessions->getOrCreate("s3");
    ctx->setSessionWorkDir("s3", "/tmp/proj-d");
    session->setWorktreeBinding(agentxx::agent::WorktreeBinding{
        .name     = "wt",
        .path     = "/tmp/repo/.agentxx/agent/worktrees/wt",
        .branch   = "agentxx/wt-wt",
        .repoRoot = "/tmp/repo",
    });
    XX_TEST_EXPECT_EQ(
        ctx->getSessionWorkDir("s3"),
        std::string{"/tmp/repo/.agentxx/agent/worktrees/wt"}
    );

    // 未知会话 id 同样回退 agent 级配置
    XX_TEST_EXPECT_EQ(ctx->getSessionWorkDir("nonexistent"), std::string{"/tmp/proj-a"});
    co_return;
}

/// 相对路径应以会话工作目录 (workDir) 而非进程 cwd 为基准:
/// 绑定 workDir=testDir 后传 "." (解析到工作目录自身), 应列出其下文件;
/// 进程 cwd 与 testDir 无关, 解析错误时将得到空/错误结果而非目录内容
asio::awaitable<void>
    test_list_relative_path_with_workdir(std::weak_ptr<agentxx::agent::AgentContext>) {
    auto ctx  = makeWorkDirContext(testDir);
    auto tool = agentxx::tools::FileSystemListTool{ctx};
    auto args = agentxx::util::Json{
        {"path", "."}
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("test1.txt") != std::string::npos
        && result.find("test2.txt") != std::string::npos
        && result.find("subdir") != std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FileSystemListTool resolves relative path against workDir" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FileSystemListTool workDir relative path failed, got: " << result
                  << std::endl;
    }
    co_return;
}

/// 相对路径读取: 以 workDir 为基准找到文件 (workDir ≠ 进程 cwd)
asio::awaitable<void> test_read_relative_with_workdir(std::weak_ptr<agentxx::agent::AgentContext>) {
    auto ctx  = makeWorkDirContext(testDir);
    auto tool = agentxx::tools::FilesystemReadTextFileTool{ctx};
    auto args = agentxx::util::Json{
        {"path", "subdir/subtest.txt"}
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("subdir file content") != std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FilesystemReadTextFileTool resolves relative path against workDir"
                  << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemReadTextFileTool workDir relative read failed, got: " << result
                  << std::endl;
    }
    co_return;
}

/// 相对 glob 模式: 以 workDir 为基准展开后匹配
asio::awaitable<void>
    test_glob_relative_pattern_with_workdir(std::weak_ptr<agentxx::agent::AgentContext>) {
    auto ctx  = makeWorkDirContext(testDir);
    auto tool = agentxx::tools::FilesystemGlobTool{ctx};
    auto args = agentxx::util::Json{
        {"file_patterns", agentxx::util::Json::array({"*.txt"})},
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("test1.txt") != std::string::npos
        && result.find("test2.txt") != std::string::npos
        && result.find("subtest.txt") == std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FilesystemGlobTool expands relative pattern against workDir" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemGlobTool workDir relative pattern failed, got: " << result
                  << std::endl;
    }
    co_return;
}

/// `~` 前缀应展开为用户主目录。为不污染用户目录, 在主目录下创建临时文件验证后删除。
asio::awaitable<void>
    test_list_file_tilde_path(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
#if XX_IS_WIN_D
    auto homeOpt = agentxx::util::ApplicationEnv::instance().get("USERPROFILE");
#else
    auto homeOpt = agentxx::util::ApplicationEnv::instance().get("HOME");
#endif
    if (!homeOpt || homeOpt->empty()) {
        g_fs_passed++;
        TEST_PASS << "skip tilde test: no home env" << std::endl;
        co_return;
    }
    auto homeFile = std::filesystem::path{*homeOpt} / "agentxx_list_tilde_probe.txt";
    {
        std::ofstream f(homeFile);
        f << "tilde probe\n";
    }
    auto tool = agentxx::tools::FileSystemListTool{agentContext};
    auto args = agentxx::util::Json{
        {"path", "~/agentxx_list_tilde_probe.txt"}
    };
    auto result = co_await tool.execute_async(args);
    std::filesystem::remove(homeFile);
    if (result.find("agentxx_list_tilde_probe.txt") != std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FileSystemListTool expands ~ to home directory" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FileSystemListTool tilde expansion failed, got: " << result << std::endl;
    }
    co_return;
}

/// 通配模式 `*`: 展开匹配到的条目本身 (等价 `ls -d`), 不展开匹配到的目录
asio::awaitable<void>
    test_list_glob_star_pattern(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FileSystemListTool{agentContext};
    auto args = agentxx::util::Json{
        {"path", testDir + "/*.txt"}
    };
    auto result = co_await tool.execute_async(args);
    // 非递归: `*.txt` 只匹配 testDir 当前层, 不含 subdir/subtest.txt
    if (result.find("test1.txt") != std::string::npos
        && result.find("test2.txt") != std::string::npos
        && result.find("subtest.txt") == std::string::npos
        && result.find("-rw-") != std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FileSystemListTool expands `*` wildcard to matched entries" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FileSystemListTool `*` wildcard listing failed, got: " << result << std::endl;
    }
    co_return;
}

/// 通配模式 `?`: 匹配单个字符
asio::awaitable<void>
    test_list_glob_question_mark(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FileSystemListTool{agentContext};
    auto args = agentxx::util::Json{
        {"path", testDir + "/test?.txt"}
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("test1.txt") != std::string::npos
        && result.find("test2.txt") != std::string::npos
        && result.find("subtest.txt") == std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FileSystemListTool expands `?` wildcard to matched entries" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FileSystemListTool `?` wildcard listing failed, got: " << result << std::endl;
    }
    co_return;
}

/// 通配模式 `**`: 递归匹配任意目录深度 (与 glob 工具一致, 不递归时 `*.txt` 只匹配当前层)
asio::awaitable<void>
    test_list_glob_recursive_segment(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FileSystemListTool{agentContext};
    auto args = agentxx::util::Json{
        {"path", testDir + "/**/*.txt"}
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("test1.txt") != std::string::npos
        && result.find("test2.txt") != std::string::npos
        && result.find("subtest.txt") != std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FileSystemListTool expands `**` wildcard recursively" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FileSystemListTool `**` wildcard listing failed, got: " << result
                  << std::endl;
    }

    // `**` 单独使用: 匹配目录自身与其下全部层级
    auto allArgs = agentxx::util::Json{
        {"path", testDir + "/**"}
    };
    auto allResult = co_await tool.execute_async(allArgs);
    if (allResult.find("subdir/") != std::string::npos
        && allResult.find("subtest.txt") != std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FileSystemListTool `**` lists directories and files" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FileSystemListTool `**` full listing failed, got: " << allResult << std::endl;
    }
    co_return;
}

/// 通配匹配到目录 + recursive = true: 目录条目本身与其内容都被列出
asio::awaitable<void>
    test_list_glob_dir_recursive(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FileSystemListTool{agentContext};
    auto args = agentxx::util::Json{
        {"path",      testDir + "/sub*"},
        {"recursive", true             },
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("subdir/") != std::string::npos
        && result.find("subtest.txt") != std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FileSystemListTool expands matched directories when recursive" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FileSystemListTool wildcard recursive expand failed, got: " << result
                  << std::endl;
    }

    // `**` 已覆盖全部后代, 再叠加 recursive 时同一条目只应出现一次 (按路径去重)
    auto dupArgs = agentxx::util::Json{
        {"path",      testDir + "/**"},
        {"recursive", true           },
    };
    auto dupResult  = co_await tool.execute_async(dupArgs);
    int  subtestHit = 0;
    for (size_t pos = dupResult.find("subtest.txt"); pos != std::string::npos;
         pos        = dupResult.find("subtest.txt", pos + 1)) {
        subtestHit++;
    }
    if (subtestHit == 1) {
        g_fs_passed++;
        TEST_PASS << "FileSystemListTool dedups wildcard matches combined with recursive"
                  << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FileSystemListTool wildcard recursive dedup failed (hits=" << subtestHit
                  << "), got: " << dupResult << std::endl;
    }
    co_return;
}

/// 通配模式的相对路径同样以 workDir 为基准展开
asio::awaitable<void>
    test_list_glob_relative_with_workdir(std::weak_ptr<agentxx::agent::AgentContext>) {
    auto ctx  = makeWorkDirContext(testDir);
    auto tool = agentxx::tools::FileSystemListTool{ctx};
    auto args = agentxx::util::Json{
        {"path", "*.txt"}
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("test1.txt") != std::string::npos
        && result.find("test2.txt") != std::string::npos
        && result.find("subtest.txt") == std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FileSystemListTool expands relative wildcard against workDir" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FileSystemListTool workDir relative wildcard failed, got: " << result
                  << std::endl;
    }
    co_return;
}

/// 通配无匹配: 返回错误说明而非 "Path not exist" 混淆
asio::awaitable<void>
    test_list_glob_no_match(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FileSystemListTool{agentContext};
    auto args = agentxx::util::Json{
        {"path", testDir + "/no_such_*.txt"}
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("No match") != std::string::npos
        && result.find("no_such_*.txt") != std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FileSystemListTool reports no match for wildcard without hits" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FileSystemListTool wildcard no-match message wrong, got: " << result
                  << std::endl;
    }
    co_return;
}

/// 通配匹配大小写敏感 (与 glob 工具一致)
asio::awaitable<void>
    test_list_glob_case_sensitive(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FileSystemListTool{agentContext};
    auto args = agentxx::util::Json{
        {"path", testDir + "/*.TXT"}
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("No match") != std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FileSystemListTool wildcard match is case-sensitive" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FileSystemListTool wildcard should be case-sensitive, got: " << result
                  << std::endl;
    }
    co_return;
}

/// 通配模式下 limit 同样限制输出条目总数 (多个匹配条目共用一个上限)
asio::awaitable<void> test_list_glob_limit(std::weak_ptr<agentxx::agent::AgentContext> agentContext
) {
    auto tool = agentxx::tools::FileSystemListTool{agentContext};
    auto args = agentxx::util::Json{
        {"path",  testDir + "/*"},
        {"limit", 1             },
    };
    auto   result    = co_await tool.execute_async(args);
    size_t lineCount = 0;
    for (size_t i = 0; i < result.size(); i++) {
        if (result[i] == '\n') {
            lineCount++;
        }
    }
    if (lineCount <= 1) {
        g_fs_passed++;
        TEST_PASS << "FileSystemListTool applies limit to wildcard results" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FileSystemListTool wildcard limit failed, got: " << result << std::endl;
    }
    co_return;
}

/// 文件名本身含通配字符 (`[`): 通配无匹配时回退为字面路径处理
/// (与 shell "无匹配时保留模式原样" 一致, 保证含特殊字符的真实文件仍可列出)
asio::awaitable<void>
    test_list_glob_literal_fallback(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    const auto literalFile = testDir + "/literal[1].txt";
    {
        std::ofstream f(literalFile);
        f << "literal fallback\n";
    }
    auto tool = agentxx::tools::FileSystemListTool{agentContext};
    auto args = agentxx::util::Json{
        {"path", literalFile}
    };
    auto            result = co_await tool.execute_async(args);
    std::error_code ec;
    std::filesystem::remove(agentxx::util::utf8ToPath(literalFile), ec);
    if (result.find("literal[1].txt") != std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FileSystemListTool falls back to literal path when no wildcard hit"
                  << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FileSystemListTool literal fallback failed, got: " << result << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_read_text_file_get_definition(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FilesystemReadTextFileTool{agentContext};
    auto def  = tool.get_definition();
    if (def.name == "agentxx_filesystem_read") {
        g_fs_passed++;
        TEST_PASS << "FilesystemReadTextFileTool::get_definition() name correct" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemReadTextFileTool::get_definition() name incorrect" << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_read_text_file_empty_path(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FilesystemReadTextFileTool{agentContext};
    auto args = agentxx::util::Json{
        {"path", ""}
    };
    try {
        auto result = co_await tool.execute_async(args);
        if (agentxx::util::isIgnoreCaseContains(result, "error")) {
            g_fs_passed++;
            TEST_PASS << "FilesystemReadTextFileTool returns error for empty path" << std::endl;
        } else {
            g_fs_failed++;
            TEST_FAIL << "FilesystemReadTextFileTool should return error for "
                         "empty path, got: "
                      << result << std::endl;
        }
    } catch (const std::exception& e) {
        g_fs_passed++;
        TEST_PASS << "FilesystemReadTextFileTool throws for empty path: " << e.what() << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_read_text_file_not_exist(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FilesystemReadTextFileTool{agentContext};
    auto args = agentxx::util::Json{
        {"path", testDir + "/nonexistent.txt"}
    };
    try {
        auto result = co_await tool.execute_async(args);
        if (agentxx::util::isIgnoreCaseContains(result, "error")
            && agentxx::util::isIgnoreCaseContains(result, "not exist")
            && !agentxx::util::isIgnoreCaseContains(result, "permission")) {
            g_fs_passed++;
            TEST_PASS
                << "FilesystemReadTextFileTool returns 'not exist' error for non-existent file"
                << std::endl;
        } else {
            g_fs_failed++;
            TEST_FAIL
                << "FilesystemReadTextFileTool should return 'not exist' error for non-existent file, "
                   "got: "
                << result << std::endl;
        }
    } catch (const std::exception& e) {
        if (agentxx::util::isIgnoreCaseContains(e.what(), "not exist")
            && !agentxx::util::isIgnoreCaseContains(e.what(), "permission")) {
            g_fs_passed++;
            TEST_PASS << "FilesystemReadTextFileTool throws 'not exist' for non-existent file: "
                      << e.what() << std::endl;
        } else {
            g_fs_failed++;
            TEST_FAIL << "FilesystemReadTextFileTool unexpected exception for non-existent file: "
                      << e.what() << std::endl;
        }
    }

    // 测试带 line_offset / line_limit 读取不存在文件
    try {
        auto offsetArgs = agentxx::util::Json{
            {"path",        testDir + "/nonexistent.txt"},
            {"line_offset", 0                           },
            {"line_limit",  10                          },
        };
        auto result = co_await tool.execute_async(offsetArgs);
        if (agentxx::util::isIgnoreCaseContains(result, "not exist")
            && !agentxx::util::isIgnoreCaseContains(result, "permission")) {
            g_fs_passed++;
            TEST_PASS
                << "FilesystemReadTextFileTool with offset/limit returns 'not exist' for non-existent file"
                << std::endl;
        } else {
            g_fs_failed++;
            TEST_FAIL
                << "FilesystemReadTextFileTool with offset/limit should return 'not exist', got: "
                << result << std::endl;
        }
    } catch (const std::exception& e) {
        if (agentxx::util::isIgnoreCaseContains(e.what(), "not exist")) {
            g_fs_passed++;
            TEST_PASS << "FilesystemReadTextFileTool with offset/limit throws 'not exist': "
                      << e.what() << std::endl;
        } else {
            g_fs_failed++;
            TEST_FAIL << "FilesystemReadTextFileTool with offset/limit unexpected exception: "
                      << e.what() << std::endl;
        }
    }

    // 测试同步版 fileReadExecute 读取不存在文件
    {
        auto syncResult = ::agentxx_fs_plugin::fileReadExecute(
            args,
            agentxx::tools::testResolvedWorkDir(agentContext)
        );
        if (agentxx::util::isIgnoreCaseContains(syncResult, "not exist")
            && !agentxx::util::isIgnoreCaseContains(syncResult, "permission")) {
            g_fs_passed++;
            TEST_PASS << "fileReadExecute (sync) returns 'not exist' for non-existent file"
                      << std::endl;
        } else {
            g_fs_failed++;
            TEST_FAIL << "fileReadExecute (sync) should return 'not exist', got: " << syncResult
                      << std::endl;
        }
    }

    // 测试读取目录路径应返回目录错误而非权限错误
    try {
        auto dirArgs = agentxx::util::Json{
            {"path", testDir}
        };
        auto result = co_await tool.execute_async(dirArgs);
        if (agentxx::util::isIgnoreCaseContains(result, "directory")
            && !agentxx::util::isIgnoreCaseContains(result, "permission")) {
            g_fs_passed++;
            TEST_PASS
                << "FilesystemReadTextFileTool returns directory error when reading a directory"
                << std::endl;
        } else {
            g_fs_failed++;
            TEST_FAIL << "FilesystemReadTextFileTool should return directory error, got: " << result
                      << std::endl;
        }
    } catch (const std::exception& e) {
        if (agentxx::util::isIgnoreCaseContains(e.what(), "directory")) {
            g_fs_passed++;
            TEST_PASS << "FilesystemReadTextFileTool throws directory error: " << e.what()
                      << std::endl;
        } else {
            g_fs_failed++;
            TEST_FAIL << "FilesystemReadTextFileTool unexpected exception for directory: "
                      << e.what() << std::endl;
        }
    }

    co_return;
}

asio::awaitable<void>
    test_read_text_file_full(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FilesystemReadTextFileTool{agentContext};
    auto args = agentxx::util::Json{
        {"path", testDir + "/test1.txt"}
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("line1") != std::string::npos && result.find("line5") != std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FilesystemReadTextFileTool reads full file content" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemReadTextFileTool full read failed, got: " << result << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_read_text_file_offset(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto offsetFile = testDir + "/offset_test.txt";
    {
        std::ofstream f(offsetFile);
        f << "aaaa\nbbbb\ncccc\ndddd\n";
    }
    auto tool = agentxx::tools::FilesystemReadTextFileTool{agentContext};
    auto args = agentxx::util::Json{
        {"path",        offsetFile},
        {"line_offset", 0         },
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("aaaa") != std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FilesystemReadTextFileTool respects line_offset=0" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemReadTextFileTool line_offset=0 failed, got: " << result
                  << std::endl;
    }
    co_return;
}

/// 相对路径读取: 应自动转为绝对路径后成功读取
asio::awaitable<void>
    test_read_text_file_relative_path(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool    = agentxx::tools::FilesystemReadTextFileTool{agentContext};
    auto relPath = testDirRelativeToCwd() + "/test1.txt";
    auto args    = agentxx::util::Json{
           {"path", relPath}
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("line1") != std::string::npos && result.find("line5") != std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FilesystemReadTextFileTool resolves relative path" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemReadTextFileTool relative path failed, got: " << result
                  << " relPath: " << relPath << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_read_text_file_limit(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FilesystemReadTextFileTool{agentContext};
    auto args = agentxx::util::Json{
        {"path",       testDir + "/offset_test.txt"},
        {"line_limit", 3                           },
    };
    auto result    = co_await tool.execute_async(args);
    auto lineCount = size_t{0};
    for (size_t i = 0; i < result.size(); i++) {
        if (result[i] == '\n') {
            lineCount++;
        }
    }
    if (lineCount <= 3) {
        g_fs_passed++;
        TEST_PASS << "FilesystemReadTextFileTool respects line_limit" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemReadTextFileTool line_limit failed, got " << lineCount << " lines"
                  << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_read_text_file_offset_and_limit(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FilesystemReadTextFileTool{agentContext};
    auto args = agentxx::util::Json{
        {"path",        testDir + "/offset_test.txt"},
        {"line_offset", 0                           },
        {"line_limit",  2                           },
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("aaaa") != std::string::npos && result.find("bbbb") != std::string::npos
        && result.find("cccc") == std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FilesystemReadTextFileTool respects both offset and limit" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemReadTextFileTool offset+limit failed, got: " << result << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_write_file_get_definition(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FilesystemWriteFileTool{agentContext};
    auto def  = tool.get_definition();
    if (def.name == "agentxx_filesystem_write") {
        g_fs_passed++;
        TEST_PASS << "FilesystemWriteFileTool::get_definition() name correct" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemWriteFileTool::get_definition() name incorrect" << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_write_file_empty_path(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FilesystemWriteFileTool{agentContext};
    auto args = agentxx::util::Json{
        {"path", ""}
    };
    auto result = co_await tool.execute_async(args);
    if (agentxx::util::isIgnoreCaseContains(result, "error")) {
        g_fs_passed++;
        TEST_PASS << "FilesystemWriteFileTool returns error for empty path" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemWriteFileTool should return error for empty "
                     "path, got: "
                  << result << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_write_file_create(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool     = agentxx::tools::FilesystemWriteFileTool{agentContext};
    auto filePath = testDir + "/write_test.txt";
    auto args     = agentxx::util::Json{
            {"path",    filePath          },
            {"content", "hello write test"},
    };
    auto result = co_await tool.execute_async(args);
    if (result == "success") {
        if (std::filesystem::exists(filePath)) {
            std::ifstream in(filePath);
            std::string   content(
                (std::istreambuf_iterator<char>(in)),
                std::istreambuf_iterator<char>()
            );
            if (content == "hello write test") {
                g_fs_passed++;
                TEST_PASS << "FilesystemWriteFileTool creates and writes file" << std::endl;
            } else {
                g_fs_failed++;
                TEST_FAIL << "FilesystemWriteFileTool wrote wrong content: '" << content << "'"
                          << std::endl;
            }
        } else {
            g_fs_failed++;
            TEST_FAIL << "FilesystemWriteFileTool file not created" << std::endl;
        }
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemWriteFileTool write failed: " << result << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_write_file_no_overwrite_existing(std::weak_ptr<agentxx::agent::AgentContext> agentContext
    ) {
    auto tool     = agentxx::tools::FilesystemWriteFileTool{agentContext};
    auto filePath = testDir + "/test1.txt";
    auto args     = agentxx::util::Json{
            {"path",      filePath     },
            {"content",   "new content"},
            {"overwrite", false        },
    };
    try {
        auto result = co_await tool.execute_async(args);
        if (agentxx::util::isIgnoreCaseContains(result, "error")
            || agentxx::util::isIgnoreCaseContains(result, "already exist")) {
            g_fs_passed++;
            TEST_PASS << "FilesystemWriteFileTool returns error when file exists and "
                         "overwrite=false"
                      << std::endl;
        } else {
            g_fs_failed++;
            TEST_FAIL << "FilesystemWriteFileTool should return error when file exists and "
                         "overwrite=false, got: "
                      << result << std::endl;
        }
    } catch (const std::exception& e) {
        g_fs_passed++;
        TEST_PASS << "FilesystemWriteFileTool throws when file exists and "
                     "overwrite=false: "
                  << e.what() << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_write_file_overwrite(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool     = agentxx::tools::FilesystemWriteFileTool{agentContext};
    auto filePath = testDir + "/overwrite_test.txt";
    {
        std::ofstream f(filePath);
        f << "original content\n";
    }
    auto args = agentxx::util::Json{
        {"path",      filePath             },
        {"content",   "overwritten content"},
        {"overwrite", true                 },
    };
    auto result = co_await tool.execute_async(args);
    if (result == "success") {
        std::ifstream in(filePath);
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (content == "overwritten content") {
            g_fs_passed++;
            TEST_PASS << "FilesystemWriteFileTool overwrites file when "
                         "overwrite=true"
                      << std::endl;
        } else {
            g_fs_failed++;
            TEST_FAIL << "FilesystemWriteFileTool overwrite wrong content: '" << content << "'"
                      << std::endl;
        }
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemWriteFileTool overwrite failed: " << result << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_edit_text_file_get_definition(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FilesystemEditTextFileTool{agentContext};
    auto def  = tool.get_definition();
    if (def.name == "agentxx_filesystem_edit") {
        g_fs_passed++;
        TEST_PASS << "FilesystemEditTextFileTool::get_definition() name correct" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemEditTextFileTool::get_definition() name incorrect" << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_edit_text_file_empty_path(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FilesystemEditTextFileTool{agentContext};
    auto args = agentxx::util::Json{
        {"path",    "" },
        {"old_str", "a"},
        {"new_str", "b"},
    };
    auto result = co_await tool.execute_async(args);
    if (agentxx::util::isIgnoreCaseContains(result, "error")) {
        g_fs_passed++;
        TEST_PASS << "FilesystemEditTextFileTool returns error for empty path" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemEditTextFileTool should return error for "
                     "empty path, got: "
                  << result << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_edit_text_file_empty_old_str(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FilesystemEditTextFileTool{agentContext};
    auto args = agentxx::util::Json{
        {"path",    testDir + "/test1.txt"},
        {"old_str", ""                    },
        {"new_str", "b"                   },
    };
    auto result = co_await tool.execute_async(args);
    if (agentxx::util::isIgnoreCaseContains(result, "error")) {
        g_fs_passed++;
        TEST_PASS << "FilesystemEditTextFileTool returns error for empty old_str" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemEditTextFileTool should return error for "
                     "empty old_str, got: "
                  << result << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_edit_text_file_single_replace(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto filePath = testDir + "/edit_test.txt";
    {
        std::ofstream f(filePath);
        f << "hello world\nfoo bar\n";
    }

    auto tool = agentxx::tools::FilesystemEditTextFileTool{agentContext};
    auto args = agentxx::util::Json{
        {"path",    filePath     },
        {"old_str", "hello world"},
        {"new_str", "hi universe"},
    };
    auto result = co_await tool.execute_async(args);
    if (result == "success") {
        std::ifstream in(filePath);
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (content.find("hi universe") != std::string::npos
            && content.find("hello world") == std::string::npos) {
            g_fs_passed++;
            TEST_PASS << "FilesystemEditTextFileTool single replace works" << std::endl;
        } else {
            g_fs_failed++;
            TEST_FAIL << "FilesystemEditTextFileTool single replace wrong "
                         "content: '"
                      << content << "'" << std::endl;
        }
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemEditTextFileTool single replace failed: " << result << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_edit_text_file_multi_replace(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto filePath = testDir + "/edit_multi_test.txt";
    {
        std::ofstream f(filePath);
        f << "foo foo foo bar\n";
    }

    auto tool = agentxx::tools::FilesystemEditTextFileTool{agentContext};
    auto args = agentxx::util::Json{
        {"path",          filePath},
        {"old_str",       "foo"   },
        {"new_str",       "baz"   },
        {"multi_replace", true    },
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("Replace 3 hits") != std::string::npos) {
        std::ifstream in(filePath);
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (content.find("foo") == std::string::npos
            && content.find("baz baz baz") != std::string::npos) {
            g_fs_passed++;
            TEST_PASS << "FilesystemEditTextFileTool multi_replace works" << std::endl;
        } else {
            g_fs_failed++;
            TEST_FAIL << "FilesystemEditTextFileTool multi_replace wrong "
                         "content: '"
                      << content << "'" << std::endl;
        }
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemEditTextFileTool multi_replace failed: " << result << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_edit_text_file_no_match(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto filePath = testDir + "/edit_test.txt";
    auto tool     = agentxx::tools::FilesystemEditTextFileTool{agentContext};
    auto args     = agentxx::util::Json{
            {"path",    filePath                },
            {"old_str", "nonexistent_string_xyz"},
            {"new_str", "replacement"           },
    };
    try {
        auto result = co_await tool.execute_async(args);
        if (agentxx::util::isIgnoreCaseContains(result, "error")
            || agentxx::util::isIgnoreCaseContains(result, "no match")) {
            g_fs_passed++;
            TEST_PASS << "FilesystemEditTextFileTool returns error when no match found"
                      << std::endl;
        } else {
            g_fs_failed++;
            TEST_FAIL << "FilesystemEditTextFileTool should return error when no match found, "
                         "got: "
                      << result << std::endl;
        }
    } catch (const std::exception& e) {
        g_fs_passed++;
        TEST_PASS << "FilesystemEditTextFileTool throws when no match found: " << e.what()
                  << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_read_text_file_crlf(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto filePath = testDir + "/crlf_read_test.txt";
    {
        std::ofstream f(filePath, std::ios::binary);
        f << "alpha\r\nbeta\r\ngamma\r\n";
    }
    auto tool = agentxx::tools::FilesystemReadTextFileTool{agentContext};

    // 完整读取: 保留原始换行符 (CRLF 文件返回 CRLF 内容, 见插件注释:
    // read 保留原始内容、edit 归一化 LF 后匹配)
    auto args = agentxx::util::Json{
        {"path", filePath}
    };
    auto full     = co_await tool.execute_async(args);
    auto hasCRLF  = full.find("alpha\r\nbeta\r\ngamma\r\n") != std::string::npos;
    auto noBareLF = full.find("alpha\nbeta\ngamma\n") == std::string::npos;
    if (hasCRLF && noBareLF) {
        g_fs_passed++;
        TEST_PASS << "read_text_file preserves CRLF on full read" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "read_text_file CRLF full read failed, got: '" << full << "'" << std::endl;
    }

    // offset/limit 读取: 同样保留原始 `\r`
    auto args2 = agentxx::util::Json{
        {"path",        filePath},
        {"line_offset", 1       },
        {"line_limit",  1       },
    };
    auto part = co_await tool.execute_async(args2);
    auto partHasBeta
        = part.find("beta") != std::string::npos && part.find("alpha") == std::string::npos;
    if (partHasBeta) {
        g_fs_passed++;
        TEST_PASS << "read_text_file preserves CRLF on offset/limit read" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "read_text_file CRLF offset/limit read failed, got: '" << part << "'"
                  << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_edit_text_file_crlf_file(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    // CRLF 文件 + LF 形式的 old_str/new_str: 归一化后匹配成功, 落盘统一为 LF
    auto filePath = testDir + "/crlf_edit_test.txt";
    {
        std::ofstream f(filePath, std::ios::binary);
        f << "hello world\r\nfoo bar\r\n";
    }

    auto tool = agentxx::tools::FilesystemEditTextFileTool{agentContext};
    auto args = agentxx::util::Json{
        {"path",    filePath              },
        {"old_str", "hello world\nfoo bar"},
        {"new_str", "hi universe\nfoo bar"},
    };
    auto result = co_await tool.execute_async(args);
    if (result == "success") {
        std::ifstream in(filePath, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (content == "hi universe\nfoo bar\n") {
            g_fs_passed++;
            TEST_PASS << "edit_text_file matches LF old_str in CRLF file and writes LF"
                      << std::endl;
        } else {
            g_fs_failed++;
            TEST_FAIL << "edit_text_file CRLF edit wrong content: '" << content << "'" << std::endl;
        }
    } else {
        g_fs_failed++;
        TEST_FAIL << "edit_text_file CRLF edit failed: " << result << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_edit_text_file_lf_file_crlf_old(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    // LF 文件 + CRLF 形式的 old_str/new_str: 应匹配成功并保持文件为 LF
    auto filePath = testDir + "/lf_edit_crlf_old_test.txt";
    {
        std::ofstream f(filePath, std::ios::binary);
        f << "alpha\nbeta\ngamma\n";
    }

    auto tool = agentxx::tools::FilesystemEditTextFileTool{agentContext};
    auto args = agentxx::util::Json{
        {"path",    filePath       },
        {"old_str", "alpha\r\nbeta"},
        {"new_str", "AA\r\nBB"     },
    };
    auto result = co_await tool.execute_async(args);
    if (result == "success") {
        std::ifstream in(filePath, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (content == "AA\nBB\ngamma\n") {
            g_fs_passed++;
            TEST_PASS << "edit_text_file matches CRLF old_str in LF file and keeps LF" << std::endl;
        } else {
            g_fs_failed++;
            TEST_FAIL << "edit_text_file LF file CRLF old_str wrong content: '" << content << "'"
                      << std::endl;
        }
    } else {
        g_fs_failed++;
        TEST_FAIL << "edit_text_file LF file CRLF old_str edit failed: " << result << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_edit_text_file_crlf_multi_replace(std::weak_ptr<agentxx::agent::AgentContext> agentContext
    ) {
    // CRLF 文件 + multi_replace: LF 形式 old_str 应替换全部并落盘为 LF
    auto filePath = testDir + "/crlf_edit_multi_test.txt";
    {
        std::ofstream f(filePath, std::ios::binary);
        f << "foo\r\nfoo\r\nfoo\r\n";
    }

    auto tool = agentxx::tools::FilesystemEditTextFileTool{agentContext};
    auto args = agentxx::util::Json{
        {"path",          filePath},
        {"old_str",       "foo"   },
        {"new_str",       "baz"   },
        {"multi_replace", true    },
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("Replace 3 hits") != std::string::npos) {
        std::ifstream in(filePath, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (content == "baz\nbaz\nbaz\n") {
            g_fs_passed++;
            TEST_PASS << "edit_text_file multi_replace works on CRLF file and writes LF"
                      << std::endl;
        } else {
            g_fs_failed++;
            TEST_FAIL << "edit_text_file CRLF multi_replace wrong content: '" << content << "'"
                      << std::endl;
        }
    } else {
        g_fs_failed++;
        TEST_FAIL << "edit_text_file CRLF multi_replace failed: " << result << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_glob_get_definition(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FilesystemGlobTool{agentContext};
    auto def  = tool.get_definition();
    if (def.name == "agentxx_filesystem_glob") {
        g_fs_passed++;
        TEST_PASS << "FilesystemGlobTool::get_definition() name correct" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemGlobTool::get_definition() name incorrect" << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_glob_empty_patterns(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FilesystemGlobTool{agentContext};
    auto args = agentxx::util::Json{
        {"file_patterns", agentxx::util::Json::array()},
    };
    auto result = co_await tool.execute_async(args);
    if (agentxx::util::isIgnoreCaseContains(result, "error")) {
        g_fs_passed++;
        TEST_PASS << "FilesystemGlobTool returns error for empty file_patterns" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemGlobTool should return error for empty "
                     "patterns, got: "
                  << result << std::endl;
    }
    co_return;
}

asio::awaitable<void> test_glob_find_files(std::weak_ptr<agentxx::agent::AgentContext> agentContext
) {
    auto tool = agentxx::tools::FilesystemGlobTool{agentContext};
    auto args = agentxx::util::Json{
        {"file_patterns", agentxx::util::Json::array({testDir + "/*.txt"})},
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("test1.txt") != std::string::npos
        || result.find("test2.txt") != std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FilesystemGlobTool finds files by pattern" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemGlobTool glob failed, got: " << result << std::endl;
    }
    co_return;
}

asio::awaitable<void> test_glob_recursive(std::weak_ptr<agentxx::agent::AgentContext> agentContext
) {
    auto tool = agentxx::tools::FilesystemGlobTool{agentContext};
    auto args = agentxx::util::Json{
        {"file_patterns", agentxx::util::Json::array({testDir + "/**/*.txt"})},
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("subtest.txt") != std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FilesystemGlobTool recursive glob finds subdirectory files" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemGlobTool recursive glob failed, got: " << result << std::endl;
    }
    co_return;
}

/// 相对 glob 模式: 应自动转为绝对路径后匹配
asio::awaitable<void>
    test_glob_relative_pattern(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool   = agentxx::tools::FilesystemGlobTool{agentContext};
    auto relDir = testDirRelativeToCwd();
    auto args   = agentxx::util::Json{
          {"file_patterns", agentxx::util::Json::array({relDir + "/*.txt"})},
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("test1.txt") != std::string::npos
        && result.find("test2.txt") != std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FilesystemGlobTool resolves relative pattern" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemGlobTool relative pattern failed, got: " << result << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_grep_get_definition(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FilesystemGrepTool{agentContext};
    auto def  = tool.get_definition();
    if (def.name == "agentxx_filesystem_grep") {
        g_fs_passed++;
        TEST_PASS << "FilesystemGrepTool::get_definition() name correct" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemGrepTool::get_definition() name incorrect" << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_grep_empty_text_patterns(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FilesystemGrepTool{agentContext};
    // text_patterns 与 regex_patterns 均为空: 应报错 (至少指定其一)
    auto args = agentxx::util::Json{
        {"text_patterns",  agentxx::util::Json::array()                    },
        {"regex_patterns", agentxx::util::Json::array()                    },
        {"file_patterns",  agentxx::util::Json::array({testDir + "/*.txt"})},
    };
    auto result = co_await tool.execute_async(args);
    if (agentxx::util::isIgnoreCaseContains(result, "error")) {
        g_fs_passed++;
        TEST_PASS << "FilesystemGrepTool returns error for empty text_patterns" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemGrepTool should return error for empty "
                     "text_patterns, got: "
                  << result << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_grep_empty_file_patterns(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FilesystemGrepTool{agentContext};
    auto args = agentxx::util::Json{
        {"text_patterns", agentxx::util::Json::array({"hello"})},
        {"file_patterns", agentxx::util::Json::array()         },
    };
    auto result = co_await tool.execute_async(args);
    if (agentxx::util::isIgnoreCaseContains(result, "error")) {
        g_fs_passed++;
        TEST_PASS << "FilesystemGrepTool returns error for empty file_patterns" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemGrepTool should return error for empty "
                     "file_patterns, got: "
                  << result << std::endl;
    }
    co_return;
}

asio::awaitable<void> test_grep_text_search(std::weak_ptr<agentxx::agent::AgentContext> agentContext
) {
    auto tool = agentxx::tools::FilesystemGrepTool{agentContext};
    // text_patterns 现为纯文本匹配 (开关已移除)
    auto args = agentxx::util::Json{
        {"text_patterns", agentxx::util::Json::array({"hello"})           },
        {"file_patterns", agentxx::util::Json::array({testDir + "/*.txt"})},
        {"output_mode",   "files_with_matches"                            },
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("test2.txt") != std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FilesystemGrepTool finds files containing text" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemGrepTool text search failed, got: " << result << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_grep_regex_search(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FilesystemGrepTool{agentContext};
    // 正则搜索改由 regex_patterns 指定 (text_patterns 不再按正则解释)
    auto args = agentxx::util::Json{
        {"regex_patterns", agentxx::util::Json::array({"line[0-9]"})           },
        {"file_patterns",  agentxx::util::Json::array({testDir + "/test1.txt"})},
        {"output_mode",    "files_with_matches"                                },
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("test1.txt") != std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FilesystemGrepTool regex search finds matches" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemGrepTool regex search failed, got: " << result << std::endl;
    }
    co_return;
}

/// 双模式并集: text_patterns 与 regex_patterns 同时指定时, 结果应为两者并集
/// - 纯文本 "hello world" 仅命中 test2.txt; 正则 "line[0-9]" 仅命中 test1.txt;
///   同时搜索应两个文件都返回 (files_with_matches 模式单文件只输出一次)
asio::awaitable<void>
    test_grep_text_and_regex_union(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FilesystemGrepTool{agentContext};
    auto args = agentxx::util::Json{
        {"text_patterns",  agentxx::util::Json::array({"hello world"})     },
        {"regex_patterns", agentxx::util::Json::array({"line[0-9]"})       },
        {"file_patterns",  agentxx::util::Json::array({testDir + "/*.txt"})},
        {"output_mode",    "files_with_matches"                            },
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("test1.txt") != std::string::npos
        && result.find("test2.txt") != std::string::npos
        && result.find("[Error]") == std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FilesystemGrepTool text+regex union returns both files" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemGrepTool text+regex union failed, got: " << result << std::endl;
    }
    co_return;
}

/// 双模式并集 + content: 同一文件同时被两种模式命中时组头只出现一次,
/// 同一行被两种模式命中时也只输出一次 (test2.txt 第 1 行 "hello world":
/// 文本 "hello" 与正则 "world" 均命中该行)
asio::awaitable<void>
    test_grep_text_and_regex_union_content(std::weak_ptr<agentxx::agent::AgentContext> agentContext
    ) {
    auto tool = agentxx::tools::FilesystemGrepTool{agentContext};
    auto args = agentxx::util::Json{
        {"text_patterns",  agentxx::util::Json::array({"hello"})               },
        {"regex_patterns", agentxx::util::Json::array({"world"})               },
        {"file_patterns",  agentxx::util::Json::array({testDir + "/test2.txt"})},
        {"output_mode",    "content"                                           },
    };
    auto result = co_await tool.execute_async(args);
    // 文本 "hello" 与正则 "world" 都命中 test2.txt 第 1 行, 应只输出一行且组头一次
    size_t headerCount = 0;
    for (auto pos = result.find("test2.txt:\n"); pos != std::string::npos;
         pos      = result.find("test2.txt:\n", pos + 1)) {
        ++headerCount;
    }
    if (headerCount == 1 && result.find("\n1:hello world\n") != std::string::npos
        && result.find("[Error]") == std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FilesystemGrepTool text+regex union content dedup works" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemGrepTool text+regex union content failed, got: " << result
                  << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_grep_content_mode(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FilesystemGrepTool{agentContext};
    auto args = agentxx::util::Json{
        {"text_patterns", agentxx::util::Json::array({"hello"})               },
        {"file_patterns", agentxx::util::Json::array({testDir + "/test2.txt"})},
        {"output_mode",   "content"                                           },
    };
    auto result = co_await tool.execute_async(args);
    // content 模式现在返回按文件分组的文本格式: 每文件组头 "{filepath}:" 仅出现一次,
    // 之后每行为 "行号:整行内容" (对齐 grep -n 且减少文件路径重复)
    // test2.txt 第 1 行是 "hello world", 应输出组头 ".../test2.txt:" 与行 "1:hello world"
    bool hasFileHeader = result.find("test2.txt:\n") != std::string::npos;
    bool hasMatchLine  = result.find("\n1:hello world\n") != std::string::npos;
    // 不应再出现旧的逐行路径前缀格式 (file:line:content)
    bool noLegacyFormat = result.find("test2.txt:1:") == std::string::npos;
    if (hasFileHeader && hasMatchLine && noLegacyFormat) {
        g_fs_passed++;
        TEST_PASS << "FilesystemGrepTool content mode returns grouped-by-file format" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemGrepTool content mode grouped format incorrect, got: " << result
                  << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_grep_content_grouped_multi_files(std::weak_ptr<agentxx::agent::AgentContext> agentContext
    ) {
    auto tool = agentxx::tools::FilesystemGrepTool{agentContext};
    // 多文件 content 模式: 每个文件应有独立的组头 "{filepath}:",
    // 且每个文件的组头仅出现一次 (减少路径重复), 各文件行归属自己的组头之下
    auto args = agentxx::util::Json{
        {"regex_patterns",     agentxx::util::Json::array({".*e.*"})           },
        {"file_patterns",      agentxx::util::Json::array({testDir + "/*.txt"})},
        {"output_mode",        "content"                                       },
        {"max_count_per_file", 1                                               },
    };
    auto result = co_await tool.execute_async(args);

    auto countSubstr = [](const std::string& text, std::string_view sub) -> size_t {
        size_t count = 0;
        for (auto pos = text.find(sub); pos != std::string::npos; pos = text.find(sub, pos + 1)) {
            ++count;
        }
        return count;
    };

    // test1.txt / test2.txt 均为非空文本且含字符 'e', 各应恰好输出一个组头
    size_t header1 = countSubstr(result, "test1.txt:\n");
    size_t header2 = countSubstr(result, "test2.txt:\n");
    if (header1 == 1 && header2 == 1) {
        g_fs_passed++;
        TEST_PASS << "FilesystemGrepTool content mode groups output per file header" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemGrepTool expected exactly one group header per file "
                     "(test1.txt=1, test2.txt=1), got "
                  << header1 << "/" << header2 << ", result: " << result << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_grep_case_insensitive(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FilesystemGrepTool{agentContext};
    // 搜索 "HELLO" (大写), case_sensitive=false 应匹配到 "hello world"
    auto args = agentxx::util::Json{
        {"text_patterns",  agentxx::util::Json::array({"HELLO"})               },
        {"file_patterns",  agentxx::util::Json::array({testDir + "/test2.txt"})},
        {"output_mode",    "files_with_matches"                                },
        {"case_sensitive", false                                               },
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("test2.txt") != std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FilesystemGrepTool case_insensitive search works" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemGrepTool case_insensitive search failed, got: " << result
                  << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_grep_case_sensitive_default(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FilesystemGrepTool{agentContext};
    // 搜索 "HELLO" (大写), 默认 case_sensitive=true 不应匹配到 "hello world"
    auto args = agentxx::util::Json{
        {"text_patterns", agentxx::util::Json::array({"HELLO"})               },
        {"file_patterns", agentxx::util::Json::array({testDir + "/test2.txt"})},
        {"output_mode",   "files_with_matches"                                },
    };
    auto result = co_await tool.execute_async(args);
    // 应该报错 (无匹配), 而不是返回 test2.txt
    if (result.find("test2.txt") == std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FilesystemGrepTool default case_sensitive=true works" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemGrepTool should not match with wrong case, got: " << result
                  << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_grep_max_count_per_file(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FilesystemGrepTool{agentContext};
    // test1.txt 有 line1~line5, 搜索 "line" 应匹配 5 次, 限制 max_count_per_file=2
    auto args = agentxx::util::Json{
        {"text_patterns",      agentxx::util::Json::array({"line"})                },
        {"file_patterns",      agentxx::util::Json::array({testDir + "/test1.txt"})},
        {"output_mode",        "files_with_matches"                                },
        {"max_count_per_file", 2                                                   },
    };
    auto result = co_await tool.execute_async(args);
    // 应输出 test1.txt:2 (限制为 2)
    if (result.find("test1.txt:2") != std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FilesystemGrepTool max_count_per_file works" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemGrepTool max_count_per_file failed, got: " << result << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_grep_context_lines(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FilesystemGrepTool{agentContext};
    // test1.txt: line1\nline2\nline3\nline4\nline5\n
    // 搜索 "line3", context_lines=1, 应输出第 2,3,4 行
    auto args = agentxx::util::Json{
        {"text_patterns", agentxx::util::Json::array({"line3"})               },
        {"file_patterns", agentxx::util::Json::array({testDir + "/test1.txt"})},
        {"output_mode",   "content"                                           },
        {"context_lines", 1                                                   },
    };
    auto result = co_await tool.execute_async(args);
    // 分组格式下组头 "{filepath}:" 仅一次; 匹配行用 `:` 分隔, 上下文行用 `-` 分隔
    // (对齐 grep -C), 行前缀不再重复文件路径
    bool hasFileHeader  = result.find("test1.txt:\n") != std::string::npos;
    bool hasMatchLine   = result.find("\n3:line3") != std::string::npos;
    bool hasContextPrev = result.find("\n2-line2") != std::string::npos;
    bool hasContextNext = result.find("\n4-line4") != std::string::npos;
    if (hasFileHeader && hasMatchLine && hasContextPrev && hasContextNext) {
        g_fs_passed++;
        TEST_PASS << "FilesystemGrepTool context_lines works" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemGrepTool context_lines failed, got: " << result << std::endl;
    }
    co_return;
}

asio::awaitable<void> test_glob_type_filter(std::weak_ptr<agentxx::agent::AgentContext> agentContext
) {
    auto tool = agentxx::tools::FilesystemGlobTool{agentContext};
    // 只匹配目录
    auto args = agentxx::util::Json{
        {"file_patterns", agentxx::util::Json::array({testDir + "/*"})},
        {"type",          "dir"                                       },
    };
    auto result = co_await tool.execute_async(args);
    // 应包含 subdir, 不应包含 test1.txt
    if (result.find("subdir") != std::string::npos
        && result.find("test1.txt") == std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FilesystemGlobTool type filter works" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemGlobTool type filter failed, got: " << result << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_glob_exclude_patterns(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FilesystemGlobTool{agentContext};
    // 匹配所有 txt, 排除 test1.txt
    auto args = agentxx::util::Json{
        {"file_patterns",    agentxx::util::Json::array({testDir + "/*.txt"})    },
        {"exclude_patterns", agentxx::util::Json::array({testDir + "/test1.txt"})},
    };
    auto result = co_await tool.execute_async(args);
    // 应包含 test2.txt, 不应包含 test1.txt
    if (result.find("test2.txt") != std::string::npos
        && result.find("test1.txt") == std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FilesystemGlobTool exclude_patterns works" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemGlobTool exclude_patterns failed, got: " << result << std::endl;
    }
    co_return;
}

/// glob 路径匹配固定为大小写敏感 (case-insensitive 支持已移除):
/// 大写模式匹配小写文件应失败
asio::awaitable<void>
    test_glob_case_sensitive(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FilesystemGlobTool{agentContext};
    // testDir 下只有小写 test1.txt, 大写模式应匹配不到 (大小写敏感)
    auto args = agentxx::util::Json{
        {"file_patterns", agentxx::util::Json::array({testDir + "/TEST1.TXT"})},
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("test1.txt") == std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FilesystemGlobTool path matching is case-sensitive" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemGlobTool case-sensitive failed, got: " << result << std::endl;
    }
    co_return;
}

/// max_depth 过滤 (glob 库 static_prefix + path_depth):
/// `{testDir}/**/*.txt` 递归匹配, max_depth=1 时 subdir/subtest.txt (深度 2) 应被排除
asio::awaitable<void> test_glob_max_depth(std::weak_ptr<agentxx::agent::AgentContext> agentContext
) {
    auto tool = agentxx::tools::FilesystemGlobTool{agentContext};
    auto args = agentxx::util::Json{
        {"file_patterns", agentxx::util::Json::array({testDir + "/**/*.txt"})},
        {"max_depth",     1                                                  },
    };
    auto result = co_await tool.execute_async(args);
    // 顶层 test1.txt 应保留, subdir/subtest.txt 应被 max_depth 排除
    if (result.find("test1.txt") != std::string::npos
        && result.find("subtest.txt") == std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FilesystemGlobTool max_depth excludes deeper entries" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemGlobTool max_depth failed, got: " << result << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_glob_non_recursive(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FilesystemGlobTool{agentContext};
    // 不含 `**` 的模式不应递归: *.txt 只匹配当前目录
    auto args = agentxx::util::Json{
        {"file_patterns", agentxx::util::Json::array({testDir + "/*.txt"})},
    };
    auto result = co_await tool.execute_async(args);
    // 应包含 test1.txt, test2.txt, 不应包含 subdir/subtest.txt
    if (result.find("test1.txt") != std::string::npos
        && result.find("subtest.txt") == std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FilesystemGlobTool non-recursive glob does not descend into subdirs"
                  << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemGlobTool non-recursive glob failed, got: " << result << std::endl;
    }
    co_return;
}

/// 回归测试: 工作协程快速失败 (file_patterns 无匹配) 时, 必须立即返回真实错误,
/// 而不能被 `workFuture || timer` 的 wait_for_one_success 语义拖到满超时才返回
asio::awaitable<void>
    test_grep_no_match_fail_fast(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FilesystemGrepTool{agentContext};
    auto args = agentxx::util::Json{
        {"text_patterns", agentxx::util::Json::array({"never_match_any_text"})     },
        {"file_patterns", agentxx::util::Json::array({testDir + "/no_such_dir/**"})},
        {"timeout",       60                                                       }, // 修复前会白等 60s
    };
    auto t0     = std::chrono::steady_clock::now();
    auto result = co_await tool.execute_async(args);
    auto ms     = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0
    )
                  .count();
    // 应返回真实错误而不是 "timed out", 且耗时远小于 timeout
    if (result.find("No match") != std::string::npos && ms < 10000) {
        g_fs_passed++;
        TEST_PASS << "FilesystemGrepTool fail-fast returns real error in " << ms << " ms"
                  << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemGrepTool should return real error immediately, got: " << result
                  << " elapsed: " << ms << " ms" << std::endl;
    }
    co_return;
}

/// 回归测试: `**/*` 模式会匹配到目录, grep 应跳过目录继续搜索文件, 而不是报错
asio::awaitable<void>
    test_grep_skip_directories(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FilesystemGrepTool{agentContext};
    auto args = agentxx::util::Json{
        {"text_patterns", agentxx::util::Json::array({"hello world"})    },
        {"file_patterns", agentxx::util::Json::array({testDir + "/**/*"})},
        {"output_mode",   "files_with_matches"                           },
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("test2.txt") != std::string::npos
        && result.find("[Error]") == std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FilesystemGrepTool skips directories in glob results" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemGrepTool should skip dirs and search files, got: " << result
                  << std::endl;
    }
    co_return;
}

/// 内存压力测试: 循环多次调用 grep, 用于排查内存泄漏/堆损坏
asio::awaitable<void> test_grep_mem_stress(std::weak_ptr<agentxx::agent::AgentContext> agentContext
) {
    namespace fs = std::filesystem;
    // 构造较大测试目录: 100 个文件, 便于观察内存行为
    auto stressDir = testDir + "/stress";
    fs::create_directories(stressDir);
    for (int i = 0; i < 100; i++) {
        std::ofstream f(stressDir + "/f" + std::to_string(i) + ".txt");
        for (int j = 0; j < 10; j++) {
            f << "stress line token_" << i << " num " << j << "\n";
        }
    }
    fs::create_directories(stressDir + "/sub");
    for (int i = 0; i < 10; i++) {
        std::ofstream f(stressDir + "/sub/s" + std::to_string(i) + ".cpp");
        f << "int func" << i << "() { return " << i << "; }\n";
    }

    auto tool = agentxx::tools::FilesystemGrepTool{agentContext};

    auto rssKB = []() -> long {
        std::ifstream f("/proc/self/status");
        std::string   line;
        while (std::getline(f, line)) {
            if (line.rfind("VmRSS:", 0) == 0) {
                return std::stol(line.substr(6));
            }
        }
        return -1;
    };

    // 正则 + files_with_matches
    auto args = agentxx::util::Json{
        {"regex_patterns", agentxx::util::Json::array({"token_\\d+", "func\\d+"})},
        {"file_patterns",  agentxx::util::Json::array({stressDir + "/**/*"})     },
    };
    auto rss0 = rssKB();
    for (int i = 0; i < 30; i++) {
        auto r = co_await tool.execute_async(args);
        if (r.empty()) {
            g_fs_failed++;
            TEST_FAIL << "grep stress regex iter " << i << " returned empty" << std::endl;
            co_return;
        }
    }
    auto rss1 = rssKB();

    // 纯文本 (AhoCorasick) + content 模式
    auto args2 = agentxx::util::Json{
        {"text_patterns", agentxx::util::Json::array({"token_5", "func3"}) },
        {"file_patterns", agentxx::util::Json::array({stressDir + "/**/*"})},
        {"output_mode",   "content"                                        },
    };
    for (int i = 0; i < 30; i++) {
        auto r = co_await tool.execute_async(args2);
        if (r.empty()) {
            g_fs_failed++;
            TEST_FAIL << "grep stress plain iter " << i << " returned empty" << std::endl;
            co_return;
        }
    }
    auto rss2 = rssKB();

    TEST_INFO << "grep stress RSS: start=" << rss0 << "KB after_regex30=" << rss1
              << "KB after_plain30=" << rss2 << "KB" << std::endl;
    g_fs_passed++;
    TEST_PASS << "FilesystemGrepTool stress 60 iterations ok" << std::endl;
}

/// 回归测试: glob 遍历含 Unicode/GBK外字符的目录树 (如 "utf8_dir_ßµ™∃")
/// 验证 1) recursive glob 遍历不抛异常且无损保留 UTF-8 字符;
///      2) 非 ASCII 字面模式直接匹配该目录下的文件
asio::awaitable<void>
    test_glob_unicode_paths(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    namespace fs    = std::filesystem;
    auto unicodeDir = testDir + "/utf8_glob_ßµ™∃";
    fs::create_directories(agentxx::util::utf8ToPath(unicodeDir));
    {
        std::ofstream f(agentxx::util::utf8ToPath(unicodeDir + "/sample.txt"));
        f << "unicode glob content\n";
    }

    auto tool = agentxx::tools::FilesystemGlobTool{agentContext};

    // 1) 递归 glob: 应能遍历该目录且路径中包含 UTF-8 字符 (不抛异常)
    auto argsRec = agentxx::util::Json{
        {"file_patterns", agentxx::util::Json::array({testDir + "/**/*"})},
        {"limit",         0                                              },
    };
    auto resRec        = co_await tool.execute_async(argsRec);
    bool hasFile       = resRec.find("sample.txt") != std::string::npos;
    bool hasUnicodeDir = resRec.find("utf8_glob_ßµ™∃") != std::string::npos;

    // 2) 直接用含非 ASCII 字符的 pattern 匹配
    auto argsDirect = agentxx::util::Json{
        {"file_patterns", agentxx::util::Json::array({unicodeDir + "/*.txt"})},
    };
    auto resDirect      = co_await tool.execute_async(argsDirect);
    bool hasDirectMatch = resDirect.find("sample.txt") != std::string::npos;

    fs::remove_all(agentxx::util::utf8ToPath(unicodeDir));

    if (hasFile && hasUnicodeDir && hasDirectMatch) {
        g_fs_passed++;
        TEST_PASS << "FilesystemGlobTool handles Unicode/non-GBK directory paths" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemGlobTool Unicode path failed, resRec: " << resRec
                  << ", resDirect: " << resDirect << std::endl;
    }
    co_return;
}

/// 回归测试: grep 遍历含 Unicode/GBK外字符的目录树并检索文件内容
/// 验证 MSVC 下 fs::path 窄化不会抛 ERROR_NO_UNICODE_TRANSLATION
asio::awaitable<void>
    test_grep_unicode_path_and_content(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    namespace fs    = std::filesystem;
    auto unicodeDir = testDir + "/utf8_grep_ßµ™∃";
    fs::create_directories(agentxx::util::utf8ToPath(unicodeDir));
    {
        std::ofstream f(agentxx::util::utf8ToPath(unicodeDir + "/target.txt"));
        f << "match_token_in_unicode_dir_12345\nother content\n";
    }

    auto tool = agentxx::tools::FilesystemGrepTool{agentContext};
    auto args = agentxx::util::Json{
        {"text_patterns", agentxx::util::Json::array({"match_token_in_unicode_dir"})},
        {"file_patterns", agentxx::util::Json::array({testDir + "/**/*"})           },
        {"output_mode",   "files_with_matches"                                      },
    };
    auto result   = co_await tool.execute_async(args);
    bool hasMatch = result.find("target.txt") != std::string::npos
                    && result.find("[Error]") == std::string::npos;

    fs::remove_all(agentxx::util::utf8ToPath(unicodeDir));

    if (hasMatch) {
        g_fs_passed++;
        TEST_PASS << "FilesystemGrepTool searches inside Unicode/non-GBK directory paths"
                  << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemGrepTool Unicode search failed, got: " << result << std::endl;
    }
    co_return;
}

/// 回归测试: grep 多 pattern 场景下单 pattern 失败/无匹配被 catchError 隔离,
/// 其余有效 pattern 仍能正常命中
asio::awaitable<void>
    test_grep_multi_pattern_fault_tolerance(std::weak_ptr<agentxx::agent::AgentContext> agentContext
    ) {
    auto tool = agentxx::tools::FilesystemGrepTool{agentContext};
    // 传入两个 pattern: 一个是指向不存在目录的 pattern, 另一个是指向有效文件的 pattern
    auto args = agentxx::util::Json{
        {"text_patterns", agentxx::util::Json::array({"hello world"})                               },
        {"file_patterns",
         agentxx::util::Json::array({testDir + "/no_such_sub_dir/**/*.txt", testDir + "/test2.txt"})
        },
        {"output_mode",   "files_with_matches"                                                      },
    };
    auto result = co_await tool.execute_async(args);
    // 有效 pattern (test2.txt) 应正常命中并返回, 不应因前一个 pattern 为空或报错而整体失败
    if (result.find("test2.txt") != std::string::npos
        && result.find("[Error]") == std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "FilesystemGrepTool multi-pattern fault tolerance (catchError) works"
                  << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemGrepTool multi-pattern fault tolerance failed, got: " << result
                  << std::endl;
    }
    co_return;
}

/// 中文/UTF-8 路径测试: list 工具遍历含中文的目录与子目录
asio::awaitable<void>
    test_list_file_chinese_path(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    namespace fs    = std::filesystem;
    auto chineseDir = testDir + "/中文测试目录_列表";
    auto subDir     = chineseDir + "/子目录_一级";
    fs::create_directories(agentxx::util::utf8ToPath(subDir));
    {
        std::ofstream f1(agentxx::util::utf8ToPath(chineseDir + "/中文文件_一.txt"));
        f1 << "文件1内容\n";
    }
    {
        std::ofstream f2(agentxx::util::utf8ToPath(subDir + "/深层中文文件.txt"));
        f2 << "深层内容\n";
    }

    auto tool = agentxx::tools::FileSystemListTool{agentContext};

    // 1) 非递归列出
    auto argsNonRec = agentxx::util::Json{
        {"path",      chineseDir},
        {"recursive", false     }
    };
    auto resNonRec = co_await tool.execute_async(argsNonRec);
    bool hasFile1  = resNonRec.find("中文文件_一.txt") != std::string::npos;
    bool hasSubDir = resNonRec.find("子目录_一级") != std::string::npos;
    bool noSubFile = resNonRec.find("深层中文文件.txt") == std::string::npos;

    // 2) 递归列出
    auto argsRec = agentxx::util::Json{
        {"path",      chineseDir},
        {"recursive", true      }
    };
    auto resRec     = co_await tool.execute_async(argsRec);
    bool hasSubFile = resRec.find("深层中文文件.txt") != std::string::npos;

    fs::remove_all(agentxx::util::utf8ToPath(chineseDir));

    if (hasFile1 && hasSubDir && noSubFile && hasSubFile) {
        g_fs_passed++;
        TEST_PASS << "FileSystemListTool handles Chinese/UTF-8 path listing" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FileSystemListTool Chinese path listing failed, resNonRec: " << resNonRec
                  << ", resRec: " << resRec << std::endl;
    }
    co_return;
}

/// 中文/UTF-8 路径测试: read 工具读取含中文路径的文本文件 (全量 + offset/limit)
asio::awaitable<void>
    test_read_text_file_chinese_path(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    namespace fs    = std::filesystem;
    auto chineseDir = testDir + "/中文测试目录_读取";
    fs::create_directories(agentxx::util::utf8ToPath(chineseDir));
    auto filePath = chineseDir + "/读取测试文件_中文.txt";
    {
        std::ofstream f(agentxx::util::utf8ToPath(filePath));
        f << "第一行中文数据\n第二行关键内容\n第三行结束行\n";
    }

    auto tool = agentxx::tools::FilesystemReadTextFileTool{agentContext};

    // 1) 全量读取 (异步/stream_file)
    auto argsFull = agentxx::util::Json{
        {"path", filePath}
    };
    auto resFull = co_await tool.execute_async(argsFull);
    bool fullOk  = resFull.find("第一行中文数据") != std::string::npos
                  && resFull.find("第三行结束行") != std::string::npos;

    // 2) 分段读取 (offset + limit)
    auto argsSlice = agentxx::util::Json{
        {"path",        filePath},
        {"line_offset", 1       },
        {"line_limit",  1       }
    };
    auto resSlice = co_await tool.execute_async(argsSlice);
    bool sliceOk  = resSlice.find("第二行关键内容") != std::string::npos
                   && resSlice.find("第一行中文数据") == std::string::npos;

    // 3) 同步执行体直测
    auto resSyncFull  = agentxx_fs_plugin::fileReadExecute(argsFull, testDir);
    auto resSyncSlice = agentxx_fs_plugin::fileReadExecute(argsSlice, testDir);
    bool syncOk       = resSyncFull.find("第一行中文数据") != std::string::npos
                  && resSyncSlice.find("第二行关键内容") != std::string::npos;

    fs::remove_all(agentxx::util::utf8ToPath(chineseDir));

    if (fullOk && sliceOk && syncOk) {
        g_fs_passed++;
        TEST_PASS << "FilesystemReadTextFileTool handles Chinese/UTF-8 path reading" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemReadTextFileTool Chinese path reading failed, resFull: " << resFull
                  << ", resSlice: " << resSlice << std::endl;
    }
    co_return;
}

/// 中文/UTF-8 路径测试: write 工具在多层中文路径下创建与覆盖文件
asio::awaitable<void>
    test_write_file_chinese_path(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    namespace fs    = std::filesystem;
    auto chineseDir = testDir + "/中文测试目录_写入/多层子目录";
    auto filePath   = chineseDir + "/新建中文文件.txt";

    auto tool = agentxx::tools::FilesystemWriteFileTool{agentContext};

    // 1) 首次创建: 自动创建多层中文父目录
    auto argsCreate = agentxx::util::Json{
        {"path",      filePath                 },
        {"content",   "中文内容第一版\n"},
        {"overwrite", false                    }
    };
    auto resCreate = co_await tool.execute_async(argsCreate);
    bool createOk  = (resCreate == "success") && fs::exists(agentxx::util::utf8ToPath(filePath));

    // 2) overwrite=false 再次写入应报错
    bool noOverwriteThrew = false;
    try {
        auto out = co_await tool.execute_async(argsCreate);
        if (agentxx::util::isIgnoreCaseContains(out, "error")
            || agentxx::util::isIgnoreCaseContains(out, "already exist")) {
            noOverwriteThrew = true;
        }
    } catch (...) {
        noOverwriteThrew = true;
    }

    // 3) overwrite=true 成功覆盖
    auto argsOverwrite = agentxx::util::Json{
        {"path",      filePath                    },
        {"content",   "覆盖后的中文内容\n"},
        {"overwrite", true                        }
    };
    auto resOverwrite = co_await tool.execute_async(argsOverwrite);
    bool overwriteOk  = (resOverwrite == "success");

    // 验证文件内容
    std::string readContent;
    {
        std::ifstream in(agentxx::util::utf8ToPath(filePath));
        readContent
            = std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    bool contentOk = readContent.find("覆盖后的中文内容") != std::string::npos;

    // 4) 同步版 write 直测
    auto syncFilePath = chineseDir + "/同步写入文件.txt";
    auto syncArgs     = agentxx::util::Json{
            {"path",      syncFilePath          },
            {"content",   "同步中文写入\n"},
            {"overwrite", true                  }
    };
    auto syncRes = agentxx_fs_plugin::fileWriteExecute(syncArgs, testDir);
    bool syncOk  = (syncRes == "success") && fs::exists(agentxx::util::utf8ToPath(syncFilePath));

    fs::remove_all(agentxx::util::utf8ToPath(testDir + "/中文测试目录_写入"));

    if (createOk && noOverwriteThrew && overwriteOk && contentOk && syncOk) {
        g_fs_passed++;
        TEST_PASS << "FilesystemWriteFileTool handles Chinese/UTF-8 path writing" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemWriteFileTool Chinese path write failed, createOk: " << createOk
                  << ", noOverwriteThrew: " << noOverwriteThrew << ", overwriteOk: " << overwriteOk
                  << ", contentOk: " << contentOk << ", syncOk: " << syncOk << std::endl;
    }
    co_return;
}

/// 中文/UTF-8 路径测试: edit 工具在中文路径下进行单处/多处替换
asio::awaitable<void>
    test_edit_text_file_chinese_path(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    namespace fs    = std::filesystem;
    auto chineseDir = testDir + "/中文测试目录_编辑";
    fs::create_directories(agentxx::util::utf8ToPath(chineseDir));
    auto filePath = chineseDir + "/编辑目标文件_中文.txt";
    {
        std::ofstream f(agentxx::util::utf8ToPath(filePath));
        f << "前缀行\n目标旧字符串_AAA\n中间行\n目标旧字符串_AAA\n后缀行\n";
    }

    auto tool = agentxx::tools::FilesystemEditTextFileTool{agentContext};

    // 1) 单处替换
    auto argsSingle = agentxx::util::Json{
        {"path",          filePath                   },
        {"old_str",       "目标旧字符串_AAA"   },
        {"new_str",       "已替换新字符串_BBB"},
        {"multi_replace", false                      }
    };
    auto resSingle = co_await tool.execute_async(argsSingle);
    bool singleOk  = (resSingle == "success");

    // 2) 多处替换 (剩余的一处替换)
    auto argsMulti = agentxx::util::Json{
        {"path",          filePath                   },
        {"old_str",       "目标旧字符串_AAA"   },
        {"new_str",       "已替换新字符串_CCC"},
        {"multi_replace", true                       }
    };
    auto resMulti = co_await tool.execute_async(argsMulti);
    bool multiOk  = resMulti.find("Success") != std::string::npos
                   || resMulti.find("success") != std::string::npos;

    // 验证文件最终内容
    std::string readContent;
    {
        std::ifstream in(agentxx::util::utf8ToPath(filePath));
        readContent
            = std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    bool contentOk = readContent.find("已替换新字符串_BBB") != std::string::npos
                     && readContent.find("已替换新字符串_CCC") != std::string::npos
                     && readContent.find("目标旧字符串_AAA") == std::string::npos;

    // 3) 同步版 edit 直测
    auto syncArgs = agentxx::util::Json{
        {"path",          filePath                   },
        {"old_str",       "已替换新字符串_BBB"},
        {"new_str",       "同步替换新内容"    },
        {"multi_replace", false                      }
    };
    auto syncRes = agentxx_fs_plugin::fileEditExecute(syncArgs, testDir);
    bool syncOk  = (syncRes == "success");

    fs::remove_all(agentxx::util::utf8ToPath(chineseDir));

    if (singleOk && multiOk && contentOk && syncOk) {
        g_fs_passed++;
        TEST_PASS << "FilesystemEditTextFileTool handles Chinese/UTF-8 path editing" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemEditTextFileTool Chinese path edit failed, resSingle: " << resSingle
                  << ", resMulti: " << resMulti << ", contentOk: " << contentOk
                  << ", syncOk: " << syncOk << std::endl;
    }
    co_return;
}

/// 中文/UTF-8 路径测试: glob 工具匹配中文目录和中文文件名
asio::awaitable<void>
    test_glob_chinese_paths(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    namespace fs    = std::filesystem;
    auto chineseDir = testDir + "/中文测试目录_通配符/子目录_中文";
    fs::create_directories(agentxx::util::utf8ToPath(chineseDir));
    {
        std::ofstream f1(agentxx::util::utf8ToPath(chineseDir + "/文档_一.txt"));
        f1 << "content1\n";
        std::ofstream f2(agentxx::util::utf8ToPath(chineseDir + "/文档_二.log"));
        f2 << "content2\n";
    }

    auto tool = agentxx::tools::FilesystemGlobTool{agentContext};

    // 1) 递归 pattern 匹配中文目录下的 .txt
    auto argsRec = agentxx::util::Json{
        {"file_patterns", agentxx::util::Json::array({testDir + "/中文测试目录_通配符/**/*.txt"})},
    };
    auto resRec = co_await tool.execute_async(argsRec);
    bool recOk  = resRec.find("文档_一.txt") != std::string::npos
                 && resRec.find("文档_二.log") == std::string::npos;

    // 2) 直接用含中文的 pattern 匹配
    auto argsDirect = agentxx::util::Json{
        {"file_patterns", agentxx::util::Json::array({chineseDir + "/*"})},
    };
    auto resDirect = co_await tool.execute_async(argsDirect);
    bool directOk  = resDirect.find("文档_一.txt") != std::string::npos
                    && resDirect.find("文档_二.log") != std::string::npos;

    fs::remove_all(agentxx::util::utf8ToPath(testDir + "/中文测试目录_通配符"));

    if (recOk && directOk) {
        g_fs_passed++;
        TEST_PASS << "FilesystemGlobTool handles Chinese/UTF-8 pattern and path matching"
                  << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemGlobTool Chinese path matching failed, resRec: " << resRec
                  << ", resDirect: " << resDirect << std::endl;
    }
    co_return;
}

/// 中文/UTF-8 路径测试: grep 工具在中文路径与中文内容下搜索
asio::awaitable<void>
    test_grep_chinese_path_and_content(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    namespace fs    = std::filesystem;
    auto chineseDir = testDir + "/中文测试目录_检索";
    fs::create_directories(agentxx::util::utf8ToPath(chineseDir));
    auto filePath = chineseDir + "/检索目标_中文.txt";
    {
        std::ofstream f(agentxx::util::utf8ToPath(filePath));
        f << "首行无用数据\n检索特征码_中文关键字_98765\n末行结束\n";
    }

    auto tool = agentxx::tools::FilesystemGrepTool{agentContext};

    // 1) files_with_matches 文本搜索
    auto argsFwm = agentxx::util::Json{
        {"text_patterns", agentxx::util::Json::array({"检索特征码_中文关键字"})},
        {"file_patterns", agentxx::util::Json::array({chineseDir + "/*.txt"})            },
        {"output_mode",   "files_with_matches"                                           }
    };
    auto resFwm = co_await tool.execute_async(argsFwm);
    bool fwmOk  = resFwm.find("检索目标_中文.txt") != std::string::npos
                 && resFwm.find("[Error]") == std::string::npos;

    // 2) content 模式正则搜索
    auto argsContent = agentxx::util::Json{
        {"regex_patterns", agentxx::util::Json::array({R"(检索特征码_中文关键字_\d+)"})},
        {"file_patterns",  agentxx::util::Json::array({testDir + "/**/*.txt"})                   },
        {"output_mode",    "content"                                                             }
    };
    auto resContent = co_await tool.execute_async(argsContent);
    bool contentOk  = resContent.find("检索目标_中文.txt") != std::string::npos
                     && resContent.find("检索特征码_中文关键字_98765") != std::string::npos;

    fs::remove_all(agentxx::util::utf8ToPath(chineseDir));

    if (fwmOk && contentOk) {
        g_fs_passed++;
        TEST_PASS << "FilesystemGrepTool handles Chinese/UTF-8 path and content search"
                  << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemGrepTool Chinese path/content search failed, resFwm: " << resFwm
                  << ", resContent: " << resContent << std::endl;
    }
    co_return;
}

/// 同步兜底路径测试: 模拟"文件异步 I/O 不可用"的环境 —— 经
/// agentxx::util::setAsyncFileIoSupported(false) 强制关闭后, read/write/edit 的
/// 协程执行体应回退同步实现 (编译期无 asio 文件 I/O, 或运行环境 io_uring 被
/// seccomp 拦截时的实际路径), 且产出与自动探测路径完全一致。
/// 关闭只作用于本测试作用域, 结束时恢复自动探测
asio::awaitable<void> test_sync_fallback_without_async_file_io(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext
) {
    namespace fs = std::filesystem;
    auto dirPath = testDir + "/同步兜底测试目录";
    fs::create_directories(agentxx::util::utf8ToPath(dirPath));
    auto filePath = dirPath + "/sync_fallback.txt";

    auto writeTool = agentxx::tools::FilesystemWriteFileTool{agentContext};
    auto readTool  = agentxx::tools::FilesystemReadTextFileTool{agentContext};
    auto editTool  = agentxx::tools::FilesystemEditTextFileTool{agentContext};

    auto writeArgs = agentxx::util::Json{
        {"path",      filePath                                     },
        {"content",   "同步兜底第一行\n目标旧字符串\n"},
        {"overwrite", true                                         }
    };
    auto readArgs = agentxx::util::Json{
        {"path", filePath}
    };
    auto readPartArgs = agentxx::util::Json{
        {"path",        filePath},
        {"line_offset", 1       },
        {"line_limit",  1       }
    };
    auto editArgs = agentxx::util::Json{
        {"path",          filePath               },
        {"old_str",       "目标旧字符串"   },
        {"new_str",       "已替换新字符串"},
        {"multi_replace", false                  }
    };

    // 记录一段完整 写→读→改→读 的输出 (供两种路径比对)
    struct RunResult {
        std::string writeRes;
        std::string readRes;
        std::string readPartRes;
        std::string editRes;
        std::string finalContent;
    };

    auto runOnce = [&]() -> asio::awaitable<RunResult> {
        RunResult r;
        r.writeRes    = co_await writeTool.execute_async(writeArgs);
        r.readRes     = co_await readTool.execute_async(readArgs);
        r.readPartRes = co_await readTool.execute_async(readPartArgs);
        r.editRes     = co_await editTool.execute_async(editArgs);
        std::ifstream in{agentxx::util::utf8ToPath(filePath)};
        r.finalContent
            = std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        co_return r;
    };

    RunResult syncRun;
    // 关闭前的自动探测结果 (首次调用即触发探测并缓存, 供恢复后比对)
    const bool autoDetected = agentxx::util::isAsyncFileIoSupported();
    {
        /// 作用域内强制关闭文件异步 I/O, 退出时 (含异常) 恢复自动探测
        struct ScopedDisableAsyncFileIo {
            ScopedDisableAsyncFileIo() {
                agentxx::util::setAsyncFileIoSupported(false);
            }

            ~ScopedDisableAsyncFileIo() {
                agentxx::util::resetAsyncFileIoSupported();
            }
        } scopedDisable;

        XX_TEST_EXPECT_FALSE(agentxx::util::isAsyncFileIoSupported());
        syncRun = co_await runOnce();
        XX_TEST_EXPECT_FALSE(agentxx::util::isAsyncFileIoSupported());
    }
    // 作用域结束恢复自动探测: 判断结果回到探测值, 不再被强制关闭
    XX_TEST_EXPECT_EQ(agentxx::util::isAsyncFileIoSupported(), autoDetected);

    // 同步兜底路径本身必须可用且行为正确
    bool syncOk = syncRun.writeRes.find("success") != std::string::npos
                  && syncRun.readRes.find("同步兜底第一行") != std::string::npos
                  && syncRun.readPartRes.find("目标旧字符串") != std::string::npos
                  && syncRun.editRes == "success"
                  && syncRun.finalContent.find("已替换新字符串") != std::string::npos
                  && syncRun.finalContent.find("目标旧字符串") == std::string::npos;

    // 自动探测路径 (本机可用则为真异步实现) 与同步兜底路径产出应完全一致
    auto autoRun = co_await runOnce();
    bool sameOk  = autoRun.writeRes == syncRun.writeRes && autoRun.readRes == syncRun.readRes
                  && autoRun.readPartRes == syncRun.readPartRes
                  && autoRun.editRes == syncRun.editRes
                  && autoRun.finalContent == syncRun.finalContent;

    fs::remove_all(agentxx::util::utf8ToPath(dirPath));

    if (syncOk && sameOk) {
        g_fs_passed++;
        TEST_PASS << "Filesystem tools sync fallback (async file io disabled) matches async path"
                  << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "Filesystem tools sync fallback failed, syncOk: " << syncOk
                  << ", sameOk: " << sameOk << ", syncRead: " << syncRun.readRes
                  << ", autoRead: " << autoRun.readRes << std::endl;
    }
    co_return;
}

/// 插件真实链路冒烟测试: dlopen agentxx_filesystem .so, 经宿主 PluginManager/
/// op_driver 全链路执行 —— 覆盖单测直测 impl 纯函数覆盖不到的接线层:
///   - read/write/edit: poll 寄生驱动三件套 (PolledToolShim start→poll 步进
///     →done 上报; asio stream_file 异步文件 I/O 在寄生 loop 上推进)
///   - list/grep: offload线程池适配异步接口
/// 会话工作目录经宿主 get_session_work_dir 接口注入 (绑定 testDir), 同时覆盖
/// work_dir 接口表装配; 插件未构建 (无 .so 产物) 时优雅跳过
asio::awaitable<void> test_plugin_real_link() {
    namespace fs = std::filesystem;

    // 定位插件库目录 (与 test_plugins 同模式: exe 同目录优先, cwd 回退)
    std::error_code       ec;
    std::vector<fs::path> candidates;
    if (auto p = fs::read_symlink("/proc/self/exe", ec); !ec) {
        candidates.push_back(p.parent_path() / "plugins" / "agentxx_filesystem");
    }
    candidates.push_back(fs::current_path(ec) / "plugins" / "agentxx_filesystem");
    auto hasLibFile = [](const fs::path& dir) {
        std::error_code                     ec2;
        std::filesystem::directory_iterator it(dir, ec2);
        std::filesystem::directory_iterator end;
        for (; it != end; it.increment(ec2)) {
            auto ext = it->path().extension().string();
            if (ext == ".so" || ext == ".dll" || ext == ".dylib") {
                return true;
            }
        }
        return false;
    };
    std::string pluginDir;
    for (const auto& c : candidates) {
        if (fs::is_directory(c, ec) && hasLibFile(c)) {
            pluginDir = c.string();
            break;
        }
    }
    if (pluginDir.empty()) {
        g_fs_passed++;
        TEST_INFO << "plugin real-link skipped (agentxx_filesystem .so not found)" << std::endl;
        co_return;
    }

    // 构造最小 AgentContext (io 线程环境, 与 test_plugins/库内无锁模型一致)
    auto linkCtx                     = std::make_shared<agentxx::agent::AgentContext>();
    linkCtx->agentConfig             = std::make_shared<agentxx::agent::AgentConfig>();
    linkCtx->agentConfig->workDir    = testDir; ///< 相对路径基准经宿主接口注入
    linkCtx->middlewareHandleContext = std::make_shared<agentxx::middleware::MiddlewareContext>();
    linkCtx->bus = std::make_shared<agentxx::events::EventBus>(co_await asio::this_coro::executor);
    linkCtx->toolRegistry  = std::make_shared<agentxx::plugin::ToolRegistry>();
    linkCtx->pluginManager = std::make_shared<agentxx::plugin::PluginManager>(linkCtx);
    linkCtx->pluginManager->setIoExecutor(co_await asio::this_coro::executor);

    auto inst = co_await linkCtx->pluginManager->loadPluginAsync(pluginDir);
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (!inst) {
        g_fs_passed++;
        TEST_INFO << "plugin real-link skipped (loadPluginAsync failed)" << std::endl;
        co_return;
    }

    // 六工具全部注册
    for (const char* name :
         {"agentxx_filesystem_list",
          "agentxx_filesystem_read",
          "agentxx_filesystem_write",
          "agentxx_filesystem_edit",
          "agentxx_filesystem_glob",
          "agentxx_filesystem_grep"}) {
        XX_TEST_EXPECT_TRUE(linkCtx->toolRegistry->contains(name));
    }

    // 验证 glob、grep、list 启用了 autoSummaryOutput (经 flags 传入 ToolcallNode 自动截断压缩),
    // read 不开启
    XX_TEST_EXPECT_EQ(
        linkCtx->toolRegistry->find("agentxx_filesystem_glob")->extra["autoSummaryOutput"],
        std::string{"true"}
    );
    XX_TEST_EXPECT_EQ(
        linkCtx->toolRegistry->find("agentxx_filesystem_grep")->extra["autoSummaryOutput"],
        std::string{"true"}
    );
    XX_TEST_EXPECT_EQ(
        linkCtx->toolRegistry->find("agentxx_filesystem_read")->extra["autoSummaryOutput"],
        std::string{"false"}
    );
    XX_TEST_EXPECT_EQ(
        linkCtx->toolRegistry->find("agentxx_filesystem_list")->extra["autoSummaryOutput"],
        std::string{"true"}
    );

    // 经 ToolRegistry 全链路执行 (op_driver 驱动插件三件套); sessionId 注入
    // thread_id → 会话工作目录解析链路
    auto callTool
        = [&](const char* name, const agentxx::util::Json& args) -> asio::awaitable<std::string> {
        auto tool = linkCtx->toolRegistry->find(name);
        if (!tool) {
            co_return "[Error] tool not found";
        }
        auto merged         = args;
        merged["sessionId"] = "t_fs_link";
        co_return co_await tool->execute_async(merged);
    };

    // write (polled): 相对路径以宿主 workDir 为基准创建文件
    {
        auto out = co_await callTool(
            "agentxx_filesystem_write",
            agentxx::util::Json{
                {"path",    "link_smoke.txt"},
                {"content", "alpha\nbeta\n" }
        }
        );
        XX_TEST_EXPECT_EQ(out, std::string{"success"});
        XX_TEST_EXPECT_TRUE(fs::exists(testDir + "/link_smoke.txt"));
    }

    // read (polled): 完整读取 + offset/limit 分段
    {
        auto out = co_await callTool(
            "agentxx_filesystem_read",
            agentxx::util::Json{
                {"path", "link_smoke.txt"}
        }
        );
        XX_TEST_EXPECT_TRUE(
            out.find("alpha") != std::string::npos && out.find("beta") != std::string::npos
        );
        auto part = co_await callTool(
            "agentxx_filesystem_read",
            agentxx::util::Json{
                {"path",        "link_smoke.txt"},
                {"line_offset", 1               },
                {"line_limit",  1               }
        }
        );
        XX_TEST_EXPECT_TRUE(
            part.find("beta") != std::string::npos && part.find("alpha") == std::string::npos
        );
    }

    // edit (polled): 替换并落盘
    {
        auto out = co_await callTool(
            "agentxx_filesystem_edit",
            agentxx::util::Json{
                {"path",    "link_smoke.txt"},
                {"old_str", "beta"          },
                {"new_str", "gamma"         }
        }
        );
        XX_TEST_EXPECT_EQ(out, std::string{"success"});
        std::ifstream in(testDir + "/link_smoke.txt");
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        XX_TEST_EXPECT_EQ(content, std::string{"alpha\ngamma\n"});
    }

    // list offload线程池适配异步接口: 列出目录内容
    {
        auto out = co_await callTool(
            "agentxx_filesystem_list",
            agentxx::util::Json{
                {"path", "."}
        }
        );
        XX_TEST_EXPECT_TRUE(out.find("link_smoke.txt") != std::string::npos);
    }

    // list offload线程池适配异步接口: 通配模式 (`*`) 展开后列出匹配条目
    {
        auto out = co_await callTool(
            "agentxx_filesystem_list",
            agentxx::util::Json{
                {"path", "*.txt"}
        }
        );
        XX_TEST_EXPECT_TRUE(out.find("link_smoke.txt") != std::string::npos);
        // 通配只匹配当前层: subdir 内的文件不出现在结果里
        XX_TEST_EXPECT_TRUE(out.find("subtest.txt") == std::string::npos);
    }

    // grep offload线程池适配异步接口: 文本搜索命中
    {
        auto out = co_await callTool(
            "agentxx_filesystem_grep",
            agentxx::util::Json{
                {"text_patterns", agentxx::util::Json::array({"gamma"})},
                {"file_patterns", agentxx::util::Json::array({"*.txt"})},
                {"output_mode",   "files_with_matches"                 }
        }
        );
        XX_TEST_EXPECT_TRUE(out.find("link_smoke.txt") != std::string::npos);
    }

    // 中文路径链路测试 (write -> read -> edit -> list -> grep)
    {
        auto outW = co_await callTool(
            "agentxx_filesystem_write",
            agentxx::util::Json{
                {"path",    "中文目录_真实链路/中文文件.txt"},
                {"content", "中文链路数据_初始版本\n"         }
        }
        );
        XX_TEST_EXPECT_EQ(outW, std::string{"success"});
        XX_TEST_EXPECT_TRUE(
            fs::exists(agentxx::util::utf8ToPath(testDir + "/中文目录_真实链路/中文文件.txt"))
        );

        auto outR = co_await callTool(
            "agentxx_filesystem_read",
            agentxx::util::Json{
                {"path", "中文目录_真实链路/中文文件.txt"}
        }
        );
        XX_TEST_EXPECT_TRUE(outR.find("中文链路数据_初始版本") != std::string::npos);

        auto outE = co_await callTool(
            "agentxx_filesystem_edit",
            agentxx::util::Json{
                {"path",    "中文目录_真实链路/中文文件.txt"},
                {"old_str", "初始版本"                              },
                {"new_str", "更新版本"                              }
        }
        );
        XX_TEST_EXPECT_EQ(outE, std::string{"success"});

        auto outL = co_await callTool(
            "agentxx_filesystem_list",
            agentxx::util::Json{
                {"path", "中文目录_真实链路/*.txt"}
        }
        );
        XX_TEST_EXPECT_TRUE(outL.find("中文文件.txt") != std::string::npos);

        auto outG = co_await callTool(
            "agentxx_filesystem_grep",
            agentxx::util::Json{
                {"text_patterns", agentxx::util::Json::array({"更新版本"})                   },
                {"file_patterns", agentxx::util::Json::array({"中文目录_真实链路/*.txt"})},
                {"output_mode",   "files_with_matches"                                           }
        }
        );
        XX_TEST_EXPECT_TRUE(outG.find("中文文件.txt") != std::string::npos);

        fs::remove_all(agentxx::util::utf8ToPath(testDir + "/中文目录_真实链路"), ec);
    }

    // 卸载 (寄生 loop)
    auto okUnload = co_await linkCtx->pluginManager->unloadAsync("agentxx_filesystem");
    XX_TEST_EXPECT_TRUE(okUnload);

    fs::remove(testDir + "/link_smoke.txt", ec);
}

/// 回归测试: 规模上限 (数量太多导致的内存/时间爆炸)
/// 背景: 一次 `file_patterns = agent/**/*` 会展开 389397 个文件 (其中
/// third_party 367089 / build 21771, 合计约 36GB), 而同一会话其余调用的模式
/// 合计只有 512 个文件 —— 规模需要可预期地上限约束。
/// 覆盖:
/// - grep `max_files`: 到限即停止收集文件, 保留已收集文件继续搜索, 结果首行给出
///   `[Note]` (提示还有更多文件未搜索)
/// - grep `max_file_size_mb`: 超大文件跳过并在结果末尾给出 `[Note]`
/// - glob `max_files`: 到限即停止遍历, 保留已匹配结果并在首行给出 `[Note]`
/// - list `max_files`: 超限截断并在输出首行给出 `[Note]`
asio::awaitable<void>
    test_scan_scale_limits(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    namespace fs               = std::filesystem;
    const std::string limitDir = testDir + "/scale_limit";
    if (fs::exists(limitDir)) {
        fs::remove_all(limitDir);
    }
    fs::create_directories(limitDir);
    // 30 个小文件 (各含 small_token) + 1 个 4KB 大文件 (含 big_token)
    for (int i = 0; i < 30; ++i) {
        std::ofstream f(limitDir + "/small" + std::to_string(i) + ".txt");
        f << "small_token line " << i << "\n";
    }
    {
        std::ofstream big(limitDir + "/big.txt");
        big << "big_token\n";
        big << std::string(4096, 'x') << "\n";
    }

    const auto workDir   = agentxx::tools::testResolvedWorkDir(agentContext);
    bool       allPassed = true;

    /// 统计输出中的"路径行"数量 (排除 `[Note]`/`[Error]` 前缀行)
    auto countPathLines = [](std::string_view text) -> size_t {
        size_t count = 0;
        for (auto line : agentxx::util::strSplit(text, '\n')) {
            if (line.empty() || line.starts_with("[Note]") || line.starts_with("[Error]")) {
                continue;
            }
            ++count;
        }
        return count;
    };

    /// 输出是否以提示行开头
    auto startsWithNote = [](std::string_view text) -> bool {
        return text.starts_with("[Note]");
    };

    // ① glob max_files: 到限停止遍历, 保留已匹配结果 + 首行提示
    {
        auto args = agentxx::util::Json{
            {"file_patterns", agentxx::util::Json::array({limitDir + "/**/*"})},
            {"max_files",     10                                              },
        };
        auto result     = ::agentxx_fs_plugin::fileGlobExecute(args, workDir);
        auto pathCount  = countPathLines(result);
        bool hasNote    = startsWithNote(result) && result.find("max_files") != std::string::npos;
        bool erasedNote = result.find("[Error]") == std::string::npos;
        if (false == (hasNote && erasedNote && pathCount == 10)) {
            allPassed = false;
            TEST_FAIL << "FilesystemGlobTool should stop at `max_files` and keep matches, got: "
                      << result.substr(0, 200) << std::endl;
        } else {
            TEST_PASS << "FilesystemGlobTool stops at `max_files` with a leading note" << std::endl;
        }
    }

    // ② glob max_files = 0 (不限): 全部匹配, 无提示
    {
        auto args = agentxx::util::Json{
            {"file_patterns", agentxx::util::Json::array({limitDir + "/**/*"})},
            {"max_files",     0                                               },
        };
        auto result    = ::agentxx_fs_plugin::fileGlobExecute(args, workDir);
        auto pathCount = countPathLines(result);
        if (pathCount != 31 || startsWithNote(result)) {
            allPassed = false;
            TEST_FAIL << "FilesystemGlobTool should list all matches when `max_files` = 0, got "
                      << pathCount << " lines: " << result.substr(0, 200) << std::endl;
        } else {
            TEST_PASS << "FilesystemGlobTool lists all matches when `max_files` = 0" << std::endl;
        }
    }

    // ③ glob max_files 未触发 (匹配数少于上限): 无提示, 结果完整
    {
        auto args = agentxx::util::Json{
            {"file_patterns", agentxx::util::Json::array({limitDir + "/*.txt"})},
            {"max_files",     100                                              },
        };
        auto result    = ::agentxx_fs_plugin::fileGlobExecute(args, workDir);
        auto pathCount = countPathLines(result);
        if (pathCount != 31 || startsWithNote(result)) {
            allPassed = false;
            TEST_FAIL << "FilesystemGlobTool should not truncate under `max_files`, got "
                      << pathCount << " lines: " << result.substr(0, 200) << std::endl;
        } else {
            TEST_PASS << "FilesystemGlobTool keeps full result under `max_files`" << std::endl;
        }
    }

    // ④ grep max_files: 到限停止收集文件, 已收集文件照常搜索 + 首行提示
    {
        auto args = agentxx::util::Json{
            {"file_patterns", agentxx::util::Json::array({limitDir + "/**/*"})},
            {"text_patterns", agentxx::util::Json::array({"small_token"})     },
            {"max_files",     10                                              },
        };
        auto result  = ::agentxx_fs_plugin::fileGrepExecute(args, workDir);
        bool hasNote = startsWithNote(result) && result.find("max_files") != std::string::npos;
        bool ok      = hasNote && countPathLines(result) > 0;
        if (false == ok) {
            allPassed = false;
            TEST_FAIL << "FilesystemGrepTool should scan the collected files and note the "
                         "`max_files` cutoff, got: "
                      << result.substr(0, 200) << std::endl;
        } else {
            TEST_PASS << "FilesystemGrepTool scans collected files with a leading `max_files` note"
                      << std::endl;
        }
    }

    // ⑤ grep max_files 未触发: 正常返回, 无提示
    {
        auto args = agentxx::util::Json{
            {"file_patterns", agentxx::util::Json::array({limitDir + "/**/*"})},
            {"text_patterns", agentxx::util::Json::array({"small_token"})     },
            {"max_files",     100                                             },
        };
        auto result = ::agentxx_fs_plugin::fileGrepExecute(args, workDir);
        if (result.find("small0.txt") == std::string::npos || startsWithNote(result)) {
            allPassed = false;
            TEST_FAIL << "FilesystemGrepTool should scan normally under `max_files`, got: "
                      << result.substr(0, 200) << std::endl;
        } else {
            TEST_PASS << "FilesystemGrepTool scans normally under `max_files`" << std::endl;
        }
    }

    // ⑥ grep 到限且已收集文件都无匹配: 错误文本说明"遍历提前停止", 不误报为"无匹配"
    {
        auto args = agentxx::util::Json{
            {"file_patterns", agentxx::util::Json::array({limitDir + "/**/*"})},
            {"text_patterns", agentxx::util::Json::array({"no_such_token"})   },
            {"max_files",     10                                              },
        };
        auto result = ::agentxx_fs_plugin::fileGrepExecute(args, workDir);
        bool ok     = result.find("[Error]") != std::string::npos
                  && result.find("max_files") != std::string::npos;
        if (false == ok) {
            allPassed = false;
            TEST_FAIL << "FilesystemGrepTool no-match error should mention the `max_files` cutoff, "
                         "got: "
                      << result.substr(0, 200) << std::endl;
        } else {
            TEST_PASS << "FilesystemGrepTool no-match error mentions the `max_files` cutoff"
                      << std::endl;
        }
    }

    // ⑦ grep max_file_size_mb: 大文件跳过并给出 [Note] (不做静默丢弃)
    {
        auto args = agentxx::util::Json{
            {"file_patterns",    agentxx::util::Json::array({limitDir + "/**/*"})        },
            {"text_patterns",    agentxx::util::Json::array({"big_token", "small_token"})},
            {"max_file_size_mb", 0.001                                                   },
        };
        auto result       = ::agentxx_fs_plugin::fileGrepExecute(args, workDir);
        bool skippedBig   = result.find("big.txt") == std::string::npos;
        bool matchedSmall = result.find("small0.txt") != std::string::npos;
        bool noted        = result.find("[Note]") != std::string::npos
                     && result.find("max_file_size_mb") != std::string::npos;
        if (false == (skippedBig && matchedSmall && noted)) {
            allPassed = false;
            TEST_FAIL << "FilesystemGrepTool should skip oversized files with a note, got: "
                      << result << std::endl;
        } else {
            TEST_PASS << "FilesystemGrepTool skips oversized file and reports a note" << std::endl;
        }
    }

    // ⑧ list max_files 截断 + 首行提示
    {
        auto tool = agentxx::tools::FileSystemListTool{agentContext};
        auto args = agentxx::util::Json{
            {"path",      limitDir + "/**/*"},
            {"max_files", 10                },
        };
        auto result = co_await tool.execute_async(args);
        if (result.find("[Note]") == std::string::npos
            || result.find("matched entries are listed") == std::string::npos) {
            allPassed = false;
            TEST_FAIL << "FileSystemListTool should truncate expansion with a note, got: " << result
                      << std::endl;
        } else {
            TEST_PASS << "FileSystemListTool truncates expansion over `max_files` with a note"
                      << std::endl;
        }
    }

    // ⑨ glob 以 `**` 结尾 (递归展开分支, 计数发生在递归遍历内部): 同样到限即停
    {
        auto argsUnlimited = agentxx::util::Json{
            {"file_patterns", agentxx::util::Json::array({limitDir + "/**"})},
            {"max_files",     0                                             },
        };
        auto full      = ::agentxx_fs_plugin::fileGlobExecute(argsUnlimited, workDir);
        auto fullCount = countPathLines(full); // 目录自身 + 31 个文件

        auto argsLimited = agentxx::util::Json{
            {"file_patterns", agentxx::util::Json::array({limitDir + "/**"})},
            {"max_files",     10                                            },
        };
        auto limited    = ::agentxx_fs_plugin::fileGlobExecute(argsLimited, workDir);
        auto limitCount = countPathLines(limited);
        if (fullCount == 0 || limitCount != 10 || false == startsWithNote(limited)
            || limited.find("[Error]") != std::string::npos) {
            allPassed = false;
            TEST_FAIL << "FilesystemGlobTool should stop recursive `**` walk at `max_files` ("
                      << limitCount << " of " << fullCount << "), got: " << limited.substr(0, 200)
                      << std::endl;
        } else {
            TEST_PASS << "FilesystemGlobTool stops recursive `**` walk at `max_files`" << std::endl;
        }
    }

    // ⑩ 多 pattern 共享同一数量上限: 前一个 pattern 到限后不再展开后续 pattern
    {
        auto args = agentxx::util::Json{
            {"file_patterns", agentxx::util::Json::array({limitDir + "/**/*", limitDir + "/*.txt"})
            },
            {"max_files",     10                                                                   },
        };
        auto result    = ::agentxx_fs_plugin::fileGlobExecute(args, workDir);
        auto pathCount = countPathLines(result);
        if (pathCount != 10 || false == startsWithNote(result)) {
            allPassed = false;
            TEST_FAIL << "FilesystemGlobTool should share `max_files` across patterns, got "
                      << pathCount << " lines: " << result.substr(0, 200) << std::endl;
        } else {
            TEST_PASS << "FilesystemGlobTool shares `max_files` across patterns" << std::endl;
        }
    }

    // ⑪ grep content 模式到限: 首行提示 + 已收集文件的匹配行照常输出
    {
        auto args = agentxx::util::Json{
            {"file_patterns", agentxx::util::Json::array({limitDir + "/**/*"})},
            {"text_patterns", agentxx::util::Json::array({"small_token"})     },
            {"output_mode",   "content"                                       },
            {"max_files",     5                                               },
        };
        auto result = ::agentxx_fs_plugin::fileGrepExecute(args, workDir);
        bool ok = startsWithNote(result) && result.find("small_token line") != std::string::npos;
        if (false == ok) {
            allPassed = false;
            TEST_FAIL
                << "FilesystemGrepTool content mode should search collected files with a note, got: "
                << result.substr(0, 200) << std::endl;
        } else {
            TEST_PASS << "FilesystemGrepTool content mode searches collected files with a note"
                      << std::endl;
        }
    }

    if (allPassed) {
        g_fs_passed++;
    } else {
        g_fs_failed++;
    }

    std::error_code ec;
    fs::remove_all(agentxx::util::utf8ToPath(limitDir), ec);
    co_return;
}

/// 回归测试: exclude_patterns 在遍历层生效 (数量上限也在遍历中拦截)
/// 构造: src/ 2 个匹配文件; build/ 1200 个文件 (超过 max_files = 1000)
/// - ① glob / ② grep: `exclude_patterns = …/build/**` (覆盖整棵子树) 时遍历直接剪掉
///   build 子树, 不把其 1200 条计入数量上限 → 正常返回 src 的匹配;
///   若排除与计数都只在遍历结果上做, 这里会先因 1201 > 1000 报 "Too many" 而失败
/// - ③ 只匹配目录自身的模式 (`…/build`, 不含非首段 `**`) 不剪枝: 目录条目本身被排除,
///   但其下文件仍被遍历与匹配 (与"匹配到的路径被排除"语义一致)
asio::awaitable<void>
    test_walk_exclude_pruning(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    namespace fs               = std::filesystem;
    const std::string baseDir  = testDir + "/walk_exclude";
    const std::string buildDir = baseDir + "/build";
    const std::string srcDir   = baseDir + "/src";
    if (fs::exists(baseDir)) {
        fs::remove_all(baseDir);
    }
    fs::create_directories(srcDir);
    fs::create_directories(buildDir);
    for (int i = 0; i < 2; ++i) {
        std::ofstream f(srcDir + "/s" + std::to_string(i) + ".txt");
        f << "prune_token from src\n";
    }
    for (int i = 0; i < 1200; ++i) {
        std::ofstream f(buildDir + "/b" + std::to_string(i) + ".txt");
        f << "prune_token from build\n";
    }

    const auto workDir   = agentxx::tools::testResolvedWorkDir(agentContext);
    bool       allPassed = true;

    // ① glob: 覆盖子树的排除模式在遍历中剪枝, 不计入数量上限
    {
        auto args = agentxx::util::Json{
            {"file_patterns",    agentxx::util::Json::array({baseDir + "/**/*"})},
            {"exclude_patterns", agentxx::util::Json::array({buildDir + "/**"}) },
            {"max_files",        1000                                           },
        };
        auto result = ::agentxx_fs_plugin::fileGlobExecute(args, workDir);
        bool ok     = result.find("s0.txt") != std::string::npos
                  && result.find("s1.txt") != std::string::npos
                  && result.find("Too many") == std::string::npos
                  && result.find("/build/") == std::string::npos;
        if (ok) {
            TEST_PASS << "FilesystemGlobTool prunes excluded subtree during walk" << std::endl;
        } else {
            allPassed = false;
            TEST_FAIL << "FilesystemGlobTool should prune excluded subtree, got: "
                      << result.substr(0, 400) << std::endl;
        }
    }

    // ② grep: 同上 (排除的 1200 个文件既不读也不计入数量上限)
    {
        auto args = agentxx::util::Json{
            {"file_patterns",    agentxx::util::Json::array({baseDir + "/**/*"})},
            {"text_patterns",    agentxx::util::Json::array({"prune_token"})    },
            {"exclude_patterns", agentxx::util::Json::array({buildDir + "/**"}) },
            {"max_files",        1000                                           },
        };
        auto result = ::agentxx_fs_plugin::fileGrepExecute(args, workDir);
        bool ok     = result.find("s0.txt") != std::string::npos
                  && result.find("Too many") == std::string::npos
                  && result.find("/build/") == std::string::npos;
        if (ok) {
            TEST_PASS << "FilesystemGrepTool prunes excluded subtree during walk" << std::endl;
        } else {
            allPassed = false;
            TEST_FAIL << "FilesystemGrepTool should prune excluded subtree, got: "
                      << result.substr(0, 400) << std::endl;
        }
    }

    // ③ 只匹配目录自身的模式不剪枝: build 目录条目被排除, 其下文件仍被搜索
    {
        auto args = agentxx::util::Json{
            {"file_patterns",    agentxx::util::Json::array({baseDir + "/**/*"})},
            {"text_patterns",    agentxx::util::Json::array({"prune_token"})    },
            {"exclude_patterns", agentxx::util::Json::array({buildDir})         },
            {"max_files",        2000                                           },
        };
        auto result = ::agentxx_fs_plugin::fileGrepExecute(args, workDir);
        bool ok     = result.find("b0.txt") != std::string::npos // 子树仍被遍历
                  && result.find("build:") == std::string::npos; // 目录条目本身被排除
        if (ok) {
            TEST_PASS << "FilesystemGrepTool keeps traversing dir excluded by itself-only pattern"
                      << std::endl;
        } else {
            allPassed = false;
            TEST_FAIL
                << "FilesystemGrepTool should keep traversing dir for itself-only pattern, got: "
                << result.substr(0, 400) << std::endl;
        }
    }

    if (allPassed) {
        g_fs_passed++;
    } else {
        g_fs_failed++;
    }

    std::error_code ec;
    fs::remove_all(agentxx::util::utf8ToPath(baseDir), ec);
    co_return;
}

asio::awaitable<TestResult>
    run_filesystem_tools_tests(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    setupTestDir();
    auto run = [agentContext](auto testFn) -> asio::awaitable<void> {
        try {
            co_await testFn(agentContext);
        } catch (const std::exception& e) {
            g_fs_failed++;
            TEST_FAIL << "Exception in test: " << e.what() << std::endl;
        }
    };

    co_await run(test_list_file_get_definition);
    co_await run(test_list_file_empty_path);
    co_await run(test_list_file_basic);
    co_await run(test_list_file_recursive);
    co_await run(test_list_file_limit);
    co_await run(test_list_file_info_fields);
    co_await run(test_list_file_relative_path);
    co_await run(test_resolved_workdir);
    co_await run(test_session_workdir_priority);
    co_await run(test_list_relative_path_with_workdir);
    co_await run(test_read_relative_with_workdir);
    co_await run(test_glob_relative_pattern_with_workdir);
    co_await run(test_list_file_tilde_path);
    co_await run(test_list_glob_star_pattern);
    co_await run(test_list_glob_question_mark);
    co_await run(test_list_glob_recursive_segment);
    co_await run(test_list_glob_dir_recursive);
    co_await run(test_list_glob_relative_with_workdir);
    co_await run(test_list_glob_no_match);
    co_await run(test_list_glob_case_sensitive);
    co_await run(test_list_glob_limit);
    co_await run(test_list_glob_literal_fallback);

    co_await run(test_read_text_file_get_definition);
    co_await run(test_read_text_file_empty_path);
    co_await run(test_read_text_file_not_exist);
    co_await run(test_read_text_file_full);
    co_await run(test_read_text_file_offset);
    co_await run(test_read_text_file_relative_path);
    co_await run(test_read_text_file_limit);
    co_await run(test_read_text_file_offset_and_limit);

    co_await run(test_write_file_get_definition);
    co_await run(test_write_file_empty_path);
    co_await run(test_write_file_create);
    co_await run(test_write_file_no_overwrite_existing);
    co_await run(test_write_file_overwrite);

    co_await run(test_edit_text_file_get_definition);
    co_await run(test_edit_text_file_empty_path);
    co_await run(test_edit_text_file_empty_old_str);
    co_await run(test_edit_text_file_single_replace);
    co_await run(test_edit_text_file_multi_replace);
    co_await run(test_edit_text_file_no_match);
    co_await run(test_read_text_file_crlf);
    co_await run(test_edit_text_file_crlf_file);
    co_await run(test_edit_text_file_lf_file_crlf_old);
    co_await run(test_edit_text_file_crlf_multi_replace);

    co_await run(test_glob_get_definition);
    co_await run(test_glob_empty_patterns);
    co_await run(test_glob_find_files);
    co_await run(test_glob_recursive);
    co_await run(test_glob_relative_pattern);
    co_await run(test_glob_non_recursive);
    co_await run(test_glob_type_filter);
    co_await run(test_glob_exclude_patterns);
    co_await run(test_glob_case_sensitive);
    co_await run(test_glob_max_depth);

    co_await run(test_grep_get_definition);
    co_await run(test_grep_empty_text_patterns);
    co_await run(test_grep_empty_file_patterns);
    co_await run(test_grep_text_search);
    co_await run(test_grep_regex_search);
    co_await run(test_grep_text_and_regex_union);
    co_await run(test_grep_text_and_regex_union_content);
    co_await run(test_grep_content_mode);
    co_await run(test_grep_content_grouped_multi_files);
    co_await run(test_grep_case_insensitive);
    co_await run(test_grep_case_sensitive_default);
    co_await run(test_grep_max_count_per_file);
    co_await run(test_grep_context_lines);
    co_await run(test_grep_no_match_fail_fast);
    co_await run(test_grep_skip_directories);
    co_await run(test_grep_mem_stress);
    co_await run(test_scan_scale_limits);
    co_await run(test_walk_exclude_pruning);
    co_await run(test_glob_unicode_paths);
    co_await run(test_grep_unicode_path_and_content);
    co_await run(test_grep_multi_pattern_fault_tolerance);

    co_await run(test_list_file_chinese_path);
    co_await run(test_read_text_file_chinese_path);
    co_await run(test_write_file_chinese_path);
    co_await run(test_edit_text_file_chinese_path);
    co_await run(test_glob_chinese_paths);
    co_await run(test_grep_chinese_path_and_content);

    // 同步兜底路径 (强制关闭文件异步 I/O 后 read/write/edit 走同步实现)
    co_await run(test_sync_fallback_without_async_file_io);

    // 插件真实链路冒烟 (dlopen + 宿主 op_driver 全链路; 插件未构建时跳过)
    // - 无 agentContext 形参, 不经 run 适配器直调 (异常兜底语义一致)
    try {
        co_await test_plugin_real_link();
    } catch (const std::exception& e) {
        g_fs_failed++;
        TEST_FAIL << "Exception in test_plugin_real_link: " << e.what() << std::endl;
    }

    cleanupTestDir();
    co_return TestResult{g_fs_passed, g_fs_failed};
}

} // namespace test
} // namespace agentxx
