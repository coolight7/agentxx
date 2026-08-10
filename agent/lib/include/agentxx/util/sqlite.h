#pragma once

#include <cstdint>
#include <string>
#include <string_view>

struct sqlite3;
struct sqlite3_stmt;

namespace agentxx {
namespace util {

/// 轻量 RAII sqlite3 封装
///
/// - 仅面向本项目的简单行存储/键值场景 (会话持久化等), 不做通用 ORM
/// - 打开时启用 WAL 日志模式 + busy_timeout(5s) + synchronous=NORMAL,
///   兼顾读写并发与写性能 (单进程多连接/多线程安全)
/// - 所有失败 (打开/执行/绑定/步进) 均抛出 std::runtime_error,
///   调用方可用 agentxx::util::catchError 系列捕获, 消息含 sqlite3 错误文本
/// - 不持有内部锁: 单连接只允许单线程同时使用; 多线程并发使用请自行加锁
class SqliteDb {
public:

    SqliteDb() = default;
    ~SqliteDb();
    SqliteDb(SqliteDb&& other) noexcept;
    SqliteDb& operator=(SqliteDb&& other) noexcept;
    SqliteDb(const SqliteDb&)            = delete;
    SqliteDb& operator=(const SqliteDb&) = delete;

    /// 打开数据库 (文件不存在则创建; ":memory:" 支持内存库, 用于测试)
    /// - 设置 WAL / busy_timeout / synchronous=NORMAL; 失败抛异常
    void open(std::string_view path);
    void close();

    bool isOpen() const noexcept {
        return db_ != nullptr;
    }

    /// 执行无参数 SQL (DDL/DML); 失败抛异常
    void exec(std::string_view sql);

    /// 预编译语句 (1-based 绑定参数, 0-based 列索引)
    class Stmt {
    public:

        Stmt() = default;
        ~Stmt();
        Stmt(Stmt&& other) noexcept;
        Stmt& operator=(Stmt&& other) noexcept;
        Stmt(const Stmt&)            = delete;
        Stmt& operator=(const Stmt&) = delete;

        /// 内部构造: 由 SqliteDb::prepare 使用 (sql 编译失败抛异常)
        Stmt(sqlite3* db, std::string_view sql);

        void bindInt64(int index, int64_t value);
        void bindText(int index, std::string_view value);
        void bindNull(int index);

        /// 执行一步; true = 取到一行, false = 执行完成 (无更多行/无结果集)
        bool step();

        /// 重置语句 (可重新绑定参数并再次 step); 失败抛异常
        void reset();

        int64_t     columnInt64(int column) const;
        std::string columnText(int column) const;
        bool        columnIsNull(int column) const;

    private:

        sqlite3_stmt* stmt_ = nullptr;
        sqlite3*      db_   = nullptr; ///< 仅用于构造错误消息
    };

    /// 预编译语句; 失败抛异常
    Stmt prepare(std::string_view sql);

    // ---- 事务 ----

    /// BEGIN IMMEDIATE (获取写锁; 与并发写者冲突时按 busy_timeout 等待)
    void beginImmediate();
    void commit();
    void rollback();

    /// 最近一次 INSERT 的行 id; 无插入返回 0
    int64_t lastInsertRowid() const noexcept;

    /// 最近一次错误文本 (无错误时为空串)
    std::string lastError() const noexcept;

private:

    sqlite3* db_ = nullptr;
};

} // namespace util
} // namespace agentxx
