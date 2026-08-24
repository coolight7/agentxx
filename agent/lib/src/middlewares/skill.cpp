#include "agentxx/middlewares/skill.h"

#include "agentxx/util/exception.h"
#include "agentxx/util/string_util.h"
#include "fmt/format.h"
#include "yaml-cpp/yaml.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace agentxx {
namespace middleware {

std::string SkillMiddlewareHandle::formatSkillsMetadataList() {
    std::string oss;
    for (const auto& item : skillCache.skillData) {
        oss += fmt::format(
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
            fmt::format("{}/SKILL.md", item.first)
        );
    }
    return oss;
}

asio::awaitable<std::pair<std::string, agentxx::middleware::_SkillMetadata>>
    SkillMiddlewareHandle::readSkillFile(std::string_view dirpath) {
    auto data = agentxx::middleware::_SkillMetadata{.dirpath = std::string{dirpath}};
    // catchErrorAsync: 读取/解析失败时返回错误消息; 取消类异常原样抛出
    co_return co_await agentxx::util::catchErrorAsync<std::pair<std::string, _SkillMetadata>>(
        [&]() -> asio::awaitable<std::pair<std::string, _SkillMetadata>> {
            std::ifstream stream;
            stream.open(fmt::format("{}/SKILL.md", dirpath));
            if (!stream) {
                auto ec = std::error_code{errno, std::system_category()};
                throw std::runtime_error{
                    fmt::format(R"(Can not open file. Error: {})", ec.message())
                };
            }
            auto filecontent = std::string{
                std::istreambuf_iterator<char>(stream),
                std::istreambuf_iterator<char>()
            };
            agentxx::util::autoConvertToUtf8(filecontent);
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
                            data.metadata[item.first.as<std::string>()]
                                = item.second.as<std::string>();
                        }
                    }
                    co_return std::make_pair("", data);
                }
            }
            co_return std::make_pair(
                "load skill metadata failed, can not find `metadata` in SKILL.md file",
                data
            );
        },
        [&data](std::string errmsg) -> asio::awaitable<std::pair<std::string, _SkillMetadata>> {
            co_return std::make_pair(std::move(errmsg), data);
        }
    );
}

asio::awaitable<void> SkillMiddlewareHandle::onAgentcallStartFunc(neograph::graph::NodeInput& in) {
    if (skillDirPaths.empty()) {
        co_return;
    }

    // list skills / load skill metadata
    // - haveLoadSkillMetadata: 首轮懒加载
    // - needReloadSkillMetadata: 插件运行期增删扫描目录后置位, 全量重扫自愈缓存
    //   (co_await 挂起期间目录可能再次变更 —— 重扫完成后纪元比对不匹配的线程
    //   状态会在下轮重建, 最终一致)
    if (false == haveLoadSkillMetadata || needReloadSkillMetadata) {
        // 先置位防止并发会话重复进入加载; 但加载结果先写入局部变量, 完成后再整体替换 skillCache。
        // 加载循环含 co_await, 挂起期间其他会话若读 skillCache 只会看到空缓存(整体赋值尚未发生),
        // 不会看到半加载状态 (单线程协程模型下整体赋值不被打断)。
        haveLoadSkillMetadata   = true;
        needReloadSkillMetadata = false;

        decltype(skillCache.skillData)  loadedData;
        decltype(skillCache.loadErrors) loadedErrors;
        auto skillQueue = std::vector<std::string>{skillDirPaths.begin(), skillDirPaths.end()};
        for (size_t i = 0; i < skillQueue.size(); ++i) {
            auto& itempath = skillQueue[i];
            // catchErrorAsync: 单个目录处理失败仅记录错误, 不中断整体加载
            co_await agentxx::util::catchErrorAsync<bool>(
                [&]() -> asio::awaitable<bool> {
                    auto dir = std::filesystem::directory_entry{itempath};
                    if (dir.is_directory()) {
                        if (std::filesystem::is_regular_file(fmt::format("{}/SKILL.md", itempath)
                            )) {
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
                    co_return true;
                },
                [&](std::string errmsg) -> asio::awaitable<bool> {
                    loadedErrors[itempath] = std::move(errmsg);
                    co_return false;
                }
            );
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

    // insert
    auto skillState = co_await getStateItem(in.ctx.thread_id);

    {
        auto agentCtxPtr = agentContext.lock();

        // 缓存失效: 首次生成 / 资源纪元变更 (插件增删 skill 目录) 时重建
        if (skillState->cacheFormatSkillPrompt.empty()
            || skillState->cachedResourceEpoch != resourceEpoch) {
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
            skillState->cachedResourceEpoch = resourceEpoch;
        }

        auto& appendSystemMsgList
            = agentCtxPtr->middlewareHandleContext->getGraphDataItemValue<std::vector<std::string>>(
                in.ctx.thread_id,
                agentxx::middleware::MiddlewareContext::graphDataKey_appendSystemMessage
            );
        appendSystemMsgList.push_back(skillState->cacheFormatSkillPrompt);
    }
    co_return;
}

void SkillMiddlewareHandle::addSkillDirs(std::vector<std::string> paths) {
    bool changed = false;
    for (auto& p : paths) {
        if (p.empty()) {
            continue;
        }
        // 去重: 与 yaml 主配置/已注册目录重复时不重复扫描
        if (std::find(skillDirPaths.begin(), skillDirPaths.end(), p) == skillDirPaths.end()) {
            skillDirPaths.push_back(std::move(p));
            changed = true;
        }
    }
    if (!changed) {
        return;
    }
    ++resourceEpoch;
    // 未加载过 → 首轮自然全量加载; 已加载 → 置重载标记下次轮次重扫
    needReloadSkillMetadata = haveLoadSkillMetadata;
}

void SkillMiddlewareHandle::removeSkillDirs(const std::vector<std::string>& paths) {
    bool changed = false;
    for (const auto& p : paths) {
        auto it = std::find(skillDirPaths.begin(), skillDirPaths.end(), p);
        if (it != skillDirPaths.end()) {
            skillDirPaths.erase(it);
            changed = true;
        }
    }
    if (!changed) {
        return;
    }
    ++resourceEpoch;
    needReloadSkillMetadata = haveLoadSkillMetadata;
}

} // namespace middleware
} // namespace agentxx
