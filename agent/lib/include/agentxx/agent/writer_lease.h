#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace agentxx {
namespace agent {

/// 会话目录写租约 (跨进程互斥)
///
/// 目的: 同一会话目录同时只允许一个进程写入, 避免两个进程 (例如本机再起一个
/// agent 服务、或调试进程与常驻进程) 打开同一 `session.db` 互相覆盖数据。
/// 读操作不需要租约: 只读路径 (会话列表/摘要、只读事务) 直接读库。
///
/// 实现:
/// - POSIX: 打开目录下 `.writer.lock` 并 `flock(LOCK_EX | LOCK_NB)`
/// - Windows: 以"不共享"方式打开 `.writer.lock` (dwShareMode = 0),
///   其他进程再次打开会拿到 ERROR_SHARING_VIOLATION
/// - 两者都由系统在内核对象/句柄关闭时释放: 持有进程崩溃后租约自动失效,
///   不使用 PID/时间戳猜测"陈旧锁", 也不设超时
///
/// 进程内可重入: 同一进程内多次获取同一目录只增加引用计数 (本进程的多个
/// SessionStore/测试实例不会互相冲突), 真正互斥的是不同进程。
///
/// 失败语义: 租约被其他进程持有时 [acquire] 返回 nullptr 并填写原因,
/// 调用方应把写操作作为明确错误上报, 不得静默降级为"照样写"。
class SessionWriterLease {
public:

    /// 获取目录写租约
    ///
    /// - `args`:
    ///     - [dir] 目标目录 (不存在时先创建)
    ///     - [errMsg] 失败原因输出 (可选); 成功时不改动
    ///
    /// - `return` 成功返回租约 (随引用释放自动解锁); 被其他进程持有或无法创建
    ///   锁文件时返回 nullptr
    static std::shared_ptr<SessionWriterLease>
        acquire(const std::filesystem::path& dir, std::string* errMsg = nullptr);

    ~SessionWriterLease();

    SessionWriterLease(const SessionWriterLease&)            = delete;
    SessionWriterLease& operator=(const SessionWriterLease&) = delete;

    /// 租约对应的目录 (标准化后的字符串)
    const std::string& dir() const noexcept {
        return dir_;
    }

    /// 锁文件路径 `<dir>/.writer.lock` (供测试与诊断查看)
    static std::string lockFilePath(const std::filesystem::path& dir);

    /// 本进程当前持有的租约目录数量 (供测试断言引用计数是否正确回收)
    static size_t heldCount();

private:

    /// 内部构造: 由 [acquire] 在引用计数登记完成后调用
    explicit SessionWriterLease(std::string dir);

    /// 释放本实例持有的一次引用 (引用计数归零时关闭句柄并解锁)
    void release();

    std::string dir_;
};

} // namespace agent
} // namespace agentxx
