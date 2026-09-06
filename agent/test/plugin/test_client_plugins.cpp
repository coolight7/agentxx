/// test_client_plugins.cpp —— client 侧插件系统测试 (模块名 `client_plugins`)
///
/// 覆盖:
/// 1. 加载 example_plugin 的 client 入口 (agentxx_plugin_client_create)
/// 2. UI 注册表: 状态栏项 / 面板 / Info 栏段落 / 命令 (含快照读取)
/// 3. 事件分发: READY / TURN_END / PLUGIN_DATA / USER_INPUT → 插件回调
/// 4. 命令执行: /example (send 动作) / example_toast (toast 动作)
/// 5. 跨端数据: send_plugin_data → adapter sendPluginData (WirePluginDataUp 路径)
/// 6. disable/enable 恢复与 unload 清理 (含 Info 栏段落注册/摘除信号)
/// 18. 通用交互: bind/unbind 覆盖 + 精确优先 vs fallback + 上下文透传
/// 19. 通用 overlay: open/close 参数校验 + 适配器信号
/// 20. planning 端到端: dispatch(open_graph) → open_overlay(MERMAID)
/// 21. kit::ActionController 单测 (header-only, 无宿主)
#include "test_client_plugins.h"

#include "agentxx-client/io/tui/plugin_ui_items.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "agentxx/plugin/client_plugin_manager.h"
#include "agentxx/util/log.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "asio/use_awaitable.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_client_plugin_passed = 0;
int g_client_plugin_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_client_plugin_passed
#define XX_TEST_FAILED g_client_plugin_failed

namespace agentxx {
namespace test {

/// 定位插件目录 (与 agent 侧 test_plugins 同路径: cwd/plugins/<name>)
/// 兼容从其他 cwd 运行: 优先 exe 同目录的构建产物, cwd 仅作回退;
/// 校验目录内存在动态库产物, 避免误命中 agent/plugins/ 源码目录
static std::string findPluginPath(const std::string& name) {
    namespace fs = std::filesystem;
    std::error_code       ec;
    std::vector<fs::path> candidates;
#if XX_IS_WIN_D
    wchar_t buf[MAX_PATH];
    if (::GetModuleFileNameW(nullptr, buf, MAX_PATH) > 0) {
        candidates.push_back(fs::path(buf).parent_path() / "plugins" / name);
    }
#else
    if (auto p = fs::read_symlink("/proc/self/exe", ec); !ec) {
        candidates.push_back(p.parent_path() / "plugins" / name);
    }
#endif
    candidates.push_back(fs::current_path(ec) / "plugins" / name);
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
    return "plugins/" + name;
}

static std::string findExamplePluginPath() {
    return findPluginPath("example_plugin");
}

/// Mock UI 适配器: 记录信号 (线程安全)
class MockPluginUiAdapter : public agentxx::plugin::PluginUiAdapter {
public:

