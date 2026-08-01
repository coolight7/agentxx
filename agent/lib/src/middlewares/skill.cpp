#include "agentxx/middlewares/skill.h"

#include "agentxx/util/string_util.h"
#include "fmt/format.h"
#include "yaml-cpp/yaml.h"
#include <filesystem>
#include <fstream>
#include <sstream>

namespace agentxx {
namespace middleware {

std::string SkillMiddlewareHandle::formatSkillsMetadataList() {
    std::ostringstream oss;
    for (const auto& item : skillCache.skillData) {
        oss << fmt::format(
            R"(
- **{}** Skill: {}
  - compatibility: {}
  - allowed-tools: {}
  - Read file `{}` for full instructions
)",
            item.second.name,
            item.second.description,
            item.second.compatibility,
            agentxx::util::stringVectorJoin(item.second.allowed_tools),
            item.first + "/SKILL.md"
        );
    }
    return oss.str();
}

asio::awaitable<std::pair<std::string, agentxx::middleware::_SkillMetadata>>
    SkillMiddlewareHandle::readSkillFile(std::string_view dirpath) {
    auto          data = agentxx::middleware::_SkillMetadata{.dirpath = std::string{dirpath}};
    std::ifstream stream;
    try {
        stream.open(fmt::format("{}/SKILL.md", dirpath));
        if (!stream) {
            auto ec = std::error_code{errno, std::system_category()};
            throw std::runtime_error{fmt::format(R"(Can not open file. Error: {})", ec.message())};
        }
        auto filecontent
            = std::string{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        stream.close();
        const auto yamlDelimiter = std::string_view{"---"};
        auto       yamlStart     = filecontent.find(yamlDelimiter);
        if (yamlStart != filecontent.npos) {
            auto yamlEnd = filecontent.find(yamlDelimiter, yamlStart + yamlDelimiter.size());
            if (yamlEnd != filecontent.npos && yamlStart + yamlDelimiter.size() < yamlEnd) {
                yamlStart += yamlDelimiter.size();
                // markdown
                data.mdText = filecontent.substr(yamlEnd + yamlDelimiter.size());

                while (yamlStart < yamlEnd
                       && (filecontent[yamlStart] == '\r' || filecontent[yamlStart] == '\n')) {
                    yamlStart++;
                }
                while (yamlStart < yamlEnd
                       && (filecontent[yamlEnd] == '\r' || filecontent[yamlEnd] == '\n')) {
                    yamlEnd--;
                }

                auto yamlContent = filecontent.substr(yamlStart, yamlEnd - yamlStart);
                auto metadata    = YAML::Load(yamlContent);

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
                        metadata["allowed-tools"].as<std::string>(),
                        ' '
                    );
                }
                if (metadata["metadata"].IsMap()) {
                    for (const auto& item : metadata["metadata"]) {
                        data.metadata[item.first.as<std::string>()] = item.second.as<std::string>();
                    }
                }
                co_return std::make_pair("", data);
            }
        }
        co_return std::make_pair(
            "load skill metadata failed, can not find `metadata` in SKILL.md file",
            data
        );
    } catch (const std::exception& e) {
        co_return std::make_pair(e.what(), data);
    }
}

asio::awaitable<void> SkillMiddlewareHandle::onAgentcallStartFunc(neograph::graph::NodeInput& in) {
    if (initSkillDirPaths.empty()) {
        co_return;
    }
    // list skills / load skill metadata
    if (false == haveLoadSkillMetadata) {
        // 先置位防止并发会话重复进入加载; 但加载结果先写入局部变量, 完成后再整体替换 skillCache。
        // 加载循环含 co_await, 挂起期间其他会话若读 skillCache 只会看到空缓存(整体赋值尚未发生),
        // 不会看到半加载状态 (单线程协程模型下整体赋值不被打断)。
        haveLoadSkillMetadata = true;

        decltype(skillCache.skillData)  loadedData;
        decltype(skillCache.loadErrors) loadedErrors;
        auto skillQueue
            = std::vector<std::string>{initSkillDirPaths.begin(), initSkillDirPaths.end()};
        for (size_t i = 0; i < skillQueue.size(); ++i) {
            auto& itempath = skillQueue[i];
            try {
                auto dir = std::filesystem::directory_entry{itempath};
                if (dir.is_directory()) {
                    if (std::filesystem::is_regular_file(itempath + "/SKILL.md")) {
                        // load skill metadata
                        const auto [err, metadata] = co_await readSkillFile(itempath);
                        if (err.empty()) {
                            loadedData[itempath] = metadata;
                        } else {
                            loadedErrors[itempath] = err;
                        }
                    } else {
                        // 添加子目录等待加载
                        for (const auto& entity : std::filesystem::directory_iterator(dir)) {
                            if (entity.is_directory()) {
                                skillQueue.push_back(entity.path().string());
                            }
                        }
                    }
                }
            } catch (const std::exception& e) {
                loadedErrors[itempath] = e.what();
            }
        }

        // 加载完成, 整体替换缓存
        skillCache.skillData  = std::move(loadedData);
        skillCache.loadErrors = std::move(loadedErrors);

        std::string content;
        for (const auto& item : skillCache.skillData) {
            content += fmt::format(
                "┣━ ✅ Load skill metadata success: `{}`({}): {}\n",
                item.second.name,
                item.second.dirpath,
                item.second.description
            );
        }
        for (const auto& item : skillCache.loadErrors) {
            content += fmt::format(
                "┣━ ❌ Load skill metadata failed: {} | {}\n",
                item.first,
                item.second
            );
        }
        XX_LOGD(
            R"_(
┏━━━━━━ Skill Load ━━━━━━┓
{}
┗━━━━━━ Skill Load ━━━━━━┛
)_",
            content
        );
    }
    co_return;
}

asio::awaitable<void> SkillMiddlewareHandle::onModelcallStartFunc(neograph::graph::NodeInput& in) {
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
                agentCtxPtr->agentConfig->prompt.systemSkillPrompt
            );
        }

        auto& appendSystemMsgList
            = agentCtxPtr->middlewareHandleContext->getGraphDataItemValue<std::vector<std::string>>(
                in.ctx.thread_id,
                agentxx::middleware::MiddlewareContext::graphDataKey_systemMessage
            );
        appendSystemMsgList.push_back(skillState->cacheFormatSkillPrompt);
    }
    co_return;
}

} // namespace middleware
} // namespace agentxx
