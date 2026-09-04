#include "test_cpu_gpu_use.h"
#include "agentxx/agent/context.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/plugin/tool_registry.h"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_cpu_passed = 0;
int g_cpu_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_cpu_passed
#define XX_TEST_FAILED g_cpu_failed

namespace agentxx {
namespace test {

#if XX_IS_WIN_D || XX_IS_LINUX_D

namespace {

/// 定位 agentxx_system_monitor 插件目录
/// 优先 exe 同目录的构建产物, cwd 仅作回退; 校验目录内存在动态库产物,
/// 避免 cwd 在源码仓库下时误命中 agent/plugins/ 下的插件源码目录
static std::string findSystemMonitorPluginPath() {
    std::error_code                    ec;
    std::vector<std::filesystem::path> candidates;
#if XX_IS_WIN_D
    wchar_t buf[4096];
    DWORD   n = ::GetModuleFileNameW(nullptr, buf, 4096);
    if (n > 0 && n < 4096) {
        int len = ::WideCharToMultiByte(
            CP_UTF8,
            0,
            buf,
            static_cast<int>(n),
            nullptr,
            0,
            nullptr,
            nullptr
        );
        if (len > 0) {
            std::string exe(static_cast<size_t>(len), '\0');
            ::WideCharToMultiByte(
                CP_UTF8,
                0,
                buf,
                static_cast<int>(n),
                exe.data(),
                len,
                nullptr,
                nullptr
            );
            candidates.push_back(
                std::filesystem::path(exe).parent_path() / "plugins" / "agentxx_system_monitor"
            );
        }
    }
#else
    // Linux: 回退可执行文件同目录 (兼容从其他 cwd 运行, 与 test_codegraph_tools 一致)
    if (auto p = std::filesystem::read_symlink("/proc/self/exe", ec); !ec) {
        candidates.push_back(p.parent_path() / "plugins" / "agentxx_system_monitor");
    }
#endif
    candidates.push_back(std::filesystem::current_path(ec) / "plugins" / "agentxx_system_monitor");
    auto hasLibFile = [](const std::filesystem::path& dir) {
        std::error_code                     ec2;
        std::filesystem::directory_iterator it(dir, ec2);
        std::filesystem::directory_iterator end;
        for (; it != end; it.increment(ec2)) {
            auto ext = it->path().extension().string();
            if (ext == ".so" || ext == ".dll" || ext == ".dylib") {
                return true;
            }
        }
        return false;
    };
    for (const auto& c : candidates) {
        if (std::filesystem::is_directory(c, ec) && hasLibFile(c)) {
            return c.string();
        }
    }
    return "plugins/agentxx_system_monitor"; // 让加载失败暴露日志
}

} // namespace

#endif // XX_IS_WIN_D || XX_IS_LINUX_D

asio::awaitable<TestResult>
    run_cpu_gpu_use_tests(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    g_cpu_passed = 0;
    g_cpu_failed = 0;

#if XX_IS_WIN_D || XX_IS_LINUX_D
    // ---- 1. 构造 AgentContext ----
    auto ctx                     = std::make_shared<agentxx::agent::AgentContext>();
    ctx->agentConfig             = std::make_shared<agentxx::agent::AgentConfig>();
    ctx->middlewareHandleContext = std::make_shared<agentxx::middleware::MiddlewareContext>();
    ctx->toolRegistry            = std::make_shared<agentxx::plugin::ToolRegistry>();
    ctx->pluginManager           = std::make_shared<agentxx::plugin::PluginManager>(ctx);
    ctx->pluginManager->setIoExecutor(co_await asio::this_coro::executor);

    // ---- 2. 加载 agentxx_system_monitor 插件 ----
    auto path = findSystemMonitorPluginPath();
    XX_TEST_EXPECT_TRUE(path.find("agentxx_system_monitor") != std::string::npos);
    auto inst = co_await ctx->pluginManager->loadPluginAsync(path);
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (!inst) {
        co_return TestResult{g_cpu_passed, g_cpu_failed};
    }
    XX_TEST_EXPECT_EQ(inst->name, "agentxx_system_monitor");
    XX_TEST_EXPECT_TRUE(ctx->toolRegistry->contains("agentxx_get_system_core_info"));

    // ---- 3. 工具 agentxx_get_system_core_info (原内置工具迁移) ----
    {
        auto tool = ctx->toolRegistry->find("agentxx_get_system_core_info");
        XX_TEST_EXPECT_TRUE(tool != nullptr);
        if (tool) {
            auto out = co_await tool->execute_async(neograph::json::object());
            TEST_INFO << "get_system_core_info output:\n" << out << std::endl;
            XX_TEST_EXPECT_TRUE(out.find("CPU Usage:") != std::string::npos);
            XX_TEST_EXPECT_TRUE(out.find("Memory:") != std::string::npos);
        }
    }

    // ---- 4. 能力 agentxx.system_usage (agent 侧周期采集 publish 的数据源) ----
    {
        XX_TEST_EXPECT_TRUE(ctx->pluginManager->hasCapability("agentxx.system_usage"));
        AgentxxPluginString err{nullptr, 0};
        int                 opStatus = -1;
        std::string         payload;
        bool                done = false;
        using StateTuple         = std::tuple<int*, std::string*, bool*>;
        StateTuple state{&opStatus, &payload, &done};

        auto* op = ctx->pluginManager->invokeCapabilityAsync(
            nullptr,
            "agentxx.system_usage",
            "query",
            "{}",
            [](void* ud, int32_t st, const AgentxxPluginStringView* pl) {
                auto* s          = static_cast<StateTuple*>(ud);
                *std::get<0>(*s) = st;
                if (pl && pl->data && pl->size > 0) {
                    std::get<1>(*s)->assign(pl->data, pl->size);
                }
                *std::get<2>(*s) = true;
            },
            &state,
            &err
        );
        XX_TEST_EXPECT_TRUE(op != nullptr);
        while (!done) {
            asio::steady_timer t(co_await asio::this_coro::executor);
            t.expires_after(std::chrono::milliseconds(2));
            co_await t.async_wait(asio::use_awaitable);
        }
        XX_TEST_EXPECT_EQ(opStatus, AGENTXX_PLUGIN_OPERATOR_OK);
        if (!payload.empty()) {
            auto j = neograph::json::parse(payload);
            if (j.is_object()) {
                double cpu = j.value("cpu", -1.0);
                XX_TEST_EXPECT_TRUE(cpu >= 0.0 && cpu <= 100.0);
                uint64_t total = j.value("mem_total_mb", uint64_t{0});
                XX_TEST_EXPECT_TRUE(total > 0);
                XX_TEST_EXPECT_TRUE(j.contains("gpus"));
            } else {
                g_cpu_failed++;
                TEST_FAIL << "system_usage capability returned non-object json" << std::endl;
            }
        } else {
            if (err.data) {
                TEST_FAIL << "system_usage capability failed: " << err.data << std::endl;
                std::free(err.data);
            } else {
                g_cpu_failed++;
                TEST_FAIL << "system_usage capability failed (no error)" << std::endl;
            }
        }
    }

    // ---- 5. spawn 宿主托管: 周期采集任务注册到宿主 + 卸载闭环 ----
    // (agentxx_system_monitor 在 create 内 spawn 周期采集任务; spawn
    // 经 agentxx.agent.tasks 接口表注册到宿主: 句柄在 outstandingOps +
    // inflight+1。卸载时 detachAll cancel → 协程退出 → notify → inflight 归零
    // → waitInflightZero 精确等待 → dlclose 安全, 无协程帧悬挂)
    {
        // spawn 协程在 io 线程 post 启动, 轮询等待注册完成 (句柄进入列表)
        bool spawned = false;
        for (int i = 0; i < 100 && !spawned; ++i) {
            auto n = inst->outstandingOps.size();
            // 采集 spawn 注册后宿主列表至少 1 项 (可能还有 client_attached 事件
            // 触发的 offload 等, 此处只要求非空)
            if (n > 0) {
                spawned = true;
            } else {
                asio::steady_timer t(co_await asio::this_coro::executor);
                t.expires_after(std::chrono::milliseconds(10));
                co_await t.async_wait(asio::use_awaitable);
            }
        }
        XX_TEST_EXPECT_TRUE(spawned);
        if (spawned) {
            // spawn 协程持 inflight (waitInflightZero 的计数来源)
            size_t inflightBefore = inst->inflight.load(std::memory_order_acquire);
            XX_TEST_EXPECT_TRUE(inflightBefore > 0);

            // 卸载: detachAll cancel spawn → 协程退出 → notify → inflight 归零;
            // 若 spawn 未被宿主托管 (游离), 卸载仍会返回但 inflight 无法等待到
            // spawn 的退出, 这里用耗时粗略区分 (托管: 立即归零, 卸载快)
            auto t0       = std::chrono::steady_clock::now();
            bool unloaded = co_await ctx->pluginManager->unloadAsync("agentxx_system_monitor");
            auto elapsed  = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - t0
            )
                               .count();
            XX_TEST_EXPECT_TRUE(unloaded);
            XX_TEST_EXPECT_TRUE(elapsed < 5000); ///< 不依赖 30s 超时 (waitInflightZero 精确等待)
            // 卸载完成 → 实例已移除; spawn 句柄已随实例清理
            XX_TEST_EXPECT_TRUE(ctx->pluginManager->find("agentxx_system_monitor") == nullptr);
        } else {
            // 注册未观察到 (异常场景): 仍执行卸载保证后续清理
            co_await ctx->pluginManager->unloadAsync("agentxx_system_monitor");
        }
    }

