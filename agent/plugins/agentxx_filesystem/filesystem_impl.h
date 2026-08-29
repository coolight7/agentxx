// agentxx_filesystem 插件 —— 工具实现 (纯函数, 不含 C ABI 胶水)
// - 从 libagentxx src/tools/filesystem 拆分: 同名工具同行为
//     list / read / write / edit / glob / grep
// - 头文件-only: 插件入口与测试共同包含, 保证插件行为与测试覆盖一致
#pragma once

#include "agentxx/util/aho_corasick.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/log.h"
#include "agentxx/util/regex.h"
#include "agentxx/util/string_util.h"
#include "agentxx/util/util.h"
#include "asio/any_io_executor.hpp"
#include "asio/error.hpp"
#include "asio/read.hpp"
#include "asio/read_until.hpp"
#include "asio/redirect_error.hpp"
#include "asio/stream_file.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "asio/write.hpp"
#include "glob/glob.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <neograph/json.h>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace agentxx_fs_plugin {

/// 取消查询回调 (返回 true 表示会话已取消); 测试可传 nullptr 等价无取消支持
using IsCancelledFn = std::function<bool()>;

/// 超时上下文: 循环内经 expired() 轮询 (deadline <= 0 表示不限时)
struct Deadline {
    std::chrono::steady_clock::time_point point{};
    int64_t                               seconds = 0;

    static Deadline after(int64_t seconds) {
        Deadline d;
        d.seconds = seconds;
        if (seconds > 0) {
            d.point = std::chrono::steady_clock::now() + std::chrono::seconds{seconds};
        }
        return d;
    }

    bool enabled() const {
        return seconds > 0;
    }

    bool expired() const {
        return seconds > 0 && std::chrono::steady_clock::now() >= point;
    }
};

namespace detail {

/// 将 std::filesystem::path 转换为无损 UTF-8 字符串
inline std::string toUtf8(const std::filesystem::path& p) {
    return agentxx::util::pathToUtf8Generic(p);
}

/// 基于 workDir 的会话工作目录解析绝对路径
/// - workDir 非空时以其为相对路径基准 (会话工作目录与进程 cwd 解耦);
///   为空时回退进程 cwd (resolvedWorkDir 兜底, 与单参 toCurrentSystemAbsolutePath 一致)
inline std::string wsAbs(const std::string& workDir, const std::string& path) {
    if (workDir.empty()) {
        return agentxx::util::toCurrentSystemAbsolutePath(path);
    }
    return agentxx::util::toCurrentSystemAbsolutePath(path, workDir);
}

/// 将文本中的 LF (`\n`) 行尾统一转换为 CRLF (`\r\n`)。
inline void normalizeCrlfToLf(std::string& text) {
    if (text.find("\r\n") == std::string::npos) {
        return;
    }
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
            out += '\n';
            i   += 2;
        } else {
            out += text[i++];
        }
    }
    text.swap(out);
}

/// 解析 `type` 参数为类型集合。支持 string 或 array 两种形式。
/// 返回空集合表示 "any" (不按类型过滤)。合法值: file / dir / symlink / other / any。
inline std::set<std::string> collectTypeFilter(const neograph::json& typeArg) {
    std::set<std::string> types;
    auto                  addOne = [&](const std::string& t) {
        if (t == "any" || t.empty()) {
            return;
        }
        types.insert(t);
    };
    if (typeArg.is_string()) {
        addOne(typeArg.get<std::string>());
    } else if (typeArg.is_array()) {
        for (const auto& item : typeArg) {
            if (item.is_string()) {
                addOne(item.get<std::string>());
            }
        }
    }
    return types;
}

/// 获取路径实体的类型字符串 (与 filesystem_glob 的 type 参数语义一致)。
/// 先判 symlink 以准确识别符号链接 (即使其指向目录)。
inline std::string fileTypeOf(const std::filesystem::path& path) {
    std::error_code ec;
    auto            status = std::filesystem::symlink_status(path, ec);
    if (ec) {
        return "other";
    }
    if (std::filesystem::is_symlink(status)) {
        return "symlink";
    }
    if (std::filesystem::is_directory(status)) {
        return "dir";
    }
    if (std::filesystem::is_regular_file(status)) {
        return "file";
    }
    return "other";
}

/// 预编译 exclude_patterns 为正则列表 (非法模式静默忽略)。
inline std::vector<std::regex> compileExcludeRegexes(const std::vector<std::string>& excludePatterns
) {
    std::vector<std::regex> regexes;
    regexes.reserve(excludePatterns.size());
    for (const auto& ep : excludePatterns) {
        // 非法模式忽略
        agentxx::util::catchError<bool>(
            [&]() -> bool {
                regexes.emplace_back(glob::to_regex(ep));
                return true;
            },
            [](std::string) -> bool {
                return false;
            }
        );
    }
    return regexes;
}

/// 判断路径 (generic_string) 是否命中任一 exclude 正则。
inline bool isExcluded(const std::string& pathStr, const std::vector<std::regex>& excludeRegexes) {
    for (const auto& re : excludeRegexes) {
        if (std::regex_match(pathStr, re)) {
            return true;
        }
    }
    return false;
}

/// 读取完整文件文本 (同步); 打开失败抛出异常
inline std::string readFileContent(const std::string& filepath) {
    auto p = agentxx::util::utf8ToPath(filepath);

    std::ifstream stream;
    stream.open(p, std::ios_base::binary);
    if (!stream) {
        auto ec = std::error_code{errno, std::system_category()};
        throw std::runtime_error(fmt::format(R"(Can not open file. Error: {})", ec.message()));
    }
    auto result
        = std::string{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    stream.close();
    return result;
}

} // namespace detail

