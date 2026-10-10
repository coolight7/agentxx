#include "agentxx/middlewares/skill.h"

#include "agentxx/util/exception.h"
#include "fmt/format.h"
#include "utilxx_base/string_util.h"
#include "yaml-cpp/yaml.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace agentxx {
namespace middleware {

/// 稳定附加段来源标识 (技能清单; 见 AgentContext::buildSystemPromptStable)
static constexpr std::string_view kStableSourceSkills = "skills";

std::string SkillMiddlewareHandle::formatSkillsMetadataList() {
    std::string oss;
    for (const auto& item : skillCache.skillData) {
        // 来源展示给模型 (计划 PRM-5): 同名技能已按优先级裁决, 每个名字一条
        oss += fmt::format(
            R"(
- **{}** Skill: {}
  - source: {}
  - compatibility: {}
  - allowed-tools: {}
  - Read file `{}` for full instructions
)",
            item.second.name,
            item.second.description,
            item.second.source.empty() ? std::string{"builtin"} : item.second.source,
            item.second.compatibility,
            utilxx_base::stringVectorJoin(item.second.allowed_tools),
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
            utilxx_base::autoConvertToUtf8(filecontent);
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
                        data.allowed_tools = utilxx_base::strSplitCopied(
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
    if (skillDirs.empty()) {
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
        // 待扫描队列: 从各扫描目录出发向下遍历 (子目录继承根目录的优先级与来源)
        auto skillQueue = std::vector<SkillDirEntry>{skillDirs.begin(), skillDirs.end()};
        // 成功加载的条目 (供"同名裁决"按优先级排序)
        struct LoadedEntry {
            int         priority = 0;
            std::string source;
            std::string dirpath;
        };
        std::vector<LoadedEntry> loadedOrder;
        for (size_t i = 0; i < skillQueue.size(); ++i) {
            const auto entry = skillQueue[i];
            // catchErrorAsync: 单个目录处理失败仅记录错误, 不中断整体加载
            co_await agentxx::util::catchErrorAsync<bool>(
                [&]() -> asio::awaitable<bool> {
                    auto dir = std::filesystem::directory_entry{entry.path};
                    if (dir.is_directory()) {
                        if (std::filesystem::is_regular_file(
                                fmt::format("{}/SKILL.md", entry.path)
                            )) {
                            // load skill metadata
                            const auto [err, metadata] = co_await readSkillFile(entry.path);
                            if (err.empty()) {
                                auto withSource       = metadata;
                                withSource.priority   = entry.priority;
                                withSource.source     = entry.source;
                                loadedData[entry.path] = std::move(withSource);
                                loadedOrder.push_back(LoadedEntry{
                                    .priority = entry.priority,
                                    .source   = entry.source,
                                    .dirpath  = entry.path
                                });
                            } else {
                                loadedErrors[entry.path] = err;
                            }
                        } else {
                            // 添加子目录等待加载 (继承本目录的优先级与来源)
                            for (const auto& entity : std::filesystem::directory_iterator(dir)) {
                                if (entity.is_directory()) {
                                    skillQueue.push_back(SkillDirEntry{
                                        .path     = entity.path().string(),
                                        .priority = entry.priority,
                                        .source   = entry.source
                                    });
                                }
                            }
                        }
                    }
                    co_return true;
                },
                [&](std::string errmsg) -> asio::awaitable<bool> {
                    loadedErrors[entry.path] = std::move(errmsg);
                    co_return false;
                }
            );
        }

        // ---- 同名裁决 (计划 PRM-5) ----
        // 优先级数字小的先注册 (会话/项目 < 用户 < 插件 < 内置): 同名技能只保留
        // 优先级最高的一份, 其余记为被遮蔽并记日志 —— 模型与 UI 看到的清单里
        // 每个名字只有一条, 且带来源
        shadowedSkills_.clear();
        if (!loadedOrder.empty()) {
            std::stable_sort(
                loadedOrder.begin(),
                loadedOrder.end(),
                [](const LoadedEntry& a, const LoadedEntry& b) {
                    return a.priority < b.priority;
                }
            );
            std::map<std::string, std::string, std::less<>> winnerOf; // name -> dirpath
            for (const auto& loaded : loadedOrder) {
                auto itData = loadedData.find(loaded.dirpath);
                if (itData == loadedData.end()) {
                    continue;
                }
                const auto& name = itData->second.name;
                if (name.empty()) {
                    continue;
                }
                if (auto itWin = winnerOf.find(name); itWin != winnerOf.end()) {
                    shadowedSkills_.push_back(
                        std::array<std::string, 3>{name, loaded.dirpath, itWin->second}
                    );
                    XX_LOGW(
                        "[skill] skill '{}' from '{}' shadowed by '{}' (existing source: {})",
                        name,
                        loaded.dirpath,
                        itWin->second,
                        itData->second.source
                    );
                    loadedData.erase(itData);
                } else {
                    winnerOf[name] = loaded.dirpath;
                }
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

    // insert
    auto skillState = co_await getStateItem(in.ctx.thread_id);

    {
        auto agentCtxPtr = agentContext.lock();

        // 缓存失效: 首次生成 / 资源纪元变更 (插件增删 skill 目录) 时重建
        if (skillState->cacheFormatSkillPrompt.empty()
            || skillState->cachedResourceEpoch != resourceEpoch) {
            // 生成 skill 系统提示词 — 仅动态技能清单，静态使用说明已移至
            // `appendSystemPrompts["skill"]` 通用扩展点，模型侧经 `systemPrompt +
            // appendSystemPrompts` 统一拼接
            skillState->cacheFormatSkillPrompt = fmt::format(
                R"_(## Skills System

You have access to a skills library that provides specialized capabilities and domain knowledge.

**Available Skills:**

{}
)_",
                formatSkillsMetadataList()
            );
            skillState->cachedResourceEpoch = resourceEpoch;
        }

        // 稳定附加段按来源写入 (计划 PRM-1): 同一来源每轮覆盖, 不再逐轮追加
        // - 技能清单随 system 消息一起进稳定前缀 (上游 KV/前缀缓存可命中);
        //   内容只在插件增删技能目录时改写, 改写会让该会话的前缀缓存失效一次
        auto& sections = agentCtxPtr->middlewareHandleContext->getGraphDataItemValue<
            std::map<std::string, std::string, std::less<>>>(
            in.ctx.thread_id,
            agentxx::middleware::MiddlewareContext::graphDataKey_appendSystemMessageStable
        );
        sections[std::string{kStableSourceSkills}] = skillState->cacheFormatSkillPrompt;
    }
    co_return;
}

SkillMiddlewareHandle::SkillMiddlewareHandle(
    const std::vector<std::string>&             in_initSkillDirPaths,
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext,
    int                                         in_priority,
    std::string_view                            in_source
) :
    BaseMiddlewareHandle<SkillMiddlewareState>("SkillMiddlewareHandle", in_agentContext) {
    skillDirs.reserve(in_initSkillDirPaths.size());
    for (const auto& path : in_initSkillDirPaths) {
        if (!path.empty()) {
            skillDirs.push_back(SkillDirEntry{
                .path     = path,
                .priority = in_priority,
                .source   = std::string{in_source}
            });
        }
    }
}

std::vector<std::string> SkillMiddlewareHandle::skillDirPathList() const {
    std::vector<std::string> paths;
    paths.reserve(skillDirs.size());
    for (const auto& dir : skillDirs) {
        paths.push_back(dir.path);
    }
    return paths;
}

void SkillMiddlewareHandle::addSkillDirs(std::vector<std::string> paths) {
    // 插件贡献的目录: 优先级低于项目/用户配置 (数字大 = 优先级低)
    addSkillDirs(std::move(paths), 100, "plugin");
}

void SkillMiddlewareHandle::addSkillDirs(
    std::vector<std::string> paths,
    int                      priority,
    std::string_view         source
) {
    bool changed = false;
    for (auto& p : paths) {
        if (p.empty()) {
            continue;
        }
        // 去重: 与 yaml 主配置/已注册目录重复时不重复扫描
        const bool exists = std::any_of(
            skillDirs.begin(),
            skillDirs.end(),
            [&](const SkillDirEntry& dir) {
                return dir.path == p;
            }
        );
        if (!exists) {
            skillDirs.push_back(SkillDirEntry{
                .path     = std::move(p),
                .priority = priority,
                .source   = std::string{source}
            });
            changed = true;
        }
    }
    if (!changed) {
        return;
    }
    // 按优先级排序 (数字小的优先; 同优先级保持注册顺序, 先注册的先扫描)
    std::stable_sort(
        skillDirs.begin(),
        skillDirs.end(),
        [](const SkillDirEntry& a, const SkillDirEntry& b) {
            return a.priority < b.priority;
        }
    );
    ++resourceEpoch;
    // 未加载过 → 首轮自然全量加载; 已加载 → 置重载标记下次轮次重扫
    needReloadSkillMetadata = haveLoadSkillMetadata;
}

void SkillMiddlewareHandle::removeSkillDirs(const std::vector<std::string>& paths) {
    bool changed = false;
    for (const auto& p : paths) {
        auto it = std::find_if(
            skillDirs.begin(),
            skillDirs.end(),
            [&](const SkillDirEntry& dir) {
                return dir.path == p;
            }
        );
        if (it != skillDirs.end()) {
            skillDirs.erase(it);
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
