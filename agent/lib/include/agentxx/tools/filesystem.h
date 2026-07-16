#pragma once

#include "agentxx/tools/tool.h"
#include "agentxx/util/aho_corasick.h"
#include "agentxx/util/log.h"
#include "agentxx/util/regex.h"
#include "agentxx/util/string_util.h"
#include "asio/as_tuple.hpp"
#include "asio/random_access_file.hpp"
#include "asio/read.hpp"
#include "asio/read_at.hpp"
#include "asio/read_until.hpp"
#include "asio/redirect_error.hpp"
#include "asio/registered_buffer.hpp"
#include "asio/stream_file.hpp"
#include "asio/use_awaitable.hpp"
#include "asio/write.hpp"
#include "fmt/format.h"
#include "glob/glob.h"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <neograph/llm/rate_limited_provider.h>
#include <neograph/llm/schema_provider.h>
#include <neograph/neograph.h>
#include <sstream>
#include <string>
#include <vector>

namespace asio = ::boost::asio;

namespace agentxx {
namespace tools {

/// 文件读取 tool 的去重 key 生成（包含 path + offset/limit 等参数）
inline std::optional<std::string>
_defFileReadGenerateKey(const neograph::json &args) {
  if (!args.is_object() || !args["path"].is_string()) {
    return std::nullopt;
  }
  auto path = args["path"].get<std::string>();
  auto line_offset = args.value<int64_t>("line_offset", -1);
  auto line_limit = args.value<int64_t>("line_limit", -1);
  auto byte_offset = args.value<double>("byte_offset", -1);
  auto byte_limit = args.value<double>("byte_limit", -1);
  auto recursive = args.value<bool>("recursive", false);
  auto limit = args.value<int64_t>("limit", 100);
  return fmt::format("filesystem:{}:lo={}:ll={}:bo={}:bl={}:r={}:l={}", path,
                     line_offset, line_limit, byte_offset, byte_limit,
                     recursive, limit);
}

/// 文件写入/编辑 tool 的去重 key 生成（仅用 path，最新写入覆盖旧写入）
inline std::optional<std::string>
_defFileWriteGenerateKey(const neograph::json &args) {
  if (args.is_object() && args["path"].is_string()) {
    return fmt::format("filesystem:{}", args["path"].get<std::string>());
  }
  return std::nullopt;
}

inline void _defTruncateToolcallRequest(neograph::ToolCall &toolcall) {
  toolcall.arguments = R"({"tip":"[Outdated Message Truncated]"})";
}

inline void _defTruncateToolcallResponse(neograph::ChatMessage &msg) {
  msg.content = "[Outdated Content truncated]";
  msg.flags |= neograph::MessageFlag::ShareStoreTruncated |
               neograph::MessageFlag::Outdated;
}

/// ls
class FileSystemListTool : public XXToolBase {
public:
  FileSystemListTool(
      std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
      : XXToolBase("filesystem_list", in_agentContext, false, false) {}

  neograph::ChatTool get_definition() const override {
    auto agentPtr = agentContext.lock();
    const auto &prompt = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

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
                },
            },
            {"required", neograph::json::array({"path"})},
        },
    };
  }

  std::optional<agentxx::middleware::SummarizationToolHandle>
  createSummarizationToolHandle() const override {
    return agentxx::middleware::SummarizationToolHandle{
        .generateDeduplicationKey = _defFileReadGenerateKey,
        .truncateRequest = nullptr,
        .truncateResponse = _defTruncateToolcallResponse,
    };
  }

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override {
    auto targetPath =
        agentxx::util::toStandardPath(arguments.value("path", std::string{}));
    if (targetPath.empty()) {
      co_return R"({"error":"Arg `path` is empty"})";
    }
    auto recursive = arguments.value("recursive", false);
    auto limit = arguments.value<int64_t>("limit", 100);

    auto result = neograph::json::array();
    auto onAppendItem = [&](const std::filesystem::directory_entry &entity) {
      auto json = neograph::json{
          {"path", entity.path().generic_string()},
          {"type", (entity.is_directory()      ? "dir"
                    : entity.is_regular_file() ? "file"
                    : entity.is_symlink()      ? "symlink"
                                               : "other")},
          {"last_write_time",
           entity.last_write_time().time_since_epoch().count()},
      };
      if (entity.is_regular_file()) {
        json["size"] = size_t(entity.file_size());
      }
      result.push_back(json);
    };

    if (false == std::filesystem::exists(targetPath)) {
      result.push_back(neograph::json{"error", "Path not exist"});
    } else if (std::filesystem::is_directory(targetPath)) {
      if (recursive) {
        for (const auto &entity :
             std::filesystem::recursive_directory_iterator(targetPath)) {
          onAppendItem(entity);
          if (limit > 0 && static_cast<int64_t>(result.size()) >= limit) {
            break;
          }
        }
      } else {
        for (const auto &entity :
             std::filesystem::directory_iterator(targetPath)) {
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
          "error", "Path exist, but is not a directory or file"});
    }

    co_return result.dump();
  }
};

