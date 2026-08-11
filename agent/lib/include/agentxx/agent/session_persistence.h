#pragma once

#include "agentxx/agent/conversation_types.h"
#include "agentxx/util/sqlite.h"
#include "neograph/json.h"
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace agentxx {
namespace agent {

/// 会话数据 SQLite 持久化 (按 threadId 分目录)
///
/// 目录结构: {root}/{sanitizedThreadId}/
///   - session.db      会话消息状态:
///                       view_message 表  展示历史 (append-only, 每消息一行 JSON)
///                       llm_context 表  LLM 上下文消息 (单行整体替换, 每轮结束保存)
///                       meta 表          msgIdCounter
///   - share_store.db  agentxx_share_store KV 存储:
///                       item 表  id(自增) -> value
///
/// 默认 root: {dataDir}/sqlite/sessions/ (dataDir 为空时 ~/.agentxx/,
/// 取不到用户主目录时回退系统临时目录), 数据目录统一由 client 经
/// AgentConfig::dataDir 重定向。
/// 分库理由:
///   - session.db: viewMessages/llmMessages 同属"会话消息状态", 生命周期一致
///     (随 thread 创建/删除), 在同一个 io 线程写入, 一轮对话结束时消息与
///     计数可事务性一起提交; 数据量可控 (llmMessages 受上下文窗口约束,
///     viewMessages 单行小)
///   - share_store.db: KV 随机读改写, 与消息顺序追加模式完全不同; 本质是
///     上下文卸载缓存, 内容可丢弃/可清理, 生命周期独立于消息历史; 可能存放
///     大型文本 (工具结果/压缩内容), 独立文件避免其膨胀拖慢消息库的
///     WAL checkpoint, 也便于未来独立裁剪/归档
///
/// 线程安全: 内部互斥锁保护所有 DB 访问; 常规使用下调用发生在 agent io 线程
/// (Session 绑定线程 / 工具执行), 锁仅在多线程并发访问时生效, 开销可忽略
class SessionPersistence {
public:

    /// @param rootDir 数据根目录; 为空使用默认 {dataDir}/sqlite/sessions/
    ///        (dataDir 为空时 ~/.agentxx/, 取不到用户主目录时回退系统临时目录)
    explicit SessionPersistence(std::string rootDir = "");

    // ---- 会话消息状态 (session.db) ----

    struct LoadedSession {
        std::vector<ViewMessage> viewMessages;
        neograph::json           llmMessages = neograph::json::array();
        /// 恢复后的 msg id 计数器 (保证新消息 id 不与已存消息冲突)
        uint64_t msgIdCounter = 0;
    };

    /// 加载指定 thread 的会话消息状态; 无数据/打开失败时返回空结构 (仅记日志)
    LoadedSession loadSession(std::string_view threadId);

    /// 列举全部持久化会话的摘要 (供会话选择弹窗), 按最近活动时间降序
    /// - 扫描根目录下各 thread 目录, 以独立临时连接读取 session.db meta 表
    ///   (threadId/title/lastActiveMs); 老数据无 meta 时回退目录名作 threadId
    /// - 打开/读取失败仅记日志并跳过该目录
    std::vector<SessionInfo> listSessions();

    /// 追加一条展示历史消息 (事务: 消息 + msgIdCounter 一起提交)
    /// - msgIdCounter 为追加后会话的计数 (新消息 id 序号), 供重启恢复
    /// - 失败仅记录日志, 不影响内存状态
    void
        appendViewMessage(std::string_view threadId, const ViewMessage& msg, uint64_t msgIdCounter);

    /// 保存 LLM 上下文消息 (整表替换; 每轮对话结束时调用)
    /// - 失败仅记录日志, 不影响内存状态
    void saveLlmMessages(std::string_view threadId, const neograph::json& llmMessages);

    // ---- share store (share_store.db) ----

    struct LoadedShareStore {
        std::map<size_t, std::string> items; ///< id -> value
        /// 下一个可分配 id (恢复自现有条目最大 id + 1; 空存储为 1)
        size_t nextId = 1;
    };

    /// 加载指定 thread 的 share store 全部条目; 打开失败时返回空结构 (仅记日志)
    LoadedShareStore loadShareStore(std::string_view threadId);

    /// 读取条目; 不存在/打开失败返回 nullopt
    std::optional<std::string> getShareStoreItem(std::string_view threadId, size_t id);

    /// 覆盖/新增指定 id 条目
    void setShareStoreItem(std::string_view threadId, size_t id, std::string_view value);

    /// 插入新条目并返回分配的 id (现有最大 id + 1, 单调递增且重启后延续,
    /// 与显式 set 的高位 id 不冲突); 失败返回 0
    size_t addShareStoreItem(std::string_view threadId, std::string_view value);

    void removeShareStoreItem(std::string_view threadId, size_t id);

    /// 当前根目录 (测试可校验路径)
    const std::string& rootDir() const noexcept {
        return rootDir_;
    }

    /// 将 threadId 清洗为安全目录名 (非法字符替换/超长截断/保留名规避,
    /// 发生改写时附加哈希尾缀保证唯一性); 静态方法便于测试
    static std::string sanitizeThreadId(std::string_view threadId);

private:

    struct ThreadDbs {
        agentxx::util::SqliteDb sessionDb;
        agentxx::util::SqliteDb shareStoreDb;
    };

    /// 获取 (或懒创建) 指定 thread 的数据库连接; 失败抛异常
    /// - 仅写入路径调用: 读取路径在目录不存在时直接返回空数据, 避免
    ///   为只读访问 (如 subagent/未开始会话) 创建目录与空 DB 文件
    ThreadDbs& dbs(std::string_view threadId);

    /// 该 thread 的数据目录是否存在 (未创建过 = 无数据, 读取直接返回空)
    bool threadDirExists(std::string_view threadId) const;

    /// 建表 (幂等)
    static void
        ensureSchema(agentxx::util::SqliteDb& sessionDb, agentxx::util::SqliteDb& shareStoreDb);

    std::string rootDir_;
    std::mutex  mutex_;
    /// key: 原始 threadId (未清洗, 清洗仅用于目录名)
    std::map<std::string, std::shared_ptr<ThreadDbs>, std::less<>> dbs_;
};

} // namespace agent
} // namespace agentxx
