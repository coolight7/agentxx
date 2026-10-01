#include "agentxx-test/core/test_writer_lease.h"

#include "agentxx/agent/writer_lease.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <string>
#include <thread>

#if XX_IS_WIN_D
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/file.h>
#  include <unistd.h>
#endif

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_wl_passed = 0;
int g_wl_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_wl_passed
#define XX_TEST_FAILED g_wl_failed

namespace agentxx {
namespace test {

using agentxx::agent::SessionWriterLease;

namespace fs = std::filesystem;

namespace {

/// 创建唯一临时目录 (测试根目录)
std::string makeTempRoot() {
    auto dir = fs::temp_directory_path()
               / fmt::format(
                   "agentxx_wl_test_{}",
                   std::chrono::steady_clock::now().time_since_epoch().count()
               );
    fs::create_directories(dir);
    return dir.string();
}

/// 删除测试临时目录 (Windows 上文件句柄可能稍晚释放, 短暂重试)
void removeTempRoot(const std::string& root) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        std::error_code ec;
        fs::remove_all(utilxx_base::utf8ToPath(root), ec);
        if (!ec) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    std::error_code ec;
    fs::remove_all(utilxx_base::utf8ToPath(root), ec);
    if (ec) {
        XX_LOGW("清理测试临时目录失败: {} ({})", root, ec.message());
    }
}

/// 用平台原生方式尝试独占锁文件: true = 已被别的持有者占用
/// - 与实现无关地验证"确实存在内核级互斥", 而不是只看进程内计数
bool osLockHeld(const std::string& dir) {
    const auto lockFile = SessionWriterLease::lockFilePath(dir);
#if XX_IS_WIN_D
    auto handle = ::CreateFileW(
        utilxx_base::utf8ToPath(lockFile).c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0, // 不共享: 已被占用时打开失败
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (handle == INVALID_HANDLE_VALUE) {
        return true;
    }
    ::CloseHandle(handle);
    return false;
#else
    int fd = ::open(lockFile.c_str(), O_RDWR);
    if (fd < 0) {
        return false;
    }
    const bool locked = ::flock(fd, LOCK_EX | LOCK_NB) != 0;
    ::flock(fd, LOCK_UN);
    ::close(fd);
    return locked;
#endif
}

} // namespace

ForeignWriterLock::ForeignWriterLock(const std::string& dir) {
    // 目录需先存在 (锁文件创建失败即视为未持锁)
    const auto lockFile = SessionWriterLease::lockFilePath(dir);
    std::error_code ec;
    fs::create_directories(utilxx_base::utf8ToPath(dir), ec);
#if XX_IS_WIN_D
    auto handle = ::CreateFileW(
        utilxx_base::utf8ToPath(lockFile).c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0, // 不共享: 其他打开者 (含 SessionWriterLease) 会失败
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (handle != INVALID_HANDLE_VALUE) {
        handle_ = handle;
        held_   = true;
    }
#else
    const int fd = ::open(lockFile.c_str(), O_CREAT | O_RDWR, 0644);
    if (fd >= 0) {
        if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
            handle_ = reinterpret_cast<void*>(static_cast<intptr_t>(fd));
            held_   = true;
        } else {
            ::close(fd);
        }
    }
#endif
}

ForeignWriterLock::~ForeignWriterLock() {
    if (!held_) {
        return;
    }
#if XX_IS_WIN_D
    if (handle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(handle_));
    }
#else
    const int fd = static_cast<int>(reinterpret_cast<intptr_t>(handle_));
    if (fd >= 0) {
        ::flock(fd, LOCK_UN);
        ::close(fd);
    }
#endif
    handle_ = nullptr;
    held_   = false;
}

TestResult testWriterLease() {
    auto root = makeTempRoot();

    // ---- 获取租约: 锁文件建立, 本进程持有一个目录 ----
    std::string err;
    auto        lease1 = SessionWriterLease::acquire(root, &err);
    XX_TEST_EXPECT_TRUE(lease1 != nullptr);
    XX_TEST_EXPECT_TRUE(fs::exists(utilxx_base::utf8ToPath(SessionWriterLease::lockFilePath(root)))
    );
    XX_TEST_EXPECT_EQ(SessionWriterLease::heldCount(), size_t{1});
    XX_TEST_EXPECT_TRUE(lease1 && lease1->dir().find(fs::path{root}.filename().string()) != std::string::npos
    );

    // ---- 进程内可重入: 同一目录再次获取成功且不新增持有点 ----
    auto lease2 = SessionWriterLease::acquire(root, &err);
    XX_TEST_EXPECT_TRUE(lease2 != nullptr);
    XX_TEST_EXPECT_EQ(SessionWriterLease::heldCount(), size_t{1});
    // 内核级互斥生效: 外部按原生方式独占会被拒绝
    XX_TEST_EXPECT_TRUE(osLockHeld(root));

    // ---- 释放一次仍持有 (引用计数) ----
    lease2.reset();
    XX_TEST_EXPECT_EQ(SessionWriterLease::heldCount(), size_t{1});
    XX_TEST_EXPECT_TRUE(osLockHeld(root));

    // ---- 全部释放后锁回收, 可再次独占 ----
    lease1.reset();
    XX_TEST_EXPECT_EQ(SessionWriterLease::heldCount(), size_t{0});
    XX_TEST_EXPECT_FALSE(osLockHeld(root));
    auto lease3 = SessionWriterLease::acquire(root, &err);
    XX_TEST_EXPECT_TRUE(lease3 != nullptr);
    // 重新获取后内核级锁又被本进程持有 (原生独占方式同样会被拒绝)
    XX_TEST_EXPECT_TRUE(osLockHeld(root));

    // ---- 不同目录互不影响 ----
    auto other     = makeTempRoot();
    auto leaseRoot  = std::move(lease3);
    auto leaseOther = SessionWriterLease::acquire(other, &err);
    XX_TEST_EXPECT_TRUE(leaseOther != nullptr);
    XX_TEST_EXPECT_EQ(SessionWriterLease::heldCount(), size_t{2});
    leaseRoot.reset();
    XX_TEST_EXPECT_EQ(SessionWriterLease::heldCount(), size_t{1});
    XX_TEST_EXPECT_FALSE(osLockHeld(root));  // 已释放
    XX_TEST_EXPECT_TRUE(osLockHeld(other));  // 仍被本进程持有 (原生独占方式同样失败)

    // ---- 目录不存在时自动创建 ----
    leaseOther.reset();
    XX_TEST_EXPECT_EQ(SessionWriterLease::heldCount(), size_t{0});
    auto nested = (fs::path{other} / "nested" / "session").string();
    auto lease4 = SessionWriterLease::acquire(nested, &err);
    XX_TEST_EXPECT_TRUE(lease4 != nullptr);
    XX_TEST_EXPECT_TRUE(fs::is_directory(utilxx_base::utf8ToPath(nested)));

    // ---- 释放后计数归零 (租约不泄漏) ----
    lease4.reset();
    XX_TEST_EXPECT_EQ(SessionWriterLease::heldCount(), size_t{0});
    XX_TEST_EXPECT_FALSE(osLockHeld(nested));

    // ---- 无法创建目录时报错返回 (失败语义: 调用方拿到原因, 不静默成功) ----
    // 用一个不可能创建的位置: 临时目录下已存在的普通文件当作父目录
    const auto        blocker = (fs::path{other} / "blocker").string();
    std::error_code   ec;
    {
        std::ofstream ofs{utilxx_base::utf8ToPath(blocker)};
        ofs << "x";
    }
    auto        badDir = (fs::path{blocker} / "sub").string();
    std::string badErr;
    auto        leaseBad = SessionWriterLease::acquire(badDir, &badErr);
    XX_TEST_EXPECT_TRUE(leaseBad == nullptr);
    XX_TEST_EXPECT_TRUE(!badErr.empty());

    removeTempRoot(root);
    removeTempRoot(other);
    return TestResult{g_wl_passed, g_wl_failed};
}

} // namespace test
} // namespace agentxx
