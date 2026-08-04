#include "agentxx/tools/filesystem.h"

#include "agentxx/util/aho_corasick.h"
#include "agentxx/util/async_offload.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/log.h"
#include "agentxx/util/regex.h"
#include "agentxx/util/string_util.h"
#include "asio/as_tuple.hpp"
#include "asio/co_spawn.hpp"
#include "asio/deferred.hpp"
#include "asio/random_access_file.hpp"
#include "asio/read.hpp"
#include "asio/read_at.hpp"
#include "asio/read_until.hpp"
#include "asio/redirect_error.hpp"
#include "asio/registered_buffer.hpp"
#include "asio/steady_timer.hpp"
#include "asio/stream_file.hpp"
#include "asio/use_awaitable.hpp"
#include "asio/write.hpp"
#include "fmt/format.h"
#include "glob/glob.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace agentxx {
namespace tools {

namespace {

/// 判断 glob 模式是否包含递归段 `**`。
/// 与 shell globstar / `find -r` 对齐: 只有显式写出 `**` 时才递归遍历子目录,
/// 否则 `*.txt` 这类模式只匹配当前目录 (不递归)。
bool globPatternHasRecursiveSegment(std::string_view pattern) {
    return pattern.find("**") != std::string_view::npos;
}

/// 提取 glob 模式中第一个通配符 (`*` `?` `[`) 之前的固定目录前缀。
/// 用于计算 max_depth 的基准目录, 以及 include_hidden 自实现遍历的起点。
/// 例如 `/a/b/*.txt` -> `/a/b/`; `*.txt` -> `.`; `/a/b/c.txt` -> `/a/b`。
std::filesystem::path globStaticPrefix(std::string_view pattern) {
    auto pos = pattern.find_first_of("*?[");
    if (pos == std::string_view::npos) {
        // 无通配符: 基准为其父目录
        return std::filesystem::path{std::string{pattern}}.parent_path();
    }
    auto prefix = pattern.substr(0, pos);
    auto slash  = prefix.find_last_of('/');
    if (slash == std::string_view::npos) {
        return std::filesystem::path{"."};
    }
    return std::filesystem::path{std::string{prefix.substr(0, slash + 1)}};
}

/// 计算相对路径的目录深度 (段数)。`.` 与空段不计, `..` 计 1。
int pathDepth(const std::filesystem::path& rel) {
    int depth = 0;
    for (auto seg = rel.begin(); seg != rel.end(); ++seg) {
        if (*seg != "." && !seg->empty()) {
            depth++;
        }
    }
    return depth;
}

/// 将 glob 通配模式转换为等价正则表达式字符串 (用于 exclude 过滤与 include_hidden 匹配)。
/// 语义从宽: `*` 与 `**` 均匹配任意字符 (含路径分隔符 `/`), `?` 匹配单个字符,
/// `[...]` 字符类原样传递 (`[!...]` 转为正则 `[^...]`), 其余正则特殊字符转义。
std::string globToRegexStr(std::string_view pattern) {
    std::string re;
    re.reserve(pattern.size() * 2 + 4);
    re             += '^';
    const size_t n  = pattern.size();
    for (size_t i = 0; i < n; ++i) {
        char c = pattern[i];
        if (c == '*') {
            re += ".*";
            // 吞掉连续的 `*` (含 `**`)
            while (i + 1 < n && pattern[i + 1] == '*') {
                i++;
            }
        } else if (c == '?') {
            re += '.';
        } else if (c == '[') {
            // 找到配对的 `]`, 字符类整体传递给正则
            size_t j = i + 1;
            if (j < n && (pattern[j] == '!' || pattern[j] == '^')) {
                j++;
            }
            if (j < n && pattern[j] == ']') {
                j++;
            }
            while (j < n && pattern[j] != ']') {
                j++;
            }
            if (j >= n) {
                // 无配对 `]`, 按字面量转义
                re += "\\[";
            } else {
                std::string cls{pattern.substr(i, j - i + 1)};
                // glob 的 `[!...]` 转正则的 `[^...]`
                if (cls.size() > 1 && cls[1] == '!') {
                    cls[1] = '^';
                }
                re += cls;
                i   = j;
            }
        } else {
            // 转义正则特殊字符
            static const std::string special = R"(\.^$+(){}|)";
            if (special.find(c) != std::string::npos) {
                re += '\\';
            }
            re += c;
        }
    }
    re += '$';
    return re;
}

/// 将模式中顶层 (不在字符类 `[]` 内、且未被 `\` 转义) 的 ASCII 字母折叠为 `[xX]` 字符类,
/// 实现大小写不敏感匹配。用于:
///  - glob 模式 (glob 库本身大小写敏感, 无内置忽略大小写选项)
///  - grep 正则 (兼容 hyperscan 与 std::regex fallback, 避免依赖 HS_FLAG_CASELESS,
///    因为 std::regex fallback 实现会忽略 flags 参数)
std::string asciiCaseFoldPattern(std::string_view pattern) {
    std::string out;
    out.reserve(pattern.size() * 2);
    bool inClass = false; // 是否处于字符类 [] 内
    for (size_t i = 0; i < pattern.size(); ++i) {
        char c = pattern[i];
        if (c == '\\' && i + 1 < pattern.size()) {
            // 转义序列原样保留
            out += c;
            out += pattern[++i];
            continue;
        }
        if (c == '[') {
            inClass  = true;
            out     += c;
            continue;
        }
        if (c == ']' && inClass) {
            inClass  = false;
            out     += c;
            continue;
        }
        if (!inClass && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
            auto lower  = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            auto upper  = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            out        += '[';
            out        += lower;
            out        += upper;
            out        += ']';
        } else {
            out += c;
        }
    }
    return out;
}

/// 将文本中的 CRLF (`\r\n`) 规范化为 LF (`\n`), 返回规范化的副本。
/// 用于生成 edit 匹配候选变体 (LF 形式)。
std::string crlfToLfCopy(std::string_view text) {
    if (text.find("\r\n") == std::string::npos) {
        return std::string{text};
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
    return out;
}

/// 将文本中的 CRLF (`\r\n`) 行尾统一规范化为 LF (`\n`)。
/// 仅在文本已被转换为 UTF-8 后调用, 避免影响其它编码 (如 UTF-16) 下的原始字节序列。
void normalizeCrlfToLf(std::string& text) {
    if (text.find("\r\n") == std::string::npos) {
        return;
    }
    auto normalized = crlfToLfCopy(text);
    text.swap(normalized);
}

/// 将文本中的 LF (`\n`) 行尾统一转换为 CRLF (`\r\n`)。
/// 已存在的 CRLF 保持原样 (不会产生 `\r\r\n`)。
[[maybe_unused]] std::string lfToCrlf(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) {
            // 仅当 `\n` 前一个字符不是 `\r` 时才补 `\r`, 避免 `\r\n` -> `\r\r\n`
            out += '\r';
        }
        out += text[i];
    }
    return out;
}

/// 解析 `type` 参数为类型集合。支持 string 或 array 两种形式。
/// 返回空集合表示 "any" (不按类型过滤)。合法值: file / dir / symlink / other / any。
std::set<std::string> collectTypeFilter(const neograph::json& typeArg) {
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

/// 获取路径实体的类型字符串 (与 filesystem_list 的 type 字段语义一致)。
/// 先判 symlink 以准确识别符号链接 (即使其指向目录)。
std::string fileTypeOf(const std::filesystem::path& path) {
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
std::vector<std::regex> compileExcludeRegexes(const std::vector<std::string>& excludePatterns) {
    std::vector<std::regex> regexes;
    regexes.reserve(excludePatterns.size());
    for (const auto& ep : excludePatterns) {
        try {
            regexes.emplace_back(globToRegexStr(ep));
        } catch (const std::exception&) {
            // 非法模式忽略
        }
    }
    return regexes;
}

/// 判断路径 (generic_string) 是否命中任一 exclude 正则。
bool isExcluded(const std::string& pathStr, const std::vector<std::regex>& excludeRegexes) {
    for (const auto& re : excludeRegexes) {
        if (std::regex_match(pathStr, re)) {
            return true;
        }
    }
    return false;
}

} // namespace

std::optional<std::string> _defFileReadGenerateKey(const neograph::json& args) {
    if (!args.is_object() || !args["path"].is_string()) {
        return std::nullopt;
    }
    auto path        = args["path"].get<std::string>();
    auto line_offset = args.value<int64_t>("line_offset", -1);
    auto line_limit  = args.value<int64_t>("line_limit", -1);
    auto byte_offset = args.value<int64_t>("byte_offset", -1);
    auto byte_limit  = args.value<int64_t>("byte_limit", -1);
    auto recursive   = args.value<bool>("recursive", false);
    auto limit       = args.value<int64_t>("limit", 100);
    return fmt::format(
        "filesystem:{}:lo={}:ll={}:bo={}:bl={}:r={}:l={}",
        path,
        line_offset,
        line_limit,
        byte_offset,
        byte_limit,
        recursive,
        limit
    );
}

std::optional<std::string> _defFileWriteGenerateKey(const neograph::json& args) {
    if (args.is_object() && args["path"].is_string()) {
        return fmt::format("filesystem:{}", args["path"].get<std::string>());
    }
    return std::nullopt;
}

void _defTruncateToolcallRequest(neograph::ToolCall& toolcall) {
    toolcall.arguments = R"({"tip":"[Outdated Message Truncated]"})";
}

void _defTruncateToolcallResponse(neograph::ChatMessage& msg) {
    msg.content  = "[Outdated Content truncated]";
    msg.flags   |= neograph::MessageFlag::ShareStoreTruncated | neograph::MessageFlag::Outdated;
}

FileSystemListTool::FileSystemListTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    XXToolBase("filesystem_list", in_agentContext, false, false) {}

neograph::ChatTool FileSystemListTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
                       {"type", "object"},
                       {
                "properties",
                {
                    {
                        "path",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("path")},
                        },
                    },
                    {
                        "recursive",
                        {
                            {"type", "boolean"},
                            {"description", prompt.getArg("recursive")},
                        },
                    },
                    {
                        "limit",
                        {
                            {"type", "number"},
                            {"description", prompt.getArg("limit")},
                        },
                    },
                    {
                        "timeout",
                        {
                            {"type", "number"},
                            {"description", prompt.getArg("timeout")},
                        },
                    },
                },
            }, {"required", neograph::json::array({"path"})},
                       },
    };
}

