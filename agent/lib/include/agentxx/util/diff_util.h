#pragma once

#include <algorithm>
#include <cstdint>
#include <fmt/format.h>
#include <string>
#include <vector>

namespace agentxx {
namespace util {

/// 逐行 diff 的类型
enum class DiffLineType : uint8_t {
    Context, // 未变更
    Add,     // 新增
    Delete,  // 删除
};

/// 一行 diff 结果
struct DiffLine {
    DiffLineType type;
    std::string  text;
    int          oldLineNo = 0; // 0 表示无 (Add 行无旧行号)
    int          newLineNo = 0; // 0 表示无 (Delete 行无新行号)
};

/// 按 '\n' 拆分文本为行 (不保留行尾换行符; 末尾空行忽略)
[[nodiscard]] inline std::vector<std::string> splitDiffLines(std::string_view s) {
    std::vector<std::string> lines;
    size_t                   start = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n') {
            lines.emplace_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    if (start < s.size()) {
        lines.emplace_back(s.substr(start));
    }
    return lines;
}

/// 基于 LCS 计算 oldText -> newText 的逐行 diff
[[nodiscard]] inline std::vector<DiffLine>
    computeLineDiff(std::string_view oldText, std::string_view newText) {
    auto       oldLines = splitDiffLines(oldText);
    auto       newLines = splitDiffLines(newText);
    const auto n        = oldLines.size();
    const auto m        = newLines.size();

    // 规模保护: LCS DP 表为 (n+1)*(m+1) 个 uint32, 超大输入会内存暴涨且 O(n*m) 极慢。
    // 超过阈值时降级为朴素 diff (全部删除 + 全部新增): 结果仍正确(表示完整变更),
    // 只是非最小编辑, 以此避免大文件(如万行级)导致资源耗尽。
    constexpr size_t kMaxDpCells = 16'000'000; // ~64MB (uint32), 约允许 4000x4000
    if (m + 1 > 0 && (n + 1) > kMaxDpCells / (m + 1)) {
        std::vector<DiffLine> result;
        result.reserve(n + m);
        int oldNo = 1;
        for (const auto& l : oldLines) {
            result.push_back({DiffLineType::Delete, l, oldNo++, 0});
        }
        int newNo = 1;
        for (const auto& l : newLines) {
            result.push_back({DiffLineType::Add, l, 0, newNo++});
        }
        return result;
    }

    // LCS 长度表 (n+1) x (m+1)
    std::vector<std::vector<uint32_t>> dp(n + 1, std::vector<uint32_t>(m + 1, 0));
    for (size_t i = 1; i <= n; ++i) {
        for (size_t j = 1; j <= m; ++j) {
            if (oldLines[i - 1] == newLines[j - 1]) {
                dp[i][j] = dp[i - 1][j - 1] + 1;
            } else {
                dp[i][j] = std::max(dp[i - 1][j], dp[i][j - 1]);
            }
        }
    }

    // 回溯生成 diff (逆序 push, 最后反转)
    std::vector<DiffLine> result;
    size_t                i = n, j = m;
    auto                  oldNo = static_cast<int>(n);
    auto                  newNo = static_cast<int>(m);
    while (i > 0 || j > 0) {
        if (i > 0 && j > 0 && oldLines[i - 1] == newLines[j - 1]) {
            result.push_back({DiffLineType::Context, oldLines[i - 1], oldNo, newNo});
            --i;
            --j;
            --oldNo;
            --newNo;
        } else if (j > 0 && (i == 0 || dp[i][j - 1] >= dp[i - 1][j])) {
            result.push_back({DiffLineType::Add, newLines[j - 1], 0, newNo});
            --j;
            --newNo;
        } else {
            result.push_back({DiffLineType::Delete, oldLines[i - 1], oldNo, 0});
            --i;
            --oldNo;
        }
    }
    std::reverse(result.begin(), result.end());
    return result;
}

/// 生成 git 风格的 unified diff 字符串 (单 hunk 覆盖全部变更)
[[nodiscard]] inline std::string
    makeUnifiedDiff(std::string_view oldText, std::string_view newText, std::string_view path) {
    const auto oldCount = splitDiffLines(oldText).size();
    const auto newCount = splitDiffLines(newText).size();
    auto       diff     = computeLineDiff(oldText, newText);

    std::string out;
    out += fmt::format("--- a/{}\n", path);
    out += fmt::format("+++ b/{}\n", path);
    out += fmt::format("@@ -1,{} +1,{} @@\n", oldCount, newCount);
    for (const auto& l : diff) {
        switch (l.type) {
            case DiffLineType::Context:
                out += fmt::format(" {}\n", l.text);
                break;
            case DiffLineType::Delete:
                out += fmt::format("-{}\n", l.text);
                break;
            case DiffLineType::Add:
                out += fmt::format("+{}\n", l.text);
                break;
        }
    }
    return out;
}

} // namespace util
} // namespace agentxx
