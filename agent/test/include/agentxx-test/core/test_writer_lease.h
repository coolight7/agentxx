#pragma once

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// 会话目录写租约 (agentxx::agent::SessionWriterLease) 测试:
/// 进程内可重入、系统级互斥、释放后回收、多目录独立
TestResult testWriterLease();

/// 模拟"另一个进程"持有目录写锁 (测试用)
/// - 直接用平台 API 独占/加锁目录下的锁文件, 不经过 SessionWriterLease 的
///   进程内引用计数; 用于验证 "目录被外部占用时写操作明确失败"
/// - Windows: CreateFile 不共享打开; POSIX: flock(LOCK_EX | LOCK_NB)
class ForeignWriterLock {
public:

    explicit ForeignWriterLock(const std::string& dir);
    ~ForeignWriterLock();

    ForeignWriterLock(const ForeignWriterLock&)            = delete;
    ForeignWriterLock& operator=(const ForeignWriterLock&) = delete;

    /// 是否成功持锁
    bool held() const noexcept {
        return held_;
    }

private:

    bool  held_   = false;
    void* handle_ = nullptr; ///< Windows: HANDLE; POSIX: 存放在 (intptr_t) 里的 fd
};

} // namespace test
} // namespace agentxx
