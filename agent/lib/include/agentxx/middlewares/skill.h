#pragma once

#include "agentxx/middlewares/middleware.h"
#include "agentxx/util/string_util.h"
#include "asio/io_context.hpp"
#include "fmt/format.h"
#include "yaml-cpp/yaml.h"
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <neograph/llm/rate_limited_provider.h>
#include <neograph/llm/schema_provider.h>
#include <neograph/neograph.h>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace middleware {

class _SkillMetadata {
public:
  std::string dirpath;

  /// Skill identifier.
  ///   Constraints per Agent Skills specification:
  ///   - 1-64 characters
  ///   - Unicode lowercase alphanumeric and hyphens only (`a-z` and `-`).
  ///   - Must not start or end with `-`
  ///   - Must not contain consecutive `--`
  ///   - Must match the parent directory name containing the `SKILL.md` file
  std::string name;

  /// What the skill does.
  ///     Constraints per Agent Skills specification:
  ///     - 1-1024 characters
  ///     - Should describe both what the skill does and when to use it
  ///     - Should include specific keywords that help agents identify
  ///     relevant tasks
  std::string description;

  /// License name or reference to bundled license file.
  std::string license;

  /// Environment requirements.
  ///   Constraints per Agent Skills specification:
  ///   - 1-500 characters if provided
  ///   - Should only be included if there are specific compatibility
  ///   requirements
  ///   - Can indicate intended product, required packages, etc.
  std::string compatibility;

  /// Arbitrary key-value mapping for additional metadata.
  /// Clients can use this to store additional properties not defined by the
  /// spec. It is recommended to keep key names unique to avoid conflicts.
  std::map<std::string, std::string> metadata;

  /// Tool names the skill recommends using.
  /// Warning: this is experimental.
  /// Constraints per Agent Skills specification:
  /// - Space-delimited list of tool names
  std::vector<std::string> allowed_tools;

  /// SKILL.md text conetnt
  std::string mdText;
};

class _SkillContext {
public:
  /// <path, data>
  std::map<std::string, _SkillMetadata> skillData{};

  /// <path, error>
  std::map<std::string, std::string> loadErrors{};
};

class SkillMiddlewareState : public BaseMiddlewareState {
public:
  std::string cacheFormatSkillPrompt;
  _SkillContext skillContext{};

  SkillMiddlewareState() {}
};