std::optional<agentxx::middleware::SummarizationToolHandle>
    FileSystemListTool::createSummarizationToolHandle() const {
    return agentxx::middleware::SummarizationToolHandle{
        .generateDeduplicationKey = _defFileReadGenerateKey,
        .truncateRequest          = nullptr,
        .truncateResponse         = _defTruncateToolcallResponse,
    };
}

asio::awaitable<std::string> FileSystemListTool::execute_async(const neograph::json& arguments) {
    auto targetPath
        = agentxx::util::toCurrentSystemStandardPath(arguments.value("path", std::string{}));
    if (targetPath.empty()) {
        co_return R"({"error":"Arg `path` is empty"})";
    }
    auto recursive = arguments.value("recursive", false);
    auto limit     = arguments.value<int64_t>("limit", 100);
    auto timeout   = static_cast<int64_t>(arguments.value<double>("timeout", 120.0));

    // 获取阻塞操作卸载线程池, 避免 std::filesystem 同步调用阻塞 io_context 事件循环
    auto  agentPtr = agentContext.lock();
    auto& pool     = *agentPtr->blockingPool;

    // 外部提供 cancelFlag, 超时时由定时器设置, 通知工作线程提前退出
    auto cancelFlag = std::make_shared<std::atomic<bool>>(false);

    // 将所有 std::filesystem 阻塞操作卸载到线程池, 支持取消传播:
    // 当父协程被 CancelToken 取消或超时时, cancelFlag 被置 true, 工作线程检测后提前退出释放线程
    auto workFuture = agentxx::util::offloadCancellableAsync<neograph::json>(
        pool,
        cancelFlag,
        [targetPath, recursive, limit](std::atomic<bool>& cancelFlag
        ) -> asio::awaitable<neograph::json> {
            auto result = neograph::json::array();

            auto onAppendItem = [&](const std::filesystem::directory_entry& entity) {
                try {
                    auto file_time = entity.last_write_time();

                    auto sys_time
                        = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                            file_time - std::filesystem::file_time_type::clock::now()
                            + std::chrono::system_clock::now()
                        );

                    // 提取 Unix 秒数
                    auto unixtime
                        = static_cast<size_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                                  sys_time.time_since_epoch()
                        )
                                                  .count());
                    auto json = neograph::json{
                        {"path", entity.path().generic_string()},
                        {"type",
                         (entity.is_directory()      ? "dir"
                          : entity.is_regular_file() ? "file"
                          : entity.is_symlink()      ? "symlink"
                                                     : "other")},
                        {"last_write_timestamp", unixtime},
                        {"last_write_time", std::format("{:%Y-%m-%d %H:%M}", sys_time)},
                    };
                    if (entity.is_regular_file()) {
                        json["size"] = static_cast<size_t>(entity.file_size());
                    }
                    result.push_back(json);
                } catch (const std::exception& e) {
                    result.push_back(neograph::json{
                        {"path",  entity.path().generic_string()},
                        {"error", e.what()                      },
                    });
                }
            };

            if (false == std::filesystem::exists(targetPath)) {
                result.push_back(neograph::json{
                    {"error", "Path not exist"}
                });
            } else if (std::filesystem::is_directory(targetPath)) {
                if (recursive) {
                    for (const auto& entity :
                         std::filesystem::recursive_directory_iterator(targetPath)) {
                        // 检查取消标志, 提前退出释放线程
                        if (cancelFlag.load(std::memory_order_acquire)) {
                            throw neograph::graph::CancelledException("filesystem_list cancelled");
                        }
                        onAppendItem(entity);
                        if (limit > 0 && static_cast<int64_t>(result.size()) >= limit) {
                            break;
                        }
                    }
                } else {
                    for (const auto& entity : std::filesystem::directory_iterator(targetPath)) {
                        // 检查取消标志, 提前退出释放线程
                        if (cancelFlag.load(std::memory_order_acquire)) {
                            throw neograph::graph::CancelledException("filesystem_list cancelled");
                        }
                        onAppendItem(entity);
                        if (limit > 0 && static_cast<int64_t>(result.size()) >= limit) {
                            break;
                        }
                    }
                }
            } else if (std::filesystem::is_regular_file(targetPath)) {
                onAppendItem(std::filesystem::directory_entry(targetPath));
            } else {
                result.push_back(neograph::json{
                    {"error", "Path exist, but is not a directory or file"}
                });
            }

            co_return result;
        }
    );

    if (timeout > 0) {
        co_return co_await agentxx::util::asyncWithTimeout<std::string>(
            [&]() -> asio::awaitable<std::string> {
                auto result = co_await std::move(workFuture);
                co_return result.dump();
            },
            std::chrono::seconds{timeout},
            [&]() {
                cancelFlag->store(true, std::memory_order_release);
                return fmt::format(
                    R"([Error] Timed out after {} seconds. Try narrowing the path or setting a limit.)",
                    timeout
                );
            }
        );
    } else {
        auto result = co_await std::move(workFuture);
        co_return result.dump();
    }
}

