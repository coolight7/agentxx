/*
 * test_plugin_resources —— 插件会话资源扩展测试 (plugin_api v8)
 *
 * 覆盖:
 * - T1 清单资源段解析 (plugin.yaml skill/memory/mcp; 相对路径解析)
 * - T2 主配置 yaml 优先 (skill/memory/mcp 冲突拒绝)
 * - T3 运行时注册 (vtable register/unregister_skill_dir + get_own_resources
 *   快照) 与 MCP 注册/冲突/幂等注销
 * - T4 声明式资源: entry 成功后应用 → 中间件列表/componentInfo/所有权快照;
 *   disable 摘生效留记录 → enable 恢复; 卸载全部清除
 * - T5 加载失败 → 声明资源不生效 ("失败不生效"语义)
 */
#include "test_plugin_resources.h"

#include "agentxx/agent/resource_applier.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/middlewares/memory_file.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/middlewares/skill.h"
#include "agentxx/plugin/plugin_common.h"
#include "agentxx/plugin/plugin_iface_helper.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/util/log.h"
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_res_passed = 0;
int g_res_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_res_passed
#define XX_TEST_FAILED g_res_failed
namespace agentxx {
namespace test {

/// 定位示例插件库目录 (与 test_plugins 同策略: exe 同目录优先, cwd 回退)
static std::string findExamplePluginDir() {
    namespace fs = std::filesystem;
    std::error_code       ec;
    std::vector<fs::path> candidates;
#if defined(_WIN32)
    wchar_t buf[MAX_PATH];
    if (::GetModuleFileNameW(nullptr, buf, MAX_PATH) > 0) {
        candidates.push_back(fs::path(buf).parent_path() / "plugins" / "example_plugin");
    }
#else
    if (auto p = fs::read_symlink("/proc/self/exe", ec); !ec) {
        candidates.push_back(p.parent_path() / "plugins" / "example_plugin");
    }
#endif
    candidates.push_back(fs::current_path(ec) / "plugins" / "example_plugin");
    auto hasLibFile = [](const fs::path& dir) {
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
        if (fs::is_directory(c, ec) && hasLibFile(c)) {
            return c.string();
        }
    }
    return {};
}

static bool writeTextFile(const std::filesystem::path& p, std::string_view content) {
    std::error_code ec;
    auto            parent = p.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) {
        return false;
    }
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    return static_cast<bool>(f);
}

/// 拷贝示例插件动态库到目标文件 (返回是否成功)
/// - 目标名按 Linux 习惯书写 (.so), 与 manifest entry 一致; Windows/macOS 下
///   按平台修正扩展名 (.dll/.dylib), 与 lib 端 resolvePluginEntryPath 的
///   入口平台化映射保持一致, 否则 Windows 下按 .dll 查找会因文件不存在而
///   LoadLibrary 报 error 126
static bool copyExampleLib(const std::filesystem::path& target) {
    namespace fs  = std::filesystem;
    auto        ex = findExamplePluginDir();
    std::error_code ec;
    if (ex.empty()) {
        return false;
    }
    // 平台化目标扩展名 (manifest 按 Linux 书写, 见 resolvePluginEntryPath)
    auto dst = target.string();
#if defined(_WIN32)
    if (dst.ends_with(".so")) {
        dst.replace(dst.size() - 3, 3, ".dll");
    }
#elif defined(__APPLE__)
    if (dst.ends_with(".so")) {
        dst.replace(dst.size() - 3, 3, ".dylib");
    }
#endif
    for (fs::directory_iterator it(ex, ec), end; it != end; it.increment(ec)) {
        auto ext = it->path().extension().string();
        if (ext == ".so" || ext == ".dll" || ext == ".dylib") {
            fs::copy_file(it->path(), dst, fs::copy_options::overwrite_existing, ec);
            return !ec;
        }
    }
    return false;
}

static asio::awaitable<void> sleepMs(int ms) {
    auto timer
        = asio::steady_timer(co_await asio::this_coro::executor, std::chrono::milliseconds{ms});
    co_await timer.async_wait(asio::use_awaitable);
}

static bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

asio::awaitable<TestResult> run_plugin_resource_tests() {
    namespace fs = std::filesystem;
    g_res_passed = 0;
    g_res_failed = 0;

    // ---- 环境准备: 最小 AgentContext + 中间件 + 资源应用器 (io 线程) ----
    auto ctx                     = std::make_shared<agentxx::agent::AgentContext>();
    ctx->agentConfig             = std::make_shared<agentxx::agent::AgentConfig>();
    ctx->middlewareHandleContext = std::make_shared<agentxx::middleware::MiddlewareContext>();
    ctx->bus = std::make_shared<agentxx::event::EventBus>(co_await asio::this_coro::executor);
    ctx->toolRegistry  = std::make_shared<agentxx::plugin::ToolRegistry>();
    ctx->pluginManager = std::make_shared<agentxx::plugin::PluginManager>(ctx);
    ctx->pluginManager->setIoExecutor(co_await asio::this_coro::executor);

    auto skillMw = std::make_shared<agentxx::middleware::SkillMiddlewareHandle>(
        std::vector<std::string>{},
        ctx
    );
    auto memMw = std::make_shared<agentxx::middleware::MemoryFileMiddlewareHandle>(
        std::vector<std::string>{},
        ctx
    );
    // 不挂 handles 栈: 本测试直接断言中间件内部列表与所有权快照, 无需参与轮次
    auto applier = std::make_shared<agentxx::agent::AgentResourceApplier>(
        ctx,
        co_await asio::this_coro::executor,
        skillMw,
        memMw
    );
    ctx->resourceApplier = applier;

    // 临时工作目录 (进程内唯一; 结束时清理)
    auto tmpRoot = fs::temp_directory_path()
                 / ("agentxx_res_test_"
                    + std::to_string(
                        std::chrono::steady_clock::now().time_since_epoch().count()
                    ));
    std::error_code ec;
    fs::remove_all(tmpRoot, ec);
    fs::create_directories(tmpRoot, ec);

    /// 实例名由插件 get_info 决定 (manifest name 仅用于依赖检查/排序)
    const std::string ownerName = "example_plugin";

    // ================= T1. 清单资源段解析 =================
    TEST_INFO << "[T1] manifest resources parsing" << std::endl;
    {
        auto dir = tmpRoot / "manifest_ok";
        fs::create_directories(dir / "skills", ec);
        writeTextFile(dir / "notes.md", "x");
        writeTextFile(
            dir / "plugin.yaml",
            R"yaml(name: mres
entry: libmres.so
depends:
skill:
  - skills
memory:
  - notes.md
mcp:
  - namespace: mt
    url: https://a.example.com/sse
    timeout: 7
interfaces:
  require:
    - agentxx.agent.core
    - agentxx.client.panel
  optional:
    - agentxx.client.toast
)yaml"
        );
        std::string              name, entry;
        std::vector<std::string> depends, optDepends;
        plugin::PluginManifestResources    res;
        plugin::PluginManifestInterfaces   ifaces;
        XX_TEST_EXPECT_TRUE(plugin::parsePluginManifest(
            dir,
            name,
            entry,
            depends,
            optDepends,
            &res,
            &ifaces
        ));
        XX_TEST_EXPECT_EQ(name, "mres");
        XX_TEST_EXPECT_TRUE(res.skillDirs.size() == 1);
        if (res.skillDirs.size() == 1) {
            XX_TEST_EXPECT_EQ(res.skillDirs[0], (dir / "skills").lexically_normal().string());
        }
        XX_TEST_EXPECT_TRUE(res.memoryFiles.size() == 1);
        if (res.memoryFiles.size() == 1) {
            XX_TEST_EXPECT_EQ(res.memoryFiles[0], (dir / "notes.md").lexically_normal().string());
        }
        XX_TEST_EXPECT_TRUE(res.mcpServers.contains("mt"));
        if (res.mcpServers.contains("mt")) {
            XX_TEST_EXPECT_EQ(res.mcpServers["mt"].url, std::string{"https://a.example.com/sse"});
            XX_TEST_EXPECT_TRUE(res.mcpServers["mt"].timeoutMs == 7000);
        }
        // ---- 接口声明段解析 (接口协商; 见 plugin_common.h) ----
        XX_TEST_EXPECT_TRUE(ifaces.require.size() == 2);
        XX_TEST_EXPECT_TRUE(contains(ifaces.require, "agentxx.agent.core"));
        XX_TEST_EXPECT_TRUE(contains(ifaces.require, "agentxx.client.panel"));
        XX_TEST_EXPECT_TRUE(ifaces.optional.size() == 1);
        if (ifaces.optional.size() == 1) {
            XX_TEST_EXPECT_EQ(ifaces.optional[0], std::string{"agentxx.client.toast"});
        }

        // ---- 前缀归属: agentxx.agent.* 仅 agent 侧关心, agentxx.client.* 仅 client 侧 ----
        XX_TEST_EXPECT_TRUE(plugin::sideCaresAboutInterface("agentxx.agent.core", true));
        XX_TEST_EXPECT_FALSE(plugin::sideCaresAboutInterface("agentxx.agent.core", false));
        XX_TEST_EXPECT_TRUE(plugin::sideCaresAboutInterface("agentxx.client.panel", false));
        XX_TEST_EXPECT_FALSE(plugin::sideCaresAboutInterface("agentxx.client.panel", true));
        XX_TEST_EXPECT_TRUE(plugin::sideCaresAboutInterface("vendor.custom", true));
        XX_TEST_EXPECT_TRUE(plugin::sideCaresAboutInterface("vendor.custom", false));

        // ---- 宿主支持集门禁 (client 视角; v4 起宿主接口集为名字集合,
        //      位图映射已移除 —— 直接构造支持集) ----
        {
            plugin::InterfaceSet hostIf;
            hostIf.insert("agentxx.client.panel");
            hostIf.insert("agentxx.client.command");
            XX_TEST_EXPECT_TRUE(hostIf.contains("agentxx.client.panel"));
            XX_TEST_EXPECT_TRUE(hostIf.contains("agentxx.client.command"));
            XX_TEST_EXPECT_FALSE(hostIf.contains("agentxx.client.toast"));

            // 满足场景: require 中 agentxx.agent.* 被忽略 (client 视角), agentxx.client.panel
            // 受支持 → satisfied; 可选 agentxx.client.toast 缺失仅进 missingOptional
            auto r1 = plugin::checkInterfacesForSide(ifaces, hostIf, false);
            XX_TEST_EXPECT_TRUE(r1.satisfied);
            XX_TEST_EXPECT_TRUE(r1.missingRequired.empty());
            XX_TEST_EXPECT_TRUE(r1.missingOptional.size() == 1);
            XX_TEST_EXPECT_TRUE(r1.missingOptional[0] == "agentxx.client.toast");

            // 不满足场景: require 声明宿主不支持的接口 → 列出全部缺失项
            plugin::PluginManifestInterfaces unsat;
            unsat.require.push_back("agentxx.client.info_section"); // caps 未含
            unsat.require.push_back("vendor.custom");
            auto r1b = plugin::checkInterfacesForSide(unsat, hostIf, false);
            XX_TEST_EXPECT_TRUE(!r1b.satisfied);
            XX_TEST_EXPECT_TRUE(r1b.missingRequired.size() == 2);
            XX_TEST_EXPECT_TRUE(contains(r1b.missingRequired, "agentxx.client.info_section"));
            XX_TEST_EXPECT_TRUE(contains(r1b.missingRequired, "vendor.custom"));

            // agent 视角: agentxx.client.* 全部被忽略, agentxx.agent.core 受支持 → 满足
            // (agent 宿主支持集 = {agentxx.agent.core}, 见接口协商设计)
            plugin::InterfaceSet agentHostIf;
            agentHostIf.insert("agentxx.agent.core");
            auto r2 = plugin::checkInterfacesForSide(ifaces, agentHostIf, true);
            XX_TEST_EXPECT_TRUE(r2.satisfied && r2.missingRequired.empty());
            // vendor 前缀: 双方都检查 (agent 视角同样不满足)
            plugin::PluginManifestInterfaces vendorDecl;
            vendorDecl.require.push_back("vendor.custom");
            auto r3 = plugin::checkInterfacesForSide(vendorDecl, agentHostIf, true);
            XX_TEST_EXPECT_TRUE(!r3.satisfied);
        }

        // ---- 入口符号意图推导 ----
        {
            plugin::PluginManifestInterfaces d;
            d.require = {"agentxx.agent.core"};
            auto s1   = plugin::requiredEntrySides(d.require);
            XX_TEST_EXPECT_TRUE(s1.agentEntry && !s1.clientEntry);
            d.require = {"agentxx.client.panel", "agentxx.client.command"};
            auto s2   = plugin::requiredEntrySides(d.require);
            XX_TEST_EXPECT_TRUE(!s2.agentEntry && s2.clientEntry);
            d.require = {"vendor.x"};
            auto s3   = plugin::requiredEntrySides(d.require);
            XX_TEST_EXPECT_TRUE(s3.agentEntry && s3.clientEntry);
        }
    }

