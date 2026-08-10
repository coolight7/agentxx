#include "agentxx/util/sqlite.h"

#include <sqlite3.h>
#include <stdexcept>

namespace agentxx {
namespace util {

namespace {

/// 拼接带 sqlite 错误文本的异常消息
[[noreturn]] void throwDbError(sqlite3* db, std::string_view what) {
    const char* msg = db ? sqlite3_errmsg(db) : "unknown";
    throw std::runtime_error{std::string{what} + ": " + msg};
}

} // namespace

// ---------------------------------------------------------------------------
// SqliteDb
// ---------------------------------------------------------------------------

SqliteDb::~SqliteDb() {
    close();
}

SqliteDb::SqliteDb(SqliteDb&& other) noexcept :
    db_(other.db_) {
    other.db_ = nullptr;
}

SqliteDb& SqliteDb::operator=(SqliteDb&& other) noexcept {
    if (this != &other) {
        close();
        db_       = other.db_;
        other.db_ = nullptr;
    }
    return *this;
}

void SqliteDb::open(std::string_view path) {
    close();
    // SQLITE_OPEN_FULLMUTEX: serialized 线程模式, 便于多线程共享连接时由外部锁保护
    int rc = sqlite3_open_v2(
        std::string{path}.c_str(),
        &db_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr
    );
    if (rc != SQLITE_OK) {
        std::string err = db_ ? sqlite3_errmsg(db_) : "open failed";
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        throw std::runtime_error{"SqliteDb open(" + std::string{path} + "): " + err};
    }
    // WAL: 读不阻塞写, 写不阻塞读; 适合会话消息频繁追加 + 连接级读取
    exec("PRAGMA journal_mode=WAL");
    // busy_timeout: 多进程/多连接并发写时等待而非立即 SQLITE_BUSY
    exec("PRAGMA busy_timeout=5000");
    // synchronous=NORMAL: WAL 模式下每次 commit fsync WAL 文件, 兼顾持久性与吞吐
    exec("PRAGMA synchronous=NORMAL");
}

void SqliteDb::close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

void SqliteDb::exec(std::string_view sql) {
    if (!db_) {
        throw std::runtime_error{"SqliteDb exec: db not open"};
    }
    char* errMsg = nullptr;
    int   rc     = sqlite3_exec(db_, std::string{sql}.c_str(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::string err = errMsg ? errMsg : sqlite3_errmsg(db_);
        sqlite3_free(errMsg);
        throw std::runtime_error{"SqliteDb exec: " + err};
    }
}

SqliteDb::Stmt SqliteDb::prepare(std::string_view sql) {
    return Stmt{db_, sql};
}

void SqliteDb::beginImmediate() {
    exec("BEGIN IMMEDIATE");
}

void SqliteDb::commit() {
    exec("COMMIT");
}

void SqliteDb::rollback() {
    exec("ROLLBACK");
}

int64_t SqliteDb::lastInsertRowid() const noexcept {
    return db_ ? sqlite3_last_insert_rowid(db_) : 0;
}

std::string SqliteDb::lastError() const noexcept {
    return db_ ? (sqlite3_errmsg(db_) ? sqlite3_errmsg(db_) : "") : "";
}

// ---------------------------------------------------------------------------
// SqliteDb::Stmt
// ---------------------------------------------------------------------------

SqliteDb::Stmt::Stmt(sqlite3* db, std::string_view sql) :
    db_(db) {
    if (!db_) {
        throw std::runtime_error{"SqliteDb::Stmt: db not open"};
    }
    int rc = sqlite3_prepare_v2(db_, std::string{sql}.c_str(), -1, &stmt_, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error{"SqliteDb prepare: " + std::string{sqlite3_errmsg(db_)}};
    }
}

SqliteDb::Stmt::~Stmt() {
    if (stmt_) {
        sqlite3_finalize(stmt_);
        stmt_ = nullptr;
    }
}

SqliteDb::Stmt::Stmt(Stmt&& other) noexcept :
    stmt_(other.stmt_),
    db_(other.db_) {
    other.stmt_ = nullptr;
    other.db_   = nullptr;
}

SqliteDb::Stmt& SqliteDb::Stmt::operator=(Stmt&& other) noexcept {
    if (this != &other) {
        if (stmt_) {
            sqlite3_finalize(stmt_);
        }
        stmt_       = other.stmt_;
        db_         = other.db_;
        other.stmt_ = nullptr;
        other.db_   = nullptr;
    }
    return *this;
}

void SqliteDb::Stmt::bindInt64(int index, int64_t value) {
    int rc = sqlite3_bind_int64(stmt_, index, value);
    if (rc != SQLITE_OK) {
        throwDbError(db_, "sqlite bind_int64");
    }
}

void SqliteDb::Stmt::bindText(int index, std::string_view value) {
    // SQLITE_TRANSIENT: sqlite 内部拷贝, 允许 value 在 step 前析构
    // 空串时 data() 可能为 nullptr, 显式传 "" 保证 sqlite 行为一致
    const char* data = value.empty() ? "" : value.data();
    int rc = sqlite3_bind_text(
        stmt_,
        index,
        data,
        static_cast<int>(value.size()),
        SQLITE_TRANSIENT
    );
    if (rc != SQLITE_OK) {
        throwDbError(db_, "sqlite bind_text");
    }
}

void SqliteDb::Stmt::bindNull(int index) {
    int rc = sqlite3_bind_null(stmt_, index);
    if (rc != SQLITE_OK) {
        throwDbError(db_, "sqlite bind_null");
    }
}

bool SqliteDb::Stmt::step() {
    if (!stmt_) {
        throw std::runtime_error{"SqliteDb::Stmt::step: stmt not prepared"};
    }
    int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) {
        return true;
    }
    if (rc == SQLITE_DONE) {
        return false;
    }
    throwDbError(db_, "sqlite step");
}

void SqliteDb::Stmt::reset() {
    int rc = sqlite3_reset(stmt_);
    if (rc != SQLITE_OK) {
        throwDbError(db_, "sqlite reset");
    }
}

int64_t SqliteDb::Stmt::columnInt64(int column) const {
    return sqlite3_column_int64(stmt_, column);
}

std::string SqliteDb::Stmt::columnText(int column) const {
    const unsigned char* text = sqlite3_column_text(stmt_, column);
    int                  size = sqlite3_column_bytes(stmt_, column);
    if (!text || size <= 0) {
        return {};
    }
    return std::string{reinterpret_cast<const char*>(text), static_cast<size_t>(size)};
}

bool SqliteDb::Stmt::columnIsNull(int column) const {
    return sqlite3_column_type(stmt_, column) == SQLITE_NULL;
}

} // namespace util
} // namespace agentxx
