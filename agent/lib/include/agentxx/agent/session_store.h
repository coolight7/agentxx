#pragma once

#include "agentxx/agent/conversation_types.h"
#include "agentxx/agent/writer_lease.h"
#include "agentxx/util/sqlite.h"
#include "utilxx_base/json.h"
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace agent {

/// 会话数据 SQLite 持久化 (按 sessionId 分目录)
///
/// 目录结构: {root}/{sanitizedSessionId}/
///   - session.db  会话全量状态 (单库):
///                   view_message 表  展示历史 (append-only, 每消息一行 JSON)
///                   llm_context 表  LLM 上下文消息 (单行整体替换, 每轮结束保存)
///                   meta 表          msgIdCounter / schema_version / session 元数据
///                   store 表         agentxx_share_store KV 存储 id(自增) -> value
///                                   (内存只保留少量最近使用的条目, 其余按需读取)
///                   usage 表         每次模型调用的用量记录 (成功与失败各一行)
///   - .writer.lock  写租约锁文件 (见 [SessionWriterLease]):
///                   同一会话目录同时只允许一个进程写入, 第二个进程写操作
///                   明确失败而不是互相覆盖; 读操作不取锁
///
/// schema 版本 (计划 STO-2): meta.schema_version 记录结构版本; 打开写连接时
/// 按相邻步骤迁移 (每步独立事务、幂等、迁移前备份 session.db.bak.v{n});
/// 库版本高于本程序支持的版本时拒绝打开, 避免新数据被旧程序改坏。
///
/// 默认 root: {dataDir}/sqlite/sessions/ (dataDir 为空时 ~/.agentxx/,
/// 取不到用户主目录时回退系统临时目录), 数据目录统一由 client 经
/// AgentConfig::dataDir 重定向。
/// 单库: share_store 与消息历史生命周期一致 (随 session 创建/删除),
///       且会话级 WAL 写入已由互斥锁串行, 合并后减少文件数、简化备份、
///       便于事务性一致与目录枚举。
///
/// 线程安全: 内部互斥锁保护所有 DB 访问; 常规使用下调用发生在 agent io 线程
/// (Session 绑定线程 / 工具执行), 锁仅在多线程并发访问时生效, 开销可忽略
class SessionStore {
public:

    /// 当前支持的最新 schema 版本 (新增结构变更时递增并追加迁移步骤)
    static constexpr int kSchemaVersion = 3;

    /// - [rootDir] 数据根目录; 为空使用默认 {dataDir}/sqlite/sessions/
    ///   (dataDir 为空时 ~/.agentxx/, 取不到用户主目录时回退系统临时目录)
    /// - [enableWriterLease] 是否对写连接启用跨进程写租约 (默认开启);
    ///   仅嵌入方在明确不需要互斥的场景才关闭
    explicit SessionStore(std::string rootDir = "", bool enableWriterLease = true);

    // ---- 会话消息状态 (session.db) ----

    struct LoadedSession {
        std::vector<ViewMessage> viewMessages;
        utilxx_base::Json        llmMessages = utilxx_base::Json::array();
        /// 恢复后的 msg id 计数器 (保证新消息 id 不与已存消息冲突)
        uint64_t msgIdCounter = 0;
        /// 库内最大展示消息序号 (计划 STO-4; 供重连增量补拉与继续编号)
        uint64_t lastViewSeq = 0;
    };

    /// 加载指定 session 的会话消息状态; 无数据/打开失败时返回空结构 (仅记日志)
    LoadedSession loadSession(std::string_view sessionId);

    /// 读取展示历史中序号大于 [afterSeq] 的消息 (计划 STO-4: 断线增量补拉)
    ///
    /// - 返回 `{seq, ViewMessage}` 列表, 按 seq 升序; 消息 id 为空的老数据也返回
    ///   (调用方按需回退全量同步)
    /// - `limit > 0` 时最多返回 limit 条 (超出时调用方应回退全量/尾窗同步,
    ///   避免一次补拉过大)
    /// - 只读连接 (不取写租约); 无数据/打开失败返回空
    using ViewMessageRow = SequencedViewMessage;

    std::vector<ViewMessageRow>
        loadViewMessagesAfter(std::string_view sessionId, uint64_t afterSeq, size_t limit = 0);

    /// 列举全部持久化会话的摘要 (供会话选择弹窗), 按最近活动时间降序
    /// - 扫描根目录下各 session 目录, 以独立临时连接读取 session.db meta 表
    ///   (sessionId/title/lastActiveMs); 老数据无 meta 时回退目录名作 sessionId
    /// - 打开/读取失败仅记日志并跳过该目录
    std::vector<SessionInfo> listSessions();

    /// 分页列举持久化会话摘要 (keyset 游标分页, 按最近活动时间降序, 最新在前)
    ///
    /// 背景: 会话数量可能很大, 一次性扫描/传输/渲染全量列表开销高; 客户端
    /// (TUI 会话弹窗) 先加载最新一页, 用户浏览到末尾时按游标继续拉取。
    /// - 游标语义: 返回排序位置严格位于 (beforeMs, beforeId) 之后的至多 limit 条
    ///   (排序: lastActiveMs 降序, 相同时间按 sessionId 升序; 与 listSessions 一致),
    ///   即把游标视为"上一页最后一条", 天然规避服务端活跃会话位移导致的
    ///   offset 分页重复/遗漏问题
    /// - beforeMs <= 0 表示从最新开始 (首页); limit == 0 等价 listSessions 全量
    /// - 实现: 两阶段扫描 —— 阶段 1 仅 stat 各目录 session.db/-wal 的修改时间
    ///   (不打开数据库) 获得近似活动顺序与总数; 阶段 2 按该顺序逐个打开 DB 读
    ///   精确 meta 收集。mtime 与 lastActiveMs 强相关 (提交时刻恒 ≥ 消息开始
    ///   时间戳) 但不完全一致 (tool 结果回填等只更新文件不改 meta), 故 mtime 仅
    ///   作读取顺序启发、绝不据此跳过目录; 已收满一页且剩余目录的有效 mtime
    ///   严格早于页边界时可安全早停 (更早 mtime 的会话必然排在边界之后)
    struct SessionListPage {
        std::vector<SessionInfo> sessions;       ///< 本页条目 (已按序排列)
        uint64_t                 totalCount = 0; ///< 当前持久化会话总数 (供 x/y 展示)
        bool                     hasMore    = false; ///< 是否可能还有未加载的更早会话
    };

    SessionListPage listSessionsPage(int64_t beforeMs, std::string_view beforeId, uint32_t limit);

    /// 追加一条展示历史消息 (事务: 消息 + msgIdCounter + viewSeqCounter 一起提交)
    /// - msgIdCounter 为追加后会话的计数 (新消息 id 序号), 供重启恢复
    /// - seq 为该消息的展示历史持久化序号 (见 `LoadedSession::lastViewSeq`);
    ///   0 表示由库内自增 (老调用方)
    /// - 失败仅记录日志, 不影响内存状态
    void appendViewMessage(
        std::string_view   sessionId,
        const ViewMessage& msg,
        uint64_t           msgIdCounter,
        uint64_t           seq = 0
    );

    /// 更新一条已持久化的展示历史消息 (按 msg.id 定位行)
    /// - 用于追加后内容再变化的消息 (如 tool 结果回填: toolFinished/toolResult/collapsed),
    ///   保证重启恢复的历史与内存状态一致
    /// - msg.id 必须非空 (appendViewMessage 分配); 找不到匹配行仅记录日志, 不影响内存状态
    void updateViewMessage(std::string_view sessionId, const ViewMessage& msg);

    /// 保存 LLM 上下文消息 (整表替换; 每轮对话结束时调用)
    /// - 失败仅记录日志, 不影响内存状态
    void saveLlmMessages(std::string_view sessionId, const utilxx_base::Json& llmMessages);

    // ---- 输入收件箱 (session.db session_input 表; 计划 LOOP-1) ----
    //
    // 输入受理时先落库 (status=admitted), 真正进入上下文时标记 promoted;
    // 进程重启后只恢复 admitted 条目为"待确认", 不自动重放副作用。

    /// 收件箱条目状态
    struct SessionInputStatus {
        /// 已受理, 尚未进入上下文 (重启后按"待确认"恢复)
        inline static constexpr std::string_view Admitted = "admitted";
        /// 已投递 (进入上下文或本次请求); 重启后不再恢复
        inline static constexpr std::string_view Promoted = "promoted";
        /// 已被用户删除 / 队列被清空
        inline static constexpr std::string_view Dropped = "dropped";
    };

    /// 收件箱条目 (payload 为 WireUserInput 的 JSON 形态, 由调用方提供)
    struct SessionInputRecord {
        std::string id;                 ///< 条目标识 (队列条目 id, 如 "q-1")
        std::string payload;            ///< 原始输入载荷 (JSON; 文本/模型/附件/投递模式)
        std::string delivery;           ///< 投递模式 (InputDelivery 取值)
        std::string status;             ///< 见 [SessionInputStatus]
        int64_t     admittedSeq = 0;    ///< 受理时的会话 delta seq
        int64_t     promotedSeq = 0;    ///< 投递时的会话 delta seq
        int64_t     createdMs   = 0;    ///< 受理时间戳 (毫秒)
    };

    /// 写入一条收件箱条目 (同 id 覆盖; 失败仅记日志)
    void addSessionInput(std::string_view sessionId, const SessionInputRecord& record);

    /// 标记条目已投递 (status=promoted + promotedSeq); 失败仅记日志
    void markSessionInputPromoted(std::string_view sessionId, std::string_view id, uint64_t promotedSeq);

    /// 标记条目终态 (dropped 等); 失败仅记日志
    void setSessionInputStatus(
        std::string_view sessionId,
        std::string_view id,
        std::string_view status
    );

    /// 列出收件箱条目 (按受理顺序; statusFilter 为空时不过滤)
    /// - 供重启恢复: 只取 [SessionInputStatus::Admitted] 条目
    std::vector<SessionInputRecord>
        listSessionInputs(std::string_view sessionId, std::string_view statusFilter = {});

    // ---- 用量记录 (session.db usage 表) ----

    /// 单次模型调用用量 (成功与失败都记一行; 失败时 ok=false 并带 errorKind)
    struct UsageRecord {
        /// 成员按尺寸从大到小排列, 减少结构体内填充字节
        std::string model;
        std::string errorKind; ///< 失败分类文本 (成功时为空)
        int64_t     timeMs                  = 0;
        int64_t     promptTokens            = 0;
        int64_t     completionTokens        = 0;
        int64_t     totalTokens             = 0;
        int64_t     cachedPromptTokens      = 0;
        /// 写入 prompt 缓存的 token 数 (Anthropic 的 cache_creation_input_tokens;
        /// 其余 provider 为 0; 计划 LLM-8)
        int64_t     cacheWritePromptTokens  = 0;
        int64_t     reasoningTokens         = 0;
        bool        ok                      = true;
    };

    /// 会话用量聚合 (供界面/诊断展示; 由用量记录汇总, 不依赖内存中的最后一次统计)
    struct UsageSummary {
        int64_t calls                  = 0; ///< 记录次数 (含失败)
        int64_t failedCalls            = 0; ///< 失败次数
        int64_t promptTokens           = 0;
        int64_t completionTokens       = 0;
        int64_t totalTokens            = 0;
        int64_t cachedPromptTokens     = 0;
        int64_t cacheWritePromptTokens = 0;
        int64_t reasoningTokens        = 0;
    };

    /// 追加一条用量记录 (失败仅记日志; 用量记录是统计信息, 不影响对话流程)
    void addUsage(std::string_view sessionId, const UsageRecord& record);

    /// 会话用量聚合; 无记录/读取失败返回全 0
    UsageSummary usageSummary(std::string_view sessionId);

    /// 最近的用量记录 (按时间倒序, 至多 limit 条; limit == 0 返回空)
    std::vector<UsageRecord> recentUsage(std::string_view sessionId, size_t limit);

    // ---- 会话标题与检索 (计划 STO-12) ----

    /// 检索默认返回条数上限 (limit == 0 时使用)
    static constexpr size_t kSearchDefaultLimit = 50;

    /// 会话标题 (meta.title; 无记录/读取失败返回空)
    std::string sessionTitle(std::string_view sessionId);

    /// 该会话是否已存在数据目录 (未创建过 = 无数据)
    /// - 供"给已有会话改名""校验目标会话"这类操作先判断存在性, 避免为了写一个
    ///   字段就把目录/库建出来 (改名不是新建会话的入口)
    bool sessionDataDirExists(std::string_view sessionId) const;

    /// 设置会话标题并标记来源为用户 (再写入 meta.titleSource = "user")
    /// - 空标题忽略并返回 false (标题为空会让会话列表无法辨认)
    /// - 自动标题 (首条用户消息预览) 只在标题不存在时写入, 不会覆盖用户改名
    bool setSessionTitle(std::string_view sessionId, std::string_view title);

    /// 会话标题来源: "user" (用户改名) / "auto" (首条用户消息预览) / 空 (未知)
    std::string sessionTitleSource(std::string_view sessionId);

    /// 会话检索命中项
    struct SessionSearchHit {
        SessionInfo info;
        std::string snippet; ///< 命中处的文本片段 (标题命中时为空)
        bool        titleMatch = false;
    };

    /// 关键词检索会话 (标题或展示历史正文包含关键词)
    ///
    /// - 匹配语义: 大小写不敏感的子串匹配 (SQL LIKE; 关键词中的 `%` `_` `\` 自动转义)
    /// - 不做跨库索引: 逐个会话目录用临时只读连接查询 (与列表接口同顺序),
    ///   需要更强检索能力时再考虑 FTS5 (见计划 STO-12)
    /// - 结果按最近活动时间降序; [limit] 为 0 时用 [kSearchDefaultLimit];
    ///   空关键词返回空结果
    std::vector<SessionSearchHit> searchSessions(std::string_view keyword, size_t limit = 0);

    // ---- share store (session.db store 表) ----

    /// 读取条目; 不存在/打开失败返回 nullopt
    std::optional<std::string> getShareStoreItem(std::string_view sessionId, size_t id);

    /// 覆盖/新增指定 id 条目
    void setShareStoreItem(std::string_view sessionId, size_t id, std::string_view value);

    /// 插入新条目并返回分配的 id (现有最大 id + 1, 单调递增且重启后延续,
    /// 与显式 set 的高位 id 不冲突); 失败返回 0
    size_t addShareStoreItem(std::string_view sessionId, std::string_view value);

    /// 已分配 id 的最大值 (空存储/目录不存在/读取失败返回 0)
    /// - 只查 max(id) 不读内容: 供 share store 恢复内存中的自增 id 计数,
    ///   避免把全部条目内容读进内存 (内容在取值时按 id 单独读取)
    size_t shareStoreLastId(std::string_view sessionId);

    /// 当前根目录 (测试可校验路径)
    const std::string& rootDir() const noexcept {
        return rootDir_;
    }

    /// 是否对写连接启用跨进程写租约 (诊断/装配快照展示用)
    bool writerLeaseEnabled() const noexcept {
        return enableWriterLease_;
    }

    /// 本程序支持的会话库 schema 版本 (写入时按相邻迁移链升到这个版本)
    static constexpr int schemaVersion() noexcept {
        return kSchemaVersion;
    }

    /// 最近一次写路径失败原因 (空 = 无失败)
    ///
    /// 写连接不可用时 (会话目录被其它进程写了、库版本高于本程序支持、目录/文件
    /// 无法创建) 写操作会失败并在此留下原因; 写库本身已记录错误日志, 本接口供
    /// 宿主在诊断或界面上给出明确提示 (而不是让用户以为消息已保存)。
    std::string lastWriteError() const;

    /// 将 sessionId 清洗为安全目录名 (非法字符替换/超长截断/保留名规避,
    /// 发生改写时附加哈希尾缀保证唯一性); 静态方法便于测试
    static std::string sanitizeSessionId(std::string_view sessionId);

private:

    struct SessionDbs {
        agentxx::util::SqliteDb sessionDb;
        /// 会话目录写租约 (跨进程互斥; 随连接一同释放, 见 [dbs])
        std::shared_ptr<SessionWriterLease> writerLease;
    };

    /// 连接缓存条目 (含最近使用序号, 供 LRU 淘汰)
    struct DbsEntry {
        std::shared_ptr<SessionDbs> dbs{};
        uint64_t                    lastUseSeq = 0;
    };

    /// 同时保持打开的会话数据库连接数上限
    /// - 每个连接占用 fd + WAL + page cache, 进程内长期运行(会话很多)时会持续
    ///   占用文件描述符 (Linux 默认 ulimit -n 常为 1024) 与内存
    /// - 超出上限时按 LRU 关闭最久未使用的连接 (关闭后下次写入自动重开, 不丢数据)
    static constexpr size_t kMaxOpenSessionDbs = 32;

    /// 获取 (或懒创建) 指定 session 的数据库连接; 失败抛异常
    /// - 仅写入路径调用: 读取路径在目录不存在时直接返回空数据, 避免
    ///   为只读访问 (如 subagent/未开始会话) 创建目录与空 DB 文件
    /// - 调用方必须持有 [mutex_]
    SessionDbs& dbs(std::string_view sessionId);

    /// LRU 淘汰: 连接数超出 [kMaxOpenSessionDbs] 时关闭最久未使用的连接
    /// - 调用方必须持有 [mutex_]
    void evictLruDbs();

    /// 建表 + schema 迁移 (幂等)
    /// - 版本低于 [kSchemaVersion] 时按相邻步骤迁移 (每步独立事务, 迁移前备份)
    /// - 库版本高于本程序支持的版本时抛异常拒绝打开
    /// - [dbFilePath] 会话库文件路径 (迁移备份用)
    static void ensureSchema(agentxx::util::SqliteDb& sessionDb, const std::string& dbFilePath);

    /// 读取 meta.schema_version (无该表/无该键时返回 0)
    static int readSchemaVersion(agentxx::util::SqliteDb& sessionDb);

    /// 写入 meta.schema_version (UPSERT)
    static void writeSchemaVersion(agentxx::util::SqliteDb& sessionDb, int version);

    /// 执行单个迁移步骤 (幂等; 由 [ensureSchema] 按顺序调用)
    static void applyMigrationStep(agentxx::util::SqliteDb& sessionDb, int step);

    /// 迁移前备份库文件 (仅库内已有表时执行; 失败只记日志, 不阻断迁移)
    static void backupDbFile(
        agentxx::util::SqliteDb& sessionDb,
        const std::string&       dbFilePath,
        int                      fromVersion
    );

    /// 迁移 view_message 的 msg_id 列与索引 (幂等; 老库 ALTER + 回填)
    static void ensureViewMessageMsgIdColumn(agentxx::util::SqliteDb& sessionDb);

    /// 迁移 usage 的 cache_write_prompt_tokens 列 (幂等; 老库 ALTER 补列, 计划 LLM-8)
    static void ensureUsageCacheWriteColumn(agentxx::util::SqliteDb& sessionDb);

    /// 读取会话 meta 中单个键 (只读临时连接, 不取写租约; 无数据返回空串)
    std::string readMetaValue(std::string_view sessionId, std::string_view key) const;

    std::string rootDir_;
    /// 是否启用跨进程写租约 (见构造函数说明)
    bool        enableWriterLease_ = true;
    /// 最近一次写路径失败原因 (空 = 无失败; 见 [lastWriteError])
    std::string lastWriteError_;
    mutable std::mutex mutex_;
    /// key: 原始 sessionId (未清洗, 清洗仅用于目录名)
    std::map<std::string, DbsEntry, std::less<>> dbs_;
    /// 连接使用序号 (每次取用连接时自增, 值越大越新; 仅 [mutex_] 内访问)
    uint64_t dbsUseSeq_ = 0;
};

} // namespace agent
} // namespace agentxx