/// read
class FilesystemReadTextFileTool : public XXToolBase {
protected:
public:
  FilesystemReadTextFileTool(
      std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
      : XXToolBase("filesystem_read_text_file", in_agentContext, false, false) {
  }

  neograph::ChatTool get_definition() const override {
    auto agentPtr = agentContext.lock();
    const auto &prompt = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

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
            },
            {"required", neograph::json::array({"path"})},
        },
    };
  }

  std::optional<agentxx::middleware::SummarizationToolHandle>
  createSummarizationToolHandle() const override {
    return agentxx::middleware::SummarizationToolHandle{
        .generateDeduplicationKey = _defFileReadGenerateKey,
        .truncateRequest = nullptr,
        .truncateResponse = _defTruncateToolcallResponse,
    };
  }

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override {
    auto filepath =
        agentxx::util::toStandardPath(arguments.value("path", std::string{}));
    if (filepath.empty()) {
      co_return R"({"error":"Arg `path` is empty"})";
    }
    auto systemCharsetFilePath = filepath;
    agentxx::util::autoConvertToSystemPath(systemCharsetFilePath);
    auto text_line_offset = arguments.value<int64_t>("line_offset", -1);
    auto text_line_limit = arguments.value<int64_t>("line_limit", -1);

#if ASIO_HAS_FILE || BOOST_ASIO_HAS_FILE
    {
      auto currentIoCtx = co_await asio::this_coro::executor;

      /// 异步读取文件
      asio::stream_file stream{currentIoCtx};
      neograph_asio_error_code errCode;
      stream.open(systemCharsetFilePath, asio::stream_file::read_only, errCode);
      if (false == stream.is_open()) {
        throw std::runtime_error{
            fmt::format(R"(Can not open file: {}")", errCode.message())};
      }

      if (text_line_offset >= 0 || text_line_limit > 0) {
        const auto offset =
            (text_line_offset >= 0) ? size_t(text_line_offset) : 0;
        const auto limit = (text_line_limit > 0)
                               ? size_t(text_line_limit)
                               : std::numeric_limits<size_t>::max();
        std::stringstream result{};
        size_t lineNum = 0;

        for (std::string buf; lineNum < offset + limit; lineNum++) {
          auto readlen = co_await asio::async_read_until(
              stream, asio::dynamic_buffer(buf), '\n',
              asio::redirect_error(asio::use_awaitable, errCode));

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
              offset, lineNum)};
        }

        auto rawStr = result.str();
        if (agentxx::util::autoConvertToUtf8(rawStr)) {
          co_return rawStr;
        }
        co_return rawStr;
      }

      // 读取完整文件
      std::string data;
      co_await asio::async_read(
          stream, asio::dynamic_buffer(data), asio::transfer_all(),
          asio::redirect_error(asio::use_awaitable, errCode));
      if (errCode && errCode != asio::error::eof) {
        throw std::system_error{errCode};
      }
      stream.close();
      if (agentxx::util::autoConvertToUtf8(data)) {
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
        throw std::runtime_error{
            fmt::format(R"(Can not open file. Error: {})", ec.message())};
      }

      if (text_line_offset >= 0 || text_line_limit > 0) {
        // 读取部分文件
        const auto offset =
            (text_line_offset >= 0) ? size_t(text_line_offset) : 0;
        const auto limit = (text_line_limit > 0)
                               ? size_t(text_line_limit)
                               : std::numeric_limits<size_t>::max();
        std::stringstream result{};
        size_t lineNum = 0;

        for (std::string line;
             std::getline(stream, line) && lineNum < offset + limit;
             lineNum++) {
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
              offset, lineNum)};
        }

        auto rawStr = result.str();
        if (agentxx::util::autoConvertToUtf8(rawStr)) {
          co_return rawStr;
        }
        co_return rawStr;
      }

      // 读取完整文件
      auto result = std::string{std::istreambuf_iterator<char>(stream),
                                std::istreambuf_iterator<char>()};
      stream.close();
      if (agentxx::util::autoConvertToUtf8(result)) {
        co_return result;
      }
      co_return result;
    }
  }
};

