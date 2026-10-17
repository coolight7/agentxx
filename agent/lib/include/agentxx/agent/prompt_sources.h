/// 运行时提示词来源注册表
///
/// 静态附加段 (配置作用域, 见 [AgentPrompt::appendSystemPrompts]) 与运行时文档段
/// (记忆文件内容 / 技能清单 / 将来的插件来源) **不合并存储**, 但共用本文件的
/// [PromptSectionView] 视图与统一的排序列; 运行时段的文本由本注册表按会话持有。
///
/// 三条约定 (改动前先读, 不要加回被否掉的做法):
/// - **没有任何文件监听 / 定时轮询 / 每轮 stat 后自动刷新**: 进程运行期间不因为
///   磁盘文件变化而改写系统提示词。每次内容变化都会让上游的前缀缓存失效一次,
///   这个代价必须由明确的时机 (压缩完成 / 资源变更 / 显式请求) 来支付。
/// - **唯一的刷新入口**是 model call 的 start 钩子
///   (见 `agentxx/middlewares/prompt_source_refresh.h`): 首次读取、压缩后刷新、
///   资源变更刷新、显式刷新全部集中在那里; `agentcall start` 不读写段落。
/// - 来源内部的 `mtime`/`size` 快照**只用于**"被要求刷新时省去读盘" (内容未变
///   直接返回上次文本), 不作为刷新触发条件。
#pragma once

// asio 来源选择必须在任何 asio 头之前引入: 本项目走 Boost.Asio
// (`NEOGRAPH_USE_BOOST_ASIO`), `asio` 是 neograph/define.h 建立的命名空间别名,
// 只包含 `asio/...` 头拿不到 `asio::awaitable`
#include "neograph/define.h"

#include "utilxx_base/json.h"
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::graph {
class CancelToken;
} // namespace neograph::graph

namespace agentxx {
namespace agent {

/// 段落作用域
enum class PromptSectionScope : uint8_t {
    Config,  ///< 配置作用域 (AgentPrompt::appendSystemPrompts, 全体会话共享)
    Session, ///< 会话作用域 (本注册表持有, 按 sessionId)
};

/// 段落落点
enum class PromptSectionSlot : uint8_t {
    Stable,  ///< 进 system 消息 (稳定前缀, 上游前缀缓存可命中)
    Dynamic, ///< 作为请求末尾独立消息 (每轮全额重发, 只放确实每轮都变的内容)
};

/// 需要重取来源内容的原因
enum class PromptRefreshReason : uint8_t {
    Initial = 0,      ///< 该会话首次取文本
    ResourcesChanged, ///< 资源变更 (插件增删记忆文件 / 技能目录, 配置重载)
    AfterCompaction,  ///< 上下文压缩完成
    Explicit,         ///< 外部显式请求 (客户端动作 / 插件 / FFI)
};

/// 取文本时的运行信息 (由刷新入口按会话填写)
struct PromptRefreshContext {
    std::string sessionId;
    /// 会话工作目录 (相对路径的解析基准; 取不到为空)
    std::string workDir;
    /// 本次刷新的原因 (注册表按会话内的标记决定, 这只是汇总值)
    PromptRefreshReason reason = PromptRefreshReason::Initial;
    /// 该会话是否首次取文本
    bool first = false;
    /// 轮次取消令牌 (可空): 取消后立即停止刷新, 保留旧文本
    std::shared_ptr<neograph::graph::CancelToken> cancelToken;
};

/// 一个运行时提示词来源
///
/// - `build` 返回 `nullopt` 表示"本次没有意见" (保留上次文本, 不算失败);
///   返回**空串**表示"该段落当前没有内容" (会清空对应段落, 用于文件被摘除等);
///   抛异常 / 超时同样保留上次文本
/// - `perSession = true` 的来源文本随会话变化 (如按会话工作目录解析的相对路径),
///   注册表按 sessionId 各存一份; false 只存一份, 全体会话共享
struct PromptSource {
    /// 段落键 (全局唯一, 如 "memory" / "skills")
    std::string key;
    /// 归属 (如 "core:memory"), 用于撤销与诊断
    std::string owner;
    /// 段内顺序 (小者在前; 同 order 按键名)
    int order = 0;
    /// 落点 (稳定段进 system 消息 / 动态段作为请求末尾消息)
    PromptSectionSlot slot = PromptSectionSlot::Stable;
    /// 文本是否随会话变化 (见上方说明)
    bool perSession = false;
    /// 压缩完成后是否也刷新 (内容与会话上下文体积无关的来源可以关掉)
    bool refreshOnCompaction = true;
    /// 单来源取文本的时间上限
    std::chrono::milliseconds timeout{3000};
    /// 取文本 (返回 nullopt = 本次没有意见)
    std::function<asio::awaitable<std::optional<std::string>>(const PromptRefreshContext&)> build;
};

/// 段落只读视图 (装配 / 快照 / 诊断都用它)
///
/// - 引用指向注册表或 `AgentPrompt` 内部存储, **只在本次调用内有效**,
///   不得跨 `co_await` 持有
struct PromptSectionView {
    std::string_view   key;
    std::string_view   text;
    int                order = 0;
    std::string_view   owner;
    PromptSectionScope scope = PromptSectionScope::Session;
    PromptSectionSlot  slot  = PromptSectionSlot::Stable;
};

/// 最近一次刷新的增量统计 (供关键指标记录; 取出即清零)
struct PromptRefreshStats {
    /// 实际执行的来源取文本次数
    uint64_t refreshes = 0;
    /// 其中内容发生变化的次数 (会让 system 前缀失效)
    uint64_t changedSections = 0;
    /// 失败/超时次数
    uint64_t failures = 0;
    /// 本次刷新耗时 (毫秒)
    int64_t lastMs = 0;
    /// 刷新后本会话可见的段落总字符数
    size_t sectionChars = 0;
};

/// 运行时提示词来源注册表
///
/// 线程模型: 状态只在 io 线程访问 (与段落进入请求装配同线程); `ensureFresh`
/// 在同一 io 线程串行执行, 内部的 `busy` 标记避免重入 (重入时只保留 dirty 标记)。
class PromptSourceRegistry {
public:

    PromptSourceRegistry()  = default;
    ~PromptSourceRegistry() = default;

    PromptSourceRegistry(const PromptSourceRegistry&)            = delete;
    PromptSourceRegistry& operator=(const PromptSourceRegistry&) = delete;

    /// 登记来源
    /// - 同 key 覆盖并记警告; owner 与已登记的相同时沿用已取到的文本
    ///   (插件启用后重新登记不会制造无意义的前缀变化)
    /// - 登记会让全部会话的下一次请求重取 (新来源本质上是一次资源变更)
    void registerSource(PromptSource src);

    /// 删除来源
    /// - `owner` 非空时必须与登记时的归属一致才删除 (插件禁用/卸载按 owner 撤销)
    ///
    /// - `return` 是否真的删除了一个来源
    bool removeSource(std::string_view key, std::string_view owner = {});

    /// 标记某会话需要刷新 (纯同步, 可在压缩路径安全调用; 不立即读盘)
    void markDirty(std::string_view sessionId, PromptRefreshReason reason);

    /// 标记全体会话需要刷新 (资源变更; 只置位, 实际读盘在各会话的请求装配前)
    void markAllDirty(PromptRefreshReason reason);

    /// 该会话是否有待处理的刷新标记 (诊断/测试)
    bool isDirty(std::string_view sessionId) const;

    /// 刷新 (唯一的读盘入口)
    /// - 无待处理标记且已初始化时立即返回 (无 I/O)
    /// - 逐来源取文本: 成功且与上次不同 → 更新 (计一次变化); 相同 → 不动
    ///   (不制造前缀变化); `nullopt` / 超时 / 异常 → 保留上次文本
    /// - 取消后立即停止, 保留旧文本与 dirty 标记 (下次再试)
    ///
    /// - `args`:
    ///     - [sessionId] 会话 id (空串按一个"默认会话"处理, 便于无会话场景)
    ///     - [ctx] 运行信息 (reason/first 由注册表按标记填写, 传入值不生效)
    ///     - [force] true 时忽略 dirty 标记, 强制重取全部来源
    asio::awaitable<void> ensureFresh(
        std::string_view            sessionId,
        const PromptRefreshContext& ctx,
        bool                        force = false
    );

