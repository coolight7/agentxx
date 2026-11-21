///
/// 测试夹具: 给"真实加载插件"的用例装上生产同款线程模型
///
/// 生产环境里插件代码只在插件执行线程上跑 (管理器构造即起线程), 阻塞工具与
/// `dlopen` 走一条工作池。测试默认没装执行器时, 插件代码直接跑在测试自己的 io
/// 线程上 —— "插件执行线程上不允许阻塞等表"这条约束就看不见, 于是这类降级
/// 一直不会被用例挡住。
///
/// 用法 (顺序不能变):
/// ```cpp
/// agentxx_test::PluginThreadFixture threads;
/// XX_TEST_EXPECT_TRUE(threads.start());
/// host->setIoExecutor(ex);
/// threads.attach(*host);              // 必须在加载任何插件之前
/// ... 加载/卸载用例 ...
/// host.reset();                        // 管理器先析构
/// threads.stop();                      // 再停线程与池
/// ```
///
/// - 线程起不来时 `start()` 返回 false, 用例可按"没有插件执行线程"的分支跳过;
/// - `attach` 是模板: 两端管理器 (`PluginManager` / `ClientPluginManager`) 都适用。
#pragma once

#include "asio/thread_pool.hpp"
#include "pluginxx/runtime/plugin_thread.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace agentxx {
namespace test {

/// 插件执行线程 + 工作池的持有者 (见文件头)
class PluginThreadFixture {
public:

    PluginThreadFixture() = default;

    ~PluginThreadFixture() { stop(); }

    PluginThreadFixture(const PluginThreadFixture&)            = delete;
    PluginThreadFixture& operator=(const PluginThreadFixture&) = delete;

    /// 起插件执行线程与工作池 (在测试 io 线程上调用)
    ///
    /// - `args`:
    ///     - [name] 线程名 (日志里能认出来)
    ///     - [workers] 工作池线程数 (生产默认 3, 测试给 2 够用)
    /// - `return` false = 线程或池没起来
    bool start(std::string_view name = "agentxx-test-plugin", std::size_t workers = 2) {
        auto thread = std::make_shared<pluginxx::PluginExecutorThread>();
        if (!thread->start(name)) {
            return false;
        }
        std::shared_ptr<asio::thread_pool> pool;
        try {
            pool = std::make_shared<asio::thread_pool>(workers == 0 ? 1 : workers);
        } catch (...) {
            thread->stop();
            return false;
        }
        thread_ = std::move(thread);
        pool_   = std::move(pool);
        return true;
    }

    /// 注入管理器 (**必须在加载任何插件之前**)
    template<typename Manager>
    void attach(Manager& mgr) {
        if (thread_) {
            mgr.setPluginExecutor(thread_->executor());
        }
        if (pool_) {
            mgr.setWorkerThreadPool(pool_);
        }
    }

    /// 停线程与池 (**必须在管理器析构之后**; 幂等)
    void stop() noexcept {
        if (pool_) {
            pool_->stop();
            pool_->join();
            pool_.reset();
        }
        if (thread_) {
            thread_->stop();
            thread_.reset();
        }
    }

    /// 是否真的装上了 (false = 用例按"没有插件线程"的降级分支处理)
    bool active() const noexcept { return thread_ != nullptr; }

private:

    std::shared_ptr<pluginxx::PluginExecutorThread> thread_;
    std::shared_ptr<asio::thread_pool>              pool_;
};

} // namespace test
} // namespace agentxx