/// read
class FilesystemReadBinaryFileTool : public XXToolBase {
public:
  FilesystemReadBinaryFileTool(
      std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
      : XXToolBase("filesystem_read_binary_file", in_agentContext, false,
                   false) {}

  neograph::ChatTool get_definition() const override {
    auto agentPtr = agentContext.lock();
    const auto &prompt = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

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
            },
            {"required", neograph::json::array({"path"})},
        },
    };
  }

  std::optional<agentxx::middleware::SummarizationToolHandle>
  createSummarizationToolHandle() const override {
    return agentxx::middleware::SummarizationToolHandle{
        .generateDeduplicationKey = _defFileReadGenerateKey,
        .truncateRequest = nullptr,
        .truncateResponse = _defTruncateToolcallResponse,
    };
  }

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override {
    auto filepath =
        agentxx::util::toStandardPath(arguments.value("path", std::string{}));
    if (filepath.empty()) {
      co_return R"({"error":"Arg `path` is empty"})";
    }
    auto systemCharsetFilePath = filepath;
    agentxx::util::autoConvertToSystemPath(systemCharsetFilePath);
    auto byte_offset = arguments.value<double>("byte_offset", -1);
    auto byte_limit = arguments.value<double>("byte_limit", -1);

#if ASIO_HAS_FILE || BOOST_ASIO_HAS_FILE
    {
      auto currentIoCtx = co_await asio::this_coro::executor;

      /// 异步读取文件
      if (byte_offset >= 0 || byte_limit >= 0) {
        asio::random_access_file stream{currentIoCtx};
        neograph_asio_error_code errCode;
        stream.open(systemCharsetFilePath, asio::random_access_file::read_only,
                    errCode);
        if (false == stream.is_open()) {
          throw std::runtime_error{
              fmt::format(R"(Can not open file: {}")", errCode.message())};
        }

        // 读取部分文件
        const size_t offset = (byte_offset >= 0) ? size_t(byte_offset) : 0;
        const size_t limit = (byte_limit >= 0)
                                 ? size_t(byte_limit)
                                 : std::numeric_limits<size_t>::max();

        auto fileSize = stream.size();
        auto bytesAvailable =
            std::max((int64_t)fileSize - (int64_t)offset, (int64_t)0);
        auto bytesRead = std::min(static_cast<std::streamsize>(limit),
                                  static_cast<std::streamsize>(bytesAvailable));

        // 没有数据可读
        if (bytesRead <= 0) {
          throw std::runtime_error{fmt::format(
              R"(Arg `byte_offset`({}) is out of range of file size({}).)",
              offset, static_cast<size_t>(fileSize))};
        }

        std::string result;
        auto bytesReadLen = co_await asio::async_read_at(
            stream, offset, asio::buffer(result, bytesRead),
            asio::redirect_error(asio::use_awaitable, errCode));
        if (errCode && errCode != asio::error::eof) {
          throw std::system_error{errCode};
        }
        stream.close();
        auto readRange = std::string_view{result}.substr(0, bytesReadLen);
        co_return neograph::json{
            {"bytes_read_len", bytesReadLen},
            {
                "base64_data",
                agentxx::util::base64Encode(readRange),
            },
        }
            .dump();
      }

      // 读取完整文件
      asio::stream_file stream{currentIoCtx};
      neograph_asio_error_code errCode;
      stream.open(systemCharsetFilePath, asio::stream_file::read_only, errCode);
      if (false == stream.is_open()) {
        throw std::runtime_error{
            fmt::format(R"(Can not open file: {}")", errCode.message())};
      }

      std::string result;
      auto bytesReadLen = co_await asio::async_read(
          stream, asio::dynamic_buffer(result), asio::transfer_all(),
          asio::redirect_error(asio::use_awaitable, errCode));
      if (errCode && errCode != asio::error::eof) {
        throw std::system_error{errCode};
      }
      stream.close();
      auto readRange = std::string_view{result}.substr(0, bytesReadLen);
      co_return neograph::json{
          {"bytes_read_len", bytesReadLen},
          {
              "base64_data",
              agentxx::util::base64Encode(readRange),
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
        throw std::runtime_error{
            fmt::format(R"(Can not open file. Error: {})", ec.message())};
      }

      if (byte_offset >= 0 || byte_limit >= 0) {
        // 读取部分文件
        const size_t offset = (byte_offset >= 0) ? size_t(byte_offset) : 0;
        const size_t limit = (byte_limit >= 0)
                                 ? size_t(byte_limit)
                                 : std::numeric_limits<size_t>::max();

        // 计算实际需要读取的字节数
        size_t fileSize =
            static_cast<size_t>(std::filesystem::file_size(filepath));
        auto bytesAvailable =
            std::max((int64_t)fileSize - (int64_t)offset, (int64_t)0);
        auto bytesRead = std::min(static_cast<std::streamsize>(limit),
                                  static_cast<std::streamsize>(bytesAvailable));

        // 没有数据可读
        if (bytesRead <= 0) {
          throw std::runtime_error{fmt::format(
              R"(Arg `byte_offset`({}) is out of range of file size({}).)",
              offset, fileSize)};
        }

        stream.seekg(offset, std::ios::beg);
        if (!stream.good()) {
          auto ec = std::error_code{errno, std::system_category()};
          throw std::runtime_error{
              fmt::format(R"(Read offset {} bytes failed. Error: {})", offset,
                          ec.message())};
        }

        std::string result;
        result.resize(bytesRead);
        stream.read(result.data(), bytesRead);
        std::streamsize realBytesRead = stream.gcount();

        stream.close();
        co_return neograph::json{
            {"bytes_read_len", realBytesRead},
            {"base64_data", agentxx::util::base64Encode(result)},
        }
            .dump();
      }

      // 读取完整文件
      auto result = std::string((std::istreambuf_iterator<char>(stream)),
                                std::istreambuf_iterator<char>());
      auto bytesReadLen = result.size();
      stream.close();
      co_return neograph::json{
          {"bytes_read_len", bytesReadLen},
          {"base64_data", agentxx::util::base64Encode(result)},
      }
          .dump();
    }
  }
};

/// write
class FilesystemWriteFileTool : public XXToolBase {
public:
  FilesystemWriteFileTool(
      std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
      : XXToolBase("filesystem_write_file", in_agentContext, false, false) {}

  neograph::ChatTool get_definition() const override {
    auto agentPtr = agentContext.lock();
    const auto &prompt = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

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
            },
            {"required", neograph::json::array({"path"})},
        },
    };
  }

  std::optional<agentxx::middleware::SummarizationToolHandle>
  createSummarizationToolHandle() const override {
    return agentxx::middleware::SummarizationToolHandle{
        .generateDeduplicationKey = _defFileWriteGenerateKey,
        .truncateRequest = _defTruncateToolcallRequest,
        .truncateResponse = nullptr,
    };
  }

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override {
    auto filepath =
        agentxx::util::toStandardPath(arguments.value("path", std::string{}));
    if (filepath.empty()) {
      co_return R"({"error":"Arg `path` is empty"})";
    }
    auto systemCharsetFilePath = filepath;
    agentxx::util::autoConvertToSystemPath(systemCharsetFilePath);
    auto content = arguments.value<std::string>("content", std::string{});
    auto overwrite = arguments.value<bool>("overwrite", false);
    auto is_binary = arguments.value<bool>("is_binary", false);

#if ASIO_HAS_FILE || BOOST_ASIO_HAS_FILE
    {
      auto currentIoCtx = co_await asio::this_coro::executor;

      // 读取完整文件
      asio::stream_file stream{currentIoCtx};
      auto path = std::filesystem::path(filepath);
      if (false == overwrite && std::filesystem::exists(path)) {
        throw std::runtime_error{"File already exist"};
      }
      if (false == std::filesystem::exists(path.parent_path()) &&
          false == std::filesystem::create_directories(path.parent_path())) {
        // 创建父目录
        throw std::runtime_error{
            fmt::format(R"(Can not create `path`({})'s parent dirs.)",
                        path.parent_path().string())};
      }

      neograph_asio_error_code errCode;
      stream.open(systemCharsetFilePath,
                  asio::stream_file::write_only | asio::stream_file::create |
                      asio::stream_file::truncate,
                  errCode);
      if (false == stream.is_open()) {
        throw std::runtime_error{
            fmt::format(R"(Can not open file: {}")", errCode.message())};
      }

      if (false == content.empty()) {
        // 写入文件内容
        if (is_binary) {
          auto result = agentxx::util::base64Decode(content);
          if (result.empty()) {
            throw std::runtime_error{"base64 decode failed"};
          }
          co_await asio::async_write(
              stream, asio::buffer(result),
              asio::redirect_error(asio::use_awaitable, errCode));
        } else {
          co_await asio::async_write(
              stream, asio::buffer(content),
              asio::redirect_error(asio::use_awaitable, errCode));
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
      auto path = std::filesystem::path(filepath);
      if (false == overwrite && std::filesystem::exists(path)) {
        throw std::runtime_error{"File already exist"};
      }
      if (false == std::filesystem::exists(path.parent_path()) &&
          false == std::filesystem::create_directories(path.parent_path())) {
        // 创建父目录
        throw std::runtime_error{
            fmt::format(R"(Can not create `path`({})'s parent dirs.)",
                        path.parent_path().string())};
      }

      stream.open(systemCharsetFilePath,
                  is_binary ? std::ios_base::out | std::ios_base::binary |
                                  std::ios_base::trunc
                            : std::ios_base::out | std::ios_base::trunc);
      if (!stream) {
        auto ec = std::error_code{errno, std::system_category()};
        throw std::runtime_error{fmt::format(
            R"(Can not create or open file. Error: {})", ec.message())};
      }

      if (false == content.empty()) {
        // 写入文件内容
        if (is_binary) {
          auto result = agentxx::util::base64Decode(content);
          if (result.empty()) {
            throw std::runtime_error{"base64 decode failed"};
          }
          stream << result;
        } else {
          stream << content;
        }
        if (!stream) {
          auto ec = std::error_code{errno, std::system_category()};
          throw std::runtime_error{fmt::format(
              R"(File created success, but write failed. Error: {})",
              ec.message())};
        }
      }

      stream.close();
      co_return "success";
    }
  }
};

/// edit file
class FilesystemEditTextFileTool : public XXToolBase {
public:
  FilesystemEditTextFileTool(
      std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
      : XXToolBase("filesystem_edit_text_file", in_agentContext, false, false) {
  }

  neograph::ChatTool get_definition() const override {
    auto agentPtr = agentContext.lock();
    const auto &prompt = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

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
            },
            {"required", neograph::json::array({"path", "old_str", "new_str"})},
        },
    };
  }

  std::optional<agentxx::middleware::SummarizationToolHandle>
  createSummarizationToolHandle() const override {
    return agentxx::middleware::SummarizationToolHandle{
        .generateDeduplicationKey = _defFileWriteGenerateKey,
        .truncateRequest = _defTruncateToolcallRequest,
        .truncateResponse = nullptr,
    };
  }

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override {
    auto filepath =
        agentxx::util::toStandardPath(arguments.value("path", std::string{}));
    if (filepath.empty()) {
      co_return R"({"error":"Arg `path` is empty"})";
    }
    auto systemCharsetFilePath = filepath;
    agentxx::util::autoConvertToSystemPath(systemCharsetFilePath);
    auto old_str = arguments.value<std::string>("old_str", std::string{});
    if (old_str.empty()) {
      co_return R"({"error":"Arg `old_str` is empty"})";
    }
    auto new_str = arguments.value<std::string>("new_str", std::string{});
    auto multi_replace = arguments.value<bool>("multi_replace", false);

#if ASIO_HAS_FILE || BOOST_ASIO_HAS_FILE
    {
      auto currentIoCtx = co_await asio::this_coro::executor;

      // 读取完整文件
      asio::stream_file stream{currentIoCtx};
      auto path = std::filesystem::path(filepath);
      if (false == std::filesystem::exists(path)) {
        throw std::runtime_error{"File not exist"};
      }

      neograph_asio_error_code errCode;
      stream.open(systemCharsetFilePath, asio::stream_file::read_only, errCode);
      if (false == stream.is_open()) {
        throw std::runtime_error{
            fmt::format(R"(Can not open file: {}")", errCode.message())};
      }

      std::string content;
      // 读出文件
      co_await asio::async_read(
          stream, asio::dynamic_buffer(content), asio::transfer_all(),
          asio::redirect_error(asio::use_awaitable, errCode));
      if (errCode && errCode != asio::error::eof) {
        throw std::system_error{errCode};
      }
      stream.close();

      int replaceHit = 0;
      size_t pos = 0;
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
            R"(No match `old_str` found, Try re-reading to get the latest file content.)"};
      }

      // 覆盖写入文件内容
      stream.open(systemCharsetFilePath,
                  asio::stream_file::write_only | asio::stream_file::create |
                      asio::stream_file::truncate,
                  errCode);
      if (false == stream.is_open()) {
        throw std::runtime_error{fmt::format(
            R"(Can not open file to write: {}")", errCode.message())};
      }
      co_await asio::async_write(
          stream, asio::buffer(content),
          asio::redirect_error(asio::use_awaitable, errCode));
      if (errCode) {
        throw std::system_error{errCode};
      }

      stream.close();
      if (multi_replace) {
        co_return fmt::format(R"(Success, Replace {} times)", replaceHit);
      } else {
        co_return "success";
      }
    }
#endif

    {
      std::fstream stream;
      auto path = std::filesystem::path(filepath);
      if (false == std::filesystem::exists(path)) {
        throw std::runtime_error{"File not exist"};
      }

      stream.open(systemCharsetFilePath, std::ios_base::in);
      if (!stream) {
        auto ec = std::error_code{errno, std::system_category()};
        throw std::runtime_error{
            fmt::format(R"(Can not open file. Error: {})", ec.message())};
      }

      std::ostringstream output;
      output << stream.rdbuf();
      std::string content = output.str();

      int replaceHit = 0;
      size_t pos = 0;
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
            R"(No match `old_str` found, Try re-reading to get the latest file content.)"};
      }

      // 写入文件内容
      stream.close();
      stream.open(systemCharsetFilePath, std::ios_base::out);
      if (!stream) {
        auto ec = std::error_code{errno, std::system_category()};
        throw std::runtime_error{fmt::format(
            R"(Can not open file to write. Error: {})", ec.message())};
      }
      stream << content;
      if (!stream) {
        auto ec = std::error_code{errno, std::system_category()};
        throw std::runtime_error{
            fmt::format(R"(Edit file failed. Error: {})", ec.message())};
      }

      stream.close();
      if (multi_replace) {
        co_return fmt::format(R"(Success, Replace {} times)", replaceHit);
      } else {
        co_return "success";
      }
    }
  }
};