// =====================================================================
// agentxx_filesystem_list 执行体 (原 FileSystemListTool::execute_async)
// =====================================================================
inline std::string fileListExecuteImpl(
    const neograph::json& arguments,
    const std::string&    workDir,
    const IsCancelledFn&  isCancelled = nullptr
) {
    auto targetPath = detail::wsAbs(workDir, arguments.value("path", std::string{}));
    if (targetPath.empty()) {
        return R"([Error] Arg `path` is empty)";
    }
    auto recursive = arguments.value("recursive", false);
    auto limit     = arguments.value<int64_t>("limit", 100);
    auto timeout   = static_cast<int64_t>(arguments.value<double>("timeout", 60.0));
    auto deadline  = Deadline::after(timeout);

    std::vector<std::string> lines;

    auto onAppendItem = [&](const std::filesystem::directory_entry& entity) {
        // 单个条目处理失败仅记录错误行, 不中断整个列表
        agentxx::util::catchError<bool>(
            [&]() -> bool {
                auto file_time = entity.last_write_time();

                auto sys_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    file_time - std::filesystem::file_time_type::clock::now()
                    + std::chrono::system_clock::now()
                );

                // 类型标识列 (仿 ls -l): 首字符为真实类型 (d/-/l/?), 权限位无数据用通用占位
                std::string typeStr = "??????????";
                std::string sizeStr = "-";
                if (entity.is_directory()) {
                    typeStr = "drwxr-xr-x";
                } else if (entity.is_regular_file()) {
                    typeStr = "-rw-r--r--";
                    sizeStr = std::to_string(entity.file_size());
                } else if (entity.is_symlink()) {
                    typeStr = "lrwxrwxrwx";
                }

                // 路径列: 目录加 `/` 后缀, 符号链接附加指向目标 (对齐 ls -l)
                auto pathStr = detail::toUtf8(entity.path());
                if (entity.is_directory()) {
                    pathStr += "/";
                } else if (entity.is_symlink()) {
                    std::error_code ec;
                    auto            target = std::filesystem::read_symlink(entity.path(), ec);
                    if (!ec) {
                        pathStr += " -> " + detail::toUtf8(target);
                    }
                }

                auto timeStr = std::format("{:%Y-%m-%d %H:%M}", sys_time);
                lines.push_back(fmt::format("{} {:>10}  {}  {}", typeStr, sizeStr, timeStr, pathStr)
                );
                return true;
            },
            [&](std::string errmsg) -> bool {
                lines.push_back(fmt::format("[Error] {}: {}", detail::toUtf8(entity.path()), errmsg)
                );
                return false;
            }
        );
    };

    auto checkStop = [&]() -> bool {
        // 返回 true 表示应提前终止 (取消或超时)
        if (isCancelled && isCancelled()) {
            lines.push_back("[Error] Cancelled");
            return true;
        }
        if (deadline.expired()) {
            lines.push_back(fmt::format(
                R"([Error] Timed out after {} seconds. Try narrowing the path or setting a limit.)",
                timeout
            ));
            return true;
        }
        return false;
    };

    if (false == std::filesystem::exists(targetPath)) {
        lines.push_back("[Error] Path not exist");
    } else if (std::filesystem::is_directory(targetPath)) {
        if (recursive) {
            // skip_permission_denied: 单个不可读目录被跳过而非中断整个列表
            for (const auto& entity : std::filesystem::recursive_directory_iterator(
                     targetPath,
                     std::filesystem::directory_options::skip_permission_denied
                 )) {
                if (checkStop()) {
                    break;
                }
                onAppendItem(entity);
                if (limit > 0 && static_cast<int64_t>(lines.size()) >= limit) {
                    break;
                }
            }
        } else {
            for (const auto& entity : std::filesystem::directory_iterator(
                     targetPath,
                     std::filesystem::directory_options::skip_permission_denied
                 )) {
                if (checkStop()) {
                    break;
                }
                onAppendItem(entity);
                if (limit > 0 && static_cast<int64_t>(lines.size()) >= limit) {
                    break;
                }
            }
        }
    } else if (std::filesystem::is_regular_file(targetPath)) {
        onAppendItem(std::filesystem::directory_entry(targetPath));
    } else {
        lines.push_back("[Error] Path exist, but is not a directory or file");
    }

    // 未触发提前终止且无条目: 空目录提示行, 让 LLM 能区分 "空目录" 与失败
    if (lines.empty()) {
        lines.push_back("[Empty]");
    }

    // 拼接为多行文本 (与命令行 ls 一致)
    std::string output;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) {
            output += '\n';
        }
        output += lines[i];
    }
    return output;
}