FilesystemReadTextFileTool::FilesystemReadTextFileTool(
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    XXToolBase("filesystem_read_text_file", in_agentContext, false, false) {}

neograph::ChatTool FilesystemReadTextFileTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
                       {"type", "object"},
                       {
                "properties",
                {
                    {
                        "path",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("path")},
                        },
                    },
                    {
                        "line_offset",
                        {
                            {"type", "number"},
                            {"description", prompt.getArg("line_offset")},
                        },
                    },
                    {
                        "line_limit",
                        {
                            {"type", "number"},
                            {"description", prompt.getArg("line_limit")},
                        },
                    },
                },
            }, {"required", neograph::json::array({"path"})},
                       },
    };
}

std::optional<agentxx::middleware::SummarizationToolHandle>
    FilesystemReadTextFileTool::createSummarizationToolHandle() const {
    return agentxx::middleware::SummarizationToolHandle{
        .generateDeduplicationKey = _defFileReadGenerateKey,
        .truncateRequest          = nullptr,
        .truncateResponse         = _defTruncateToolcallResponse,
    };
}

asio::awaitable<std::string>
    FilesystemReadTextFileTool::execute_async(const neograph::json& arguments) {
    auto filepath
        = agentxx::util::toCurrentSystemStandardPath(arguments.value("path", std::string{}));
    if (filepath.empty()) {
        co_return R"({"error":"Arg `path` is empty"})";
    }
    auto systemCharsetFilePath = filepath;
    agentxx::util::autoConvertToSystemPath(systemCharsetFilePath);
    auto text_line_offset = arguments.value<int64_t>("line_offset", -1);
    auto text_line_limit  = arguments.value<int64_t>("line_limit", -1);

#if ASIO_HAS_FILE || BOOST_ASIO_HAS_FILE
    {
        auto currentIoCtx = co_await asio::this_coro::executor;

        /// 异步读取文件
        asio::stream_file        stream{currentIoCtx};
        neograph_asio_error_code errCode;
        stream.open(systemCharsetFilePath, asio::stream_file::read_only, errCode);
        if (false == stream.is_open()) {
            throw std::runtime_error{fmt::format(R"(Can not open file: {}")", errCode.message())};
        }

        if (text_line_offset >= 0 || text_line_limit > 0) {
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
                    if (lineNum >= offset) {
                        result << buf;
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
            if (lineNum <= offset) {
                // offset 超出文件行数
                throw std::runtime_error{fmt::format(
                    R"(Arg `line_offset`({} lines) is out of range of file lines({} lines).)",
                    offset,
                    lineNum
                )};
            }

            auto rawStr = result.str();
            if (agentxx::util::autoConvertToUtf8(rawStr)) {
                // 保留原始的 crlf 或 \n 换行符不转换
                co_return rawStr;
            }
            co_return rawStr;
        }

        // 读取完整文件
        std::string data;
        co_await asio::async_read(
            stream,
            asio::dynamic_buffer(data),
            asio::transfer_all(),
            asio::redirect_error(asio::use_awaitable, errCode)
        );
        if (errCode && errCode != asio::error::eof) {
            throw std::system_error{errCode};
        }
        stream.close();
        if (agentxx::util::autoConvertToUtf8(data)) {
            // 保留原始的 crlf 或 \n 换行符不转换
            co_return data;
        }
        co_return data;
    }
#endif

    {
        /// 同步阻塞读取文件
        std::ifstream stream;
        stream.open(systemCharsetFilePath);
        if (!stream) {
            auto ec = std::error_code{errno, std::system_category()};
            throw std::runtime_error{fmt::format(R"(Can not open file. Error: {})", ec.message())};
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
            if (lineNum <= offset) {
                // offset 超出文件行数
                throw std::runtime_error{fmt::format(
                    R"(Arg `line_offset`({} lines) is out of range of file lines({} lines).)",
                    offset,
                    lineNum
                )};
            }

            auto rawStr = result.str();
            if (agentxx::util::autoConvertToUtf8(rawStr)) {
                // 保留原始的 crlf 或 \n 换行符不转换
                co_return rawStr;
            }
            co_return rawStr;
        }

        // 读取完整文件
        auto result
            = std::string{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        stream.close();
        if (agentxx::util::autoConvertToUtf8(result)) {
            // 保留原始的 crlf 或 \n 换行符不转换
            co_return result;
        }
        co_return result;
    }
}

FilesystemReadBinaryFileTool::FilesystemReadBinaryFileTool(
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    XXToolBase("filesystem_read_binary_file", in_agentContext, false, false) {}

neograph::ChatTool FilesystemReadBinaryFileTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
                       {"type", "object"},
                       {
                "properties",
                {
                    {
                        "path",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("path")},
                        },
                    },
                    {
                        "byte_offset",
                        {
                            {"type", "number"},
                            {"description", prompt.getArg("byte_offset")},
                        },
                    },
                    {
                        "byte_limit",
                        {
                            {"type", "number"},
                            {"description", prompt.getArg("byte_limit")},
                        },
                    },
                },
            }, {"required", neograph::json::array({"path"})},
                       },
    };
}

std::optional<agentxx::middleware::SummarizationToolHandle>
    FilesystemReadBinaryFileTool::createSummarizationToolHandle() const {
    return agentxx::middleware::SummarizationToolHandle{
        .generateDeduplicationKey = _defFileReadGenerateKey,
        .truncateRequest          = nullptr,
        .truncateResponse         = _defTruncateToolcallResponse,
    };
}

