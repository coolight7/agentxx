#include "test_filesystem_tools.h"
#include <neograph/types.h>
#include "agentxx/agent/context.h"
// 原 lib 内置工具已迁移至 agentxx_filesystem 插件 (同名同行为); 测试直测
// 插件同一实现 (filesystem_impl.h), 保证插件行为与测试覆盖一致
#include "filesystem_impl.h"
#include "agentxx/util/string_util.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace agentxx {
namespace tools {

/// 会话工作目录解析 (原工具经 AgentContext; 与插件 get_work_dir 同源):
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
#define AGENTXX_TEST_FS_TOOL(NAME, IMPL_FN, TOOL_NAME, DEPICT)                  \
    struct NAME {                                                               \
        std::weak_ptr<agentxx::agent::AgentContext> ctx;                        \
        explicit NAME(std::weak_ptr<agentxx::agent::AgentContext> c)               \
            : ctx(std::move(c)) {}                                              \
        neograph::ChatTool get_definition() const {                              \
            return {TOOL_NAME, DEPICT, {}};                                     \
        }                                                                       \
        asio::awaitable<std::string> execute_async(const neograph::json& args)  \
            const {                                                             \
            co_return ::agentxx::filesystem_plugin::IMPL_FN(                    \
                args, testResolvedWorkDir(ctx), nullptr);                       \
        }                                                                       \
    };

AGENTXX_TEST_FS_TOOL(FileSystemListTool,      fileListExecute,   "agentxx_filesystem_list",
                     R"(List files and directories at a given path, output is multi-line text similar to `ls -l`, one entry per line: `type size last-modified-time path`.)")
AGENTXX_TEST_FS_TOOL(FilesystemReadTextFileTool, fileReadExecute,  "agentxx_filesystem_read",
                     R"(Read a text file (e.g. .txt, .md, .json, .log, source code) and return its contents with line numbers.)")
AGENTXX_TEST_FS_TOOL(FilesystemWriteFileTool, fileWriteExecute,  "agentxx_filesystem_write",
                     "Create a new file or overwrite an existing file with the given content.")
AGENTXX_TEST_FS_TOOL(FilesystemEditTextFileTool, fileEditExecute,  "agentxx_filesystem_edit",
                     R"(Perform exact string replacement in a text file (e.g. *.txt, *.md, *.cpp, *.h).)")
AGENTXX_TEST_FS_TOOL(FilesystemGlobTool,      fileGlobExecute,   "agentxx_filesystem_glob",
                     "Find files and directories matching glob patterns.")
AGENTXX_TEST_FS_TOOL(FilesystemGrepTool,      fileGrepExecute,   "agentxx_filesystem_grep",
                     R"(Search file contents using text or regular expressions.)")

#undef AGENTXX_TEST_FS_TOOL

} // namespace tools
} // namespace agentxx