    agentxx::plugin::InterfaceSet supportedInterfaces() const override {
        namespace pi = agentxx::plugin::plugin_interfaces;
        return {
            std::string{pi::ClientStatusItem},
            std::string{pi::ClientPanel},
            std::string{pi::ClientToast},
            std::string{pi::ClientInfoSection},
            std::string{pi::ClientCommand},
            std::string{pi::ClientMsgDecor},
            std::string{pi::ClientAction},
            std::string{pi::ClientOverlay}
        };
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

    bool
        sendPluginData(const std::string& plugin, const std::string& event, const std::string& json)
            override {
        std::lock_guard<std::mutex> lock(m_);
        ++dataUpCount_;
        lastDataPlugin_ = plugin;
        lastDataEvent_  = event;
        lastDataJson_   = json;
        return true;
    }

    void onOverlayOpen(
        const std::string& plugin,
        int                type,
        const std::string& title,
        const std::string& payload,
        const std::string& extraJson
    ) override {
        std::lock_guard<std::mutex> lock(m_);
        ++overlayOpenCount_;
        lastOverlayPlugin_  = plugin;
        lastOverlayType_    = type;
        lastOverlayTitle_   = title;
        lastOverlayPayload_ = payload;
        lastOverlayExtra_   = extraJson;
    }

    void onOverlayClose(const std::string& plugin) override {
        std::lock_guard<std::mutex> lock(m_);
        ++overlayCloseCount_;
        lastOverlayClosePlugin_ = plugin;
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

    int overlayOpenCount() const {
        std::lock_guard<std::mutex> lock(m_);
        return overlayOpenCount_;
    }

    int overlayCloseCount() const {
        std::lock_guard<std::mutex> lock(m_);
        return overlayCloseCount_;
    }

    int lastOverlayType() const {
        std::lock_guard<std::mutex> lock(m_);
        return lastOverlayType_;
    }

    std::string lastOverlayPlugin() const {
        std::lock_guard<std::mutex> lock(m_);
        return lastOverlayPlugin_;
    }

    std::string lastOverlayTitle() const {
        std::lock_guard<std::mutex> lock(m_);
        return lastOverlayTitle_;
    }

    std::string lastOverlayPayload() const {
        std::lock_guard<std::mutex> lock(m_);
        return lastOverlayPayload_;
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
    int                statusRegistered_      = 0;
    int                statusUpdated_         = 0;
    int                statusRemoved_         = 0;
    int                panelRegistered_       = 0;
    int                panelUpdated_          = 0;
    int                panelRemoved_          = 0;
    int                infoSectionRegistered_ = 0;
    int                infoSectionUpdated_    = 0;
    int                infoSectionRemoved_    = 0;
    int                toastCount_            = 0;
    int                sendCount_             = 0;
    int                dataUpCount_           = 0;
    int                overlayOpenCount_      = 0;
    int                overlayCloseCount_     = 0;
    int                lastToastLvl_          = 0;
    int                lastOverlayType_       = -1;
    std::string        lastStatusId_;
    std::string        lastPanelId_;
    std::string        lastInfoSectionId_;
    std::string        lastToast_;
    std::string        lastSent_;
    std::string        lastDataPlugin_;
    std::string        lastDataEvent_;
    std::string        lastDataJson_;
    std::string        lastOverlayPlugin_;
    std::string        lastOverlayTitle_;
    std::string        lastOverlayPayload_;
    std::string        lastOverlayExtra_;
    std::string        lastOverlayClosePlugin_;
};

asio::awaitable<TestResult> run_client_plugin_tests() {
    g_client_plugin_passed = 0;
    g_client_plugin_failed = 0;

    auto ex      = co_await asio::this_coro::executor;
    auto adapter = std::make_shared<MockPluginUiAdapter>();
    auto mgr     = std::make_shared<agentxx::plugin::ClientPluginManager>(ex);
    mgr->setUiAdapter(adapter);
    mgr->setSessionId("sess-test");

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

    // ---- 2.5 接口表: agentxx.client.self get_language / set_language ----
    {
        auto ifaceSelf = agentxx::plugin::queryInterface<AgentxxClientSelfIface>(
            &inst->host,
            AGENTXX_IFACE_CLIENT_SELF
        );
        XX_TEST_EXPECT_TRUE(ifaceSelf != nullptr);
        if (ifaceSelf) {
            XX_TEST_EXPECT_EQ(ifaceSelf->version, AGENTXX_IFACE_CLIENT_SELF_VERSION);
            // 默认语言为 "en"
            AgentxxPluginString langOut{};
            XX_TEST_EXPECT_EQ(ifaceSelf->get_language(&inst->host, &langOut), 0);
            XX_TEST_EXPECT_TRUE(langOut.data != nullptr);
            if (langOut.data) {
                XX_TEST_EXPECT_EQ(std::string(langOut.data, langOut.size), std::string("en"));
            }
            agentxx::plugin::PluginString::free(&inst->host, &langOut);

            // 设置语言为 "zh-cn"
            AgentxxPluginStringView zhSv{"zh-cn", 5};
            XX_TEST_EXPECT_EQ(ifaceSelf->set_language(&inst->host, &zhSv), 0);
            XX_TEST_EXPECT_EQ(ifaceSelf->get_language(&inst->host, &langOut), 0);
            XX_TEST_EXPECT_TRUE(langOut.data != nullptr);
            if (langOut.data) {
                XX_TEST_EXPECT_EQ(std::string(langOut.data, langOut.size), std::string("zh-cn"));
            }
            agentxx::plugin::PluginString::free(&inst->host, &langOut);

            // 不支持 auto, 传 auto 回退为 en
            AgentxxPluginStringView autoSv{"auto", 4};
            XX_TEST_EXPECT_EQ(ifaceSelf->set_language(&inst->host, &autoSv), 0);
            XX_TEST_EXPECT_EQ(ifaceSelf->get_language(&inst->host, &langOut), 0);
            XX_TEST_EXPECT_TRUE(langOut.data != nullptr);
            if (langOut.data) {
                XX_TEST_EXPECT_EQ(std::string(langOut.data, langOut.size), std::string("en"));
            }
            agentxx::plugin::PluginString::free(&inst->host, &langOut);
        }
    }
    XX_TEST_EXPECT_FALSE(mgr->hasCommand("no_such_command"));

    // 插件 loadNativeAsync 在 io 线程, entry 的注册经 ioCallSync 同步完成:
    // 适配器应已收到注册信号
    XX_TEST_EXPECT_EQ(adapter->statusRegistered(), 1);
    XX_TEST_EXPECT_EQ(adapter->panelRegistered(), 1);
    XX_TEST_EXPECT_EQ(adapter->infoSectionRegistered(), 1);

    // ---- 3. 事件分发 ----
    // READY: 插件回调更新状态栏 + 跨端 send_plugin_data("hello");
    //        插件事件分发完成后宿主上报 client_interfaces (三期6, 镜像
    //        server_plugins —— agent 侧插件据此感知 client 接口集)
    mgr->onReady();
    XX_TEST_EXPECT_TRUE(adapter->dataUpCount() >= 2);
    // 宿主接口集上报最后发出 (插件 READY 处理器先行)
    XX_TEST_EXPECT_EQ(adapter->lastDataEvent(), "client_interfaces");
    XX_TEST_EXPECT_EQ(adapter->lastDataPlugin(), "agentxx_host");

    // TURN_END: 状态栏项文本更新 (turns: 1)
    {
        agentxx::agent::WireTurnResult r;
        r.sessionId = "sess-test";
        r.hasError  = false;
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
            // Info 段落: TURN_END 时插件更新内容 (text "Turns: 1" + hint 说明)
            bool infoFound = false;
            for (const auto& s : reg->infoSections) {
                if (s.id == "example_plugin.info" && s.items.is_array() && !s.items.empty()) {
                    infoFound        = true;
                    const auto first = s.items[0];
                    XX_TEST_EXPECT_EQ(first.value("kind", std::string{}), "text");
                    XX_TEST_EXPECT_EQ(first.value("text", std::string{}), "Turns: 1");
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

    // ---- 5. 跨端数据 (agentxx.client.wire 接口表 send_plugin_data 路径) ----
    {
        const auto wire = agentxx::plugin::ClientIfaces::query(&inst->host).wire;
        XX_TEST_EXPECT_TRUE(wire != nullptr && wire->send_plugin_data != nullptr);
        auto evtSv = agentxx::plugin::PluginStringView::fromCstr("rebuild");
        auto paySv = agentxx::plugin::PluginStringView::fromCstr(R"({"x":1})");
        int  rc    = wire ? wire->send_plugin_data(&inst->host, &evtSv, &paySv) : -1;
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
        auto reg    = mgr->uiRegistrySnapshot();
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

    // ---- 8. A1/B8 回归: 订阅生命周期安全 ----
    // - 多次订阅触发订阅 vector 扩容, 逐个退订不得悬垂/错位
    // - 派发中动态订阅 (handler 内再订阅) 不得使 dispatch 快照中的后续
    //   回调悬垂 (旧实现 subscriptions 按值存储 + 裸指针 → UAF)
    // - unload 回调内主动退订安全 (detachAll 已断链)
    {
        auto inst2 = co_await mgr->loadNativeAsync(path);
        XX_TEST_EXPECT_TRUE(inst2 != nullptr);
        if (!inst2) {
            co_return TestResult{g_client_plugin_passed, g_client_plugin_failed};
        }

        // 8.1 多次订阅 (1→2→4 扩容) + 逐个退订
        std::atomic<int>           hits{0};
        AgentxxPluginSubscription* subs[4] = {};
        auto                       subFn   = +[](const AgentxxPluginStringView*, void* ud) {
            ++(*static_cast<std::atomic<int>*>(ud));
        };
        const auto events8 = agentxx::plugin::ClientIfaces::query(&inst2->host).events;
        XX_TEST_EXPECT_TRUE(events8 != nullptr && events8->subscribe != nullptr);
        for (int i = 0; i < 4; ++i) {
            subs[i]
                = events8
                      ? events8
                            ->subscribe(&inst2->host, AGENTXX_CLIENT_EVT_CONN_STATE, subFn, &hits)
                      : nullptr;
            XX_TEST_EXPECT_TRUE(subs[i] != nullptr);
        }
        for (int i = 0; i < 4; ++i) {
            if (events8) {
                events8->unsubscribe(subs[i]);
            }
        }
        mgr->onConnStateChanged("connected", "100%");
        XX_TEST_EXPECT_EQ(hits.load(), 0); // 全部退订后事件不再达

        // 8.2 派发中动态订阅: 订阅回调内再 subscribe → 快照不受影响
        // (旧实现 dispatch 快照存裸指针, 回调内订阅触发 vector 扩容后悬垂)
        struct DynSubState {
            agentxx::plugin::ClientPluginInstance* inst   = nullptr;
            const AgentxxClientEventsIface*        events = nullptr;
            std::atomic<int>                       hits{0};
            AgentxxPluginSubscription*             dynSub        = nullptr;
            void (*incFn)(const AgentxxPluginStringView*, void*) = nullptr;
        };

        auto st    = std::make_shared<DynSubState>();
        st->inst   = inst2.get();
        st->events = agentxx::plugin::ClientIfaces::query(&inst2->host).events;
        st->incFn  = +[](const AgentxxPluginStringView*, void* ud) {
            ++(*static_cast<std::atomic<int>*>(ud));
        };
        auto aFn = +[](const AgentxxPluginStringView*, void* ud) {
            auto* s = static_cast<DynSubState*>(ud);
            ++s->hits;
            if (!s->dynSub) {
                s->dynSub = s->events->subscribe(
                    &s->inst->host,
                    AGENTXX_CLIENT_EVT_USER_INPUT,
                    s->incFn,
                    &s->hits
                );
            }
        };
        AgentxxPluginSubscription* a
            = st->events
                  ? st->events
                        ->subscribe(&inst2->host, AGENTXX_CLIENT_EVT_USER_INPUT, aFn, st.get())
                  : nullptr;
        XX_TEST_EXPECT_TRUE(a != nullptr);
        mgr->onUserInput("sess-test", "x");
        // 首次派发: 仅快照中的 a 被调 (dynSub 派发后才注册)
        XX_TEST_EXPECT_EQ(st->hits.load(), 1);
        mgr->onUserInput("sess-test", "y");
        // 第二次派发: a + dynSub 都被调
        XX_TEST_EXPECT_EQ(st->hits.load(), 3);
        if (st->events) {
            st->events->unsubscribe(a);
        }
        if (st->dynSub && st->events) {
            st->events->unsubscribe(st->dynSub);
        }

        // 8.3 收尾: 卸载 (unload 回调内 vtable 反注册路径已由段 7 覆盖)
        bool unloaded2 = co_await mgr->unloadAsync("example_plugin");
        XX_TEST_EXPECT_TRUE(unloaded2);
        XX_TEST_EXPECT_TRUE(mgr->find("example_plugin") == nullptr);
    }

    // ---- 9. loadConfiguredClientPlugins: sides 过滤 + args 传递 (client 侧对称) ----
    // - sides=Agent 的配置项在 client 侧跳过 (属于 agent 侧)
    // - sides=Auto: 加载 + args 随配置直接传入实例 (get_plugin_args 可读)
    // - Auto 无 client 入口的纯 agent 插件静默跳过 (允许缺失, 不报错)
    {
        std::vector<agentxx::agent::PluginConfig> cfgs;
        agentxx::agent::PluginConfig              pc;
        pc.path    = path;
        pc.enabled = true;
        pc.args    = neograph::json{
               {"client_key", "client_val"}
        };

        // 9.1 sides=Agent: client 侧跳过
        pc.sides = agentxx::agent::PluginSide::Agent;
        cfgs.push_back(pc);
        co_await mgr->loadConfiguredClientPlugins(cfgs);
        XX_TEST_EXPECT_TRUE(mgr->find("example_plugin") == nullptr);

        // 9.2 sides=Auto: 加载 + args 传递
        pc.sides = agentxx::agent::PluginSide::Auto;
        cfgs.clear();
        cfgs.push_back(pc);
        co_await mgr->loadConfiguredClientPlugins(cfgs);
        auto instCfg = mgr->find("example_plugin");
        XX_TEST_EXPECT_TRUE(instCfg != nullptr);
        if (instCfg) {
            XX_TEST_EXPECT_EQ(instCfg->args.value("client_key", std::string{}), "client_val");
            // agentxx.client.self 接口表 get_plugin_args 返回实例 args
            const auto          self9 = agentxx::plugin::ClientIfaces::query(&instCfg->host).self;
            AgentxxPluginString json{nullptr, 0};
            if (self9 && self9->get_plugin_args) {
                self9->get_plugin_args(&instCfg->host, &json);
            }
            XX_TEST_EXPECT_TRUE(json.data != nullptr);
            if (json.data) {
                try {
                    auto j = neograph::json::parse(std::string{json.data, json.size});
                    XX_TEST_EXPECT_EQ(j["client_key"].get<std::string>(), "client_val");
                } catch (const std::exception& e) {
                    XX_TEST_EXPECT_TRUE(false);
                    XX_LOGE("[client_plugin] 9.2 args json parse failed: {}", e.what());
                }
                agentxx::plugin::PluginString::free(&instCfg->host, &json);
            }
            bool unloadedCfg = co_await mgr->unloadAsync("example_plugin");
            XX_TEST_EXPECT_TRUE(unloadedCfg);
            XX_TEST_EXPECT_TRUE(mgr->find("example_plugin") == nullptr);
        }

        // 9.3 sides=Client: 加载 (显式要求 client 入口, 缺失时须报错)
        pc.sides = agentxx::agent::PluginSide::Client;
        cfgs.clear();
        cfgs.push_back(pc);
        co_await mgr->loadConfiguredClientPlugins(cfgs);
        auto instCfg2 = mgr->find("example_plugin");
        XX_TEST_EXPECT_TRUE(instCfg2 != nullptr);
        if (instCfg2) {
            co_await mgr->unloadAsync("example_plugin");
        }
    }

    // ---- 10. 宿主约定事件 (server_plugins) + 对端缺失可观测性 ----
    // - server_plugins (结构化载荷) → 记录服务端已加载插件列表, get_client_state
    //   以 "agentPlugins" 暴露 [{name,version,interfaces},...] (client 插件
    //   判断对端可用性/能力的正式通道)
    // - PLUGIN_DATA 无任何 client 订阅者: 不崩溃 (每插件名一次警告, 无法
    //   断言日志, 仅验证路径安全)
    {
        agentxx::agent::WirePluginData d;
        d.plugin = "agentxx_host";
        d.event  = "server_plugins";
        d.data
            = R"({"plugins":[)"
              R"({"name":"agentxx_codegraph","version":"1.0.0","interfaces":["agentxx.agent.core"]},)"
              R"({"name":"agentxx_system_monitor","version":"1.0.0",)"
              R"("interfaces":["agentxx.agent.core"]}]})";
        mgr->onPluginData(d);
        auto stateJson = mgr->clientStateJson();
        XX_TEST_EXPECT_TRUE(stateJson.find("agentPlugins") != std::string::npos);
        XX_TEST_EXPECT_TRUE(stateJson.find("\"interfaces\"") != std::string::npos);
        XX_TEST_EXPECT_TRUE(stateJson.find("agentxx_codegraph") != std::string::npos);
        XX_TEST_EXPECT_TRUE(stateJson.find("agentxx_system_monitor") != std::string::npos);

        // 非法载荷: 保留旧值, 不崩溃
        agentxx::agent::WirePluginData bad;
        bad.plugin = "agentxx_host";
        bad.event  = "server_plugins";
        bad.data   = "not-json";
        mgr->onPluginData(bad);
        auto stateJson2 = mgr->clientStateJson();
        XX_TEST_EXPECT_TRUE(stateJson2.find("agentxx_codegraph") != std::string::npos);

        // 无订阅者路径 (当前无任何已加载插件): 安全
        agentxx::agent::WirePluginData orphan;
        orphan.plugin = "some_plugin";
        orphan.event  = "progress";
        orphan.data   = R"({})";
        mgr->onPluginData(orphan);
    }

    // ---- 11. 接口协商: 宿主支持集 / require 门禁 / 发现通道 ----
    // - 门禁双道生效: dlopen 前跳过 (loadConfiguredClientPlugins) 与
    //   dlopen 后直连路径 (loadNativeAsync); 此处覆盖后者 + 发现通道
    {
        namespace fs = std::filesystem;

        // 11.1 clientStateJson 暴露宿主接口清单 (Mock 适配器: status_item/
        //      panel/toast/info_section/command)
        auto stateJson = mgr->clientStateJson();
        XX_TEST_EXPECT_TRUE(stateJson.find("\"interfaces\"") != std::string::npos);
        XX_TEST_EXPECT_TRUE(stateJson.find("agentxx.client.panel") != std::string::npos);
        XX_TEST_EXPECT_TRUE(stateJson.find("agentxx.client.command") != std::string::npos);
        XX_TEST_EXPECT_TRUE(stateJson.find("agentxx.client.status_item") != std::string::npos);
        // 未置位的能力不得出现 (keybind 预留位未置)
        XX_TEST_EXPECT_FALSE(stateJson.find("agentxx.client.keybind") != std::string::npos);

        // 11.2 require 未满足 → 加载跳过并记录原因 (直连路径, dlopen 后门禁):
        // 拷贝真实可加载的示例库, manifest 声明本宿主不支持的必选接口
        auto gateDir
            = fs::temp_directory_path()
              / ("agentxx_iface_gate_"
                 + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::error_code ec;
        fs::create_directories(gateDir, ec);
        bool copied = false;
        for (fs::directory_iterator it(findExamplePluginPath(), ec), end; it != end;
             it.increment(ec)) {
            auto ext = it->path().extension().string();
            if (ext == ".so" || ext == ".dll" || ext == ".dylib") {
                fs::copy_file(
                    it->path(),
                    gateDir / it->path().filename(),
                    fs::copy_options::overwrite_existing,
                    ec
                );
                copied = !ec;
                break;
            }
        }
        XX_TEST_EXPECT_TRUE(copied);
        if (copied) {
            {
                std::ofstream f(gateDir / "plugin.yaml", std::ios::binary | std::ios::trunc);
                f << "name: gate_missing_iface\nentry: libexample_plugin.so\ndepends:\n"
                     "interfaces:\n  require:\n    - agentxx.client.panel\n    - vendor.nonexistent\n";
            }
            size_t skippedBefore = mgr->skippedPlugins().size();
            auto   gated         = co_await mgr->loadNativeAsync(gateDir.string());
            XX_TEST_EXPECT_TRUE(gated == nullptr); // 未注册进插件表
            // 实例名以 get_info 为准 ("example_plugin"; 清单名仅用于依赖/
            // 排序 —— 既有行为), 跳过记录用同一名字
            XX_TEST_EXPECT_TRUE(mgr->find("example_plugin") == nullptr);
            // 跳过原因已记录 (含缺失接口名)
            XX_TEST_EXPECT_TRUE(mgr->skippedPlugins().size() == skippedBefore + 1);
            if (mgr->skippedPlugins().contains("example_plugin")) {
                XX_TEST_EXPECT_TRUE(
                    mgr->skippedPlugins().at("example_plugin").find("vendor.nonexistent")
                    != std::string::npos
                );
            }

            // 11.3 同一插件声明全部可满足 → 正常加载 (门禁放行回归) +
            //      READY payload 携带接口清单
            {
                std::ofstream f(gateDir / "plugin.yaml", std::ios::binary | std::ios::trunc);
                f << "name: gate_ok_iface\nentry: libexample_plugin.so\ndepends:\n"
                     "interfaces:\n  require:\n    - agentxx.client.panel\n    - agentxx.client.command\n"
                     "  optional:\n    - agentxx.client.toast\n";
            }
            auto okInst = co_await mgr->loadNativeAsync(gateDir.string());
            XX_TEST_EXPECT_TRUE(okInst != nullptr);
            if (okInst) {
                // 声明已随加载保存 (list() 可查)
                bool foundDecl = false;
                for (const auto& v : mgr->list()) {
                    if (v.name == "example_plugin") {
                        foundDecl = true;
                        XX_TEST_EXPECT_TRUE(v.requiredInterfaces.size() == 2);
                        XX_TEST_EXPECT_TRUE(v.optionalInterfaces.size() == 1);
                    }
                }
                XX_TEST_EXPECT_TRUE(foundDecl);

                // READY payload 含 interfaces 数组 (经 agentxx.client.events 接口表订阅;
                // onReady 同步分发到当前 io 线程)
                std::string readyPayload;
                auto        readyFn = +[](const AgentxxPluginStringView* payload, void* ud) {
                    if (payload && payload->data) {
                        static_cast<std::string*>(ud)->assign(payload->data, payload->size);
                    }
                };
                const auto events11 = agentxx::plugin::ClientIfaces::query(&okInst->host).events;
                auto       sub      = events11 ? events11->subscribe(
                                          &okInst->host,
                                          AGENTXX_CLIENT_EVT_READY,
                                          readyFn,
                                          &readyPayload
                                      )
                                               : nullptr;
                XX_TEST_EXPECT_TRUE(sub != nullptr);
                mgr->onReady();
                XX_TEST_EXPECT_TRUE(readyPayload.find("\"interfaces\"") != std::string::npos);
                XX_TEST_EXPECT_TRUE(readyPayload.find("agentxx.client.panel") != std::string::npos);
                if (events11) {
                    events11->unsubscribe(sub);
                }

                co_await mgr->unloadAsync("example_plugin");
                XX_TEST_EXPECT_TRUE(mgr->find("example_plugin") == nullptr);
            }
        }
        fs::remove_all(gateDir, ec);

        // 11.4 加载后 skippedPlugins() 不回退 (仅记录, 不影响后续加载决策)
        XX_TEST_EXPECT_TRUE(!mgr->skippedPlugins().empty());
    }

    // ---- 12. agentxx_codegraph client 插件: 索引状态与进度 Info 栏渲染 ----
    {
        auto cgPath = findPluginPath("agentxx_codegraph");
        auto cgInst = co_await mgr->loadNativeAsync(cgPath);
        XX_TEST_EXPECT_TRUE(cgInst != nullptr);
        if (cgInst) {
            auto reg          = mgr->uiRegistrySnapshot();
            bool hasCgSection = false;
            for (const auto& sec : reg->infoSections) {
                if (sec.id == "agentxx_codegraph.status") {
                    hasCgSection = true;
                    XX_TEST_EXPECT_EQ(sec.title, std::string{"CodeGraph"});
                }
            }
            XX_TEST_EXPECT_TRUE(hasCgSection);

            // status 事件: 新版 Append 段风格仅显示 “|- wait for index” (已加载但无进度时)
            agentxx::agent::WirePluginData st;
            st.plugin = "agentxx_codegraph";
            st.event  = "status";
            st.data   = R"({"loaded":true,"project_root":"/workspace/test_proj"})";
            mgr->onPluginData(st);

            reg = mgr->uiRegistrySnapshot();
            for (const auto& sec : reg->infoSections) {
                if (sec.id == "agentxx_codegraph.status") {
                    std::string dump = sec.items.dump();
                    // 当前实现: 无进度时展示 wait for index (小写+, hint)
                    XX_TEST_EXPECT_TRUE(dump.find("wait for index") != std::string::npos);
                }
            }

            // progress 事件 (进行中): 新版为 “|- indexing 25% (25/100)” + progress 条 + 文件名
            agentxx::agent::WirePluginData prog;
            prog.plugin = "agentxx_codegraph";
            prog.event  = "progress";
            prog.data   = R"({"processed":25,"total":100,"current_file":"src/main.cpp"})";
            mgr->onPluginData(prog);

            reg = mgr->uiRegistrySnapshot();
            for (const auto& sec : reg->infoSections) {
                if (sec.id == "agentxx_codegraph.status") {
                    std::string dump = sec.items.dump();
                    // 25/100 -> 25% ( {:.0f}% 保留整数, 25%)
                    XX_TEST_EXPECT_TRUE(dump.find("25/100") != std::string::npos);
                    XX_TEST_EXPECT_TRUE(
                        dump.find("indexing") != std::string::npos
                        || dump.find("Indexing") != std::string::npos
                    );
                    // 文件名仅保留 basename "main.cpp"
                    XX_TEST_EXPECT_TRUE(dump.find("main.cpp") != std::string::npos);
                    XX_TEST_EXPECT_TRUE(dump.find("progress") != std::string::npos);
                }
            }

            // progress 事件 (完成): 新版为 “|- available · 100”
            agentxx::agent::WirePluginData progDone;
            progDone.plugin = "agentxx_codegraph";
            progDone.event  = "progress";
            progDone.data   = R"({"processed":100,"total":100,"current_file":""})";
            mgr->onPluginData(progDone);

            reg = mgr->uiRegistrySnapshot();
            for (const auto& sec : reg->infoSections) {
                if (sec.id == "agentxx_codegraph.status") {
                    std::string dump = sec.items.dump();
                    XX_TEST_EXPECT_TRUE(dump.find("available") != std::string::npos);
                    XX_TEST_EXPECT_TRUE(dump.find("100") != std::string::npos);
                }
            }

            co_await mgr->unloadAsync("agentxx_codegraph");
            XX_TEST_EXPECT_TRUE(mgr->find("agentxx_codegraph") == nullptr);
        }
    }

    // ---- 13. agentxx_system_monitor client 插件: CPU/内存/GPU 资源监控渲染 ----
    {
        auto smPath = findPluginPath("agentxx_system_monitor");
        auto smInst = co_await mgr->loadNativeAsync(smPath);
        XX_TEST_EXPECT_TRUE(smInst != nullptr);
        if (smInst) {
            auto reg          = mgr->uiRegistrySnapshot();
            bool hasSmSection = false;
            for (const auto& sec : reg->infoSections) {
                if (sec.id == "agentxx_system_monitor.usage") {
                    hasSmSection = true;
                    XX_TEST_EXPECT_EQ(sec.title, std::string{"System"});
                }
            }
            XX_TEST_EXPECT_TRUE(hasSmSection);

            // usage 事件: 新版 Append 段风格为 “|- CPU 36%” / “|- RAM 52% (8G/16G)” / “|- GPU 42%”
            agentxx::agent::WirePluginData usage;
            usage.plugin = "agentxx_system_monitor";
            usage.event  = "usage";
            usage.data
                = R"({"cpu":35.5,"mem_percent":52.0,"mem_used_mb":8192,"mem_total_mb":16384,"gpus":[{"name":"NVIDIA RTX 4090","dedicated_vram_mb":24576,"dedicated_vram_used_mb":6144,"usage_percent":42.0}]})";
            mgr->onPluginData(usage);

            reg = mgr->uiRegistrySnapshot();
            for (const auto& sec : reg->infoSections) {
                if (sec.id == "agentxx_system_monitor.usage") {
                    std::string dump = sec.items.dump();
                    // CPU 四舍五入到整数 (35.5 -> 36)
                    XX_TEST_EXPECT_TRUE(dump.find("CPU") != std::string::npos);
                    XX_TEST_EXPECT_TRUE(
                        dump.find("36%") != std::string::npos
                        || dump.find("35.5%") != std::string::npos
                    );
                    // RAM 带格式化大小 (8192 MB -> 8G, 16384 -> 16G)
                    XX_TEST_EXPECT_TRUE(dump.find("RAM") != std::string::npos);
                    XX_TEST_EXPECT_TRUE(dump.find("52%") != std::string::npos);
                    XX_TEST_EXPECT_TRUE(dump.find("8G") != std::string::npos);
                    XX_TEST_EXPECT_TRUE(dump.find("16G") != std::string::npos);
                    // GPU 仅显示峰值百分比, 不含名称及 MB
                    XX_TEST_EXPECT_TRUE(dump.find("GPU") != std::string::npos);
                    XX_TEST_EXPECT_TRUE(dump.find("42%") != std::string::npos);
                }
            }

            // sysinfo 命令执行: 新版切换文案为 “System resource info: ON/OFF”
            XX_TEST_EXPECT_TRUE(mgr->hasCommand("sysinfo"));
            auto toastBefore = adapter->toastCount();
            mgr->invokeCommand("sysinfo", R"({})");
            XX_TEST_EXPECT_TRUE(adapter->toastCount() > toastBefore);
            XX_TEST_EXPECT_TRUE(
                adapter->lastToast().find("System resource info") != std::string::npos
            );

            co_await mgr->unloadAsync("agentxx_system_monitor");
            XX_TEST_EXPECT_TRUE(mgr->find("agentxx_system_monitor") == nullptr);
        }
    }

    // ---- 14. agentxx_planning client 插件: 工具消息装饰与侧边栏概览 ----
    {
        auto plPath = findPluginPath("agentxx_planning");
        auto plInst = co_await mgr->loadNativeAsync(plPath);
        XX_TEST_EXPECT_TRUE(plInst != nullptr);
        if (plInst) {
            // 14.1 规划事件推送 -> Info 栏段落更新
            agentxx::agent::WirePluginData plData;
            plData.plugin = "agentxx_planning";
            plData.event  = "planning";
            plData.data
                = R"({"roadmap":"stateDiagram-v2\n[*] --> step1\nstep1 --> [*]","todos":[{"state":"in_progress","content":"do step 1","summary":"doing step 1"},{"state":"completed","content":"done step 0"}],"notes":"test note 123"})";
            mgr->onPluginData(plData);

            auto reg            = mgr->uiRegistrySnapshot();
            bool hasPlanSection = false;
            for (const auto& sec : reg->infoSections) {
                if (sec.id == "agentxx_planning.plan") {
                    hasPlanSection = true;
                    XX_TEST_EXPECT_EQ(sec.title, std::string{"Plan"});
                    std::string dump = sec.items.dump();
                    XX_TEST_EXPECT_TRUE(dump.find("Graph") != std::string::npos);
                    XX_TEST_EXPECT_TRUE(dump.find("[~] do step 1") != std::string::npos);
                    XX_TEST_EXPECT_TRUE(dump.find("[#] done step 0") != std::string::npos);
                    XX_TEST_EXPECT_TRUE(dump.find("test note 123") != std::string::npos);
                    // 验证 items 中的 |- 前缀与 Graph 按钮布局契约
                    // (双发: 新 action_id + 老 mermaid 兼容字段)
                    XX_TEST_EXPECT_TRUE(sec.items.is_array());
                    XX_TEST_EXPECT_TRUE(sec.items.size() >= 2);
                    XX_TEST_EXPECT_EQ(sec.items[0].value("kind", ""), std::string{"text"});
                    XX_TEST_EXPECT_EQ(sec.items[0].value("text", ""), std::string{"|- "});
                    XX_TEST_EXPECT_EQ(sec.items[1].value("kind", ""), std::string{"button"});
                    XX_TEST_EXPECT_EQ(sec.items[1].value("label", ""), std::string{"[Graph]"});
                    XX_TEST_EXPECT_EQ(
                        sec.items[1].value("action_id", ""),
                        std::string{"planning.open_graph"}
                    );
                    XX_TEST_EXPECT_TRUE(sec.items[1].contains("mermaid"));
                }
            }
            XX_TEST_EXPECT_TRUE(hasPlanSection);

            // 14.2 工具调用 WireDelta: tool_start (write 模式) -> 工具装饰推送
            agentxx::agent::WireDelta deltaWriteStart{
                .type       = agentxx::agent::WireDelta::Type::ToolStart,
                .toolName   = "agentxx_planning",
                .toolCallId = "call_plan_write_1",
                .arguments
                = R"({"mode":"write","roadmap":"stateDiagram-v2\n[*] --> p1\np1 --> [*]","todos":[{"state":"in_progress","content":"write task"}]})",
            };
            mgr->onDelta(deltaWriteStart);

            reg             = mgr->uiRegistrySnapshot();
            bool foundDecor = false;
            for (const auto& d : reg->toolDecors) {
                if (d.toolCallId == "call_plan_write_1") {
                    foundDecor = true;
                    XX_TEST_EXPECT_EQ(d.displayName, std::string{"Plan"});
                    XX_TEST_EXPECT_TRUE(d.summary.find("[~] write task") != std::string::npos);
                    std::string dump = d.items.dump();
                    XX_TEST_EXPECT_TRUE(dump.find("diagram") != std::string::npos);
                    XX_TEST_EXPECT_TRUE(dump.find("write task") != std::string::npos);
                }
            }
            XX_TEST_EXPECT_TRUE(foundDecor);

            // 14.3 工具调用 WireDelta: tool_start (read 模式) -> 占位装饰
            agentxx::agent::WireDelta deltaReadStart{
                .type       = agentxx::agent::WireDelta::Type::ToolStart,
                .toolName   = "agentxx_planning",
                .toolCallId = "call_plan_read_1",
                .arguments  = R"({"mode":"read"})",
            };
            mgr->onDelta(deltaReadStart);

            reg = mgr->uiRegistrySnapshot();
            for (const auto& d : reg->toolDecors) {
                if (d.toolCallId == "call_plan_read_1") {
                    std::string dump = d.items.dump();
                    XX_TEST_EXPECT_TRUE(
                        dump.find("Reading saved planning...") != std::string::npos
                    );
                }
            }

            // tool_end (read 模式) -> 结果装饰刷新
            agentxx::agent::WireDelta deltaReadEnd{
                .type       = agentxx::agent::WireDelta::Type::ToolEnd,
                .toolName   = "agentxx_planning",
                .toolCallId = "call_plan_read_1",
                .result
                = R"({"roadmap":"stateDiagram-v2\n[*] --> p2\np2 --> [*]","todos":[{"state":"completed","content":"read task done"}]})",
            };
            mgr->onDelta(deltaReadEnd);

            reg = mgr->uiRegistrySnapshot();
            for (const auto& d : reg->toolDecors) {
                if (d.toolCallId == "call_plan_read_1") {
                    std::string dump = d.items.dump();
                    XX_TEST_EXPECT_TRUE(dump.find("read task done") != std::string::npos);
                }
            }

            // 14.4 会话切换 -> 清除装饰与段落
            mgr->onSessionSwitched("new_session_123");
            reg                        = mgr->uiRegistrySnapshot();
            bool hasPlanSecAfterSwitch = false;
            for (const auto& sec : reg->infoSections) {
                if (sec.id == "agentxx_planning.plan") {
                    hasPlanSecAfterSwitch = true;
                }
            }
            XX_TEST_EXPECT_FALSE(hasPlanSecAfterSwitch);
            bool hasDecorAfterSwitch = false;
            for (const auto& d : reg->toolDecors) {
                if (d.plugin == "agentxx_planning") {
                    hasDecorAfterSwitch = true;
                }
            }
            XX_TEST_EXPECT_FALSE(hasDecorAfterSwitch);

            co_await mgr->unloadAsync("agentxx_planning");
            XX_TEST_EXPECT_TRUE(mgr->find("agentxx_planning") == nullptr);
        }
    }

    // ---- 15. agentxx_filesystem client 插件: 工具特化渲染 (template + func + diff) ----
    {
        auto fsPath = findPluginPath("agentxx_filesystem");
        auto fsInst = co_await mgr->loadNativeAsync(fsPath);
        XX_TEST_EXPECT_TRUE(fsInst != nullptr);
        if (fsInst) {
            auto reg = mgr->uiRegistrySnapshot();
            XX_TEST_EXPECT_TRUE(reg != nullptr);

            // 15.1 list (预设模版)
            auto listRes = agentxx::plugin::renderClientTool(
                reg.get(),
                "call_list_1",
                "agentxx_filesystem_list",
                R"({"path":"/home/user"})",
                "",
                true,
                false,
                100
            );
            XX_TEST_EXPECT_TRUE(listRes.matched);
            XX_TEST_EXPECT_EQ(listRes.displayName, "List");
            XX_TEST_EXPECT_EQ(listRes.summary, "/home/user");

            // 15.2 read (回调函数: [offset, limit] 区间参数)
            auto readRes = agentxx::plugin::renderClientTool(
                reg.get(),
                "call_read_1",
                "agentxx_filesystem_read",
                R"({"path":"/home/user/a.cpp","line_offset":10,"line_limit":50})",
                "",
                true,
                false,
                100
            );
            XX_TEST_EXPECT_TRUE(readRes.matched);
            XX_TEST_EXPECT_EQ(readRes.displayName, "Read");
            XX_TEST_EXPECT_TRUE(readRes.summary.find("[10, 50]") != std::string::npos);
            XX_TEST_EXPECT_TRUE(readRes.summary.find("/home/user/a.cpp") != std::string::npos);

            // 15.3 edit (回调函数: path 摘要 + diff items)
            auto editRes = agentxx::plugin::renderClientTool(
                reg.get(),
                "call_edit_1",
                "agentxx_filesystem_edit",
                R"({"path":"/home/user/b.cpp","old_str":"foo","new_str":"bar"})",
                "success",
                true,
                false,
                100
            );
            XX_TEST_EXPECT_TRUE(editRes.matched);
            XX_TEST_EXPECT_EQ(editRes.displayName, "Edit");
            XX_TEST_EXPECT_TRUE(editRes.summary.find("/home/user/b.cpp") != std::string::npos);
            XX_TEST_EXPECT_EQ(editRes.items.size(), 1U);
            if (!editRes.items.empty()) {
                XX_TEST_EXPECT_EQ(editRes.items[0].value("kind", std::string{}), "diff");
                XX_TEST_EXPECT_EQ(
                    editRes.items[0].value("path", std::string{}),
                    "/home/user/b.cpp"
                );
                XX_TEST_EXPECT_EQ(editRes.items[0].value("old_str", std::string{}), "foo");
                XX_TEST_EXPECT_EQ(editRes.items[0].value("new_str", std::string{}), "bar");
            }

            co_await mgr->unloadAsync("agentxx_filesystem");
            XX_TEST_EXPECT_TRUE(mgr->find("agentxx_filesystem") == nullptr);
        }
    }

    // ---- 15b. agentxx_filesystem client 插件: glob/grep 渲染兼容单字符串参数 ----
    // 真实会话中 LLM 常把 file_patterns/text_patterns/regex_patterns 下发为
    // 单个字符串而非数组; 渲染回调取不到数组时摘要退化为空, 折叠头丢失检索参数。
    // 此处用真实插件的注册回调覆盖: 数组 / 单字符串 / 混合 text+regex。
    {
        auto fsPath = findPluginPath("agentxx_filesystem");
        auto fsInst = co_await mgr->loadNativeAsync(fsPath);
        XX_TEST_EXPECT_TRUE(fsInst != nullptr);
        if (fsInst) {
            auto reg = mgr->uiRegistrySnapshot();
            XX_TEST_EXPECT_TRUE(reg != nullptr);

            // glob 数组形态 (旧行为, 回归保护)
            auto globArr = agentxx::plugin::renderClientTool(
                reg.get(),
                "call_glob_arr",
                "agentxx_filesystem_glob",
                R"({"file_patterns":["a","b","c","d"]})",
                "",
                true,
                false,
                100
            );
            XX_TEST_EXPECT_TRUE(globArr.matched);
            XX_TEST_EXPECT_EQ(globArr.displayName, "Glob");
            XX_TEST_EXPECT_TRUE(globArr.summary.find("a, b, ...") != std::string::npos);

            // glob 单字符串形态 (真实 LLM 下发形态)
            auto globStr = agentxx::plugin::renderClientTool(
                reg.get(),
                "call_glob_str",
                "agentxx_filesystem_glob",
                R"({"file_patterns":"agent/test/*.cpp"})",
                "",
                true,
                false,
                100
            );
            XX_TEST_EXPECT_TRUE(globStr.matched);
            XX_TEST_EXPECT_EQ(globStr.displayName, "Glob");
            XX_TEST_EXPECT_TRUE(
                globStr.summary.find("agent/test/*.cpp") != std::string::npos
            );

            // grep 数组形态 (旧行为, 回归保护)
            auto grepArr = agentxx::plugin::renderClientTool(
                reg.get(),
                "call_grep_arr",
                "agentxx_filesystem_grep",
                R"({"text_patterns":["foo"],"file_patterns":["src/**/*.h"]})",
                "",
                true,
                false,
                100
            );
            XX_TEST_EXPECT_TRUE(grepArr.matched);
            XX_TEST_EXPECT_EQ(grepArr.displayName, "Grep");
            XX_TEST_EXPECT_TRUE(grepArr.summary.find(R"("foo")") != std::string::npos);
            XX_TEST_EXPECT_TRUE(grepArr.summary.find("src/**/*.h") != std::string::npos);

            // grep 单字符串形态 (真实 LLM 下发形态: file/text 均为单字符串)
            auto grepStr = agentxx::plugin::renderClientTool(
                reg.get(),
                "call_grep_str",
                "agentxx_filesystem_grep",
                R"({"text_patterns":"startDaSimServer","file_patterns":"agent/test/core/test_agent.cpp"})",
                "",
                true,
                false,
                100
            );
            XX_TEST_EXPECT_TRUE(grepStr.matched);
            XX_TEST_EXPECT_EQ(grepStr.displayName, "Grep");
            XX_TEST_EXPECT_TRUE(
                grepStr.summary.find(R"("startDaSimServer")") != std::string::npos
            );
            XX_TEST_EXPECT_TRUE(
                grepStr.summary.find("agent/test/core/test_agent.cpp") != std::string::npos
            );

            // grep 混合形态: 单字符串 text + 数组 regex (摘要合并展示)
            auto grepMix = agentxx::plugin::renderClientTool(
                reg.get(),
                "call_grep_mix",
                "agentxx_filesystem_grep",
                R"({"text_patterns":"hello","regex_patterns":["world"],"file_patterns":["src/**/*.txt"]})",
                "",
                true,
                false,
                100
            );
            XX_TEST_EXPECT_TRUE(grepMix.matched);
            XX_TEST_EXPECT_TRUE(grepMix.summary.find(R"("hello")") != std::string::npos);
            XX_TEST_EXPECT_TRUE(grepMix.summary.find(R"("world")") != std::string::npos);
            XX_TEST_EXPECT_TRUE(grepMix.summary.find("src/**/*.txt") != std::string::npos);

            co_await mgr->unloadAsync("agentxx_filesystem");
            XX_TEST_EXPECT_TRUE(mgr->find("agentxx_filesystem") == nullptr);
        }
    }

    // ---- 16. agentxx_websearch client 插件: 工具特化渲染 (模版) ----
    {
        auto wsPath = findPluginPath("agentxx_websearch");
        auto wsInst = co_await mgr->loadNativeAsync(wsPath);
        XX_TEST_EXPECT_TRUE(wsInst != nullptr);
        if (wsInst) {
            auto reg = mgr->uiRegistrySnapshot();
            XX_TEST_EXPECT_TRUE(reg != nullptr);

            auto searchRes = agentxx::plugin::renderClientTool(
                reg.get(),
                "call_search_1",
                "agentxx_web_search",
                R"({"query":"c++26 features"})",
                "",
                true,
                false,
                100
            );
            XX_TEST_EXPECT_TRUE(searchRes.matched);
            XX_TEST_EXPECT_EQ(searchRes.displayName, "Search");
            XX_TEST_EXPECT_EQ(searchRes.summary, "c++26 features");

            co_await mgr->unloadAsync("agentxx_websearch");
            XX_TEST_EXPECT_TRUE(mgr->find("agentxx_websearch") == nullptr);
        }
    }

    // ---- 17. agentxx_execute_command client 插件: 工具特化渲染 (模版) ----
    {
        auto cmdPath = findPluginPath("agentxx_execute_command");
        auto cmdInst = co_await mgr->loadNativeAsync(cmdPath);
        XX_TEST_EXPECT_TRUE(cmdInst != nullptr);
        if (cmdInst) {
            auto reg = mgr->uiRegistrySnapshot();
            XX_TEST_EXPECT_TRUE(reg != nullptr);

            auto bashRes = agentxx::plugin::renderClientTool(
                reg.get(),
                "call_bash_1",
                "agentxx_execute_bash_command",
                R"({"command":"uname -a"})",
                "",
                true,
                false,
                100
            );
            XX_TEST_EXPECT_TRUE(bashRes.matched);
            XX_TEST_EXPECT_EQ(bashRes.displayName, "Bash");
            XX_TEST_EXPECT_EQ(bashRes.summary, "uname -a");

            co_await mgr->unloadAsync("agentxx_execute_command");
            XX_TEST_EXPECT_TRUE(mgr->find("agentxx_execute_command") == nullptr);
        }
    }

    // ---- 18. 通用交互: bind/unbind 覆盖语义 + 精确优先 vs fallback ----
    {
        std::atomic<int> hitsExact{0};
        std::atomic<int> hitsFallback{0};
        std::string      gotOwner, gotAction, gotArgs;
        auto             exactFn = +[](const AgentxxUiActionContext* ctx, void* ud) {
            ++(*static_cast<std::atomic<int>*>(ud));
            (void)ctx;
        };
        auto fallbackFn = +[](const AgentxxUiActionContext* ctx, void* ud) {
            ++(*static_cast<std::atomic<int>*>(ud));
            (void)ctx;
        };
        // 用 example_plugin 实例的 host 直调 vtable (io 线程同步)
        auto exPath = findExamplePluginPath();
        auto exInst = co_await mgr->loadNativeAsync(exPath);
        XX_TEST_EXPECT_TRUE(exInst != nullptr);
        if (exInst) {
            const auto ui = agentxx::plugin::ClientIfaces::query(&exInst->host).ui;
            XX_TEST_EXPECT_TRUE(ui != nullptr && ui->bind_action_handler != nullptr);
            XX_TEST_EXPECT_TRUE(ui != nullptr && ui->unbind_action_handler != nullptr);
            if (ui && ui->bind_action_handler && ui->unbind_action_handler) {
                // cb 空 → 失败
                auto emptySv = agentxx::plugin::PluginStringView::from("", 0);
                XX_TEST_EXPECT_TRUE(
                    ui->bind_action_handler(&exInst->host, &emptySv, nullptr, &hitsFallback)
                    != 0
                );
                // bind fallback ("") + 精确 ("sec1")
                auto secSv = agentxx::plugin::PluginStringView::fromCstr("sec1");
                XX_TEST_EXPECT_EQ(
                    ui->bind_action_handler(&exInst->host, &emptySv, fallbackFn, &hitsFallback),
                    0
                );
                XX_TEST_EXPECT_EQ(
                    ui->bind_action_handler(&exInst->host, &secSv, exactFn, &hitsExact),
                    0
                );
                // 快照可见 (UI 线程渲染依据)
                {
                    auto reg = mgr->uiRegistrySnapshot();
                    XX_TEST_EXPECT_TRUE(reg != nullptr);
                    if (reg) {
                        XX_TEST_EXPECT_TRUE(reg->actionBindings.size() >= 2);
                    }
                }
                // 精确优先: owner=sec1 → exact
                mgr->dispatchAction("example_plugin", "sec1", "a.do", "{}");
                XX_TEST_EXPECT_EQ(hitsExact.load(), 1);
                XX_TEST_EXPECT_EQ(hitsFallback.load(), 0);
                // 未命中精确 → 回落 "": owner=other → fallback
                mgr->dispatchAction("example_plugin", "other", "a.do", "{}");
                XX_TEST_EXPECT_EQ(hitsExact.load(), 1);
                XX_TEST_EXPECT_EQ(hitsFallback.load(), 1);
                // 覆盖语义: 同 (plugin,target) 重绑覆盖
                std::atomic<int> hitsExact2{0};
                XX_TEST_EXPECT_EQ(
                    ui->bind_action_handler(&exInst->host, &secSv, exactFn, &hitsExact2),
                    0
                );
                mgr->dispatchAction("example_plugin", "sec1", "a.do", "{}");
                XX_TEST_EXPECT_EQ(hitsExact.load(), 1);
                XX_TEST_EXPECT_EQ(hitsExact2.load(), 1);
                // unbind 精确后回落 fallback
                XX_TEST_EXPECT_EQ(ui->unbind_action_handler(&exInst->host, &secSv), 0);
                mgr->dispatchAction("example_plugin", "sec1", "a.do", "{}");
                XX_TEST_EXPECT_EQ(hitsFallback.load(), 2);
                // disable 保留 / enable 恢复
                mgr->disable("example_plugin");
                {
                    auto reg = mgr->uiRegistrySnapshot();
                    bool any = false;
                    if (reg) {
                        for (const auto& b : reg->actionBindings) {
                            if (b.plugin == "example_plugin") {
                                any = true;
                            }
                        }
                    }
                    XX_TEST_EXPECT_FALSE(any);
                }
                mgr->enable("example_plugin");
                {
                    auto reg = mgr->uiRegistrySnapshot();
                    bool any = false;
                    if (reg) {
                        for (const auto& b : reg->actionBindings) {
                            if (b.plugin == "example_plugin") {
                                any = true;
                            }
                        }
                    }
                    XX_TEST_EXPECT_TRUE(any);
                }
                // dispatch 上下文内容校验 (owner/action/args 透传)
                struct CtxCap {
                    std::string owner, action, args;
                };
                CtxCap cap;
                auto   capFn = +[](const AgentxxUiActionContext* ctx, void* ud) {
                    auto* c = static_cast<CtxCap*>(ud);
                    if (ctx) {
                        if (ctx->owner_id.data) {
                            c->owner.assign(ctx->owner_id.data, ctx->owner_id.size);
                        }
                        if (ctx->action_id.data) {
                            c->action.assign(ctx->action_id.data, ctx->action_id.size);
                        }
                        if (ctx->action_args.data) {
                            c->args.assign(ctx->action_args.data, ctx->action_args.size);
                        }
                    }
                };
                XX_TEST_EXPECT_EQ(
                    ui->bind_action_handler(&exInst->host, &emptySv, capFn, &cap),
                    0
                );
                mgr->dispatchAction("example_plugin", "toolCallX", "planning.open_graph", R"({"k":1})");
                XX_TEST_EXPECT_EQ(cap.owner, "toolCallX");
                XX_TEST_EXPECT_EQ(cap.action, "planning.open_graph");
                XX_TEST_EXPECT_TRUE(cap.args.find("\"k\"") != std::string::npos);
                // 未知 action_id 同样派发 (插件侧查表, 宿主不拦截);
                // 无绑定插件 → 丢弃不崩溃
                mgr->dispatchAction("example_plugin", "sec1", "no.such", "{}");
                mgr->dispatchAction("no_plugin", "sec1", "a.do", "{}");
            }
            co_await mgr->unloadAsync("example_plugin");
            XX_TEST_EXPECT_TRUE(mgr->find("example_plugin") == nullptr);
            // unload 后点击丢弃 (快照已清)
            {
                auto reg = mgr->uiRegistrySnapshot();
                bool any = false;
                if (reg) {
                    for (const auto& b : reg->actionBindings) {
                        if (b.plugin == "example_plugin") {
                            any = true;
                        }
                    }
                }
                XX_TEST_EXPECT_FALSE(any);
            }
        }
    }

    // ---- 19. 通用 overlay: open/close 参数校验 + 适配器信号 ----
    {
        auto exPath = findExamplePluginPath();
        auto exInst = co_await mgr->loadNativeAsync(exPath);
        XX_TEST_EXPECT_TRUE(exInst != nullptr);
        if (exInst) {
            const auto ui = agentxx::plugin::ClientIfaces::query(&exInst->host).ui;
            XX_TEST_EXPECT_TRUE(ui != nullptr && ui->open_overlay != nullptr);
            XX_TEST_EXPECT_TRUE(ui != nullptr && ui->close_overlay != nullptr);
            if (ui && ui->open_overlay && ui->close_overlay) {
                // version 非法 → 失败
                AgentxxOverlaySpec badVer{};
                badVer.version = 99;
                badVer.type    = AGENTXX_OVERLAY_MERMAID;
                XX_TEST_EXPECT_TRUE(ui->open_overlay(&exInst->host, &badVer) != 0);
                // type 越界 → 失败
                AgentxxOverlaySpec badType{};
                badType.version = 1;
                badType.type    = 99;
                XX_TEST_EXPECT_TRUE(ui->open_overlay(&exInst->host, &badType) != 0);
                // MERMAID 成功 → 适配器收到信号
                const char* mermaid = "stateDiagram-v2\n[*] --> a\na --> [*]";
                AgentxxOverlaySpec spec{};
                spec.version    = 1;
                spec.type       = AGENTXX_OVERLAY_MERMAID;
                spec.title      = agentxx::plugin::PluginStringView::fromCstr("T");
                spec.payload    = agentxx::plugin::PluginStringView::fromCstr(mermaid);
                spec.extra_json = agentxx::plugin::PluginStringView::fromCstr("{}");
                XX_TEST_EXPECT_EQ(ui->open_overlay(&exInst->host, &spec), 0);
                XX_TEST_EXPECT_EQ(adapter->overlayOpenCount(), 1);
                XX_TEST_EXPECT_EQ(adapter->lastOverlayType(), AGENTXX_OVERLAY_MERMAID);
                XX_TEST_EXPECT_EQ(adapter->lastOverlayPlugin(), "example_plugin");
                XX_TEST_EXPECT_TRUE(
                    adapter->lastOverlayPayload().find("stateDiagram-v2") != std::string::npos
                );
                // close → 适配器收到信号 (不崩溃)
                ui->close_overlay(&exInst->host);
                XX_TEST_EXPECT_EQ(adapter->overlayCloseCount(), 1);
                // DIFF payload 透传 (宿主不解析, 仅校验类型)
                AgentxxOverlaySpec diffSpec{};
                diffSpec.version    = 1;
                diffSpec.type       = AGENTXX_OVERLAY_DIFF;
                diffSpec.title      = agentxx::plugin::PluginStringView::fromCstr("D");
                diffSpec.payload    = agentxx::plugin::PluginStringView::fromCstr(
                    R"({"path":"a","old_str":"x","new_str":"y"})"
                );
                diffSpec.extra_json = agentxx::plugin::PluginStringView::fromCstr("{}");
                XX_TEST_EXPECT_EQ(ui->open_overlay(&exInst->host, &diffSpec), 0);
                XX_TEST_EXPECT_EQ(adapter->overlayOpenCount(), 2);
                XX_TEST_EXPECT_EQ(adapter->lastOverlayType(), AGENTXX_OVERLAY_DIFF);
            }
            co_await mgr->unloadAsync("example_plugin");
            XX_TEST_EXPECT_TRUE(mgr->find("example_plugin") == nullptr);
        }
    }

    // ---- 20. planning 端到端: dispatch(open_graph) → open_overlay(MERMAID) ----
    {
        auto plPath = findPluginPath("agentxx_planning");
        auto plInst = co_await mgr->loadNativeAsync(plPath);
        XX_TEST_EXPECT_TRUE(plInst != nullptr);
        if (plInst) {
            // 先推送规划 (Info 段 + last_plan_json 就绪)
            agentxx::agent::WirePluginData plData;
            plData.plugin = "agentxx_planning";
            plData.event  = "planning";
            plData.data
                = R"({"roadmap":"stateDiagram-v2\n[*] --> e2e\ne2e --> [*]","todos":[]})";
            mgr->onPluginData(plData);
            const int openBefore = adapter->overlayOpenCount();
            // decor owner=toolCallId 同样被 fallback 接住
            mgr->dispatchAction("agentxx_planning", "call_e2e_1", "planning.open_graph", "{}");
            XX_TEST_EXPECT_EQ(adapter->overlayOpenCount(), openBefore + 1);
            XX_TEST_EXPECT_EQ(adapter->lastOverlayType(), AGENTXX_OVERLAY_MERMAID);
            XX_TEST_EXPECT_TRUE(
                adapter->lastOverlayPayload().find("e2e") != std::string::npos
            );
            XX_TEST_EXPECT_EQ(adapter->lastOverlayTitle(), "Planning Roadmap");
            co_await mgr->unloadAsync("agentxx_planning");
            XX_TEST_EXPECT_TRUE(mgr->find("agentxx_planning") == nullptr);
        }
    }

    // ---- 21. kit::ActionController 单测 (header-only, 无宿主) ----
    {
        agentxx::plugin::kit::ActionController ctl;
        XX_TEST_EXPECT_TRUE(ctl.empty());
        int hitsA = 0, hitsB = 0;
        ctl.on("a.open", [&](const neograph::json&) {
            ++hitsA;
        });
        auto btn = ctl.makeButton(
            "Go",
            [&](const neograph::json& args) {
                ++hitsB;
                XX_TEST_EXPECT_EQ(args.value("k", 0), 1);
            },
            "|- ",
            "accent",
            neograph::json{{"k", 1}}
        );
        XX_TEST_EXPECT_TRUE(!ctl.empty());
        XX_TEST_EXPECT_EQ(ctl.size(), 2U);
        XX_TEST_EXPECT_EQ(btn.value("kind", ""), "button");
        XX_TEST_EXPECT_EQ(btn.value("label", ""), "Go");
        XX_TEST_EXPECT_EQ(btn.value("prefix", ""), "|- ");
        XX_TEST_EXPECT_EQ(btn.value("role", ""), "accent");
        XX_TEST_EXPECT_TRUE(btn.contains("action_id"));
        XX_TEST_EXPECT_TRUE(btn.contains("args"));
        // dispatch 固定 id
        AgentxxUiActionContext ctxA{};
        ctxA.version     = 1;
        auto aId         = agentxx::plugin::PluginStringView::fromCstr("a.open");
        auto emptyArgs   = agentxx::plugin::PluginStringView::fromCstr("{}");
        ctxA.action_id   = aId;
        ctxA.action_args = emptyArgs;
        agentxx::plugin::kit::ActionController::dispatch(&ctxA, &ctl);
        XX_TEST_EXPECT_EQ(hitsA, 1);
        // dispatch 自增 id (args 透传)
        AgentxxUiActionContext ctxB{};
        ctxB.version = 1;
        std::string bIdStr = btn.value("action_id", "");
        std::string bArgsStr = btn["args"].dump();
        auto bIdSv = agentxx::plugin::PluginStringView::from(bIdStr.data(), bIdStr.size());
        auto bArgsSv = agentxx::plugin::PluginStringView::from(bArgsStr.data(), bArgsStr.size());
        ctxB.action_id = bIdSv;
        ctxB.action_args = bArgsSv;
        agentxx::plugin::kit::ActionController::dispatch(&ctxB, &ctl);
        XX_TEST_EXPECT_EQ(hitsB, 1);
        // 未知 id / 空指针守卫 (不崩溃)
        AgentxxUiActionContext ctxC{};
        ctxC.version = 1;
        auto cId = agentxx::plugin::PluginStringView::fromCstr("no.such");
        ctxC.action_id = cId;
        ctxC.action_args = emptyArgs;
        agentxx::plugin::kit::ActionController::dispatch(&ctxC, &ctl);
        agentxx::plugin::kit::ActionController::dispatch(nullptr, &ctl);
        agentxx::plugin::kit::ActionController::dispatch(&ctxC, nullptr);
        XX_TEST_EXPECT_EQ(hitsA, 1);
        XX_TEST_EXPECT_EQ(hitsB, 1);
    }

    // ---- 22. plugin_ui_items 共享 helper 单测 (headless, 无 FTXUI 布局) ----
    {
        using namespace agentxx::client;
        agentxx::plugin::ClientUiRegistry reg;
        // 无绑定 → button 不可点
        PluginButtonDesc desc;
        neograph::json btnJson = neograph::json::parse(
            R"({"kind":"button","label":"[Graph]","action_id":"planning.open_graph","args":{},"role":"accent"})"
        );
        XX_TEST_EXPECT_TRUE(parsePluginButton(btnJson, "agentxx_planning", &reg, desc));
        XX_TEST_EXPECT_EQ(desc.label, "[Graph]");
        XX_TEST_EXPECT_EQ(desc.actionId, "planning.open_graph");
        XX_TEST_EXPECT_EQ(desc.argsJson, "{}");
        XX_TEST_EXPECT_TRUE(desc.role == PluginButtonRole::Accent);
        XX_TEST_EXPECT_FALSE(desc.clickable);
        // 绑定后 → 可点 (fallback "" 覆盖)
        agentxx::plugin::ClientActionBinding b;
        b.plugin   = "agentxx_planning";
        b.targetId = "";
        b.cb = +[](const AgentxxUiActionContext*, void*) {};
        reg.actionBindings.push_back(b);
        XX_TEST_EXPECT_TRUE(hasPluginBinding("agentxx_planning", &reg));
        XX_TEST_EXPECT_TRUE(hasPluginBindingFor("agentxx_planning", "call_x", &reg));
        XX_TEST_EXPECT_FALSE(hasPluginBindingFor("agentxx_planning", "call_x", nullptr));
        XX_TEST_EXPECT_FALSE(hasPluginBinding("other_plugin", &reg));
        PluginButtonDesc desc2;
        XX_TEST_EXPECT_TRUE(parsePluginButton(btnJson, "agentxx_planning", &reg, desc2));
        XX_TEST_EXPECT_TRUE(desc2.clickable);
        // 精确绑定同样可点; 他插件不可点
        reg.actionBindings.clear();
        b.targetId = "call_x";
        reg.actionBindings.push_back(b);
        XX_TEST_EXPECT_TRUE(hasPluginBindingFor("agentxx_planning", "call_x", &reg));
        // owner=other 时精确未命中且无 "" 兜底 → false
        XX_TEST_EXPECT_FALSE(hasPluginBindingFor("agentxx_planning", "other_owner", &reg));
        PluginButtonDesc desc3;
        XX_TEST_EXPECT_TRUE(parsePluginButton(btnJson, "other_plugin", &reg, desc3));
        XX_TEST_EXPECT_FALSE(desc3.clickable);
        // 旧 action kind 兼容: action_id=id, role=accent
        neograph::json actJson = neograph::json::parse(
            R"({"kind":"action","id":"rebuild","label":"Rebuild"})"
        );
        PluginButtonDesc descAct;
        XX_TEST_EXPECT_TRUE(parsePluginButton(actJson, "agentxx_planning", &reg, descAct));
        XX_TEST_EXPECT_EQ(descAct.actionId, "rebuild");
        XX_TEST_EXPECT_TRUE(descAct.role == PluginButtonRole::Accent);
        // 无 action_id → 静态 (不可点, 但解析成功)
        neograph::json staticJson = neograph::json::parse(
            R"({"kind":"button","label":"Static"})"
        );
        PluginButtonDesc descStatic;
        XX_TEST_EXPECT_TRUE(parsePluginButton(staticJson, "agentxx_planning", &reg, descStatic));
        XX_TEST_EXPECT_TRUE(descStatic.actionId.empty());
        XX_TEST_EXPECT_FALSE(descStatic.clickable);
        // 非 button → false
        neograph::json textJson = neograph::json::parse(
            R"({"kind":"text","text":"hi"})"
        );
        PluginButtonDesc descText;
        XX_TEST_EXPECT_FALSE(parsePluginButton(textJson, "agentxx_planning", &reg, descText));
        // role 非法值 → Normal; danger 映射
        XX_TEST_EXPECT_TRUE(parseButtonRole("normal") == PluginButtonRole::Normal);
        XX_TEST_EXPECT_TRUE(parseButtonRole("accent") == PluginButtonRole::Accent);
        XX_TEST_EXPECT_TRUE(parseButtonRole("danger") == PluginButtonRole::Danger);
        XX_TEST_EXPECT_TRUE(parseButtonRole("weird") == PluginButtonRole::Normal);
        // render 不崩溃 (headless 仅构造 Element, 不布局)
        {
            auto el1 = renderPluginButton(desc2, TUITheme::darkTheme());
            XX_TEST_EXPECT_TRUE(static_cast<bool>(el1));
            auto el2 = renderPluginTextItem("hi", "title", TUITheme::darkTheme());
            XX_TEST_EXPECT_TRUE(static_cast<bool>(el2));
            auto el3 = renderPluginDiff("p", "a\n", "b\n", TUITheme::darkTheme(), 120);
            XX_TEST_EXPECT_TRUE(static_cast<bool>(el3));
        }
    }

    co_return TestResult{g_client_plugin_passed, g_client_plugin_failed};
}

} // namespace test
} // namespace agentxx