    /// 稳定段落视图 (本注册表持有的稳定段; 按 (order, key) 排序)
    std::vector<PromptSectionView> stableSections(std::string_view sessionId) const;

    /// 动态段落视图 (按 (order, key) 排序)
    std::vector<PromptSectionView> dynamicSections(std::string_view sessionId) const;

    /// 取某来源的当前文本 (测试/诊断; 无内容或来源不存在时为空串)
    std::string sectionText(std::string_view key, std::string_view sessionId = {}) const;

    /// 已登记的来源数
    size_t sourceCount() const;

    /// 已记录的会话数 (诊断/测试)
    size_t sessionCount() const;

    /// 会话回收: 释放该会话持有的来源文本 (防止按会话累积)
    void dropSession(std::string_view sessionId);

    /// 取出并清零最近一次刷新的增量统计 (无刷新时全为 0)
    PromptRefreshStats takeRefreshStats();

    /// 诊断快照: 每来源的 key/owner/order/slot/per_session/last_reason/last_ms/chars/
    /// refreshes/changed/failures/last_error, 与会话数、总字符数 (不含正文)
    utilxx_base::Json snapshot() const;

private:

    /// 一份文本 (带"是否已取过"标记, 用于区分"空内容"与"还没取")
    struct TextSlot {
        std::string text;
        bool        built = false;
    };

    /// 单个来源的运行状态
    struct SourceState {
        PromptSource src;
        /// perSession == false 时的共享文本
        TextSlot shared;
        /// perSession == true 时按会话的文本
        std::map<std::string, TextSlot, std::less<>> sessionSlots;

        PromptRefreshReason lastReason = PromptRefreshReason::Initial;
        int64_t              lastMs    = 0;
        size_t               chars     = 0;
        uint64_t             refreshes = 0;
        uint64_t             changed   = 0;
        uint64_t             failures  = 0;
        std::string          lastError;

        /// 该会话对应的文本槽 (不存在时返回 nullptr)
        const TextSlot* findSlot(std::string_view sessionId) const;
        /// 该会话对应的文本槽 (按需创建)
        TextSlot& slotFor(std::string_view sessionId);

        /// 某会话当前可见的文本 (无内容时返回空串引用)
        const std::string& resolvedText(std::string_view sessionId) const;
    };

    /// 单个会话的刷新标记
    struct SessionState {
        bool     initialized    = false;
        /// 待处理原因位集 (1u << static_cast<uint32_t>(PromptRefreshReason))
        uint32_t pendingReasons = 0;
        /// 正在刷新 (重入时只保留 dirty 标记)
        bool busy = false;
    };

    /// 原因位集辅助
    static constexpr uint32_t reasonBit(PromptRefreshReason reason) noexcept {
        return 1u << static_cast<uint32_t>(reason);
    }

    /// 汇总本次刷新的原因 (按优先级选一个写进 build 的上下文)
    static PromptRefreshReason dominantReason(uint32_t bits, bool first) noexcept;

    /// 该来源在给定标记下是否需要重取
    bool needsBuild(
        const SourceState&  state,
        const SessionState& session,
        bool                force,
        std::string_view    sessionId
    ) const;

    /// 按 (order, key) 排序的来源指针表
    std::vector<SourceState*> orderedSources();

    /// 全部会话置脏 (来源表变更时)
    void markAllSessions(uint32_t bits);

    std::map<std::string, SourceState, std::less<>>  sources_;
    std::map<std::string, SessionState, std::less<>> sessions_;

    /// 共享来源 (perSession == false) 是否需要重取 (markAllDirty 置位)
    bool sharedDirty_ = true;

    /// 最近一次刷新的增量统计 (takeRefreshStats 取出即清零)
    PromptRefreshStats lastStats_;
};

} // namespace agent
} // namespace agentxx