asio::awaitable<std::string>
    FilesystemReadBinaryFileTool::execute_async(const neograph::json& arguments) {
    auto filepath
        = agentxx::util::toCurrentSystemStandardPath(arguments.value("path", std::string{}));
    if (filepath.empty()) {
        co_return R"({"error":"Arg `path` is empty"})";
    }
    auto systemCharsetFilePath = filepath;
    agentxx::util::autoConvertToSystemPath(systemCharsetFilePath);
    auto byte_offset = arguments.value<int64_t>("byte_offset", -1);
    auto byte_limit  = arguments.value<int64_t>("byte_limit", -1);

#if ASIO_HAS_FILE || BOOST_ASIO_HAS_FILE
    {
        auto currentIoCtx = co_await asio::this_coro::executor;

        /// 异步读取文件
        if (byte_offset >= 0 || byte_limit >= 0) {
            asio::random_access_file stream{currentIoCtx};
            neograph_asio_error_code errCode;
            stream.open(systemCharsetFilePath, asio::random_access_file::read_only, errCode);
            if (false == stream.is_open()) {
                throw std::runtime_error{fmt::format(R"(Can not open file: {}")", errCode.message())
                };
            }

            // 读取部分文件
            const size_t offset = (byte_offset >= 0) ? static_cast<size_t>(byte_offset) : 0;
            const size_t limit  = (byte_limit >= 0) ? static_cast<size_t>(byte_limit)
                                                    : std::numeric_limits<size_t>::max();

            auto fileSize       = stream.size();
            auto bytesAvailable = std::max((int64_t)fileSize - (int64_t)offset, (int64_t)0);
            auto bytesRead      = std::min(
                static_cast<std::streamsize>(limit),
                static_cast<std::streamsize>(bytesAvailable)
            );

            // 没有数据可读
            if (bytesRead <= 0) {
                throw std::runtime_error{fmt::format(
                    R"(Arg `byte_offset`({}) is out of range of file size({}).)",
                    offset,
                    static_cast<size_t>(fileSize)
                )};
            }

            std::string result;
            result.resize(bytesRead);
            auto bytesReadLen = co_await asio::async_read_at(
                stream,
                offset,
                asio::buffer(result, bytesRead),
                asio::redirect_error(asio::use_awaitable, errCode)
            );
            if (errCode && errCode != asio::error::eof) {
                throw std::system_error{errCode};
            }
            stream.close();
            co_return neograph::json{
                {"bytes_read_len", bytesReadLen                                                 },
                {
                 "base64_data",    agentxx::util::base64Encode(std::string_view{result}.substr(0, bytesReadLen)),
                 },
            }
                .dump();
        }

        // 读取完整文件
        asio::stream_file        stream{currentIoCtx};
        neograph_asio_error_code errCode;
        stream.open(systemCharsetFilePath, asio::stream_file::read_only, errCode);
        if (false == stream.is_open()) {
            throw std::runtime_error{fmt::format(R"(Can not open file: {}")", errCode.message())};
        }

        std::string result;
        auto        bytesReadLen = co_await asio::async_read(
            stream,
            asio::dynamic_buffer(result),
            asio::transfer_all(),
            asio::redirect_error(asio::use_awaitable, errCode)
        );
        if (errCode && errCode != asio::error::eof) {
            throw std::system_error{errCode};
        }
        stream.close();
        auto readRange = std::string_view{result}.substr(0, bytesReadLen);
        co_return neograph::json{
            {"bytes_read_len", bytesReadLen},
            {
             "base64_data", agentxx::util::base64Encode(readRange),
             },
        }
            .dump();
    }
#endif

    {
        /// 同步读取
        std::ifstream stream;
        stream.open(systemCharsetFilePath, std::ios::binary);
        if (!stream) {
            auto ec = std::error_code{errno, std::system_category()};
            throw std::runtime_error{fmt::format(R"(Can not open file. Error: {})", ec.message())};
        }

        if (byte_offset >= 0 || byte_limit >= 0) {
            // 读取部分文件
            const size_t offset = (byte_offset >= 0) ? static_cast<size_t>(byte_offset) : 0;
            const size_t limit  = (byte_limit >= 0) ? static_cast<size_t>(byte_limit)
                                                    : std::numeric_limits<size_t>::max();

            // 计算实际需要读取的字节数
            size_t fileSize       = static_cast<size_t>(std::filesystem::file_size(filepath));
            auto   bytesAvailable = std::max((int64_t)fileSize - (int64_t)offset, (int64_t)0);
            auto   bytesRead      = std::min(
                static_cast<std::streamsize>(limit),
                static_cast<std::streamsize>(bytesAvailable)
            );

            // 没有数据可读
            if (bytesRead <= 0) {
                throw std::runtime_error{fmt::format(
                    R"(Arg `byte_offset`({}) is out of range of file size({}).)",
                    offset,
                    fileSize
                )};
            }

            stream.seekg(offset, std::ios::beg);
            if (!stream.good()) {
                auto ec = std::error_code{errno, std::system_category()};
                throw std::runtime_error{
                    fmt::format(R"(Read offset {} bytes failed. Error: {})", offset, ec.message())
                };
            }

            std::string result;
            result.resize(bytesRead);
            stream.read(result.data(), bytesRead);
            std::streamsize realBytesRead = stream.gcount();

            stream.close();
            co_return neograph::json{
                {"bytes_read_len", realBytesRead                      },
                {"base64_data",    agentxx::util::base64Encode(result)},
            }
                .dump();
        }

        // 读取完整文件
        auto result = std::string(
            (std::istreambuf_iterator<char>(stream)),
            std::istreambuf_iterator<char>()
        );
        auto bytesReadLen = result.size();
        stream.close();
        co_return neograph::json{
            {"bytes_read_len", bytesReadLen                       },
            {"base64_data",    agentxx::util::base64Encode(result)},
        }
            .dump();
    }
}

FilesystemWriteFileTool::FilesystemWriteFileTool(
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    XXToolBase("filesystem_write_file", in_agentContext, false, false) {}

neograph::ChatTool FilesystemWriteFileTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
                       {"type", "object"},
                       {
                "properties",
                {
                    {
                        "path",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("path")},
                        },
                    },
                    {
                        "content",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("content")},
                        },
                    },
                    {
                        "overwrite",
                        {
                            {"type", "boolean"},
                            {"description", prompt.getArg("overwrite")},
                        },
                    },
                    {
                        "is_binary",
                        {
                            {"type", "boolean"},
                            {"description", prompt.getArg("is_binary")},
                        },
                    },
                },
            }, {"required", neograph::json::array({"path"})},
                       },
    };
}

std::optional<agentxx::middleware::SummarizationToolHandle>
    FilesystemWriteFileTool::createSummarizationToolHandle() const {
    return agentxx::middleware::SummarizationToolHandle{
        .generateDeduplicationKey = _defFileWriteGenerateKey,
        .truncateRequest          = _defTruncateToolcallRequest,
        .truncateResponse         = nullptr,
    };
}