// =====================================================================
// agentxx_filesystem_read 执行体 (原 FilesystemReadTextFileTool::execute_async)
// =====================================================================
inline std::string fileReadExecuteImpl(
    const neograph::json& arguments,
    const std::string&    workDir,
    const IsCancelledFn& isCancelled = nullptr // 单文件短操作不轮询; 形参保持与其他执行体一致
) {
    auto filepath = detail::wsAbs(workDir, arguments.value("path", std::string{}));
    if (filepath.empty()) {
        return R"([Error] Arg `path` is empty)";
    }
    auto systemCharsetFilePath = filepath;
    agentxx::util::autoConvertToSystemPath(systemCharsetFilePath);
    auto text_line_offset = arguments.value<int64_t>("line_offset", -1);
    auto text_line_limit  = arguments.value<int64_t>("line_limit", -1);

    /// 同步阻塞读取文件
    std::ifstream stream;
    stream.open(systemCharsetFilePath, std::ios_base::binary);
    if (!stream) {
        auto ec = std::error_code{errno, std::system_category()};
        throw std::runtime_error(fmt::format(R"(Can not open file. Error: {})", ec.message()));
    }

    if (text_line_offset >= 0 || text_line_limit > 0) {
        // 读取部分文件
        const auto offset = (text_line_offset >= 0) ? static_cast<size_t>(text_line_offset) : 0;
        const auto limit  = (text_line_limit > 0) ? static_cast<size_t>(text_line_limit)
                                                  : std::numeric_limits<size_t>::max();
        std::stringstream result{};
        size_t            lineNum = 0;
        size_t            endLine = offset;

        if (offset < std::numeric_limits<size_t>::max() - limit) {
            // 防止相加溢出回绕
            endLine = offset + limit;
        } else {
            endLine = std::numeric_limits<size_t>::max();
        }

        for (std::string line; std::getline(stream, line) && lineNum < endLine; lineNum++) {
            // 跳过偏移行
            if (lineNum < offset) {
                continue;
            }
            result << line << "\n";
        }

        stream.close();
        if (lineNum == 0 && offset == 0) {
            // 空文件: 第 0 行视为空行, 返回空串而非报错
            return "";
        }
        if (lineNum <= offset) {
            // offset 超出文件行数
            throw std::runtime_error{fmt::format(
                R"(Arg `line_offset`({} lines) is out of range of file lines({} lines).)",
                offset,
                lineNum
            )};
        }

        auto rawStr = result.str();
        // 保留原始的 crlf 或 \n 换行符不转换
        agentxx::util::autoConvertToUtf8(rawStr);
        return rawStr;
    }

    // 读取完整文件
    auto result
        = std::string{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    stream.close();
    // 保留原始的 crlf 或 \n 换行符不转换
    agentxx::util::autoConvertToUtf8(result);
    return result;
}

// =====================================================================
// agentxx_filesystem_write 执行体 (原 FilesystemWriteFileTool::execute_async)
// =====================================================================
inline std::string fileWriteExecuteImpl(
    const neograph::json& arguments,
    const std::string&    workDir,
    const IsCancelledFn& isCancelled = nullptr // 单文件短操作不轮询; 形参保持与其他执行体一致
) {
    auto filepath = detail::wsAbs(workDir, arguments.value("path", std::string{}));
    if (filepath.empty()) {
        return R"([Error] Arg `path` is empty)";
    }
    auto content   = arguments.value<std::string>("content", std::string{});
    auto overwrite = arguments.value<bool>("overwrite", false);

    std::ofstream stream;
    auto          path = agentxx::util::utf8ToPath(filepath);
    if (false == overwrite && std::filesystem::exists(path)) {
        throw std::runtime_error{"File already exist. Set `overwrite` = true if want to overwrite."
        };
    }
    if (false == std::filesystem::exists(path.parent_path())
        && false == std::filesystem::create_directories(path.parent_path())) {
        // 创建父目录
        throw std::runtime_error{fmt::format(
            R"(Can not create `path`({})'s parent dirs.)",
            detail::toUtf8(path.parent_path())
        )};
    }

    stream.open(path, std::ios_base::out | std::ios_base::trunc | std::ios_base::binary);
    if (!stream) {
        auto ec = std::error_code{errno, std::system_category()};
        throw std::runtime_error{
            fmt::format(R"(Can not create or open file. Error: {})", ec.message())
        };
    }

    if (false == content.empty()) {
        // 写入文本内容
        stream << content;
        if (!stream) {
            auto ec = std::error_code{errno, std::system_category()};
            throw std::runtime_error{
                fmt::format(R"(File created success, but write failed. Error: {})", ec.message())
            };
        }
    }

    stream.close();
    return "success";
}

// =====================================================================
// agentxx_filesystem_edit 执行体 (原 FilesystemEditTextFileTool::execute_async)
// =====================================================================
inline std::string fileEditExecuteImpl(
    const neograph::json& arguments,
    const std::string&    workDir,
    const IsCancelledFn& isCancelled = nullptr // 单文件短操作不轮询; 形参保持与其他执行体一致
) {
    auto filepath = detail::wsAbs(workDir, arguments.value("path", std::string{}));
    if (filepath.empty()) {
        return "[Error] Arg `path` is empty";
    }
    auto systemCharsetFilePath = filepath;
    agentxx::util::autoConvertToSystemPath(systemCharsetFilePath);
    auto old_str = arguments.value<std::string>("old_str", std::string{});
    if (old_str.empty()) {
        return "[Error] Arg `old_str` is empty";
    }
    auto new_str       = arguments.value<std::string>("new_str", std::string{});
    auto multi_replace = arguments.value<bool>("multi_replace", false);

    if (new_str == old_str) {
        return "[Error] Arg `old_str` and `new_str` are equal and unchanged.";
    }

    // 统一到 \n 换行符
    // - 与 filesystem_read 的逻辑不同，read 应当保留原始的内容，edit 应当尽可能保证修改成功，
    //   如果 llm 需要写回 crlf，可使用 shell
    detail::normalizeCrlfToLf(old_str);
    detail::normalizeCrlfToLf(new_str);

    auto path = std::filesystem::path(filepath);
    if (false == std::filesystem::exists(path)) {
        throw std::runtime_error{"File not exist"};
    }

    // 读取完整文件并预处理 (先转 UTF-8 使 GBK 等编码文件可正常匹配, 再统一换行符)
    std::string content = detail::readFileContent(filepath);
    agentxx::util::autoConvertToUtf8(content);
    detail::normalizeCrlfToLf(content);

    int    replaceHit = 0;
    size_t pos        = 0;
    while ((pos = content.find(old_str, pos)) != std::string::npos) {
        replaceHit++;
        content.replace(pos, old_str.length(), new_str);
        // 跳过新字符串，避免死循环
        pos += new_str.length();
        if (false == multi_replace) {
            break;
        }
    }

    if (0 == replaceHit) {
        throw std::runtime_error{
            R"(No match `old_str` found, Try re-reading to get the latest file content.)"
        };
    }

    // 原子写: 先写同目录临时文件, 成功后 rename 覆盖原文件,
    // 避免直接 truncate 原文件后写入中途失败导致原内容永久丢失
    static std::atomic<uint64_t> s_editTmpSeq{0};
    const auto                   tmpPath
        = systemCharsetFilePath + fmt::format(".agentxx_edit_tmp_{}", s_editTmpSeq.fetch_add(1));

    {
        std::ofstream stream(
            tmpPath,
            std::ios_base::out | std::ios_base::trunc | std::ios_base::binary
        );
        if (!stream) {
            auto ec = std::error_code{errno, std::system_category()};
            throw std::runtime_error{
                fmt::format(R"(Can not open temp file to write: {})", ec.message())
            };
        }
        stream << content;
        stream.close();
        if (!stream) {
            std::error_code rmEc;
            std::filesystem::remove(tmpPath, rmEc);
            auto ec = std::error_code{errno, std::system_category()};
            throw std::runtime_error{fmt::format(R"(Write temp file failed: {})", ec.message())};
        }
    }

    std::error_code renameEc;
    std::filesystem::rename(tmpPath, systemCharsetFilePath, renameEc);
    if (renameEc) {
        std::error_code rmEc;
        std::filesystem::remove(tmpPath, rmEc);
        throw std::runtime_error{
            fmt::format(R"(Failed to replace original file: {})", renameEc.message())
        };
    }

    if (multi_replace) {
        return fmt::format(R"(Success, Replace {} hits)", replaceHit);
    }
    return "success";
}

// =====================================================================
// agentxx_filesystem_glob 执行体 (原 FilesystemGlobTool::execute_async)
// =====================================================================
inline std::string fileGlobExecuteImpl(
    const neograph::json& arguments,
    const std::string&    workDir,
    const IsCancelledFn&  isCancelled = nullptr
) {
    auto file_patterns = arguments.value("file_patterns", std::vector<std::string>{});
    if (file_patterns.empty()) {
        return R"([Error] Arg `file_patterns` is empty)";
    }
    auto timeout  = static_cast<int64_t>(arguments.value<double>("timeout", 60.0));
    auto deadline = Deadline::after(timeout);

    for (auto& item : file_patterns) {
        item = detail::wsAbs(workDir, item);
    }

    // 注: 路径匹配固定为大小写敏感 (移除 case-insensitive 支持)。历史原因见
    // lib filesystem.cpp 同注释 (case_fold 会破坏 max_depth 前缀计算与盘符识别)。
    auto maxDepth        = arguments.value<int64_t>("max_depth", -1);
    auto doSort          = arguments.value<bool>("sort", false);
    auto typeFilter      = detail::collectTypeFilter(arguments.value("type", neograph::json{}));
    auto excludePatterns = arguments.value("exclude_patterns", std::vector<std::string>{});
    for (auto& item : excludePatterns) {
        item = detail::wsAbs(workDir, item);
    }

    auto checkCancel = [&]() -> bool {
        return isCancelled && isCancelled();
    };

    // glob 取消标志: 本实现以 deadline/cancel 轮询控制整体流程,
    // 遍历内部传入恒为 false 的局部标志 (不泄漏堆分配)
    std::atomic<bool> globNeverCancel{false};

    // 智能选择 glob/rglob: 含 `**` 的模式使用 rglob (递归), 否则使用 glob (仅当前目录)
    // 对齐 shell globstar 行为: `*.txt` 只匹配当前目录, `**/*.txt` 才递归
    std::vector<std::filesystem::path> resultList;
    for (const auto& pattern : file_patterns) {
        if (checkCancel() || deadline.expired()) {
            return "[Error] Cancelled or timed out";
        }
        if (glob::has_recursive_segment(pattern)) {
            auto matched = glob::rglob(pattern, true, globNeverCancel);
            resultList.insert(
                resultList.end(),
                std::make_move_iterator(matched.begin()),
                std::make_move_iterator(matched.end())
            );
        } else {
            auto matched = glob::glob(pattern, true, globNeverCancel);
            resultList.insert(
                resultList.end(),
                std::make_move_iterator(matched.begin()),
                std::make_move_iterator(matched.end())
            );
        }
    }

    if (resultList.empty()) {
        return R"([Error] No match `file_patterns` file found)";
    }

    // 默认去重: 多 pattern 可能匹配到相同路径
    std::sort(resultList.begin(), resultList.end());
    resultList.erase(std::unique(resultList.begin(), resultList.end()), resultList.end());

    // 类型过滤 (对齐 find -type): file / dir / symlink / other / any
    if (!typeFilter.empty()) {
        std::vector<std::filesystem::path> filtered;
        filtered.reserve(resultList.size());
        for (const auto& p : resultList) {
            if (checkCancel() || deadline.expired()) {
                return "[Error] Cancelled or timed out";
            }
            if (typeFilter.count(detail::fileTypeOf(p))) {
                filtered.push_back(p);
            }
        }
        resultList = std::move(filtered);
    }

    // exclude_patterns 过滤: 将 glob 模式转为正则, 匹配到的路径被排除
    if (!excludePatterns.empty()) {
        auto excludeRegexes = detail::compileExcludeRegexes(excludePatterns);
        if (!excludeRegexes.empty()) {
            std::vector<std::filesystem::path> filtered;
            filtered.reserve(resultList.size());
            for (const auto& p : resultList) {
                if (false == detail::isExcluded(detail::toUtf8(p), excludeRegexes)) {
                    filtered.push_back(p);
                }
            }
            resultList = std::move(filtered);
        }
    }

    // max_depth 过滤 (对齐 find -maxdepth):
    // 计算匹配路径相对于模式静态前缀目录的深度, 超出则排除
    if (maxDepth >= 0) {
        std::vector<std::filesystem::path> filtered;
        filtered.reserve(resultList.size());
        for (const auto& p : resultList) {
            // 对每个 pattern 检查深度, 任一 pattern 满足即保留
            bool keep = false;
            for (const auto& pattern : file_patterns) {
                auto            baseDir = glob::static_prefix(pattern);
                std::error_code ec;
                auto            rel = std::filesystem::relative(p, baseDir, ec);
                if (ec) {
                    continue;
                }
                if (glob::path_depth(rel) <= static_cast<int>(maxDepth)) {
                    keep = true;
                    break;
                }
            }
            if (keep) {
                filtered.push_back(p);
            }
        }
        resultList = std::move(filtered);
    }

    // 排序 (去重时已排序, 但过滤后顺序可能变化; 用户显式要求排序时重新排序)
    if (doSort) {
        std::sort(resultList.begin(), resultList.end());
    }

    if (resultList.empty()) {
        return R"([Error] No match `file_patterns` file found after filtering)";
    }

    auto oss = std::ostringstream{};
    for (const auto& item : resultList) {
        if (checkCancel() || deadline.expired()) {
            return "[Error] Cancelled or timed out";
        }
        oss << detail::toUtf8(item) << '\n';
    }
    return oss.str();
}

// =====================================================================
// agentxx_filesystem_grep 执行体 (原 FilesystemGrepTool::execute_async)
// =====================================================================
inline std::string fileGrepExecuteImpl(
    const neograph::json& arguments,
    const std::string&    workDir,
    const IsCancelledFn&  isCancelled = nullptr
) {
    auto text_patterns_is_regex = arguments.value("text_patterns_is_regex", true);
    auto text_patterns          = arguments.value("text_patterns", std::vector<std::string>{});
    if (text_patterns.empty()) {
        return R"([Error] Arg `text_patterns` is empty)";
    }
    auto file_patterns = arguments.value("file_patterns", std::vector<std::string>{});
    if (file_patterns.empty()) {
        return R"([Error] Arg `file_patterns` is empty)";
    }
    for (auto& item : file_patterns) {
        item = detail::wsAbs(workDir, item);
    }
    auto output_mode = arguments.value("output_mode", std::string{"files_with_matches"});
    if (output_mode.empty()) {
        return R"([Error] Arg `output_mode` is empty)";
    }
    auto timeout  = static_cast<int64_t>(arguments.value<double>("timeout", 60.0));
    auto deadline = Deadline::after(timeout);

    // 新增参数 (对齐 Linux grep 行为)
    auto caseSensitive   = arguments.value<bool>("case_sensitive", true);
    auto maxCountPerFile = arguments.value<int64_t>("max_count_per_file", 0); // 0 = 不限
    auto contextLines    = arguments.value<int64_t>("context_lines", 0);

    auto checkStop = [&]() -> bool {
        return (isCancelled && isCancelled()) || deadline.expired();
    };

    // ---- glob 收集候选文件 ----
    std::atomic<bool>                  globNeverCancel{false};
    std::vector<std::filesystem::path> refilelist{};
    for (const auto& pattern : file_patterns) {
        if (checkStop()) {
            return "[Error] Cancelled or timed out";
        }
        // 智能选择 glob/rglob: 含 `**` 的模式使用 rglob (递归), 否则使用 glob
        // (仅当前目录); 路径匹配固定为大小写敏感
        // 单个 pattern 的遍历失败 (如目录树中存在系统代码页无法表示的文件名,
        // MSVC 下 fs::path 窄化转换抛 system_error) 不应中断整体搜索,
        // 经 catchError 隔离后跳过该 pattern 继续其余 pattern
        std::vector<std::filesystem::path> matched;
        auto                               globOk = agentxx::util::catchError<bool>(
            [&]() -> bool {
                if (glob::has_recursive_segment(pattern)) {
                    matched = glob::rglob(pattern, true, globNeverCancel);
                } else {
                    matched = glob::glob(pattern, true, globNeverCancel);
                }
                return true;
            },
            [&](std::string errmsg) -> bool {
                XX_LOGW("filesystem_grep: glob pattern '{}' failed, skipped: {}", pattern, errmsg);
                return false;
            }
        );
        if (false == globOk || matched.empty()) {
            continue;
        }
        refilelist.insert(
            refilelist.end(),
            std::make_move_iterator(matched.begin()),
            std::make_move_iterator(matched.end())
        );
    }
    // 去重: 多 pattern 可能匹配到相同路径
    std::sort(refilelist.begin(), refilelist.end());
    refilelist.erase(std::unique(refilelist.begin(), refilelist.end()), refilelist.end());
    // 仅保留普通文件: glob 模式可能匹配到目录 (如 `**/*`), 在此过滤
    refilelist.erase(
        std::remove_if(
            refilelist.begin(),
            refilelist.end(),
            [](const std::filesystem::path& p) {
                std::error_code ec;
                return false == std::filesystem::is_regular_file(p, ec);
            }
        ),
        refilelist.end()
    );

    if (refilelist.empty()) {
        throw std::runtime_error{"No match `file_patterns` file found"};
    }

    bool isContentMode = ("content" == output_mode);
    auto resultStr     = std::ostringstream{};

    // ---- 行索引辅助: 每文件只扫描一次构建"行起始偏移索引", 行号查询二分 O(log n)

    /// 构建行起始偏移索引 (0-based): lineStarts[i] 为第 i 行的起始字节偏移
    auto buildLineStarts = [](std::string_view text) -> std::vector<size_t> {
        std::vector<size_t> lineStarts;
        lineStarts.reserve(text.size() / 40 + 1);
        lineStarts.push_back(0);
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\n') {
                lineStarts.push_back(i + 1);
            }
        }
        return lineStarts;
    };

    /// 计算 pos 所在的行号 (0-based)
    auto lineNumberOf = [](const std::vector<size_t>& lineStarts, size_t pos) -> size_t {
        auto it = std::upper_bound(lineStarts.begin(), lineStarts.end(), pos);
        return static_cast<size_t>(it - lineStarts.begin()) - 1;
    };

    /// 提取 text 中第 lineIdx 行 (0-based) 的内容 (不含末尾换行符)
    auto extractLine
        = [](std::string_view text, const std::vector<size_t>& lineStarts, size_t lineIdx
          ) -> std::string_view {
        if (lineIdx >= lineStarts.size()) {
            return {};
        }
        const size_t start = lineStarts[lineIdx];
        // lineStarts[i+1] 为下一行行首 (本行 `\n` 后一字节), 减 1 排除换行符
        const size_t end
            = (lineIdx + 1 < lineStarts.size()) ? lineStarts[lineIdx + 1] - 1 : text.size();
        return text.substr(start, end - start);
    };

    /// 计算文件总行数
    auto totalLines = [](std::string_view text) -> size_t {
        size_t count = 1; // 至少 1 行 (即使空文件)
        for (char c : text) {
            if (c == '\n') {
                ++count;
            }
        }
        // 如果文件以 \n 结尾, 最后不算额外一行
        if (!text.empty() && text.back() == '\n') {
            --count;
        }
        return count;
    };

    /// 加载并预处理文本文件: 跳过二进制文件 (含 NUL 字节), 非 UTF-8 编码
    /// (GBK 等) 转换为 UTF-8, 转换失败视为非文本跳过。额外统一 CRLF -> LF
    /// (归一化仅作用于本次搜索的内存副本, 不修改原文件)
    auto loadSearchableText = [](const std::string& filepath) -> std::optional<std::string> {
        try {
            auto filetext = detail::readFileContent(filepath);
            // 仅搜索文本文件: 含 NUL 字节视为二进制, 跳过
            if (filetext.find('\0') != std::string::npos) {
                return std::nullopt;
            }
            // 非 UTF-8 编码文本转 UTF-8; 转换失败视为非文本, 跳过
            if (false == agentxx::util::autoConvertToUtf8(filetext)) {
                return std::nullopt;
            }
            detail::normalizeCrlfToLf(filetext);
            return filetext;
        } catch (...) {
            // 单个文件读取失败跳过 (与原实现异常跳过行为一致)
            return std::nullopt;
        }
    };

    /// content 模式输出辅助: 将单个文件的匹配结果以"按文件分组"格式写入输出流。
    /// 格式设计 (减少每行的文件路径重复):
    ///   {filepath}:          <- 组头, 每个文件仅输出一次
    ///   {line}:{content}     <- 匹配行 (对齐 grep -n)
    ///   {line}-{content}     <- 上下文行 (对齐 grep -n -C)
    ///   --                   <- 同一文件内不连续的上下文块之间 (对齐 grep -C)
    auto appendGroupedContent = [&](std::ostringstream&        out,
                                    const std::string&         filepath,
                                    std::string_view           filetext,
                                    const std::vector<size_t>& matchStarts) {
        auto lineStarts = buildLineStarts(filetext);
        auto fileLines  = totalLines(filetext);

        // 收集需要输出的行号 (含上下文), 去重后按行号排序输出
        std::set<size_t> matchLineSet{};   // 匹配行
        std::set<size_t> contextLineSet{}; // 上下文行
        for (size_t mi = 0; mi < matchStarts.size(); ++mi) {
            size_t lineIdx = lineNumberOf(lineStarts, matchStarts[mi]);
            matchLineSet.insert(lineIdx);
            if (contextLines > 0) {
                size_t ctxStart = (lineIdx > static_cast<size_t>(contextLines))
                                      ? lineIdx - static_cast<size_t>(contextLines)
                                      : 0;
                size_t ctxEnd   = std::min(
                    lineIdx + static_cast<size_t>(contextLines),
                    fileLines > 0 ? fileLines - 1 : 0
                );
                for (size_t l = ctxStart; l <= ctxEnd; ++l) {
                    contextLineSet.insert(l);
                }
            }
        }

        // 合并所有需要输出的行, 按行号排序
        std::set<size_t> allLines = contextLineSet;
        allLines.insert(matchLineSet.begin(), matchLineSet.end());

        // 文件路径作为组头仅输出一次, 行前缀不再重复完整路径
        out << filepath << ":\n";
        size_t prevLine = 0;
        bool   first    = true;
        for (size_t lineIdx : allLines) {
            // 对齐 grep -C: 不连续的上下文块之间用 -- 分隔
            if (!first && contextLines > 0 && lineIdx > prevLine + 1) {
                out << "--\n";
            }
            first = false;

            auto lineContent = extractLine(filetext, lineStarts, lineIdx);
            // 匹配行用 `:` 分隔, 上下文行用 `-` 分隔 (对齐 grep -n -C)
            char sep = matchLineSet.count(lineIdx) ? ':' : '-';
            out << (lineIdx + 1) << sep << lineContent << "\n";
            prevLine = lineIdx;
        }
    };

    // 应用 max_count_per_file 限制 (对齐 grep -m) 并按 output_mode 输出。
    // MatchT 为 XXRegexMatchResult / AhoCorasick 匹配结构 (均有 start 成员);
    // 以模板 lambda 实现 (块作用域内不允许 template 函数声明)
    auto emitMatches = [&]<typename MatchT>(
                           const std::string&         filepath,
                           const std::string&         filetext,
                           const std::vector<MatchT>& matchRanges
                       ) -> void {
        // 应用 max_count_per_file 限制 (对齐 grep -m)
        size_t effectiveCount = matchRanges.size();
        if (maxCountPerFile > 0 && static_cast<size_t>(maxCountPerFile) < effectiveCount) {
            effectiveCount = static_cast<size_t>(maxCountPerFile);
        }

        if (!isContentMode) {
            // files_with_matches 模式: 输出 file:match_count
            resultStr << filepath << ":" << effectiveCount << "\n";
        } else {
            // content 模式: 输出匹配整行, 按文件分组格式 (对齐 grep -n);
            // 行索引构建 + 行号查询 O(log n)
            std::vector<size_t> matchStarts;
            matchStarts.reserve(effectiveCount);
            for (size_t mi = 0; mi < effectiveCount; ++mi) {
                matchStarts.push_back(matchRanges[mi].start);
            }
            appendGroupedContent(resultStr, filepath, filetext, matchStarts);
        }
    };

    if (text_patterns_is_regex) {
        // 正则匹配: 大小写不敏感直接由 XXRegex 后端实现
        // (Hyperscan 用 HS_FLAG_CASELESS, std::regex fallback 用 icase)
        auto regex = agentxx::util::XXRegex::createRegex(
            text_patterns,
            agentxx::util::XXRegex::defHSFlags_normal,
            !caseSensitive
        );
        if (!regex) {
            return "[Error] Regex compilation failed";
        }

        for (const auto& item : refilelist) {
            if (checkStop()) {
                return "[Error] Cancelled or timed out";
            }
            auto filepath = detail::toUtf8(item);
            // 读取并预处理: 跳过二进制/非文本文件 (glob 阶段已过滤目录)
            auto filetextOpt = loadSearchableText(filepath);
            if (false == filetextOpt.has_value()) {
                continue;
            }
            auto& filetext = filetextOpt.value();
            auto  matchs   = std::vector<agentxx::util::XXRegexMatchResult>{};
            if (regex->match(filetext, matchs)) {
                emitMatches(filepath, filetext, matchs);
            }
        }
    } else {
        // 文本精确匹配 (对齐 grep -F)
        auto search = agentxx::util::AhoCorasick<char>{text_patterns, !caseSensitive};
        for (const auto& item : refilelist) {
            if (checkStop()) {
                return "[Error] Cancelled or timed out";
            }
            auto filepath    = detail::toUtf8(item);
            auto filetextOpt = loadSearchableText(filepath);
            if (false == filetextOpt.has_value()) {
                continue;
            }
            auto& filetext = filetextOpt.value();
            auto  matchs   = search.search(filetext);
            if (false == matchs.empty()) {
                emitMatches(filepath, filetext, matchs);
            }
        }
    }

    auto str = resultStr.str();
    if (false == str.empty()) {
        return str;
    }
    throw std::runtime_error{fmt::format(
        R"_(Found {} files match `file_patterns`, but no match `text_patterns` file found.)_",
        refilelist.size()
    )};
}