    // ================= T2. 主配置 yaml 优先 =================
    TEST_INFO << "[T2] main yaml priority on conflicts" << std::endl;
    {
        auto conflictDir = tmpRoot / "yaml_conflict";
        fs::create_directories(conflictDir, ec);
        auto memPath = (tmpRoot / "yaml_mem.md").lexically_normal().string();
        ctx->agentConfig->skillDirPaths.push_back(conflictDir.string());
        ctx->agentConfig->memoryFilePaths.push_back(memPath);

        std::string err;
        XX_TEST_EXPECT_FALSE(applier->addSkillDir("p_conflict", conflictDir.string(), err));
        XX_TEST_EXPECT_FALSE(applier->addMemoryFile("p_conflict", memPath, err));
        auto snap = applier->ownedBy("p_conflict");
        XX_TEST_EXPECT_TRUE(snap.skillDirs.empty() && snap.memoryFiles.empty());

        agentxx::agent::McpServerConfig mc;
        mc.url                            = "https://yaml.example";
        ctx->agentConfig->mcpServerUrls["yaml_ns"] = mc;
        XX_TEST_EXPECT_FALSE(applier->addMcpServer("p_conflict", "yaml_ns", mc, err));

        // 还原主配置 (后续测试依赖空配置)
        ctx->agentConfig->skillDirPaths.clear();
        ctx->agentConfig->memoryFilePaths.clear();
        ctx->agentConfig->mcpServerUrls.clear();
    }

