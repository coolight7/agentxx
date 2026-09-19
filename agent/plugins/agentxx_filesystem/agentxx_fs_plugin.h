/// agentxx_filesystem 插件 —— 共享头
#pragma once

#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_guard.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "utilxx/diff_util.h"
#include "utilxx_base/json.h"
#include "utilxx_base/string_util.h"
#include <cstdint>
#include <fmt/format.h>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx_fs_plugin {

/// 公共助手统一由插件 SDK 提供 (实现见 plugin_kit.h)
using agentxx::plugin::ctxGuardLogger;
using agentxx::plugin::pluginLog;
using agentxx::plugin::pluginStrdup;

struct PluginCtx : public agentxx::plugin::PluginBase {};

/// 提取"字符串列表"参数 (client 渲染摘要用):
/// - 值为数组时逐项提取其中的字符串元素 (跳过非字符串元素)
/// - 值不是数组时 (如 LLM 下发的单字符串) 直接按单个字符串渲染
///   (如 file_patterns 写成 "agent/test/*.cpp")
/// - 缺失/其他类型: 返回空列表
inline std::vector<std::string> stringListArg(const utilxx_base::Json& args, std::string_view key) {
    std::vector<std::string> out;
    const std::string        k{key};
    if (!args.is_object() || !args.contains(k)) {
        return out;
    }
    const auto& v = args[k];
    if (v.is_string()) {
        out.push_back(v.get<std::string>());
    } else if (v.is_array()) {
        for (const auto& item : v) {
            if (item.is_string()) {
                out.push_back(item.get<std::string>());
            }
        }
    }
    return out;
}

/// 逐行 diff 的增删行数摘要 (`[+3 -1]`, 折叠头展示的"类似 git 的 +行 -行 提示")
/// - 计数基于逐行 LCS (`utilxx::computeLineDiff`, 与 client 展开体 diff 渲染同一
///   算法), 因此折叠头显示的行数与展开后看到的差异一致 —— 不是 `old_str`/`new_str`
///   的行数差 (那样会把未变的上下文行也算进来)
/// - 先按 LF 归一化行尾: 与 edit 工具执行体一致 (它匹配前会把两侧统一为 LF),
///   否则 LLM 下发 CRLF 的多行 `old_str` 会被判为整段重写
///
/// - `args`:
///     - [oldStr] 替换前文本 (`agentxx_filesystem_edit` 的 `old_str` 参数)
///     - [newStr] 替换后文本 (同上 `new_str` 参数)
///     - [repeat] 替换处数 (默认 1); `multi_replace` 场景传实际命中处数
///       (见 [parseEditReplaceHits]), 得到的是整个文件的**总**增删行数
///       (单处统计 × 处数); 传 < 1 时按 1 处理
///
/// - `return` 形如 `[+3 -1]` 的摘要; 两侧都为空或逐行比较无变化时返回空串
///   (调用方跳过该段, 不显示 `[+0 -0]`)
inline std::string
    diffStatText(std::string_view oldStr, std::string_view newStr, int64_t repeat = 1) {
    if (oldStr.empty() && newStr.empty()) {
        return {};
    }
    // 参数此刻可能仍在流式输出中 (截断的 JSON 由调用方按解析失败处理),
    // 这里只做文本处理: 拷贝一份再归一化, 不改动调用方持有的字符串
    std::string oldText{oldStr};
    std::string newText{newStr};
    utilxx_base::normalizeCrlfToLf(oldText);
    utilxx_base::normalizeCrlfToLf(newText);

    int64_t added   = 0;
    int64_t removed = 0;
    for (const auto& line : utilxx::computeLineDiff(oldText, newText)) {
        if (line.type == utilxx::DiffLineType::Add) {
            ++added;
        } else if (line.type == utilxx::DiffLineType::Delete) {
            ++removed;
        }
    }
    if (added == 0 && removed == 0) {
        return {};
    }
    const int64_t times = (repeat > 1) ? repeat : 1;
    return fmt::format("[+{} -{}]", added * times, removed * times);
}

/// 解析 edit 工具 `multi_replace` 结果里的替换处数
/// - 结果格式由工具执行体产出 (见 filesystem_impl.h 的 `fileEditExecuteImpl`):
///   `Success, Replace 3 hits`; 单次替换 (非 multi_replace) 的结果是 `success`
/// - 只接受严格形态 (固定前后缀 + 中间纯数字), 结果文本意外变形时返回 0,
///   调用方按单处展示 (不报错, 只退化为不含总数的摘要)
///
/// - `args`:
///     - [resultText] 工具结果文本 (渲染输入的 `result_text`)
///
/// - `return` 替换处数 (>= 1); 非该形态/解析失败返回 0
inline int64_t parseEditReplaceHits(std::string_view resultText) {
    constexpr std::string_view kPrefix = "Success, Replace ";
    constexpr std::string_view kSuffix = " hits";
    /// 处数上限: 只用于兜底防御 (避免畸形文本导致处数畸大 / 乘法溢出)
    constexpr int64_t kMaxHits = 1'000'000;

    if (!resultText.starts_with(kPrefix) || !resultText.ends_with(kSuffix)) {
        return 0;
    }
    // 前后缀区间重叠的畸形文本 (长度不足) 会让下面的差值下溢, 一并拦掉
    const size_t digitsLen = resultText.size() - kPrefix.size() - kSuffix.size();
    if (digitsLen == 0 || digitsLen > resultText.size()) {
        return 0;
    }
    int64_t hits = 0;
    for (const char c : resultText.substr(kPrefix.size(), digitsLen)) {
        if (c < '0' || c > '9') {
            return 0;
        }
        hits = hits * 10 + (c - '0');
        if (hits > kMaxHits) {
            return 0;
        }
    }
    return (hits > 0) ? hits : 0;
}

} // namespace agentxx_fs_plugin