// =====================================================================
// 对外执行体: 与原 lib 工具的 asyncWithTimeout+catchErrorAsync 外层语义一致
// —— 可预期异常 (如无匹配/文件打开失败) 统一转为 "[Error] ..." 错误文本返回,
// 而非向调用方抛出; 保证插件 execute 回调、测试直测两种路径行为一致
// =====================================================================
namespace detail {

/// 异常 → "[Error] ..." 文本包装 (取消类异常不在本层出现: impl 内部仅以
/// 返回值表达取消, 见各执行体的 checkStop 分支)
template<typename Fn>
inline std::string asErrorText(Fn&& fn) {
    try {
        return fn();
    } catch (const std::exception& ex) {
        XX_LOGD("filesystem tool error -> text: {}", ex.what());
        return fmt::format("[Error] {}", ex.what());
    }
}

} // namespace detail

inline std::string fileListExecute(
    const neograph::json& arguments,
    const std::string&    workDir,
    const IsCancelledFn&  isCancelled = nullptr
) {
    return detail::asErrorText([&] {
        return fileListExecuteImpl(arguments, workDir, isCancelled);
    });
}

inline std::string fileReadExecute(
    const neograph::json& arguments,
    const std::string&    workDir,
    const IsCancelledFn&  isCancelled = nullptr
) {
    return detail::asErrorText([&] {
        return fileReadExecuteImpl(arguments, workDir, isCancelled);
    });
}