    XX_TEST_EXPECT_FALSE(ctx->toolRegistry->contains("agentxx_get_system_core_info"));
    XX_TEST_EXPECT_TRUE(ctx->pluginManager->find("agentxx_system_monitor") == nullptr);

    // ---- 6. spawn 卸载回收循环: 反复 load/unload 无协程帧悬挂 ----
    // 每次加载都会 spawn 周期采集任务; 若卸载未回收 spawn (旧缺陷: 协程帧 +
    // 内部对象泄漏 504B/次), 循环多次会累积。此处显式断言每次卸载后:
    // - 卸载快速返回 (waitInflightZero 等到 spawn 退出, 非 30s 超时)
    // - 实例彻底移除 (句柄/协程随 destroy 释放)
    {
        bool loopOk = true;
        for (int i = 0; i < 3 && loopOk; ++i) {
            auto instL = co_await ctx->pluginManager->loadPluginAsync(path);
            if (!instL) {
                loopOk = false;
                break;
            }
            // 等待 spawn 注册完成 (协程挂起于 sleep, inflight 持住)
            bool spawned = false;
            for (int j = 0; j < 100 && !spawned; ++j) {
                if (!instL->outstandingOps.empty()) {
                    spawned = true;
                } else {
                    asio::steady_timer t(co_await asio::this_coro::executor);
                    t.expires_after(std::chrono::milliseconds(10));
                    co_await t.async_wait(asio::use_awaitable);
                }
            }
            if (!spawned) {
                loopOk = false;
                break;
            }
            auto t0      = std::chrono::steady_clock::now();
            auto ok      = co_await ctx->pluginManager->unloadAsync("agentxx_system_monitor");
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - t0
            )
                               .count();
            if (!ok || elapsed >= 5000) {
                loopOk = false;
                break;
            }
            XX_TEST_EXPECT_TRUE(ctx->pluginManager->find("agentxx_system_monitor") == nullptr);
        }
        XX_TEST_EXPECT_TRUE(loopOk);
    }
#else
    TEST_SKIP << "agentxx_system_monitor (system monitor) 仅支持 Windows/Linux 平台" << std::endl;
#endif

    (void)agentContext;
    co_return TestResult{g_cpu_passed, g_cpu_failed};
}

} // namespace test
} // namespace agentxx