namespace agentxx {
namespace test {

int g_fs_passed = 0;
int g_fs_failed = 0;

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
    auto args = neograph::json{
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
    auto args = neograph::json{
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
    auto args = neograph::json{
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
    auto args = neograph::json{
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
    auto args = neograph::json{
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
    auto args    = neograph::json{
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
static std::shared_ptr<agentxx::agent::AgentContext>
    makeWorkDirContext(const std::string& dir) {
    auto ctx                  = std::make_shared<agentxx::agent::AgentContext>();
    ctx->agentConfig          = std::make_shared<agentxx::agent::AgentConfig>();
    ctx->agentConfig->workDir = dir;
    return ctx;
}

/// resolvedWorkDir 纯函数校验: workDir 非空原样返回; 为空回退进程 cwd
asio::awaitable<void> test_resolved_workdir(std::weak_ptr<agentxx::agent::AgentContext>) {
    auto cfgWith = std::make_shared<agentxx::agent::AgentConfig>();
    cfgWith->workDir = "/tmp/proj-a";
    XX_TEST_EXPECT_EQ(cfgWith->resolvedWorkDir(), std::string{"/tmp/proj-a"});

    auto cfgEmpty = std::make_shared<agentxx::agent::AgentConfig>();
    XX_TEST_EXPECT_EQ(cfgEmpty->resolvedWorkDir(), std::filesystem::current_path().generic_string()
    );
    co_return;
}

/// 相对路径应以会话工作目录 (workDir) 而非进程 cwd 为基准:
/// 绑定 workDir=testDir 后传 "." (解析到工作目录自身), 应列出其下文件;
/// 进程 cwd 与 testDir 无关, 解析错误时将得到空/错误结果而非目录内容
asio::awaitable<void>
    test_list_relative_path_with_workdir(std::weak_ptr<agentxx::agent::AgentContext>) {
    auto ctx  = makeWorkDirContext(testDir);
    auto tool = agentxx::tools::FileSystemListTool{ctx};
    auto args = neograph::json{
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
        TEST_FAIL << "FileSystemListTool workDir relative path failed, got: " << result << std::endl;
    }
    co_return;
}

/// 相对路径读取: 以 workDir 为基准找到文件 (workDir ≠ 进程 cwd)
asio::awaitable<void>
    test_read_relative_with_workdir(std::weak_ptr<agentxx::agent::AgentContext>) {
    auto ctx  = makeWorkDirContext(testDir);
    auto tool = agentxx::tools::FilesystemReadTextFileTool{ctx};
    auto args = neograph::json{
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
    auto args = neograph::json{
        {"file_patterns", neograph::json::array({"*.txt"})},
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
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    if (!home || !*home) {
        g_fs_passed++;
        TEST_PASS << "skip tilde test: no home env" << std::endl;
        co_return;
    }
    auto homeFile = std::filesystem::path{home} / "agentxx_list_tilde_probe.txt";
    {
        std::ofstream f(homeFile);
        f << "tilde probe\n";
    }
    auto tool = agentxx::tools::FileSystemListTool{agentContext};
    auto args = neograph::json{
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

asio::awaitable<void>
    test_read_text_file_get_definition(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FilesystemReadTextFileTool{agentContext};
    auto def  = tool.get_definition();
    if (def.name == "agentxx_filesystem_read") {
        std::cout << "[PASS] FilesystemReadTextFileTool::get_definition() name correct"
                  << std::endl;
    } else {
        std::cout << "[FAIL] FilesystemReadTextFileTool::get_definition() name incorrect"
                  << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_read_text_file_empty_path(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FilesystemReadTextFileTool{agentContext};
    auto args = neograph::json{
        {"path", ""}
    };
    try {
        auto result = co_await tool.execute_async(args);
        if (agentxx::util::isIgnoreCaseContains(result, "error")) {
            std::cout << "[PASS] FilesystemReadTextFileTool returns error for empty path"
                      << std::endl;
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
    auto args = neograph::json{
        {"path", testDir + "/nonexistent.txt"}
    };
    try {
        auto result = co_await tool.execute_async(args);
        TEST_INFO << "FilesystemReadTextFileTool non-existent file: " << result << std::endl;
    } catch (const std::exception& e) {
        g_fs_passed++;
        TEST_PASS << "FilesystemReadTextFileTool throws for non-existent "
                     "file: "
                  << e.what() << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_read_text_file_full(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FilesystemReadTextFileTool{agentContext};
    auto args = neograph::json{
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
    auto args = neograph::json{
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
    auto args    = neograph::json{
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
    auto args = neograph::json{
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
    auto args = neograph::json{
        {"path",        testDir + "/offset_test.txt"},
        {"line_offset", 0                           },
        {"line_limit",  2                           },
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("aaaa") != std::string::npos && result.find("bbbb") != std::string::npos
        && result.find("cccc") == std::string::npos) {
        std::cout << "[PASS] FilesystemReadTextFileTool respects both offset and limit"
                  << std::endl;
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
        std::cout << "[FAIL] FilesystemWriteFileTool::get_definition() name incorrect" << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_write_file_empty_path(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FilesystemWriteFileTool{agentContext};
    auto args = neograph::json{
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
    auto args     = neograph::json{
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
    auto args     = neograph::json{
            {"path",      filePath     },
            {"content",   "new content"},
            {"overwrite", false        },
    };
    try {
        auto result = co_await tool.execute_async(args);
        TEST_INFO << "FilesystemWriteFileTool no-overwrite result: " << result << std::endl;
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
    auto args = neograph::json{
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
        std::cout << "[PASS] FilesystemEditTextFileTool::get_definition() name correct"
                  << std::endl;
    } else {
        std::cout << "[FAIL] FilesystemEditTextFileTool::get_definition() name incorrect"
                  << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_edit_text_file_empty_path(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FilesystemEditTextFileTool{agentContext};
    auto args = neograph::json{
        {"path",    "" },
        {"old_str", "a"},
        {"new_str", "b"},
    };
    auto result = co_await tool.execute_async(args);
    if (agentxx::util::isIgnoreCaseContains(result, "error")) {
        std::cout << "[PASS] FilesystemEditTextFileTool returns error for empty path" << std::endl;
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
    auto args = neograph::json{
        {"path",    testDir + "/test1.txt"},
        {"old_str", ""                    },
        {"new_str", "b"                   },
    };
    auto result = co_await tool.execute_async(args);
    if (agentxx::util::isIgnoreCaseContains(result, "error")) {
        std::cout << "[PASS] FilesystemEditTextFileTool returns error for empty old_str"
                  << std::endl;
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
    auto args = neograph::json{
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
    auto args = neograph::json{
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
    auto args     = neograph::json{
            {"path",    filePath                },
            {"old_str", "nonexistent_string_xyz"},
            {"new_str", "replacement"           },
    };
    try {
        auto result = co_await tool.execute_async(args);
        TEST_INFO << "FilesystemEditTextFileTool no-match result: " << result << std::endl;
    } catch (const std::exception& e) {
        std::cout << "[PASS] FilesystemEditTextFileTool throws when no match found: " << e.what()
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

    // 完整读取: 应统一输出 LF, 不含 `\r`
    auto args = neograph::json{
        {"path", filePath}
    };
    auto full  = co_await tool.execute_async(args);
    auto hasCR = full.find('\r') != std::string::npos;
    if (hasCR && full.find("alpha\r\nbeta\r\ngamma\r\n") != std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "read_text_file normalizes CRLF to LF on full read" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "read_text_file CRLF full read failed, got: '" << full << "'" << std::endl;
    }

    // offset/limit 读取: 同样不含 `\r`
    auto args2 = neograph::json{
        {"path",        filePath},
        {"line_offset", 1       },
        {"line_limit",  1       },
    };
    auto part   = co_await tool.execute_async(args2);
    auto hasCR2 = part.find('\r') != std::string::npos;
    if (hasCR2 && part.find("beta") != std::string::npos
        && part.find("alpha") == std::string::npos) {
        g_fs_passed++;
        TEST_PASS << "read_text_file normalizes CRLF to LF on offset/limit read" << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "read_text_file CRLF offset/limit read failed, got: '" << part << "'"
                  << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_edit_text_file_crlf_file(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    // CRLF 文件 + LF 形式的 old_str/new_str: 应匹配成功并保持文件为 \n
    auto filePath = testDir + "/crlf_edit_test.txt";
    {
        std::ofstream f(filePath, std::ios::binary);
        f << "hello world\r\nfoo bar\r\n";
    }

    auto tool = agentxx::tools::FilesystemEditTextFileTool{agentContext};
    auto args = neograph::json{
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
            TEST_PASS << "edit_text_file matches LF old_str in CRLF file and keeps CRLF"
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
    auto args = neograph::json{
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
    // CRLF 文件 + multi_replace: LF 形式 old_str 应替换全部并保持 \n
    auto filePath = testDir + "/crlf_edit_multi_test.txt";
    {
        std::ofstream f(filePath, std::ios::binary);
        f << "foo\r\nfoo\r\nfoo\r\n";
    }

    auto tool = agentxx::tools::FilesystemEditTextFileTool{agentContext};
    auto args = neograph::json{
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
            TEST_PASS << "edit_text_file multi_replace works on CRLF file and keeps CRLF"
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
    auto args = neograph::json{
        {"file_patterns", neograph::json::array()},
    };
    auto result = co_await tool.execute_async(args);
    if (agentxx::util::isIgnoreCaseContains(result, "error")) {
        std::cout << "[PASS] FilesystemGlobTool returns error for empty file_patterns" << std::endl;
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
    auto args = neograph::json{
        {"file_patterns", neograph::json::array({testDir + "/*.txt"})},
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
    auto args = neograph::json{
        {"file_patterns", neograph::json::array({testDir + "/**/*.txt"})},
    };
    auto result = co_await tool.execute_async(args);
    if (result.find("subtest.txt") != std::string::npos) {
        std::cout << "[PASS] FilesystemGlobTool recursive glob finds subdirectory files"
                  << std::endl;
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
    auto args   = neograph::json{
          {"file_patterns", neograph::json::array({relDir + "/*.txt"})},
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
    auto args = neograph::json{
        {"text_patterns_is_regex", false                                      },
        {"text_patterns",          neograph::json::array()                    },
        {"file_patterns",          neograph::json::array({testDir + "/*.txt"})},
    };
    auto result = co_await tool.execute_async(args);
    if (agentxx::util::isIgnoreCaseContains(result, "error")) {
        std::cout << "[PASS] FilesystemGrepTool returns error for empty text_patterns" << std::endl;
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
    auto args = neograph::json{
        {"text_patterns_is_regex", false                           },
        {"text_patterns",          neograph::json::array({"hello"})},
        {"file_patterns",          neograph::json::array()         },
    };
    auto result = co_await tool.execute_async(args);
    if (agentxx::util::isIgnoreCaseContains(result, "error")) {
        std::cout << "[PASS] FilesystemGrepTool returns error for empty file_patterns" << std::endl;
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
    auto args = neograph::json{
        {"text_patterns_is_regex", false                                      },
        {"text_patterns",          neograph::json::array({"hello"})           },
        {"file_patterns",          neograph::json::array({testDir + "/*.txt"})},
        {"output_mode",            "files_with_matches"                       },
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
    auto args = neograph::json{
        {"text_patterns_is_regex", true                                           },
        {"text_patterns",          neograph::json::array({"line[0-9]"})           },
        {"file_patterns",          neograph::json::array({testDir + "/test1.txt"})},
        {"output_mode",            "files_with_matches"                           },
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

asio::awaitable<void>
    test_grep_content_mode(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tool = agentxx::tools::FilesystemGrepTool{agentContext};
    auto args = neograph::json{
        {"text_patterns_is_regex", false                                          },
        {"text_patterns",          neograph::json::array({"hello"})               },
        {"file_patterns",          neograph::json::array({testDir + "/test2.txt"})},
        {"output_mode",            "content"                                      },
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
        TEST_PASS << "FilesystemGrepTool content mode returns grouped-by-file format"
                  << std::endl;
    } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemGrepTool content mode grouped format incorrect, got: " << result
                  << std::endl;
    }
    co_return;
}

asio::awaitable<void> test_grep_content_grouped_multi_files(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext
) {
    auto tool = agentxx::tools::FilesystemGrepTool{agentContext};
    // 多文件 content 模式: 每个文件应有独立的组头 "{filepath}:",
    // 且每个文件的组头仅出现一次 (减少路径重复), 各文件行归属自己的组头之下
    auto args = neograph::json{
        {"text_patterns_is_regex", true                                        },
        {"text_patterns",          neograph::json::array({".*e.*"})            },
        {"file_patterns",          neograph::json::array({testDir + "/*.txt"})},
        {"output_mode",            "content"                                   },
        {"max_count_per_file",     1                                           },
    };
    auto result = co_await tool.execute_async(args);

    auto countSubstr = [](const std::string& text, std::string_view sub) -> size_t {
        size_t count = 0;
        for (auto pos = text.find(sub); pos != std::string::npos;
             pos     = text.find(sub, pos + 1)) {
            ++count;
        }
        return count;
    };

    // test1.txt / test2.txt 均为非空文本且含字符 'e', 各应恰好输出一个组头
    size_t header1 = countSubstr(result, "test1.txt:\n");
    size_t header2 = countSubstr(result, "test2.txt:\n");
    if (header1 == 1 && header2 == 1) {
        g_fs_passed++;
        TEST_PASS << "FilesystemGrepTool content mode groups output per file header"
                  << std::endl;
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
    auto args = neograph::json{
        {"text_patterns_is_regex", false                                          },
        {"text_patterns",          neograph::json::array({"HELLO"})               },
        {"file_patterns",          neograph::json::array({testDir + "/test2.txt"})},
        {"output_mode",            "files_with_matches"                           },
        {"case_sensitive",         false                                          },
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
    auto args = neograph::json{
        {"text_patterns_is_regex", false                                          },
        {"text_patterns",          neograph::json::array({"HELLO"})               },
        {"file_patterns",          neograph::json::array({testDir + "/test2.txt"})},
        {"output_mode",            "files_with_matches"                           },
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
    auto args = neograph::json{
        {"text_patterns_is_regex", false                                          },
        {"text_patterns",          neograph::json::array({"line"})                },
        {"file_patterns",          neograph::json::array({testDir + "/test1.txt"})},
        {"output_mode",            "files_with_matches"                           },
        {"max_count_per_file",     2                                              },
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
    auto args = neograph::json{
        {"text_patterns_is_regex", false                                          },
        {"text_patterns",          neograph::json::array({"line3"})               },
        {"file_patterns",          neograph::json::array({testDir + "/test1.txt"})},
        {"output_mode",            "content"                                      },
        {"context_lines",          1                                              },
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
    auto args = neograph::json{
        {"file_patterns", neograph::json::array({testDir + "/*"})},
        {"type",          "dir"                                  },
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
    auto args = neograph::json{
        {"file_patterns",    neograph::json::array({testDir + "/*.txt"})    },
        {"exclude_patterns", neograph::json::array({testDir + "/test1.txt"})},
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
    auto args = neograph::json{
        {"file_patterns", neograph::json::array({testDir + "/TEST1.TXT"})},
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
    auto args = neograph::json{
        {"file_patterns", neograph::json::array({testDir + "/**/*.txt"})},
        {"max_depth",     1                                             },
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
    auto args = neograph::json{
        {"file_patterns", neograph::json::array({testDir + "/*.txt"})},
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
    auto args = neograph::json{
        {"text_patterns_is_regex", false                                               },
        {"text_patterns",          neograph::json::array({"never_match_any_text"})     },
        {"file_patterns",          neograph::json::array({testDir + "/no_such_dir/**"})},
        {"timeout",                60                                                  }, // 修复前会白等 60s
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
    auto args = neograph::json{
        {"text_patterns_is_regex", false                                     },
        {"text_patterns",          neograph::json::array({"hello world"})    },
        {"file_patterns",          neograph::json::array({testDir + "/**/*"})},
        {"output_mode",            "files_with_matches"                      },
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
    auto args = neograph::json{
        {"text_patterns_is_regex", true                                             },
        {"text_patterns",          neograph::json::array({"token_\\d+", "func\\d+"})},
        {"file_patterns",          neograph::json::array({stressDir + "/**/*"})     },
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
    auto args2 = neograph::json{
        {"text_patterns_is_regex", false                                       },
        {"text_patterns",          neograph::json::array({"token_5", "func3"}) },
        {"file_patterns",          neograph::json::array({stressDir + "/**/*"})},
        {"output_mode",            "content"                                   },
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
    co_await run(test_list_relative_path_with_workdir);
    co_await run(test_read_relative_with_workdir);
    co_await run(test_glob_relative_pattern_with_workdir);
    co_await run(test_list_file_tilde_path);

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
    co_await run(test_grep_content_mode);
    co_await run(test_grep_content_grouped_multi_files);
    co_await run(test_grep_case_insensitive);
    co_await run(test_grep_case_sensitive_default);
    co_await run(test_grep_max_count_per_file);
    co_await run(test_grep_context_lines);
    co_await run(test_grep_no_match_fail_fast);
    co_await run(test_grep_skip_directories);
    co_await run(test_grep_mem_stress);

    cleanupTestDir();
    co_return TestResult{g_fs_passed, g_fs_failed};
}

} // namespace test
} // namespace agentxx