inline std::string fileWriteExecute(
    const neograph::json& arguments,
    const std::string&    workDir,
    const IsCancelledFn&  isCancelled = nullptr
) {
    return detail::asErrorText([&] {
        return fileWriteExecuteImpl(arguments, workDir, isCancelled);
    });
}

inline std::string fileEditExecute(
    const neograph::json& arguments,
    const std::string&    workDir,
    const IsCancelledFn&  isCancelled = nullptr
) {
    return detail::asErrorText([&] {
        return fileEditExecuteImpl(arguments, workDir, isCancelled);
    });
}

inline std::string fileGlobExecute(
    const neograph::json& arguments,
    const std::string&    workDir,
    const IsCancelledFn&  isCancelled = nullptr
) {
    return detail::asErrorText([&] {
        return fileGlobExecuteImpl(arguments, workDir, isCancelled);
    });
}

inline std::string fileGrepExecute(
    const neograph::json& arguments,
    const std::string&    workDir,
    const IsCancelledFn&  isCancelled = nullptr
) {
    return detail::asErrorText([&] {
        return fileGrepExecuteImpl(arguments, workDir, isCancelled);
    });
}

// =====================================================================
// 协程版执行体
// - 插件入口已统一改用 plugin_kit::blocking_tool + Scheduler::offload
//   卸载到线程池，上述协程路径不再被注册，仅保留于头文件内供测试直调；
//   BOOST_ASIO_HAS_FILE 不可用平台回退到同步实现，行为与 offload 一致
// =====================================================================
namespace detail {

/// stream_file 异步读取完整文件内容 (原始字节; 不做编码转换)
/// - 打开失败抛出异常; 读到 EOF 视为正常结束
inline asio::awaitable<std::string> asyncReadWholeFile(
    const asio::any_io_executor& executor,
    const std::string&           systemCharsetFilePath
) {
    asio::stream_file        stream{executor};
    neograph_asio_error_code errCode;
    stream.open(systemCharsetFilePath, asio::stream_file::read_only, errCode);
    if (false == stream.is_open()) {
        throw std::runtime_error{fmt::format(R"(Can not open file: {})", errCode.message())};
    }
    std::string data;
    co_await asio::async_read(
        stream,
        asio::dynamic_buffer(data),
        asio::transfer_all(),
        asio::redirect_error(asio::use_awaitable, errCode)
    );
    // transfer_all 在文件结束时以 eof 返回, 属预期终止
    if (errCode && errCode != asio::error::eof) {
        throw std::system_error{errCode};
    }
    stream.close();
    co_return data;
}

} // namespace detail