class FilesystemGlobTool : public XXToolBase {
public:
  FilesystemGlobTool(
      std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
      : XXToolBase("filesystem_glob", in_agentContext, false, false) {}

  neograph::ChatTool get_definition() const override {
    auto agentPtr = agentContext.lock();
    const auto &prompt = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

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
                },
            },
            {"required", neograph::json::array({"file_patterns"})},
        },
    };
  }

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override {
    auto patterns =
        arguments.value("file_patterns", std::vector<std::string>{});
    if (patterns.empty()) {
      co_return R"({"error":"Arg `file_patterns` is empty"})";
    }

    auto relist = glob::rglob(patterns);
    if (relist.empty()) {
      co_return R"({"error":"No match `file_patterns` found"})";
    }

    auto result = std::ostringstream{};
    for (auto &item : relist) {
      result << item.string() << std::endl;
    }

    co_return result.str();
  }
};

class FilesystemGrepTool : public XXToolBase {
public:
  FilesystemGrepTool(
      std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
      : XXToolBase("filesystem_grep", in_agentContext, false, false) {}

  neograph::ChatTool get_definition() const override {
    auto agentPtr = agentContext.lock();
    const auto &prompt = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

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
                            {"description",
                             prompt.getArg("text_patterns_is_regex")},
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
                            {"enum", neograph::json::array(
                                         {"files_with_matches", "content"})},
                            {"description", prompt.getArg("output_mode")},
                        },
                    },
                },
            },
            {"required", neograph::json::array({
                             "text_patterns_is_regex",
                             "text_patterns",
                             "file_patterns",
                         })},
        },
    };
  }

  asio::awaitable<std::string> readFileContent(const std::string &filepath) {
    auto systemCharsetFilePath = agentxx::util::toStandardPath(filepath);
    agentxx::util::autoConvertToSystemPath(systemCharsetFilePath);

#if ASIO_HAS_FILE || BOOST_ASIO_HAS_FILE
    {
      auto currentIoCtx = co_await asio::this_coro::executor;

      /// 异步读取文件
      asio::stream_file stream{currentIoCtx};
      neograph_asio_error_code errCode;
      stream.open(systemCharsetFilePath, asio::stream_file::read_only, errCode);
      if (false == stream.is_open()) {
        throw std::runtime_error{
            fmt::format(R"(Can not open file. Error: {})", errCode.message())};
      }

      // 读取完整文件
      std::string data;
      co_await asio::async_read(
          stream, asio::dynamic_buffer(data), asio::transfer_all(),
          asio::redirect_error(asio::use_awaitable, errCode));
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
        throw std::runtime_error{
            fmt::format(R"(Can not open file. Error: {})", ec.message())};
      }

      // 读取完整文件
      auto result = std::string{std::istreambuf_iterator<char>(stream),
                                std::istreambuf_iterator<char>()};
      stream.close();
      co_return result;
    }
  }

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override {
    auto text_patterns_is_regex =
        arguments.value("text_patterns_is_regex", true);
    auto text_patterns =
        arguments.value("text_patterns", std::vector<std::string>{});
    if (text_patterns.empty()) {
      co_return R"({"error":"Arg `text_patterns` is empty"})";
    }
    auto file_patterns =
        arguments.value("file_patterns", std::vector<std::string>{});
    if (file_patterns.empty()) {
      co_return R"({"error":"Arg `file_patterns` is empty"})";
    }
    auto output_mode =
        arguments.value("output_mode", std::string{"files_with_matches"});
    if (output_mode.empty()) {
      co_return R"({"error":"Arg `output_mode` is empty"})";
    }

    std::vector<std::filesystem::path> refilelist{};
    auto relist = glob::rglob(file_patterns);
    refilelist.insert(refilelist.end(), std::make_move_iterator(relist.begin()),
                      std::make_move_iterator(relist.end()));
    if (refilelist.empty()) {
      throw std::runtime_error{"No match `file_patterns` file found"};
    }

    bool isContainsMode = ("content" != output_mode);
    auto resultStr = std::ostringstream{};
    auto resultJson = neograph::json::array();

    if (text_patterns_is_regex) {
      // 正则匹配
      auto regex = agentxx::util::XXRegex::createRegex(text_patterns);
      for (const auto &item : refilelist) {
        auto filepath = item.generic_string();
        auto filetext = co_await readFileContent(filepath);
        auto matchs = std::vector<agentxx::util::XXRegexMatchResult>{};
        if (regex->match(filetext, matchs)) {
          if (isContainsMode) {
            resultStr << filepath << ":" << matchs.size() << "\n";
          } else {
            auto matchsContent = neograph::json::array();
            size_t index = 0;
            size_t lineCount = 0;

            for (const auto &match : matchs) {
              // 计算到 match 时的行数
              for (size_t i = index; i < match.start; ++i) {
                if (filetext[i] == '\n') {
                  ++lineCount;
                }
              }

              matchsContent.push_back(neograph::json{
                  {"content", std::string_view{filetext}.substr(
                                  match.start, match.end - match.start)},
                  {"line", lineCount},
              });
              index = match.start;
            }

            resultJson.push_back(neograph::json{
                {"filepath", filepath},
                {"matchs", matchsContent},
            });
          }
        }
      }
    } else {
      // 文本精确匹配
      auto search = agentxx::util::AhoCorasick<char>{text_patterns, true};
      for (const auto &item : refilelist) {
        auto filepath = item.generic_string();
        auto filetext = co_await readFileContent(filepath);
        auto matchs = search.search(filetext);
        if (false == matchs.empty()) {
          if (isContainsMode) {
            resultStr << filepath << ":" << matchs.size() << "\n";
          } else {
            auto matchsContent = neograph::json::array();
            size_t index = 0;
            size_t lineCount = 0;

            for (const auto &match : matchs) {
              // 计算到 match 时的行数
              for (size_t i = index; i < match.start; ++i) {
                if (filetext[i] == '\n') {
                  ++lineCount;
                }
              }

              matchsContent.push_back(neograph::json{
                  {"content", text_patterns},
                  {"line", lineCount},
              });
              index = match.start;
            }

            resultJson.push_back(neograph::json{
                {"filepath", filepath},
                {"matchs", matchsContent},
            });
          }
        }
      }
    }

    if (isContainsMode) {
      auto str = resultStr.str();
      if (false == str.empty()) {
        co_return str;
      } else {
        throw std::runtime_error{
            fmt::format("Found {} files match `file_patterns`, but no match "
                        "`text_patterns` file found.",
                        refilelist.size())};
      }
    } else {
      if (false == resultJson.empty()) {
        co_return resultJson.dump();
      } else {
        throw std::runtime_error{
            fmt::format("Found {} files match `file_patterns`, but no match "
                        "`text_patterns` file found.",
                        refilelist.size())};
      }
    }
  }
};

} // namespace tools
} // namespace agentxx