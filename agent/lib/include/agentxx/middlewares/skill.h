#pragma once

#include "agentxx/middlewares/middleware.h"
#include "asio/io_context.hpp"
#include <array>
#include <cstdint>
#include <map>
#include <neograph/neograph.h>
#include <string>
#include <string_view>
#include <utility>
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

    /// 所属扫描目录的优先级 (数字小的优先; 计划 PRM-5)
    int priority = 0;

    /// 来源标识 (config / project / user / plugin / builtin; 展示给模型与 UI)
    std::string source;
};

/// skill 扫描目录条目 (计划 PRM-5: 优先级与来源)
/// - `priority` 小的先注册; 同名技能由优先级小(高)的一个生效,
///   其余记为被遮蔽 (shadowed) 供诊断展示
/// - 约定优先级: 会话/项目 0, 用户配置 10, 插件 100, 内置 200
struct SkillDirEntry {
    std::string path;
    int         priority = 0;
    std::string source   = "config";
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
    /// 生成缓存时的资源纪元 (SkillMiddlewareHandle::resourceEpoch;
    /// 插件运行期增删 skill 目录后纪元递增, 缓存据此失效重建)
    uint64_t      cachedResourceEpoch = 0;
    _SkillContext skillContext{};

    SkillMiddlewareState() {}
};

class SkillMiddlewareHandle : public BaseMiddlewareHandle<SkillMiddlewareState> {
protected:

    inline static constexpr std::string_view defSkillPromptTemplate = std::string_view{R"_(
## Skills System

You have access to a skills library that provides specialized capabilities and domain knowledge.

**Available Skills:**

{}

**How to Use Skills (Progressive Disclosure):**

Skills follow a **progressive disclosure** pattern - you see their name and description above, but only read full instructions when needed:

1. **Recognize when a skill applies**: Check if the user's task matches a skill's description
2. **Read the skill's full instructions**: Use toolcall `agentxx_filesystem_read` on the path shown in the skill list above.
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
2. Read the full skill file by toolcall: `agentxx_filesystem_read`
3. Follow the skill's research workflow (search -> organize -> synthesize)
4. Use any helper scripts with absolute paths

Remember: Skills make you more capable and consistent. When in doubt, check if a skill exists for the task!
)_"};

    /// skill 扫描目录列表 (可变: 支持插件运行期追加/摘除 —— 见 addSkillDirs/
    /// removeSkillDirs; 仅 io 线程读写, 与轮次执行同线程无锁)
    /// - 顺序即优先级顺序 (先注册的优先); 同名技能取最先注册的目录
    std::vector<SkillDirEntry> skillDirs;

    _SkillContext skillCache{};
    bool          haveLoadSkillMetadata = false;

    /// 被同名高优先级技能遮蔽的技能 ({技能名, 被遮蔽目录, 生效目录}; 计划 PRM-5)
    std::vector<std::array<std::string, 3>> shadowedSkills_;

    /// 是否需要重扫 (插件运行期增删扫描目录后置位; 下次 onAgentcallStartFunc 全量重扫)
    bool needReloadSkillMetadata = false;
    /// 资源纪元 (扫描目录变更时递增; 各线程状态缓存据此失效重建)
    /// - 初始为 1 (状态侧 cachedResourceEpoch 默认 0, 保证首轮必定生成缓存)
    uint64_t resourceEpoch = 1;

public:

    SkillMiddlewareHandle(
        const std::vector<std::string>&             in_initSkillDirPaths,
        std::weak_ptr<agentxx::agent::AgentContext> in_agentContext,
        int                                         in_priority = 0,
        std::string_view                            in_source   = "config"
    );

    std::string formatSkillsMetadataList();

    /// <error, metadata>
    asio::awaitable<std::pair<std::string, agentxx::middleware::_SkillMetadata>>
        readSkillFile(std::string_view dirpath);

    asio::awaitable<void> onAgentcallStartFunc(neograph::graph::NodeInput& in) override;

    // ---------------- 插件资源扩展: 动态增删扫描目录 (仅 io 线程调用) ----------------

    /// 动态追加 skill 扫描目录 (插件声明/运行时注册)
    /// - 元数据未加载: 直接并入列表, 首轮懒加载自然包含
    /// - 已加载: 置重载标记 + 递增纪元, 下次轮次开始全量重扫 (缓存失效自愈)
    /// - 默认按"插件"来源与优先级 (插件贡献的目录优先级低于项目/用户配置)
    void addSkillDirs(std::vector<std::string> paths);

    /// 追加扫描目录并指定优先级/来源 (计划 PRM-5; 数字小的优先)
    void addSkillDirs(std::vector<std::string> paths, int priority, std::string_view source);

    /// 摘除扫描目录并置重载标记 (io 线程; 缓存随下次轮次重建)
    void removeSkillDirs(const std::vector<std::string>& paths);

    /// 当前扫描目录列表 (测试/调试用)
    std::vector<std::string> skillDirPathList() const;

    /// 被同名高优先级技能遮蔽的技能 (诊断/UI 展示用)
    /// - 每项: {技能名, 被遮蔽目录, 生效目录}
    const std::vector<std::array<std::string, 3>>& shadowedSkills() const {
        return shadowedSkills_;
    }
};

} // namespace middleware
} // namespace agentxx