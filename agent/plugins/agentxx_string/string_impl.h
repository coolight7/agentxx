/// agentxx_string 插件 —— 工具实现 (纯函数, 不含 C ABI 胶水)
/// - 从 libagentxx src/tools/string 拆分: 同名工具同行为
/// - 头文件-only: 插件入口与测试
///   ([test_string_tools.cpp](/agent/test/core/test_string_tools.cpp))
///   共同包含, 保证插件行为与测试覆盖一致
/// - 依赖: html2md (第三方, AGENTXX_INSTALL_DIR 安装) + cxx_utilxx (utilxx::Regex)
#pragma once

#include "utilxx/regex.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"
#include <html2md/html2md.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace agentxx_string_plugin {

/// 参数检查失败时抛出的异常 (由插件 SDK 边界捕获, 宿主统一按工具错误结果回给模型)
/// - 参数错误不返回编码后的 JSON: 错误与正常结果不混在同一形态里
using ArgError = std::invalid_argument;

/// agentxx_string_html_to_markdown 执行体 (原 StringHtml2MarkdownTool::execute_async)
/// - content 为空时抛 [ArgError]; 其余异常由调用方 (C ABI 边界) 捕获
inline std::string htmlToMarkdownExecute(const utilxx_base::Json& arguments) {
    auto content = arguments.value("content", std::string{});
    if (content.empty()) {
        throw ArgError{"Arg `content` is empty"};
    }

    auto options = html2md::Options{
        .splitLines = false,
    };
    auto convert = html2md::Converter{content, &options};
    return convert.convert();
}

/// agentxx_string_regexp 执行体 (原 StringRegexpTool::execute_async)
inline std::string regexpExecute(const utilxx_base::Json& arguments) {
    auto content = arguments.value("content", std::string{});
    if (content.empty()) {
        throw ArgError{"Arg `content` is empty"};
    }
    auto match_exps = arguments.value("exps", std::vector<std::string>{});
    if (match_exps.empty()) {
        throw ArgError{"Arg `exps` is empty"};
    }
    auto match_opt = arguments.value("opt", std::string{});
    if (match_opt.empty()) {
        throw ArgError{"Arg `opt` is empty"};
    }

    auto regex = utilxx::XXRegex::createRegex(match_exps);
    if (!regex) {
        return "[Error] Regex compilation failed";
    }
    if (match_opt == std::string_view{"search"}) {
        auto results = std::vector<utilxx::XXRegexMatchResult>{};
        if (regex->match(content, results)) {
            auto relist = utilxx_base::Json::array();
            for (size_t i = 0; i < results.size(); ++i) {
                const auto& item = results[i];
                relist.push_back(content.substr(item.start, item.end - item.start));
            }
            return utilxx_base::Json{
                {"tip", fmt::format("Match found {} items.", results.size())},
                {"result", relist},
            }
                .dump();
        }
    } else if (match_opt == std::string_view{"replace"}) {
        auto results = std::vector<utilxx::XXRegexMatchResult>{};
        auto restr
            = regex->replace(content, arguments.value("replace_str", std::string{}), results);
        if (false == results.empty()) {
            return restr;
        }
    } else if (match_opt == std::string_view{"remove"}) {
        auto results = std::vector<utilxx::XXRegexMatchResult>{};
        auto restr   = regex->remove(content, results);
        if (false == results.empty()) {
            return restr;
        }
    } else {
        throw ArgError{fmt::format("Arg `opt` is invalid: {}", match_opt)};
    }
    // 无匹配属正常结果 (非参数错误): 返回纯文本提示
    return "No match found";
}

} // namespace agentxx_string_plugin
