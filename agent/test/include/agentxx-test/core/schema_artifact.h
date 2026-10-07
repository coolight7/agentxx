#pragma once

/// 生成物与新鲜度门禁的共用骨架 (计划 PRO-4 / CFG-9)
///
/// "生成物 + 逐字节比对" 这套做法在项目里有两处: Wire 协议字段清单 (`wire_schema`)
/// 与配置键目录 (`config_keys`)。两者只差"生成什么内容", 读写/比对/差异输出应当
/// 只有一份实现, 因此抽到本头。
///
/// 用法:
/// ```cpp
/// const auto text = buildArtifact();                       // 生成内容
/// const bool update = artifactUpdateMode("AGENTXX_UPDATE_..."); // 一键更新开关
/// const auto result = checkGeneratedArtifact(path, text, update, "config-keys.json");
/// XX_TEST_EXPECT_TRUE(result.ok);
/// if (!result.ok) { TEST_FAIL << result.message << std::endl; }
/// ```
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace test {

/// 生成物比对结果
struct ArtifactCheck {
    bool        ok = false;
    std::string message; ///< 失败原因 (含首个差异位置与重新生成命令)
};

/// 读取文件全文; 文件不存在/打不开返回 nullopt
inline std::optional<std::string> readArtifactFile(const std::string& path) {
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

/// 写入文件全文 (父目录自动创建)
inline bool writeArtifactFile(const std::string& path, std::string_view content) {
    std::error_code ec;
    const auto      file = std::filesystem::path(path);
    if (const auto parent = file.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << content;
    return out.good();
}

/// 逐行比较, 返回首个不同位置的可读说明 (便于定位改了什么)
inline std::string artifactDiffLines(const std::string& expected, const std::string& actual) {
    auto split = [](const std::string& text) {
        std::vector<std::string> out;
        std::string              cur;
        std::istringstream       in(text);
        while (std::getline(in, cur)) {
            if (!cur.empty() && cur.back() == '\r') {
                cur.pop_back();
            }
            out.push_back(cur);
        }
        return out;
    };
    const auto expLines = split(expected);
    const auto actLines = split(actual);
    const auto count    = std::max(expLines.size(), actLines.size());
    for (size_t i = 0; i < count; ++i) {
        const std::string e = i < expLines.size() ? expLines[i] : std::string{"<missing>"};
        const std::string a = i < actLines.size() ? actLines[i] : std::string{"<missing>"};
        if (e != a) {
            return fmt::format(
                "first difference at line {}:\n  baseline: {}\n  generated: {}",
                i + 1,
                e,
                a
            );
        }
    }
    return "content differs only in line endings";
}

/// 更新模式下写回; 否则与仓库内生成物逐字节比对
/// - 路径为空 (未注入编译期定义, 如独立构建) 时退化为"只生成不比较"
/// - [updateEnv] 为 `1` / `true` 时进入更新模式 (只重写生成物, 由人工 review diff)
/// - [artifact] 仅用于失败信息里的名字
inline ArtifactCheck checkGeneratedArtifact(
    const std::string& path,
    const std::string& actual,
    bool               update,
    std::string_view   artifact       = "artifact",
    std::string_view   updateEnvName  = ""
) {
    ArtifactCheck out;
    if (path.empty()) {
        out.ok      = true;
        out.message = "no artifact path injected, skipping comparison";
        return out;
    }
    if (update) {
        out.ok = writeArtifactFile(path, actual);
        out.message
            = out.ok ? fmt::format("[{}] regenerated: {}", artifact, path)
                     : fmt::format("[{}] failed to write: {}", artifact, path);
        return out;
    }
    const auto baseline = readArtifactFile(path);
    if (!baseline.has_value()) {
        out.message = fmt::format(
            "[{}] missing: {}{}",
            artifact,
            path,
            updateEnvName.empty()
                ? std::string{}
                : fmt::format(" (run with {}=1 to generate)", updateEnvName)
        );
        return out;
    }
    if (*baseline == actual) {
        out.ok = true;
        return out;
    }
    out.message = fmt::format(
        "[{}] out of date: {}\n{}{}",
        artifact,
        path,
        artifactDiffLines(*baseline, actual),
        updateEnvName.empty() ? std::string{}
                              : fmt::format("\nrun with {}=1 to regenerate", updateEnvName)
    );
    return out;
}

/// 读取环境变量开关 (`1` / `true` 视为打开)
inline bool artifactUpdateMode(std::string_view envName) {
    const char* value = std::getenv(std::string{envName}.c_str());
    if (value == nullptr) {
        return false;
    }
    const std::string_view sv{value};
    return sv == "1" || sv == "true";
}

} // namespace test
} // namespace agentxx