#if defined(BOOST_ASIO_HAS_FILE)

/// agentxx_filesystem_read 执行体协程版 (原 FilesystemReadTextFileTool::execute_async)
/// - line_offset/line_limit 模式经 async_read_until 逐行推进 (保留原始换行符);
///   其余整文件读取; 读取后 autoConvertToUtf8 (保留 crlf 或 \n 原样不转换)
inline asio::awaitable<std::string>
    fileReadExecuteAsyncImpl(const neograph::json& arguments, const std::string& workDir) {
    auto filepath = detail::wsAbs(workDir, arguments.value("path", std::string{}));
    if (filepath.empty()) {
        co_return R"([Error] Arg `path` is empty)";
    }
    auto systemCharsetFilePath = filepath;
    agentxx::util::autoConvertToSystemPath(systemCharsetFilePath);
    auto text_line_offset = arguments.value<int64_t>("line_offset", -1);
    auto text_line_limit  = arguments.value<int64_t>("line_limit", -1);

    auto executor = co_await asio::this_coro::executor;

    if (text_line_offset >= 0 || text_line_limit > 0) {
        // 读取部分文件: 逐行 async_read_until, 跳过偏移行后收集至结果
        asio::stream_file        stream{executor};
        neograph_asio_error_code errCode;
        stream.open(systemCharsetFilePath, asio::stream_file::read_only, errCode);
        if (false == stream.is_open()) {
            throw std::runtime_error{fmt::format(R"(Can not open file: {})", errCode.message())};
        }

        const auto offset = (text_line_offset >= 0) ? static_cast<size_t>(text_line_offset) : 0;
        const auto limit  = (text_line_limit > 0) ? static_cast<size_t>(text_line_limit)
                                                  : std::numeric_limits<size_t>::max();
        std::stringstream result{};
        size_t            lineNum = 0;
        size_t            endLine = offset;
        if (offset < std::numeric_limits<size_t>::max() - limit) {
            // 防止相加溢出回绕
            endLine = offset + limit;
        } else {
            endLine = std::numeric_limits<size_t>::max();
        }

        for (std::string buf; lineNum < endLine; lineNum++) {
            auto readlen = co_await asio::async_read_until(
                stream,
                asio::dynamic_buffer(buf),
                '\n',
                asio::redirect_error(asio::use_awaitable, errCode)
            );

            if (errCode == asio::error::eof) {
                // 文件结束 (末行可能无换行符): readlen 为 EOF 前已读入的字节数
                if (lineNum >= offset) {
                    auto line = std::string_view{buf}.substr(0, readlen);
                    result << line;
                }
                break;
            } else if (errCode) {
                throw std::system_error{errCode};
            }

            if (lineNum >= offset) {
                auto line = std::string_view{buf}.substr(0, readlen);
                result << line;
            }
            buf.erase(0, readlen);
        }

        stream.close();
        if (lineNum == 0 && offset == 0) {
            // 空文件: 第 0 行视为空行, 返回空串而非报错
            co_return "";
        }
        if (lineNum <= offset) {
            // offset 超出文件行数
            throw std::runtime_error{fmt::format(
                R"(Arg `line_offset`({} lines) is out of range of file lines({} lines).)",
                offset,
                lineNum
            )};
        }

        auto rawStr = result.str();
        // 保留原始的 crlf 或 \n 换行符不转换
        agentxx::util::autoConvertToUtf8(rawStr);
        co_return rawStr;
    }

    // 读取完整文件
    auto data = co_await detail::asyncReadWholeFile(executor, systemCharsetFilePath);
    // 保留原始的 crlf 或 \n 换行符不转换
    agentxx::util::autoConvertToUtf8(data);
    co_return data;
}

