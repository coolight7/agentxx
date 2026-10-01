#include "agentxx/agent/writer_lease.h"

#include "fmt/format.h"
#include "utilxx_base/log.h"
#include <mutex>
#include <unordered_map>

#if XX_IS_WIN_D
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <cerrno>
#  include <cstring>
#  include <fcntl.h>
#  include <sys/file.h>
#  include <unistd.h>
#endif

namespace agentxx {
namespace agent {

namespace {

namespace fs = std::filesystem;

/// 锁文件名 (会话目录内, 与 session.db 同级)
static constexpr const char* kLockFileName = ".writer.lock";

#if XX_IS_WIN_D
/// Windows 下持有的一次锁: 不共享方式打开的文件句柄
using LockHandle = HANDLE;
static constexpr LockHandle kInvalidHandle = nullptr;
#else
/// POSIX 下持有的一次锁: flock 成功的文件描述符
using LockHandle = int;
static constexpr LockHandle kInvalidHandle = -1;
#endif

/// 进程内已持有目录 → {系统锁句柄, 引用计数}
struct LeaseEntry {
    LockHandle handle   = kInvalidHandle;
    int        refcount = 0;
};

std::mutex                             g_leaseMutex;
std::unordered_map<std::string, LeaseEntry> g_leases;

/// 目录的稳定键 (绝对规范路径; 同一目录的不同写法视为同一租约)
std::string normalizeDir(const fs::path& dir) {
    std::error_code ec;
    auto            abs = fs::weakly_canonical(dir, ec);
    if (ec) {
        abs = fs::absolute(dir, ec);
        if (ec) {
            abs = dir;
        }
    }
    return abs.generic_string();
}

/// 真正获取系统级锁 (调用方持有 g_leaseMutex)
LockHandle systemLock(const fs::path& lockFile, std::string* errMsg) {
#if XX_IS_WIN_D
    // dwShareMode = 0: 其他进程再次打开同一文件会失败 (ERROR_SHARING_VIOLATION)
    // FILE_ATTRIBUTE_HIDDEN: 不让锁文件出现在资源管理器里
    auto handle = ::CreateFileW(
        lockFile.wstring().c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_HIDDEN,
        nullptr
    );
    if (handle == INVALID_HANDLE_VALUE) {
        if (errMsg) {
            *errMsg = fmt::format(
                "create lock file `{}` failed (win32 error {})",
                lockFile.string(),
                ::GetLastError()
            );
        }
        return kInvalidHandle;
    }
    return handle;
#else
    const auto path = lockFile.string();
    int        fd   = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (fd < 0) {
        if (errMsg) {
            *errMsg = fmt::format("open lock file `{}` failed: {}", path, std::strerror(errno));
        }
        return kInvalidHandle;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int err = errno;
        ::close(fd);
        if (errMsg) {
            *errMsg = fmt::format(
                "lock file `{}` is held by another process: {}",
                path,
                std::strerror(err)
            );
        }
        return kInvalidHandle;
    }
    return fd;
#endif
}

/// 释放系统级锁 (调用方持有 g_leaseMutex)
void systemUnlock(LockHandle handle) {
    if (handle == kInvalidHandle) {
        return;
    }
#if XX_IS_WIN_D
    ::CloseHandle(handle);
#else
    // flock 随 fd 关闭自动释放; 先显式解锁便于语义清晰
    ::flock(handle, LOCK_UN);
    ::close(handle);
#endif
}

} // namespace

std::string SessionWriterLease::lockFilePath(const fs::path& dir) {
    return (dir / kLockFileName).string();
}

std::shared_ptr<SessionWriterLease> SessionWriterLease::acquire(
    const fs::path& dir,
    std::string*    errMsg
) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        if (errMsg) {
            *errMsg = fmt::format(
                "create session dir `{}` failed: {}",
                dir.string(),
                ec.message()
            );
        }
        return nullptr;
    }

    const auto  key      = normalizeDir(dir);
    const auto  lockFile = fs::path{key} / kLockFileName;

    std::lock_guard<std::mutex> lock(g_leaseMutex);
    if (auto it = g_leases.find(key); it != g_leases.end()) {
        // 本进程已持有: 仅增加引用计数 (进程内可重入)
        ++it->second.refcount;
        return std::shared_ptr<SessionWriterLease>{new SessionWriterLease{key}};
    }
    auto handle = systemLock(lockFile, errMsg);
    if (handle == kInvalidHandle) {
        if (errMsg && errMsg->empty()) {
            *errMsg = fmt::format("session dir `{}` is locked by another process", key);
        }
        return nullptr;
    }
    g_leases.emplace(key, LeaseEntry{handle, 1});
    return std::shared_ptr<SessionWriterLease>{new SessionWriterLease{key}};
}

SessionWriterLease::SessionWriterLease(std::string dir) :
    dir_(std::move(dir)) {}

SessionWriterLease::~SessionWriterLease() {
    release();
}

void SessionWriterLease::release() {
    if (dir_.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_leaseMutex);
    auto                        it = g_leases.find(dir_);
    if (it == g_leases.end()) {
        // 已被释放 (重复释放/顺序异常): 保持幂等, 不重复关闭句柄
        dir_.clear();
        return;
    }
    if (--it->second.refcount > 0) {
        dir_.clear();
        return;
    }
    systemUnlock(it->second.handle);
    g_leases.erase(it);
    XX_LOGD("SessionWriterLease: released session dir `{}`", dir_);
    dir_.clear();
}

size_t SessionWriterLease::heldCount() {
    std::lock_guard<std::mutex> lock(g_leaseMutex);
    return g_leases.size();
}

} // namespace agent
} // namespace agentxx