asio::awaitable<std::string> FilesystemWriteFileTool::execute_async(const neograph::json& arguments
) {
    auto filepath
        = agentxx::util::toCurrentSystemStandardPath(arguments.value("path", std::string{}));
    if (filepath.empty()) {
        co_return R"({"error":"Arg `path` is empty"})";
    }
    auto systemCharsetFilePath = filepath;
    agentxx::util::autoConvertToSystemPath(systemCharsetFilePath);
    auto content   = arguments.value<std::string>("content", std::string{});
    auto overwrite = arguments.value<bool>("overwrite", false);
    auto is_binary = arguments.value<bool>("is_binary", false);

#if ASIO_HAS_FILE || BOOST_ASIO_HAS_FILE
    {
        auto currentIoCtx = co_await asio::this_coro::executor;

        // 读取完整文件
        asio::stream_file stream{currentIoCtx};
        auto              path = std::filesystem::path(filepath);
        if (false == overwrite && std::filesystem::exists(path)) {
            throw std::runtime_error{
                "File already exist. Set `overwrite` = true if want to overwrite."
            };
        }
        if (false == std::filesystem::exists(path.parent_path())
            && false == std::filesystem::create_directories(path.parent_path())) {
            // 创建父目录
            throw std::runtime_error{fmt::format(
                R"(Can not create `path`({})'s parent dirs.)",
                path.parent_path().generic_string()
            )};
        }

        neograph_asio_error_code errCode;
        stream.open(
            systemCharsetFilePath,
            asio::stream_file::write_only | asio::stream_file::create | asio::stream_file::truncate,
            errCode
        );
        if (false == stream.is_open()) {
            throw std::runtime_error{fmt::format(R"(Can not open file: {}")", errCode.message())};
        }

        if (false == content.empty()) {
            // 写入文件内容
            if (is_binary) {
                auto result = agentxx::util::base64Decode(content);
                if (!result.has_value()) {
                    throw std::runtime_error{"base64 decode failed"};
                }
                co_await asio::async_write(
                    stream,
                    asio::buffer(result.value()),
                    asio::redirect_error(asio::use_awaitable, errCode)
                );
            } else {
                co_await asio::async_write(
                    stream,
                    asio::buffer(content),
                    asio::redirect_error(asio::use_awaitable, errCode)
                );
            }
            if (errCode) {
                throw std::system_error{errCode};
            }
        }

        stream.close();
        co_return "success";
    }
#endif

    {
        std::ofstream stream;
        auto          path = std::filesystem::path(filepath);
        if (false == overwrite && std::filesystem::exists(path)) {
            throw std::runtime_error{
                "File already exist. Set `overwrite` = true if want to overwrite."
            };
        }
        if (false == std::filesystem::exists(path.parent_path())
            && false == std::filesystem::create_directories(path.parent_path())) {
            // 创建父目录
            throw std::runtime_error{fmt::format(
                R"(Can not create `path`({})'s parent dirs.)",
                path.parent_path().generic_string()
            )};
        }

        stream.open(
            systemCharsetFilePath,
            is_binary ? std::ios_base::out | std::ios_base::binary | std::ios_base::trunc
                      : std::ios_base::out | std::ios_base::trunc
        );
        if (!stream) {
            auto ec = std::error_code{errno, std::system_category()};
            throw std::runtime_error{
                fmt::format(R"(Can not create or open file. Error: {})", ec.message())
            };
        }

        if (false == content.empty()) {
            // 写入文件内容
            if (is_binary) {
                auto result = agentxx::util::base64Decode(content);
                if (!result.has_value()) {
                    throw std::runtime_error{"base64 decode failed"};
                }
                stream << result.value();
            } else {
                stream << content;
            }
            if (!stream) {
                auto ec = std::error_code{errno, std::system_category()};
                throw std::runtime_error{fmt::format(
                    R"(File created success, but write failed. Error: {})",
                    ec.message()
                )};
            }
        }

        stream.close();
        co_return "success";
    }
}

FilesystemEditTextFileTool::FilesystemEditTextFileTool(
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    XXToolBase("filesystem_edit_text_file", in_agentContext, false, false) {}

neograph::ChatTool FilesystemEditTextFileTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
                       {"type", "object"},
                       {
                "properties",
                {
                    {
                        "path",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("path")},
                        },
                    },
                    {
                        "old_str",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("old_str")},
                        },
                    },
                    {
                        "new_str",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("new_str")},
                        },
                    },
                    {
                        "multi_replace",
                        {
                            {"type", "boolean"},
                            {"description", prompt.getArg("multi_replace")},
                        },
                    },

                },
            }, {"required", neograph::json::array({"path", "old_str", "new_str"})},
                       },
    };
}

std::optional<agentxx::middleware::SummarizationToolHandle>
    FilesystemEditTextFileTool::createSummarizationToolHandle() const {
    return agentxx::middleware::SummarizationToolHandle{
        .generateDeduplicationKey = _defFileWriteGenerateKey,
        .truncateRequest          = _defTruncateToolcallRequest,
        .truncateResponse         = nullptr,
    };
}

asio::awaitable<std::string>
    FilesystemEditTextFileTool::execute_async(const neograph::json& arguments) {
    auto filepath
        = agentxx::util::toCurrentSystemStandardPath(arguments.value("path", std::string{}));
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

    // 统一到 \n 换行符
    // - 与 filesystem_read 的逻辑不同，read 应当保留原始的内容，edit 应当尽可能保证修改成功，如果
    // llm 需要写回 crlf，可使用 shell
    normalizeCrlfToLf(old_str);
    normalizeCrlfToLf(new_str);

#if ASIO_HAS_FILE || BOOST_ASIO_HAS_FILE
    {
        auto currentIoCtx = co_await asio::this_coro::executor;

        // 读取完整文件
        asio::stream_file stream{currentIoCtx};
        auto              path = std::filesystem::path(filepath);
        if (false == std::filesystem::exists(path)) {
            throw std::runtime_error{"File not exist"};
        }

        neograph_asio_error_code errCode;
        stream.open(systemCharsetFilePath, asio::stream_file::read_only, errCode);
        if (false == stream.is_open()) {
            throw std::runtime_error{fmt::format("Can not open file: {}", errCode.message())};
        }

        std::string content;
        // 读出文件
        co_await asio::async_read(
            stream,
            asio::dynamic_buffer(content),
            asio::transfer_all(),
            asio::redirect_error(asio::use_awaitable, errCode)
        );
        if (errCode && errCode != asio::error::eof) {
            throw std::system_error{errCode};
        }
        stream.close();
        normalizeCrlfToLf(content);

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
        const auto                   tmpPath = systemCharsetFilePath
                             + fmt::format(".agentxx_edit_tmp_{}", s_editTmpSeq.fetch_add(1));
        stream.open(
            tmpPath,
            asio::stream_file::write_only | asio::stream_file::create | asio::stream_file::truncate,
            errCode
        );
        if (false == stream.is_open()) {
            throw std::runtime_error{
                fmt::format(R"(Can not open temp file to write: {}")", errCode.message())
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
        } else {
            co_return "success";
        }
    }
#endif

    {
        std::fstream stream;
        auto         path = std::filesystem::path(filepath);
        if (false == std::filesystem::exists(path)) {
            throw std::runtime_error{"File not exist"};
        }

        stream.open(systemCharsetFilePath, std::ios_base::in);
        if (!stream) {
            auto ec = std::error_code{errno, std::system_category()};
            throw std::runtime_error{fmt::format("Can not open file. Error: {}", ec.message())};
        }

        std::ostringstream output;
        output << stream.rdbuf();
        std::string content = output.str();
        normalizeCrlfToLf(content);

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

        // 写入文件内容
        stream.close();
        stream.open(systemCharsetFilePath, std::ios_base::out);
        if (!stream) {
            auto ec = std::error_code{errno, std::system_category()};
            throw std::runtime_error{
                fmt::format("Can not open file to write. Error: {}", ec.message())
            };
        }
        stream << content;
        if (!stream) {
            auto ec = std::error_code{errno, std::system_category()};
            throw std::runtime_error{fmt::format("Edit file failed. Error: {}", ec.message())};
        }

        stream.close();
        if (multi_replace) {
            co_return fmt::format("Success, Replace {} hits", replaceHit);
        } else {
            co_return "success";
        }
    }
}

FilesystemGlobTool::FilesystemGlobTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    XXToolBase("filesystem_glob", in_agentContext, false, false) {}

neograph::ChatTool FilesystemGlobTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
                       {"type", "object"},
                       {
                "properties",
                {
                    {
                        "file_patterns",
                        {
                            {"type", "array"},
                            {"items", {{"type", "string"}}},
                            {"description", prompt.getArg("file_patterns")},
                        },
                    },
                    {
                        "type",
                        {
                            {"type", {"string", "array"}},
                            {"items", {{"type", "string"}}},
                            {"description", prompt.getArg("type")},
                        },
                    },
                    {
                        "exclude_patterns",
                        {
                            {"type", "array"},
                            {"items", {{"type", "string"}}},
                            {"description", prompt.getArg("exclude_patterns")},
                        },
                    },
                    {
                        "case_sensitive",
                        {
                            {"type", "boolean"},
                            {"description", prompt.getArg("case_sensitive")},
                        },
                    },
                    {
                        "max_depth",
                        {
                            {"type", "number"},
                            {"description", prompt.getArg("max_depth")},
                        },
                    },
                    {
                        "sort",
                        {
                            {"type", "boolean"},
                            {"description", prompt.getArg("sort")},
                        },
                    },
                    {
                        "timeout",
                        {
                            {"type", "number"},
                            {"description", prompt.getArg("timeout")},
                        },
                    },
                },
            }, {"required", neograph::json::array({"file_patterns"})},
                       },
    };
}

