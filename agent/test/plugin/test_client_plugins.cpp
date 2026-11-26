/// test_client_plugins.cpp —— client 侧插件系统测试 (模块名 `client_plugins`)
///
/// 覆盖:
/// 1. 加载 example_plugin 的 client 入口 (agentxx_client_plugin_create)
/// 2. UI 注册表: 状态栏项 / 面板 / Info 栏段落 / 命令 (含快照读取)
/// 3. 频道分发: `agentxx.client.ready` / `turn_end` / `user_input` → 插件回调
/// 4. 命令执行: /example (send 动作) / example_toast (toast 动作)
/// 5. 跨端数据: send_plugin_data → adapter sendPluginData (WirePluginDataUp 路径)
/// 6. disable/enable 恢复与 unload 清理 (含 Info 栏段落注册/摘除信号)
/// 13b. agentxx_chatbot client 插件: 侧边栏「机器人」栏 (面板注册 / 快照渲染 /
///      刷新请求 / 坏载荷容错 / 卸载清理)
/// 18. 通用交互: bind/unbind 覆盖 + 精确优先 vs fallback + 上下文原样转发
/// 19. 通用 overlay: open/close 参数校验 + 适配器信号
/// 20. planning 全流程: dispatch(open_graph) → open_overlay(MERMAID)
/// 21. kit::ActionController 单测 (header-only, 无主程序)
/// 28. 主程序内置工具特化渲染: agentxx_share_store / agentxx_subagent
///     (lib 内置工具无对应插件, 由主程序注册内置渲染器)
/// 34. JS 脚本作为 client 插件: 工具行 / 展示面 / 命令 / 动作
/// 34b. 形态注册表: 第二个运行器接管预置 `js` (native 不可接管、代次沿用自带声明)
/// 29. 接口协商: TUI 主程序声明 ui 表整体 + 全部子能力, 构建产物中的内置插件
///     client 侧声明必须全部被满足 (避免启动误报"可选接口不支持")
#include "agentxx-test/plugin/test_client_plugins.h"
#include "agentxx-test/plugin/test_table_op.h"
#include "agentxx-test/test_coro_util.h"

#include "agentxx-client/io/tui/plugin_ui_items.h"
#include "agentxx-client/io/tui/tui_plugin_adapter.h"
#include "agentxx/plugin/api/agentxx_broadcast_ids.h"
#include "agentxx/plugin/api/agent_plugin_kit.h"
#include "agentxx/plugin/api/client_plugin_kit.h"
#include "agentxx/plugin/builtin_tool_renderers.h"
#include "agentxx/plugin/client_plugin_manager.h"
#include "agentxx/util/diagnostics.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/executor_work_guard.hpp"
#include "asio/io_context.hpp"
#include "asio/post.hpp"
#include "asio/use_awaitable.hpp"
#include "pluginxx/host/kind_registry.h"
#include "pluginxx/runtime/instance_base.h"
#include "utilxx_base/log.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_client_plugin_passed = 0;
int g_client_plugin_failed = 0;

/// 等待若干毫秒 (工具语义渲染请求投递到 io 线程后等待其完成)
static asio::awaitable<void> sleepMs(int ms) {
    auto timer
        = asio::steady_timer(co_await asio::this_coro::executor, std::chrono::milliseconds{ms});
    co_await timer.async_wait(asio::use_awaitable);
}

/// 轮询等待条件成立 (插件回调在插件执行线程上跑, 断言前要先等效果出现)
///
/// - `pred` 在测试协程 (client io 线程) 上检查, 只读主程序注册表快照与适配器计数器
/// - 超时返回 false, 由调用处断言报出 (helper 自己不记失败, 便于一次定位)
asio::awaitable<bool> waitUntil(
    std::function<bool()> pred,
    int                   timeoutMs = 2000,
    int                   stepMs    = 5
) {
    const int loops = timeoutMs / stepMs;
    for (int i = 0; i < loops; ++i) {
        if (pred()) {
            co_return true;
        }
        co_await sleepMs(stepMs);
    }
    co_return pred();
}

/// 等某个插件的执行线程把已投递的回调都跑完 (FIFO 探针)
///
/// 事件注入 (`onPluginData` / `onReady` / `onConnStateChanged` / `onDelta` / 命令 ...)
/// 只是把回调投到插件执行线程, 效果要等它跑完才出现在注册表与适配器上。探针与
/// 先前回调同在一条队列上, 探针跑到就说明先前的都已返回 (插件回调里的表操作是同步
/// 等待, 返回即已生效)。
asio::awaitable<void> drainPlugin(
    const std::shared_ptr<agentxx::plugin::ClientPluginManager>& mgr,
    const std::string&                                           name
) {
    auto inst = mgr->find(name);
    if (!inst) {
        co_return;
    }
    auto owner = inst->self.lock();
    if (!owner) {
        co_return;
    }
    auto       done   = std::make_shared<std::atomic<bool>>(false);
    const bool queued = pluginxx::dispatchToPlugin(
        owner,
        "test drain",
        [done] { done->store(true, std::memory_order_release); },
        pluginxx::PluginDispatchOptions{.allowClosing = true, .measure = false}
    );
    if (!queued) {
        co_return;
    }
    (void)co_await waitUntil(
        [done] { return done->load(std::memory_order_acquire); },
        5000
    );
}

/// client 表调用的结果 (测试夹具)
///
/// client 六张表都是异步操作契约。测试协程与 `ClientPluginManager` 的 io 线程是**同一个
/// 线程**, 因此表操作在提交点内联完成: 提交返回时 `done` 已经是 true。
struct ClientCall {
    bool        accepted = false; ///< 入口是否受理 (false = 立即被拒, 原因在 payload)
    bool        done     = false; ///< 是否已收到完成通知
    int32_t     status   = PLUGINXX_OPERATOR_FAILED;
    std::string payload; ///< 结果正文 (未受理时 = 拒绝原因)

    bool ok() const noexcept {
        return accepted && done && status == PLUGINXX_OPERATOR_OK;
    }
};

/// 取回表操作的句柄 (payload = 进程内地址的十进制文本)
template<typename Handle>
Handle* handleOf(const ClientCall& call) {
    return call.ok() ? agentxx::plugin::clientHandleFromText<Handle>(call.payload) : nullptr;
}

void PLUGINXX_CALL clientCallDone(void* ud, int32_t status, const PluginxxStringView* payload) {
    auto* call   = static_cast<ClientCall*>(ud);
    call->done   = true;
    call->status = status;
    if (payload && payload->data && payload->size > 0) {
        call->payload.assign(payload->data, static_cast<size_t>(payload->size));
    }
}

/// 提交一次 client 表调用并取回结果 (见 [ClientCall])
template<typename StartFn>
ClientCall clientCall(const PluginxxHost* host, StartFn&& start) {
    ClientCall     call;
    PluginxxString err{nullptr, 0};
    call.accepted = start(&clientCallDone, &call, &err) != nullptr;
    if (!call.accepted) {
        call.done = true;
        if (err.data) {
            call.payload.assign(err.data, static_cast<size_t>(err.size));
        }
    }
    if (err.data) {
        agentxx::plugin::PluginString::free(host, &err);
    }
    return call;
}

/* ---------- client 表便捷包装 (测试侧: 提交即取结果) ----------
 *
 * 形态与改造前的同步入口一一对应, 便于用例本体保持原样; 实现走 [clientCall]。
 */

static int32_t uiBindAction(
    const PluginxxHost*         host,
    const AgentxxClientUiIface* ui,
    std::string_view            target,
    AgentxxUiActionFn           fn,
    void*                       ud
) {
    if (!ui || !ui->bind_action_handler || !fn) {
        return -1;
    }
    auto targetSv = agentxx::plugin::PluginStringView::from(target.data(), target.size());
    const auto call = clientCall(host, [&](PluginxxOperatorCallback cb, void* cud, PluginxxString* err) {
        return ui->bind_action_handler(host, &targetSv, fn, ud, cb, cud, err);
    });
    return call.ok() ? 0 : -1;
}

static int32_t uiUnbindAction(
    const PluginxxHost*         host,
    const AgentxxClientUiIface* ui,
    std::string_view            target
) {
    if (!ui || !ui->unbind_action_handler) {
        return -1;
    }
    auto targetSv = agentxx::plugin::PluginStringView::from(target.data(), target.size());
    const auto call = clientCall(host, [&](PluginxxOperatorCallback cb, void* cud, PluginxxString* err) {
        return ui->unbind_action_handler(host, &targetSv, cb, cud, err);
    });
    return call.ok() ? 0 : -1;
}

static int32_t uiOpenOverlay(
    const PluginxxHost*         host,
    const AgentxxClientUiIface* ui,
    const AgentxxOverlaySpec*   spec
) {
    if (!ui || !ui->open_overlay || !spec) {
        return -1;
    }
    const auto call = clientCall(host, [&](PluginxxOperatorCallback cb, void* cud, PluginxxString* err) {
        return ui->open_overlay(host, spec, cb, cud, err);
    });
    return call.ok() ? 0 : -1;
}

static int32_t uiCloseOverlay(const PluginxxHost* host, const AgentxxClientUiIface* ui) {
    if (!ui || !ui->close_overlay) {
        return -1;
    }
    const auto call = clientCall(host, [&](PluginxxOperatorCallback cb, void* cud, PluginxxString* err) {
        return ui->close_overlay(host, cb, cud, err);
    });
    return call.ok() ? 0 : -1;
}

static int32_t uiUpdateToolDecor(
    const PluginxxHost*         host,
    const AgentxxClientUiIface* ui,
    std::string_view            toolCallId,
    std::string_view            decorJson
) {
    if (!ui || !ui->update_tool_decor) {
        return -1;
    }
    auto tcidSv  = agentxx::plugin::PluginStringView::from(toolCallId.data(), toolCallId.size());
    auto decorSv = agentxx::plugin::PluginStringView::from(decorJson.data(), decorJson.size());
    const auto call = clientCall(host, [&](PluginxxOperatorCallback cb, void* cud, PluginxxString* err) {
        return ui->update_tool_decor(host, &tcidSv, &decorSv, cb, cud, err);
    });
    return call.ok() ? 0 : -1;
}

static AgentxxStatusItem* uiRegisterStatusItem(
    const PluginxxHost*         host,
    const AgentxxClientUiIface* ui,
    std::string_view            id,
    std::string_view            initialJson,
    int32_t                     align = 0,
    int32_t                     order = 0
) {
    if (!ui || !ui->register_status_item) {
        return nullptr;
    }
    auto idSv   = agentxx::plugin::PluginStringView::from(id.data(), id.size());
    auto jsonSv = agentxx::plugin::PluginStringView::from(initialJson.data(), initialJson.size());
    const auto call = clientCall(host, [&](PluginxxOperatorCallback cb, void* cud, PluginxxString* err) {
        return ui->register_status_item(host, &idSv, &jsonSv, align, order, cb, cud, err);
    });
    return handleOf<AgentxxStatusItem>(call);
}

static void
    uiUnregisterStatusItem(const PluginxxHost* host, const AgentxxClientUiIface* ui, AgentxxStatusItem* item) {
    if (!ui || !ui->unregister_status_item || !item) {
        return;
    }
    ui->unregister_status_item(host, item, nullptr, nullptr, nullptr);
}

static AgentxxPanel* uiRegisterPanel(
    const PluginxxHost*         host,
    const AgentxxClientUiIface* ui,
    std::string_view            id,
    std::string_view            propsJson
) {
    if (!ui || !ui->register_panel) {
        return nullptr;
    }
    auto idSv    = agentxx::plugin::PluginStringView::from(id.data(), id.size());
    auto propsSv = agentxx::plugin::PluginStringView::from(propsJson.data(), propsJson.size());
    const auto call = clientCall(host, [&](PluginxxOperatorCallback cb, void* cud, PluginxxString* err) {
        return ui->register_panel(host, &idSv, &propsSv, cb, cud, err);
    });
    return handleOf<AgentxxPanel>(call);
}

static int32_t uiUpdatePanel(
    const PluginxxHost*         host,
    const AgentxxClientUiIface* ui,
    AgentxxPanel*               panel,
    std::string_view            itemsJson
) {
    if (!ui || !ui->update_panel) {
        return -1;
    }
    auto jsonSv = agentxx::plugin::PluginStringView::from(itemsJson.data(), itemsJson.size());
    const auto call = clientCall(host, [&](PluginxxOperatorCallback cb, void* cud, PluginxxString* err) {
        return ui->update_panel(host, panel, &jsonSv, cb, cud, err);
    });
    return call.ok() ? 0 : -1;
}

static AgentxxTimer* timerSet(
    const PluginxxHost*            host,
    const AgentxxClientTimerIface* table,
    const AgentxxTimerSpec*        spec
) {
    if (!table || !table->set_timer || !spec) {
        return nullptr;
    }
    const auto call = clientCall(host, [&](PluginxxOperatorCallback cb, void* cud, PluginxxString* err) {
        return table->set_timer(host, spec, cb, cud, err);
    });
    return handleOf<AgentxxTimer>(call);
}

static void
    timerCancel(const PluginxxHost* host, const AgentxxClientTimerIface* table, AgentxxTimer* timer) {
    if (!table || !table->cancel_timer) {
        return;
    }
    table->cancel_timer(host, timer, nullptr, nullptr, nullptr);
}

/// 关联区域是否可见; 未上报/非法 = 0 (与表成员的取值相同)
static int32_t timerIsVisible(
    const PluginxxHost*            host,
    const AgentxxClientTimerIface* table,
    std::string_view               ownerId
) {
    if (!table || !table->is_visible) {
        return 0;
    }
    auto idSv = agentxx::plugin::PluginStringView::from(ownerId.data(), ownerId.size());
    const auto call = clientCall(host, [&](PluginxxOperatorCallback cb, void* cud, PluginxxString* err) {
        return table->is_visible(host, &idSv, cb, cud, err);
    });
    return (call.ok() && call.payload == "1") ? 1 : 0;
}

static AgentxxKeybind* keybindRegister(
    const PluginxxHost*              host,
    const AgentxxClientKeybindIface* table,
    const AgentxxKeybindSpec*        spec
) {
    if (!table || !table->register_keybind || !spec) {
        return nullptr;
    }
    const auto call = clientCall(host, [&](PluginxxOperatorCallback cb, void* cud, PluginxxString* err) {
        return table->register_keybind(host, spec, cb, cud, err);
    });
    return handleOf<AgentxxKeybind>(call);
}

static void keybindUnregister(
    const PluginxxHost*              host,
    const AgentxxClientKeybindIface* table,
    AgentxxKeybind*                  bind
) {
    if (!table || !table->unregister_keybind) {
        return;
    }
    table->unregister_keybind(host, bind, nullptr, nullptr, nullptr);
}

/// 已注册快捷键列表 JSON (失败返回空串)
static std::string
    keybindList(const PluginxxHost* host, const AgentxxClientKeybindIface* table) {
    if (!table || !table->list_keybinds) {
        return {};
    }
    const auto call = clientCall(host, [&](PluginxxOperatorCallback cb, void* cud, PluginxxString* err) {
        return table->list_keybinds(host, cb, cud, err);
    });
    return call.ok() ? call.payload : std::string{};
}

/// 同步关闭路径无法等待 stop 事务: 该 hook 只用于断言“未被调用”。
void PLUGINXX_CALL fakeClientStopHook(void*, const PluginxxOperatorNotify*, PluginxxString*) {}

/// client 侧 create/stop 事务探针 (记录调用次数; 不保存任何跨调用全局状态)
struct ClientLifecycleProbe {
    int starts = 0;
    int stops  = 0;
};

/// create = 构造 + 注册 (可重入): 上下文已经是上一次的 `&probe`, 只计数
void PLUGINXX_CALL probeClientCreateHook(
    const PluginxxHost*,
    void**                        plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*
) {
    ++static_cast<ClientLifecycleProbe*>(*plugin_ctx)->starts;
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
}

void PLUGINXX_CALL
    probeClientStopHook(void* ud, const PluginxxOperatorNotify* notify, PluginxxString*) {
    ++static_cast<ClientLifecycleProbe*>(ud)->stops;
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
}

#if XX_IS_WIN_D
static void setSystemEnvVar(const std::string& key, const std::string& value) {
    _putenv_s(key.c_str(), value.c_str());
}

static void clearSystemEnvVar(const std::string& key) {
    _putenv_s(key.c_str(), "");
}
#else
static void setSystemEnvVar(const std::string& key, const std::string& value) {
    setenv(key.c_str(), value.c_str(), 1);
}

static void clearSystemEnvVar(const std::string& key) {
    unsetenv(key.c_str());
}
#endif

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
    if (auto exeDir = executableDir()) {
        candidates.push_back(*exeDir / "plugins" / name);
    }
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
        return {// 与 TuiPluginAdapter 声明集保持一致 (完整消息渲染面主程序):
                // ui 表整体 + 全部子能力
                std::string{pi::ClientUi},
                std::string{pi::ClientStatusItem},
                std::string{pi::ClientPanel},
                std::string{pi::ClientToast},
                std::string{pi::ClientInfoSection},
                std::string{pi::ClientCommand},
                std::string{pi::ClientMsgDecor},
                std::string{pi::ClientAction},
                std::string{pi::ClientOverlay},
                std::string{pi::ClientTimer},
                std::string{pi::ClientKeybind}
        };
    }

    void onStatusItemRegistered(
        const std::string& id,
        const utilxx_base::Json& /*props*/,
        int /*align*/,
        int /*order*/
    ) override {
        std::lock_guard<std::mutex> lock(m_);
        ++statusRegistered_;
        lastStatusId_ = id;
    }

    void onStatusItemUpdated(const std::string& id, const utilxx_base::Json& /*props*/) override {
        std::lock_guard<std::mutex> lock(m_);
        ++statusUpdated_;
        lastStatusId_ = id;
    }

    void onStatusItemRemoved(const std::string& id) override {
        std::lock_guard<std::mutex> lock(m_);
        ++statusRemoved_;
        lastStatusId_ = id;
    }

    void onPanelRegistered(const std::string& id, const utilxx_base::Json& /*props*/) override {
        std::lock_guard<std::mutex> lock(m_);
        ++panelRegistered_;
        lastPanelId_ = id;
    }

    void onPanelUpdated(const std::string& id, const utilxx_base::Json& /*items*/) override {
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
        const utilxx_base::Json& /*props*/
    ) override {
        std::lock_guard<std::mutex> lock(m_);
        ++infoSectionRegistered_;
        lastInfoSectionId_ = id;
    }

    void onInfoSectionUpdated(const std::string& id, const utilxx_base::Json& /*items*/) override {
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

    bool sendPluginData(const std::string& topic, const std::string& json) override {
        std::lock_guard<std::mutex> lock(m_);
        ++dataUpCount_;
        lastDataTopic_ = topic;
        lastDataJson_  = json;
        dataUpTopics_.push_back(topic);
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

    std::string lastDataTopic() const {
        std::lock_guard<std::mutex> lock(m_);
        return lastDataTopic_;
    }

    /// 某主题是否曾经上行过 (插件回调异步执行, 多条上行的先后不固定)
    bool hasDataTopic(const std::string& topic) const {
        std::lock_guard<std::mutex> lock(m_);
        for (const auto& t : dataUpTopics_) {
            if (t == topic) {
                return true;
            }
        }
        return false;
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
    std::string        lastDataTopic_;
    std::string        lastDataJson_;
    std::vector<std::string> dataUpTopics_; ///< 全部上行主题 (按到达顺序)
    std::string        lastOverlayPlugin_;
    std::string        lastOverlayTitle_;
    std::string        lastOverlayPayload_;
    std::string        lastOverlayExtra_;
    std::string        lastOverlayClosePlugin_;
};

/// 工具特化渲染的测试视图 (折叠内容与展开内容各查一次, 与 UI 线程同一路径)
struct RenderedTool {
    std::string       displayName; ///< 同步查表的生效显示名 (未命中时为原始工具名)
    std::string       summary;     ///< 折叠内容 (不含分隔符 " · ")
    utilxx_base::Json items;       ///< 展开内容
    bool              matched          = false; ///< 折叠内容非空 (有自定义内容)
    bool              collapsedPending = false; ///< 折叠内容未就绪且没有上一份
    bool              expandedPending  = false; ///< 展开内容未就绪且没有上一份
};

/// 展开内容查询模型: UI 线程只读缓存, 未命中时提交一次请求再等结果写入
asio::awaitable<agentxx::plugin::ClientToolExpandedResult> queryExpandedAsync(
    const std::shared_ptr<agentxx::plugin::ClientPluginManager>& mgr,
    const std::string&                                           toolCallId,
    const std::string&                                           toolName,
    const std::string&                                           argsJson,
    const std::string&                                           resultText,
    bool                                                         isFinished,
    bool                                                         isError,
    int                                                          maxWidth
) {
    auto reg   = mgr->uiRegistrySnapshot();
    auto cache = mgr->toolRenderCache();
    auto res   = agentxx::plugin::renderClientToolExpanded(
        reg.get(),
        cache.get(),
        toolCallId,
        toolName,
        argsJson,
        resultText,
        isFinished,
        isError,
        maxWidth
    );
    if (!res.pending) {
        co_return res;
    }
    agentxx::plugin::ClientToolRenderRequest req;
    req.toolCallId = toolCallId;
    req.toolName   = toolName;
    req.argsJson   = argsJson;
    req.resultText = resultText;
    req.part       = agentxx::plugin::ClientToolRenderPart::Expanded;
    req.isFinished = isFinished;
    req.isError    = isError;
    req.maxWidth   = maxWidth;
    const std::string key = agentxx::plugin::ClientToolRenderRequest::keyFor(
        toolCallId,
        toolName,
        agentxx::plugin::ClientToolRenderPart::Expanded
    );
    const uint64_t hash = req.inputHash();
    mgr->requestToolRender(req);
    // 请求可能内联执行 (本协程与 manager 同 executor), 否则让出执行权等它完成
    for (int i = 0; i < 500 && !cache->lookup(key, hash); ++i) {
        co_await sleepMs(1);
    }
    auto reg2 = mgr->uiRegistrySnapshot();
    co_return agentxx::plugin::renderClientToolExpanded(
        reg2.get(),
        cache.get(),
        toolCallId,
        toolName,
        argsJson,
        resultText,
        isFinished,
        isError,
        maxWidth
    );
}

/// 工具特化渲染模型:
/// - 显示名: 同步查表 (decor > 注册表), 不投递请求
/// - 折叠内容 / 展开内容: 各自只读缓存, 未命中时提交一次请求并等结果写入,
///   再从缓存读取 (UI 线程只读主程序语义快照, 不进入插件代码)
asio::awaitable<RenderedTool> renderToolAsync(
    const std::shared_ptr<agentxx::plugin::ClientPluginManager>& mgr,
    std::string                                                  toolCallId,
    std::string                                                  toolName,
    std::string                                                  argsJson,
    std::string                                                  resultText,
    bool                                                         isFinished,
    bool                                                         isError,
    int                                                          maxWidth
) {
    RenderedTool out;
    auto         reg = mgr->uiRegistrySnapshot();
    auto         cache = mgr->toolRenderCache();
    const auto   name
        = agentxx::plugin::toolDisplayNameFor(reg.get(), toolCallId, toolName);
    out.displayName = name.empty() ? toolName : std::string{name};

    // 折叠内容
    auto collapsed = agentxx::plugin::renderClientToolCollapsed(
        reg.get(),
        cache.get(),
        toolCallId,
        toolName,
        argsJson,
        resultText,
        isFinished,
        isError,
        maxWidth
    );
    if (collapsed.pending) {
        agentxx::plugin::ClientToolRenderRequest req;
        req.toolCallId = toolCallId;
        req.toolName   = toolName;
        req.argsJson   = argsJson;
        req.resultText = resultText;
        req.part       = agentxx::plugin::ClientToolRenderPart::Collapsed;
        req.isFinished = isFinished;
        req.isError    = isError;
        req.maxWidth   = maxWidth;
        const std::string key = agentxx::plugin::ClientToolRenderRequest::keyFor(
            toolCallId,
            toolName,
            agentxx::plugin::ClientToolRenderPart::Collapsed
        );
        const uint64_t hash = req.inputHash();
        mgr->requestToolRender(req);
        for (int i = 0; i < 500 && !cache->lookup(key, hash); ++i) {
            co_await sleepMs(1);
        }
        auto reg2  = mgr->uiRegistrySnapshot();
        collapsed  = agentxx::plugin::renderClientToolCollapsed(
            reg2.get(),
            cache.get(),
            toolCallId,
            toolName,
            argsJson,
            resultText,
            isFinished,
            isError,
            maxWidth
        );
    }
    out.summary          = collapsed.text;
    out.matched          = !collapsed.text.empty();
    out.collapsedPending = collapsed.pending;

    // 展开内容 (调用方会渲染展开体时才需要; 测试里总是查一次)
    auto expanded          = co_await queryExpandedAsync(
        mgr,
        toolCallId,
        toolName,
        argsJson,
        resultText,
        isFinished,
        isError,
        maxWidth
    );
    out.items           = std::move(expanded.items);
    out.expandedPending = expanded.pending;
    co_return out;
}

// ---- 声明式界面的测试生成函数 ----

/// 生成函数的现场 (测试自己持有, 必须在内容撤销之后再释放)
struct UiGenProbe {
    std::atomic<int> calls{0};
    int              delayMs = 0;
    bool             fail    = false;
};

/// 生成函数: 数次数 + 把上下文原样放进 `data`
/// - `delayMs > 0` 时先睡一下 (抛给“插件卡住时界面不阻塞”的用例)
/// - `fail` 时用失败状态交回 (主程序保留上一份内容)
void PLUGINXX_CALL testUiGenerate(
    const PluginxxHost*           /*host*/,
    const PluginxxStringView*     ctx,
    void*                         ud,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               /*error_out*/
) {
    auto* probe = static_cast<UiGenProbe*>(ud);
    if (probe == nullptr || notify == nullptr || notify->done == nullptr) {
        return;
    }
    if (probe->delayMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds{probe->delayMs});
    }
    if (probe->fail) {
        const std::string        reason{"generate failed on purpose"};
        const PluginxxStringView view{reason.data(), reason.size()};
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_FAILED, &view);
        return;
    }
    const int n = probe->calls.fetch_add(1) + 1;
    utilxx_base::Json block;
    block["kind"] = "Text";
    block["text"] = fmt::format("n={}", n);
    utilxx_base::Json doc;
    doc["blocks"] = utilxx_base::Json::array({block});
    utilxx_base::Json body;
    body["json"] = doc;
    if (ctx != nullptr && ctx->data != nullptr) {
        body["data"] = utilxx_base::Json::parse(
            std::string{ctx->data, static_cast<size_t>(ctx->size)}
        );
    }
    const std::string        payload = body.dump();
    const PluginxxStringView view{payload.data(), payload.size()};
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, &view);
}

/// 工具行内容的测试生成函数 (声明式写法)
/// - 折叠头 (`part == "summary"`) 给一行文本的块
/// - 展开体 (`part == "body"`) 给一块 KV (带这次调用的参数)
/// 同时把收到的上下文顺序记进 [g_toolRowContexts], 供用例检查“变量只给声明过的”。
std::vector<std::string> g_toolRowContexts;

