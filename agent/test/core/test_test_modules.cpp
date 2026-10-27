///
/// 背景: `agent/test/test.cpp` 是"有哪些测试模块"的唯一事实来源, 但设计文档
/// (`docs/zh-cn/design/index.md`) 里那份手写清单曾经落后几十个模块 (还自称与
/// test.cpp 一致), 让读者找不到对应用例。这里沿用项目里成熟的做法
/// (`config_keys` / `wire_schema` 同款): 从注册调用生成清单文档, 再逐字节比对。
///
/// - 生成物: `docs/zh-cn/design/test-modules.md`
/// - 更新: `AGENTXX_UPDATE_TEST_MODULES=1` 重建 (更新后人工 review diff)
/// - 生成规则完全来自 `test.cpp` 文本:
///   - `runSync("名字", ...)` → 同步模块
///   - `run("名字", ...)` / `runCtx(\n "名字", ...)` → 异步模块
///   - 落在 `#ifdef AGENTXX_BUILD_CLIENT` 段内的 → 仅 client 构建包含
#include "agentxx-test/core/test_test_modules.h"

#include "agentxx-test/core/schema_artifact.h"
#include "utilxx_base/string_util.h"
#include <algorithm>
#include <filesystem>
#include <fmt/format.h>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见)
int g_tm_passed = 0;
int g_tm_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_tm_passed
#define XX_TEST_FAILED g_tm_failed

namespace agentxx {
namespace test {

namespace {

/// 一个注册到的测试模块
struct ModuleEntry {
    std::string name;
    /// 调度方式文本 (`同步` / `异步`)
    std::string mode;
    /// 是否只在不带 client 的构建里也有 (`通用` / `仅 client`)
    std::string build;
};

/// 从一行文本里取第一个双引号字符串; 没有返回空
std::string firstQuoted(std::string_view line) {
    const auto begin = line.find('"');
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = line.find('"', begin + 1);
    if (end == std::string_view::npos) {
        return {};
    }
    return std::string{line.substr(begin + 1, end - begin - 1)};
}

/// 解析 `test.cpp` 的注册调用表
/// - 支持 `runCtx(` 换行写参数: 名字在后续第一行里
std::vector<ModuleEntry> parseModules(const std::string& text) {
    std::vector<ModuleEntry> out;
    int                      clientDepth = 0; // `#ifdef AGENTXX_BUILD_CLIENT` 嵌套层数

    std::vector<std::string> lines;
    {
        std::string cur;
        for (char ch : text) {
            if (ch == '\n') {
                lines.push_back(cur);
                cur.clear();
            } else if (ch != '\r') {
                cur.push_back(ch);
            }
        }
        if (!cur.empty()) {
            lines.push_back(std::move(cur));
        }
    }

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string_view line{lines[i]};
        if (line.find("#ifdef AGENTXX_BUILD_CLIENT") != std::string_view::npos) {
            ++clientDepth;
            continue;
        }
        if (clientDepth > 0 && line.find("#endif") != std::string_view::npos) {
            --clientDepth;
            continue;
        }
        std::string mode;
        if (line.find("runSync(") != std::string_view::npos) {
            mode = "同步";
        } else if (line.find("runCtx(") != std::string_view::npos
                   || line.find("run(\"") != std::string_view::npos) {
            mode = "异步";
        } else {
            continue;
        }
        auto name = firstQuoted(line);
        if (name.empty()) {
            // 名字写到了下一行 (多行参数写法)
            for (size_t k = i + 1; k < lines.size() && k <= i + 2; ++k) {
                name = firstQuoted(lines[k]);
                if (!name.empty()) {
                    break;
                }
            }
        }
        if (name.empty()) {
            continue;
        }
        out.push_back(ModuleEntry{
            .name  = std::move(name),
            .mode  = std::move(mode),
            .build = clientDepth > 0 ? "仅 client" : "通用",
        });
    }
    return out;
}

/// 生成清单文档文本 (逐字节比较对象)
std::string buildModulesDoc(const std::vector<ModuleEntry>& modules) {
    std::string out;
    out += "# 测试模块清单 (生成物)\n\n";
    out += "> 由 `agent/test/test.cpp` 的注册调用生成, 请勿手工编辑。\n";
    out += "> 重新生成: `AGENTXX_UPDATE_TEST_MODULES=1 path/to/agentxx_test test_modules`\n\n";
    out += "运行方式:\n\n";
    out += "```bash\n";
    out += "path/to/agentxx_test                    # 跑全部模块\n";
    out += "path/to/agentxx_test --fail-fast        # 遇到失败立即终止\n";
    out += "path/to/agentxx_test string_util regex  # 只跑指定模块\n";
    out += "```\n\n";
    out += fmt::format("模块总数: {} (同步 {} / 异步 {})\n\n",
                       modules.size(),
                       static_cast<size_t>(std::count_if(
                           modules.begin(),
                           modules.end(),
                           [](const ModuleEntry& m) { return m.mode == "同步"; }
                       )),
                       static_cast<size_t>(std::count_if(
                           modules.begin(),
                           modules.end(),
                           [](const ModuleEntry& m) { return m.mode == "异步"; }
                       )));
    out += "| 模块 | 调度 | 构建 |\n";
    out += "|---|---|---|\n";
    for (const auto& m : modules) {
        out += fmt::format("| `{}` | {} | {} |\n", m.name, m.mode, m.build);
    }
    return out;
}

} // namespace

TestResult testTestModules() {
    const auto testCppPath = sourceFilePath("test/test.cpp");
    XX_TEST_EXPECT_TRUE(!testCppPath.empty());
    if (testCppPath.empty()) {
        TEST_FAIL << "test_modules: 找不到 agent/test/test.cpp (构建期源码根未注入?)"
                  << std::endl;
        return TestResult{g_tm_passed, g_tm_failed};
    }
    const auto text = readArtifactFile(testCppPath.string());
    XX_TEST_EXPECT_TRUE(text.has_value());
    if (!text.has_value()) {
        TEST_FAIL << "test_modules: 读取失败 " << testCppPath.string() << std::endl;
        return TestResult{g_tm_passed, g_tm_failed};
    }

    const auto modules = parseModules(*text);
    // 扫描量下限: 解析规则失效时不能"零模块全通过"
    XX_TEST_EXPECT_GE(modules.size(), size_t{120});
    // 模块名不重复 (重名会让 `agentxx_test 名字` 跑两个模块)
    {
        std::map<std::string, int> seen;
        std::string                duplicated;
        for (const auto& m : modules) {
            if (++seen[m.name] == 2) {
                duplicated = m.name;
            }
        }
        XX_TEST_EXPECT_TRUE(duplicated.empty());
        if (!duplicated.empty()) {
            TEST_FAIL << "test_modules: 模块名重复 `" << duplicated << "`" << std::endl;
        }
    }

    const auto doc       = buildModulesDoc(modules);
    const auto docPath   = sourceRootDir().parent_path() / "docs" / "zh-cn" / "design"
                         / "test-modules.md";
    const bool update    = artifactUpdateMode("AGENTXX_UPDATE_TEST_MODULES");
    const auto check     = checkGeneratedArtifact(
        docPath.string(),
        doc,
        update,
        "test-modules.md",
        "AGENTXX_UPDATE_TEST_MODULES"
    );
    XX_TEST_EXPECT_TRUE(check.ok);
    if (!check.ok) {
        TEST_FAIL << check.message << std::endl;
    }
    return TestResult{g_tm_passed, g_tm_failed};
}

} // namespace test
} // namespace agentxx