    // ================= T3. 运行时注册 (agentxx.agent.resources 接口表) + MCP 注册/冲突/注销 =================
    TEST_INFO << "[T3] runtime registration via agentxx.agent.resources iface + mcp lifecycle"
              << std::endl;
    {
        auto hostDir = tmpRoot / "hostplug";
        fs::create_directories(hostDir, ec);
        XX_TEST_EXPECT_TRUE(copyExampleLib(hostDir / "libtest_host.so"));
        writeTextFile(hostDir / "plugin.yaml", "name: res_host\nentry: libtest_host.so\ndepends:\n");

        auto inst = co_await ctx->pluginManager->loadPluginAsync(hostDir.string());
        XX_TEST_EXPECT_TRUE(inst != nullptr);
        if (!inst) {
            co_return TestResult{g_res_passed, g_res_failed};
        }
        XX_TEST_EXPECT_EQ(inst->name, ownerName);
        const auto res3 = agentxx::plugin::AgentIfaces::query(&inst->host).resources;
        XX_TEST_EXPECT_TRUE(res3 != nullptr && res3->register_skill_dir != nullptr);

        // ---- 运行时注册 skill 目录 ----
        auto runtimeSkill = tmpRoot / "runtime_skills";
        fs::create_directories(runtimeSkill, ec);
        int rc = res3 ? res3->register_skill_dir(
            &inst->host,
            AGENTXX_SV(runtimeSkill.string().c_str()))
                      : -1;
        XX_TEST_EXPECT_EQ(rc, 0);
        XX_TEST_EXPECT_TRUE(contains(skillMw->skillDirPathList(), runtimeSkill.string()));

        // ---- 快照 JSON (get_own_resources) ----
        char* json = res3 && res3->get_own_resources ? res3->get_own_resources(&inst->host)
                                                     : nullptr;
        XX_TEST_EXPECT_TRUE(json != nullptr);
        if (json) {
            XX_TEST_EXPECT_TRUE(std::string_view(json).find("runtime_skills")
                                != std::string_view::npos);
            inst->host.vtable->free(json);
        }

        // ---- 重复注册幂等成功 ----
        rc = res3 ? res3->register_skill_dir(
            &inst->host,
            AGENTXX_SV(runtimeSkill.string().c_str()))
                  : -1;
        XX_TEST_EXPECT_EQ(rc, 0);

        // ---- 注销; 再注销非 0 ----
        rc = res3 ? res3->unregister_skill_dir(
            &inst->host,
            AGENTXX_SV(runtimeSkill.string().c_str()))
                  : -1;
        XX_TEST_EXPECT_EQ(rc, 0);
        XX_TEST_EXPECT_FALSE(contains(skillMw->skillDirPathList(), runtimeSkill.string()));
        rc = res3 ? res3->unregister_skill_dir(
            &inst->host,
            AGENTXX_SV(runtimeSkill.string().c_str()))
                  : 0;
        XX_TEST_EXPECT_TRUE(rc != 0);

        // ---- MCP 注册 (不可达 URL; 失败仅记日志, 所有权记录保留) ----
        const char* mcpSpec = R"({"namespace":"t_mcp","url":"https://127.0.0.1:9/sse","timeout":3})";
        rc = res3 ? res3->register_mcp_server(&inst->host, AGENTXX_SV(mcpSpec)) : -1;
        XX_TEST_EXPECT_EQ(rc, 0);
        co_await sleepMs(150); // 让连接协程跑一轮 (无论成败, 记录均在)
        {
            char* j2 = res3 && res3->get_own_resources ? res3->get_own_resources(&inst->host)
                                                       : nullptr;
            XX_TEST_EXPECT_TRUE(j2 != nullptr);
            if (j2) {
                XX_TEST_EXPECT_TRUE(std::string_view(j2).find("t_mcp") != std::string_view::npos);
                inst->host.vtable->free(j2);
            }
        }

        // ---- 主配置命名空间冲突 → 拒绝 ----
        agentxx::agent::McpServerConfig ycfg;
        ycfg.url                                    = "https://yaml.example";
        ctx->agentConfig->mcpServerUrls["yaml_ns2"] = ycfg;
        const char* specConflict = R"({"namespace":"yaml_ns2","url":"https://z"})";
        rc = res3 ? res3->register_mcp_server(&inst->host, AGENTXX_SV(specConflict)) : 0;
        XX_TEST_EXPECT_TRUE(rc != 0);

        // ---- 其他 owner 抢注同名命名空间 → 拒绝 (确定性: 所有权记录已存在) ----
        agentxx::agent::McpServerConfig anyCfg;
        anyCfg.url = "https://any";
        std::string err;
        XX_TEST_EXPECT_FALSE(applier->addMcpServer("other_owner", "t_mcp", anyCfg, err));

        // ---- 注销 (连接可能已失败: 幂等语义仍成功) ----
        rc = res3 ? res3->unregister_mcp_server(&inst->host, AGENTXX_SV("t_mcp")) : -1;
        XX_TEST_EXPECT_EQ(rc, 0);
        auto snapAfter = applier->ownedBy(ownerName);
        XX_TEST_EXPECT_FALSE(contains(snapAfter.mcpNamespaces, "t_mcp"));

        co_await ctx->pluginManager->unloadAsync(ownerName);
        XX_TEST_EXPECT_TRUE(ctx->pluginManager->find(ownerName) == nullptr);
    }