void PLUGINXX_CALL testToolRowGenerate(
    const PluginxxHost*           /*host*/,
    const PluginxxStringView*     ctx,
    void*                         /*ud*/,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               /*error_out*/
) {
    if (notify == nullptr || notify->done == nullptr) {
        return;
    }
    const std::string ctxText
        = (ctx != nullptr && ctx->data != nullptr)
              ? std::string{ctx->data, static_cast<size_t>(ctx->size)}
              : std::string{"{}"};
    g_toolRowContexts.push_back(ctxText);
    utilxx_base::Json parsed;
    try {
        parsed = utilxx_base::Json::parse(ctxText);
    } catch (...) {
        parsed = utilxx_base::Json::object();
    }
    const std::string part  = parsed.value("agentxx.client.tool.part", std::string{});
    const std::string args  = parsed.value("agentxx.client.tool.args", std::string{});
    utilxx_base::Json doc   = utilxx_base::Json::object();
    utilxx_base::Json block = utilxx_base::Json::object();
    if (part == "summary") {
        block["kind"] = "Text";
        block["text"] = "summary line";
    } else {
        block["kind"]  = "Text";
        block["text"]  = "body " + args;
    }
    doc["blocks"] = utilxx_base::Json::array({block});
    utilxx_base::Json body = utilxx_base::Json::object();
    body["json"]           = doc;
    const std::string        payload = body.dump();
    const PluginxxStringView view{payload.data(), payload.size()};
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, &view);
}

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
    // 首次加载插件时管理器自动起插件执行线程 (不再有开关): 插件代码不在 client io 线程
    XX_TEST_EXPECT_TRUE(mgr->hasPluginExecutor());

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
            // 旧注册项同样落在声明式注册表里: 同一份存储, 界面只有一条绘制路径
            if (auto uiSnap = mgr->uiContentSnapshot()) {
                const auto* status = uiSnap->contentOf(
                    agentxx::plugin::kClientUiSlotStatus,
                    "example_plugin.turns"
                );
                XX_TEST_EXPECT_TRUE(status != nullptr);
                if (status != nullptr) {
                    XX_TEST_EXPECT_TRUE(status->hasResult);
                    // 文本形态的状态栏项按一行文本给出
                    XX_TEST_EXPECT_TRUE(status->parsed.contains("blocks"));
                    XX_TEST_EXPECT_TRUE(status->json.find("turns: 0") != std::string::npos);
                }
                XX_TEST_EXPECT_TRUE(
                    uiSnap->contentOf(agentxx::plugin::kClientUiSlotPanel, "example_plugin.panel")
                    != nullptr
                );
                XX_TEST_EXPECT_TRUE(
                    uiSnap->contentOf(agentxx::plugin::kClientUiSlotInfo, "example_plugin.info")
                    != nullptr
                );
            }
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

    // ---- 2.4 接口表: `<应用名>.net` (客户端也提供网络能力) ----
    {
        auto ifaceNet = agentxx::plugin::queryInterface<PluginxxNetIface>(
            inst->hostView(),
            PLUGINXX_IFACE_NET
        );
        XX_TEST_EXPECT_TRUE(ifaceNet != nullptr);
        if (ifaceNet) {
            XX_TEST_EXPECT_EQ(ifaceNet->version, PLUGINXX_IFACE_NET_VERSION);

            // 表入口是异步操作契约: 提交后结果经完成回调交回 (client io 线程)
            struct WaitDone {
                std::mutex              mutex;
                bool                    done   = false;
                int32_t                 status = PLUGINXX_OPERATOR_FAILED;
                std::string             payload;
            };
            auto submitNet = [&](const char* opText, const char* argsText) {
                auto               state = std::make_shared<WaitDone>();
                PluginxxStringView opSv
                    = agentxx::plugin::PluginStringView::from(opText, std::strlen(opText));
                PluginxxStringView argsSv
                    = agentxx::plugin::PluginStringView::from(argsText, std::strlen(argsText));
                int64_t        requestId = 0;
                PluginxxString err{};
                auto*          handle   = ifaceNet->call(
                    inst->hostView(),
                    &opSv,
                    &argsSv,
                    &requestId,
                    [](void* ud, int32_t status, const PluginxxStringView* payload) {
                        auto* s = static_cast<WaitDone*>(ud);
                        std::lock_guard<std::mutex> lk(s->mutex);
                        s->done   = true;
                        s->status = status;
                        if (payload && payload->data) {
                            s->payload.assign(payload->data, static_cast<size_t>(payload->size));
                        }
                    },
                    state.get(),
                    &err
                );
                if (handle == nullptr) {
                    std::lock_guard<std::mutex> lk(state->mutex);
                    state->done = true;
                    if (err.data) {
                        state->payload.assign(err.data, static_cast<size_t>(err.size));
                        agentxx::plugin::PluginString::free(inst->hostView(), &err);
                    }
                }
                return state;
            };
            auto waitNet = [&](const std::shared_ptr<WaitDone>& state) -> asio::awaitable<bool> {
                for (int i = 0; i < 400; ++i) {
                    {
                        std::lock_guard<std::mutex> lk(state->mutex);
                        if (state->done) {
                            co_return true;
                        }
                    }
                    co_await sleepMs(5);
                }
                co_return false;
            };

            // net.info: 服务/连接快照, 空闲时也是合法结果
            {
                auto state = submitNet("net.info", "{}");
                XX_TEST_EXPECT_TRUE(co_await waitNet(state));
                XX_TEST_EXPECT_EQ(state->status, PLUGINXX_OPERATOR_OK);
                XX_TEST_EXPECT_TRUE(state->payload.find("\"ok\":true") != std::string::npos);
            }

            // 未知 op: 参数层拒绝 (不是"业务失败"), 返回 NULL + 原因
            {
                PluginxxStringView opSv
                    = agentxx::plugin::PluginStringView::from("net.no_such_op", 15);
                PluginxxStringView argsSv = agentxx::plugin::PluginStringView::from("{}", 2);
                PluginxxString     err{};
                int64_t            requestId = 0;
                auto*              handle    = ifaceNet->call(
                    inst->hostView(),
                    &opSv,
                    &argsSv,
                    &requestId,
                    nullptr,
                    nullptr,
                    &err
                );
                XX_TEST_EXPECT_TRUE(handle == nullptr);
                if (err.data) {
                    agentxx::plugin::PluginString::free(inst->hostView(), &err);
                }
            }

            // cancel 不存在的请求: 参数层给出 NOT_FOUND
            {
                PluginxxString out{};
                XX_TEST_EXPECT_EQ(ifaceNet->cancel(inst->hostView(), 999999, &out),
                                  PLUGINXX_RESULT_ERR_NOT_FOUND);
                if (out.data) {
                    agentxx::plugin::PluginString::free(inst->hostView(), &out);
                }
            }
        }
    }

    // ---- 2.5 接口表: agentxx.client.self get_language / set_language ----
    {
        auto ifaceSelf = agentxx::plugin::queryInterface<AgentxxClientSelfIface>(
            inst->hostView(),
            AGENTXX_IFACE_CLIENT_SELF
        );
        XX_TEST_EXPECT_TRUE(ifaceSelf != nullptr);
        if (ifaceSelf) {
            XX_TEST_EXPECT_EQ(ifaceSelf->version, AGENTXX_IFACE_CLIENT_SELF_VERSION);
            auto host  = inst->hostView();
            auto getLanguage = [&]() {
                return clientCall(host, [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                    return ifaceSelf->get_language(host, cb, ud, err);
                });
            };            auto setLanguage = [&](std::string_view lang) {
                auto sv = agentxx::plugin::PluginStringView::from(lang.data(), lang.size());
                return clientCall(host, [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                    return ifaceSelf->set_language(host, &sv, cb, ud, err);
                });
            };
            // 默认语言为 "en"
            {
                const auto lang = getLanguage();
                XX_TEST_EXPECT_TRUE(lang.ok());
                XX_TEST_EXPECT_EQ(lang.payload, std::string("en"));
            }
            // 设置语言为 "zh-cn"
            {
                XX_TEST_EXPECT_TRUE(setLanguage("zh-cn").ok());
                const auto lang = getLanguage();
                XX_TEST_EXPECT_TRUE(lang.ok());
                XX_TEST_EXPECT_EQ(lang.payload, std::string("zh-cn"));
            }
            // 不支持 auto, 传 auto 回退为 en
            {
                XX_TEST_EXPECT_TRUE(setLanguage("auto").ok());
                const auto lang = getLanguage();
                XX_TEST_EXPECT_TRUE(lang.ok());
                XX_TEST_EXPECT_EQ(lang.payload, std::string("en"));
            }
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
    //        插件事件分发完成后主程序上报 client_interfaces (三期6, 镜像
    //        server_plugins —— agent 侧插件据此知道 client 接口集)
    mgr->onReady();
    // 插件 READY 处理器在插件执行线程上跑, 主程序的接口集上报在 io 线程先发出:
    // 两条上行都会到, 但先后不固定, 所以按主题查找而不看最后一条
    XX_TEST_EXPECT_TRUE(co_await waitUntil([&] { return adapter->dataUpCount() >= 2; }));
    XX_TEST_EXPECT_TRUE(adapter->hasDataTopic("agentxx.client.host.client_interfaces"));
    XX_TEST_EXPECT_TRUE(adapter->hasDataTopic("agentxx.client.plugin.example_plugin.hello"));

    // TURN_END: 状态栏项文本更新 (turns: 1)
    {
        agentxx::agent::WireTurnResult r;
        r.sessionId = "sess-test";
        r.hasError  = false;
        mgr->onTurnResult(r);
    }
    // TURN_END 的处理在插件执行线程上: 等这次回调整段跑完再断言
    // (状态栏项与 Info 段落同属一次回调, 只等其中一个会漏)
    co_await drainPlugin(mgr, "example_plugin");
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
                    XX_TEST_EXPECT_EQ(first.value("kind", std::string{}), "Text");
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
        d.topic = "plugin.example_plugin.progress";
        d.data  = R"({"done":1})";
        mgr->onPluginData(d);
    }
    XX_TEST_EXPECT_TRUE(co_await waitUntil([&] { return adapter->panelUpdated() >= 1; }));

    // ---- 4. 命令执行 ----
    // /example (send 动作)
    mgr->invokeCommand("example", R"({"text":"hi"})");
    XX_TEST_EXPECT_TRUE(co_await waitUntil([&] { return adapter->sendCount() >= 1; }));
    XX_TEST_EXPECT_EQ(adapter->sendCount(), 1);
    XX_TEST_EXPECT_TRUE(adapter->lastSent().find("Hello from example plugin") != std::string::npos);
    XX_TEST_EXPECT_TRUE(adapter->lastSent().find("(hi)") != std::string::npos);

    // example_toast (toast 动作)
    mgr->invokeCommand("example_toast", R"({"text":"notice"})");
    XX_TEST_EXPECT_TRUE(co_await waitUntil([&] { return adapter->toastCount() >= 1; }));
    XX_TEST_EXPECT_EQ(adapter->toastCount(), 1);
    XX_TEST_EXPECT_EQ(adapter->lastToast(), "notice");
    XX_TEST_EXPECT_EQ(adapter->lastToastLvl(), 1);

    // 未注册命令: 记日志不崩溃
    mgr->invokeCommand("no_such_command", "{}");

    // ---- 5. 跨端数据 (agentxx.client.wire 接口表 send_plugin_data 路径) ----
    {
        const auto wire = agentxx::plugin::ClientIfaces::query(inst->hostView()).wire;
        XX_TEST_EXPECT_TRUE(wire != nullptr && wire->send_plugin_data != nullptr);
        auto host = inst->hostView();
        // 主题写全名: `agentxx.client.plugin.<本实例插件名>.<事件>`
        auto sendData = [&](std::string_view topic, std::string_view json) {
            auto topicSv = agentxx::plugin::PluginStringView::from(topic.data(), topic.size());
            auto jsonSv  = agentxx::plugin::PluginStringView::from(json.data(), json.size());
            return clientCall(host, [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                return wire->send_plugin_data(host, &topicSv, &jsonSv, cb, ud, err);
            });
        };
        const auto sent = sendData("agentxx.client.plugin.example_plugin.rebuild", R"({"x":1})");
        XX_TEST_EXPECT_TRUE(sent.ok());

        // 不在自己那一族里的主题被拒 (框架只校验不补前缀)
        // - 形状合法但主题不在自己那一族: 受理后按失败交回 (主程序记警告)
        const auto rejected = sendData("rebuild", "{}");
        XX_TEST_EXPECT_FALSE(rejected.ok());
    }
    XX_TEST_EXPECT_TRUE(adapter->dataUpCount() >= 2);
    XX_TEST_EXPECT_EQ(adapter->lastDataTopic(), "agentxx.client.plugin.example_plugin.rebuild");

    // ---- 6. disable / enable ----
    co_await mgr->disable("example_plugin");
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

    co_await mgr->enable("example_plugin");
    // 启用走 client start 事务 (client io 线程异步执行): 等待 UI 注册恢复
    for (int i = 0; i < 200 && !mgr->hasCommand("example"); ++i) {
        co_await sleepMs(5);
    }
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

    // ---- 广播频道 (客户端事件的唯一通道) ----
    // - 状态类 / 事件类频道在管理器构造时声明好, 插件在 start 里就能订阅;
    // - 跨进程频道 (`plugin.<插件名>.*` / `agentxx.client.plugin.<插件名>.*`)
    //   本进程常常还没人声明, 订阅入口先补一条声明;
    // - 卸载时框架按归属撤销订阅 (句柄由实例保活, 裸指针不悬垂)。
    {
        auto registry = mgr->broadcasts();
        XX_TEST_EXPECT_TRUE(registry != nullptr);
        if (registry != nullptr) {
            for (const auto id : {
                     "agentxx.client.ready",
                     "agentxx.client.connection_state",
                     "agentxx.client.user_input",
                     "agentxx.client.delta",
                     "agentxx.client.turn_end",
                     "agentxx.client.session_switch",
                     "agentxx.client.ui_layout",
                 }) {
                XX_TEST_EXPECT_TRUE(registry->declares(id));
            }
            // 状态类频道保留当前值 (插件上线能读到最新状态), 事件类不缓存
            const auto readyDecl = registry->declOf("agentxx.client.ready");
            XX_TEST_EXPECT_TRUE(
                readyDecl != nullptr
                && readyDecl->cache == pluginxx::BroadcastCache_e::Latest
            );
            const auto deltaDecl = registry->declOf("agentxx.client.delta");
            XX_TEST_EXPECT_TRUE(
                deltaDecl != nullptr && deltaDecl->cache == pluginxx::BroadcastCache_e::None
            );
        }

        auto inst2 = co_await mgr->loadNativeAsync(path);
        XX_TEST_EXPECT_TRUE(inst2 != nullptr);
        if (!inst2) {
            co_return TestResult{g_client_plugin_passed, g_client_plugin_failed};
        }

        // 8.1 一次发布到达多个订阅; 逐个退订后不再到达
        std::atomic<int>      hits{0};
        PluginxxSubscription* subs[3] = {};
        auto                  subFn   = +[](const PluginxxStringView*, void* ud) {
            ++(*static_cast<std::atomic<int>*>(ud));
        };
        const auto broadcasts8
            = agentxx::plugin::ClientIfaces::query(inst2->hostView()).broadcasts;
        XX_TEST_EXPECT_TRUE(
            broadcasts8 != nullptr && broadcasts8->subscribe_broadcast != nullptr
        );
        for (int i = 0; i < 3; ++i) {
            subs[i] = broadcasts8
                          ? agentxx::test::subscribeBroadcastWait(
                                inst2->hostView(),
                                broadcasts8,
                                nullptr,
                                agentxx::plugin::broadcasts::ClientConnectionState,
                                subFn,
                                &hits
                            )
                          : nullptr;
            XX_TEST_EXPECT_TRUE(subs[i] != nullptr);
        }
        mgr->onConnStateChanged("connected", "100%");
        co_await drainPlugin(mgr, "example_plugin");
        XX_TEST_EXPECT_EQ(hits.load(), 3);
        for (auto* sub : subs) {
            if (broadcasts8 && sub) {
                XX_TEST_EXPECT_TRUE(
                    agentxx::plugin::unsubscribePluginBroadcastNoWait(
                        inst2->hostView(),
                        broadcasts8,
                        sub
                    )
                );
            }
        }
        mgr->onConnStateChanged("connected", "100%");
        co_await drainPlugin(mgr, "example_plugin");
        XX_TEST_EXPECT_EQ(hits.load(), 3); ///< 全部退订后不再到达

        // 8.2 未声明的跨进程频道: 订阅入口先补声明 (否则跨端事件永远收不到)
        {
            constexpr std::string_view kTopic = "plugin.agentxx_other.telemetry";
            XX_TEST_EXPECT_FALSE(mgr->broadcasts()->declares(kTopic));
            auto sub = agentxx::test::subscribeBroadcastWait(
                inst2->hostView(),
                broadcasts8,
                nullptr,
                kTopic,
                subFn,
                &hits
            );
            XX_TEST_EXPECT_TRUE(sub != nullptr);
            XX_TEST_EXPECT_TRUE(mgr->broadcasts()->declares(kTopic));
            const auto decl = mgr->broadcasts()->declOf(kTopic);
            // 属主按名字推断 (按实例回收频道用), 发布权与插件自己声明的默认值一致
            // ("任何人可发", 几个插件可以共写一条频道)
            XX_TEST_EXPECT_TRUE(
                decl != nullptr && decl->owner == "agentxx_other"
                && decl->publish == pluginxx::BroadcastPublisher_e::Anyone
            );
            // 主程序代发 (发布者 = 应用) → 订阅收到
            mgr->publishClientChannelValue(kTopic, R"({"n":1})");
            co_await drainPlugin(mgr, "example_plugin");
            XX_TEST_EXPECT_EQ(hits.load(), 4);
            XX_TEST_EXPECT_TRUE(
                agentxx::plugin::unsubscribePluginBroadcastNoWait(inst2->hostView(), broadcasts8, sub)
            );
        }

        // 8.3 收尾: 卸载时订阅被撤销, 之后再发布不再进入插件代码
        const int beforeUnload = hits.load();
        bool      unloaded2    = co_await mgr->unloadAsync("example_plugin");
        XX_TEST_EXPECT_TRUE(unloaded2);
        XX_TEST_EXPECT_TRUE(mgr->find("example_plugin") == nullptr);
        mgr->onConnStateChanged("connected", "100%");
        co_await sleepMs(50); ///< 实例已卸载: 没有可派的回调, 只给误派留时间
        XX_TEST_EXPECT_EQ(hits.load(), beforeUnload);
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
        pc.args    = utilxx_base::Json{
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
            const auto self9 = agentxx::plugin::ClientIfaces::query(instCfg->hostView()).self;
            XX_TEST_EXPECT_TRUE(self9 != nullptr && self9->get_plugin_args != nullptr);
            if (self9 && self9->get_plugin_args) {
                const auto args = clientCall(
                    instCfg->hostView(),
                    [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                        return self9->get_plugin_args(instCfg->hostView(), cb, ud, err);
                    }
                );
                XX_TEST_EXPECT_TRUE(args.ok());
                try {
                    auto j = utilxx_base::Json::parse(args.payload);
                    XX_TEST_EXPECT_EQ(j["client_key"].get<std::string>(), "client_val");
                } catch (const std::exception& e) {
                    XX_TEST_EXPECT_TRUE(false);
                    XX_LOGE("[client_plugin] 9.2 args json parse failed: {}", e.what());
                }
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

    // ---- 10. 主程序约定事件 (server_plugins) + 对端缺失可观测性 ----
    // - server_plugins (结构化载荷) → 记录服务端已加载插件列表, get_client_state
    //   以 "agentPlugins" 暴露 [{name,version,interfaces},...] (client 插件
    //   判断对端可用性/能力的正式通道)
    // - 无订阅者提示只看插件族主题 (每插件名一次): 主程序约定事件
    //   (`agentxx.agent.host.*`) 不属于任何插件, 不得报警 (TUI 启动时
    //   client_attached 每次都会到, 报警是误报)
    {
        // 捕获日志后断言: 约定事件无警告, 插件族主题恰好一次
        agentxx::util::enableLogCapture(256);
        agentxx::util::clearCapturedLogs();

        agentxx::agent::WirePluginData d;
        d.topic = "agentxx.agent.host.server_plugins";
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
        bad.topic = "agentxx.agent.host.server_plugins";
        bad.data   = "not-json";
        mgr->onPluginData(bad);
        auto stateJson2 = mgr->clientStateJson();
        XX_TEST_EXPECT_TRUE(stateJson2.find("agentxx_codegraph") != std::string::npos);

        // 约定事件 client_attached / 无订阅方的 server_plugins: 只分发, 不报警
        agentxx::agent::WirePluginData attached;
        attached.topic = "agentxx.agent.host.client_attached";
        attached.data  = R"({"sessionId":"session"})";
        mgr->onPluginData(attached);
        mgr->onPluginData(d);

        // 插件族主题无订阅者 (当前无任何已加载插件): 安全, 同一插件名只警告一次
        agentxx::agent::WirePluginData orphan;
        orphan.topic = "plugin.some_plugin.progress";
        orphan.data   = R"({})";
        mgr->onPluginData(orphan);
        mgr->onPluginData(orphan);

        size_t hostWarn = 0;
        size_t orphanWarn = 0;
        for (const auto& line : agentxx::util::recentLogLines()) {
            if (line.find("no client plugin subscribed") == std::string::npos) {
                continue;
            }
            if (line.find("agentxx.agent.host.") != std::string::npos) {
                ++hostWarn;
            }
            if (line.find("plugin.some_plugin.progress") != std::string::npos) {
                ++orphanWarn;
            }
        }
        XX_TEST_EXPECT_EQ(hostWarn, size_t{0});
        XX_TEST_EXPECT_EQ(orphanWarn, size_t{1});
        agentxx::util::clearCapturedLogs();
        agentxx::util::disableLogCapture();
    }

    // ---- 11. 接口协商: 主程序支持集 / require 限制 / 发现通道 ----
    // - 限制双道生效: dlopen 前跳过 (loadConfiguredClientPlugins) 与
    //   dlopen 后直连路径 (loadNativeAsync); 此处覆盖后者 + 发现通道
    {
        namespace fs = std::filesystem;

        // 11.1 clientStateJson 暴露主程序接口清单 (Mock 适配器: status_item/
        //      panel/toast/info_section/command)
        auto stateJson = mgr->clientStateJson();
        XX_TEST_EXPECT_TRUE(stateJson.find("\"interfaces\"") != std::string::npos);
        XX_TEST_EXPECT_TRUE(stateJson.find("agentxx.client.panel") != std::string::npos);
        XX_TEST_EXPECT_TRUE(stateJson.find("agentxx.client.command") != std::string::npos);
        XX_TEST_EXPECT_TRUE(stateJson.find("agentxx.client.status_item") != std::string::npos);
        // 未置位的能力不得出现 (prompt_modal 仍未实现, 预留位不置)
        XX_TEST_EXPECT_FALSE(stateJson.find("agentxx.client.prompt_modal") != std::string::npos);
        // 本次实现的能力 (定时器/快捷键) 由 Mock 适配器声明, 应出现在清单里
        // (接口名就是表 IID: 下划线形式 `<应用名>_client_<表名>`)
        XX_TEST_EXPECT_TRUE(stateJson.find("agentxx_client_timer") != std::string::npos);
        XX_TEST_EXPECT_TRUE(stateJson.find("agentxx_client_keybind") != std::string::npos);

        // 11.1b 区域尺寸快照: 上报后进入 client_state.regions, 值未变化不重复记录
        XX_TEST_EXPECT_TRUE(mgr->regionSizes().empty());
        mgr->reportRegionSize("agentxx_test.panel", 60, 12);
        mgr->reportRegionSize("agentxx_test.panel", 60, 12); // 重复上报: 无变化
        auto sizes = mgr->regionSizes();
        XX_TEST_EXPECT_EQ(sizes.size(), size_t{1});
        if (!sizes.empty()) {
            XX_TEST_EXPECT_EQ(sizes[0].id, std::string{"agentxx_test.panel"});
            XX_TEST_EXPECT_EQ(sizes[0].width, 60);
            XX_TEST_EXPECT_EQ(sizes[0].height, 12);
        }
        // 尺寸变化后覆盖同一 id (不新增条目)
        mgr->reportRegionSize("agentxx_test.panel", 80, 20);
        sizes = mgr->regionSizes();
        XX_TEST_EXPECT_EQ(sizes.size(), size_t{1});
        if (!sizes.empty()) {
            XX_TEST_EXPECT_EQ(sizes[0].width, 80);
            XX_TEST_EXPECT_EQ(sizes[0].height, 20);
        }
        // 非法上报 (空 id / 非正宽度) 被忽略
        mgr->reportRegionSize("", 10, 10);
        mgr->reportRegionSize("agentxx_test.panel", 0, 10);
        XX_TEST_EXPECT_EQ(mgr->regionSizes().size(), size_t{1});
        XX_TEST_EXPECT_TRUE(mgr->clientStateJson().find("\"regions\"") != std::string::npos);
        XX_TEST_EXPECT_TRUE(mgr->clientStateJson().find("agentxx_test.panel") != std::string::npos);

        // 11.2 require 未满足 → 加载跳过并记录原因 (直连路径, dlopen 后限制):
        // 拷贝真实可加载的示例库, manifest 声明本主程序不支持的必选接口
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

            // 11.3 同一插件声明全部可满足 → 正常加载 (限制放行回归) +
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

                // READY 频道载荷含 interfaces 数组 (频道值由 onReady 发布到
                // `agentxx.client.ready`; onReady 同步分发到当前 io 线程)
                std::string readyPayload;
                auto        readyFn = +[](const PluginxxStringView* payload, void* ud) {
                    if (payload && payload->data) {
                        static_cast<std::string*>(ud)->assign(payload->data, payload->size);
                    }
                };
                const auto broadcasts11
                    = agentxx::plugin::ClientIfaces::query(okInst->hostView()).broadcasts;
                auto sub = broadcasts11
                               ? agentxx::test::subscribeBroadcastWait(
                                     okInst->hostView(),
                                     broadcasts11,
                                     nullptr,
                                     agentxx::plugin::broadcasts::ClientReady,
                                     readyFn,
                                     &readyPayload
                                 )
                               : nullptr;
                XX_TEST_EXPECT_TRUE(sub != nullptr);
                mgr->onReady();
                co_await drainPlugin(mgr, "example_plugin");
                XX_TEST_EXPECT_TRUE(readyPayload.find("\"interfaces\"") != std::string::npos);
                XX_TEST_EXPECT_TRUE(readyPayload.find("agentxx.client.panel") != std::string::npos);
                if (broadcasts11 && sub) {
                    XX_TEST_EXPECT_TRUE(
                        agentxx::plugin::unsubscribePluginBroadcastNoWait(
                            okInst->hostView(),
                            broadcasts11,
                            sub
                        )
                    );
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
            st.topic = "plugin.agentxx_codegraph.status";
            st.data   = R"({"loaded":true,"project_root":"/workspace/test_proj"})";
            mgr->onPluginData(st);
            co_await drainPlugin(mgr, "agentxx_codegraph");

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
            prog.topic = "plugin.agentxx_codegraph.progress";
            prog.data   = R"({"processed":25,"total":100,"current_file":"src/main.cpp"})";
            mgr->onPluginData(prog);
            co_await drainPlugin(mgr, "agentxx_codegraph");

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
                    XX_TEST_EXPECT_TRUE(dump.find("Progress") != std::string::npos);
                }
            }

            // progress 事件 (完成): 新版为 “|- available · 100”
            agentxx::agent::WirePluginData progDone;
            progDone.topic = "plugin.agentxx_codegraph.progress";
            progDone.data   = R"({"processed":100,"total":100,"current_file":""})";
            mgr->onPluginData(progDone);
            co_await drainPlugin(mgr, "agentxx_codegraph");

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
    // 插件平台支持情况: windows/linux/android/macos 有真实实现 (iOS 无产物), 其余跳过
#if XX_IS_WIN_D || XX_IS_LINUX_D || XX_IS_ANDROID_D || XX_IS_MACOS_D
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

            // usage 事件: Append 段风格为 “|- CPU 36% · 12x” / “|- RAM 52% · 8G/16G” / “|- GPU 42%”
            agentxx::agent::WirePluginData usage;
            usage.topic = "plugin.agentxx_system_monitor.usage";
            usage.data
                = R"({"cpu":35.5,"cpu_cores":12,"mem_percent":52.0,"mem_used_mb":8192,"mem_total_mb":16384,"gpus":[{"name":"NVIDIA RTX 4090","dedicated_vram_mb":24576,"dedicated_vram_used_mb":6144,"usage_percent":42.0}]})";
            mgr->onPluginData(usage);
            co_await drainPlugin(mgr, "agentxx_system_monitor");

            reg = mgr->uiRegistrySnapshot();
            for (const auto& sec : reg->infoSections) {
                if (sec.id == "agentxx_system_monitor.usage") {
                    std::string dump = sec.items.dump();
                    // CPU 段首个 item 的完整文案: “|- CPU 36% · 12x”
                    // (核数 12 -> “12x”, 利用率四舍五入到整数 35.5 -> 36)
                    XX_TEST_EXPECT_TRUE(sec.items.is_array() && !sec.items.empty());
                    if (sec.items.is_array() && !sec.items.empty()) {
                        XX_TEST_EXPECT_EQ(
                            sec.items[0].value("text", std::string{}),
                            std::string{"|- CPU 36% · 12x"}
                        );
                    }
                    XX_TEST_EXPECT_TRUE(dump.find("CPU") != std::string::npos);
                    XX_TEST_EXPECT_TRUE(dump.find("12x") != std::string::npos);
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

            // 旧版本 agent 侧未上报 cpu_cores (取值 0): 回退为不带核数的 “|- CPU 71%”
            usage.data
                = R"({"cpu":71.2,"mem_percent":52.0,"mem_used_mb":8192,"mem_total_mb":16384})";
            mgr->onPluginData(usage);
            co_await drainPlugin(mgr, "agentxx_system_monitor");
            reg = mgr->uiRegistrySnapshot();
            for (const auto& sec : reg->infoSections) {
                if (sec.id == "agentxx_system_monitor.usage") {
                    std::string dump = sec.items.dump();
                    XX_TEST_EXPECT_TRUE(dump.find("CPU 71%") != std::string::npos);
                }
            }

            // sysinfo 命令执行: 新版切换文案为 “System resource info: ON/OFF”
            XX_TEST_EXPECT_TRUE(mgr->hasCommand("sysinfo"));
            auto toastBefore = adapter->toastCount();
            mgr->invokeCommand("sysinfo", R"({})");
            co_await drainPlugin(mgr, "agentxx_system_monitor");
            // 命令回调是异步操作契约: 插件回传内容前可能还要等一次表调用
            // (查询服务端插件清单), 因此这里等到 toast 出现, 不假设它已就绪
            XX_TEST_EXPECT_TRUE(
                co_await waitUntil([&] { return adapter->toastCount() > toastBefore; }, 5000)
            );
            XX_TEST_EXPECT_TRUE(
                adapter->lastToast().find("System resource info") != std::string::npos
            );

            co_await mgr->unloadAsync("agentxx_system_monitor");
            XX_TEST_EXPECT_TRUE(mgr->find("agentxx_system_monitor") == nullptr);
        }
    }
#endif // XX_IS_WIN_D || XX_IS_LINUX_D || XX_IS_ANDROID_D || XX_IS_MACOS_D

    // ---- 13b. agentxx_chatbot client 插件: 侧边栏「机器人」栏 ----
    //  面板数据只来自 agent 侧发布的快照 (topic agentxx_chatbot.panel 经
    //  WirePluginData 下发), 所以这里直接投一条快照事件断言渲染结果。
    {
        auto cbPath = findPluginPath("agentxx_chatbot");
        if (!cbPath.empty()) {
            auto cbInst = co_await mgr->loadNativeAsync(cbPath);
            XX_TEST_EXPECT_TRUE(cbInst != nullptr);
            if (cbInst) {
                // 面板注册: id / 标题
                auto reg      = mgr->uiRegistrySnapshot();
                bool hasPanel = false;
                utilxx_base::Json placeholder = utilxx_base::Json::array();
                for (const auto& p : reg->panels) {
                    if (p.id == "agentxx_chatbot.bots") {
                        hasPanel    = true;
                        placeholder = p.items;
                        XX_TEST_EXPECT_EQ(p.title, std::string{"机器人"});
                    }
                }
                XX_TEST_EXPECT_TRUE(hasPanel);
                // 还没收到快照: 一行提示 (不留空白栏)
                XX_TEST_EXPECT_TRUE(
                    placeholder.dump().find("等待服务端数据") != std::string::npos
                );

                // 快照事件: 一个在线账号 + 一个人设 + 一条正在执行且排队 3 条的群会话
                const auto snapshot = std::string{
                    R"({"v":1,"tick":7,"startupTime":"2026-10-29 20:00:00",)"
                    R"("accounts":[{"id":"qq_main","type":"qq","state":"online"}],)"
                    R"("personas":[{"name":"assistant","model":"gpt-x","promptHead":"你是 QQ 群里的助手","rootId":"root-1","mounted":true,"chats":2,"budget":64,"binds":3}],)"
                    R"("sessions":[{"key":"qq_main|group|G1","title":"技术群","persona":"assistant","account":"qq_main","type":"group","mode":"always","channelId":7,"ready":true,"running":true,"queued":3,"turns":4,"sent":6}],)"
                    R"("stats":{"received":12,"sent":5,"failed":1}})"
                };
                agentxx::agent::WirePluginData panelData;
                panelData.topic = "plugin.agentxx_chatbot.panel";
                panelData.data   = snapshot;
                mgr->onPluginData(panelData);
                co_await drainPlugin(mgr, "agentxx_chatbot");

                reg = mgr->uiRegistrySnapshot();
                bool checkedPanel = false;
                for (const auto& p : reg->panels) {
                    if (p.id != "agentxx_chatbot.bots") {
                        continue;
                    }
                    checkedPanel         = true;
                    const std::string dump = p.items.dump();
                    // 段标题 `标题: 数量` + 列表项 `|- 内容`, 段之间是一行空白
                    XX_TEST_EXPECT_TRUE(dump.find("会话: 1") != std::string::npos);
                    XX_TEST_EXPECT_TRUE(dump.find("|- ") != std::string::npos);
                    XX_TEST_EXPECT_TRUE(dump.find("账号: 1") != std::string::npos);
                    XX_TEST_EXPECT_TRUE(dump.find("Gap") != std::string::npos);
                    XX_TEST_EXPECT_TRUE(dump.find("Divider") == std::string::npos);
                    // 副标题/说明等减淡内容走 hint 色 (与 `|- ` 前缀一致, 不再叠加 dim)
                    XX_TEST_EXPECT_TRUE(dump.find("\"color\":\"hint\"") != std::string::npos);
                    // 会话行: 标题 + 副标题 + 会话 id + 左侧加载帧 (tick 7 -> 第 8 帧 ⠧)
                    XX_TEST_EXPECT_TRUE(dump.find("技术群") != std::string::npos);
                    XX_TEST_EXPECT_TRUE(dump.find("assistant · 群聊 · always") != std::string::npos);
                    // 会话 id 单独一行 (从 chatKey 第三段取, 用户要抄进 bind.chats[].id)
                    XX_TEST_EXPECT_TRUE(dump.find("\"text\":\"id G1\"") != std::string::npos);
                    XX_TEST_EXPECT_TRUE(dump.find("⠧") != std::string::npos);
                    // 右侧排队条数
                    XX_TEST_EXPECT_TRUE(dump.find("\"text\":\"3\"") != std::string::npos);
                    // 人设段
                    XX_TEST_EXPECT_TRUE(dump.find("会话 2/64 · 绑定 3") != std::string::npos);
                    XX_TEST_EXPECT_TRUE(dump.find("gpt-x") != std::string::npos);
                    XX_TEST_EXPECT_TRUE(dump.find("你是 QQ 群里的助手") != std::string::npos);
                    // 底部: 账号状态与收发计数
                    XX_TEST_EXPECT_TRUE(dump.find("qq_main online") != std::string::npos);
                    XX_TEST_EXPECT_TRUE(dump.find("失败 1") != std::string::npos);
                }
                XX_TEST_EXPECT_TRUE(checkedPanel);

                // 坏载荷不能清掉已有内容 (面板停在上一份快照)
                agentxx::agent::WirePluginData broken;
                broken.topic = "plugin.agentxx_chatbot.panel";
                broken.data   = "not-json";
                mgr->onPluginData(broken);
                co_await drainPlugin(mgr, "agentxx_chatbot");
                reg = mgr->uiRegistrySnapshot();
                for (const auto& p : reg->panels) {
                    if (p.id == "agentxx_chatbot.bots") {
                        XX_TEST_EXPECT_TRUE(p.items.dump().find("技术群") != std::string::npos);
                    }
                }

                // 连接就绪: 插件向上要一份快照 (send_plugin_data("refresh"))
                // 注: 用连接状态事件而不是 onReady —— onReady 末尾主程序自己还会发
                // client_interfaces, 会把 adapter 记的"最后一条"盖掉
                const int upBefore = adapter->dataUpCount();
                mgr->onConnStateChanged("connected", "100%");
                co_await drainPlugin(mgr, "agentxx_chatbot");
                XX_TEST_EXPECT_TRUE(adapter->dataUpCount() > upBefore);
                XX_TEST_EXPECT_EQ(
                    adapter->lastDataTopic(),
                    std::string{"agentxx.client.plugin.agentxx_chatbot.refresh"}
                );

                // 卸载: 面板随之撤销
                co_await mgr->unloadAsync("agentxx_chatbot");
                XX_TEST_EXPECT_TRUE(mgr->find("agentxx_chatbot") == nullptr);
                reg = mgr->uiRegistrySnapshot();
                for (const auto& p : reg->panels) {
                    XX_TEST_EXPECT_TRUE(p.id != "agentxx_chatbot.bots");
                }
            }
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
            plData.topic = "plugin.agentxx_planning.planning";
            plData.data
                = R"({"roadmap":"stateDiagram-v2\n[*] --> step1\nstep1 --> [*]","todos":[{"state":"in_progress","content":"do step 1","summary":"doing step 1"},{"state":"completed","content":"done step 0"}],"notes":"test note 123"})";
            mgr->onPluginData(plData);
            co_await drainPlugin(mgr, "agentxx_planning");

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
                    // 验证 "|- " 前缀 + Graph 按钮的布局契约: 前缀文本与按钮是**相邻两项**
                    // (客户端把相邻的 Text + Button 渲染在同一行, 按钮紧跟文本之后),
                    // 动作是 dispatch (描述层规范写法)
                    XX_TEST_EXPECT_TRUE(sec.items.is_array());
                    XX_TEST_EXPECT_TRUE(sec.items.size() >= 3);
                    const auto& graphPrefix = sec.items[0];
                    XX_TEST_EXPECT_EQ(graphPrefix.value("kind", ""), std::string{"Text"});
                    XX_TEST_EXPECT_EQ(graphPrefix.value("text", ""), std::string{"|- "});
                    const auto& graphButton = sec.items[1];
                    XX_TEST_EXPECT_EQ(graphButton.value("kind", ""), std::string{"Button"});
                    XX_TEST_EXPECT_EQ(graphButton.value("label", ""), std::string{"Graph"});
                    // 前缀与按钮之间不夹别的项, 否则客户端不会合并到同一行
                    XX_TEST_EXPECT_TRUE(sec.items[2].is_object());
                    XX_TEST_EXPECT_EQ(sec.items[2].value("kind", ""), std::string{"Text"});
                    // 动作按描述层解析: 短写字符串与对象写法等价 (dispatch + 名字)
                    const auto graphAction = pluginxx::ui::parseAction(graphButton["action"]);
                    XX_TEST_EXPECT_EQ(graphAction.kind, pluginxx::ui::Action::Kind::Call);
                    XX_TEST_EXPECT_EQ(graphAction.name, std::string{"planning.open_graph"});
                    // 状态图源码经 dispatch 动作派发时由插件自持 (不随按钮双发)
                }
            }
            XX_TEST_EXPECT_TRUE(hasPlanSection);

            // 工具行构造: 与 agent 端下发的一致 (insert_message 未完成 / update_message 已完成)
            auto toolRowDelta
                = [](std::string callId, std::string args, std::string result, bool finished) {
                      auto row = agentxx::agent::ViewMessage::makeText(
                          agentxx::agent::ViewMessage::Role::Tool,
                          std::move(args)
                      );
                      row.tool               = agentxx::agent::ViewMessage::ToolData{};
                      row.tool->toolName     = "agentxx_planning";
                      row.tool->toolCallId   = std::move(callId);
                      row.tool->toolResult   = std::move(result);
                      row.tool->toolFinished = finished;
                      row.collapsed          = true;
                      agentxx::agent::WireDelta d;
                      d.type    = finished ? agentxx::agent::WireDelta::Type::UpdateMessage
                                           : agentxx::agent::WireDelta::Type::InsertMessage;
                      d.msgId   = row.id;
                      d.message = std::make_shared<agentxx::agent::ViewMessage>(std::move(row));
                      return d;
                  };

            // 14.2 工具行插入 (write 模式) -> 工具装饰推送
            auto deltaWriteStart = toolRowDelta(
                "call_plan_write_1",
                R"({"mode":"write","roadmap":"stateDiagram-v2\n[*] --> p1\np1 --> [*]","todos":[{"state":"in_progress","content":"write task"}]})",
                "",
                false
            );
            mgr->onDelta(deltaWriteStart);
            co_await drainPlugin(mgr, "agentxx_planning");

            reg             = mgr->uiRegistrySnapshot();
            bool foundDecor = false;
            for (const auto& d : reg->toolDecors) {
                if (d.toolCallId == "call_plan_write_1") {
                    foundDecor = true;
                    XX_TEST_EXPECT_EQ(d.displayName, std::string{"Plan"});
                    XX_TEST_EXPECT_TRUE(d.summary.find("[~] write task") != std::string::npos);
                    std::string dump = d.items.dump();
                    XX_TEST_EXPECT_TRUE(dump.find("Diagram") != std::string::npos);
                    XX_TEST_EXPECT_TRUE(dump.find("write task") != std::string::npos);
                }
            }
            XX_TEST_EXPECT_TRUE(foundDecor);

            // 14.3 工具行插入 (read 模式) -> 占位装饰
            auto deltaReadStart = toolRowDelta("call_plan_read_1", R"({"mode":"read"})", "", false);
            mgr->onDelta(deltaReadStart);
            co_await drainPlugin(mgr, "agentxx_planning");

            reg = mgr->uiRegistrySnapshot();
            for (const auto& d : reg->toolDecors) {
                if (d.toolCallId == "call_plan_read_1") {
                    std::string dump = d.items.dump();
                    XX_TEST_EXPECT_TRUE(
                        dump.find("Reading saved planning...") != std::string::npos
                    );
                }
            }

            // 工具行更新 (read 模式完成) -> 结果装饰刷新
            auto deltaReadEnd = toolRowDelta(
                "call_plan_read_1",
                R"({"mode":"read"})",
                R"({"roadmap":"stateDiagram-v2\n[*] --> p2\np2 --> [*]","todos":[{"state":"completed","content":"read task done"}]})",
                true
            );
            mgr->onDelta(deltaReadEnd);
            co_await drainPlugin(mgr, "agentxx_planning");

            reg = mgr->uiRegistrySnapshot();
            for (const auto& d : reg->toolDecors) {
                if (d.toolCallId == "call_plan_read_1") {
                    std::string dump = d.items.dump();
                    XX_TEST_EXPECT_TRUE(dump.find("read task done") != std::string::npos);
                }
            }

            // 14.4 历史恢复路径 (重启/重连/切换会话后由 Sync 回放的 Tool 消息):
            // 没有任何 EVT_DELTA / update_tool_decor 推送, 渲染内容由 `tool` 槽位上的
            // 声明项 + 内容从消息自带的参数/结果推导 —— 改动前此类消息走通用渲染
            // (折叠头显示原始 toolName "agentxx_planning" + 参数/结果原文)
            {
                // 渲染器已按 tool_name 注册
                reg              = mgr->uiRegistrySnapshot();
                bool hasRenderer = false;
                for (const auto& r : reg->toolRenderers) {
                    if (r.plugin == "agentxx_planning" && r.toolName == "agentxx_planning") {
                        hasRenderer
                            = (r.collapsedFn != nullptr || r.expandedFn != nullptr);
                    }
                }
                XX_TEST_EXPECT_TRUE(hasRenderer);

                // 14.4.1 write 模式: 参数即规划内容 (历史消息只有参数, 无装饰)
                auto planWrite = co_await renderToolAsync(
                    mgr,
                    "call_plan_hist_write_1",
                    "agentxx_planning",
                    R"({"mode":"write","roadmap":"stateDiagram-v2\n[*] --> h1\nh1 --> [*]","todos":[{"state":"in_progress","content":"hist task"}],"notes":"hist note"})",
                    "success",
                    true,
                    false,
                    100
                );
                XX_TEST_EXPECT_TRUE(planWrite.matched);
                XX_TEST_EXPECT_EQ(planWrite.displayName, "Plan");
                XX_TEST_EXPECT_TRUE(planWrite.summary.find("[~] hist task") != std::string::npos);
                std::string writeItems = planWrite.items.dump();
                XX_TEST_EXPECT_TRUE(writeItems.find("Diagram") != std::string::npos);
                XX_TEST_EXPECT_TRUE(writeItems.find("[~] hist task") != std::string::npos);
                XX_TEST_EXPECT_TRUE(writeItems.find("hist note") != std::string::npos);

                // 14.4.2 read 模式: 结果即已保存的规划 JSON
                auto planRead = co_await renderToolAsync(
                    mgr,
                    "call_plan_hist_read_1",
                    "agentxx_planning",
                    R"({"mode":"read"})",
                    R"({"roadmap":"stateDiagram-v2\n[*] --> h2\nh2 --> [*]","todos":[{"state":"completed","content":"hist read task"}]})",
                    true,
                    false,
                    100
                );
                XX_TEST_EXPECT_TRUE(planRead.matched);
                XX_TEST_EXPECT_EQ(planRead.displayName, "Plan");
                XX_TEST_EXPECT_TRUE(
                    planRead.items.dump().find("hist read task") != std::string::npos
                );

                // 14.4.3 read 模式结果未返回: 占位提示
                auto planReading = co_await renderToolAsync(
                    mgr,
                    "call_plan_hist_read_2",
                    "agentxx_planning",
                    R"({"mode":"read"})",
                    "",
                    false,
                    false,
                    100
                );
                XX_TEST_EXPECT_FALSE(planReading.matched); ///< 占位内容只有 items, 没有折叠内容
                XX_TEST_EXPECT_TRUE(
                    planReading.items.dump().find("Reading saved planning...") != std::string::npos
                );

                // 14.4.4 参数不可解析: 展开体回退通用展示 (items 为空), 不产生错误内容
                auto planBadArgs = co_await renderToolAsync(
                    mgr,
                    "call_plan_hist_bad_1",
                    "agentxx_planning",
                    R"({"mode":"write","roadmap":)",
                    "",
                    true,
                    false,
                    100
                );
                XX_TEST_EXPECT_TRUE(planBadArgs.items.empty());

                // 渲染器路径不得产生运行时装饰 (历史消息没有装饰推送)
                reg                     = mgr->uiRegistrySnapshot();
                bool histDecorFromDelta = false;
                for (const auto& d : reg->toolDecors) {
                    if (d.toolCallId == "call_plan_hist_write_1"
                        || d.toolCallId == "call_plan_hist_read_1") {
                        histDecorFromDelta = true;
                    }
                }
                XX_TEST_EXPECT_FALSE(histDecorFromDelta);
            }

            // 14.5 会话切换 -> 清除装饰与段落
            mgr->onSessionSwitched("new_session_123");
            co_await drainPlugin(mgr, "agentxx_planning");
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

            // 15.1 list (折叠内容 = 路径)
            auto listRes = co_await renderToolAsync(
                mgr,
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
            auto readRes = co_await renderToolAsync(
                mgr,
                "call_read_1",
                "agentxx_filesystem_read",
                R"({"path":"/home/user/a.cpp","offset":10,"limit":50})",
                "",
                true,
                false,
                100
            );
            XX_TEST_EXPECT_TRUE(readRes.matched);
            XX_TEST_EXPECT_EQ(readRes.displayName, "Read");
            XX_TEST_EXPECT_TRUE(readRes.summary.find("[10, 60]") != std::string::npos);
            XX_TEST_EXPECT_TRUE(readRes.summary.find("/home/user/a.cpp") != std::string::npos);

            // 15.3 edit (回调函数: "[+行 -行] path" 摘要 + diff items)
            auto editRes = co_await renderToolAsync(
                mgr,
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
            // 折叠头摘要: 类似 git 的增删行数提示 (与展开体 diff 行数同源)
            XX_TEST_EXPECT_TRUE(editRes.summary.find("[+1 -1]") != std::string::npos);
            XX_TEST_EXPECT_TRUE(editRes.summary.find("/home/user/b.cpp") != std::string::npos);
            XX_TEST_EXPECT_EQ(editRes.items.size(), 1U);
            if (!editRes.items.empty()) {
                XX_TEST_EXPECT_EQ(editRes.items[0].value("kind", std::string{}), "Diff");
                XX_TEST_EXPECT_EQ(
                    editRes.items[0].value("path", std::string{}),
                    "/home/user/b.cpp"
                );
                XX_TEST_EXPECT_EQ(editRes.items[0].value("oldStr", std::string{}), "foo");
                XX_TEST_EXPECT_EQ(editRes.items[0].value("newStr", std::string{}), "bar");
            }

            // 15.3b edit: 增删行数按逐行 diff 统计 (未变的上下文行不计入)
            auto editCtx = co_await renderToolAsync(
                mgr,
                "call_edit_2",
                "agentxx_filesystem_edit",
                R"({"path":"/home/user/c.cpp","old_str":"int a = 1;\nreturn a;\n}","new_str":"int a = 2;\nreturn a;\n}"})",
                "success",
                true,
                false,
                100
            );
            XX_TEST_EXPECT_TRUE(editCtx.matched);
            XX_TEST_EXPECT_TRUE(
                editCtx.summary.find("[+1 -1] /home/user/c.cpp") != std::string::npos
            );

            // 15.3c edit: old_str/new_str 缺失时不加中括号提示 (仅路径)
            auto editNoText = co_await renderToolAsync(
                mgr,
                "call_edit_3",
                "agentxx_filesystem_edit",
                R"({"path":"/home/user/d.cpp"})",
                "success",
                true,
                false,
                100
            );
            XX_TEST_EXPECT_TRUE(editNoText.matched);
            XX_TEST_EXPECT_TRUE(editNoText.summary.find("[+0 -0]") == std::string::npos);
            XX_TEST_EXPECT_TRUE(editNoText.summary.find("/home/user/d.cpp") != std::string::npos);

            // 15.3d edit 单段: 每段各替换一处, 折叠头按逐行 diff 给出增删行数
            auto editSingle = co_await renderToolAsync(
                mgr,
                "call_edit_4",
                "agentxx_filesystem_edit",
                R"({"path":"/home/user/e.cpp","old_str":"oldCall();","new_str":"newCall();"})",
                "success",
                true,
                false,
                100
            );
            XX_TEST_EXPECT_TRUE(editSingle.matched);
            XX_TEST_EXPECT_TRUE(
                editSingle.summary.find("[+1 -1] /home/user/e.cpp") != std::string::npos
            );

            // 15.3f edit 多段替换 (`edits` 数组): 展开体每段一个 Diff 片段
            // (路径只在首段) + 段间分隔线; 折叠头各段增删行数求和
            auto editSegments = co_await renderToolAsync(
                mgr,
                "call_edit_6",
                "agentxx_filesystem_edit",
                R"({"path":"/home/user/g.cpp","edits":[{"old_str":"oldA();","new_str":"newA();"},{"old_str":"x();","new_str":"x2();\ny2();"}]})",
                "success",
                true,
                false,
                100
            );
            XX_TEST_EXPECT_TRUE(editSegments.matched);
            XX_TEST_EXPECT_EQ(editSegments.displayName, "Edit");
            // 第 1 段 +1 -1, 第 2 段 +2 -1 → 合计 [+3 -2]
            XX_TEST_EXPECT_TRUE(
                editSegments.summary.find("[+3 -2] /home/user/g.cpp") != std::string::npos
            );
            XX_TEST_EXPECT_EQ(editSegments.items.size(), 3U);
            if (editSegments.items.size() == 3) {
                XX_TEST_EXPECT_EQ(editSegments.items[0].value("kind", std::string{}), "Diff");
                XX_TEST_EXPECT_EQ(
                    editSegments.items[0].value("path", std::string{}),
                    "/home/user/g.cpp"
                );
                XX_TEST_EXPECT_EQ(editSegments.items[0].value("oldStr", std::string{}), "oldA();");
                XX_TEST_EXPECT_EQ(editSegments.items[1].value("kind", std::string{}), "Divider");
                XX_TEST_EXPECT_EQ(editSegments.items[2].value("kind", std::string{}), "Diff");
                // 后续段不带 path (路径只在首段展示一次)
                XX_TEST_EXPECT_EQ(editSegments.items[2].value("path", std::string{}), "");
                XX_TEST_EXPECT_EQ(
                    editSegments.items[2].value("newStr", std::string{}),
                    "x2();\ny2();"
                );
            }

            // 15.3h edit `edits`: 客户端只收到数组形态
            // - 模型少写一层数组 (传单个对象) 时, agent 在写展示历史前已按工具 schema
            //   包成数组 (见 util::autoFixToolArgsTypes 与 event_bridge 的
            //   tool_args_normalize 用例), 渲染器不再自己做兼容
            auto editSingleObj = co_await renderToolAsync(
                mgr,
                "call_edit_8",
                "agentxx_filesystem_edit",
                R"({"path":"/home/user/i.cpp","edits":[{"old_str":"oldA();","new_str":"newA();"}]})",
                "success",
                true,
                false,
                100
            );
            XX_TEST_EXPECT_TRUE(editSingleObj.matched);
            XX_TEST_EXPECT_EQ(editSingleObj.displayName, "Edit");
            XX_TEST_EXPECT_TRUE(
                editSingleObj.summary.find("[+1 -1] /home/user/i.cpp") != std::string::npos
            );
            XX_TEST_EXPECT_EQ(editSingleObj.items.size(), 1U);
            if (editSingleObj.items.size() == 1) {
                XX_TEST_EXPECT_EQ(editSingleObj.items[0].value("kind", std::string{}), "Diff");
                XX_TEST_EXPECT_EQ(
                    editSingleObj.items[0].value("path", std::string{}),
                    "/home/user/i.cpp"
                );
                XX_TEST_EXPECT_EQ(editSingleObj.items[0].value("oldStr", std::string{}), "oldA();");
                XX_TEST_EXPECT_EQ(editSingleObj.items[0].value("newStr", std::string{}), "newA();");
            }

            // 15.3i 同一份参数多次请求: 展开体与折叠头逐字一致 (缓存/幂等)
            auto editArrayOne = co_await renderToolAsync(
                mgr,
                "call_edit_9",
                "agentxx_filesystem_edit",
                R"({"path":"/home/user/i.cpp","edits":[{"old_str":"oldA();","new_str":"newA();"}]})",
                "success",
                true,
                false,
                100
            );
            XX_TEST_EXPECT_TRUE(editArrayOne.matched);
            XX_TEST_EXPECT_EQ(editArrayOne.summary, editSingleObj.summary);
            XX_TEST_EXPECT_EQ(editArrayOne.items.dump(), editSingleObj.items.dump());

            co_await mgr->unloadAsync("agentxx_filesystem");
            XX_TEST_EXPECT_TRUE(mgr->find("agentxx_filesystem") == nullptr);
        }
    }

    // ---- 15b. agentxx_filesystem client 插件: glob/grep 渲染 (数組参数) ----
    // 真实会话中 LLM 常把 file_patterns/text_patterns/regex_patterns 下发为单个字符串;
    // agent 在写展示历史前已按工具 schema 包成数组 (util::autoFixToolArgsTypes,
    // 见 event_bridge 的 tool_args_normalize 用例), 所以渲染器拿到的总是数组。
    {
        auto fsPath = findPluginPath("agentxx_filesystem");
        auto fsInst = co_await mgr->loadNativeAsync(fsPath);
        XX_TEST_EXPECT_TRUE(fsInst != nullptr);
        if (fsInst) {
            auto reg = mgr->uiRegistrySnapshot();
            XX_TEST_EXPECT_TRUE(reg != nullptr);

            // glob 数组形态 (旧行为, 回归保护)
            auto globArr = co_await renderToolAsync(
                mgr,
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

            // glob 单元素数组 (模型下发单字符串, 经 agent 参数自愈后的形态)
            auto globStr = co_await renderToolAsync(
                mgr,
                "call_glob_str",
                "agentxx_filesystem_glob",
                R"({"file_patterns":["agent/test/*.cpp"]})",
                "",
                true,
                false,
                100
            );
            XX_TEST_EXPECT_TRUE(globStr.matched);
            XX_TEST_EXPECT_EQ(globStr.displayName, "Glob");
            XX_TEST_EXPECT_TRUE(globStr.summary.find("agent/test/*.cpp") != std::string::npos);

            // grep 数组形态 (旧行为, 回归保护)
            auto grepArr = co_await renderToolAsync(
                mgr,
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

            // grep 单元素数组 (模型对 text/file 都下发单字符串时的自愈形态)
            auto grepStr = co_await renderToolAsync(
                mgr,
                "call_grep_str",
                "agentxx_filesystem_grep",
                R"({"text_patterns":["startDaSimServer"],"file_patterns":["agent/test/core/test_agent.cpp"]})",
                "",
                true,
                false,
                100
            );
            XX_TEST_EXPECT_TRUE(grepStr.matched);
            XX_TEST_EXPECT_EQ(grepStr.displayName, "Grep");
            XX_TEST_EXPECT_TRUE(grepStr.summary.find(R"("startDaSimServer")") != std::string::npos);
            XX_TEST_EXPECT_TRUE(
                grepStr.summary.find("agent/test/core/test_agent.cpp") != std::string::npos
            );

            // grep 混合形态: 单元素 text 数组 + 单元素 regex 数组 (摘要合并展示)
            auto grepMix = co_await renderToolAsync(
                mgr,
                "call_grep_mix",
                "agentxx_filesystem_grep",
                R"({"text_patterns":["hello"],"regex_patterns":["world"],"file_patterns":["src/**/*.txt"]})",
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

    // ---- 16. agentxx_websearch client 插件: 工具特化渲染 (折叠内容) ----
    {
        auto wsPath = findPluginPath("agentxx_websearch");
        auto wsInst = co_await mgr->loadNativeAsync(wsPath);
        XX_TEST_EXPECT_TRUE(wsInst != nullptr);
        if (wsInst) {
            auto reg = mgr->uiRegistrySnapshot();
            XX_TEST_EXPECT_TRUE(reg != nullptr);

            auto searchRes = co_await renderToolAsync(
                mgr,
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

    // ---- 17. agentxx_execute_command client 插件: 工具特化渲染 (折叠内容) ----
    {
        auto cmdPath = findPluginPath("agentxx_execute_command");
        auto cmdInst = co_await mgr->loadNativeAsync(cmdPath);
        XX_TEST_EXPECT_TRUE(cmdInst != nullptr);
        if (cmdInst) {
            auto reg = mgr->uiRegistrySnapshot();
            XX_TEST_EXPECT_TRUE(reg != nullptr);

            auto bashRes = co_await renderToolAsync(
                mgr,
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
            const auto ui = agentxx::plugin::ClientIfaces::query(exInst->hostView()).ui;
            XX_TEST_EXPECT_TRUE(ui != nullptr && ui->bind_action_handler != nullptr);
            XX_TEST_EXPECT_TRUE(ui != nullptr && ui->unbind_action_handler != nullptr);
            if (ui && ui->bind_action_handler && ui->unbind_action_handler) {
                auto exHost = exInst->hostView();
                // cb 空 → 失败
                XX_TEST_EXPECT_TRUE(uiBindAction(exHost, ui, "", nullptr, &hitsFallback) != 0);
                // bind fallback ("") + 精确 ("sec1")
                XX_TEST_EXPECT_EQ(uiBindAction(exHost, ui, "", fallbackFn, &hitsFallback), 0);
                XX_TEST_EXPECT_EQ(uiBindAction(exHost, ui, "sec1", exactFn, &hitsExact), 0);
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
                co_await drainPlugin(mgr, "example_plugin");
                XX_TEST_EXPECT_EQ(hitsExact.load(), 1);
                XX_TEST_EXPECT_EQ(hitsFallback.load(), 0);
                // 未命中精确 → 回落 "": owner=other → fallback
                mgr->dispatchAction("example_plugin", "other", "a.do", "{}");
                co_await drainPlugin(mgr, "example_plugin");
                XX_TEST_EXPECT_EQ(hitsExact.load(), 1);
                XX_TEST_EXPECT_EQ(hitsFallback.load(), 1);
                // 覆盖语义: 同 (plugin,target) 重绑覆盖
                std::atomic<int> hitsExact2{0};
                XX_TEST_EXPECT_EQ(uiBindAction(exHost, ui, "sec1", exactFn, &hitsExact2), 0);
                mgr->dispatchAction("example_plugin", "sec1", "a.do", "{}");
                co_await drainPlugin(mgr, "example_plugin");
                XX_TEST_EXPECT_EQ(hitsExact.load(), 1);
                XX_TEST_EXPECT_EQ(hitsExact2.load(), 1);
                // unbind 精确后回落 fallback
                XX_TEST_EXPECT_EQ(uiUnbindAction(exHost, ui, "sec1"), 0);
                mgr->dispatchAction("example_plugin", "sec1", "a.do", "{}");
                co_await drainPlugin(mgr, "example_plugin");
                XX_TEST_EXPECT_EQ(hitsFallback.load(), 2);
                // disable 保留 / enable 恢复
                co_await mgr->disable("example_plugin");
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
                co_await mgr->enable("example_plugin");
                // 启用走 client start 事务 (client io 线程异步执行): 等插件自身注册恢复;
                // 测试手动绑定的处理器不属于插件 start 事务, 需要重新绑定
                for (int i = 0; i < 200 && !mgr->hasCommand("example"); ++i) {
                    co_await sleepMs(5);
                }
                XX_TEST_EXPECT_EQ(uiBindAction(exHost, ui, "sec1", exactFn, &hitsExact), 0);
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

                // dispatch 上下文内容校验 (owner/action/args 原样转发)
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
                XX_TEST_EXPECT_EQ(uiBindAction(exHost, ui, "", capFn, &cap), 0);
                mgr->dispatchAction(
                    "example_plugin",
                    "toolCallX",
                    "planning.open_graph",
                    R"({"k":1})"
                );
                co_await drainPlugin(mgr, "example_plugin");
                XX_TEST_EXPECT_EQ(cap.owner, "toolCallX");
                XX_TEST_EXPECT_EQ(cap.action, "planning.open_graph");
                XX_TEST_EXPECT_TRUE(cap.args.find("\"k\"") != std::string::npos);
                // 未知 action_id 同样派发 (插件侧查表, 主程序不拦截);
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
            const auto ui = agentxx::plugin::ClientIfaces::query(exInst->hostView()).ui;
            XX_TEST_EXPECT_TRUE(ui != nullptr && ui->open_overlay != nullptr);
            XX_TEST_EXPECT_TRUE(ui != nullptr && ui->close_overlay != nullptr);
            if (ui && ui->open_overlay && ui->close_overlay) {
                auto exHost = exInst->hostView();
                // version 非法 → 失败
                AgentxxOverlaySpec badVer{};
                badVer.version = 99;
                badVer.type    = AGENTXX_OVERLAY_MERMAID;
                XX_TEST_EXPECT_TRUE(uiOpenOverlay(exHost, ui, &badVer) != 0);
                // type 越界 → 失败
                AgentxxOverlaySpec badType{};
                badType.version = 1;
                badType.type    = 99;
                XX_TEST_EXPECT_TRUE(uiOpenOverlay(exHost, ui, &badType) != 0);
                // MERMAID 成功 → 适配器收到信号
                const char*        mermaid = "stateDiagram-v2\n[*] --> a\na --> [*]";
                AgentxxOverlaySpec spec{};
                spec.version    = 1;
                spec.type       = AGENTXX_OVERLAY_MERMAID;
                spec.title      = agentxx::plugin::PluginStringView::fromCstr("T");
                spec.payload    = agentxx::plugin::PluginStringView::fromCstr(mermaid);
                spec.extra_json = agentxx::plugin::PluginStringView::fromCstr("{}");
                XX_TEST_EXPECT_EQ(uiOpenOverlay(exHost, ui, &spec), 0);
                XX_TEST_EXPECT_EQ(adapter->overlayOpenCount(), 1);
                XX_TEST_EXPECT_EQ(adapter->lastOverlayType(), AGENTXX_OVERLAY_MERMAID);
                XX_TEST_EXPECT_EQ(adapter->lastOverlayPlugin(), "example_plugin");
                XX_TEST_EXPECT_TRUE(
                    adapter->lastOverlayPayload().find("stateDiagram-v2") != std::string::npos
                );
                // close → 适配器收到信号 (不崩溃)
                uiCloseOverlay(exHost, ui);
                XX_TEST_EXPECT_EQ(adapter->overlayCloseCount(), 1);
                // DIFF payload 原样转发 (主程序不解析, 仅校验类型)
                AgentxxOverlaySpec diffSpec{};
                diffSpec.version = 1;
                diffSpec.type    = AGENTXX_OVERLAY_DIFF;
                diffSpec.title   = agentxx::plugin::PluginStringView::fromCstr("D");
                diffSpec.payload = agentxx::plugin::PluginStringView::fromCstr(
                    R"({"path":"a","old_str":"x","new_str":"y"})"
                );
                diffSpec.extra_json = agentxx::plugin::PluginStringView::fromCstr("{}");
                XX_TEST_EXPECT_EQ(uiOpenOverlay(exHost, ui, &diffSpec), 0);
                XX_TEST_EXPECT_EQ(adapter->overlayOpenCount(), 2);
                XX_TEST_EXPECT_EQ(adapter->lastOverlayType(), AGENTXX_OVERLAY_DIFF);

                // SDK 便捷方法 showDiffOverlay: 单段按旧写法下发 (老主程序也能渲染),
                // 多段用 `edits` 数组下发; 空段列表打开"无变化"的 diff
                struct SdkProbe : agentxx::plugin::ClientPluginBase {};

                SdkProbe sdk;
                sdk.host            = exInst->hostView();
                sdk.iface           = agentxx::plugin::ClientIfaces::query(exInst->hostView());
                const int beforeSdk = adapter->overlayOpenCount();

                XX_TEST_EXPECT_EQ(
                    sdk.showDiffOverlay(
                        "单段",
                        "/p/a.cpp",
                        {
                            {"old();", "new();"}
                },
                        "{}"
                    ),
                    0
                );
                XX_TEST_EXPECT_EQ(adapter->overlayOpenCount(), beforeSdk + 1);
                XX_TEST_EXPECT_EQ(adapter->lastOverlayType(), AGENTXX_OVERLAY_DIFF);
                {
                    auto payload = utilxx_base::Json::parse(adapter->lastOverlayPayload());
                    XX_TEST_EXPECT_EQ(payload.value("path", std::string{}), "/p/a.cpp");
                    XX_TEST_EXPECT_EQ(payload.value("old_str", std::string{}), "old();");
                    XX_TEST_EXPECT_EQ(payload.value("new_str", std::string{}), "new();");
                    XX_TEST_EXPECT_FALSE(payload.contains("edits"));
                }

                XX_TEST_EXPECT_EQ(
                    sdk.showDiffOverlay(
                        "多段",
                        "/p/b.cpp",
                        {
                            {"a();", "A();"},
                            {"b();", "B();"}
                },
                        "{}"
                    ),
                    0
                );
                XX_TEST_EXPECT_EQ(adapter->overlayOpenCount(), beforeSdk + 2);
                {
                    auto payload = utilxx_base::Json::parse(adapter->lastOverlayPayload());
                    XX_TEST_EXPECT_EQ(payload.value("path", std::string{}), "/p/b.cpp");
                    XX_TEST_EXPECT_TRUE(payload.contains("edits"));
                    XX_TEST_EXPECT_EQ(payload["edits"].size(), size_t{2});
                    XX_TEST_EXPECT_EQ(payload["edits"][0].value("old_str", std::string{}), "a();");
                    XX_TEST_EXPECT_EQ(payload["edits"][1].value("new_str", std::string{}), "B();");
                }

                XX_TEST_EXPECT_EQ(sdk.showDiffOverlay("空", "/p/c.cpp", {}, "{}"), 0);
                XX_TEST_EXPECT_EQ(adapter->overlayOpenCount(), beforeSdk + 3);
            }
            co_await mgr->unloadAsync("example_plugin");
            XX_TEST_EXPECT_TRUE(mgr->find("example_plugin") == nullptr);
        }
    }

    // ---- 20. planning 全流程: dispatch(open_graph) → open_overlay(MERMAID) ----
    {
        auto plPath = findPluginPath("agentxx_planning");
        auto plInst = co_await mgr->loadNativeAsync(plPath);
        XX_TEST_EXPECT_TRUE(plInst != nullptr);
        if (plInst) {
            // 先推送规划 (Info 段 + last_plan_json 就绪)
            agentxx::agent::WirePluginData plData;
            plData.topic = "plugin.agentxx_planning.planning";
            plData.data   = R"({"roadmap":"stateDiagram-v2\n[*] --> e2e\ne2e --> [*]","todos":[]})";
            mgr->onPluginData(plData);
            co_await drainPlugin(mgr, "agentxx_planning");
            const int openBefore = adapter->overlayOpenCount();
            // decor owner=toolCallId 同样被 fallback 接住
            mgr->dispatchAction("agentxx_planning", "call_e2e_1", "planning.open_graph", "{}");
            co_await drainPlugin(mgr, "agentxx_planning");
            XX_TEST_EXPECT_EQ(adapter->overlayOpenCount(), openBefore + 1);
            XX_TEST_EXPECT_EQ(adapter->lastOverlayType(), AGENTXX_OVERLAY_MERMAID);
            XX_TEST_EXPECT_TRUE(adapter->lastOverlayPayload().find("e2e") != std::string::npos);
            XX_TEST_EXPECT_EQ(adapter->lastOverlayTitle(), "Planning Roadmap");
            co_await mgr->unloadAsync("agentxx_planning");
            XX_TEST_EXPECT_TRUE(mgr->find("agentxx_planning") == nullptr);
        }
    }

    // ---- 21. kit::ActionController 单测 (header-only, 无主程序) ----
    {
        agentxx::plugin::kit::ActionController ctl;
        XX_TEST_EXPECT_TRUE(ctl.empty());
        int hitsA = 0, hitsB = 0;
        ctl.on("a.open", [&](const utilxx_base::Json&) {
            ++hitsA;
        });
        // 生成按钮组件 (界面描述层): 动作以 dispatch 形式写在 item.action 上,
        // 函数指针留在插件侧 map (见 ActionController 说明)
        auto btn = ctl.makeButton(
            "Go",
            [&](const utilxx_base::Json& args) {
                ++hitsB;
                XX_TEST_EXPECT_EQ(args.value("k", 0), 1);
        },
            "accent",
            utilxx_base::Json{{"k", 1}}
        );
        XX_TEST_EXPECT_TRUE(!ctl.empty());
        XX_TEST_EXPECT_EQ(ctl.size(), 2U);
        XX_TEST_EXPECT_EQ(btn.kind, std::string{"Button"});
        XX_TEST_EXPECT_EQ(btn.label.fallback, std::string{"Go"});
        XX_TEST_EXPECT_EQ(btn.color, std::string{"accent"});
        XX_TEST_EXPECT_EQ(btn.variant, std::string{"normal"});
        XX_TEST_EXPECT_TRUE(btn.action.kind == pluginxx::ui::Action::Kind::Call);
        XX_TEST_EXPECT_TRUE(btn.action.name.rfind("act_", 0) == 0);
        XX_TEST_EXPECT_EQ(btn.action.argsJson, std::string{R"({"k":1})"});
        // dispatch 固定 id
        AgentxxUiActionContext ctxA{};
        ctxA.version     = 1;
        auto aId         = agentxx::plugin::PluginStringView::fromCstr("a.open");
        auto emptyArgs   = agentxx::plugin::PluginStringView::fromCstr("{}");
        ctxA.action_id   = aId;
        ctxA.action_args = emptyArgs;
        agentxx::plugin::kit::ActionController::dispatch(&ctxA, &ctl);
        XX_TEST_EXPECT_EQ(hitsA, 1);
        // dispatch 自增 id (args 原样转发)
        AgentxxUiActionContext ctxB{};
        ctxB.version         = 1;
        std::string bIdStr   = btn.action.name;
        std::string bArgsStr = btn.action.argsJson;
        auto        bIdSv = agentxx::plugin::PluginStringView::from(bIdStr.data(), bIdStr.size());
        auto bArgsSv   = agentxx::plugin::PluginStringView::from(bArgsStr.data(), bArgsStr.size());
        ctxB.action_id = bIdSv;
        ctxB.action_args = bArgsSv;
        agentxx::plugin::kit::ActionController::dispatch(&ctxB, &ctl);
        XX_TEST_EXPECT_EQ(hitsB, 1);
        // 未知 id / 空指针守卫 (不崩溃)
        AgentxxUiActionContext ctxC{};
        ctxC.version     = 1;
        auto cId         = agentxx::plugin::PluginStringView::fromCstr("no.such");
        ctxC.action_id   = cId;
        ctxC.action_args = emptyArgs;
        agentxx::plugin::kit::ActionController::dispatch(&ctxC, &ctl);
        agentxx::plugin::kit::ActionController::dispatch(nullptr, &ctl);
        agentxx::plugin::kit::ActionController::dispatch(&ctxC, nullptr);
        XX_TEST_EXPECT_EQ(hitsA, 1);
        XX_TEST_EXPECT_EQ(hitsB, 1);
    }

    // ---- 22. 按钮绑定判定与渲染 helper (headless, 无 FTXUI 布局) ----
    {
        using namespace agentxx::client;
        agentxx::plugin::ClientUiRegistry reg;
        // 无绑定 → 不可点
        XX_TEST_EXPECT_FALSE(hasPluginBinding("agentxx_planning", &reg));
        // 绑定后 → 可点 (fallback "" 覆盖)
        agentxx::plugin::ClientActionBinding b;
        b.plugin   = "agentxx_planning";
        b.targetId = "";
        b.cb       = +[](const AgentxxUiActionContext*, void*) {};
        reg.actionBindings.push_back(b);
        XX_TEST_EXPECT_TRUE(hasPluginBinding("agentxx_planning", &reg));
        XX_TEST_EXPECT_TRUE(hasPluginBindingFor("agentxx_planning", "call_x", &reg));
        XX_TEST_EXPECT_FALSE(hasPluginBindingFor("agentxx_planning", "call_x", nullptr));
        XX_TEST_EXPECT_FALSE(hasPluginBinding("other_plugin", &reg));
        // 精确绑定同样可点; 他插件不可点
        reg.actionBindings.clear();
        b.targetId = "call_x";
        reg.actionBindings.push_back(b);
        XX_TEST_EXPECT_TRUE(hasPluginBindingFor("agentxx_planning", "call_x", &reg));
        // owner=other 时精确未命中且无 "" 绑定 → false
        XX_TEST_EXPECT_FALSE(hasPluginBindingFor("agentxx_planning", "other_owner", &reg));
        XX_TEST_EXPECT_FALSE(hasPluginBinding("other_plugin", &reg));
        // color → 主题色映射 (组件层与中断项共用)
        const auto theme = TUITheme::darkTheme();
        XX_TEST_EXPECT_TRUE(uiRoleColor("hint", theme) == theme.hintColor);
        XX_TEST_EXPECT_TRUE(uiRoleColor("title", theme) == theme.accentColor);
        XX_TEST_EXPECT_TRUE(uiRoleColor("error", theme) == theme.errorColor);
        XX_TEST_EXPECT_TRUE(uiRoleColor("weird", theme) == theme.normalColor);
        // render 不崩溃 (headless 仅构造 Element, 不布局)
        {
            PluginButtonDesc desc;
            desc.label     = "Graph";
            desc.actionId  = "planning.open_graph";
            desc.argsJson  = "{}";
            desc.role      = PluginButtonRole::Accent;
            desc.clickable = true;
            auto el1       = renderPluginButton(desc, theme);
            XX_TEST_EXPECT_TRUE(static_cast<bool>(el1));
            auto el3 = renderPluginDiff("p", "a\n", "b\n", theme, 120);
            XX_TEST_EXPECT_TRUE(static_cast<bool>(el3));
        }
    }

    // ---- 21b. 工具语义渲染缓存: UI 线程不调用插件 renderer, 旧快照安全回退 ----
    {
        auto fsPath = findPluginPath("agentxx_filesystem");
        auto fsInst = co_await mgr->loadNativeAsync(fsPath);
        XX_TEST_EXPECT_TRUE(fsInst != nullptr);
        if (fsInst) {
            const uint64_t generation = fsInst->lifetime ? fsInst->lifetime->generation() : 0;
            auto           reg        = mgr->uiRegistrySnapshot();
            auto           cache      = mgr->toolRenderCache();
            XX_TEST_EXPECT_TRUE(reg != nullptr && cache != nullptr);

            // 显示名是注册时确定的静态信息: UI 线程同步读得到, 首帧就能显示名字,
            // 不投递渲染请求
            XX_TEST_EXPECT_EQ(
                agentxx::plugin::toolDisplayNameFor(reg.get(), "call_read_cache_1",
                                                   "agentxx_filesystem_read"),
                std::string_view{"Read"}
            );
            XX_TEST_EXPECT_EQ(
                agentxx::plugin::toolDisplayNameFor(reg.get(), {}, "agentxx_filesystem_read"),
                std::string_view{"Read"}
            );
            XX_TEST_EXPECT_TRUE(
                agentxx::plugin::toolDisplayNameFor(reg.get(), {}, "no_such_tool").empty()
            );

            agentxx::plugin::ClientToolRenderRequest req;
            req.toolCallId = "call_read_cache_1";
            req.toolName   = "agentxx_filesystem_read";
            req.argsJson   = R"({"path":"/home/user/a.cpp","offset":10,"limit":50})";
            req.resultText = "";
            req.part       = agentxx::plugin::ClientToolRenderPart::Collapsed;
            req.isFinished = true;
            req.isError    = false;
            req.maxWidth   = 100;
            const std::string collapsedKey = agentxx::plugin::ClientToolRenderRequest::keyFor(
                req.toolCallId,
                req.toolName,
                agentxx::plugin::ClientToolRenderPart::Collapsed
            );
            const std::string expandedKey = agentxx::plugin::ClientToolRenderRequest::keyFor(
                req.toolCallId,
                req.toolName,
                agentxx::plugin::ClientToolRenderPart::Expanded
            );
            XX_TEST_EXPECT_TRUE(collapsedKey != expandedKey);
            const uint64_t hash = req.inputHash();

            // 首次查询折叠内容: 回调还没算出来 → pending 且没有上一份 → 内容为空
            // (界面此时只画名字, 不画 " · ")
            auto first = agentxx::plugin::renderClientToolCollapsed(
                reg.get(),
                cache.get(),
                req.toolCallId,
                req.toolName,
                req.argsJson,
                req.resultText,
                req.isFinished,
                req.isError,
                req.maxWidth
            );
            XX_TEST_EXPECT_TRUE(first.pending);
            XX_TEST_EXPECT_EQ(first.pendingPlugin, std::string{"agentxx_filesystem"});
            XX_TEST_EXPECT_TRUE(first.text.empty());
            XX_TEST_EXPECT_FALSE(first.isDecor);
            XX_TEST_EXPECT_TRUE(cache->lookup(collapsedKey, hash) == nullptr);
            // 折叠行不触发展开内容渲染: 展开部分的键还没有任何条目
            XX_TEST_EXPECT_EQ(cache->version(expandedKey), uint64_t{0});

            // 提交折叠内容请求: 结果由 client io 线程写入缓存 (含插件名与实例代次)
            mgr->requestToolRender(req);
            for (int i = 0; i < 500 && !cache->lookup(collapsedKey, hash); ++i) {
                co_await sleepMs(1);
            }
            auto entry = cache->lookup(collapsedKey, hash);
            XX_TEST_EXPECT_TRUE(entry != nullptr);
            if (entry) {
                XX_TEST_EXPECT_TRUE(entry->matched);
                XX_TEST_EXPECT_EQ(entry->plugin, std::string{"agentxx_filesystem"});
                XX_TEST_EXPECT_EQ(entry->generation, generation);
                XX_TEST_EXPECT_TRUE(entry->text.find("[10, 60]") != std::string::npos);
                XX_TEST_EXPECT_TRUE(entry->items.empty()); ///< 折叠部分不带 items
            }
            const uint64_t versionAfterRender = cache->version(collapsedKey);
            XX_TEST_EXPECT_TRUE(versionAfterRender > 0);
            // 折叠内容写入不影响展开内容的键 (两部分各自独立)
            XX_TEST_EXPECT_EQ(cache->version(expandedKey), uint64_t{0});

            auto second = agentxx::plugin::renderClientToolCollapsed(
                reg.get(),
                cache.get(),
                req.toolCallId,
                req.toolName,
                req.argsJson,
                req.resultText,
                req.isFinished,
                req.isError,
                req.maxWidth
            );
            XX_TEST_EXPECT_FALSE(second.pending);
            XX_TEST_EXPECT_TRUE(second.text.find("[10, 60]") != std::string::npos);

            // 展开内容: 同一输入但独立的键与请求 (Read 只注册了折叠内容 →
            // 展开部分没有自定义内容, 界面走默认展开体)
            // 展开内容: 工具行内容是同一个生成函数管两处, 所以展开部分也有注册项 ——
            // 首次查询是 pending (生成要在插件线程上跑), 这一帧展开体为空
            auto expandedFirst = agentxx::plugin::renderClientToolExpanded(
                reg.get(),
                cache.get(),
                req.toolCallId,
                req.toolName,
                req.argsJson,
                req.resultText,
                req.isFinished,
                req.isError,
                req.maxWidth
            );
            XX_TEST_EXPECT_TRUE(expandedFirst.pending);
            XX_TEST_EXPECT_EQ(expandedFirst.pendingPlugin, std::string{"agentxx_filesystem"});
            XX_TEST_EXPECT_TRUE(expandedFirst.items.empty());

            // 生成结果为空 (Read 没有展开体内容) → matched == false 写进缓存,
            // 界面回到默认展开体, 且不会每帧重复请求
            {
                agentxx::plugin::ClientToolRenderRequest expandedReq;
                expandedReq.toolCallId = req.toolCallId;
                expandedReq.toolName   = req.toolName;
                expandedReq.argsJson   = req.argsJson;
                expandedReq.resultText = req.resultText;
                expandedReq.part       = agentxx::plugin::ClientToolRenderPart::Expanded;
                expandedReq.isFinished = true;
                expandedReq.isError    = false;
                expandedReq.maxWidth   = req.maxWidth;
                mgr->requestToolRender(expandedReq);
                for (int i = 0; i < 500 && !cache->lookup(expandedKey, hash); ++i) {
                    co_await sleepMs(1);
                }
                const auto expandedEntry = cache->lookup(expandedKey, hash);
                XX_TEST_EXPECT_TRUE(expandedEntry != nullptr);
                if (expandedEntry) {
                    XX_TEST_EXPECT_FALSE(expandedEntry->matched);
                    XX_TEST_EXPECT_TRUE(expandedEntry->items.empty());
                }
            }

            auto expandedSecond = agentxx::plugin::renderClientToolExpanded(
                reg.get(),
                cache.get(),
                req.toolCallId,
                req.toolName,
                req.argsJson,
                req.resultText,
                req.isFinished,
                req.isError,
                req.maxWidth
            );
            XX_TEST_EXPECT_TRUE(expandedSecond.items.empty());
            XX_TEST_EXPECT_FALSE(expandedSecond.isDecor);

            // 输入变化 (参数) → 折叠内容不命中 → pending, 但沿用同一调用同一部分的
            // 上一份内容 (界面上内容只增不减)
            auto changed = agentxx::plugin::renderClientToolCollapsed(
                reg.get(),
                cache.get(),
                req.toolCallId,
                req.toolName,
                R"({"path":"/home/user/a.cpp","offset":1,"limit":2})",
                req.resultText,
                req.isFinished,
                req.isError,
                req.maxWidth
            );
            XX_TEST_EXPECT_TRUE(changed.pending);
            XX_TEST_EXPECT_TRUE(changed.text.find("[10, 60]") != std::string::npos);

            // 上一份内容的归属插件不一致 (插件重载/被其它插件覆盖) 时不得沿用
            {
                agentxx::plugin::ClientToolRenderEntry foreign;
                foreign.key = agentxx::plugin::ClientToolRenderRequest::keyFor(
                    "call_read_foreign",
                    req.toolName,
                    agentxx::plugin::ClientToolRenderPart::Collapsed
                );
                foreign.plugin  = "some_other_plugin";
                foreign.matched = true;
                foreign.text    = "stale content";
                cache->store(std::move(foreign));

                auto foreignSticky = agentxx::plugin::renderClientToolCollapsed(
                    reg.get(),
                    cache.get(),
                    "call_read_foreign",
                    req.toolName,
                    R"({"path":"/home/user/b.cpp"})",
                    "",
                    false,
                    false,
                    100
                );
                XX_TEST_EXPECT_TRUE(foreignSticky.pending);
                XX_TEST_EXPECT_TRUE(foreignSticky.text.empty());
            }

            // 没有 tool_call_id 时键退化为 "#工具名#c", 可能是另一次调用的内容:
            // 只报 pending, 不沿用
            {
                auto noId = agentxx::plugin::renderClientToolCollapsed(
                    reg.get(),
                    cache.get(),
                    std::string_view{},
                    req.toolName,
                    R"({"path":"/home/user/d.cpp"})",
                    "",
                    false,
                    false,
                    100
                );
                XX_TEST_EXPECT_TRUE(noId.pending);
                XX_TEST_EXPECT_TRUE(noId.text.empty());
            }

            // 回调返回失败/空内容 = "null": 写入 unmatched 条目, 之后不再 pending,
            // 内容为空由界面走默认渲染 (Edit 参数为空 → 折叠内容为空)
            {
                const std::string editArgs = R"({"path":"","old_str":"","new_str":""})";
                agentxx::plugin::ClientToolRenderRequest editReq;
                editReq.toolCallId = "call_edit_null";
                editReq.toolName   = "agentxx_filesystem_edit";
                editReq.argsJson   = editArgs;
                editReq.part       = agentxx::plugin::ClientToolRenderPart::Collapsed;
                editReq.isFinished = true;
                editReq.maxWidth   = 100;
                const std::string editKey = agentxx::plugin::ClientToolRenderRequest::keyFor(
                    editReq.toolCallId,
                    editReq.toolName,
                    agentxx::plugin::ClientToolRenderPart::Collapsed
                );
                const uint64_t editHash = editReq.inputHash();
                auto           pendingRes = agentxx::plugin::renderClientToolCollapsed(
                    reg.get(),
                    cache.get(),
                    editReq.toolCallId,
                    editReq.toolName,
                    editArgs,
                    "",
                    true,
                    false,
                    100
                );
                XX_TEST_EXPECT_TRUE(pendingRes.pending);
                mgr->requestToolRender(editReq);
                for (int i = 0; i < 500 && !cache->lookup(editKey, editHash); ++i) {
                    co_await sleepMs(1);
                }
                auto nullEntry = cache->lookup(editKey, editHash);
                XX_TEST_EXPECT_TRUE(nullEntry != nullptr);
                if (nullEntry) {
                    XX_TEST_EXPECT_FALSE(nullEntry->matched);
                    XX_TEST_EXPECT_TRUE(nullEntry->text.empty());
                }
                auto nullRes = agentxx::plugin::renderClientToolCollapsed(
                    reg.get(),
                    cache.get(),
                    editReq.toolCallId,
                    editReq.toolName,
                    editArgs,
                    "",
                    true,
                    false,
                    100
                );
                XX_TEST_EXPECT_FALSE(nullRes.pending); ///< 已算出 (无自定义内容), 不再请求
                XX_TEST_EXPECT_TRUE(nullRes.text.empty());
            }

            // 装饰 (update_tool_decor) 优先, 但缺某一项时回落注册项 (不是清空)
            {
                std::string decorJson = R"({"displayName":"ReadDecor"})";
                auto        ui = agentxx::plugin::ClientIfaces::query(fsInst->hostView()).ui;
                XX_TEST_EXPECT_TRUE(ui != nullptr && ui->update_tool_decor != nullptr);
                if (ui && ui->update_tool_decor) {
                    XX_TEST_EXPECT_EQ(
                        uiUpdateToolDecor(fsInst->hostView(), ui, "call_read_decor", decorJson),
                        0
                    );
                    auto decorReg = mgr->uiRegistrySnapshot();
                    // 名字: 装饰优先
                    XX_TEST_EXPECT_EQ(
                        agentxx::plugin::toolDisplayNameFor(
                            decorReg.get(),
                            "call_read_decor",
                            "agentxx_filesystem_read"
                        ),
                        std::string_view{"ReadDecor"}
                    );
                    // 折叠内容: 装饰没给 summary → 回落注册项 (pending + 请求)
                    auto decorCollapsed = agentxx::plugin::renderClientToolCollapsed(
                        decorReg.get(),
                        cache.get(),
                        "call_read_decor",
                        "agentxx_filesystem_read",
                        R"({"path":"/home/user/e.cpp"})",
                        "",
                        true,
                        false,
                        100
                    );
                    XX_TEST_EXPECT_TRUE(decorCollapsed.pending);
                    XX_TEST_EXPECT_FALSE(decorCollapsed.isDecor);
                    // 展开内容: 装饰没给 items → 同样回落注册项 (内容没给展开体,
                    // 第一次展开先 pending, 结果为空后回到默认展开体)
                    auto decorExpanded = agentxx::plugin::renderClientToolExpanded(
                        decorReg.get(),
                        cache.get(),
                        "call_read_decor",
                        "agentxx_filesystem_read",
                        R"({"path":"/home/user/e.cpp"})",
                        "",
                        true,
                        false,
                        100
                    );
                    XX_TEST_EXPECT_TRUE(decorExpanded.pending);
                    XX_TEST_EXPECT_FALSE(decorExpanded.isDecor);
                    XX_TEST_EXPECT_TRUE(decorExpanded.items.empty());
                }
            }

            // 卸载后旧快照 + 旧缓存只能回退: 版本号递增驱动 UI 重建为默认渲染
            co_await mgr->unloadAsync("agentxx_filesystem");
            XX_TEST_EXPECT_TRUE(mgr->find("agentxx_filesystem") == nullptr);
            XX_TEST_EXPECT_TRUE(cache->lookup(collapsedKey, hash) == nullptr);
            XX_TEST_EXPECT_TRUE(cache->version(collapsedKey) > versionAfterRender);
            auto afterUnload = agentxx::plugin::renderClientToolCollapsed(
                reg.get(),
                cache.get(),
                req.toolCallId,
                req.toolName,
                req.argsJson,
                req.resultText,
                req.isFinished,
                req.isError,
                req.maxWidth
            );
            // 旧快照里的注册项还在, 但租约已失效 → 结果为默认渲染 (无自定义内容)
            XX_TEST_EXPECT_FALSE(afterUnload.isDecor);
        }
    }

    // ---- 21c. 动作代次: 重新加载同名插件后旧点击不转交新实例 ----
    {
        std::atomic<int> hits{0};
        auto             probeFn = +[](const AgentxxUiActionContext*, void* ud) {
            ++(*static_cast<std::atomic<int>*>(ud));
        };
        auto fsPath = findPluginPath("agentxx_filesystem");
        auto first  = co_await mgr->loadNativeAsync(fsPath);
        XX_TEST_EXPECT_TRUE(first != nullptr);
        if (first) {
            const auto ui = agentxx::plugin::ClientIfaces::query(first->hostView()).ui;
            XX_TEST_EXPECT_TRUE(ui != nullptr && ui->bind_action_handler != nullptr);
            if (ui && ui->bind_action_handler) {
                auto secSv = agentxx::plugin::PluginStringView::fromCstr("gen_sec");
                XX_TEST_EXPECT_EQ(
                    uiBindAction(first->hostView(), ui, "gen_sec", probeFn, &hits),
                    0
                );
                auto           reg  = mgr->uiRegistrySnapshot();
                const uint64_t gen1 = reg ? reg->generationOf("agentxx_filesystem") : 0;
                XX_TEST_EXPECT_TRUE(gen1 != 0);

                // 当前代次点击 → 派发
                mgr->dispatchAction("agentxx_filesystem", "gen_sec", "a.do", "{}", gen1);
                co_await drainPlugin(mgr, "agentxx_filesystem");
                XX_TEST_EXPECT_EQ(hits.load(), 1);

                // 卸载 + 重新加载 (新代次) → 旧代次点击丢弃, 新代次点击派发
                co_await mgr->unloadAsync("agentxx_filesystem");
                XX_TEST_EXPECT_TRUE(mgr->find("agentxx_filesystem") == nullptr);
                auto second = co_await mgr->loadNativeAsync(fsPath);
                XX_TEST_EXPECT_TRUE(second != nullptr);
                if (second) {
                    auto           reg2 = mgr->uiRegistrySnapshot();
                    const uint64_t gen2 = reg2 ? reg2->generationOf("agentxx_filesystem") : 0;
                    XX_TEST_EXPECT_TRUE(gen2 != 0);
                    XX_TEST_EXPECT_TRUE(gen2 != gen1);
                    // 旧代次点击: 实例已重载 → 丢弃 (即使绑定同名同 owner)
                    mgr->dispatchAction("agentxx_filesystem", "gen_sec", "a.do", "{}", gen1);
                    co_await drainPlugin(mgr, "agentxx_filesystem");
                    XX_TEST_EXPECT_EQ(hits.load(), 1);
                    // 新实例重新绑定后, 新代次点击正常派发
                    const auto ui2 = agentxx::plugin::ClientIfaces::query(second->hostView()).ui;
                    XX_TEST_EXPECT_TRUE(ui2 != nullptr && ui2->bind_action_handler != nullptr);
                    if (ui2 && ui2->bind_action_handler) {
                        XX_TEST_EXPECT_EQ(
                            uiBindAction(second->hostView(), ui2, "gen_sec", probeFn, &hits),
                            0
                        );
                    }
                    mgr->dispatchAction("agentxx_filesystem", "gen_sec", "a.do", "{}", gen2);
                    co_await drainPlugin(mgr, "agentxx_filesystem");
                    XX_TEST_EXPECT_EQ(hits.load(), 2);
                    co_await mgr->unloadAsync("agentxx_filesystem");
                }
            }
        }
    }

    // ---- 22. 同步关闭不得绕过 stop: 实例、上下文与 DSO 保留 + CloseFailed ----
    {
        asio::io_context syncIo;
        auto             syncMgr
            = std::make_shared<agentxx::plugin::ClientPluginManager>(syncIo.get_executor());
        auto fake = std::make_shared<agentxx::plugin::ClientPluginInstance>("fake_pending_stop");
        fake->lifecycleStop    = &fakeClientStopHook;
        fake->lifecycleStarted = true;
        fake->pluginCreated    = true;
        fake->manager          = syncMgr;
        fake->self             = fake;
        fake->runtime          = syncMgr->runtime();
        fake->lifetime         = std::make_shared<pluginxx::InstanceLifetime>(
            syncIo.get_executor(),
            fake->name,
            uint64_t{1}
        );
        fake->lifetime->setState(pluginxx::PluginInstanceState::Ready);
        syncMgr->plugins_.emplace(fake->name, fake);

        XX_TEST_EXPECT_TRUE(fake->lifecycleStopPending());
        syncMgr->shutdownAll();
        XX_TEST_EXPECT_TRUE(syncMgr->find("fake_pending_stop") != nullptr);
        XX_TEST_EXPECT_EQ(
            static_cast<int>(fake->lifetime->state()),
            static_cast<int>(pluginxx::PluginInstanceState::CloseFailed)
        );
        XX_TEST_EXPECT_FALSE(fake->pluginDestroyed);
        XX_TEST_EXPECT_TRUE(syncMgr->hasPendingClose());

        // 直接构造的伪实例 (无 stop 入口, 只有测试会这样构造) 仍走同步关闭:
        // 契约要求真实插件必须导出 start/stop, 但关闭守卫不能因此变成无条件泄漏。
        auto legacy      = std::make_shared<agentxx::plugin::ClientPluginInstance>("fake_legacy");
        legacy->manager  = syncMgr;
        legacy->self     = legacy;
        legacy->runtime  = syncMgr->runtime();
        legacy->lifetime = std::make_shared<pluginxx::InstanceLifetime>(
            syncIo.get_executor(),
            legacy->name,
            uint64_t{2}
        );
        legacy->lifetime->setState(pluginxx::PluginInstanceState::Ready);
        legacy->pluginCreated = true;
        syncMgr->plugins_.emplace(legacy->name, legacy);

        syncMgr->shutdownAll();
        XX_TEST_EXPECT_TRUE(syncMgr->find("fake_legacy") == nullptr);
        XX_TEST_EXPECT_TRUE(legacy->pluginDestroyed);
        // stop 未完成的实例仍然保留, 不能因其它实例关闭成功而被清理。
        XX_TEST_EXPECT_TRUE(syncMgr->find("fake_pending_stop") != nullptr);
        XX_TEST_EXPECT_TRUE(syncMgr->hasPendingClose());
    }

    // ---- 23. client 侧禁用/启用事务: 导出 start/stop 的插件收到 stop/start ----
    {
        auto createFake = [](const std::shared_ptr<agentxx::plugin::ClientPluginManager>& m,
                             asio::io_context&                                            io,
                             std::string                                                  name,
                             uint64_t generation) {
            auto inst  = std::make_shared<agentxx::plugin::ClientPluginInstance>(std::move(name));
            inst->self = inst;
            inst->manager  = m;
            inst->runtime  = m->runtime();
            inst->lifetime = std::make_shared<pluginxx::InstanceLifetime>(
                io.get_executor(),
                inst->name,
                generation
            );
            inst->lifetime->setState(pluginxx::PluginInstanceState::Ready);
            inst->pluginCreated = true;
            m->plugins_.emplace(inst->name, inst);
            return inst;
        };
        // 管理器自带插件执行线程: 禁用/启用事务在插件线程上跑, 推进要把两边都跑完
        auto drainIo = [](asio::io_context& io, agentxx::plugin::ClientPluginManager& mgr) {
            for (int round = 0; round < 8; ++round) {
                io.restart();
                io.poll();
                auto ex = mgr.pluginExecutor();
                if (!ex) {
                    continue;
                }
                // 哨兵: 插件线程按 FIFO 执行, 它跑完说明之前派发的回调都已结束
                std::atomic<bool> reached{false};
                asio::post(ex, [&reached] {
                    reached.store(true, std::memory_order_release);
                });
                for (int spin = 0; spin < 2000 && !reached.load(std::memory_order_acquire); ++spin) {
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                }
            }
            io.restart();
        };

        asio::io_context io;
        auto mgr2 = std::make_shared<agentxx::plugin::ClientPluginManager>(io.get_executor());
        ClientLifecycleProbe probe;
        auto                 inst = createFake(mgr2, io, "fake_lifecycle", uint64_t{7});
        inst->lifecycleCreate    = &probeClientCreateHook;
        inst->lifecycleStop      = &probeClientStopHook;
        inst->pluginCtx          = &probe;
        inst->lifecycleStarted   = true;
        // disable/enable 是协程: 完成包落在管理器自己的 io 上, 外层测试协程直接
        // co_await 会互相等 (没有别人推进这个局部 io), 所以投到它上面再推动它
        auto runLocal = [&]<typename Fn>(Fn fn) {
            bool finished = false;
            asio::co_spawn(
                io,
                keepAliveRun([fn = std::move(fn), &finished]() -> asio::awaitable<void> {
                    co_await fn();
                    finished = true;
                }),
                asio::detached
            );
            for (int round = 0; round < 2000 && !finished; ++round) {
                io.restart();
                io.poll();
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            io.restart();
            io.poll();
        };

        // 禁用: 主程序侧摘除 + stop 事务已走完 (disable 返回即已落定)
        runLocal([&]() -> asio::awaitable<void> {
            co_await mgr2->disable("fake_lifecycle");
        });
        XX_TEST_EXPECT_FALSE(inst->enabled);
        XX_TEST_EXPECT_TRUE(inst->userDisabled);
        XX_TEST_EXPECT_EQ(probe.stops, 1);
        XX_TEST_EXPECT_TRUE(inst->lifecycleStopped);
        XX_TEST_EXPECT_EQ(
            static_cast<int>(inst->lifetime->state()),
            static_cast<int>(pluginxx::PluginInstanceState::Disabled)
        );

        // 启用: create 事务重新声明注册, 成功后回到 Ready
        runLocal([&]() -> asio::awaitable<void> {
            co_await mgr2->enable("fake_lifecycle");
        });
        XX_TEST_EXPECT_TRUE(inst->enabled);
        XX_TEST_EXPECT_FALSE(inst->userDisabled);
        XX_TEST_EXPECT_EQ(probe.starts, 1);
        XX_TEST_EXPECT_FALSE(inst->lifecycleStopped);
        XX_TEST_EXPECT_EQ(
            static_cast<int>(inst->lifetime->state()),
            static_cast<int>(pluginxx::PluginInstanceState::Ready)
        );

        // 关闭流程中拒绝启用状态变化
        inst->lifetime->requestClose();
        runLocal([&]() -> asio::awaitable<void> {
            co_await mgr2->disable("fake_lifecycle");
        });
        XX_TEST_EXPECT_TRUE(inst->enabled);

        // 收尾: 卸载路径补齐欠着的 stop, 之后才能 destroy/dlclose
        bool closed = false;
        asio::co_spawn(
            io,
            keepAliveRun([&]() -> asio::awaitable<void> {
                closed
                    = co_await mgr2->unloadAsync("fake_lifecycle", std::chrono::milliseconds{500});
            }),
            asio::detached
        );
        drainIo(io, *mgr2);
        XX_TEST_EXPECT_TRUE(closed);
        XX_TEST_EXPECT_EQ(probe.stops, 2);
        XX_TEST_EXPECT_TRUE(mgr2->find("fake_lifecycle") == nullptr);
    }

    // ---- 24. client 侧依赖级联: 三级/菱形禁用-恢复 + 用户禁用不被级联恢复 ----
    {
        auto createFake = [](const std::shared_ptr<agentxx::plugin::ClientPluginManager>& m,
                             asio::io_context&                                            io,
                             std::string                                                  name,
                             uint64_t generation) {
            auto inst  = std::make_shared<agentxx::plugin::ClientPluginInstance>(std::move(name));
            inst->self = inst;
            inst->manager  = m;
            inst->runtime  = m->runtime();
            inst->lifetime = std::make_shared<pluginxx::InstanceLifetime>(
                io.get_executor(),
                inst->name,
                generation
            );
            inst->lifetime->setState(pluginxx::PluginInstanceState::Ready);
            m->plugins_.emplace(inst->name, inst);
            return inst;
        };
        asio::io_context io;
        auto mgr2     = std::make_shared<agentxx::plugin::ClientPluginManager>(io.get_executor());
        auto leaf     = createFake(mgr2, io, "leaf", 30);
        auto mid      = createFake(mgr2, io, "mid", 31);
        auto side     = createFake(mgr2, io, "side", 32);
        auto top      = createFake(mgr2, io, "top", 33);
        mid->depends  = {"leaf"};
        side->depends = {"leaf"};
        top->depends  = {"mid", "side"}; // 菱形: top 经 mid/side 两级依赖 leaf

        // disable/enable 是协程: 完成包落在管理器自己的 io 上, 外层测试协程直接
        // co_await 会互相等 (没有别人推进这个局部 io), 所以投到它上面再推动它
        auto runLocal = [&]<typename Fn>(Fn fn) {
            bool finished = false;
            asio::co_spawn(
                io,
                keepAliveRun([fn = std::move(fn), &finished]() -> asio::awaitable<void> {
                    co_await fn();
                    finished = true;
                }),
                asio::detached
            );
            for (int round = 0; round < 2000 && !finished; ++round) {
                io.restart();
                io.poll();
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            io.restart();
            io.poll();
        };

        runLocal([&]() -> asio::awaitable<void> {
            co_await mgr2->disable("leaf");
        });
        XX_TEST_EXPECT_FALSE(leaf->enabled);
        XX_TEST_EXPECT_FALSE(mid->enabled);
        XX_TEST_EXPECT_TRUE(mid->blockedByDependencies);
        XX_TEST_EXPECT_FALSE(side->enabled);
        XX_TEST_EXPECT_FALSE(top->enabled); // 三级依赖同样被级联
        XX_TEST_EXPECT_TRUE(top->blockedByDependencies);

        runLocal([&]() -> asio::awaitable<void> {
            co_await mgr2->enable("leaf");
        });
        XX_TEST_EXPECT_TRUE(leaf->enabled);
        XX_TEST_EXPECT_TRUE(mid->enabled);
        XX_TEST_EXPECT_TRUE(side->enabled);
        XX_TEST_EXPECT_TRUE(top->enabled);

        // 用户显式禁用的插件不被级联恢复
        runLocal([&]() -> asio::awaitable<void> {
            co_await mgr2->disable("top");
        });
        XX_TEST_EXPECT_TRUE(top->userDisabled);
        runLocal([&]() -> asio::awaitable<void> {
            co_await mgr2->disable("leaf");
        });
        XX_TEST_EXPECT_FALSE(mid->enabled);
        runLocal([&]() -> asio::awaitable<void> {
            co_await mgr2->enable("leaf");
        });
        XX_TEST_EXPECT_TRUE(mid->enabled);
        XX_TEST_EXPECT_FALSE(top->enabled);
        XX_TEST_EXPECT_TRUE(top->userDisabled);
        runLocal([&]() -> asio::awaitable<void> {
            co_await mgr2->enable("top");
        });
        XX_TEST_EXPECT_TRUE(top->enabled);
        XX_TEST_EXPECT_FALSE(top->userDisabled);
    }

    // ---- 25. 客户端旧 host 指针在卸载后安全失败 (client 侧) ----
    // 插件保存的 host 指针跨卸载继续调用时必须安全失败: 控制块地址稳定,
    // 实例失效后各入口返回失败值, 且同名重载后旧指针不会转交新实例。
    {
        auto mgr2     = std::make_shared<agentxx::plugin::ClientPluginManager>(ex);
        auto adapter2 = std::make_shared<MockPluginUiAdapter>();
        mgr2->setUiAdapter(adapter2);

        auto instA = co_await mgr2->loadNativeAsync(path);
        XX_TEST_EXPECT_TRUE(instA != nullptr);
        if (instA) {
            const PluginxxHost* oldHost = instA->hostView();
            XX_TEST_EXPECT_TRUE(oldHost != nullptr && oldHost->vtable != nullptr);
            XX_TEST_EXPECT_TRUE(
                co_await mgr2->unloadAsync("example_plugin", std::chrono::seconds{5})
            );
            // 控制块视图地址保持稳定; 接口表本身是进程级静态只读, 查询仍可用
            XX_TEST_EXPECT_TRUE(oldHost->vtable != nullptr);
            auto ui = agentxx::plugin::queryInterface<AgentxxClientUiIface>(
                oldHost,
                AGENTXX_IFACE_CLIENT_UI
            );
            XX_TEST_EXPECT_TRUE(ui != nullptr);
            if (ui) {
                // 迟到注册: 实例已销毁 -> 安全失败, 不访问已释放对象
                XX_TEST_EXPECT_TRUE(
                    uiRegisterStatusItem(oldHost, ui, "late.probe", R"({"text":"x"})") == nullptr
                );

                // 同名重载: 旧指针不得路由到新实例
                auto instB = co_await mgr2->loadNativeAsync(path);
                XX_TEST_EXPECT_TRUE(instB != nullptr);
                if (instB) {
                    XX_TEST_EXPECT_TRUE(instB->hostView() != oldHost);
                    XX_TEST_EXPECT_TRUE(
                        uiRegisterStatusItem(oldHost, ui, "late.probe", R"({"text":"x"})")
                        == nullptr
                    );
                    // 新实例自己的 host 视图可以注册 (证明失败来自指针失效而非环境)
                    auto* item = uiRegisterStatusItem(
                        instB->hostView(),
                        ui,
                        "late.probe",
                        R"({"text":"x"})"
                    );
                    XX_TEST_EXPECT_TRUE(item != nullptr);
                    if (item) {
                        uiUnregisterStatusItem(instB->hostView(), ui, item);
                    }
                    XX_TEST_EXPECT_TRUE(
                        co_await mgr2->unloadAsync("example_plugin", std::chrono::seconds{5})
                    );
                }
            }
        }
    }

    // ---- 26. 客户端加载期 start 失败的真实 DSO 回滚 (UI 项) ----
    // 测试插件 start 依次注册 状态栏项/面板/Info 段落/命令/订阅, 每步注册后
    // 立即经句柄自检 (update 必须成功, 保证"走到最后一步"意味着前面的注册
    // 真实生效), 然后主动失败: 主程序必须回滚全部 UI 注册与订阅。
    // 随后置 `AGENTXX_TEST_CLIENT_START_OK` 再次加载同一 DSO 必须完整成功
    // (证明 id/命令/订阅无残留冲突), 卸载后再次清空。
    {
#ifdef AGENTXX_TEST_CLIENT_START_FAIL_PLUGIN_PATH
        auto mgr2     = std::make_shared<agentxx::plugin::ClientPluginManager>(ex);
        auto adapter2 = std::make_shared<MockPluginUiAdapter>();
        mgr2->setUiAdapter(adapter2);
        mgr2->setSessionId("sess-rollback");

        const std::string dsoPath = AGENTXX_TEST_CLIENT_START_FAIL_PLUGIN_PATH;
        auto              findInRegistry
            = [](const std::shared_ptr<const agentxx::plugin::ClientUiRegistry>& reg,
                 const char*                                                     id) {
                  if (!reg) {
                      return false;
                  }
                  for (const auto& s : reg->statusItems) {
                      if (s.id == id) {
                          return true;
                      }
                  }
                  for (const auto& p : reg->panels) {
                      if (p.id == id) {
                          return true;
                      }
                  }
                  for (const auto& s : reg->infoSections) {
                      if (s.id == id) {
                          return true;
                      }
                  }
                  return false;
              };

        // 第一次加载: 全注册后失败, 主程序回滚
        clearSystemEnvVar("AGENTXX_TEST_CLIENT_START_OK");
        auto failed = co_await mgr2->loadNativeAsync(dsoPath);
        XX_TEST_EXPECT_TRUE(failed == nullptr);
        XX_TEST_EXPECT_FALSE(mgr2->hasCommand("test_client_start_fail_cmd"));
        {
            auto reg = mgr2->uiRegistrySnapshot();
            XX_TEST_EXPECT_TRUE(reg != nullptr);
            XX_TEST_EXPECT_FALSE(findInRegistry(reg, "test_client_start_fail.status"));
            XX_TEST_EXPECT_FALSE(findInRegistry(reg, "test_client_start_fail.panel"));
            XX_TEST_EXPECT_FALSE(findInRegistry(reg, "test_client_start_fail.info"));
        }

        // 第二次加载: 同名注册必须完整重新成功 (无残留冲突)
        setSystemEnvVar("AGENTXX_TEST_CLIENT_START_OK", "1");
        auto ok = co_await mgr2->loadNativeAsync(dsoPath);
        clearSystemEnvVar("AGENTXX_TEST_CLIENT_START_OK");
        XX_TEST_EXPECT_TRUE(ok != nullptr);
        if (ok) {
            XX_TEST_EXPECT_EQ(ok->name, "test_client_start_fail_plugin");
            XX_TEST_EXPECT_TRUE(mgr2->hasCommand("test_client_start_fail_cmd"));
            auto reg = mgr2->uiRegistrySnapshot();
            XX_TEST_EXPECT_TRUE(reg != nullptr);
            if (reg) {
                bool statusOk = false;
                for (const auto& s : reg->statusItems) {
                    if (s.id == "test_client_start_fail.status") {
                        statusOk = (s.text == "probe: 1");
                    }
                }
                bool panelOk = false;
                for (const auto& p : reg->panels) {
                    if (p.id == "test_client_start_fail.panel") {
                        panelOk = (p.title == "Probe");
                    }
                }
                bool infoOk = false;
                for (const auto& s : reg->infoSections) {
                    if (s.id == "test_client_start_fail.info") {
                        infoOk = (s.title == "Probe Info");
                    }
                }
                XX_TEST_EXPECT_TRUE(statusOk);
                XX_TEST_EXPECT_TRUE(panelOk);
                XX_TEST_EXPECT_TRUE(infoOk);
            }
            // 卸载: UI 项与命令再次清空
            XX_TEST_EXPECT_TRUE(
                co_await mgr2->unloadAsync("test_client_start_fail_plugin", std::chrono::seconds{5})
            );
            XX_TEST_EXPECT_FALSE(mgr2->hasCommand("test_client_start_fail_cmd"));
            auto regAfter = mgr2->uiRegistrySnapshot();
            XX_TEST_EXPECT_FALSE(findInRegistry(regAfter, "test_client_start_fail.status"));
            XX_TEST_EXPECT_FALSE(findInRegistry(regAfter, "test_client_start_fail.panel"));
            XX_TEST_EXPECT_FALSE(findInRegistry(regAfter, "test_client_start_fail.info"));
        }
#else
        // 独立构建未接线测试插件: 跳过
#endif
    }

    // ---- 27. 工具语义渲染缓存容量上限 ----
    // 长会话按 tool_call_id 持续写入不再无限增长: 超限淘汰最旧条目并回收
    // 其版本记录; 插件失效时的版本号仍单调变化 (旧快照重建信号)。
    {
        agentxx::plugin::ClientToolRenderCache cache(3);
        auto makeEntry = [](const std::string& key, const std::string& plugin) {
            agentxx::plugin::ClientToolRenderEntry entry;
            entry.key         = key;
            entry.plugin      = plugin;
            entry.inputHash   = 1;
            entry.matched     = true;
            entry.text        = key;
            return entry;
        };

        cache.store(makeEntry("k1", "p"));
        cache.store(makeEntry("k2", "p"));
        cache.store(makeEntry("k3", "p"));
        XX_TEST_EXPECT_EQ(cache.size(), size_t{3});
        XX_TEST_EXPECT_TRUE(cache.lookup("k1", 1) != nullptr);

        // 超限: 最旧的 k1 条目与版本记录一并回收
        cache.store(makeEntry("k4", "p"));
        XX_TEST_EXPECT_EQ(cache.size(), size_t{3});
        XX_TEST_EXPECT_TRUE(cache.lookup("k1", 1) == nullptr);
        XX_TEST_EXPECT_TRUE(cache.lookup("k4", 1) != nullptr);
        XX_TEST_EXPECT_EQ(cache.version("k1"), uint64_t{0});

        // 插件失效: 剩余条目的版本号递增 (即使条目被摘除)
        const uint64_t versionK2 = cache.version("k2");
        cache.invalidatePlugin("p");
        XX_TEST_EXPECT_TRUE(cache.lookup("k2", 1) == nullptr);
        XX_TEST_EXPECT_TRUE(cache.version("k2") > versionK2);

        // 继续写入: 被失效键的版本记录随后续淘汰按序回收 (有界)
        cache.store(makeEntry("k5", "p"));
        cache.store(makeEntry("k6", "p"));
        cache.store(makeEntry("k7", "p"));
        XX_TEST_EXPECT_EQ(cache.size(), size_t{3});
        XX_TEST_EXPECT_TRUE(cache.lookup("k5", 1) != nullptr);
        XX_TEST_EXPECT_TRUE(cache.lookup("k7", 1) != nullptr);
        XX_TEST_EXPECT_EQ(cache.version("k2"), uint64_t{0});
        XX_TEST_EXPECT_EQ(cache.version("k3"), uint64_t{0});
    }

    // ---- 28. 主程序内置工具特化渲染: agentxx_share_store / agentxx_subagent ----
    // lib 内置工具没有对应插件, 渲染器由主程序自身注册
    // (registerBuiltinToolRenderers → ClientPluginManager::registerBuiltinToolRenderer):
    // 存入 builtinToolRenderers 分表, 渲染走与插件渲染器相同的路径
    // (client io 线程执行 + 语义缓存), 展开体保持主程序通用展示 (不提供 items)。
    {
        auto mgr2     = std::make_shared<agentxx::plugin::ClientPluginManager>(ex);
        auto adapter2 = std::make_shared<MockPluginUiAdapter>();
        mgr2->setUiAdapter(adapter2);
        mgr2->setSessionId("sess-builtin");

        // 28.1 注册前: 无特化渲染 (UI 回退通用展示)
        {
            auto res = co_await renderToolAsync(
                mgr2,
                "call_bs_before",
                "agentxx_share_store",
                R"({"opt":"new","text":"a\nb"})",
                "",
                true,
                false,
                100
            );
            XX_TEST_EXPECT_FALSE(res.matched);
        }

        // 28.2 注册: 内置渲染器与插件渲染器分表存放; 重复注册幂等
        agentxx::plugin::registerBuiltinToolRenderers(*mgr2);
        {
            auto reg = mgr2->uiRegistrySnapshot();
            XX_TEST_EXPECT_TRUE(reg != nullptr);
            if (reg) {
                XX_TEST_EXPECT_EQ(reg->builtinToolRenderers.size(), size_t{2});
                XX_TEST_EXPECT_TRUE(reg->toolRenderers.empty());
                for (const auto& r : reg->builtinToolRenderers) {
                    XX_TEST_EXPECT_TRUE(r.builtin);
                    XX_TEST_EXPECT_EQ(
                        r.plugin,
                        std::string{agentxx::plugin::kBuiltinRendererOwner}
                    );
                    XX_TEST_EXPECT_TRUE(r.collapsedFn != nullptr);
                }
            }
        }
        agentxx::plugin::registerBuiltinToolRenderers(*mgr2);
        XX_TEST_EXPECT_EQ(mgr2->uiRegistrySnapshot()->builtinToolRenderers.size(), size_t{2});

        // 28.3 share_store new: 操作 + 新增 id (取自结果 JSON; 区间只有 get 才有)
        {
            auto res = co_await renderToolAsync(
                mgr2,
                "call_bs_new",
                "agentxx_share_store",
                R"({"opt":"new","text":"line1\nline2\nline3"})",
                R"({"id":7})",
                true,
                false,
                100
            );
            XX_TEST_EXPECT_TRUE(res.matched);
            XX_TEST_EXPECT_EQ(res.displayName, "Store");
            XX_TEST_EXPECT_EQ(res.summary, "new #7");
            XX_TEST_EXPECT_TRUE(res.items.empty()); ///< 展开体保持通用展示 (无 items)
        }

        // 28.4 运行中 (结果未回): 摘要不含新增 id
        {
            auto res = co_await renderToolAsync(
                mgr2,
                "call_bs_new_running",
                "agentxx_share_store",
                R"({"opt":"new","text":"a\nb"})",
                "",
                false,
                false,
                100
            );
            XX_TEST_EXPECT_TRUE(res.matched);
            XX_TEST_EXPECT_EQ(res.summary, "new");
        }

        // 28.5 get/set/delete: 区间只有 get 展示; 操作与 id 依次排列 (仅一侧分页参数时另一侧开放)
        {
            auto getRes = co_await renderToolAsync(
                mgr2,
                "call_bs_get",
                "agentxx_share_store",
                R"({"opt":"get","id":12,"offset":0,"limit":100})",
                "content",
                true,
                false,
                100
            );
            XX_TEST_EXPECT_EQ(getRes.displayName, "Store");
            XX_TEST_EXPECT_EQ(getRes.summary, "[0, 100] get #12");

            auto getOpen = co_await renderToolAsync(
                mgr2,
                "call_bs_get_open",
                "agentxx_share_store",
                R"({"opt":"get","id":12,"offset":40})",
                "",
                true,
                false,
                100
            );
            XX_TEST_EXPECT_EQ(getOpen.summary, "[40, ~] get #12");

            auto setRes = co_await renderToolAsync(
                mgr2,
                "call_bs_set",
                "agentxx_share_store",
                R"({"opt":"update","id":3,"text":"x\ny"})",
                "success",
                true,
                false,
                100
            );
            XX_TEST_EXPECT_EQ(setRes.summary, "update #3");

            // `delete` 操作已移除: 与其它未识别的 opt 一样原样展示 (不带 id 摘要)
            auto delRes = co_await renderToolAsync(
                mgr2,
                "call_bs_delete",
                "agentxx_share_store",
                R"({"opt":"delete","id":3})",
                "success",
                true,
                false,
                100
            );
            XX_TEST_EXPECT_EQ(delRes.summary, "delete");
        }

        // 28.6 参数未就绪 (空串 / 半截 JSON): 只提供显示名, 摘要为空 (不报错)
        {
            auto emptyArgs = co_await renderToolAsync(
                mgr2,
                "call_bs_empty",
                "agentxx_share_store",
                "",
                "",
                false,
                false,
                100
            );
            XX_TEST_EXPECT_FALSE(emptyArgs.matched); ///< 折叠内容为空 = 无自定义内容 (界面走默认渲染)
            XX_TEST_EXPECT_EQ(emptyArgs.displayName, "Store");
            XX_TEST_EXPECT_TRUE(emptyArgs.summary.empty());

            auto partialArgs = co_await renderToolAsync(
                mgr2,
                "call_bs_partial",
                "agentxx_share_store",
                R"({"opt":"ins)",
                "",
                false,
                false,
                100
            );
            XX_TEST_EXPECT_FALSE(partialArgs.matched);
            XX_TEST_EXPECT_EQ(partialArgs.displayName, "Store");
            XX_TEST_EXPECT_TRUE(partialArgs.summary.empty());
        }

        // 28.7 subagent 单发: 子代理名 + 任务首行 (多行只取首行)
        {
            auto res = co_await renderToolAsync(
                mgr2,
                "call_sa_single",
                "agentxx_subagent",
                R"({"subagent":"explorer","message":"fix login\ndetails"})",
                "",
                true,
                false,
                100
            );
            XX_TEST_EXPECT_TRUE(res.matched);
            XX_TEST_EXPECT_EQ(res.displayName, "Subagent");
            XX_TEST_EXPECT_EQ(res.summary, "explorer · fix login");
        }

        // 28.8 subagent 批量: 任务数 + 子代理名 (最多 3 个, 其余折叠为 ...)
        {
            auto batch = co_await renderToolAsync(
                mgr2,
                "call_sa_batch",
                "agentxx_subagent",
                R"({"tasks":[{"subagent":"explorer","message":"a"},{"subagent":"coder","message":"b"},{"subagent":"planner","message":"c"},{"subagent":"writer","message":"d"}]})",
                "",
                true,
                false,
                100
            );
            XX_TEST_EXPECT_EQ(batch.summary, "4 tasks: explorer, coder, planner, ...");
        }

        // 28.9 subagent 仅 messages 形态 (任务文本取最后一条 content) / 无名任务 /
        //      长文本截断
        {
            auto msgOnly = co_await renderToolAsync(
                mgr2,
                "call_sa_messages",
                "agentxx_subagent",
                R"({"subagent":"x","messages":[{"role":"user","content":"hi there"}]})",
                "",
                true,
                false,
                100
            );
            XX_TEST_EXPECT_EQ(msgOnly.summary, "x · hi there");

            // 无子代理名 (仅任务文本): 摘要直接接在 " ·" 之后, 不留双分隔符
            auto noName = co_await renderToolAsync(
                mgr2,
                "call_sa_noname",
                "agentxx_subagent",
                R"({"message":"summarize the module"})",
                "",
                true,
                false,
                100
            );
            XX_TEST_EXPECT_EQ(noName.summary, "summarize the module");

            // 宽度预算不足 (<20) 时按固定 80 列预算截断
            const std::string longTask(200, 't');
            auto              longRes = co_await renderToolAsync(
                mgr2,
                "call_sa_long",
                "agentxx_subagent",
                R"({"subagent":"x","message":")" + longTask + R"("})",
                "",
                true,
                false,
                10
            );
            XX_TEST_EXPECT_TRUE(longRes.summary.size() < longTask.size());
            XX_TEST_EXPECT_TRUE(longRes.summary.find("...") != std::string::npos);
        }

        // 28.10 匹配优先级: 插件注册项优先于主程序内置项 (同一 tool_name)
        {
            // 插件侧渲染回调是异步操作契约: 内容经回调自己的 notify 交回
            auto pluginCollapsed = +[](void*                         ud,
                                       const agentxx::plugin::ClientToolRenderInput* input,
                                       const PluginxxOperatorNotify* notify,
                                       PluginxxString*               errorOut) -> int32_t {
                (void)ud;
                (void)input;
                (void)errorOut;
                auto text = agentxx::plugin::PluginStringView::fromCstr("plugin wins");
                if (notify && notify->done) {
                    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, &text);
                }
                return 0;
            };

            agentxx::plugin::ClientUiRegistry reg;
            reg.builtinToolRenderers.push_back({
                .plugin      = std::string{agentxx::plugin::kBuiltinRendererOwner},
                .toolName    = "agentxx_share_store",
                .displayName = "Store",
                .collapsedFn = &agentxx::plugin::builtinRenderShareStoreCollapsed,
                .builtin     = true,
            });
            reg.toolRenderers.push_back({
                .plugin      = "some_plugin",
                .toolName    = "agentxx_share_store",
                .displayName = "PluginStore",
                .collapsedFn = pluginCollapsed,
            });

            const std::string args = R"({"opt":"new","text":"a\nb"})";
            // 显示名: 插件注册项优先
            XX_TEST_EXPECT_EQ(
                agentxx::plugin::toolDisplayNameFor(&reg, {}, "agentxx_share_store"),
                std::string_view{"PluginStore"}
            );

            // 无语义缓存 = 同步执行回调 (不进入异步请求路径)
            auto withPlugin = agentxx::plugin::renderClientToolCollapsed(
                &reg,
                nullptr,
                "call_prec_1",
                "agentxx_share_store",
                args,
                "",
                true,
                false,
                100
            );
            XX_TEST_EXPECT_EQ(withPlugin.text, std::string{"plugin wins"});
            XX_TEST_EXPECT_FALSE(withPlugin.pending);

            // 插件项不存在时回退主程序内置渲染器
            reg.toolRenderers.clear();
            XX_TEST_EXPECT_EQ(
                agentxx::plugin::toolDisplayNameFor(&reg, {}, "agentxx_share_store"),
                std::string_view{"Store"}
            );
            auto builtinOnly = agentxx::plugin::renderClientToolCollapsed(
                &reg,
                nullptr,
                "call_prec_2",
                "agentxx_share_store",
                args,
                "",
                true,
                false,
                100
            );
            XX_TEST_EXPECT_EQ(builtinOnly.text, std::string{"new"});
            XX_TEST_EXPECT_FALSE(builtinOnly.pending);
        }

        // 28.11 内置渲染器不受插件禁用/卸载影响 (进程级注册, 与插件生命周期无关)
        {
            auto exInst = co_await mgr2->loadNativeAsync(findExamplePluginPath());
            XX_TEST_EXPECT_TRUE(exInst != nullptr);
            if (exInst) {
                co_await mgr2->unloadAsync("example_plugin");
                XX_TEST_EXPECT_TRUE(mgr2->find("example_plugin") == nullptr);
            }
            auto afterUnload = co_await renderToolAsync(
                mgr2,
                "call_bs_after_unload",
                "agentxx_share_store",
                R"({"opt":"new","text":"a\nb"})",
                R"({"id":9})",
                true,
                false,
                100
            );
            XX_TEST_EXPECT_TRUE(afterUnload.matched);
            XX_TEST_EXPECT_EQ(afterUnload.summary, "new #9");
            XX_TEST_EXPECT_EQ(mgr2->uiRegistrySnapshot()->builtinToolRenderers.size(), size_t{2});
        }
    }

    // ---- 29. 接口协商: TUI 主程序声明 ui 表整体 + 内置插件 client 侧声明全被满足 ----
    // "agentxx.client.ui" 是 ui 表 IID (表整体): 只有覆盖表内全部子能力的主程序才声明它;
    // 内置插件的 client 入口只用到工具消息特化渲染, 清单按细粒度名
    // agentxx.client.msg_decor 声明。两者必须互相对应 —— 否则 TUI 启动会误报
    // "optional interface `...` not supported by host, related features disabled"
    // (或 require 该接口的插件被跳过加载)。
    {
        namespace fs = std::filesystem;
        namespace pi = agentxx::plugin::plugin_interfaces;

        agentxx::client::TuiPluginAdapter tuiAdapter{
            std::weak_ptr<agentxx::client::TUIClientAgentIO>{}
        };
        const auto tuiIfaces = tuiAdapter.supportedInterfaces();

        // 29.1 表整体 + 全部已实现子能力 (第三方插件按表名声明时同样不再误报)
        const std::string_view tuiUiInterfaces[] = {
            pi::ClientUi,
            pi::ClientStatusItem,
            pi::ClientPanel,
            pi::ClientToast,
            pi::ClientInfoSection,
            pi::ClientCommand,
            pi::ClientMsgDecor,
            pi::ClientAction,
            pi::ClientOverlay,
            pi::ClientComponents,
            pi::ClientForm,
            pi::ClientLayout,
            pi::ClientTimer,
            pi::ClientKeybind,
        };
        for (auto ifaceName : tuiUiInterfaces) {
            XX_TEST_EXPECT_TRUE(tuiIfaces.contains(std::string{ifaceName}));
        }

        // 29.2 构建产物中的内置插件清单: client 侧声明 (require + optional) 必须全部
        // 被 TUI 支持集满足 (未满足即启动告警/插件被跳过, 属于本次修复要消除的情况)
        const fs::path  pluginsRoot = fs::path{findPluginPath("agentxx_filesystem")}.parent_path();
        std::error_code ec;
        if (fs::is_directory(pluginsRoot, ec)) {
            int checkedPlugins = 0;
            for (const auto& entry : fs::directory_iterator{pluginsRoot, ec}) {
                if (ec || !entry.is_directory()) {
                    continue;
                }
                if (!fs::exists(entry.path() / "plugin.yaml", ec)) {
                    continue;
                }
                std::string                               pluginName;
                std::string                               entryName;
                std::vector<std::string>                  depends;
                std::vector<std::string>                  optionalDepends;
                agentxx::plugin::PluginManifestInterfaces decl;
                if (!agentxx::plugin::parsePluginManifest(
                        entry.path(),
                        pluginName,
                        entryName,
                        depends,
                        optionalDepends,
                        nullptr,
                        &decl
                    )) {
                    continue;
                }
                auto check = agentxx::plugin::checkInterfacesForSide(decl, tuiIfaces, false);
                if (!check.satisfied || !check.missingOptional.empty()) {
                    std::string missing;
                    for (const auto& n : check.missingRequired) {
                        missing += " " + n;
                    }
                    for (const auto& n : check.missingOptional) {
                        missing += " " + n;
                    }
                    XX_LOGW(
                        "[test] plugin `{}` client 侧接口声明未被 TUI 主程序支持: {}",
                        pluginName,
                        missing
                    );
                }
                XX_TEST_EXPECT_TRUE(check.satisfied);
                XX_TEST_EXPECT_TRUE(check.missingOptional.empty());
                ++checkedPlugins;
            }
            XX_TEST_EXPECT_TRUE(checkedPlugins > 0);
        }
    }
    // ---- 30. 定时器表 (agentxx.client.timer): 注册/触发/门控/取消/生命周期 ----
    {
        auto mgrT = std::make_shared<agentxx::plugin::ClientPluginManager>(ex);
        mgrT->setUiAdapter(std::make_shared<MockPluginUiAdapter>());
        auto instT = co_await mgrT->loadNativeAsync(findExamplePluginPath());
        XX_TEST_EXPECT_TRUE(instT != nullptr);
        auto ifaceTimer = agentxx::plugin::queryInterface<AgentxxClientTimerIface>(
            instT->hostView(),
            AGENTXX_IFACE_CLIENT_TIMER
        );
        XX_TEST_EXPECT_TRUE(ifaceTimer != nullptr);
        if (instT && ifaceTimer) {
            XX_TEST_EXPECT_EQ(ifaceTimer->version, AGENTXX_IFACE_CLIENT_TIMER_VERSION);
            XX_TEST_EXPECT_TRUE(ifaceTimer->set_timer != nullptr);
            XX_TEST_EXPECT_TRUE(ifaceTimer->cancel_timer != nullptr);

            struct Counter {
                int ticks = 0;
            } counter;

            auto makeSpec = [&](int intervalMs, int repeat, bool pauseHidden) {
                AgentxxTimerSpec spec{};
                spec.version           = 1;
                spec.interval_ms       = intervalMs;
                spec.repeat            = repeat;
                spec.pause_when_hidden = pauseHidden ? 1 : 0;
                spec.owner_id = agentxx::plugin::PluginStringView::from("example_plugin.panel", 20);
                spec.on_timer = [](void* ud) {
                    ++static_cast<Counter*>(ud)->ticks;
                };
                spec.user_data = &counter;
                return spec;
            };

            // 30.1 基本校验: 版本/回调缺失 → 拒绝
            {
                auto bad    = makeSpec(60, 0, false);
                bad.version = 2;
                XX_TEST_EXPECT_TRUE(timerSet(instT->hostView(), ifaceTimer, &bad) == nullptr);
                bad.version  = 1;
                bad.on_timer = nullptr;
                XX_TEST_EXPECT_TRUE(timerSet(instT->hostView(), ifaceTimer, &bad) == nullptr);
            }

            // 30.2 一次性定时器: 触发恰好一次 (间隔低于下限时按 50ms 收敛)
            {
                counter.ticks = 0;
                auto  spec    = makeSpec(10, 0, false);
                auto* oneShot = timerSet(instT->hostView(), ifaceTimer, &spec);
                XX_TEST_EXPECT_TRUE(oneShot != nullptr);
                co_await sleepMs(200);
                XX_TEST_EXPECT_EQ(counter.ticks, 1);
                co_await sleepMs(120);
                XX_TEST_EXPECT_EQ(counter.ticks, 1);                  // 不再触发
                timerCancel(instT->hostView(), ifaceTimer, oneShot); // 已结束: 幂等
            }

            // 30.3 周期定时器: repeat 次数上限到达后自动停止
            {
                counter.ticks  = 0;
                auto  spec     = makeSpec(50, 3, false);
                auto* periodic = timerSet(instT->hostView(), ifaceTimer, &spec);
                XX_TEST_EXPECT_TRUE(periodic != nullptr);
                co_await sleepMs(400);
                XX_TEST_EXPECT_EQ(counter.ticks, 3);
                co_await sleepMs(150);
                XX_TEST_EXPECT_EQ(counter.ticks, 3);
            }

            // 30.4 区域可见性门控 (is_visible + pause_when_hidden)
            {
                XX_TEST_EXPECT_EQ(timerIsVisible(instT->hostView(), ifaceTimer, "example_plugin.panel"), 0);
                counter.ticks = 0;
                auto  spec    = makeSpec(50, 0, true); // 不可见 → 跳过回调 (计时继续)
                auto* hidden  = timerSet(instT->hostView(), ifaceTimer, &spec);
                XX_TEST_EXPECT_TRUE(hidden != nullptr);
                co_await sleepMs(160);
                XX_TEST_EXPECT_EQ(counter.ticks, 0);

                mgrT->reportRegionVisible("example_plugin.panel", true);
                XX_TEST_EXPECT_EQ(timerIsVisible(instT->hostView(), ifaceTimer, "example_plugin.panel"), 1);
                XX_TEST_EXPECT_TRUE(mgrT->isRegionVisible("example_plugin.panel"));
                // 可见性变化不影响已在计时的一次性定时器: 它会在下一次到期时回调
                counter.ticks = 0;
                co_await sleepMs(200);
                XX_TEST_EXPECT_EQ(counter.ticks, 1);
                // 再次上报相同值: 不做任何事 (值变化才更新)
                mgrT->reportRegionVisible("example_plugin.panel", true);
                XX_TEST_EXPECT_TRUE(mgrT->regionVisibility().at("example_plugin.panel"));
                // 不可见时取消 (cancel_timer 任何时候都有效)
                counter.ticks  = 0;
                auto  spec2    = makeSpec(50, 5, false);
                auto* cancelMe = timerSet(instT->hostView(), ifaceTimer, &spec2);
                XX_TEST_EXPECT_TRUE(cancelMe != nullptr);
                timerCancel(instT->hostView(), ifaceTimer, cancelMe);
                timerCancel(instT->hostView(), ifaceTimer, cancelMe); // 重复取消幂等
                co_await sleepMs(200);
                XX_TEST_EXPECT_EQ(counter.ticks, 0);
            }

            // 30.5 动画等级门控: Disabled 时拒绝注册 (插件据返回值降级)
            {
                mgrT->setAnimationEnabled(false);
                XX_TEST_EXPECT_FALSE(mgrT->animationEnabled());
                auto spec = makeSpec(50, 0, false);
                XX_TEST_EXPECT_TRUE(timerSet(instT->hostView(), ifaceTimer, &spec) == nullptr);
                mgrT->setAnimationEnabled(true);
                auto ok = timerSet(instT->hostView(), ifaceTimer, &spec);
                XX_TEST_EXPECT_TRUE(ok != nullptr);
                timerCancel(instT->hostView(), ifaceTimer, ok);
            }

            // 30.6 单实例数量上限 (超出返回 NULL, 插件自行降级)
            {
                std::vector<AgentxxTimer*> timers;
                for (size_t i = 0; i < 8; ++i) {
                    auto  spec = makeSpec(1000, 0, false);
                    auto* t    = timerSet(instT->hostView(), ifaceTimer, &spec);
                    if (t) {
                        timers.push_back(t);
                    }
                }
                XX_TEST_EXPECT_EQ(timers.size(), size_t{8});
                auto spec = makeSpec(1000, 0, false);
                XX_TEST_EXPECT_TRUE(timerSet(instT->hostView(), ifaceTimer, &spec) == nullptr);
                for (auto* t : timers) {
                    timerCancel(instT->hostView(), ifaceTimer, t);
                }
            }

            // 30.7 生命周期: 卸载后定时器全部取消 (卸载会等待尚未返回的回调结束)
            {
                counter.ticks = 0;
                auto  spec    = makeSpec(50, 100, false);
                auto* live    = timerSet(instT->hostView(), ifaceTimer, &spec);
                XX_TEST_EXPECT_TRUE(live != nullptr);
                XX_TEST_EXPECT_TRUE(co_await mgrT->unloadAsync("example_plugin"));
                XX_TEST_EXPECT_EQ(instT->timers.size(), size_t{0});
                const int afterUnload = counter.ticks;
                co_await sleepMs(200);
                XX_TEST_EXPECT_EQ(counter.ticks, afterUnload); // 不再有回调
            }
        }
    }

    // ---- 31. 快捷键表 (agentxx.client.keybind): 注册/冲突/派发/注销/生命周期 ----
    {
        auto mgrK = std::make_shared<agentxx::plugin::ClientPluginManager>(ex);
        mgrK->setUiAdapter(std::make_shared<MockPluginUiAdapter>());
        auto instK = co_await mgrK->loadNativeAsync(findExamplePluginPath());
        XX_TEST_EXPECT_TRUE(instK != nullptr);
        auto ifaceKey = agentxx::plugin::queryInterface<AgentxxClientKeybindIface>(
            instK->hostView(),
            AGENTXX_IFACE_CLIENT_KEYBIND
        );
        XX_TEST_EXPECT_TRUE(ifaceKey != nullptr);
        if (instK && ifaceKey) {
            XX_TEST_EXPECT_EQ(ifaceKey->version, AGENTXX_IFACE_CLIENT_KEYBIND_VERSION);

            struct KeyCounter {
                int hits = 0;
            } keyCounter;

            auto makeSpec = [&](std::string_view keys, std::string_view desc) {
                AgentxxKeybindSpec spec{};
                spec.version = 1;
                spec.keys    = agentxx::plugin::PluginStringView::from(keys.data(), keys.size());
                spec.description
                    = agentxx::plugin::PluginStringView::from(desc.data(), desc.size());
                spec.on_keybind = [](void* ud) {
                    ++static_cast<KeyCounter*>(ud)->hits;
                };
                spec.user_data = &keyCounter;
                return spec;
            };

            // 31.1 键位规范化: 别名/顺序/大小写归一; 非法键位拒绝
            {
                XX_TEST_EXPECT_EQ(
                    agentxx::plugin::normalizeKeybindSpec("  Control + Alt + K "),
                    std::string{"ctrl+alt+k"}
                );
                XX_TEST_EXPECT_EQ(
                    agentxx::plugin::normalizeKeybindSpec("shift+ctrl+F9"),
                    std::string{"ctrl+shift+f9"}
                );
                XX_TEST_EXPECT_EQ(
                    agentxx::plugin::normalizeKeybindSpec("cmd+space"),
                    std::string{"super+space"}
                );
                XX_TEST_EXPECT_EQ(
                    agentxx::plugin::normalizeKeybindSpec("return"),
                    std::string{"enter"}
                );
                // 无修饰键的可打印字符不参与匹配; 未知修饰键/主键/空段非法
                XX_TEST_EXPECT_TRUE(agentxx::plugin::normalizeKeybindSpec("k").empty());
                XX_TEST_EXPECT_TRUE(agentxx::plugin::normalizeKeybindSpec("ctrl+").empty());
                XX_TEST_EXPECT_TRUE(agentxx::plugin::normalizeKeybindSpec("ctrl++k").empty());
                XX_TEST_EXPECT_TRUE(agentxx::plugin::normalizeKeybindSpec("hyper+k").empty());
                XX_TEST_EXPECT_TRUE(agentxx::plugin::normalizeKeybindSpec("ctrl+f99").empty());
                XX_TEST_EXPECT_TRUE(agentxx::plugin::normalizeKeybindSpec("ctrl").empty());
            }

            // 31.2 注册/查询/派发/注销
            {
                auto  spec = makeSpec("ctrl+alt+k", "test keybind");
                auto* bind = keybindRegister(instK->hostView(), ifaceKey, &spec);
                XX_TEST_EXPECT_TRUE(bind != nullptr);
                XX_TEST_EXPECT_TRUE(mgrK->hasKeybind("ctrl+alt+k"));
                XX_TEST_EXPECT_FALSE(mgrK->hasKeybind("ctrl+alt+j"));
                {
                    auto list = mgrK->keybinds();
                    XX_TEST_EXPECT_EQ(list.size(), size_t{1});
                    if (!list.empty()) {
                        XX_TEST_EXPECT_EQ(list[0].keys, std::string{"ctrl+alt+k"});
                        XX_TEST_EXPECT_EQ(list[0].plugin, std::string{"example_plugin"});
                        XX_TEST_EXPECT_EQ(list[0].description, std::string{"test keybind"});
                    }
                }
                // list_keybinds 表接口 (JSON)
                {
                    const std::string json = keybindList(instK->hostView(), ifaceKey);
                    XX_TEST_EXPECT_TRUE(!json.empty());
                    XX_TEST_EXPECT_TRUE(json.find("ctrl+alt+k") != std::string::npos);
                    XX_TEST_EXPECT_TRUE(json.find("example_plugin") != std::string::npos);
                }
                // 冲突: 同键位重复注册 (同实例或跨实例) 一律拒绝
                auto dup = makeSpec("ctrl+alt+k", "dup");
                XX_TEST_EXPECT_TRUE(
                    keybindRegister(instK->hostView(), ifaceKey, &dup) == nullptr
                );

                keyCounter.hits = 0;
                mgrK->postKeybindInvocation("ctrl+alt+k");
                co_await sleepMs(50);
                XX_TEST_EXPECT_EQ(keyCounter.hits, 1);
                // 未注册的键位不会派发
                mgrK->postKeybindInvocation("ctrl+alt+j");
                co_await sleepMs(50);
                XX_TEST_EXPECT_EQ(keyCounter.hits, 1);

                keybindUnregister(instK->hostView(), ifaceKey, bind);
                XX_TEST_EXPECT_FALSE(mgrK->hasKeybind("ctrl+alt+k"));
                XX_TEST_EXPECT_EQ(mgrK->keybinds().size(), size_t{0});
                mgrK->postKeybindInvocation("ctrl+alt+k");
                co_await sleepMs(50);
                XX_TEST_EXPECT_EQ(keyCounter.hits, 1);                 // 已注销: 不再回调
                keybindUnregister(instK->hostView(), ifaceKey, bind);  // 重复注销幂等
            }

            // 31.3 非法键位注册被拒绝
            {
                auto bad = makeSpec("q", "no modifier");
                XX_TEST_EXPECT_TRUE(
                    keybindRegister(instK->hostView(), ifaceKey, &bad) == nullptr
                );
                auto bad2 = makeSpec("ctrl+nosuchkey", "bad key");
                XX_TEST_EXPECT_TRUE(
                    keybindRegister(instK->hostView(), ifaceKey, &bad2) == nullptr
                );
            }

            // 31.4 卸载: 注册表摘除 (按键不再派发)
            {
                auto  spec = makeSpec("ctrl+shift+p", "unload case");
                auto* bind = keybindRegister(instK->hostView(), ifaceKey, &spec);
                XX_TEST_EXPECT_TRUE(bind != nullptr);
                XX_TEST_EXPECT_TRUE(mgrK->hasKeybind("ctrl+shift+p"));
                keyCounter.hits = 0;
                XX_TEST_EXPECT_TRUE(co_await mgrK->unloadAsync("example_plugin"));
                XX_TEST_EXPECT_FALSE(mgrK->hasKeybind("ctrl+shift+p"));
                mgrK->postKeybindInvocation("ctrl+shift+p");
                co_await sleepMs(50);
                XX_TEST_EXPECT_EQ(keyCounter.hits, 0);
            }
        }
    }

    // ---- 31.5 快捷键冲突记录 (注册受阻于已占用键位: 记录 + 生命周期清理) ----
    {
        /// 暴露 createInstance / detachDomainRegistrations 的测试管理器
        /// (伪实例只需要实例名与"禁用/卸载"生命周期入口)
        struct ConflictTestManager : agentxx::plugin::ClientPluginManager {
            using ClientPluginManager::ClientPluginManager;
            using ClientPluginManager::createInstance;
            using ClientPluginManager::detachDomainRegistrations;
        };

        auto mgrC = std::make_shared<ConflictTestManager>(ex);
        mgrC->setUiAdapter(std::make_shared<MockPluginUiAdapter>());
        auto owner = mgrC->createInstance("conflict_owner");
        auto other = mgrC->createInstance("conflict_other");

        auto makeBind = [](std::string_view keys, std::string_view desc) {
            AgentxxKeybindSpec spec{};
            spec.version     = 1;
            spec.keys        = agentxx::plugin::PluginStringView::from(keys.data(), keys.size());
            spec.description = agentxx::plugin::PluginStringView::from(desc.data(), desc.size());
            spec.on_keybind  = [](void*) {};
            return spec;
        };

        // 占用方注册成功: 无冲突
        auto  specOwner = makeBind("ctrl+alt+k", "owner bind");
        auto* bindOwner = mgrC->registerKeybind(owner.get(), &specOwner);
        XX_TEST_EXPECT_TRUE(bindOwner != nullptr);
        XX_TEST_EXPECT_TRUE(mgrC->uiRegistrySnapshot()->keybindConflicts.empty());

        // 同插件重复注册同一键位: 拒绝但不记冲突 (不是跨插件冲突)
        auto specSelfDup = makeBind("ctrl+alt+k", "self dup");
        XX_TEST_EXPECT_TRUE(mgrC->registerKeybind(owner.get(), &specSelfDup) == nullptr);
        XX_TEST_EXPECT_TRUE(mgrC->uiRegistrySnapshot()->keybindConflicts.empty());

        // 跨插件占用: 记一条冲突 (请求方 / 占用方)
        auto specOther = makeBind("ctrl+alt+k", "other bind");
        XX_TEST_EXPECT_TRUE(mgrC->registerKeybind(other.get(), &specOther) == nullptr);
        {
            auto snap = mgrC->uiRegistrySnapshot();
            XX_TEST_EXPECT_EQ(snap->keybindConflicts.size(), size_t{1});
            if (snap->keybindConflicts.size() == 1) {
                XX_TEST_EXPECT_EQ(snap->keybindConflicts[0].keys, std::string{"ctrl+alt+k"});
                XX_TEST_EXPECT_EQ(snap->keybindConflicts[0].plugin, std::string{"conflict_other"});
                XX_TEST_EXPECT_EQ(snap->keybindConflicts[0].owner, std::string{"conflict_owner"});
            }
        }
        // 同一 (键位, 请求方) 重复尝试只保留一条
        auto specOtherAgain = makeBind("ctrl+alt+k", "other bind again");
        XX_TEST_EXPECT_TRUE(mgrC->registerKeybind(other.get(), &specOtherAgain) == nullptr);
        XX_TEST_EXPECT_EQ(mgrC->uiRegistrySnapshot()->keybindConflicts.size(), size_t{1});

        // 占用方注销键位: 键位空出, 冲突记录随之消失, 请求方此时可以注册成功
        mgrC->unregisterKeybind(owner.get(), bindOwner);
        XX_TEST_EXPECT_TRUE(mgrC->uiRegistrySnapshot()->keybindConflicts.empty());
        auto  specOtherTake = makeBind("ctrl+alt+k", "other take over");
        auto* bindOther     = mgrC->registerKeybind(other.get(), &specOtherTake);
        XX_TEST_EXPECT_TRUE(bindOther != nullptr);
        XX_TEST_EXPECT_EQ(mgrC->uiRegistrySnapshot()->keybindConflicts.size(), size_t{0});

        // 反向占用: 前占用方此时成为请求方
        auto specOwnerLose = makeBind("ctrl+alt+k", "owner lose");
        XX_TEST_EXPECT_TRUE(mgrC->registerKeybind(owner.get(), &specOwnerLose) == nullptr);
        XX_TEST_EXPECT_EQ(mgrC->uiRegistrySnapshot()->keybindConflicts.size(), size_t{1});

        // 占用方被摘除 (插件禁用/卸载): 键位空出 + 冲突记录清空
        mgrC->detachDomainRegistrations(other.get());
        XX_TEST_EXPECT_TRUE(mgrC->uiRegistrySnapshot()->keybindConflicts.empty());
        XX_TEST_EXPECT_EQ(mgrC->uiRegistrySnapshot()->keybinds.size(), size_t{0});

        // 请求方被摘除: 记录作废 (重新启用后由插件 start 事务重新尝试)
        auto specOtherBack = makeBind("ctrl+alt+k", "other back");
        XX_TEST_EXPECT_TRUE(mgrC->registerKeybind(other.get(), &specOtherBack) != nullptr);
        auto specOwnerAgain = makeBind("ctrl+alt+k", "owner again");
        XX_TEST_EXPECT_TRUE(mgrC->registerKeybind(owner.get(), &specOwnerAgain) == nullptr);
        XX_TEST_EXPECT_EQ(mgrC->uiRegistrySnapshot()->keybindConflicts.size(), size_t{1});
        mgrC->detachDomainRegistrations(owner.get()); // 只摘除请求方
        XX_TEST_EXPECT_TRUE(mgrC->uiRegistrySnapshot()->keybindConflicts.empty());
        XX_TEST_EXPECT_EQ(mgrC->uiRegistrySnapshot()->keybinds.size(), size_t{1}); // 占用方键位仍在
    }

    // ---- 32. UI 描述体积上限 (1 MiB: 入口拒绝 + 注册表不变) ----
    {
        auto mgr = std::make_shared<agentxx::plugin::ClientPluginManager>(ex);
        mgr->setUiAdapter(std::make_shared<MockPluginUiAdapter>());
        auto inst = co_await mgr->loadNativeAsync(findExamplePluginPath());
        XX_TEST_EXPECT_TRUE(inst != nullptr);
        auto* ifaceUi = agentxx::plugin::queryInterface<AgentxxClientUiIface>(
            inst->hostView(),
            AGENTXX_IFACE_CLIENT_UI
        );
        XX_TEST_EXPECT_TRUE(ifaceUi != nullptr);
        if (inst && ifaceUi) {
            // 超大描述: 单条 JSON 超过 1 MiB (主程序在入口处直接拒绝, 不解析不落表)
            const std::string huge = std::string{"{\"items\":[{\"kind\":\"text\",\"text\":\""}
                                     + std::string(1024 * 1024 + 64, 'x') + "\"}]}";
            XX_TEST_EXPECT_TRUE(huge.size() > agentxx::plugin::kUiJsonMaxBytes);

            PluginxxStringView idSv
                = agentxx::plugin::PluginStringView::from(std::string_view{"example_plugin.big"});
            PluginxxStringView propSv
                = agentxx::plugin::PluginStringView::from(std::string_view{"{\"title\":\"Big\"}"});
            auto* panel = uiRegisterPanel(
                inst->hostView(),
                ifaceUi,
                "example_plugin.big",
                std::string_view{propSv.data, static_cast<size_t>(propSv.size)}
            );
            (void)idSv;
            XX_TEST_EXPECT_TRUE(panel != nullptr);
            if (panel) {
                // 正常大小先写一次: 后续"拒绝"要与这次的内容对比
                const std::string small{"{\"items\":[{\"kind\":\"text\",\"text\":\"ok\"}]}"};
                XX_TEST_EXPECT_EQ(uiUpdatePanel(inst->hostView(), ifaceUi, panel, small), 0);

                XX_TEST_EXPECT_TRUE(uiUpdatePanel(inst->hostView(), ifaceUi, panel, huge) != 0);

                // 注册表内容保持为上一次成功写入 (拒绝不留半截状态)
                {
                    auto                                reg   = mgr->uiRegistrySnapshot();
                    const agentxx::plugin::ClientPanel* found = nullptr;
                    for (const auto& p : reg->panels) {
                        if (p.id == "example_plugin.big") {
                            found = &p;
                        }
                    }
                    XX_TEST_EXPECT_TRUE(found != nullptr);
                    if (found) {
                        XX_TEST_EXPECT_EQ(found->items.size(), size_t{1});
                        XX_TEST_EXPECT_EQ(
                            found->items[0].value("text", std::string{}),
                            std::string{"ok"}
                        );
                        XX_TEST_EXPECT_EQ(found->version, uint64_t{1}); // 只有第一次成功更新
                    }
                }
            }
        }
        // 收尾: 卸载实例 (管理器析构时不留待关闭的实例)
        XX_TEST_EXPECT_TRUE(co_await mgr->unloadAsync("example_plugin"));
    }

    // ---- 33. 定时器同周期合并: 回调未返回时到期的那几次不排队追补 ----
    //
    // 用**独立 io_context 并由本线程驱动**: 回调在插件执行线程上阻塞 370ms (不是
    // 周期的整数倍, 让阻塞结束落在两个到期点之间), 若借用测试共享的 io 上下文会推迟
    // 其它模块尚未处理的事件 (影响与本用例无关的时序); 用额外线程则需要处理停机,
    // 这里直接在主线程 run_for 驱动。
    {
        using clk = std::chrono::steady_clock;

        /// 回调观测: 触发次数 / 是否已经阻塞过 / "两个回调几乎同时开始"的次数
        ///
        /// 合并生效时回调之间至少隔一个周期; 未合并 (追赶式补发) 时积压的那几次会在
        /// 阻塞结束后连着跑 —— 相邻两次开始的间隔几乎为 0。
        struct Probe {
            std::atomic<int>     ticks{0};
            std::atomic<bool>    blocked{false};
            std::atomic<int>     burst{0};       ///< 相邻两次开始间隔 < 20ms 的次数
            std::atomic<int64_t> lastStartMs{0}; ///< 上次开始时刻 (steady_clock 毫秒)
        } probe;

        asio::io_context ioT;
        auto             work = asio::make_work_guard(ioT);

        auto mgrC = std::make_shared<agentxx::plugin::ClientPluginManager>(ioT.get_executor());
        mgrC->setUiAdapter(std::make_shared<MockPluginUiAdapter>());

        std::atomic<bool> loaded{false};
        asio::co_spawn(
            ioT,
            keepAliveRun([&]() -> asio::awaitable<void> {
                auto instC = co_await mgrC->loadNativeAsync(findExamplePluginPath());
                if (!instC) {
                    co_return;
                }
                auto* ifaceTimerC = agentxx::plugin::queryInterface<AgentxxClientTimerIface>(
                    instC->hostView(),
                    AGENTXX_IFACE_CLIENT_TIMER
                );
                if (ifaceTimerC == nullptr) {
                    co_return;
                }
                // 周期定时器 (interval=50ms): 第一次回调占住插件执行线程 370ms ——
                // 期间每次到期的周期触发都被跳过 (不排队追补), 回调返回后从下一个
                // 周期继续
                AgentxxTimerSpec periodic{};
                periodic.version     = 1;
                periodic.interval_ms = 50;
                periodic.repeat      = 100; // 周期 (上限远大于本用例观测窗口)
                periodic.on_timer    = [](void* ud) {
                    auto* p = static_cast<Probe*>(ud);
                    p->ticks.fetch_add(1);
                    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                              clk::now().time_since_epoch()
                    )
                                              .count();
                    const int64_t prev = p->lastStartMs.exchange(nowMs);
                    if (prev != 0 && nowMs - prev < 20) {
                        // 两次回调几乎同时开始: 这是被追补的那一批
                        p->burst.fetch_add(1);
                    }
                    if (!p->blocked.load()) {
                        // 第一次: 占住插件执行线程 370ms (远大于 50ms 周期)
                        std::this_thread::sleep_for(std::chrono::milliseconds{370});
                        p->blocked.store(true);
                    }
                };
                periodic.user_data = &probe;
                (void)timerSet(instC->hostView(), ifaceTimerC, &periodic);
                loaded.store(true);
            }),
            asio::detached
        );

        // 主线程驱动 io 事件: 让定时器按各自周期触发 (阻塞回调也在此发生)
        // 窗口取 1500ms: 首个回调要在插件执行线程上阻塞 370ms, 之后才恢复周期; 机器
        // 负载高 (并行构建/其它用例) 时 700ms 只够 2~3 次, 断言会随负载抖动
        {
            const auto deadline = clk::now() + std::chrono::milliseconds{1500};
            while (clk::now() < deadline) {
                ioT.run_for(std::chrono::milliseconds{20});
            }
        }
        XX_TEST_EXPECT_TRUE(loaded.load());
        XX_TEST_EXPECT_TRUE(probe.blocked.load());
        // 合并生效: 阻塞期间到期的触发被丢弃, 回调之间至少隔一个周期
        // (未合并时积压的那几次会在阻塞结束后连着跑 → burst > 0)
        XX_TEST_EXPECT_EQ(probe.burst.load(), 0);
        // 定时器仍然活着并持续触发 (合并不等于停摆)
        XX_TEST_EXPECT_TRUE(probe.ticks.load() >= 4);

        // 收尾: 卸载实例 (等待尚未返回的回调结束), 之后不再有待处理事件
        std::atomic<bool> unloaded{false};
        asio::co_spawn(
            ioT,
            keepAliveRun([&]() -> asio::awaitable<void> {
                unloaded.store(co_await mgrC->unloadAsync("example_plugin"));
            }),
            asio::detached
        );
        {
            const auto deadline = clk::now() + std::chrono::milliseconds{2000};
            while (clk::now() < deadline && !unloaded.load()) {
                ioT.run_for(std::chrono::milliseconds{20});
            }
        }
        XX_TEST_EXPECT_TRUE(unloaded.load());
        work.reset();
        ioT.run(); // 释放剩余事件 (无待处理事件时立即返回)
        mgrC.reset();
    }

        // ---- 34. JS 脚本作为 client 插件: 工具行特化渲染由脚本提供 ----
    //
    // 加载顺序 = 依赖顺序: 先 JS 引擎 (client 入口), 再 example_js (它的 client
    // 入口把 plugin.js 交给引擎执行)。脚本用 `agentxx.side` 判定跑在 client 侧,
    // 注册 `js_hello` 的渲染器; 内容由脚本回调给出 (io 线程 → JS 线程同步桥)。
    {
        auto mgrJs    = std::make_shared<agentxx::plugin::ClientPluginManager>(ex);
        auto adapterJs = std::make_shared<MockPluginUiAdapter>();
        mgrJs->setUiAdapter(adapterJs);

        auto jsEngine = co_await mgrJs->loadNativeAsync(findPluginPath("agentxx_javascript_engine"));
        XX_TEST_EXPECT_TRUE(jsEngine != nullptr);
        if (jsEngine) {
            auto jsShell = co_await mgrJs->loadNativeAsync(findPluginPath("example_js"));
            XX_TEST_EXPECT_TRUE(jsShell != nullptr);
            if (jsShell) {
                // 注册表: 脚本注册的渲染项已生效 (显示名来自脚本)
                {
                    auto                                        reg = mgrJs->uiRegistrySnapshot();
                    const agentxx::plugin::ClientToolRenderReg* hit = nullptr;
                    for (const auto& r : reg->toolRenderers) {
                        if (r.toolName == "js_hello") {
                            hit = &r;
                        }
                    }
                    XX_TEST_EXPECT_TRUE(hit != nullptr);
                    if (hit) {
                        XX_TEST_EXPECT_EQ(hit->plugin, std::string{"example_js"});
                        XX_TEST_EXPECT_EQ(hit->displayName, std::string{"JS Hello"});
                        XX_TEST_EXPECT_TRUE(hit->collapsedFn != nullptr);
                        XX_TEST_EXPECT_TRUE(hit->expandedFn != nullptr);
                    }
                }

                // 折叠内容 (运行中 / 已完成): 两次内容不同 → 回调真的在执行
                auto runningJs = co_await renderToolAsync(
                    mgrJs,
                    "call-js-1",
                    "js_hello",
                    R"({"name":"Ada"})",
                    "",
                    false,
                    false,
                    80
                );
                XX_TEST_EXPECT_EQ(runningJs.displayName, std::string{"JS Hello"});
                XX_TEST_EXPECT_TRUE(runningJs.matched);
                XX_TEST_EXPECT_EQ(runningJs.summary, std::string{"greeting Ada..."});

                auto doneJs = co_await renderToolAsync(
                    mgrJs,
                    "call-js-2",
                    "js_hello",
                    R"({"name":"Ada"})",
                    R"({"greeting":"Hello, Ada!"})",
                    true,
                    false,
                    80
                );
                XX_TEST_EXPECT_EQ(doneJs.summary, std::string{"greeted Ada"});

                // 展开内容: items 由脚本给出 (第一项是标题)
                XX_TEST_EXPECT_TRUE(doneJs.items.is_array());
                XX_TEST_EXPECT_TRUE(doneJs.items.size() >= 2);
                if (doneJs.items.is_array() && doneJs.items.size() >= 2) {
                    XX_TEST_EXPECT_EQ(
                        doneJs.items[0].value("text", std::string{}),
                        std::string{"JS Hello (client side)"}
                    );
                    XX_TEST_EXPECT_EQ(
                        doneJs.items[1].value("text", std::string{}),
                        std::string{"target: Ada"}
                    );
                }

                // 只给了折叠内容的其它工具仍走默认渲染 (未注册 → 不命中)
                auto otherJs = co_await renderToolAsync(
                    mgrJs,
                    "call-js-3",
                    "js_async_wait",
                    "{}",
                    "{}",
                    true,
                    false,
                    80
                );
                XX_TEST_EXPECT_EQ(otherJs.displayName, std::string{"js_async_wait"});
                XX_TEST_EXPECT_TRUE(!otherJs.matched);

                // 展示面: 状态栏项 / Info 段落 / 面板 / 命令 (脚本注册, 句柄是整数 token)
                {
                    auto reg = mgrJs->uiRegistrySnapshot();
                    bool hasStatus = false;
                    std::string statusText;
                    for (const auto& s : reg->statusItems) {
                        if (s.id == "js.demo") {
                            hasStatus  = true;
                            statusText = s.text;
                        }
                    }
                    XX_TEST_EXPECT_TRUE(hasStatus);
                    XX_TEST_EXPECT_EQ(statusText, std::string{"js: ready"});

                    bool hasInfo = false;
                    for (const auto& sec : reg->infoSections) {
                        if (sec.id == "js.demo.info") {
                            hasInfo = true;
                            XX_TEST_EXPECT_EQ(sec.title, std::string{"JS Demo"});
                        }
                    }
                    XX_TEST_EXPECT_TRUE(hasInfo);

                    bool hasPanel = false;
                    for (const auto& p : reg->panels) {
                        if (p.id == "js.demo.panel") {
                            hasPanel = true;
                        }
                    }
                    XX_TEST_EXPECT_TRUE(hasPanel);
                    XX_TEST_EXPECT_TRUE(mgrJs->hasCommand("jsdemo"));
                }

                // 命令: 脚本 execute 返回动作 JSON; 脚本里的 showToast 是表调用
                // (JS 线程 → io 线程), 稍后才到达 —— 等它一下再断言
                {
                    const int before = adapterJs->toastCount();
                    mgrJs->invokeCommand("jsdemo", "{}");
                    for (int i = 0; i < 500 && adapterJs->toastCount() == before; ++i) {
                        co_await sleepMs(4);
                    }
                    XX_TEST_EXPECT_EQ(adapterJs->toastCount(), before + 1);
                    XX_TEST_EXPECT_EQ(adapterJs->lastToast(), std::string{"hello from JS"});
                }

                // 快捷键: 派发后脚本更新状态栏文本
                mgrJs->postKeybindInvocation("ctrl+alt+j");
                co_await sleepMs(80);
                {
                    auto        reg  = mgrJs->uiRegistrySnapshot();
                    std::string text = "(missing)";
                    for (const auto& s : reg->statusItems) {
                        if (s.id == "js.demo") {
                            text = s.text;
                        }
                    }
                    XX_TEST_EXPECT_EQ(text, std::string{"js: key 1"});
                }

                // 按钮动作: 面板 button 派发 → 脚本回调 (fire-and-forget)
                {
                    const int before = adapterJs->toastCount();
                    mgrJs->dispatchAction("example_js", "js.demo.panel", "js.demo.hello", "{}");
                    for (int i = 0; i < 60 && adapterJs->toastCount() == before; ++i) {
                        co_await sleepMs(5);
                    }
                    XX_TEST_EXPECT_EQ(adapterJs->toastCount(), before + 1);
                    XX_TEST_EXPECT_EQ(
                        adapterJs->lastToast(),
                        std::string{"JS action: js.demo.hello"}
                    );
                }

                // 定时器: 主程序驱动 (60ms × 2; 区域不可见时顺延, 这里先上报可见)
                mgrJs->reportRegionVisible("js.demo.info", true);
                {
                    bool fired = false;
                    for (int i = 0; i < 120 && !fired; ++i) {
                        co_await sleepMs(10);
                        auto reg = mgrJs->uiRegistrySnapshot();
                        for (const auto& sec : reg->infoSections) {
                            if (sec.id == "js.demo.info"
                                && sec.items.dump().find("timer tick 2") != std::string::npos) {
                                fired = true;
                            }
                        }
                    }
                    XX_TEST_EXPECT_TRUE(fired);
                }

                // 注销与查询入口: 快捷键回调里调表 (投递式, io 线程不在等, 不会互等)
                {
                    const int before = adapterJs->toastCount();
                    mgrJs->postKeybindInvocation("ctrl+alt+u");
                    for (int i = 0; i < 100 && adapterJs->toastCount() == before; ++i) {
                        co_await sleepMs(5);
                    }
                    XX_TEST_EXPECT_EQ(adapterJs->toastCount(), before + 1);
                    // listKeybinds() 拿到数组 (本例两个键位), isRegionVisible 前面已上报
                    XX_TEST_EXPECT_TRUE(
                        adapterJs->lastToast().rfind("cleanup: binds=", 0) == 0
                    );
                    XX_TEST_EXPECT_TRUE(
                        adapterJs->lastToast().find("visible=true") != std::string::npos
                    );
                    // 状态栏项 / 面板 / Info 段落与另一个键位都已注销
                    auto reg = mgrJs->uiRegistrySnapshot();
                    bool hasStatus = false;
                    bool hasInfo   = false;
                    bool hasPanel  = false;
                    for (const auto& s : reg->statusItems) {
                        if (s.id == "js.demo") {
                            hasStatus = true;
                        }
                    }
                    for (const auto& sec : reg->infoSections) {
                        if (sec.id == "js.demo.info") {
                            hasInfo = true;
                        }
                    }
                    for (const auto& p : reg->panels) {
                        if (p.id == "js.demo.panel") {
                            hasPanel = true;
                        }
                    }
                    XX_TEST_EXPECT_TRUE(!hasStatus);
                    XX_TEST_EXPECT_TRUE(!hasInfo);
                    XX_TEST_EXPECT_TRUE(!hasPanel);
                    XX_TEST_EXPECT_TRUE(!mgrJs->hasKeybind("ctrl+alt+j"));
                    XX_TEST_EXPECT_TRUE(mgrJs->hasKeybind("ctrl+alt+u"));
                }

                // 网络: 要结果的动作给 Promise (`net.info` / `netCall`), 结果经 toast 补上
                // - 表入口是异步操作契约: 命令回调只同步返回一句"探测中"
                // - 命令动作由脚本异步交回 (JS 线程 → io 线程), 先等第一条 toast
                {
                    const int before = adapterJs->toastCount();
                    mgrJs->invokeCommand("jsnet", "{}");
                    for (int i = 0; i < 100 && adapterJs->toastCount() == before; ++i) {
                        co_await sleepMs(5);
                    }
                    XX_TEST_EXPECT_TRUE(adapterJs->toastCount() >= before + 1);
                    // 第二条来自 Promise 链 (`netInfo` → `netCall` 失败也按 `ok` 回)
                    for (int i = 0; i < 300
                         && adapterJs->lastToast().rfind("net info ok=", 0) != 0;
                         ++i) {
                        co_await sleepMs(10);
                    }
                    XX_TEST_EXPECT_TRUE(adapterJs->toastCount() >= before + 2);
                    XX_TEST_EXPECT_TRUE(
                        adapterJs->lastToast().rfind("net info ok=true", 0) == 0
                    );
                    // 未知 op 是参数层错误, 脚本侧只看 `ok`
                    XX_TEST_EXPECT_TRUE(
                        adapterJs->lastToast().find("bogus ok=false") != std::string::npos
                    );
                }

                // 网络: 回环往返 (脚本起服务 + 静态路由 → netFetch → Promise 回 JS 线程)
                {
                    const int before = adapterJs->toastCount();
                    mgrJs->dispatchAction("example_js", "js.demo.net", "fetch", "{}");
                    for (int i = 0; i < 300 && adapterJs->toastCount() == before; ++i) {
                        co_await sleepMs(10);
                    }
                    XX_TEST_EXPECT_EQ(adapterJs->toastCount(), before + 1);
                    XX_TEST_EXPECT_TRUE(
                        adapterJs->lastToast().rfind("net fetch: ok=true status=200", 0) == 0
                    );
                    XX_TEST_EXPECT_TRUE(
                        adapterJs->lastToast().find("hello-from-js-client") != std::string::npos
                    );
                }

                // 通用表: 插件清单 / 自身信息 / 配置 / 广播声明-发布-读-结束
                {
                    const int before = adapterJs->toastCount();
                    mgrJs->dispatchAction("example_js", "js.demo.sdk", "probe", "{}");
                    for (int i = 0; i < 100 && adapterJs->toastCount() == before; ++i) {
                        co_await sleepMs(5);
                    }
                    XX_TEST_EXPECT_EQ(adapterJs->toastCount(), before + 1);
                    const std::string sdkToast = adapterJs->lastToast();
                    XX_TEST_EXPECT_TRUE(sdkToast.rfind("sdk: plugins=", 0) == 0);
                    XX_TEST_EXPECT_TRUE(sdkToast.find(" self=example_js") != std::string::npos);
                    XX_TEST_EXPECT_TRUE(sdkToast.find(" read=true") != std::string::npos);
                    XX_TEST_EXPECT_TRUE(sdkToast.find(" ended=true") != std::string::npos);
                    XX_TEST_EXPECT_TRUE(sdkToast.find(" afterEnd=true") != std::string::npos);
                    // 第一轮: 脚本自己的订阅还没收到过东西
                    XX_TEST_EXPECT_TRUE(sdkToast.find(" echo=0 ends=0") != std::string::npos);
                    // 能力行 (client 侧登记为进程级行): 登记 → 调用 → 注销 一轮跑通
                    XX_TEST_EXPECT_TRUE(
                        sdkToast.find(" cap=true/pong/client") != std::string::npos
                    );
                    // JSON 辅助 (`agentxx.json`): 取顶层字符串字段
                    XX_TEST_EXPECT_TRUE(sdkToast.find(" json=v") != std::string::npos);

                    // 第二轮: 上一轮的 publish / end 已经派发到脚本订阅 (回调在 JS 线程)
                    // - 同一频道多个订阅各收一次; 流结束回调 (onEnd) 也到一次
                    for (int i = 0; i < 100; ++i) {
                        co_await sleepMs(5);
                        if (adapterJs->toastCount() > before) {
                            break;
                        }
                    }
                    mgrJs->dispatchAction("example_js", "js.demo.sdk", "probe", "{}");
                    for (int i = 0; i < 100 && adapterJs->toastCount() == before + 1; ++i) {
                        co_await sleepMs(5);
                    }
                    XX_TEST_EXPECT_EQ(adapterJs->toastCount(), before + 2);
                    const std::string sdkToast2 = adapterJs->lastToast();
                    XX_TEST_EXPECT_TRUE(sdkToast2.find(" echo=1 ends=1") != std::string::npos);
                    XX_TEST_EXPECT_TRUE(
                        sdkToast2.find(" cap=true/pong/client") != std::string::npos
                    );
                }

                // 禁用 → 注册项与缓存一齐摘除; 启用 → start 重跑脚本重新注册
                co_await mgrJs->disable("example_js");
                {
                    auto reg    = mgrJs->uiRegistrySnapshot();
                    bool exists = false;
                    for (const auto& r : reg->toolRenderers) {
                        if (r.toolName == "js_hello") {
                            exists = true;
                        }
                    }
                    XX_TEST_EXPECT_TRUE(!exists);
                }
                co_await mgrJs->enable("example_js");
                bool restored = false;
                for (int i = 0; i < 200 && !restored; ++i) {
                    co_await sleepMs(5);
                    auto reg = mgrJs->uiRegistrySnapshot();
                    for (const auto& r : reg->toolRenderers) {
                        if (r.toolName == "js_hello" && r.displayName == "JS Hello") {
                            restored = true;
                        }
                    }
                }
                XX_TEST_EXPECT_TRUE(restored);
                // 重新注册后的回调照样可用 (新实例 + 新的脚本函数)。
                // - enable 后脚本顶层还在跑: 它每做一次展示项注册都要在主程序 io 线程执行
                //   (ioCallSyncKeep), 这期间 JS 线程正卡在等主程序 (`jsHostCallDepth_ > 0`),
                //   同步桥按设计直接放弃 → 这一次渲染回退默认内容 (真实界面下一帧会再请求)
                // - 失败的渲染会写一份“未命中”缓存, 同 tool_call_id + 同参数不会重试,
                //   所以这里用新的 id 重试到脚本跑完
                std::string againSummary;
                for (int i = 0; i < 60 && againSummary.empty(); ++i) {
                    if (i > 0) {
                        co_await sleepMs(10);
                    }
                    auto againJs = co_await renderToolAsync(
                        mgrJs,
                        "call-js-4-" + std::to_string(i),
                        "js_hello",
                        R"({"name":"Bob"})",
                        "",
                        false,
                        false,
                        80
                    );
                    againSummary = againJs.summary;
                }
                XX_TEST_EXPECT_EQ(againSummary, std::string{"greeting Bob..."});

                // 卸载 example_js → 脚本注册的渲染项随之消失
                XX_TEST_EXPECT_TRUE(co_await mgrJs->unloadAsync("example_js"));
                {
                    auto reg  = mgrJs->uiRegistrySnapshot();
                    bool exists = false;
                    for (const auto& r : reg->toolRenderers) {
                        if (r.toolName == "js_hello") {
                            exists = true;
                        }
                    }
                    XX_TEST_EXPECT_TRUE(!exists);
                }
            }
            // 引擎最后卸载 (example_js 依赖它, 必须先卸)
            XX_TEST_EXPECT_TRUE(co_await mgrJs->unloadAsync("agentxx_javascript_engine"));
        }
        mgrJs.reset();
    }

    // ---- 34b. 形态注册表: 第二个运行器接管预置 `js` ----
    //
    // `js` 是主程序预置的形态位置 (不带实现): 引擎插件以 `overrideBuiltin` 登记同名形态后
    // 才算有运行器; 撤销后回到“位置”本身。动态库形态 (`native`) 不允许插件接管。
    // 未登记的形态按动态库处理 (框架不知道它是怎么编的)。
    {
        auto mgrKind = std::make_shared<agentxx::plugin::ClientPluginManager>(ex);
        auto& kinds  = mgrKind->kindRegistry();

        const auto* jsBuiltin = kinds.find("js");
        XX_TEST_EXPECT_TRUE(jsBuiltin != nullptr);
        XX_TEST_EXPECT_TRUE(jsBuiltin->frameworkOwned());
        const int32_t jsVersion = jsBuiltin->interfaceVersion;
        const int32_t jsMin     = jsBuiltin->minInterfaceVersion;

        // 1. `native` 是动态库形态: 即使写了 overrideBuiltin 也不给接管
        {
            pluginxx::PluginxxKindSpec spec;
            spec.kind           = "native";
            spec.provider       = "alt_runner";
            spec.loadCapability = "plugin.alt_runner.load";
            std::string err;
            XX_TEST_EXPECT_TRUE(!kinds.registerKind(spec, /*overrideBuiltin=*/true, err));
            XX_TEST_EXPECT_TRUE(err.find("native-library") != std::string::npos);
        }

        // 2. 不写 overrideBuiltin 时不能被接管
        {
            pluginxx::PluginxxKindSpec spec;
            spec.kind                = "js";
            spec.provider            = "alt_runner";
            spec.loadCapability      = "plugin.alt_runner.load";
            spec.interfaceVersion    = 99;
            spec.minInterfaceVersion = 99;
            std::string err;
            XX_TEST_EXPECT_TRUE(!kinds.registerKind(spec, /*overrideBuiltin=*/false, err));
            XX_TEST_EXPECT_TRUE(err.find("overrideBuiltin") != std::string::npos);
        }

        // 3. 接管后由该插件提供, 接口代次对沿用自带声明 (不让接管者改)
        {
            pluginxx::PluginxxKindSpec spec;
            spec.kind                = "js";
            spec.provider            = "alt_runner";
            spec.loadCapability      = "plugin.alt_runner.load";
            spec.unloadCapability    = "plugin.alt_runner.unload";
            spec.interfaceVersion    = 99;
            spec.minInterfaceVersion = 99;
            std::string err;
            XX_TEST_EXPECT_TRUE(kinds.registerKind(spec, /*overrideBuiltin=*/true, err));
            const auto* taken = kinds.find("js");
            XX_TEST_EXPECT_TRUE(taken != nullptr);
            if (taken) {
                XX_TEST_EXPECT_EQ(taken->provider, std::string{"alt_runner"});
                XX_TEST_EXPECT_TRUE(taken->builtin);
                XX_TEST_EXPECT_TRUE(!taken->frameworkOwned());
                XX_TEST_EXPECT_EQ(taken->interfaceVersion, jsVersion);
                XX_TEST_EXPECT_EQ(taken->minInterfaceVersion, jsMin);
                XX_TEST_EXPECT_EQ(taken->loadCapability, std::string{"plugin.alt_runner.load"});
            }
            // 别的插件不能再抢已经被接管的形态
            pluginxx::PluginxxKindSpec other;
            other.kind           = "js";
            other.provider       = "other_runner";
            other.loadCapability = "plugin.other_runner.load";
            std::string otherErr;
            XX_TEST_EXPECT_TRUE(!kinds.registerKind(other, /*overrideBuiltin=*/true, otherErr));
            XX_TEST_EXPECT_TRUE(otherErr.find("alt_runner") != std::string::npos);
        }

        // 4. 撤销后回到自带实现
        {
            std::string err;
            XX_TEST_EXPECT_TRUE(kinds.unregisterKind("js", "alt_runner", err));
            const auto* back = kinds.find("js");
            XX_TEST_EXPECT_TRUE(back != nullptr);
            XX_TEST_EXPECT_TRUE(back->frameworkOwned());
        }

        // 5. 未登记的形态: 按动态库处理 (宁可要求够新的接口版本)
        XX_TEST_EXPECT_TRUE(kinds.find("no_such_kind") == nullptr);
        XX_TEST_EXPECT_TRUE(kinds.requiresPluginAbi("no_such_kind"));
        XX_TEST_EXPECT_TRUE(!kinds.requiresPluginAbi("js"));

        mgrKind.reset();
    }

    // ---- 35. 声明式界面: 声明项 + 内容 (生成 / 推送 / 重新生成 / 停用撤销) ----
    //
    // 校验的是主程序这一层: 注册表 (快照) / 生成结果与版本号 / 失败保留上一份 /
    // 生成慢时注册与快照不等待 / 停用后整批撤销。
    {
        auto mgrUi     = std::make_shared<agentxx::plugin::ClientPluginManager>(ex);
        auto adapterUi = std::make_shared<MockPluginUiAdapter>();
        mgrUi->setUiAdapter(adapterUi);
        auto inst = co_await mgrUi->loadNativeAsync(path);
        XX_TEST_EXPECT_TRUE(inst != nullptr);
        if (inst) {
            auto probe = std::make_unique<UiGenProbe>();

            // 声明项: 状态栏上的一个位置 (数据里带 align / order)
            {
                PluginxxUiEntrySpec entry{};
                entry.version     = 1;
                entry.struct_size = sizeof(PluginxxUiEntrySpec);
                const std::string itemId = "sys";
                const std::string type   = "status";
                const std::string data   = R"({"align":1,"order":7})";
                entry.item_id   = PluginxxStringView{itemId.data(), itemId.size()};
                entry.type      = PluginxxStringView{type.data(), type.size()};
                entry.data_json = PluginxxStringView{data.data(), data.size()};
                entry.order     = 7;
                XX_TEST_EXPECT_EQ(
                    mgrUi->registerUiEntry(inst.get(), &entry),
                    PLUGINXX_RESULT_OK
                );
                auto snap = mgrUi->uiContentSnapshot();
                const auto* view = snap ? snap->entryOf("agentxx.client.status", "sys") : nullptr;
                XX_TEST_EXPECT_TRUE(view != nullptr);
                if (view) {
                    XX_TEST_EXPECT_EQ(view->order, 7);
                    XX_TEST_EXPECT_TRUE(view->dataJson.find("\"align\"") != std::string::npos);
                }
                // 未知槽位拒绝
                PluginxxUiEntrySpec bad = entry;
                const std::string   badType = "no_such_slot";
                bad.type = PluginxxStringView{badType.data(), badType.size()};
                XX_TEST_EXPECT_EQ(
                    mgrUi->registerUiEntry(inst.get(), &bad),
                    PLUGINXX_RESULT_ERR_NOT_DECLARED
                );
            }

            // 内容: 同一 id 放在 status 槽位上 (依赖 state 变量)
            {
                PluginxxUiContentSpec spec{};
                spec.version     = 1;
                spec.struct_size = sizeof(PluginxxUiContentSpec);
                const std::string id   = "sys";
                const std::string slot = "status";
                const std::string deps = R"(["agentxx.client.state"])";
                spec.id        = PluginxxStringView{id.data(), id.size()};
                spec.slot      = PluginxxStringView{slot.data(), slot.size()};
                spec.deps_json = PluginxxStringView{deps.data(), deps.size()};
                spec.generate  = &testUiGenerate;
                spec.user_data = probe.get();
                XX_TEST_EXPECT_EQ(
                    mgrUi->registerUiContent(inst.get(), &spec),
                    PLUGINXX_RESULT_OK
                );
                const bool generated = co_await waitUntil([&]() {
                    auto snap = mgrUi->uiContentSnapshot();
                    const auto* c = snap
                                        ? snap->contentOf("agentxx.client.status", "sys")
                                        : nullptr;
                    return c != nullptr && c->hasResult;
                });
                XX_TEST_EXPECT_TRUE(generated);
                auto snap = mgrUi->uiContentSnapshot();
                const auto* c = snap ? snap->contentOf("agentxx.client.status", "sys") : nullptr;
                XX_TEST_EXPECT_TRUE(c != nullptr);
                if (c) {
                    XX_TEST_EXPECT_TRUE(c->json.find("n=1") != std::string::npos);
                    XX_TEST_EXPECT_TRUE(c->rev >= 1);
                    // 声明的变量进了生成上下文 (state 里带 sessionId)
                    XX_TEST_EXPECT_TRUE(c->data.find("sessionId") != std::string::npos);
                }
                const int revAfterFirst = c ? c->rev : 0;

                // 主动推送: 不等生成函数, 版本号 +1 且内容换成推来的那份
                const int32_t pushed = mgrUi->pushUiContent(
                    inst.get(),
                    "sys",
                    R"({"json":{"blocks":[{"kind":"Text","text":"pushed"}]}})"
                );
                XX_TEST_EXPECT_EQ(pushed, PLUGINXX_RESULT_OK);
                {
                    auto snap2 = mgrUi->uiContentSnapshot();
                    const auto* c2 = snap2
                                         ? snap2->contentOf("agentxx.client.status", "sys")
                                         : nullptr;
                    XX_TEST_EXPECT_TRUE(c2 != nullptr);
                    if (c2) {
                        XX_TEST_EXPECT_TRUE(c2->json.find("pushed") != std::string::npos);
                        XX_TEST_EXPECT_EQ(c2->rev, revAfterFirst + 1);
                    }
                }

                // 按当前变量重新生成: 生成函数再跑一次, 内容回到它给的那份
                XX_TEST_EXPECT_EQ(mgrUi->updateUiSlot(inst.get(), "status", "[]"), PLUGINXX_RESULT_OK);
                const bool regenerated = co_await waitUntil([&]() {
                    auto s = mgrUi->uiContentSnapshot();
                    const auto* item = s ? s->contentOf("agentxx.client.status", "sys") : nullptr;
                    return item != nullptr && item->json.find("n=2") != std::string::npos;
                });
                XX_TEST_EXPECT_TRUE(regenerated);
            }

            // 失败: 保留上一份内容, 不把界面清空
            {
                auto badProbe  = std::make_unique<UiGenProbe>();
                badProbe->fail = true;
                PluginxxUiContentSpec spec{};
                spec.version     = 1;
                spec.struct_size = sizeof(PluginxxUiContentSpec);
                const std::string id   = "bad";
                const std::string slot = "status";
                spec.id        = PluginxxStringView{id.data(), id.size()};
                spec.slot      = PluginxxStringView{slot.data(), slot.size()};
                spec.generate  = &testUiGenerate;
                spec.user_data = badProbe.get();
                XX_TEST_EXPECT_EQ(
                    mgrUi->registerUiContent(inst.get(), &spec),
                    PLUGINXX_RESULT_OK
                );
                const bool tried = co_await waitUntil([&]() {
                    return badProbe->calls.load() + 0 >= 0; // 失败不会加 calls, 只看下面的状态
                }, 200);
                (void)tried;
                co_await sleepMs(80);
                auto snap = mgrUi->uiContentSnapshot();
                const auto* c = snap ? snap->contentOf("agentxx.client.status", "bad") : nullptr;
                XX_TEST_EXPECT_TRUE(c != nullptr);
                if (c) {
                    XX_TEST_EXPECT_TRUE(!c->hasResult); // 失败不写入内容
                }
            }

            // 生成慢: 注册 / 快照都不等它 (界面拿上一份或空白), 结果稍后到
            {
                auto slowProbe    = std::make_unique<UiGenProbe>();
                slowProbe->delayMs = 200;
                PluginxxUiContentSpec spec{};
                spec.version     = 1;
                spec.struct_size = sizeof(PluginxxUiContentSpec);
                const std::string id   = "slow";
                const std::string slot = "status";
                spec.id        = PluginxxStringView{id.data(), id.size()};
                spec.slot      = PluginxxStringView{slot.data(), slot.size()};
                spec.generate  = &testUiGenerate;
                spec.user_data = slowProbe.get();
                const auto begin = std::chrono::steady_clock::now();
                XX_TEST_EXPECT_EQ(
                    mgrUi->registerUiContent(inst.get(), &spec),
                    PLUGINXX_RESULT_OK
                );
                const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - begin
                ).count();
                XX_TEST_EXPECT_TRUE(elapsedMs < 100); // 没等那 200 ms
                {
                    auto snap = mgrUi->uiContentSnapshot();
                    const auto* c = snap ? snap->contentOf("agentxx.client.status", "slow") : nullptr;
                    XX_TEST_EXPECT_TRUE(c != nullptr);
                    if (c) {
                        XX_TEST_EXPECT_TRUE(!c->hasResult);
                    }
                }
                const bool arrived = co_await waitUntil([&]() {
                    auto s = mgrUi->uiContentSnapshot();
                    const auto* item = s ? s->contentOf("agentxx.client.status", "slow") : nullptr;
                    return item != nullptr && item->hasResult;
                }, 3000);
                XX_TEST_EXPECT_TRUE(arrived);
            }

            // 停用: 声明项 / 内容 / 自建槽位一起撤销
            {
                PluginxxUiSlotSpec slotSpec{};
                slotSpec.version     = 1;
                slotSpec.struct_size = sizeof(PluginxxUiSlotSpec);
                const std::string slotId = "plugin.example_plugin.ui.card";
                const std::string kind   = "insert";
                slotSpec.id   = PluginxxStringView{slotId.data(), slotId.size()};
                slotSpec.kind = PluginxxStringView{kind.data(), kind.size()};
                XX_TEST_EXPECT_EQ(
                    mgrUi->defineUiSlot(inst.get(), &slotSpec),
                    PLUGINXX_RESULT_OK
                );
                {
                    auto snap = mgrUi->uiContentSnapshot();
                    bool found = false;
                    if (snap) {
                        for (const auto& s : snap->slots) {
                            if (s.id == slotId) {
                                found = true;
                            }
                        }
                    }
                    XX_TEST_EXPECT_TRUE(found);
                }
                // 别人的命名空间被拒
                const std::string otherId = "plugin.other.ui.card";
                PluginxxUiSlotSpec other = slotSpec;
                other.id = PluginxxStringView{otherId.data(), otherId.size()};
                XX_TEST_EXPECT_EQ(
                    mgrUi->defineUiSlot(inst.get(), &other),
                    PLUGINXX_RESULT_ERR_PERMISSION
                );

                XX_TEST_EXPECT_TRUE(co_await mgrUi->disable("example_plugin"));
                auto snap = mgrUi->uiContentSnapshot();
                XX_TEST_EXPECT_TRUE(snap != nullptr);
                if (snap) {
                    bool anyContent = false;
                    for (const auto& c : snap->contents) {
                        if (c.plugin == "example_plugin") {
                            anyContent = true;
                        }
                    }
                    bool anyEntry = false;
                    for (const auto& e : snap->entries) {
                        if (e.plugin == "example_plugin") {
                            anyEntry = true;
                        }
                    }
                    bool anySlot = false;
                    for (const auto& s : snap->slots) {
                        if (s.plugin == "example_plugin") {
                            anySlot = true;
                        }
                    }
                    XX_TEST_EXPECT_TRUE(!anyContent);
                    XX_TEST_EXPECT_TRUE(!anyEntry);
                    XX_TEST_EXPECT_TRUE(!anySlot);
                }
                XX_TEST_EXPECT_TRUE(co_await mgrUi->unloadAsync("example_plugin", std::chrono::seconds{5}));
            }
        }
        mgrUi.reset();
    }

    // ---- 36. 工具行内容: 注册一份内容 = 该工具名的行渲染 (按调用各生成一份) ----
    {
        // 前面的用例已经把 example_plugin 卸载了, 这里重新加载一份来做注册
        auto toolInst = co_await mgr->loadNativeAsync(path);
        XX_TEST_EXPECT_TRUE(toolInst != nullptr);
        if (toolInst) {
            const std::string id      = "demo_tool_row";
            const std::string slot    = "tool";
            const std::string data    = R"({"tool":"demo_tool","displayName":"Demo"})";
            const std::string deps    = R"(["agentxx.client.tool.args","agentxx.client.tool.part"])";

            PluginxxUiEntrySpec entry{};
            entry.version     = 1;
            entry.struct_size = sizeof(PluginxxUiEntrySpec);
            entry.item_id     = PluginxxStringView{id.data(), id.size()};
            entry.type        = PluginxxStringView{slot.data(), slot.size()}; // 短名 = agentxx.client.tool
            entry.data_json   = PluginxxStringView{data.data(), data.size()};
            XX_TEST_EXPECT_EQ(mgr->registerUiEntry(toolInst.get(), &entry), PLUGINXX_RESULT_OK);

            PluginxxUiContentSpec spec{};
            spec.version     = 1;
            spec.struct_size = sizeof(PluginxxUiContentSpec);
            spec.id          = PluginxxStringView{id.data(), id.size()};
            spec.slot        = PluginxxStringView{slot.data(), slot.size()};
            spec.deps_json   = PluginxxStringView{deps.data(), deps.size()};
            spec.generate    = &testToolRowGenerate;
            XX_TEST_EXPECT_EQ(mgr->registerUiContent(toolInst.get(), &spec), PLUGINXX_RESULT_OK);

            // 显示名来自声明项 data (UI 线程同步可读)
            {
                auto regName = mgr->uiRegistrySnapshot();
                XX_TEST_EXPECT_EQ(
                    std::string{agentxx::plugin::toolDisplayNameFor(regName.get(), {}, "demo_tool")},
                    std::string{"Demo"}
                );
            }

            auto cache = mgr->toolRenderCache();
            const auto renderOnce
                = [&](agentxx::plugin::ClientToolRenderPart part,
                      const std::string&                     callId) -> asio::awaitable<std::shared_ptr<const agentxx::plugin::ClientToolRenderEntry>> {
                agentxx::plugin::ClientToolRenderRequest req;
                req.toolCallId = callId;
                req.toolName   = "demo_tool";
                req.argsJson   = R"({"q":"hello"})";
                req.resultText = "done";
                req.part       = part;
                req.isFinished = true;
                req.maxWidth   = 60;
                const std::string key = agentxx::plugin::ClientToolRenderRequest::keyFor(
                    callId,
                    "demo_tool",
                    part
                );
                const uint64_t hash = req.inputHash();
                mgr->requestToolRender(req);
                for (int i = 0; i < 500 && !cache->lookup(key, hash); ++i) {
                    co_await sleepMs(1);
                }
                co_return cache->lookup(key, hash);
            };

            // 折叠头: 内容是一块 Text, 主程序取纯文本作为那一行
            {
                g_toolRowContexts.clear();
                const auto  hit = co_await renderOnce(
                    agentxx::plugin::ClientToolRenderPart::Collapsed,
                    "tool_row_call_1"
                );
                XX_TEST_EXPECT_TRUE(hit != nullptr);
                if (hit != nullptr) {
                    XX_TEST_EXPECT_TRUE(hit->matched);
                    XX_TEST_EXPECT_EQ(hit->text, std::string{"summary line"});
                    XX_TEST_EXPECT_EQ(hit->plugin, std::string{"example_plugin"});
                }
                // 上下文只给声明过的变量 (工具名 / 结果 / 宽度都没声明)
                XX_TEST_EXPECT_EQ(g_toolRowContexts.size(), size_t{1});
                if (!g_toolRowContexts.empty()) {
                    const auto ctx = utilxx_base::Json::parse(g_toolRowContexts.front());
                    XX_TEST_EXPECT_EQ(
                        ctx.value("agentxx.client.tool.part", std::string{}),
                        std::string{"summary"}
                    );
                    XX_TEST_EXPECT_TRUE(ctx.contains("agentxx.client.tool.args"));
                    XX_TEST_EXPECT_TRUE(!ctx.contains("agentxx.client.tool.name"));
                }
            }

            // 展开体: 内容里的 blocks 作为展开块
            {
                const auto  hit = co_await renderOnce(
                    agentxx::plugin::ClientToolRenderPart::Expanded,
                    "tool_row_call_1"
                );
                XX_TEST_EXPECT_TRUE(hit != nullptr);
                if (hit != nullptr) {
                    XX_TEST_EXPECT_TRUE(hit->matched);
                    XX_TEST_EXPECT_TRUE(hit->items.is_array());
                    XX_TEST_EXPECT_EQ(hit->items.size(), size_t{1});
                    XX_TEST_EXPECT_TRUE(hit->items.dump().find("body") != std::string::npos);
                }
            }

            // 另一次调用各算一份 (同一内容, 不同调用不互相覆盖)
            {
                const auto  first = co_await renderOnce(
                    agentxx::plugin::ClientToolRenderPart::Collapsed,
                    "tool_row_call_2"
                );
                XX_TEST_EXPECT_TRUE(first != nullptr);
                const auto  firstCall = cache->lookup(
                    agentxx::plugin::ClientToolRenderRequest::keyFor(
                        "tool_row_call_1",
                        "demo_tool",
                        agentxx::plugin::ClientToolRenderPart::Collapsed
                    ),
                    agentxx::plugin::ClientToolRenderRequest::hashInputs(
                        "demo_tool",
                        "{\"q\":\"hello\"}",
                        "done",
                        true,
                        false,
                        60
                    )
                );
                XX_TEST_EXPECT_TRUE(firstCall != nullptr);
            }

            // 撤销内容 → 渲染器跟着摘 (显示名回退空串, 请求不再命中)
            XX_TEST_EXPECT_EQ(
                mgr->unregisterUiContent(toolInst.get(), id),
                PLUGINXX_RESULT_OK
            );
            {
                auto regGone = mgr->uiRegistrySnapshot();
                XX_TEST_EXPECT_TRUE(
                    agentxx::plugin::toolDisplayNameFor(regGone.get(), {}, "demo_tool").empty()
                );
            }
            XX_TEST_EXPECT_EQ(
                mgr->unregisterUiEntry(toolInst.get(), id),
                PLUGINXX_RESULT_OK
            );
        }
        XX_TEST_EXPECT_TRUE(
            co_await mgr->unloadAsync("example_plugin", std::chrono::seconds{5})
        );
    }

    co_return TestResult{g_client_plugin_passed, g_client_plugin_failed};

}

} // namespace test
} // namespace agentxx
