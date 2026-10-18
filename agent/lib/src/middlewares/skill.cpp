#include "agentxx/middlewares/skill.h"

#include "agentxx/agent/context.h"
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

namespace {

/// 取文件时间戳 (mtime 计数 + 字节数); 失败返回 false
bool readFileStamp(const std::string& path, uint64_t& mtime, uint64_t& size) {
    std::error_code ec;
    auto            nativePath = utilxx_base::utf8ToPath(path);
    const auto      bytes      = std::filesystem::file_size(nativePath, ec);
    if (ec) {
        return false;
    }
    size = static_cast<uint64_t>(bytes);
    const auto writeTime = std::filesystem::last_write_time(nativePath, ec);
    if (ec) {
        return false;
    }
    mtime = static_cast<uint64_t>(writeTime.time_since_epoch().count());
    return true;
}

} // namespace

std::string SkillMiddlewareHandle::formatSkillsMetadataList() {
    std::string oss;
    for (const auto& item : skillCache.skillData) {
        // 来源展示给模型: 同名技能已按优先级裁决, 每个名字一条
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

asio::awaitable<bool> SkillMiddlewareHandle::scanSkillsAsync() {
    // ---- 1. 遍历扫描目录, 收集"含 SKILL.md 的目录"及其时间戳 ----
    // 待扫描队列: 从各扫描目录出发向下遍历 (子目录继承根目录的优先级与来源)
    struct Candidate {
        std::string skillDir;
        int         priority = 0;
        std::string source;
    };
    auto                          skillQueue = std::vector<SkillDirEntry>{skillDirs.begin(), skillDirs.end()};
    std::vector<Candidate>        candidates;
    std::map<std::string, std::string> walkErrors;
    std::map<std::string, std::pair<uint64_t, uint64_t>, std::less<>> newSnapshot;

    for (size_t i = 0; i < skillQueue.size(); ++i) {
        const auto entry = skillQueue[i];
        // catchErrorAsync: 单个目录处理失败仅记录错误, 不中断整体扫描
        co_await agentxx::util::catchErrorAsync<bool>(
            [&]() -> asio::awaitable<bool> {
                auto dir = std::filesystem::directory_entry{utilxx_base::utf8ToPath(entry.path)};
                if (dir.is_directory()) {
                    const auto skillFile = fmt::format("{}/SKILL.md", entry.path);
                    if (std::filesystem::is_regular_file(utilxx_base::utf8ToPath(skillFile))) {
                        uint64_t mtime = 0;
                        uint64_t size  = 0;
                        if (readFileStamp(skillFile, mtime, size)) {
                            newSnapshot[entry.path] = {mtime, size};
                            candidates.push_back(Candidate{
                                .skillDir = entry.path,
                                .priority = entry.priority,
                                .source   = entry.source,
                            });
                        } else {
                            walkErrors[entry.path] = "SKILL.md not readable";
                        }
                    } else {
                        // 添加子目录等待扫描 (继承本目录的优先级与来源)
                        for (const auto& entity :
                             std::filesystem::directory_iterator(utilxx_base::utf8ToPath(entry.path))) {
                            if (entity.is_directory()) {
                                skillQueue.push_back(SkillDirEntry{
                                    .path     = entity.path().string(),
                                    .priority = entry.priority,
                                    .source   = entry.source,
                                });
                            }
                        }
                    }
                }
                co_return true;
            },
            [&](std::string errmsg) -> asio::awaitable<bool> {
                walkErrors[entry.path] = std::move(errmsg);
                co_return false;
            }
        );
    }

    // ---- 2. 结构未变 (同一批 SKILL.md, 时间戳与字节数都相同) → 不重读 ----
    // 注意: 这里只是"省去读盘", 不是刷新触发条件 (刷新由 model call 的 start 钩子决定)
    if (newSnapshot == scanSnapshot_ && !skillCache.skillData.empty()) {
        co_return false;
    }
    if (newSnapshot == scanSnapshot_ && candidates.empty() && walkErrors.empty()) {
        co_return false;
    }

    // ---- 3. 逐目录解析 SKILL.md ----
    decltype(skillCache.skillData)  loadedData;
    decltype(skillCache.loadErrors) loadedErrors = std::move(walkErrors);
    struct LoadedEntry {
        int         priority = 0;
        std::string source;
        std::string dirpath;
    };
    std::vector<LoadedEntry> loadedOrder;
    for (const auto& candidate : candidates) {
        // catchErrorAsync: 单个技能解析失败仅记录错误, 不影响其他技能
        co_await agentxx::util::catchErrorAsync<bool>(
            [&]() -> asio::awaitable<bool> {
                // 结果接成具名变量而不是结构化绑定: GCC 在 -O3 下会把协程帧里的
                // `auto [a, b] = co_await ...` 误报为 -Wmaybe-uninitialized
                auto loaded = co_await readSkillFile(candidate.skillDir);
                if (loaded.first.empty()) {
                    auto withSource                = std::move(loaded.second);
                    withSource.priority            = candidate.priority;
                    withSource.source              = candidate.source;
                    loadedData[candidate.skillDir] = std::move(withSource);
                    loadedOrder.push_back(LoadedEntry{
                        .priority = candidate.priority,
                        .source   = candidate.source,
                        .dirpath  = candidate.skillDir,
                    });
                } else {
                    loadedErrors[candidate.skillDir] = std::move(loaded.first);
                }
                co_return true;
            },
            [&](std::string errmsg) -> asio::awaitable<bool> {
                loadedErrors[candidate.skillDir] = std::move(errmsg);
                co_return false;
            }
        );
    }

    // ---- 4. 同名裁决 ----
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

    // 扫描完成, 整体替换缓存与快照
    skillCache.skillData  = std::move(loadedData);
    skillCache.loadErrors = std::move(loadedErrors);
    scanSnapshot_         = std::move(newSnapshot);

    // 只在首次 / 有失败项时输出加载块 (避免每次刷新都刷日志)
    if (!skillCache.skillData.empty() || !skillCache.loadErrors.empty()) {
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
    co_return true;
}

asio::awaitable<std::optional<std::string>> SkillMiddlewareHandle::buildSectionAsync() {
    if (skillDirs.empty()) {
        // 没有配置扫描目录: 空串表示"该段落没有内容"
        co_return std::string{};
    }
    const bool changed = co_await scanSkillsAsync();
    if (!changed && !cachedListText_.empty()) {
        co_return cachedListText_;
    }
    // 生成 skill 系统提示词 — 仅动态技能清单, 静态使用说明已移至
    // `appendSystemPrompts["skill"]` 通用扩展点, 模型侧经 `systemPrompt +
    // appendSystemPrompts` 统一拼接
    cachedListText_ = fmt::format(
        R"_(## Skills System

You have access to a skills library that provides specialized capabilities and domain knowledge.

**Available Skills:**

{}
)_",
        formatSkillsMetadataList()
    );
    co_return cachedListText_;
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

    auto ctx = in_agentContext.lock();
    if (!ctx || !ctx->promptSources) {
        XX_LOGW("[skill] no prompt source registry, skill list will not be injected");
        return;
    }
    promptSources_ = ctx->promptSources;

    // 技能清单与会话无关 (目录来自配置/插件), 全体会话共享一份文本
    ctx->promptSources->registerSource(agentxx::agent::PromptSource{
        .key                 = std::string{kSectionKey},
        .owner               = std::string{kSectionOwner},
        .order               = 200,
        .slot                = agentxx::agent::PromptSectionSlot::Stable,
        .perSession          = false,
        .refreshOnCompaction = true,
        .timeout             = std::chrono::milliseconds{5000},
        .build =
            [this](const agentxx::agent::PromptRefreshContext& /*rc*/)
            -> asio::awaitable<std::optional<std::string>> {
            co_return co_await buildSectionAsync();
        },
    });
}

SkillMiddlewareHandle::~SkillMiddlewareHandle() {
    // 撤销来源: 本对象销毁后 build 里捕获的 `this` 不再有效
    if (auto registry = promptSources_.lock()) {
        registry->removeSource(kSectionKey, kSectionOwner);
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
    // 资源变更: 只标记, 实际重扫在下一个请求装配前 (model call 的 start 钩子)
    if (auto registry = promptSources_.lock()) {
        registry->markAllDirty(agentxx::agent::PromptRefreshReason::ResourcesChanged);
    }
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
    if (auto registry = promptSources_.lock()) {
        registry->markAllDirty(agentxx::agent::PromptRefreshReason::ResourcesChanged);
    }
}

} // namespace middleware
} // namespace agentxx