/// agentxx_filesystem_write 执行体协程版 (原 FilesystemWriteFileTool::execute_async)
/// - overwrite=false 且目标存在时报错; 自动创建缺失的父目录;
///   stream_file create|truncate 打开后 async_write 全量写入
inline asio::awaitable<std::string>
    fileWriteExecuteAsyncImpl(const neograph::json& arguments, const std::string& workDir) {
    auto filepath = detail::wsAbs(workDir, arguments.value("path", std::string{}));
    if (filepath.empty()) {
        co_return R"([Error] Arg `path` is empty)";
    }
    auto systemCharsetFilePath = filepath;
    agentxx::util::autoConvertToSystemPath(systemCharsetFilePath);
    auto content   = arguments.value<std::string>("content", std::string{});
    auto overwrite = arguments.value<bool>("overwrite", false);

    // 存在性检查与父目录创建: 快速元数据操作, 与原实现一致内联执行
    auto path = std::filesystem::path(filepath);
    if (false == overwrite && std::filesystem::exists(path)) {
        throw std::runtime_error{"File already exist. Set `overwrite` = true if want to overwrite."
        };
    }
    if (false == std::filesystem::exists(path.parent_path())
        && false == std::filesystem::create_directories(path.parent_path())) {
        throw std::runtime_error{fmt::format(
            R"(Can not create `path`({})'s parent dirs.)",
            detail::toUtf8(path.parent_path())
        )};
    }

    auto executor = co_await asio::this_coro::executor;

    asio::stream_file        stream{executor};
    neograph_asio_error_code errCode;
    stream.open(
        systemCharsetFilePath,
        asio::stream_file::write_only | asio::stream_file::create | asio::stream_file::truncate,
        errCode
    );
    if (false == stream.is_open()) {
        throw std::runtime_error{fmt::format(R"(Can not open file: {})", errCode.message())};
    }

    if (false == content.empty()) {
        // 写入文本内容
        co_await asio::async_write(
            stream,
            asio::buffer(content),
            asio::redirect_error(asio::use_awaitable, errCode)
        );
        if (errCode) {
            throw std::system_error{errCode};
        }
    }

    stream.close();
    co_return "success";
}

