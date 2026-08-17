/*
 * test_client_plugins.cpp —— client 侧插件系统测试 (模块名 `client_plugins`)
 *
 * 覆盖:
 * 1. 加载 example_plugin 的 client 入口 (agentxx_client_entry)
 * 2. UI 注册表: 状态栏项 / 面板 / Info 栏段落 / 命令 (含快照读取)
 * 3. 事件分发: READY / TURN_END / PLUGIN_DATA / USER_INPUT → 插件回调
 * 4. 命令执行: /example (send 动作) / example_toast (toast 动作)
 * 5. 跨端数据: send_plugin_data → adapter sendPluginData (WirePluginDataUp 路径)
 * 6. disable/enable 恢复与 unload 清理 (含 Info 栏段落注册/摘除信号)
 */
#include "test_client_plugins.h"

#include "agentxx/plugin/client_plugin_manager.h"
#include "agentxx/util/log.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "asio/use_awaitable.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace agentxx {
namespace test {

int g_client_plugin_passed = 0;
int g_client_plugin_failed = 0;

/// 定位示例插件目录 (与 agent 侧 test_plugins 同路径: cwd/plugins/example_plugin)
static std::string findExamplePluginPath() {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto            cwd = fs::current_path(ec) / "plugins" / "example_plugin";
    if (!ec) {
        return cwd.string();
    }
    return "plugins/example_plugin";
}

/// Mock UI 适配器: 记录信号 (线程安全)
class MockPluginUiAdapter : public agentxx::plugin::PluginUiAdapter {
public:

    uint32_t uiCaps() const override {
        return AGENTXX_UI_CAP_STATUS_ITEM | AGENTXX_UI_CAP_PANEL | AGENTXX_UI_CAP_TOAST
               | AGENTXX_UI_CAP_INFO_SECTION;
    }

    void onStatusItemRegistered(
        const std::string& id,
        const neograph::json& /*props*/,
        int /*align*/,
        int /*order*/
    ) override {
        std::lock_guard<std::mutex> lock(m_);
        ++statusRegistered_;
        lastStatusId_ = id;
    }

    void onStatusItemUpdated(const std::string& id, const neograph::json& /*props*/) override {
        std::lock_guard<std::mutex> lock(m_);
        ++statusUpdated_;
        lastStatusId_ = id;
    }

    void onStatusItemRemoved(const std::string& id) override {
        std::lock_guard<std::mutex> lock(m_);
        ++statusRemoved_;
        lastStatusId_ = id;
    }

    void onPanelRegistered(const std::string& id, const neograph::json& /*props*/) override {
        std::lock_guard<std::mutex> lock(m_);
        ++panelRegistered_;
        lastPanelId_ = id;
    }

    void onPanelUpdated(const std::string& id, const neograph::json& /*items*/) override {
        std::lock_guard<std::mutex> lock(m_);
        ++panelUpdated_;
        lastPanelId_ = id;
    }

    void onPanelRemoved(const std::string& id) override {
        std::lock_guard<std::mutex> lock(m_);
        ++panelRemoved_;
        lastPanelId_ = id;
    }

    void onInfoSectionRegistered(
        const std::string& id,
        const neograph::json& /*props*/
    ) override {
        std::lock_guard<std::mutex> lock(m_);
        ++infoSectionRegistered_;
        lastInfoSectionId_ = id;
    }

    void onInfoSectionUpdated(const std::string& id, const neograph::json& /*items*/) override {
        std::lock_guard<std::mutex> lock(m_);
        ++infoSectionUpdated_;
        lastInfoSectionId_ = id;
    }

    void onInfoSectionRemoved(const std::string& id) override {
        std::lock_guard<std::mutex> lock(m_);
        ++infoSectionRemoved_;
        lastInfoSectionId_ = id;
    }

    void onToast(const std::string& text, int level) override {
        std::lock_guard<std::mutex> lock(m_);
        ++toastCount_;
        lastToast_    = text;
        lastToastLvl_ = level;
    }

    void sendPluginMessage(const std::string& text) override {
        std::lock_guard<std::mutex> lock(m_);
        ++sendCount_;
        lastSent_ = text;
    }

    bool sendPluginData(
        const std::string& plugin,
        const std::string& event,
        const std::string& json
    ) override {
        std::lock_guard<std::mutex> lock(m_);
        ++dataUpCount_;
        lastDataPlugin_ = plugin;
        lastDataEvent_  = event;
        lastDataJson_   = json;
        return true;
    }