    // ================= T4. 声明式资源: 应用/disable-enable/卸载清理 =================
    TEST_INFO << "[T4] declarative resources apply/disable/enable/unload" << std::endl;
    {
        auto declDir = tmpRoot / "declplug";
        fs::create_directories(declDir / "skills/hello", ec);
        fs::create_directories(declDir / "assets", ec);
        writeTextFile(declDir / "skills/hello/SKILL.md",
                      "---\nname: hello-x\ndescription: demo\n---\n# hi\n");
        writeTextFile(declDir / "assets/NOTES.md", "# notes\n");
        XX_TEST_EXPECT_TRUE(copyExampleLib(declDir / "libdecl.so"));
        writeTextFile(
            declDir / "plugin.yaml",
            R"yaml(name: declp
entry: libdecl.so
depends:
skill:
  - skills
memory:
  - assets/NOTES.md
mcp:
  - namespace: t_decl
    url: https://127.0.0.1:9/sse
    timeout: 3
)yaml"
        );

        auto absSkill = (declDir / "skills").lexically_normal().string();
        auto absMem   = (declDir / "assets/NOTES.md").lexically_normal().string();

        auto inst = co_await ctx->pluginManager->loadPluginAsync(declDir.string());
        XX_TEST_EXPECT_TRUE(inst != nullptr);
        if (!inst) {
            co_return TestResult{g_res_passed, g_res_failed};
        }