/// agentxx_filesystem_edit 执行体协程版 (原 FilesystemEditTextFileTool::execute_async)
/// - 异步读完整文件 → UTF-8/LF 归一化 → 替换 → 原子写 (同目录临时文件 +
///   rename 覆盖), 与同步版行为一致
inline asio::awaitable<std::string>
    fileEditExecuteAsyncImpl(const neograph::json& arguments, const std::string& workDir) {
    auto filepath = detail::wsAbs(workDir, arguments.value("path", std::string{}));
    if (filepath.empty()) {
        co_return "[Error] Arg `path` is empty";
    }
    auto systemCharsetFilePath = filepath;
    agentxx::util::autoConvertToSystemPath(systemCharsetFilePath);
    auto old_str = arguments.value<std::string>("old_str", std::string{});
    if (old_str.empty()) {
        co_return "[Error] Arg `old_str` is empty";
    }
    auto new_str       = arguments.value<std::string>("new_str", std::string{});
    auto multi_replace = arguments.value<bool>("multi_replace", false);

    if (new_str == old_str) {
        co_return "[Error] Arg `old_str` and `new_str` are equal and unchanged.";
    }

    // 统一到 \n 换行符
    // - 与 filesystem_read 的逻辑不同，read 应当保留原始的内容，edit 应当尽可能保证修改成功，
    //   如果 llm 需要写回 crlf，可使用 shell
    detail::normalizeCrlfToLf(old_str);
    detail::normalizeCrlfToLf(new_str);

    if (false == std::filesystem::exists(std::filesystem::path(filepath))) {
        throw std::runtime_error{"File not exist"};
    }

    // 异步读取完整文件并预处理 (先转 UTF-8 使 GBK 等编码文件可正常匹配, 再统一换行符)
    auto        executor = co_await asio::this_coro::executor;
    std::string content  = co_await detail::asyncReadWholeFile(executor, systemCharsetFilePath);
    agentxx::util::autoConvertToUtf8(content);
    detail::normalizeCrlfToLf(content);

    int    replaceHit = 0;
    size_t pos        = 0;
    while ((pos = content.find(old_str, pos)) != std::string::npos) {
        replaceHit++;
        content.replace(pos, old_str.length(), new_str);
        // 跳过新字符串，避免死循环
        pos += new_str.length();
        if (false == multi_replace) {
            break;
        }
    }

    if (0 == replaceHit) {
        throw std::runtime_error{
            R"(No match `old_str` found, Try re-reading to get the latest file content.)"
        };
    }

    // 原子写: 先写同目录临时文件, 成功后 rename 覆盖原文件,
    // 避免直接 truncate 原文件后写入中途失败导致原内容永久丢失
    // (注: 计数器仅保证进程内唯一性, 多实例共享无害, 不属于实例状态)
    static std::atomic<uint64_t> s_editTmpSeq{0};
    const auto                   tmpPath
        = systemCharsetFilePath + fmt::format(".agentxx_edit_tmp_{}", s_editTmpSeq.fetch_add(1));

    {
        asio::stream_file        stream{executor};
        neograph_asio_error_code errCode;
        stream.open(
            tmpPath,
            asio::stream_file::write_only | asio::stream_file::create | asio::stream_file::truncate,
            errCode
        );
        if (false == stream.is_open()) {
            throw std::runtime_error{
                fmt::format(R"(Can not open temp file to write: {})", errCode.message())
            };
        }
        co_await asio::async_write(
            stream,
            asio::buffer(content),
            asio::redirect_error(asio::use_awaitable, errCode)
        );
        stream.close();
        if (errCode) {
            std::error_code rmEc;
            std::filesystem::remove(tmpPath, rmEc);
            throw std::system_error{errCode};
        }
    }

    std::error_code renameEc;
    std::filesystem::rename(tmpPath, systemCharsetFilePath, renameEc);
    if (renameEc) {
        std::error_code rmEc;
        std::filesystem::remove(tmpPath, rmEc);
        throw std::runtime_error{
            fmt::format(R"(Failed to replace original file: {})", renameEc.message())
        };
    }

    if (multi_replace) {
        co_return fmt::format(R"(Success, Replace {} hits)", replaceHit);
    }
    co_return "success";
}

#else // !BOOST_ASIO_HAS_FILE

/// 文件异步 I/O 不可用平台 (无 io_uring/iocp 文件支持): 回退同步实现。
/// 注册侧检测同一宏, 会改走 sync 垫片 (offload 池线程) 注册, 本回退仅供
/// 测试等直调场景保持单一入口
inline asio::awaitable<std::string>
    fileReadExecuteAsyncImpl(const neograph::json& arguments, const std::string& workDir) {
    co_return fileReadExecuteImpl(arguments, workDir);
}

inline asio::awaitable<std::string>
    fileWriteExecuteAsyncImpl(const neograph::json& arguments, const std::string& workDir) {
    co_return fileWriteExecuteImpl(arguments, workDir);
}

inline asio::awaitable<std::string>
    fileEditExecuteAsyncImpl(const neograph::json& arguments, const std::string& workDir) {
    co_return fileEditExecuteImpl(arguments, workDir);
}

#endif // BOOST_ASIO_HAS_FILE

/// 对外协程执行体: 与同步版 *Execute 外层语义一致 —— 可预期异常统一转为
/// "[Error] ..." 错误文本返回, 保证 polled 寄生驱动路径与测试直测行为一致
/// (单文件读写为短操作不轮询取消, 故不设 isCancelled 形参)
/// - 注意: 本包装自身必须是协程 (而非返回惰性协程的普通函数) —— 参数引用在
///   协程帧内存续, 若经普通函数中转临时 lambda 会因栈帧提前返回而悬垂
///   (ASan stack-use-after-return 已复现)
inline asio::awaitable<std::string>
    fileReadExecuteAsync(const neograph::json& arguments, const std::string& workDir) {
    try {
#if defined(BOOST_ASIO_HAS_FILE)
        co_return co_await fileReadExecuteAsyncImpl(arguments, workDir);
#else
        co_return fileReadExecuteImpl(arguments, workDir);
#endif
    } catch (const std::exception& ex) {
        XX_LOGD("filesystem tool error -> text: {}", ex.what());
        co_return fmt::format("[Error] {}", ex.what());
    }
}

inline asio::awaitable<std::string>
    fileWriteExecuteAsync(const neograph::json& arguments, const std::string& workDir) {
    try {
#if defined(BOOST_ASIO_HAS_FILE)
        co_return co_await fileWriteExecuteAsyncImpl(arguments, workDir);
#else
        co_return fileWriteExecuteImpl(arguments, workDir);
#endif
    } catch (const std::exception& ex) {
        XX_LOGD("filesystem tool error -> text: {}", ex.what());
        co_return fmt::format("[Error] {}", ex.what());
    }
}

inline asio::awaitable<std::string>
    fileEditExecuteAsync(const neograph::json& arguments, const std::string& workDir) {
    try {
#if defined(BOOST_ASIO_HAS_FILE)
        co_return co_await fileEditExecuteAsyncImpl(arguments, workDir);
#else
        co_return fileEditExecuteImpl(arguments, workDir);
#endif
    } catch (const std::exception& ex) {
        XX_LOGD("filesystem tool error -> text: {}", ex.what());
        co_return fmt::format("[Error] {}", ex.what());
    }
}

} // namespace agentxx_fs_plugin