class SkillMiddlewareHandle
    : public BaseMiddlewareHandle<SkillMiddlewareState> {
protected:
  inline static constexpr std::string_view defSkillPromptTemplate =
      std::string_view{R"_(
## Skills System

You have access to a skills library that provides specialized capabilities and domain knowledge.

**Available Skills:**

{}

**How to Use Skills (Progressive Disclosure):**

Skills follow a **progressive disclosure** pattern - you see their name and description above, but only read full instructions when needed:

1. **Recognize when a skill applies**: Check if the user's task matches a skill's description
2. **Read the skill's full instructions**: Use toolcall `filesystem_read_text_file` on the path shown in the skill list above.
   Pass `line_limit=1000` since the default of 100 lines is too small for most skill files.
3. **Follow the skill's instructions**: SKILL.md contains step-by-step workflows, best practices, and examples
4. **Access supporting files**: Skills may include helper scripts, configs, or reference docs - use absolute paths

**When to Use Skills:**
- User's request matches a skill's domain (e.g., "analyse X" -> `data-analyse` skill)
- You need specialized knowledge or structured workflows
- A skill provides proven patterns for complex tasks

**Executing Skill Scripts:**
Skills may contain Python scripts or other executable files. Always use absolute paths from the skill list.

**Example Workflow:**

User: "Can you analyse the latest developments in quantum computing?"

1. Check available skills -> See "data-analyse" skill with its path
2. Read the full skill file by toolcall: `filesystem_read_text_file`
3. Follow the skill's research workflow (search -> organize -> synthesize)
4. Use any helper scripts with absolute paths

Remember: Skills make you more capable and consistent. When in doubt, check if a skill exists for the task!
)_"};

  /// 初始化后固定，按指定的文件夹扫描 SKILL.md
  const std::vector<std::string> initSkillDirPaths;

  _SkillContext skillCache{};
  bool haveLoadSkillMetadata = false;

public:
  SkillMiddlewareHandle(
      const std::vector<std::string> &in_initSkillDirPaths,
      std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
      : BaseMiddlewareHandle<SkillMiddlewareState>("SkillMiddlewareHandle",
                                                   in_agentContext),
        initSkillDirPaths(in_initSkillDirPaths) {}

  std::string formatSkillsMetadataList() {
    std::ostringstream oss;
    for (const auto &item : skillCache.skillData) {
      oss << fmt::format(
          R"(
- **{}** Skill: {}
  - compatibility: {}
  - allowed-tools: {}
  - Read file `{}` for full instructions
)",
          item.second.name, item.second.description, item.second.compatibility,
          agentxx::util::stringVectorJoin(item.second.allowed_tools),
          item.first + "/SKILL.md");
    }
    return oss.str();
  }

  /// <error, metadata>
  asio::awaitable<std::pair<std::string, agentxx::middleware::_SkillMetadata>>
  readSkillFile(std::string_view dirpath) {
    auto data =
        agentxx::middleware::_SkillMetadata{.dirpath = std::string{dirpath}};
    std::ifstream stream;
    try {
      stream.open(std::string{dirpath} + "/SKILL.md");
      if (!stream) {
        auto ec = std::error_code{errno, std::system_category()};
        throw std::runtime_error{
            fmt::format(R"(Can not open file. Error: {})", ec.message())};
      }
      auto filecontent = std::string{std::istreambuf_iterator<char>(stream),
                                     std::istreambuf_iterator<char>()};
      stream.close();
      const auto yamlDelimiter = std::string_view{"---"};
      auto yamlStart =
          filecontent.find_first_of(yamlDelimiter) + yamlDelimiter.size();
      auto yamlEnd = filecontent.find_first_of(yamlDelimiter, yamlStart);
      if (yamlStart >= 0 && yamlStart < yamlEnd &&
          yamlEnd < filecontent.size()) {
        // markdown
        data.mdText = filecontent.substr(yamlEnd + yamlDelimiter.size());

        while (yamlStart < yamlEnd && (filecontent[yamlStart] == '\r' ||
                                       filecontent[yamlStart] == '\n')) {
          yamlStart++;
        }
        while (yamlStart < yamlEnd &&
               (filecontent[yamlEnd] == '\r' || filecontent[yamlEnd] == '\n')) {
          yamlEnd--;
        }

        auto yamlContent = filecontent.substr(yamlStart, yamlEnd - yamlStart);
        auto metadata = YAML::Load(yamlContent);

        if (metadata["name"]) {
          data.name = metadata["name"].as<std::string>();
        }
        if (metadata["description"]) {
          data.description = metadata["description"].as<std::string>();
        }
        if (metadata["license"]) {
          data.license = metadata["license"].as<std::string>();
        }
        if (metadata["compatibility"]) {
          data.compatibility = metadata["compatibility"].as<std::string>();
        }
        if (metadata["allowed-tools"].IsScalar()) {
          data.allowed_tools = agentxx::util::strSplitCopid(
              metadata["allowed-tools"].as<std::string>(), ' ');
        }
        if (metadata["metadata"].IsMap()) {
          for (const auto &item : metadata["metadata"]) {
            data.metadata[item.first.as<std::string>()] =
                item.second.as<std::string>();
          }
        }
        co_return std::make_pair("", data);
      }
      co_return std::make_pair(
          "load skill metadata failed, can not found metadata in SKILL.md file",
          data);
    } catch (const std::exception &e) {
      co_return std::make_pair(e.what(), data);
    }
  }

  asio::awaitable<void>
  onAgentcallStartFunc(neograph::graph::NodeInput &in) override {
    if (initSkillDirPaths.empty()) {
      co_return;
    }
    // list skills / load skill metadata
    if (false == haveLoadSkillMetadata) {
      haveLoadSkillMetadata = true;

      skillCache.skillData.clear();
      skillCache.loadErrors.clear();
      auto skillQueue = std::vector<std::string>{initSkillDirPaths.begin(),
                                                 initSkillDirPaths.end()};
      for (size_t i = 0; i < skillQueue.size(); ++i) {
        auto &itempath = skillQueue[i];
        try {
          auto dir = std::filesystem::directory_entry{itempath};
          if (dir.is_directory()) {
            if (std::filesystem::is_regular_file(itempath + "/SKILL.md")) {
              // load skill metadata
              const auto [err, metadata] = co_await readSkillFile(itempath);
              if (err.empty()) {
                skillCache.skillData[itempath] = metadata;
              } else {
                skillCache.loadErrors[itempath] = err;
              }
            } else {
              // 添加子目录等待加载
              for (const auto &entity :
                   std::filesystem::directory_iterator(dir)) {
                if (entity.is_directory()) {
                  skillQueue.push_back(entity.path().string());
                }
              }
            }
          }
        } catch (const std::exception &e) {
          skillCache.loadErrors[itempath] = e.what();
        }
      }

      std::string content;
      for (const auto &item : skillCache.skillData) {
        content += fmt::format(
            "┣━ ✅ Load skill metadata success: `{}`({}): {}\n",
            item.second.name, item.second.dirpath, item.second.description);
      }
      for (const auto &item : skillCache.loadErrors) {
        content += fmt::format("┣━ ❌ Load skill metadata failed: {} | {}\n",
                               item.first, item.second);
      }
      XX_LOGD(R"_(
┏━━━━━━ Skill Load ━━━━━━┓
{}
┗━━━━━━ Skill Load ━━━━━━┛
)_",
              content);
    }
    co_return;
  }

  asio::awaitable<void>
  onModelcallStartFunc(neograph::graph::NodeInput &in) override {
    if (initSkillDirPaths.empty()) {
      co_return;
    }

    auto skillState = co_await getStateItem(in.ctx.thread_id);

    {
      auto agentCtxPtr = agentContext.lock();

      if (skillState->cacheFormatSkillPrompt.empty()) {
        // 生成 skill 系统提示词
        skillState->cacheFormatSkillPrompt = fmt::format(
            R"_(## Skills System

You have access to a skills library that provides specialized capabilities and domain knowledge.

**Available Skills:**

{}

{}
)_",
            formatSkillsMetadataList(),
            agentCtxPtr->agentConfig->prompt.systemSkillPrompt);
      }

      auto &appendSystemMsgList =
          agentCtxPtr->middlewareHandleContext
              ->getGraphDataItemValue<std::vector<std::string>>(
                  in.ctx.thread_id, agentxx::middleware::MiddlewareContext::
                                        graphDataKey_systemMessage);
      appendSystemMsgList.push_back(skillState->cacheFormatSkillPrompt);
    }
    co_return;
  }
};

} // namespace middleware
} // namespace agentxx