        // ---- 应用断言: 所有权快照 / 中间件列表 / componentInfo ----
        auto snap = applier->ownedBy(ownerName);
        XX_TEST_EXPECT_TRUE(contains(snap.skillDirs, absSkill));
        XX_TEST_EXPECT_TRUE(contains(snap.memoryFiles, absMem));
        XX_TEST_EXPECT_TRUE(contains(snap.mcpNamespaces, "t_decl"));
        XX_TEST_EXPECT_TRUE(contains(skillMw->skillDirPathList(), absSkill));
        XX_TEST_EXPECT_TRUE(contains(memMw->memoryFilePathList(), absMem));
        XX_TEST_EXPECT_TRUE(contains(ctx->appendComponentInfo.skills, absSkill));
        XX_TEST_EXPECT_TRUE(contains(ctx->appendComponentInfo.memoryFiles, absMem));

        // ---- disable: 摘生效, 留记录 ----
        ctx->pluginManager->disable(ownerName);
        XX_TEST_EXPECT_FALSE(contains(skillMw->skillDirPathList(), absSkill));
        XX_TEST_EXPECT_FALSE(contains(memMw->memoryFilePathList(), absMem));
        snap = applier->ownedBy(ownerName);
        XX_TEST_EXPECT_TRUE(contains(snap.skillDirs, absSkill));