    // ---- 读取 (加锁) ----
    int statusRegistered() const {
        std::lock_guard<std::mutex> lock(m_);
        return statusRegistered_;
    }
    int statusUpdated() const {
        std::lock_guard<std::mutex> lock(m_);
        return statusUpdated_;
    }
    int panelRegistered() const {
        std::lock_guard<std::mutex> lock(m_);
        return panelRegistered_;
    }
    int panelUpdated() const {
        std::lock_guard<std::mutex> lock(m_);
        return panelUpdated_;
    }
    int infoSectionRegistered() const {
        std::lock_guard<std::mutex> lock(m_);
        return infoSectionRegistered_;
    }
    int infoSectionUpdated() const {
        std::lock_guard<std::mutex> lock(m_);
        return infoSectionUpdated_;
    }
    int infoSectionRemoved() const {
        std::lock_guard<std::mutex> lock(m_);
        return infoSectionRemoved_;
    }
    int toastCount() const {
        std::lock_guard<std::mutex> lock(m_);
        return toastCount_;
    }
    int sendCount() const {
        std::lock_guard<std::mutex> lock(m_);
        return sendCount_;
    }
    int dataUpCount() const {
        std::lock_guard<std::mutex> lock(m_);
        return dataUpCount_;
    }
    std::string lastSent() const {
        std::lock_guard<std::mutex> lock(m_);
        return lastSent_;
    }
    std::string lastToast() const {
        std::lock_guard<std::mutex> lock(m_);
        return lastToast_;
    }
    int lastToastLvl() const {
        std::lock_guard<std::mutex> lock(m_);
        return lastToastLvl_;
    }
    std::string lastDataPlugin() const {
        std::lock_guard<std::mutex> lock(m_);
        return lastDataPlugin_;
    }
    std::string lastDataEvent() const {
        std::lock_guard<std::mutex> lock(m_);
        return lastDataEvent_;
    }

private:

    mutable std::mutex m_;
    int                statusRegistered_ = 0;
    int                statusUpdated_    = 0;
    int                statusRemoved_    = 0;
    int                panelRegistered_  = 0;
    int                panelUpdated_     = 0;
    int                panelRemoved_     = 0;
    int                infoSectionRegistered_ = 0;
    int                infoSectionUpdated_    = 0;
    int                infoSectionRemoved_    = 0;
    int                toastCount_       = 0;
    int                sendCount_        = 0;
    int                dataUpCount_      = 0;
    int                lastToastLvl_     = 0;
    std::string        lastStatusId_;
    std::string        lastPanelId_;
    std::string        lastInfoSectionId_;
    std::string        lastToast_;
    std::string        lastSent_;
    std::string        lastDataPlugin_;
    std::string        lastDataEvent_;
    std::string        lastDataJson_;
};

asio::awaitable<TestResult> run_client_plugin_tests() {
    g_client_plugin_passed = 0;
    g_client_plugin_failed = 0;

    auto ex      = co_await asio::this_coro::executor;
    auto adapter = std::make_shared<MockPluginUiAdapter>();
    auto mgr     = std::make_shared<agentxx::plugin::ClientPluginManager>(ex);
    mgr->setUiAdapter(adapter);
    mgr->setThreadId("sess-test");

    // ---- 1. 加载 example_plugin (client 入口) ----
    auto path = findExamplePluginPath();
    XX_TEST_EXPECT_TRUE(path.find("example_plugin") != std::string::npos);
    auto inst = co_await mgr->loadNativeAsync(path);
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    if (!inst) {
        co_return TestResult{g_client_plugin_passed, g_client_plugin_failed};
    }
    XX_TEST_EXPECT_EQ(inst->name, "example_plugin");

    // ---- 2. UI 注册表 ----
    {
        auto reg = mgr->uiRegistrySnapshot();
        XX_TEST_EXPECT_TRUE(reg != nullptr);
        if (reg) {
            bool hasStatus = false;
            for (const auto& s : reg->statusItems) {
                if (s.id == "example_plugin.turns") {
                    hasStatus = true;
                    XX_TEST_EXPECT_EQ(s.text, "turns: 0");
                }
            }
            XX_TEST_EXPECT_TRUE(hasStatus);
            bool hasPanel = false;
            for (const auto& p : reg->panels) {
                if (p.id == "example_plugin.panel") {
                    hasPanel = true;
                    XX_TEST_EXPECT_EQ(p.title, "Example");
                }
            }
            XX_TEST_EXPECT_TRUE(hasPanel);
            bool hasInfo = false;
            for (const auto& s : reg->infoSections) {
                if (s.id == "example_plugin.info") {
                    hasInfo = true;
                    XX_TEST_EXPECT_EQ(s.title, "Example Info");
                }
            }
            XX_TEST_EXPECT_TRUE(hasInfo);
            bool hasCmd1 = false, hasCmd2 = false;
            for (const auto& c : reg->commands) {
                if (c.name == "example") {
                    hasCmd1 = true;
                }
                if (c.name == "example_toast") {
                    hasCmd2 = true;
                }
            }
            XX_TEST_EXPECT_TRUE(hasCmd1);
            XX_TEST_EXPECT_TRUE(hasCmd2);
        }
    }
    XX_TEST_EXPECT_TRUE(mgr->hasCommand("example"));
    XX_TEST_EXPECT_TRUE(mgr->hasCommand("example_toast"));
    XX_TEST_EXPECT_FALSE(mgr->hasCommand("no_such_command"));

    // 插件 loadNativeAsync 在 io 线程, entry 的注册经 ioCallSync 同步完成:
    // 适配器应已收到注册信号
    XX_TEST_EXPECT_EQ(adapter->statusRegistered(), 1);
    XX_TEST_EXPECT_EQ(adapter->panelRegistered(), 1);
    XX_TEST_EXPECT_EQ(adapter->infoSectionRegistered(), 1);

    // ---- 3. 事件分发 ----
    // READY: 插件回调更新状态栏 + 跨端 send_plugin_data("hello")
    mgr->onReady();
    XX_TEST_EXPECT_TRUE(adapter->dataUpCount() >= 1);
    XX_TEST_EXPECT_EQ(adapter->lastDataEvent(), "hello");
    XX_TEST_EXPECT_EQ(adapter->lastDataPlugin(), "example_plugin");

    // TURN_END: 状态栏项文本更新 (turns: 1)
    {
        agentxx::agent::WireTurnResult r;
        r.threadId = "sess-test";
        r.hasError = false;
        mgr->onTurnResult(r);
    }
    {
        auto reg = mgr->uiRegistrySnapshot();
        if (reg) {
            bool found = false;
            for (const auto& s : reg->statusItems) {
                if (s.id == "example_plugin.turns") {
                    found = true;
                    XX_TEST_EXPECT_EQ(s.text, "turns: 1");
                }
            }
            XX_TEST_EXPECT_TRUE(found);
            // Info 段落: TURN_END 时插件更新内容 (kv Turns=1 + text)
            bool infoFound = false;
            for (const auto& s : reg->infoSections) {
                if (s.id == "example_plugin.info" && s.items.is_array()
                    && !s.items.empty()) {
                    infoFound = true;
                    const auto first = s.items[0];
                    XX_TEST_EXPECT_EQ(first.value("kind", std::string{}), "kv");
                    XX_TEST_EXPECT_EQ(first.value("value", std::string{}), "1");
                }
            }
            XX_TEST_EXPECT_TRUE(infoFound);
        }
    }
    XX_TEST_EXPECT_TRUE(adapter->infoSectionUpdated() >= 1);

    // USER_INPUT: 通知事件 (payload 含 text)
    mgr->onUserInput("sess-test", "hello world");

    // PLUGIN_DATA: 面板内容更新 (adapter onPanelUpdated)
    {
        agentxx::agent::WirePluginData d;
        d.plugin = "example_plugin";
        d.event  = "progress";
        d.data   = R"({"done":1})";
        mgr->onPluginData(d);
    }
    XX_TEST_EXPECT_TRUE(adapter->panelUpdated() >= 1);

    // ---- 4. 命令执行 ----
    // /example (send 动作)
    mgr->invokeCommand("example", R"({"text":"hi"})");
    XX_TEST_EXPECT_EQ(adapter->sendCount(), 1);
    XX_TEST_EXPECT_TRUE(adapter->lastSent().find("Hello from example plugin") != std::string::npos);
    XX_TEST_EXPECT_TRUE(adapter->lastSent().find("(hi)") != std::string::npos);

    // example_toast (toast 动作)
    mgr->invokeCommand("example_toast", R"({"text":"notice"})");
    XX_TEST_EXPECT_EQ(adapter->toastCount(), 1);
    XX_TEST_EXPECT_EQ(adapter->lastToast(), "notice");
    XX_TEST_EXPECT_EQ(adapter->lastToastLvl(), 1);

    // 未注册命令: 记日志不崩溃
    mgr->invokeCommand("no_such_command", "{}");

    // ---- 5. 跨端数据 (vtable send_plugin_data 路径) ----
    {
        int rc = inst->host.vtable->send_plugin_data(
            &inst->host,
            AGENTXX_SV("rebuild"),
            AGENTXX_SV(R"({"x":1})")
        );
        XX_TEST_EXPECT_EQ(rc, 0);
    }
    XX_TEST_EXPECT_TRUE(adapter->dataUpCount() >= 2);
    XX_TEST_EXPECT_EQ(adapter->lastDataEvent(), "rebuild");

    // ---- 6. disable / enable ----
    mgr->disable("example_plugin");
    {
        auto reg = mgr->uiRegistrySnapshot();
        XX_TEST_EXPECT_TRUE(reg != nullptr);
        if (reg) {
            bool any = false;
            for (const auto& s : reg->statusItems) {
                if (s.plugin == "example_plugin") {
                    any = true;
                }
            }
            for (const auto& p : reg->panels) {
                if (p.plugin == "example_plugin") {
                    any = true;
                }
            }
            for (const auto& s : reg->infoSections) {
                if (s.plugin == "example_plugin") {
                    any = true;
                }
            }
            for (const auto& c : reg->commands) {
                if (c.plugin == "example_plugin") {
                    any = true;
                }
            }
            XX_TEST_EXPECT_FALSE(any); // 全部摘除
        }
    }
    XX_TEST_EXPECT_TRUE(adapter->infoSectionRemoved() >= 1);
    XX_TEST_EXPECT_FALSE(mgr->hasCommand("example"));
    // disable 期间命令不执行
    mgr->invokeCommand("example", R"({"text":"x"})");
    XX_TEST_EXPECT_EQ(adapter->sendCount(), 1); // 未增加

    mgr->enable("example_plugin");
    {
        auto reg = mgr->uiRegistrySnapshot();
        bool hasCmd = false, hasStatus = false, hasInfo = false;
        if (reg) {
            for (const auto& c : reg->commands) {
                if (c.name == "example") {
                    hasCmd = true;
                }
            }
            for (const auto& s : reg->statusItems) {
                if (s.id == "example_plugin.turns") {
                    hasStatus = true;
                }
            }
            for (const auto& s : reg->infoSections) {
                if (s.id == "example_plugin.info") {
                    hasInfo = true;
                }
            }
        }
        XX_TEST_EXPECT_TRUE(hasCmd);
        XX_TEST_EXPECT_TRUE(hasStatus);
        XX_TEST_EXPECT_TRUE(hasInfo);
    }
    XX_TEST_EXPECT_TRUE(mgr->hasCommand("example"));

    // ---- 7. unload 清理 ----
    bool unloaded = co_await mgr->unloadAsync("example_plugin");
    XX_TEST_EXPECT_TRUE(unloaded);
    {
        auto reg = mgr->uiRegistrySnapshot();
        bool any = false;
        if (reg) {
            for (const auto& s : reg->statusItems) {
                if (s.plugin == "example_plugin") {
                    any = true;
                }
            }
            for (const auto& p : reg->panels) {
                if (p.plugin == "example_plugin") {
                    any = true;
                }
            }
            for (const auto& s : reg->infoSections) {
                if (s.plugin == "example_plugin") {
                    any = true;
                }
            }
            for (const auto& c : reg->commands) {
                if (c.plugin == "example_plugin") {
                    any = true;
                }
            }
        }
        XX_TEST_EXPECT_FALSE(any);
    }
    // 插件卸载时主动反注册 (unload 回调): adapter 信号计数增加
    XX_TEST_EXPECT_TRUE(adapter->infoSectionRemoved() >= 2);
    XX_TEST_EXPECT_TRUE(mgr->find("example_plugin") == nullptr);

    co_return TestResult{g_client_plugin_passed, g_client_plugin_failed};
}

} // namespace test
} // namespace agentxx
