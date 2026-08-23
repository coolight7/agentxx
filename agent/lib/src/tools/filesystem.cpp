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
#include "neograph/graph/cancel.h"
#include <algorithm>
#include <cctype>
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

using agentxx::util::toCurrentSystemAbsolutePath;

namespace {

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

/// 获取路径实体的类型字符串 (与 filesystem_glob 的 type 参数语义一致)。
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
    // 统一使用绝对路径生成 key, 避免同一文件以相对/绝对形式出现时去重失效
    auto path        = toCurrentSystemAbsolutePath(args["path"].get<std::string>());
    auto line_offset = args.value<int64_t>("line_offset", -1);
    auto line_limit  = args.value<int64_t>("line_limit", -1);
    auto recursive   = args.value<bool>("recursive", false);
    auto limit       = args.value<int64_t>("limit", 100);
    return fmt::format(
        "filesystem:{}:lo={}:ll={}:r={}:l={}",
        path,
        line_offset,
        line_limit,
        recursive,
        limit
    );
}

std::optional<std::string> _defFileWriteGenerateKey(const neograph::json& args) {
    if (args.is_object() && args["path"].is_string()) {
        return fmt::format(
            "filesystem:{}",
            toCurrentSystemAbsolutePath(args["path"].get<std::string>())
        );
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
    XXToolBase("agentxx_filesystem_list", in_agentContext, false, false) {}

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
                            {"type", "integer"},
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
    auto targetPath = toCurrentSystemAbsolutePath(arguments.value("path", std::string{}));
    if (targetPath.empty()) {
        co_return R"([Error] Arg `path` is empty)";
    }
    auto recursive = arguments.value("recursive", false);
    auto limit     = arguments.value<int64_t>("limit", 100);
    auto timeout   = static_cast<int64_t>(arguments.value<double>("timeout", 60.0));

    // 获取阻塞操作卸载线程池, 避免 std::filesystem 同步调用阻塞 io_context 事件循环
    auto  agentPtr = agentContext.lock();
    auto& pool     = *agentPtr->threadPool;
    // 会话取消令牌: 取消时由 watcher 置位 cancelFlag, 工作线程提前退出
    auto cancelToken = agentxx::tools::getSessionCancelToken(agentPtr, arguments);

    // 外部提供 cancelFlag, 超时时由定时器设置, 通知工作线程提前退出
    auto cancelFlag = std::make_shared<std::atomic<bool>>(false);

    // 将所有 std::filesystem 阻塞操作卸载到线程池, 支持取消传播:
    // 当会话 CancelToken 取消 (watcher 监听置位) 或超时时, cancelFlag 被置 true,
    // 工作线程检测后提前退出释放线程
    // - 输出格式: 仿 `ls -l` 的多行文本, 每行一个条目:
    //   `类型标识  大小  修改时间  路径`
    auto workFuture = agentxx::util::offloadCancellableAsync<std::string>(
        pool,
        cancelFlag,
        cancelToken,
        [targetPath, recursive, limit](std::atomic<bool>& cancelFlag
        ) -> asio::awaitable<std::string> {
            std::vector<std::string> lines;

            auto onAppendItem = [&](const std::filesystem::directory_entry& entity) {
                // 单个条目处理失败仅记录错误行, 不中断整个列表
                agentxx::util::catchError<bool>(
                    [&]() -> bool {
                        auto file_time = entity.last_write_time();

                        auto sys_time
                            = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                                file_time - std::filesystem::file_time_type::clock::now()
                                + std::chrono::system_clock::now()
                            );

                        // 类型标识列 (仿 ls -l): 首字符为真实类型 (d/-/l/?),
                        // 权限位无数据用通用占位
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
                        auto pathStr = entity.path().generic_string();
                        if (entity.is_directory()) {
                            pathStr += "/";
                        } else if (entity.is_symlink()) {
                            std::error_code ec;
                            auto target = std::filesystem::read_symlink(entity.path(), ec);
                            if (!ec) {
                                pathStr += " -> " + target.generic_string();
                            }
                        }

                        auto timeStr = std::format("{:%Y-%m-%d %H:%M}", sys_time);
                        lines.push_back(
                            fmt::format("{} {:>10}  {}  {}", typeStr, sizeStr, timeStr, pathStr)
                        );
                        return true;
                    },
                    [&](std::string errmsg) -> bool {
                        lines.push_back(
                            fmt::format("[Error] {}: {}", entity.path().generic_string(), errmsg)
                        );
                        return false;
                    }
                );
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
                        // 检查取消标志, 提前退出释放线程
                        if (cancelFlag.load(std::memory_order_acquire)) {
                            throw neograph::graph::CancelledException("filesystem_list cancelled");
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
                        // 检查取消标志, 提前退出释放线程
                        if (cancelFlag.load(std::memory_order_acquire)) {
                            throw neograph::graph::CancelledException("filesystem_list cancelled");
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

            // 空目录: 输出提示行, 让 LLM 能区分 "空目录" 与失败
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
            co_return output;
        }
    );

    if (timeout > 0) {
        co_return co_await agentxx::util::asyncWithTimeout<std::string>(
            [&]() -> asio::awaitable<std::string> {
                auto result = co_await std::move(workFuture);
                co_return result;
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
        co_return result;
    }
}

FilesystemReadTextFileTool::FilesystemReadTextFileTool(
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    XXToolBase("agentxx_filesystem_read", in_agentContext, false, false) {}

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
                            {"type", "integer"},
                            {"description", prompt.getArg("line_offset")},
                        },
                    },
                    {
                        "line_limit",
                        {
                            {"type", "integer"},
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
    auto filepath = toCurrentSystemAbsolutePath(arguments.value("path", std::string{}));
    if (filepath.empty()) {
        co_return R"([Error] Arg `path` is empty)";
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
            throw std::runtime_error{fmt::format(R"(Can not open file: {})", errCode.message())};
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
        // 保留原始的 crlf 或 \n 换行符不转换
        agentxx::util::autoConvertToUtf8(data);
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
        auto result
            = std::string{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        stream.close();
        // 保留原始的 crlf 或 \n 换行符不转换
        agentxx::util::autoConvertToUtf8(result);
        co_return result;
    }
}

FilesystemWriteFileTool::FilesystemWriteFileTool(
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    XXToolBase("agentxx_filesystem_write", in_agentContext, false, false) {}

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
    auto filepath = toCurrentSystemAbsolutePath(arguments.value("path", std::string{}));
    if (filepath.empty()) {
        co_return R"([Error] Arg `path` is empty)";
    }
    auto systemCharsetFilePath = filepath;
    agentxx::util::autoConvertToSystemPath(systemCharsetFilePath);
    auto content   = arguments.value<std::string>("content", std::string{});
    auto overwrite = arguments.value<bool>("overwrite", false);

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

        stream.open(systemCharsetFilePath, std::ios_base::out | std::ios_base::trunc);
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
    XXToolBase("agentxx_filesystem_edit", in_agentContext, false, false) {}

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
    auto filepath = toCurrentSystemAbsolutePath(arguments.value("path", std::string{}));
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
        agentxx::util::autoConvertToUtf8(content);
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
        // 与异步分支行为一致: 先转 UTF-8 (GBK 等编码文件可正常匹配), 再统一换行符
        agentxx::util::autoConvertToUtf8(content);
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
    XXToolBase("agentxx_filesystem_glob", in_agentContext, false, false) {}

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
                            // 注意: 必须声明单一类型。联合类型 ["string","array"] 会被
                            // Gemini 网关拒绝 (其 protobuf Schema 不支持 type 数组, 且带
                            // items 时必须 type==ARRAY, 否则 HTTP 400 INVALID_ARGUMENT);
                            // 工具实现 collectTypeFilter 仍兼容 string/array 两种入参
                            {"type", "string"},
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
                        "max_depth",
                        {
                            {"type", "integer"},
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
        co_return R"([Error] Arg `file_patterns` is empty)";
    }
    auto timeout = static_cast<int64_t>(arguments.value<double>("timeout", 60.0));

    for (auto& item : file_patterns) {
        item = toCurrentSystemAbsolutePath(item);
    }

    // 注: 路径匹配固定为大小写敏感 (移除 case-insensitive 支持)。
    // 历史原因: 曾用 glob::case_fold_pattern 折叠模式以支持忽略大小写, 但折叠后的
    // `[xX]` 字符类会让 glob::static_prefix 在 `[` 处截断前缀, 导致 max_depth 深度
    // 计算基准目录失效 (过滤结果错误), 且 Windows 盘符折叠后无法再识别为绝对路径。
    // 若需忽略大小写匹配, 可先用 filesystem_list 列出候选路径再精确匹配。
    auto maxDepth        = arguments.value<int64_t>("max_depth", -1);
    auto doSort          = arguments.value<bool>("sort", false);
    auto typeFilter      = collectTypeFilter(arguments.value("type", neograph::json{}));
    auto excludePatterns = arguments.value("exclude_patterns", std::vector<std::string>{});
    for (auto& item : excludePatterns) {
        item = toCurrentSystemAbsolutePath(item);
    }

    // 获取阻塞操作卸载线程池, 避免 glob 同步调用阻塞 io_context 事件循环
    auto  agentPtr = agentContext.lock();
    auto& pool     = *agentPtr->threadPool;
    // 会话取消令牌: 取消时由 watcher 置位 cancelFlag, 工作线程提前退出
    auto cancelToken = agentxx::tools::getSessionCancelToken(agentPtr, arguments);

    // 外部提供 cancelFlag, 超时时由定时器设置, 通知工作线程提前退出
    auto cancelFlag = std::make_shared<std::atomic<bool>>(false);

    // 将 glob 阻塞操作卸载到线程池, 支持取消传播:
    // 当会话 CancelToken 取消 (watcher 监听置位) 或超时时, cancelFlag 被置 true,
    // 工作线程检测后提前退出释放线程
    auto workFuture = agentxx::util::offloadCancellableAsync<std::string>(
        pool,
        cancelFlag,
        cancelToken,
        [file_patterns, typeFilter, excludePatterns, maxDepth, doSort](std::atomic<bool>& cancelFlag
        ) -> asio::awaitable<std::string> {
            if (cancelFlag.load(std::memory_order_acquire)) {
                throw neograph::graph::CancelledException("filesystem_glob cancelled");
            }

            // 智能选择 glob/rglob: 含 `**` 的模式使用 rglob (递归), 否则使用 glob (仅当前目录)
            // 对齐 shell globstar 行为: `*.txt` 只匹配当前目录, `**/*.txt` 才递归
            // 路径匹配固定为大小写敏感
            std::vector<std::filesystem::path> resultList;
            for (const auto& pattern : file_patterns) {
                if (cancelFlag.load(std::memory_order_acquire)) {
                    throw neograph::graph::CancelledException("filesystem_glob cancelled");
                }
                if (glob::has_recursive_segment(pattern)) {
                    auto matched = glob::rglob(pattern, true, cancelFlag);
                    resultList.insert(
                        resultList.end(),
                        std::make_move_iterator(matched.begin()),
                        std::make_move_iterator(matched.end())
                    );
                } else {
                    auto matched = glob::glob(pattern, true, cancelFlag);
                    resultList.insert(
                        resultList.end(),
                        std::make_move_iterator(matched.begin()),
                        std::make_move_iterator(matched.end())
                    );
                }
            }

            if (resultList.empty()) {
                co_return R"([Error] No match `file_patterns` found)";
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
                co_return R"([Error] No match `file_patterns` found after filtering)";
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
    XXToolBase("agentxx_filesystem_grep", in_agentContext, false, false) {}

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
                            {"type", "integer"},
                            {"description", prompt.getArg("max_count_per_file")},
                        },
                    },
                    {
                        "context_lines",
                        {
                            {"type", "integer"},
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
        co_return R"([Error] Arg `text_patterns` is empty)";
    }
    auto file_patterns = arguments.value("file_patterns", std::vector<std::string>{});
    if (file_patterns.empty()) {
        co_return R"([Error] Arg `file_patterns` is empty)";
    }
    for (auto& item : file_patterns) {
        item = toCurrentSystemAbsolutePath(item);
    }
    auto output_mode = arguments.value("output_mode", std::string{"files_with_matches"});
    if (output_mode.empty()) {
        co_return R"([Error] Arg `output_mode` is empty)";
    }
    auto timeout = static_cast<int64_t>(arguments.value<double>("timeout", 60.0));

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
                 arguments,
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
            auto& pool     = *agentPtr->threadPool;
            // 会话取消令牌: 取消时由 watcher 置位 cancelFlag, 工作线程提前退出
            auto cancelToken = agentxx::tools::getSessionCancelToken(agentPtr, arguments);

            // 将 glob 阻塞操作卸载到线程池, 支持取消传播:
            // 当会话 CancelToken 取消 (watcher 监听置位) 或超时时, cancelFlag 被置 true,
            // 工作线程检测后提前退出
            auto relist = co_await agentxx::util::offloadCancellableAsync<
                std::vector<std::filesystem::path>>(
                pool,
                cancelFlag,
                cancelToken,
                [file_patterns](std::atomic<bool>& cancelFlag
                ) -> asio::awaitable<std::vector<std::filesystem::path>> {
                    // 智能选择 glob/rglob: 含 `**` 的模式使用 rglob (递归), 否则使用 glob
                    // (仅当前目录); 路径匹配固定为大小写敏感
                    std::vector<std::filesystem::path> resultList;
                    for (const auto& pattern : file_patterns) {
                        if (cancelFlag.load(std::memory_order_acquire)) {
                            throw neograph::graph::CancelledException("filesystem_grep cancelled");
                        }
                        if (glob::has_recursive_segment(pattern)) {
                            auto matched = glob::rglob(pattern, true, cancelFlag);
                            resultList.insert(
                                resultList.end(),
                                std::make_move_iterator(matched.begin()),
                                std::make_move_iterator(matched.end())
                            );
                        } else {
                            auto matched = glob::glob(pattern, true, cancelFlag);
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
                    // 仅保留普通文件: glob 模式可能匹配到目录 (如 `**/*`), 在此过滤
                    // 避免在 io_context 线程上逐个同步 is_regular_file 检查阻塞事件循环
                    resultList.erase(
                        std::remove_if(
                            resultList.begin(),
                            resultList.end(),
                            [](const std::filesystem::path& p) {
                                std::error_code ec;
                                return false == std::filesystem::is_regular_file(p, ec);
                            }
                        ),
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

        // ---- 行索引辅助 (优化): 每文件只扫描一次构建"行起始偏移索引", 行号查询
        // 二分 O(log n), 替代原实现对每个 match 全文本扫描 O(n) 的 lineNumberOf ----

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

        /// 加载并预处理文本文件: 跳过二进制文件 (含 NUL 字节), 非 UTF-8 编码
        /// (GBK 等) 转换为 UTF-8, 转换失败视为非文本跳过。
        /// 额外统一 CRLF -> LF: Windows 文本文件行尾为 \r\n, 若不归一化,
        /// content 模式输出的匹配行会携带行尾 \r (extractLine 仅剥离 \n),
        /// 且正则 `$` 等锚点在 CRLF 下语义漂移; 归一化仅作用于本次搜索的
        /// 内存副本, 不修改原文件
        auto loadSearchableText
            = [&](const std::string& filepath) -> asio::awaitable<std::optional<std::string>> {
            auto filetext = co_await readFileContent(filepath);
            // 仅搜索文本文件: 含 NUL 字节视为二进制, 跳过
            if (filetext.find('\0') != std::string::npos) {
                co_return std::nullopt;
            }
            // 非 UTF-8 编码文本转 UTF-8; 转换失败视为非文本, 跳过
            if (false == agentxx::util::autoConvertToUtf8(filetext)) {
                co_return std::nullopt;
            }
            normalizeCrlfToLf(filetext);
            co_return filetext;
        };

        /// content 模式输出辅助: 将单个文件的匹配结果以"按文件分组"格式写入输出流。
        /// 格式设计 (减少每行的文件路径重复, 旧格式为每行重复完整路径 file:line:content):
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

        if (text_patterns_is_regex) {
            // 正则匹配: 大小写不敏感直接由 XXRegex 后端实现
            // (Hyperscan 用 HS_FLAG_CASELESS, std::regex fallback 用 icase),
            // 不再需要外部改写模式
            auto regex = agentxx::util::XXRegex::createRegex(
                text_patterns,
                agentxx::util::XXRegex::defHSFlags_normal,
                !caseSensitive
            );
            if (!regex) {
                co_return "[Error] Regex compilation failed";
            }

            for (const auto& item : refilelist) {
                // 检查取消/超时标志, 提前退出
                if (cancelFlag->load(std::memory_order_acquire)) {
                    throw neograph::graph::CancelledException("[Cancelled]");
                }
                auto filepath = item.generic_string();
                // 读取并预处理: 跳过二进制/非文本文件 (glob 阶段已过滤目录)
                auto filetextOpt = co_await loadSearchableText(filepath);
                if (false == filetextOpt.has_value()) {
                    continue;
                }
                auto& filetext = filetextOpt.value();
                auto  matchs   = std::vector<agentxx::util::XXRegexMatchResult>{};
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
                        // content 模式: 输出匹配整行, 按文件分组格式 (见 appendGroupedContent,
                        // 对齐 grep -n); 行索引构建 + 行号查询 O(log n)
                        std::vector<size_t> matchStarts;
                        matchStarts.reserve(effectiveCount);
                        for (size_t mi = 0; mi < effectiveCount; ++mi) {
                            matchStarts.push_back(matchs[mi].start);
                        }
                        appendGroupedContent(resultStr, filepath, filetext, matchStarts);
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
                // 读取并预处理: 跳过二进制/非文本文件 (glob 阶段已过滤目录)
                auto filetextOpt = co_await loadSearchableText(filepath);
                if (false == filetextOpt.has_value()) {
                    continue;
                }
                auto& filetext = filetextOpt.value();
                auto  matchs   = search.search(filetext);
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
                        // content 模式: 输出匹配整行, 按文件分组格式 (见 appendGroupedContent,
                        // 对齐 grep -n); 行索引构建 + 行号查询 O(log n)
                        std::vector<size_t> matchStarts;
                        matchStarts.reserve(effectiveCount);
                        for (size_t mi = 0; mi < effectiveCount; ++mi) {
                            matchStarts.push_back(matchs[mi].start);
                        }
                        appendGroupedContent(resultStr, filepath, filetext, matchStarts);
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
