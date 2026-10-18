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
#include <utility>
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

/// 逐行 diff 的增删行数 (`[+3 -1]` 折叠头提示的原始计数)
/// - 计数基于逐行 LCS (`utilxx::computeLineDiff`, 与 client 展开体 diff 渲染同一
///   算法), 因此折叠头显示的行数与展开后看到的差异一致 —— 不是 `old_str`/`new_str`
///   的行数差 (那样会把未变的上下文行也算进来)
/// - 先按 LF 归一化行尾: 与 edit 工具执行体一致 (它匹配前会把两侧统一为 LF),
///   否则 LLM 下发 CRLF 的多行 `old_str` 会被判为整段重写
///
/// - `args`:
///     - [oldStr] 替换前文本 (`agentxx_filesystem_edit` 的 `old_str` 参数)
///     - [newStr] 替换后文本 (同上 `new_str` 参数)
///
/// - `return` 单段的增删行数 (未变的上下文行不计入)
inline std::pair<int64_t, int64_t>
    diffStatCounts(std::string_view oldStr, std::string_view newStr) {
    if (oldStr.empty() && newStr.empty()) {
        return {0, 0};
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
    return {added, removed};
}

/// 一段替换 (`agentxx_filesystem_edit` 的一次 `old_str` -> `new_str`)
struct EditSegmentText {
    std::string oldStr;
    std::string newStr;
};

/// 从 edit 工具参数取替换段 (client 渲染摘要与展开体用)
/// - `edits` 数组: 逐项取 `old_str`/`new_str` (非对象元素跳过)
/// - `edits` 为单个对象 (少写一层数组的常见形态): 按单个段处理
/// - 未给 `edits`: 按 `old_str`/`new_str` 取单个段 (旧写法)
/// - 参数还在流式输出中 (解析失败) / 两种写法都没有: 返回空列表
///   (调用方跳过摘要与 diff 片段)
inline std::vector<EditSegmentText> editSegmentsArg(const utilxx_base::Json& args) {
    std::vector<EditSegmentText> out;
    if (!args.is_object()) {
        return out;
    }
    auto takeOne = [&out](const utilxx_base::Json& item) {
        if (!item.is_object()) {
            return;
        }
        out.push_back(EditSegmentText{
            item.value<std::string>("old_str", std::string{}),
            item.value<std::string>("new_str", std::string{})
        });
    };
    // 缺失键 → 只读 operator[] 返回 Null 常量
    const auto& editsArg = args["edits"];
    if (!editsArg.is_null()) {
        if (editsArg.is_array()) {
            for (const auto& item : editsArg) {
                takeOne(item);
            }
        } else {
            takeOne(editsArg);
        }
        return out;
    }
    auto oldStr = args.value<std::string>("old_str", std::string{});
    auto newStr = args.value<std::string>("new_str", std::string{});
    if (!oldStr.empty() || !newStr.empty()) {
        out.push_back(EditSegmentText{std::move(oldStr), std::move(newStr)});
    }
    return out;
}

/// 替换段的增删行数摘要 (`[+3 -1]`, 折叠头展示的"类似 git 的 +行 -行 提示")
/// - 各段各替换一处, 按逐行 diff 的增删行数求和 (见 [diffStatCounts])
/// - 无任何变化时返回空串 (调用方不显示 `[+0 -0]`)
inline std::string diffStatTextOfSegments(const std::vector<EditSegmentText>& segments) {
    int64_t added   = 0;
    int64_t removed = 0;
    for (const auto& seg : segments) {
        const auto [oneAdded, oneRemoved]  = diffStatCounts(seg.oldStr, seg.newStr);
        added                             += oneAdded;
        removed                           += oneRemoved;
    }
    if (added == 0 && removed == 0) {
        return {};
    }
    return fmt::format("[+{} -{}]", added, removed);
}

} // namespace agentxx_fs_plugin