        // ---- enable: 恢复生效 ----
        ctx->pluginManager->enable(ownerName);
        XX_TEST_EXPECT_TRUE(contains(skillMw->skillDirPathList(), absSkill));
        XX_TEST_EXPECT_TRUE(contains(memMw->memoryFilePathList(), absMem));

        // ---- unload: 全部清除 (detachAll → removeAllOwned) ----
        auto ok = co_await ctx->pluginManager->unloadAsync(ownerName);
        XX_TEST_EXPECT_TRUE(ok);
        snap = applier->ownedBy(ownerName);
        XX_TEST_EXPECT_TRUE(snap.skillDirs.empty() && snap.memoryFiles.empty()
                            && snap.mcpNamespaces.empty());
        XX_TEST_EXPECT_FALSE(contains(skillMw->skillDirPathList(), absSkill));
        XX_TEST_EXPECT_FALSE(contains(memMw->memoryFilePathList(), absMem));
        XX_TEST_EXPECT_FALSE(contains(ctx->appendComponentInfo.skills, absSkill));
        XX_TEST_EXPECT_FALSE(contains(ctx->appendComponentInfo.memoryFiles, absMem));
    }

    // ================= T5. 加载失败 → 声明资源不生效 =================
    TEST_INFO << "[T5] load failure keeps declared resources unapplied" << std::endl;
    {
        auto failDir = tmpRoot / "failplug";
        fs::create_directories(failDir / "skills", ec);
        writeTextFile(
            failDir / "plugin.yaml",
            R"yaml(name: failp
entry: libmissing.so
depends:
skill:
  - skills
memory:
  - notes.md
mcp:
  - namespace: t_fail
    url: https://127.0.0.1:9/sse
)yaml"
        );
        auto beforeSkills = skillMw->skillDirPathList();
        auto inst         = co_await ctx->pluginManager->loadPluginAsync(failDir.string());
        XX_TEST_EXPECT_TRUE(inst == nullptr);
        auto snap = applier->ownedBy("failp");
        XX_TEST_EXPECT_TRUE(snap.skillDirs.empty() && snap.memoryFiles.empty()
                            && snap.mcpNamespaces.empty());
        XX_TEST_EXPECT_TRUE(skillMw->skillDirPathList() == beforeSkills); // 未被污染
    }

    // 清理临时目录
    ctx->resourceApplier.reset();
    ctx->pluginManager.reset();
    fs::remove_all(tmpRoot, ec);

    co_return TestResult{g_res_passed, g_res_failed};
}

} // namespace test
} // namespace agentxx