asio::awaitable<std::string> FilesystemGlobTool::execute_async(const neograph::json& arguments) {
    auto file_patterns = arguments.value("file_patterns", std::vector<std::string>{});
    if (file_patterns.empty()) {
        co_return R"({"error":"Arg `file_patterns` is empty"})";
    }
    auto timeout = static_cast<int64_t>(arguments.value<double>("timeout", 120.0));

    for (auto& item : file_patterns) {
        item = agentxx::util::toCurrentSystemStandardPath(item);
    }

    // 解析可选参数
    auto caseSensitive   = arguments.value<bool>("case_sensitive", true);
    auto maxDepth        = arguments.value<int64_t>("max_depth", -1);
    auto doSort          = arguments.value<bool>("sort", false);
    auto typeFilter      = collectTypeFilter(arguments.value("type", neograph::json{}));
    auto excludePatterns = arguments.value("exclude_patterns", std::vector<std::string>{});

    // 大小写不敏感时, 将 glob 模式中的字母折叠为 [xX] 字符类
    // (glob 库本身大小写敏感, 无内置忽略大小写选项)
    if (!caseSensitive) {
        for (auto& item : file_patterns) {
            item = asciiCaseFoldPattern(item);
        }
    }

    // 获取阻塞操作卸载线程池, 避免 glob 同步调用阻塞 io_context 事件循环
    auto  agentPtr = agentContext.lock();
    auto& pool     = *agentPtr->blockingPool;

    // 外部提供 cancelFlag, 超时时由定时器设置, 通知工作线程提前退出
    auto cancelFlag = std::make_shared<std::atomic<bool>>(false);

    // 将 glob 阻塞操作卸载到线程池, 支持取消传播:
    // 当父协程被 CancelToken 取消或超时时, cancelFlag 被置 true, 工作线程检测后提前退出释放线程
    auto workFuture = agentxx::util::offloadCancellableAsync<std::string>(
        pool,
        cancelFlag,
        [file_patterns, typeFilter, excludePatterns, maxDepth, doSort](std::atomic<bool>& cancelFlag
        ) -> asio::awaitable<std::string> {
            if (cancelFlag.load(std::memory_order_acquire)) {
                throw neograph::graph::CancelledException("filesystem_glob cancelled");
            }

            // 智能选择 glob/rglob: 含 `**` 的模式使用 rglob (递归), 否则使用 glob (仅当前目录)
            // 对齐 shell globstar 行为: `*.txt` 只匹配当前目录, `**/*.txt` 才递归
            std::vector<std::filesystem::path> resultList;
            for (const auto& pattern : file_patterns) {
                if (cancelFlag.load(std::memory_order_acquire)) {
                    throw neograph::graph::CancelledException("filesystem_glob cancelled");
                }
                if (globPatternHasRecursiveSegment(pattern)) {
                    auto matched = glob::rglob(pattern, cancelFlag);
                    resultList.insert(
                        resultList.end(),
                        std::make_move_iterator(matched.begin()),
                        std::make_move_iterator(matched.end())
                    );
                } else {
                    auto matched = glob::glob(pattern, cancelFlag);
                    resultList.insert(
                        resultList.end(),
                        std::make_move_iterator(matched.begin()),
                        std::make_move_iterator(matched.end())
                    );
                }
            }

            if (resultList.empty()) {
                co_return R"({"error":"No match `file_patterns` found"})";
            }

            // 默认去重: 多 pattern 可能匹配到相同路径
            std::sort(resultList.begin(), resultList.end());
            resultList.erase(std::unique(resultList.begin(), resultList.end()), resultList.end());

            // 类型过滤 (对齐 find -type): file / dir / symlink / other / any
            if (!typeFilter.empty()) {
                std::vector<std::filesystem::path> filtered;
                filtered.reserve(resultList.size());
                for (const auto& p : resultList) {
                    if (typeFilter.count(fileTypeOf(p))) {
                        filtered.push_back(p);
                    }
                }
                resultList = std::move(filtered);
            }

            // exclude_patterns 过滤: 将 glob 模式转为正则, 匹配到的路径被排除
            if (!excludePatterns.empty()) {
                auto excludeRegexes = compileExcludeRegexes(excludePatterns);
                if (!excludeRegexes.empty()) {
                    std::vector<std::filesystem::path> filtered;
                    filtered.reserve(resultList.size());
                    for (const auto& p : resultList) {
                        if (!isExcluded(p.generic_string(), excludeRegexes)) {
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
                        auto            baseDir = globStaticPrefix(pattern);
                        std::error_code ec;
                        auto            rel = std::filesystem::relative(p, baseDir, ec);
                        if (ec) {
                            continue;
                        }
                        if (pathDepth(rel) <= static_cast<int>(maxDepth)) {
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
                co_return R"({"error":"No match `file_patterns` found after filtering"})";
            }

            auto oss = std::ostringstream{};
            for (const auto& item : resultList) {
                if (cancelFlag.load(std::memory_order_acquire)) {
                    throw neograph::graph::CancelledException("filesystem_glob cancelled");
                }
                oss << item.generic_string() << '\n';
            }
            co_return oss.str();
        }
    );

    if (timeout > 0) {
        co_return co_await agentxx::util::asyncWithTimeout<std::string>(
            [&]() -> asio::awaitable<std::string> {
                co_return co_await std::move(workFuture);
            },
            std::chrono::seconds{timeout},
            [&]() {
                cancelFlag->store(true, std::memory_order_release);
                return fmt::format(
                    R"([Error] Timed out after {} seconds. Try narrowing the file_patterns.)",
                    timeout
                );
            }
        );
    } else {
        co_return co_await std::move(workFuture);
    }
}

FilesystemGrepTool::FilesystemGrepTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    XXToolBase("filesystem_grep", in_agentContext, false, false) {}

neograph::ChatTool FilesystemGrepTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
                       {"type", "object"},
                       {
                "properties",
                {
                    {
                        "text_patterns_is_regex",
                        {
                            {"type", "boolean"},
                            {"description", prompt.getArg("text_patterns_is_regex")},
                        },
                    },
                    {
                        "text_patterns",
                        {
                            {"type", "array"},
                            {"items", {{"type", "string"}}},
                            {"description", prompt.getArg("text_patterns")},
                        },
                    },
                    {
                        "file_patterns",
                        {
                            {"type", "array"},
                            {"items", {{"type", "string"}}},
                            {"description", prompt.getArg("file_patterns")},
                        },
                    },
                    {
                        "output_mode",
                        {
                            {"type", "string"},
                            {"enum", neograph::json::array({"files_with_matches", "content"})},
                            {"description", prompt.getArg("output_mode")},
                        },
                    },
                    {
                        "case_sensitive",
                        {
                            {"type", "boolean"},
                            {"description", prompt.getArg("case_sensitive")},
                        },
                    },
                    {
                        "max_count_per_file",
                        {
                            {"type", "number"},
                            {"description", prompt.getArg("max_count_per_file")},
                        },
                    },
                    {
                        "context_lines",
                        {
                            {"type", "number"},
                            {"description", prompt.getArg("context_lines")},
                        },
                    },
                    {
                        "timeout",
                        {
                            {"type", "number"},
                            {"description", prompt.getArg("timeout")},
                        },
                    },
                },
            }, {
                "required",
                neograph::json::array({
                    "text_patterns_is_regex",
                    "text_patterns",
                    "file_patterns",
                }),
            }, },
    };
}

asio::awaitable<std::string> FilesystemGrepTool::readFileContent(std::string_view filepath) {
    auto systemCharsetFilePath = agentxx::util::toCurrentSystemStandardPath(filepath);
    agentxx::util::autoConvertToSystemPath(systemCharsetFilePath);

#if ASIO_HAS_FILE || BOOST_ASIO_HAS_FILE
    {
        auto currentIoCtx = co_await asio::this_coro::executor;

        /// 异步读取文件
        asio::stream_file        stream{currentIoCtx};
        neograph_asio_error_code errCode;
        stream.open(systemCharsetFilePath, asio::stream_file::read_only, errCode);
        if (false == stream.is_open()) {
            throw std::runtime_error{
                fmt::format(R"(Can not open file. Error: {})", errCode.message())
            };
        }

        // 读取完整文件
        std::string data;
        co_await asio::async_read(
            stream,
            asio::dynamic_buffer(data),
            asio::transfer_all(),
            asio::redirect_error(asio::use_awaitable, errCode)
        );
        if (errCode && errCode != asio::error::eof) {
            throw std::system_error{errCode};
        }
        stream.close();
        co_return data;
    }
#endif

    {
        /// 同步阻塞读取文件
        std::ifstream stream;
        stream.open(systemCharsetFilePath);
        if (!stream) {
            auto ec = std::error_code{errno, std::system_category()};
            throw std::runtime_error{fmt::format(R"(Can not open file. Error: {})", ec.message())};
        }

        // 读取完整文件
        auto result
            = std::string{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        stream.close();
        co_return result;
    }
}

asio::awaitable<std::string> FilesystemGrepTool::execute_async(const neograph::json& arguments) {
    auto text_patterns_is_regex = arguments.value("text_patterns_is_regex", true);
    auto text_patterns          = arguments.value("text_patterns", std::vector<std::string>{});
    if (text_patterns.empty()) {
        co_return R"({"error":"Arg `text_patterns` is empty"})";
    }
    auto file_patterns = arguments.value("file_patterns", std::vector<std::string>{});
    if (file_patterns.empty()) {
        co_return R"({"error":"Arg `file_patterns` is empty"})";
    }
    for (auto& item : file_patterns) {
        item = agentxx::util::toCurrentSystemStandardPath(item);
    }
    auto output_mode = arguments.value("output_mode", std::string{"files_with_matches"});
    if (output_mode.empty()) {
        co_return R"({"error":"Arg `output_mode` is empty"})";
    }
    auto timeout = static_cast<int64_t>(arguments.value<double>("timeout", 120.0));

    // 新增参数 (对齐 Linux grep 行为)
    auto caseSensitive   = arguments.value<bool>("case_sensitive", true);
    auto maxCountPerFile = arguments.value<int64_t>("max_count_per_file", 0); // 0 = 不限
    auto contextLines    = arguments.value<int64_t>("context_lines", 0);

    // 外部提供 cancelFlag, 超时时由定时器设置, 通知 glob 工作线程提前退出
    auto cancelFlag = std::make_shared<std::atomic<bool>>(false);

    // 将整个 grep 操作 (glob + 逐文件搜索) 包装为子协程, 与超时定时器竞争。
    // 注意: 必须用"具名 lambda + 调用", 不能写"临时 lambda 立即调用 ([...](){...}())"。
    // 因为 asio::awaitable 是惰性协程, 调用时不执行协程体, 其协程帧通过隐式对象指针引用
    // lambda 闭包 (捕获项 this/cancelFlag/...); 若 lambda 是临时对象, 会在本语句结束即析构,
    // 待下方 co_await 真正恢复协程时捕获项已失效 -> stack-use-after-return。
    // 具名 lambda work 作为本协程局部变量, 存活至 execute_async 结束, 覆盖整个 co_await 周期。
    auto work = [this,
                 file_patterns,
                 text_patterns,
                 text_patterns_is_regex,
                 output_mode,
                 cancelFlag,
                 caseSensitive,
                 maxCountPerFile,
                 contextLines]() -> asio::awaitable<std::string> {
        std::vector<std::filesystem::path> refilelist{};
        {
            auto  agentPtr = agentContext.lock();
            auto& pool     = *agentPtr->blockingPool;

            // 将 glob 阻塞操作卸载到线程池, 支持取消传播:
            // 当父协程被 CancelToken 取消或超时时, cancelFlag 被置 true, 工作线程检测后提前退出
            auto relist = co_await agentxx::util::offloadCancellableAsync<
                std::vector<std::filesystem::path>>(
                pool,
                cancelFlag,
                [file_patterns](std::atomic<bool>& cancelFlag
                ) -> asio::awaitable<std::vector<std::filesystem::path>> {
                    // 智能选择 glob/rglob: 含 `**` 的模式使用 rglob (递归), 否则使用 glob
                    // (仅当前目录)
                    std::vector<std::filesystem::path> resultList;
                    for (const auto& pattern : file_patterns) {
                        if (cancelFlag.load(std::memory_order_acquire)) {
                            throw neograph::graph::CancelledException("filesystem_grep cancelled");
                        }
                        if (globPatternHasRecursiveSegment(pattern)) {
                            auto matched = glob::rglob(pattern, cancelFlag);
                            resultList.insert(
                                resultList.end(),
                                std::make_move_iterator(matched.begin()),
                                std::make_move_iterator(matched.end())
                            );
                        } else {
                            auto matched = glob::glob(pattern, cancelFlag);
                            resultList.insert(
                                resultList.end(),
                                std::make_move_iterator(matched.begin()),
                                std::make_move_iterator(matched.end())
                            );
                        }
                    }
                    // 去重: 多 pattern 可能匹配到相同路径
                    std::sort(resultList.begin(), resultList.end());
                    resultList.erase(
                        std::unique(resultList.begin(), resultList.end()),
                        resultList.end()
                    );
                    co_return resultList;
                }
            );
            refilelist.insert(
                refilelist.end(),
                std::make_move_iterator(relist.begin()),
                std::make_move_iterator(relist.end())
            );
        }
        if (refilelist.empty()) {
            throw std::runtime_error{"No match `file_patterns` file found"};
        }

        bool isContentMode = ("content" == output_mode);
        auto resultStr     = std::ostringstream{};

        /// 辅助: 计算 text[0..pos) 中的换行符数量, 即 pos 所在的行号 (0-based)
        auto lineNumberOf = [](std::string_view text, size_t pos) -> size_t {
            size_t count = 0;
            for (size_t i = 0; i < pos && i < text.size(); ++i) {
                if (text[i] == '\n') {
                    ++count;
                }
            }
            return count;
        };

        /// 辅助: 提取 text 中第 lineIdx 行 (0-based) 的内容 (不含末尾换行符)
        auto extractLine = [](std::string_view text, size_t lineIdx) -> std::string_view {
            size_t start   = 0;
            size_t curLine = 0;
            // 找到第 lineIdx 行的起始位置
            while (curLine < lineIdx && start < text.size()) {
                if (text[start] == '\n') {
                    ++curLine;
                }
                ++start;
            }
            if (start >= text.size()) {
                return {};
            }
            // 找到行尾
            size_t end = text.find('\n', start);
            if (end == std::string_view::npos) {
                end = text.size();
            }
            return text.substr(start, end - start);
        };

        /// 辅助: 计算文件总行数
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

        if (text_patterns_is_regex) {
            // 正则匹配
            std::shared_ptr<agentxx::util::XXRegex> regex;
            if (caseSensitive) {
                regex = agentxx::util::XXRegex::createRegex(text_patterns);
            } else {
                // 大小写不敏感: 将模式中字母折叠为 [xX] 字符类
                std::vector<std::string> foldedPatterns;
                foldedPatterns.reserve(text_patterns.size());
                for (const auto& p : text_patterns) {
                    foldedPatterns.push_back(asciiCaseFoldPattern(p));
                }
                regex = agentxx::util::XXRegex::createRegex(foldedPatterns);
            }
            if (!regex) {
                co_return "[Error] Regex compilation failed";
            }

            for (const auto& item : refilelist) {
                // 检查取消/超时标志, 提前退出
                if (cancelFlag->load(std::memory_order_acquire)) {
                    throw neograph::graph::CancelledException("[Cancelled]");
                }
                auto filepath = item.generic_string();
                // glob 模式可能匹配到目录 (如 `**/*`), grep 只搜索普通文件
                std::error_code fec;
                if (!std::filesystem::is_regular_file(filepath, fec)) {
                    continue;
                }
                auto filetext = co_await readFileContent(filepath);
                auto matchs   = std::vector<agentxx::util::XXRegexMatchResult>{};
                if (regex->match(filetext, matchs)) {
                    // 应用 max_count_per_file 限制 (对齐 grep -m)
                    size_t effectiveCount = matchs.size();
                    if (maxCountPerFile > 0
                        && static_cast<size_t>(maxCountPerFile) < effectiveCount) {
                        effectiveCount = static_cast<size_t>(maxCountPerFile);
                    }

                    if (!isContentMode) {
                        // files_with_matches 模式: 输出 file:match_count
                        resultStr << filepath << ":" << effectiveCount << "\n";
                    } else {
                        // content 模式: 输出匹配整行, 格式 file:line:content (对齐 grep -n)
                        // 收集需要输出的行号 (含上下文), 去重后按行号排序输出
                        std::set<size_t> matchLineSet;   // 匹配行
                        std::set<size_t> contextLineSet; // 上下文行
                        size_t           fileLines = totalLines(filetext);

                        for (size_t mi = 0; mi < effectiveCount; ++mi) {
                            size_t lineIdx = lineNumberOf(filetext, matchs[mi].start);
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

                        size_t prevLine = 0;
                        bool   first    = true;
                        for (size_t lineIdx : allLines) {
                            // 对齐 grep -C: 不连续的上下文块之间用 -- 分隔
                            if (!first && contextLines > 0 && lineIdx > prevLine + 1) {
                                resultStr << "--\n";
                            }
                            first = false;

                            auto lineContent = extractLine(filetext, lineIdx);
                            // 匹配行用 `:` 分隔, 上下文行用 `-` 分隔 (对齐 grep -n -C)
                            char sep = matchLineSet.count(lineIdx) ? ':' : '-';
                            resultStr << filepath << sep << (lineIdx + 1) << sep << lineContent
                                      << "\n";
                            prevLine = lineIdx;
                        }
                    }
                }
            }
        } else {
            // 文本精确匹配 (对齐 grep -F)
            auto search = agentxx::util::AhoCorasick<char>{text_patterns, !caseSensitive};
            for (const auto& item : refilelist) {
                // 检查取消/超时标志, 提前退出
                if (cancelFlag->load(std::memory_order_acquire)) {
                    throw neograph::graph::CancelledException("filesystem_grep cancelled");
                }
                auto filepath = item.generic_string();
                // glob 模式可能匹配到目录 (如 `**/*`), grep 只搜索普通文件
                std::error_code fec;
                if (!std::filesystem::is_regular_file(filepath, fec)) {
                    continue;
                }
                auto filetext = co_await readFileContent(filepath);
                auto matchs   = search.search(filetext);
                if (false == matchs.empty()) {
                    // 应用 max_count_per_file 限制 (对齐 grep -m)
                    size_t effectiveCount = matchs.size();
                    if (maxCountPerFile > 0
                        && static_cast<size_t>(maxCountPerFile) < effectiveCount) {
                        effectiveCount = static_cast<size_t>(maxCountPerFile);
                    }

                    if (!isContentMode) {
                        // files_with_matches 模式: 输出 file:match_count
                        resultStr << filepath << ":" << effectiveCount << "\n";
                    } else {
                        // content 模式: 输出匹配整行, 格式 file:line:content (对齐 grep -n)
                        std::set<size_t> matchLineSet;
                        std::set<size_t> contextLineSet;
                        size_t           fileLines = totalLines(filetext);

                        for (size_t mi = 0; mi < effectiveCount; ++mi) {
                            size_t lineIdx = lineNumberOf(filetext, matchs[mi].start);
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

                        std::set<size_t> allLines = contextLineSet;
                        allLines.insert(matchLineSet.begin(), matchLineSet.end());

                        size_t prevLine = 0;
                        bool   first    = true;
                        for (size_t lineIdx : allLines) {
                            if (!first && contextLines > 0 && lineIdx > prevLine + 1) {
                                resultStr << "--\n";
                            }
                            first = false;

                            auto lineContent = extractLine(filetext, lineIdx);
                            char sep         = matchLineSet.count(lineIdx) ? ':' : '-';
                            resultStr << filepath << sep << (lineIdx + 1) << sep << lineContent
                                      << "\n";
                            prevLine = lineIdx;
                        }
                    }
                }
            }
        }

        auto str = resultStr.str();
        if (false == str.empty()) {
            co_return str;
        } else {
            throw std::runtime_error{fmt::format(
                R"_(Found {} files match `file_patterns`, but no match `text_patterns` file found.)_",
                refilelist.size()
            )};
        }
    };
    auto workFuture = work();

    if (timeout > 0) {
        co_return co_await agentxx::util::asyncWithTimeout<std::string>(
            [&]() -> asio::awaitable<std::string> {
                // 工作协程异常转为错误字符串返回 (与工具"返回错误文本而非抛异常"的约定一致);
                // 取消类异常 (CancelledException/NodeInterrupt) 由 catchErrorAsync 原样抛出
                return agentxx::util::catchErrorAsync<std::string>(
                    [&]() -> asio::awaitable<std::string> {
                        co_return co_await std::move(workFuture);
                    },
                    [&](std::string errmsg) -> asio::awaitable<std::string> {
                        co_return fmt::format("[Error] {}", errmsg);
                    }
                );
            },
            std::chrono::seconds{timeout},
            [&]() {
                cancelFlag->store(true, std::memory_order_release);
                return fmt::format(
                    R"([Error] Timed out after {} seconds. Try narrowing the file_patterns or text_patterns.)",
                    timeout
                );
            }
        );
    } else {
        co_return co_await std::move(workFuture);
    }
}

} // namespace tools
} // namespace agentxx
