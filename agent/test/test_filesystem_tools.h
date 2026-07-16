#pragma once

#include "agentxx/agent/context.h"
#include "agentxx/tools/filesystem.h"
#include "neograph/neograph.h"
#include "test_framework.h"
#include <asio/awaitable.hpp>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace agentxx {
namespace test {

inline static int g_fs_passed = 0;
inline static int g_fs_failed = 0;

const std::string testDir =
    (std::filesystem::temp_directory_path() / "agentxx_test_filesystem")
        .generic_string();

inline void setupTestDir() {
  namespace fs = std::filesystem;
  if (fs::exists(testDir)) {
    fs::remove_all(testDir);
  }
  fs::create_directories(testDir);

  std::ofstream f1(testDir + "/test1.txt");
  f1 << "line1\nline2\nline3\nline4\nline5\n";
  f1.close();

  std::ofstream f2(testDir + "/test2.txt");
  f2 << "hello world\nthis is a test file\n";
  f2.close();

  fs::create_directory(testDir + "/subdir");
  std::ofstream f3(testDir + "/subdir/subtest.txt");
  f3 << "subdir file content\n";
  f3.close();
}

inline void cleanupTestDir() { std::filesystem::remove_all(testDir); }

inline asio::awaitable<void> test_list_file_get_definition(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::FileSystemListTool{agentContext};
  auto def = tool.get_definition();
  if (def.name == "filesystem_list") {
    g_fs_passed++;
    TEST_PASS << "FileSystemListTool::get_definition() name correct"
              << std::endl;
  } else {
    g_fs_failed++;
    TEST_FAIL << "FileSystemListTool::get_definition() name incorrect"
              << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_list_file_empty_path(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::FileSystemListTool{agentContext};
  auto args = neograph::json{{"path", ""}};
  auto result = co_await tool.execute_async(args);
  if (result.find("\"error\"") != std::string::npos) {
    g_fs_passed++;
    TEST_PASS << "FileSystemListTool returns error for empty path" << std::endl;
  } else {
    g_fs_failed++;
    TEST_FAIL << "FileSystemListTool should return error for empty "
                 "path, got: "
              << result << std::endl;
  }
  co_return;
}

inline asio::awaitable<void>
test_list_file_basic(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::FileSystemListTool{agentContext};
  auto args = neograph::json{{"path", testDir}};
  auto result = co_await tool.execute_async(args);
  if (result.find("test1.txt") != std::string::npos &&
      result.find("test2.txt") != std::string::npos &&
      result.find("subdir") != std::string::npos) {
    g_fs_passed++;
    TEST_PASS << "FileSystemListTool lists directory contents" << std::endl;
  } else {
    g_fs_failed++;
    TEST_FAIL << "FileSystemListTool listing failed, got: " << result
              << " path: " << testDir << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_list_file_recursive(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::FileSystemListTool{agentContext};
  auto args = neograph::json{
      {"path", testDir},
      {"recursive", true},
  };
  auto result = co_await tool.execute_async(args);
  if (result.find("subtest.txt") != std::string::npos) {
    g_fs_passed++;
    TEST_PASS << "FileSystemListTool recursive lists subdirectories"
              << std::endl;
  } else {
    g_fs_failed++;
    TEST_FAIL << "FileSystemListTool recursive listing failed, got: " << result
              << " path: " << testDir << std::endl;
  }
  co_return;
}

inline asio::awaitable<void>
test_list_file_limit(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::FileSystemListTool{agentContext};
  auto args = neograph::json{
      {"path", testDir},
      {"limit", 1},
  };
  auto result = co_await tool.execute_async(args);
  auto jsonResult = neograph::json::parse(result);
  if (jsonResult.is_array() && jsonResult.size() <= 1) {
    g_fs_passed++;
    TEST_PASS << "FileSystemListTool respects limit parameter" << std::endl;
  } else {
    g_fs_failed++;
    TEST_FAIL << "FileSystemListTool limit failed, got: " << result
              << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_list_file_info_fields(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::FileSystemListTool{agentContext};
  auto args = neograph::json{{"path", testDir}};
  auto result = co_await tool.execute_async(args);
  auto jsonResult = neograph::json::parse(result);
  if (jsonResult.is_array() && jsonResult.size() >= 1) {
    bool hasAllFields = false;
    for (const auto &item : jsonResult) {
      if (item.contains("path") && item.contains("type") &&
          item.contains("last_write_time") && item.contains("size")) {
        hasAllFields = true;
        break;
      }
    }
    if (hasAllFields) {
      g_fs_passed++;
      TEST_PASS << "FileSystemListTool returns file info with all fields"
                << std::endl;
    } else {
      g_fs_failed++;
      TEST_FAIL << "FileSystemListTool missing file info fields, "
                   "first item keys: ";
      auto first = jsonResult[0];
      for (auto it = first.begin(); it != first.end(); ++it) {
        std::cout << it.key() << " ";
      }
      std::cout << std::endl;
    }
  } else {
    g_fs_failed++;
    TEST_FAIL << "FileSystemListTool expected array result, got: " << result
              << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_read_text_file_get_definition(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::FilesystemReadTextFileTool{agentContext};
  auto def = tool.get_definition();
  if (def.name == "filesystem_read_text_file") {
    std::cout
        << "[PASS] FilesystemReadTextFileTool::get_definition() name correct"
        << std::endl;
  } else {
    std::cout
        << "[FAIL] FilesystemReadTextFileTool::get_definition() name incorrect"
        << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_read_text_file_empty_path(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::FilesystemReadTextFileTool{agentContext};
  auto args = neograph::json{{"path", ""}};
  try {
    auto result = co_await tool.execute_async(args);
    if (result.find("\"error\"") != std::string::npos) {
      std::cout
          << "[PASS] FilesystemReadTextFileTool returns error for empty path"
          << std::endl;
    } else {
      g_fs_failed++;
      TEST_FAIL << "FilesystemReadTextFileTool should return error for "
                   "empty path, got: "
                << result << std::endl;
    }
  } catch (const std::exception &e) {
    g_fs_passed++;
    TEST_PASS << "FilesystemReadTextFileTool throws for empty path: "
              << e.what() << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_read_text_file_not_exist(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::FilesystemReadTextFileTool{agentContext};
  auto args = neograph::json{{"path", testDir + "/nonexistent.txt"}};
  try {
    auto result = co_await tool.execute_async(args);
    TEST_INFO << "FilesystemReadTextFileTool non-existent file: " << result
              << std::endl;
  } catch (const std::exception &e) {
    g_fs_passed++;
    TEST_PASS << "FilesystemReadTextFileTool throws for non-existent "
                 "file: "
              << e.what() << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_read_text_file_full(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::FilesystemReadTextFileTool{agentContext};
  auto args = neograph::json{{"path", testDir + "/test1.txt"}};
  auto result = co_await tool.execute_async(args);
  if (result.find("line1") != std::string::npos &&
      result.find("line5") != std::string::npos) {
    g_fs_passed++;
    TEST_PASS << "FilesystemReadTextFileTool reads full file content"
              << std::endl;
  } else {
    g_fs_failed++;
    TEST_FAIL << "FilesystemReadTextFileTool full read failed, got: " << result
              << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_read_text_file_offset(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto offsetFile = testDir + "/offset_test.txt";
  {
    std::ofstream f(offsetFile);
    f << "aaaa\nbbbb\ncccc\ndddd\n";
  }
  auto tool = agentxx::tools::FilesystemReadTextFileTool{agentContext};
  auto args = neograph::json{
      {"path", offsetFile},
      {"line_offset", 0},
  };
  auto result = co_await tool.execute_async(args);
  if (result.find("aaaa") != std::string::npos) {
    g_fs_passed++;
    TEST_PASS << "FilesystemReadTextFileTool respects line_offset=0"
              << std::endl;
  } else {
    g_fs_failed++;
    TEST_FAIL << "FilesystemReadTextFileTool line_offset=0 failed, got: "
              << result << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_read_text_file_limit(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::FilesystemReadTextFileTool{agentContext};
  auto args = neograph::json{
      {"path", testDir + "/offset_test.txt"},
      {"line_limit", 3},
  };
  auto result = co_await tool.execute_async(args);
  auto lineCount = size_t{0};
  for (size_t i = 0; i < result.size(); i++) {
    if (result[i] == '\n')
      lineCount++;
  }
  if (lineCount <= 3) {
    g_fs_passed++;
    TEST_PASS << "FilesystemReadTextFileTool respects line_limit" << std::endl;
  } else {
    g_fs_failed++;
    TEST_FAIL << "FilesystemReadTextFileTool line_limit failed, got "
              << lineCount << " lines" << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_read_text_file_offset_and_limit(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::FilesystemReadTextFileTool{agentContext};
  auto args = neograph::json{
      {"path", testDir + "/offset_test.txt"},
      {"line_offset", 0},
      {"line_limit", 2},
  };
  auto result = co_await tool.execute_async(args);
  if (result.find("aaaa") != std::string::npos &&
      result.find("bbbb") != std::string::npos &&
      result.find("cccc") == std::string::npos) {
    std::cout
        << "[PASS] FilesystemReadTextFileTool respects both offset and limit"
        << std::endl;
  } else {
    g_fs_failed++;
    TEST_FAIL << "FilesystemReadTextFileTool offset+limit failed, got: "
              << result << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_read_binary_file_get_definition(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::FilesystemReadBinaryFileTool{agentContext};
  auto def = tool.get_definition();
  if (def.name == "filesystem_read_binary_file") {
    std::cout
        << "[PASS] FilesystemReadBinaryFileTool::get_definition() name correct"
        << std::endl;
  } else {
    g_fs_failed++;
    TEST_FAIL << "FilesystemReadBinaryFileTool::get_definition() name "
                 "incorrect"
              << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_read_binary_file_full(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::FilesystemReadBinaryFileTool{agentContext};
  auto args = neograph::json{{"path", testDir + "/test1.txt"}};
  auto result = co_await tool.execute_async(args);
  if (result.find("base64_data") != std::string::npos &&
      result.find("bytes_read_len") != std::string::npos) {
    std::cout
        << "[PASS] FilesystemReadBinaryFileTool returns base64 encoded data"
        << std::endl;
  } else {
    g_fs_failed++;
    TEST_FAIL << "FilesystemReadBinaryFileTool binary read failed, got: "
              << result << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_write_file_get_definition(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::FilesystemWriteFileTool{agentContext};
  auto def = tool.get_definition();
  if (def.name == "filesystem_write_file") {
    g_fs_passed++;
    TEST_PASS << "FilesystemWriteFileTool::get_definition() name correct"
              << std::endl;
  } else {
    std::cout
        << "[FAIL] FilesystemWriteFileTool::get_definition() name incorrect"
        << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_write_file_empty_path(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::FilesystemWriteFileTool{agentContext};
  auto args = neograph::json{{"path", ""}};
  auto result = co_await tool.execute_async(args);
  if (result.find("\"error\"") != std::string::npos) {
    g_fs_passed++;
    TEST_PASS << "FilesystemWriteFileTool returns error for empty path"
              << std::endl;
  } else {
    g_fs_failed++;
    TEST_FAIL << "FilesystemWriteFileTool should return error for empty "
                 "path, got: "
              << result << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_write_file_create(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::FilesystemWriteFileTool{agentContext};
  auto filePath = testDir + "/write_test.txt";
  auto args = neograph::json{
      {"path", filePath},
      {"content", "hello write test"},
  };
  auto result = co_await tool.execute_async(args);
  if (result == "success") {
    if (std::filesystem::exists(filePath)) {
      std::ifstream in(filePath);
      std::string content((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
      if (content == "hello write test") {
        g_fs_passed++;
        TEST_PASS << "FilesystemWriteFileTool creates and writes file"
                  << std::endl;
      } else {
        g_fs_failed++;
        TEST_FAIL << "FilesystemWriteFileTool wrote wrong content: '" << content
                  << "'" << std::endl;
      }
    } else {
      g_fs_failed++;
      TEST_FAIL << "FilesystemWriteFileTool file not created" << std::endl;
    }
  } else {
    g_fs_failed++;
    TEST_FAIL << "FilesystemWriteFileTool write failed: " << result
              << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_write_file_no_overwrite_existing(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::FilesystemWriteFileTool{agentContext};
  auto filePath = testDir + "/test1.txt";
  auto args = neograph::json{
      {"path", filePath},
      {"content", "new content"},
      {"overwrite", false},
  };
  try {
    auto result = co_await tool.execute_async(args);
    TEST_INFO << "FilesystemWriteFileTool no-overwrite result: " << result
              << std::endl;
  } catch (const std::exception &e) {
    g_fs_passed++;
    TEST_PASS << "FilesystemWriteFileTool throws when file exists and "
                 "overwrite=false: "
              << e.what() << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_write_file_overwrite(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::FilesystemWriteFileTool{agentContext};
  auto filePath = testDir + "/overwrite_test.txt";
  {
    std::ofstream f(filePath);
    f << "original content\n";
  }
  auto args = neograph::json{
      {"path", filePath},
      {"content", "overwritten content"},
      {"overwrite", true},
  };
  auto result = co_await tool.execute_async(args);
  if (result == "success") {
    std::ifstream in(filePath);
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    if (content == "overwritten content") {
      g_fs_passed++;
      TEST_PASS << "FilesystemWriteFileTool overwrites file when "
                   "overwrite=true"
                << std::endl;
    } else {
      g_fs_failed++;
      TEST_FAIL << "FilesystemWriteFileTool overwrite wrong content: '"
                << content << "'" << std::endl;
    }
  } else {
    g_fs_failed++;
    TEST_FAIL << "FilesystemWriteFileTool overwrite failed: " << result
              << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_edit_text_file_get_definition(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::FilesystemEditTextFileTool{agentContext};
  auto def = tool.get_definition();
  if (def.name == "filesystem_edit_text_file") {
    std::cout
        << "[PASS] FilesystemEditTextFileTool::get_definition() name correct"
        << std::endl;
  } else {
    std::cout
        << "[FAIL] FilesystemEditTextFileTool::get_definition() name incorrect"
        << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_edit_text_file_empty_path(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::FilesystemEditTextFileTool{agentContext};
  auto args = neograph::json{
      {"path", ""},
      {"old_str", "a"},
      {"new_str", "b"},
  };
  auto result = co_await tool.execute_async(args);
  if (result.find("\"error\"") != std::string::npos) {
    std::cout
        << "[PASS] FilesystemEditTextFileTool returns error for empty path"
        << std::endl;
  } else {
    g_fs_failed++;
    TEST_FAIL << "FilesystemEditTextFileTool should return error for "
                 "empty path, got: "
              << result << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_edit_text_file_empty_old_str(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::FilesystemEditTextFileTool{agentContext};
  auto args = neograph::json{
      {"path", testDir + "/test1.txt"},
      {"old_str", ""},
      {"new_str", "b"},
  };
  auto result = co_await tool.execute_async(args);
  if (result.find("\"error\"") != std::string::npos) {
    std::cout
        << "[PASS] FilesystemEditTextFileTool returns error for empty old_str"
        << std::endl;
  } else {
    g_fs_failed++;
    TEST_FAIL << "FilesystemEditTextFileTool should return error for "
                 "empty old_str, got: "
              << result << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_edit_text_file_single_replace(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto filePath = testDir + "/edit_test.txt";
  {
    std::ofstream f(filePath);
    f << "hello world\nfoo bar\n";
  }

  auto tool = agentxx::tools::FilesystemEditTextFileTool{agentContext};
  auto args = neograph::json{
      {"path", filePath},
      {"old_str", "hello world"},
      {"new_str", "hi universe"},
  };
  auto result = co_await tool.execute_async(args);
  if (result == "success") {
    std::ifstream in(filePath);
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    if (content.find("hi universe") != std::string::npos &&
        content.find("hello world") == std::string::npos) {
      g_fs_passed++;
      TEST_PASS << "FilesystemEditTextFileTool single replace works"
                << std::endl;
    } else {
      g_fs_failed++;
      TEST_FAIL << "FilesystemEditTextFileTool single replace wrong "
                   "content: '"
                << content << "'" << std::endl;
    }
  } else {
    g_fs_failed++;
    TEST_FAIL << "FilesystemEditTextFileTool single replace failed: " << result
              << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_edit_text_file_multi_replace(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto filePath = testDir + "/edit_multi_test.txt";
  {
    std::ofstream f(filePath);
    f << "foo foo foo bar\n";
  }

  auto tool = agentxx::tools::FilesystemEditTextFileTool{agentContext};
  auto args = neograph::json{
      {"path", filePath},
      {"old_str", "foo"},
      {"new_str", "baz"},
      {"multi_replace", true},
  };
  auto result = co_await tool.execute_async(args);
  if (result.find("Replace 3 times") != std::string::npos) {
    std::ifstream in(filePath);
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    if (content.find("foo") == std::string::npos &&
        content.find("baz baz baz") != std::string::npos) {
      g_fs_passed++;
      TEST_PASS << "FilesystemEditTextFileTool multi_replace works"
                << std::endl;
    } else {
      g_fs_failed++;
      TEST_FAIL << "FilesystemEditTextFileTool multi_replace wrong "
                   "content: '"
                << content << "'" << std::endl;
    }
  } else {
    g_fs_failed++;
    TEST_FAIL << "FilesystemEditTextFileTool multi_replace failed: " << result
              << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_edit_text_file_no_match(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto filePath = testDir + "/edit_test.txt";
  auto tool = agentxx::tools::FilesystemEditTextFileTool{agentContext};
  auto args = neograph::json{
      {"path", filePath},
      {"old_str", "nonexistent_string_xyz"},
      {"new_str", "replacement"},
  };
  try {
    auto result = co_await tool.execute_async(args);
    TEST_INFO << "FilesystemEditTextFileTool no-match result: " << result
              << std::endl;
  } catch (const std::exception &e) {
    std::cout
        << "[PASS] FilesystemEditTextFileTool throws when no match found: "
        << e.what() << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_glob_get_definition(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::FilesystemGlobTool{agentContext};
  auto def = tool.get_definition();
  if (def.name == "filesystem_glob") {
    g_fs_passed++;
    TEST_PASS << "FilesystemGlobTool::get_definition() name correct"
              << std::endl;
  } else {
    g_fs_failed++;
    TEST_FAIL << "FilesystemGlobTool::get_definition() name incorrect"
              << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_glob_empty_patterns(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::FilesystemGlobTool{agentContext};
  auto args = neograph::json{
      {"file_patterns", neograph::json::array()},
  };
  auto result = co_await tool.execute_async(args);
  if (result.find("\"error\"") != std::string::npos) {
    std::cout
        << "[PASS] FilesystemGlobTool returns error for empty file_patterns"
        << std::endl;
  } else {
    g_fs_failed++;
    TEST_FAIL << "FilesystemGlobTool should return error for empty "
                 "patterns, got: "
              << result << std::endl;
  }
  co_return;
}

inline asio::awaitable<void>
test_glob_find_files(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::FilesystemGlobTool{agentContext};
  auto args = neograph::json{
      {"file_patterns", neograph::json::array({testDir + "/*.txt"})},
  };
  auto result = co_await tool.execute_async(args);
  if (result.find("test1.txt") != std::string::npos ||
      result.find("test2.txt") != std::string::npos) {
    g_fs_passed++;
    TEST_PASS << "FilesystemGlobTool finds files by pattern" << std::endl;
  } else {
    g_fs_failed++;
    TEST_FAIL << "FilesystemGlobTool glob failed, got: " << result << std::endl;
  }
  co_return;
}

inline asio::awaitable<void>
test_glob_recursive(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::FilesystemGlobTool{agentContext};
  auto args = neograph::json{
      {"file_patterns", neograph::json::array({testDir + "/**/*.txt"})},
  };
  auto result = co_await tool.execute_async(args);
  if (result.find("subtest.txt") != std::string::npos) {
    std::cout
        << "[PASS] FilesystemGlobTool recursive glob finds subdirectory files"
        << std::endl;
  } else {
    g_fs_failed++;
    TEST_FAIL << "FilesystemGlobTool recursive glob failed, got: " << result
              << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_grep_get_definition(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::FilesystemGrepTool{agentContext};
  auto def = tool.get_definition();
  if (def.name == "filesystem_grep") {
    g_fs_passed++;
    TEST_PASS << "FilesystemGrepTool::get_definition() name correct"
              << std::endl;
  } else {
    g_fs_failed++;
    TEST_FAIL << "FilesystemGrepTool::get_definition() name incorrect"
              << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_grep_empty_text_patterns(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::FilesystemGrepTool{agentContext};
  auto args = neograph::json{
      {"text_patterns_is_regex", false},
      {"text_patterns", neograph::json::array()},
      {"file_patterns", neograph::json::array({testDir + "/*.txt"})},
  };
  auto result = co_await tool.execute_async(args);
  if (result.find("\"error\"") != std::string::npos) {
    std::cout
        << "[PASS] FilesystemGrepTool returns error for empty text_patterns"
        << std::endl;
  } else {
    g_fs_failed++;
    TEST_FAIL << "FilesystemGrepTool should return error for empty "
                 "text_patterns, got: "
              << result << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_grep_empty_file_patterns(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::FilesystemGrepTool{agentContext};
  auto args = neograph::json{
      {"text_patterns_is_regex", false},
      {"text_patterns", neograph::json::array({"hello"})},
      {"file_patterns", neograph::json::array()},
  };
  auto result = co_await tool.execute_async(args);
  if (result.find("\"error\"") != std::string::npos) {
    std::cout
        << "[PASS] FilesystemGrepTool returns error for empty file_patterns"
        << std::endl;
  } else {
    g_fs_failed++;
    TEST_FAIL << "FilesystemGrepTool should return error for empty "
                 "file_patterns, got: "
              << result << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_grep_text_search(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::FilesystemGrepTool{agentContext};
  auto args = neograph::json{
      {"text_patterns_is_regex", false},
      {"text_patterns", neograph::json::array({"hello"})},
      {"file_patterns", neograph::json::array({testDir + "/*.txt"})},
      {"output_mode", "files_with_matches"},
  };
  auto result = co_await tool.execute_async(args);
  if (result.find("test2.txt") != std::string::npos) {
    g_fs_passed++;
    TEST_PASS << "FilesystemGrepTool finds files containing text" << std::endl;
  } else {
    g_fs_failed++;
    TEST_FAIL << "FilesystemGrepTool text search failed, got: " << result
              << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_grep_regex_search(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::FilesystemGrepTool{agentContext};
  auto args = neograph::json{
      {"text_patterns_is_regex", true},
      {"text_patterns", neograph::json::array({"line[0-9]"})},
      {"file_patterns", neograph::json::array({testDir + "/test1.txt"})},
      {"output_mode", "files_with_matches"},
  };
  auto result = co_await tool.execute_async(args);
  if (result.find("test1.txt") != std::string::npos) {
    g_fs_passed++;
    TEST_PASS << "FilesystemGrepTool regex search finds matches" << std::endl;
  } else {
    g_fs_failed++;
    TEST_FAIL << "FilesystemGrepTool regex search failed, got: " << result
              << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_grep_content_mode(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tool = agentxx::tools::FilesystemGrepTool{agentContext};
  auto args = neograph::json{
      {"text_patterns_is_regex", false},
      {"text_patterns", neograph::json::array({"hello"})},
      {"file_patterns", neograph::json::array({testDir + "/test2.txt"})},
      {"output_mode", "content"},
  };
  auto result = co_await tool.execute_async(args);
  auto jsonResult = neograph::json::parse(result);
  if (jsonResult.is_array() && jsonResult.size() > 0) {
    std::cout
        << "[PASS] FilesystemGrepTool content mode returns structured JSON"
        << std::endl;
  } else {
    g_fs_failed++;
    TEST_FAIL << "FilesystemGrepTool content mode failed, got: " << result
              << std::endl;
  }
  co_return;
}

inline asio::awaitable<TestResult> run_filesystem_tools_tests(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  setupTestDir();

  auto run = [agentContext](auto testFn) -> asio::awaitable<void> {
    try {
      co_await testFn(agentContext);
    } catch (const std::exception &e) {
      g_fs_failed++;
      TEST_FAIL << "Exception in test: " << e.what() << std::endl;
    }
  };

  co_await run(test_list_file_get_definition);
  co_await run(test_list_file_empty_path);
  co_await run(test_list_file_basic);
  co_await run(test_list_file_recursive);
  co_await run(test_list_file_limit);
  co_await run(test_list_file_info_fields);

  co_await run(test_read_text_file_get_definition);
  co_await run(test_read_text_file_empty_path);
  co_await run(test_read_text_file_not_exist);
  co_await run(test_read_text_file_full);
  co_await run(test_read_text_file_offset);
  co_await run(test_read_text_file_limit);
  co_await run(test_read_text_file_offset_and_limit);

  co_await run(test_read_binary_file_get_definition);
  co_await run(test_read_binary_file_full);

  co_await run(test_write_file_get_definition);
  co_await run(test_write_file_empty_path);
  co_await run(test_write_file_create);
  co_await run(test_write_file_no_overwrite_existing);
  co_await run(test_write_file_overwrite);

  co_await run(test_edit_text_file_get_definition);
  co_await run(test_edit_text_file_empty_path);
  co_await run(test_edit_text_file_empty_old_str);
  co_await run(test_edit_text_file_single_replace);
  co_await run(test_edit_text_file_multi_replace);
  co_await run(test_edit_text_file_no_match);

  co_await run(test_glob_get_definition);
  co_await run(test_glob_empty_patterns);
  co_await run(test_glob_find_files);
  co_await run(test_glob_recursive);

  co_await run(test_grep_get_definition);
  co_await run(test_grep_empty_text_patterns);
  co_await run(test_grep_empty_file_patterns);
  co_await run(test_grep_text_search);
  co_await run(test_grep_regex_search);
  co_await run(test_grep_content_mode);

  cleanupTestDir();
  co_return TestResult{g_fs_passed, g_fs_failed};
}

} // namespace test
} // namespace agentxx