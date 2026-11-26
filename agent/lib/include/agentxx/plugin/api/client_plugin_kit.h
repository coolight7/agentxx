/// 插件开发 SDK (C++ header-only) —— client 侧业务部分
///
/// 包含本头即可写 client 侧插件: `ClientPluginBase` (实例上下文基类, 接口表聚合为
/// `ClientIfaces`)、client 六张表的调用形态、界面组件构建、工具栏装饰 / 面板 / Info
/// 段落 / overlay / 定时器 / 快捷键便捷方法, 以及 `AGENTXX_CLIENT_PLUGIN_*_EXPORT` 导出宏。
///
/// 通用设施 (`pluginxx::kit` 名字、广播 / 网络便捷入口) 在 `plugin_kit.h`, 本头已包含它。
#pragma once
#include "agentxx/plugin/api/plugin_kit.h"
#include "agentxx/plugin/api/client_plugin_api.h"
#include "agentxx/plugin/api/agentxx_ui_kit.g.h"

namespace agentxx {
namespace plugin {


/// client 侧接口表聚合 (一次查询; 成员为 NULL 表示主程序未实现该接口)
///
/// 与 agent 侧同一套通用表 (log/json/config/plugins/broadcasts/scheduler/
/// coroutine/tasks/cancel/capabilities), 另加 client 自有表
/// (`agentxx.client.*`: ui/session/wire/self/timer/keybind) 与网络表 (`agentxx.net`)。
struct ClientIfaces {
    const AgentxxClientUiIface*      ui      = nullptr; ///< "agentxx.client.ui"
    const AgentxxClientSessionIface* session = nullptr; ///< "agentxx.client.session"
    const AgentxxClientWireIface*    wire    = nullptr; ///< "agentxx.client.wire"
    const AgentxxClientSelfIface*    self    = nullptr; ///< "agentxx.client.self"
    const PluginxxJsonIface*         json    = nullptr; ///< "pluginxx.json"
    const PluginxxLogIface*          log     = nullptr; ///< "pluginxx.log"
    const AgentxxClientTimerIface*   timer   = nullptr; ///< "agentxx.client.timer"
    const AgentxxClientKeybindIface* keybind = nullptr; ///< "agentxx.client.keybind"
    /// 广播 (声明 / 发布 / 订阅 / 读当前值 / 结束): client 侧事件的唯一通道
    /// - 客户端频道 (`agentxx.client.ready` / `connection_state` / `user_input` /
    ///   `delta` / `turn_end` / `session_switch` / `ui_layout`) 由主程序启动时声明,
    ///   插件订阅即可, 不需要先声明; 跨端数据走
    ///   `agentxx.client.plugin.<插件名>.<事件>`
    const PluginxxBroadcastsIface* broadcasts = nullptr;
    /// 网络 (`<应用名>.net`): HTTP / TCP / WS / UDP; 与 agent 侧同一个网络服务
    /// - 网络服务自有线程, 同步 `call` 不投递到 client io 线程 (不阻塞界面)
    /// - 资源按插件实例记账: 禁用/卸载时整批收掉 (服务、连接、未完成请求)
    const PluginxxNetIface* net = nullptr;
    /// 配置查询 (本插件条目参数 / 配置文件路径 / 语言)
    const PluginxxConfigIface* config = nullptr; ///< "pluginxx.config"
    /// 本主程序已加载的插件清单 (自描述查询)
    const PluginxxPluginsIface* plugins = nullptr; ///< "pluginxx.plugins"
    /// 取消 (通用表): 每个异步操作句柄都能在这里取消
    const PluginxxCancelIface* cancel = nullptr; ///< "pluginxx.cancel"
    /// 调度 (通用表): 睡眠 / 卸载 / io 线程判定
    const PluginxxSchedulerIface* scheduler = nullptr; ///< "pluginxx.scheduler"
    /// 后台任务 (通用表): kit 的协程 `spawn` 用它登记取消
    const PluginxxTasksIface* tasks = nullptr; ///< "pluginxx.tasks"

    /// 从主程序查询全部已知 client 侧接口表 (host 为空时返回全 NULL 聚合)
    static ClientIfaces query(const PluginxxHost* host) {
        ClientIfaces f;
        if (!host || !host->vtable || !host->vtable->query_interface) {
            return f;
        }
        f.ui      = queryInterface<AgentxxClientUiIface>(host, AGENTXX_IFACE_CLIENT_UI);
        f.session = queryInterface<AgentxxClientSessionIface>(host, AGENTXX_IFACE_CLIENT_SESSION);
        f.wire    = queryInterface<AgentxxClientWireIface>(host, AGENTXX_IFACE_CLIENT_WIRE);
        f.self    = queryInterface<AgentxxClientSelfIface>(host, AGENTXX_IFACE_CLIENT_SELF);
        f.json    = queryInterface<PluginxxJsonIface>(host, PLUGINXX_IFACE_JSON);
        f.log     = queryInterface<PluginxxLogIface>(host, PLUGINXX_IFACE_LOG);
        f.timer   = queryInterface<AgentxxClientTimerIface>(host, AGENTXX_IFACE_CLIENT_TIMER);
        f.keybind = queryInterface<AgentxxClientKeybindIface>(host, AGENTXX_IFACE_CLIENT_KEYBIND);
        f.broadcasts
            = queryInterface<PluginxxBroadcastsIface>(host, PLUGINXX_IFACE_BROADCASTS);
        f.net      = queryInterface<PluginxxNetIface>(host, PLUGINXX_IFACE_NET);
        f.config   = queryInterface<PluginxxConfigIface>(host, PLUGINXX_IFACE_CONFIG);
        f.plugins  = queryInterface<PluginxxPluginsIface>(host, PLUGINXX_IFACE_PLUGINS);
        f.cancel    = queryInterface<PluginxxCancelIface>(host, PLUGINXX_IFACE_CANCEL);
        f.scheduler = queryInterface<PluginxxSchedulerIface>(host, PLUGINXX_IFACE_SCHEDULER);
        f.tasks     = queryInterface<PluginxxTasksIface>(host, PLUGINXX_IFACE_TASKS);
        return f;
    }
};

/* ==================== client 表便捷调用 (插件侧) ====================
 *
 * client 侧六张表也是异步契约。插件侧按“要不要结果”选两种写法:
 * - **不要结果** (toast / 更新面板 / 打开 overlay …): 传 `cb = nullptr` 提交,
 *   调用即返回, 任何线程都不等 (`*NoWait` 系列与 [clientTableCall]);
 * - **要结果**: `co_await clientTableOpAsync(ctx, ...)` 或下面的 `*Async` 便捷入口;
 *   没有同步形态 —— 等结果必须先写成协程 (插件执行线程与 client io 线程是两条线程,
 *   在那里同步等会把它卡住)。
 */

/// client 表调用 (**提交即返回**): 只要发起, 不要结果, 任何线程都不等
///
/// 失败原因由主程序自己记日志 (插件拿不到, 也不需要)。
template<typename StartFn>
inline bool clientTableCall(const PluginxxHost* host, const ClientIfaces&, StartFn&& start) {
    PluginxxString err{nullptr, 0};
    auto*          handle = start(nullptr, nullptr, &err);
    if (err.data != nullptr && host != nullptr) {
        PluginString::free(host, &err);
    }
    return handle != nullptr;
}

namespace detail {

/// 取当前协程执行器后提交一次 client 表调用 (asio 形态的公共部分)
///
/// client 表全部是异步操作契约: 提交即开始, 结果经 `asio::post` 回到本协程的执行器。
template<typename Ctx, typename StartFn>
inline asio::awaitable<pluginxx::TableOpResult>
    clientTableAwaitAsync(const Ctx& ctx, StartFn start) {
    co_return co_await pluginxx::detail::tableOpAwaitAsyncSlot(ctx, std::move(start));
}

} // namespace detail

/// client 表调用 (**asio 协程形态**): `co_await` 拿结果
///
/// 插件把注册事务写成 `asio::awaitable` 协程时用它 (`start` 入口先 `finishLater(notify)`
/// 再 `ctx.spawn(...)`):
///
/// ```cpp
/// auto r = co_await clientTableOpAsync(ctx, [&](auto cb, auto ud, auto err) {
///     return ui->register_status_item(host, &idSv, &jsonSv, 0, 10, cb, ud, err);
/// });
/// auto* handle = clientHandleOf<AgentxxStatusItem>(r);
/// ```
///
/// - 结果是 `pluginxx::TableOpResult`: 业务失败不抛异常, 看 `ok()` / `text()`;
/// - 提交发生在开始 `co_await` 时; 想并行提交用 `asio::co_spawn` 显式开协程。
template<typename Ctx, typename StartFn>
inline asio::awaitable<pluginxx::TableOpResult> clientTableOpAsync(const Ctx& ctx, StartFn&& start) {
    co_return co_await detail::clientTableAwaitAsync(ctx, std::forward<StartFn>(start));
}

/// 从句柄类表结果里取句柄 (失败 / 未受理 → nullptr)
///
/// client 表把句柄以十进制文本放在 `payload` 里 (见 `clientHandleFromText`)。
template<typename Handle>
inline Handle* clientHandleOf(const pluginxx::detail::TableOpResult& result) {
    return result.ok() ? agentxx::plugin::clientHandleFromText<Handle>(result.text()) : nullptr;
}


/// 表调用结果 → 旧状态码 (只区分“成功”与“没成功”)
inline int32_t tableStatus(const pluginxx::detail::TableOpResult& res) {
    return res.ok() ? AGENTXX_RESULT_OK : AGENTXX_RESULT_ERR_FAILED;
}

/* ==================== client 展示面便捷入口 (插件侧) ====================
 *
 * 这些函数给 client 侧的 start/mount/回调里的**同步写法**用: 调用方本来就在
 * client IO 线程时主程序内联完成 (注册完立刻可用, 不产生等待); 在插件自己的线程上会
 * 等到完成。事件回调这类不能写成协程的地方都能直接调。
 */

/// 注册状态栏项 (**协程形态**; 空 id / 主程序不支持时返回 nullptr)
template<typename Ctx>
inline asio::awaitable<AgentxxStatusItem*> clientRegisterStatusItemAsync(
    Ctx&         ctx,
    std::string  id,
    std::string  initialJson,
    int32_t      align = 0,
    int32_t      order = 0
) {
    const auto* ui = ctx.iface.ui;
    if (!ctx.host || !ui || !ui->register_status_item || id.empty()) {
        co_return nullptr;
    }
    auto idSv   = PluginStringView::from(id);
    auto jsonSv = PluginStringView::from(initialJson);
    auto r      = co_await detail::clientTableAwaitAsync(
        ctx,
        [&ctx, ui, &idSv, &jsonSv, align, order](
                                                 PluginxxOperatorCallback cb,
                                                 void*                    ud,
                                                 PluginxxString*          err
                                             ) {
            return ui->register_status_item(ctx.host, &idSv, &jsonSv, align, order, cb, ud, err);
        }
    );
    co_return clientHandleOf<AgentxxStatusItem>(r);
}

/// 更新状态栏项文本; 返回 0 成功
inline int32_t clientUpdateStatusItem(
    const PluginxxHost*        host,
    const AgentxxClientUiIface* ui,
    AgentxxStatusItem*         item,
    std::string_view           json
) {
    if (!host || !ui || !ui->update_status_item || !item) {
        return AGENTXX_RESULT_ERR_UNSUPPORTED;
    }
    const auto iface = ClientIfaces::query(host);
    auto       sv    = PluginStringView::from(json.data(), json.size());
    clientTableCall(host, iface, [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
        return ui->update_status_item(host, item, &sv, cb, ud, err);
    });
    return AGENTXX_RESULT_OK;
}

/// 注销状态栏项 (只提交, 不等结果)
inline void clientUnregisterStatusItem(
    const PluginxxHost*        host,
    const AgentxxClientUiIface* ui,
    AgentxxStatusItem*         item
) {
    if (!host || !ui || !ui->unregister_status_item || !item) {
        return;
    }
    ui->unregister_status_item(host, item, nullptr, nullptr, nullptr);
}

/// 注册侧边栏面板 (**协程形态**)
template<typename Ctx>
inline asio::awaitable<AgentxxPanel*> clientRegisterPanelAsync(
    Ctx&        ctx,
    std::string id,
    std::string propsJson
) {
    const auto* ui = ctx.iface.ui;
    if (!ctx.host || !ui || !ui->register_panel || id.empty()) {
        co_return nullptr;
    }
    auto idSv  = PluginStringView::from(id);
    auto props = PluginStringView::from(propsJson);
    auto r     = co_await detail::clientTableAwaitAsync(
        ctx,
        [&ctx, ui, &idSv, &props](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
            return ui->register_panel(ctx.host, &idSv, &props, cb, ud, err);
        }
    );
    co_return clientHandleOf<AgentxxPanel>(r);
}

/// 更新面板内容; 返回 0 成功
inline int32_t clientUpdatePanel(
    const PluginxxHost*        host,
    const AgentxxClientUiIface* ui,
    AgentxxPanel*              panel,
    std::string_view           itemsJson
) {
    if (!host || !ui || !ui->update_panel || !panel) {
        return AGENTXX_RESULT_ERR_UNSUPPORTED;
    }
    const auto iface = ClientIfaces::query(host);
    auto       sv    = PluginStringView::from(itemsJson.data(), itemsJson.size());
    clientTableCall(host, iface, [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
        return ui->update_panel(host, panel, &sv, cb, ud, err);
    });
    return AGENTXX_RESULT_OK;
}

/// 注销侧边栏面板 (只提交, 不等结果)
inline void clientUnregisterPanel(
    const PluginxxHost*        host,
    const AgentxxClientUiIface* ui,
    AgentxxPanel*              panel
) {
    if (!host || !ui || !ui->unregister_panel || !panel) {
        return;
    }
    ui->unregister_panel(host, panel, nullptr, nullptr, nullptr);
}

/// 注册 Info 栏段落 (**协程形态**)
template<typename Ctx>
inline asio::awaitable<AgentxxInfoSection*> clientRegisterInfoSectionAsync(
    Ctx&        ctx,
    std::string id,
    std::string propsJson
) {
    const auto* ui = ctx.iface.ui;
    if (!ctx.host || !ui || !ui->register_info_section || id.empty()) {
        co_return nullptr;
    }
    auto idSv  = PluginStringView::from(id);
    auto props = PluginStringView::from(propsJson);
    auto r     = co_await detail::clientTableAwaitAsync(
        ctx,
        [&ctx, ui, &idSv, &props](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
            return ui->register_info_section(ctx.host, &idSv, &props, cb, ud, err);
        }
    );
    co_return clientHandleOf<AgentxxInfoSection>(r);
}

/// 更新 Info 栏段落内容; 返回 0 成功
inline int32_t clientUpdateInfoSection(
    const PluginxxHost*        host,
    const AgentxxClientUiIface* ui,
    AgentxxInfoSection*        section,
    std::string_view           itemsJson
) {
    if (!host || !ui || !ui->update_info_section || !section) {
        return AGENTXX_RESULT_ERR_UNSUPPORTED;
    }
    const auto iface = ClientIfaces::query(host);
    auto       sv    = PluginStringView::from(itemsJson.data(), itemsJson.size());
    clientTableCall(host, iface, [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
        return ui->update_info_section(host, section, &sv, cb, ud, err);
    });
    return AGENTXX_RESULT_OK;
}

/// 注销 Info 栏段落 (只提交, 不等结果)
inline void clientUnregisterInfoSection(
    const PluginxxHost*        host,
    const AgentxxClientUiIface* ui,
    AgentxxInfoSection*        section
) {
    if (!host || !ui || !ui->unregister_info_section || !section) {
        return;
    }
    ui->unregister_info_section(host, section, nullptr, nullptr, nullptr);
}

/// 更新/删除工具消息装饰 (空 decorJson = 删除); 返回 0 成功
inline int32_t clientUpdateToolDecor(
    const PluginxxHost*        host,
    const AgentxxClientUiIface* ui,
    std::string_view           toolCallId,
    std::string_view           decorJson
) {
    if (!host || !ui || !ui->update_tool_decor) {
        return AGENTXX_RESULT_ERR_UNSUPPORTED;
    }
    const auto iface = ClientIfaces::query(host);
    auto       tcid  = PluginStringView::from(toolCallId.data(), toolCallId.size());
    auto       decor = PluginStringView::from(decorJson.data(), decorJson.size());
    clientTableCall(host, iface, [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
        return ui->update_tool_decor(host, &tcid, &decor, cb, ud, err);
    });
    return AGENTXX_RESULT_OK;
}

/// 打开通用 overlay (**提交即返回**); 返回 `AGENTXX_RESULT_OK` 表示已提交
inline int32_t clientOpenOverlay(
    const PluginxxHost*        host,
    const AgentxxClientUiIface* ui,
    const AgentxxOverlaySpec*  spec
) {
    if (!host || !ui || !ui->open_overlay || !spec) {
        return AGENTXX_RESULT_ERR_UNSUPPORTED;
    }
    const auto iface = ClientIfaces::query(host);
    clientTableCall(host, iface, [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
        return ui->open_overlay(host, spec, cb, ud, err);
    });
    return AGENTXX_RESULT_OK;
}

/// 绑定动作处理器 (**提交即返回**); 返回是否已提交
inline bool clientBindAction(
    const PluginxxHost*        host,
    const AgentxxClientUiIface* ui,
    std::string_view           targetId,
    AgentxxUiActionFn          onAction,
    void*                      userData
) {
    if (!host || !ui || !ui->bind_action_handler || !onAction) {
        return false;
    }
    const auto iface  = ClientIfaces::query(host);
    auto       target = PluginStringView::from(targetId.data(), targetId.size());
    return clientTableCall(host, iface, [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
        return ui->bind_action_handler(host, &target, onAction, userData, cb, ud, err);
    });
}

/// 发一条跨端事件到 agent 侧 (**提交即返回**); 返回是否已提交
inline bool clientSendPluginData(
    const PluginxxHost*          host,
    const AgentxxClientWireIface* wire,
    std::string_view             topic,
    std::string_view             json
) {
    if (!host || !wire || !wire->send_plugin_data || topic.empty()) {
        return false;
    }
    const auto iface   = ClientIfaces::query(host);
    auto       t       = PluginStringView::from(topic.data(), topic.size());
    auto       payload = PluginStringView::from(json.data(), json.size());
    return clientTableCall(host, iface, [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
        return wire->send_plugin_data(host, &t, &payload, cb, ud, err);
    });
}

/// 注册斜杠命令 (**协程形态**); 返回是否已受理
/// - `execute` 是异步操作契约: 动作 JSON 经它自己的 `notify` 交回 (见
///   [AgentxxClientCommandExecuteFn]), 回调内部立刻 `notifyText` 就是同步完成;
/// - `commandData` 只给 `execute` 用 (与本次调用的完成回调无关)
template<typename Ctx>
inline asio::awaitable<bool> clientRegisterCommandAsync(
    Ctx&                          ctx,
    std::string                   name,
    std::string                   description,
    AgentxxClientCommandExecuteFn execute,
    void*                         commandData
) {
    const auto* ui = ctx.iface.ui;
    if (!ctx.host || !ui || !ui->register_command || name.empty() || !execute) {
        co_return false;
    }
    auto nameSv = PluginStringView::from(name);
    auto descSv = PluginStringView::from(description);
    auto r      = co_await detail::clientTableAwaitAsync(
        ctx,
        [&ctx, ui, &nameSv, &descSv, execute, commandData](
                                                    PluginxxOperatorCallback cb,
                                                    void*                    ud,
                                                    PluginxxString*          err
                                                ) {
            return ui->register_command(
                ctx.host,
                &nameSv,
                &descSv,
                execute,
                commandData,
                cb,
                ud,
                err
            );
        }
    );
    co_return r.ok();
}

/// 注销斜杠命令 (只提交, 不等结果)
inline void clientUnregisterCommand(
    const PluginxxHost*         host,
    const AgentxxClientUiIface* ui,
    std::string_view            name
) {
    if (!host || !ui || !ui->unregister_command || name.empty()) {
        return;
    }
    auto nameSv = PluginStringView::from(name.data(), name.size());
    ui->unregister_command(host, &nameSv, nullptr, nullptr, nullptr);
}

/// 以成功完成一次操作回调 (正文原样交给调用方; 空正文 = 没有内容)
///
/// 命令回调、渲染回调这类“回答型”回调都用它收尾; 每个 `notify` 只回调一次。
inline void notifyText(const PluginxxOperatorNotify* notify, std::string_view text) {
    if (!notify || !notify->done) {
        return;
    }
    auto sv = PluginStringView::from(text.data(), text.size());
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, &sv);
}

/// 以失败完成一次操作回调 (原因文本交给调用方)
inline void notifyError(const PluginxxOperatorNotify* notify, std::string_view reason) {
    if (!notify || !notify->done) {
        return;
    }
    auto sv = PluginStringView::from(reason.data(), reason.size());
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_FAILED, &sv);
}



namespace detail {

/// 手写 get_info 入口的公共实现 (client 侧)
inline int32_t fillClientPluginInfo(
    PluginxxInfo* out,
    const char*   name,
    const char*   version,
    const char*   description
) noexcept {
    return pluginxx::detail::fillPluginInfo(
        out,
        name,
        version,
        description,
        AGENTXX_CLIENT_PLUGIN_MIN_API_VERSION
    );
}

} // namespace detail

/* ==================== 工具行: 声明项 + 内容 ==================== */

/// 一次工具行的输入 (折叠头与展开体各调用一次)
///
/// - 参数可能还没输出完 (运行中), 按"缺值就给默认展示"处理
/// - `summary` 为真时只要一行文字, 为假时给整块内容
struct ToolRowInput {
    std::string toolName;   ///< 工具名 (如 `agentxx_filesystem_read`)
    std::string argsJson;   ///< 这次调用的参数原文
    std::string resultText; ///< 结果文本 (运行中为空)
    int         width    = 0;     ///< 可用的显示宽度 (列; 0 = 未知)
    bool        summary  = true;  ///< true = 折叠头, false = 展开体
    bool        finished = false; ///< 这次调用已结束
    bool        error    = false; ///< 这次调用以失败结束
};

/// 工具行内容: 两个可选部分, 都不给内容时界面走默认渲染
struct ToolRowSpec {
    /// 折叠头一行 (空串 = 本次没有内容)
    std::function<std::string(const ToolRowInput&)> summary;
    /// 展开体 (界面描述层的块数组; 一项也不加 = 默认展开体)
    std::function<void(const ToolRowInput&, utilxx_base::Json&)> body;
};

/// 已注册的工具行 (生成函数的状态; 插件实例持它到卸载)
///
/// 注册走 `agentxx.client.tool` 上的**声明项 + 内容**, 不再是单独的回调注册:
/// 主程序按 (调用, 折叠 / 展开) 各请一份并缓存, 插件只管把这次调用变成内容。
class ToolRow {
public:

    /// `agentxx.client.tool` 槽位的短名
    static constexpr std::string_view kSlot = "tool";

    ToolRow(std::string toolName, std::string displayName, ToolRowSpec spec) :
        toolName_(std::move(toolName)),
        displayName_(displayName.empty() ? toolName_ : std::move(displayName)),
        spec_(std::move(spec)) {}

    const std::string& toolName() const noexcept {
        return toolName_;
    }

    const std::string& displayName() const noexcept {
        return displayName_;
    }

    /// 声明项的 `data` (`{"tool":…,"displayName":…}`: 主程序据此知道这份内容对应哪个工具)
    std::string declarationDataJson() const {
        utilxx_base::Json data = utilxx_base::Json::object();
        data["tool"]           = toolName_;
        data["displayName"]    = displayName_;
        return data.dump();
    }

    /// 内容生成函数 (主程序调; 内容契约 `{"json": <界面文档>}`)
    ///
    /// - 拿不到内容时交空文档: 界面走自己的默认渲染, 不算失败
    /// - 在插件执行线程上跑 (与工具 execute 同一个线程)
    static void PLUGINXX_CALL generate(
        const PluginxxHost*           host,
        const PluginxxStringView*     ctx_json,
        void*                         user_data,
        const PluginxxOperatorNotify* notify,
        PluginxxString*               error_out
    ) {
        if (notify == nullptr || notify->done == nullptr) {
            PluginString::set(host, error_out, "tool row needs a completion callback");
            return;
        }
        auto* self = static_cast<ToolRow*>(user_data);
        if (self == nullptr) {
            PluginString::set(host, error_out, "tool row is gone");
            return;
        }
        std::string payload = "{\"json\":{}}";
        try {
            utilxx_base::Json ctx = utilxx_base::Json::object();
            if (ctx_json != nullptr && ctx_json->data != nullptr && ctx_json->size > 0) {
                auto parsed = utilxx_base::Json::parse(
                    std::string_view{ctx_json->data, static_cast<size_t>(ctx_json->size)}
                );
                if (parsed.is_object()) {
                    ctx = std::move(parsed);
                }
            }
            auto stringOf = [&ctx](const char* key) -> std::string {
                const auto it = ctx.find(key);
                return (it != ctx.end() && it->is_string()) ? it->get<std::string>()
                                                            : std::string{};
            };
            ToolRowInput in;
            in.toolName   = self->toolName_;
            in.argsJson   = stringOf("agentxx.client.tool.args");
            in.resultText = stringOf("agentxx.client.tool.result");
            if (const auto w = ctx.find("agentxx.client.tool.width");
                w != ctx.end() && w->is_number()) {
                in.width = w->get<int>();
            }
            in.summary        = stringOf("agentxx.client.tool.part") != "body";
            const std::string state = stringOf("agentxx.client.tool.state");
            in.finished       = state != "running";
            in.error          = state == "error";
            if (in.summary) {
                const std::string line = self->spec_.summary ? self->spec_.summary(in)
                                                             : std::string{};
                if (!line.empty()) {
                    utilxx_base::Json block = utilxx_base::Json::object();
                    block["kind"]           = "Text";
                    block["text"]           = line;
                    utilxx_base::Json doc   = utilxx_base::Json::object();
                    doc["blocks"]           = utilxx_base::Json::array({block});
                    payload = std::string{"{\"json\":"} + doc.dump() + "}";
                }
            } else if (self->spec_.body) {
                utilxx_base::Json blocks = utilxx_base::Json::array();
                self->spec_.body(in, blocks);
                if (blocks.is_array() && !blocks.empty()) {
                    payload = std::string{"{\"json\":{\"blocks\":"} + blocks.dump() + "}}";
                }
            }
        } catch (...) {
            // 生成失败按“没有内容”处理 (界面走默认渲染), 不打扰用户
            payload = "{\"json\":{}}";
        }
        const PluginxxStringView view{payload.data(), static_cast<uint64_t>(payload.size())};
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, &view);
    }

private:

    std::string toolName_;
    std::string displayName_;
    ToolRowSpec spec_;
};

/// 注册一个工具行 (声明项 + 内容); 返回的句柄要由插件实例持有
///
/// - 主程序不支持声明式界面时返回 `nullptr` (界面保持默认渲染)
/// - 两条表调用都要等, 所以只能写在协程里:
///
/// ```cpp
/// ctx.toolRows.push_back(
///     co_await clientRegisterToolRowAsync(ctx, "agentxx_fs_read", "Read", spec));
/// ```
template<typename Ctx>
inline asio::awaitable<std::shared_ptr<ToolRow>> clientRegisterToolRowAsync(
    Ctx&           ctx,
    std::string_view toolName,
    std::string_view displayName,
    ToolRowSpec    spec
) {
    const auto* ui = ctx.iface.ui;
    if (!ctx.host || !ui || !ui->register_ui_item || !ui->register_ui_content) {
        co_return nullptr;
    }
    if (toolName.empty()) {
        co_return nullptr;
    }
    auto row = std::make_shared<ToolRow>(
        std::string{toolName},
        std::string{displayName},
        std::move(spec)
    );
    std::string data   = row->declarationDataJson();
    std::string title  = row->displayName();
    auto        idSv   = PluginStringView::from(toolName);
    auto        slotSv = PluginStringView::from(ToolRow::kSlot);
    auto        dataSv = PluginStringView::from(data);

    PluginxxUiEntrySpec entry{};
    entry.version     = 1;
    entry.struct_size = sizeof(PluginxxUiEntrySpec);
    entry.item_id     = idSv;
    entry.type        = slotSv;
    entry.data_json   = dataSv;
    auto entryResult  = co_await detail::clientTableAwaitAsync(
        ctx,
        [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
            return ui->register_ui_item(ctx.host, &entry, cb, ud, err);
        }
    );
    if (!entryResult.ok()) {
        co_return nullptr;
    }

    auto        titleSv = PluginStringView::from(title);
    std::string deps    = "[\"agentxx.client.tool.name\",\"agentxx.client.tool.args\","
                          "\"agentxx.client.tool.result\",\"agentxx.client.tool.width\","
                          "\"agentxx.client.tool.part\",\"agentxx.client.tool.state\"]";
    auto        depsSv  = PluginStringView::from(deps);
    PluginxxUiContentSpec content{};
    content.version     = 1;
    content.struct_size = sizeof(PluginxxUiContentSpec);
    content.id          = idSv;
    content.slot        = slotSv;
    content.title       = titleSv;
    content.deps_json   = depsSv;
    content.generate    = &ToolRow::generate;
    content.user_data   = row.get();
    auto contentResult  = co_await detail::clientTableAwaitAsync(
        ctx,
        [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
            return ui->register_ui_content(ctx.host, &content, cb, ud, err);
        }
    );
    if (!contentResult.ok()) {
        // 内容没注册上就撤销声明项, 不留一个画不出来的位置
        PluginxxString err{nullptr, 0};
        ui->unregister_ui_item(ctx.host, &idSv, nullptr, nullptr, &err);
        if (err.data != nullptr) {
            PluginString::free(ctx.host, &err);
        }
        co_return nullptr;
    }
    co_return row;
}


/* ==================== Client 侧通用交互: ActionController (header-only) ====================
 *
/// 插件侧 Lambda 风格的动作绑定设施 (实例内存 map<动作 id, handler>):
/// - 描述里只传动作名 (函数指针不可序列化, 见三铁律), 映射留在插件侧内存
/// - 只在 IO 线程 on/dispatch (与事件 handler 同约定), 无需锁
/// - dispatch 为 C 回调 (填入 bind_action_handler), 空指针守卫 + 参数解析失败给 {}
///   + handler 异常吞掉记日志, 不外泄 C 边界
///
/// 按钮是**界面描述层**的组件项 (`pluginxx::ui::Item`, 见 [makeButton]): 动作以
/// dispatch 形式写进 `item.action`, 客户端按动作通道派发回本控制器的 handler。
/// 组件加载走 kit: 基础 kit 在 `pluginxx::ui::kit`, 本客户端的常用组合在
/// `agentxx::ui::kit`(生成的 `agentxx/plugin/api/agentxx_ui_kit.g.h`, 自包含)。
 */

namespace kit {

class ActionController {
public:

    using Handler = std::function<void(const utilxx_base::Json& args)>;

    /// 注册动作处理器 (IO 线程; 同 actionId 覆盖)
    void on(std::string actionId, Handler h) {
        handlers_[std::move(actionId)] = std::move(h);
    }

    /// 注销动作处理器 (不存在忽略)
    void off(const std::string& actionId) {
        handlers_.erase(actionId);
    }

    /// 生成按钮组件 (动作 id 自增 act_N; 处理函数留在插件侧 map, 见类头说明)
    /// - `onClick` 为空时仍生成可点按钮 (固定动作名由调用方另行 on() 绑定, 例如
    ///   planning 的常量动作名)
    /// - `color`: 语义色 (空 = 按钮默认色; "accent" / "error" / "hint" 等)
    /// - `variant`: 按钮样式 ("normal" / "full" / "simple"; 空 = normal)
    /// - `args`: 派发时回传给 handler 的参数 (客户端在动作参数里原样带上)
    pluginxx::ui::Item makeButton(
        std::string       label,
        Handler           onClick = nullptr,
        std::string       color    = {},
        utilxx_base::Json args    = utilxx_base::Json::object(),
        std::string       variant = {}
    ) {
        const std::string id = "act_" + std::to_string(++counter_);
        if (onClick) {
            handlers_[id] = std::move(onClick);
        }
        pluginxx::ui::Action action;
        action.kind = pluginxx::ui::Action::Kind::Call;
        action.name = id;
        if (args.is_object() && !args.empty()) {
            action.argsJson = args.dump();
        }
        // 经 agentxx 扩展 kit 组装 (kit 只加载, 语义与手写一致; 生成物见
        // agentxx/plugin/api/agentxx_ui_kit.g.h)
        pluginxx::ui::Json params;
        params["label"]        = std::move(label);
        params["variant"]      = variant.empty() ? std::string{"normal"} : std::move(variant);
        params["action"]       = pluginxx::ui::dumpAction(action);
        pluginxx::ui::Item btn = agentxx::ui::kit::button(params);
        if (!color.empty()) {
            btn.color = std::move(color);
        }
        return btn;
    }

    /// C 回调 (填入 bind_action_handler 的 on_action; ud = 本实例指针)
    static void PLUGINXX_CALL dispatch(const AgentxxUiActionContext* ctx, void* ud) {
        auto* self = static_cast<ActionController*>(ud);
        if (!self || !ctx) {
            return;
        }
        try {
            const std::string actionId(
                ctx->action_id.data ? ctx->action_id.data : "",
                static_cast<size_t>(ctx->action_id.size)
            );
            auto it = self->handlers_.find(actionId);
            if (it == self->handlers_.end() || !it->second) {
                return;
            }
            utilxx_base::Json args = utilxx_base::Json::object();
            if (ctx->action_args.data && ctx->action_args.size > 0) {
                try {
                    auto parsed = utilxx_base::Json::parse(std::string_view{
                        ctx->action_args.data,
                        static_cast<size_t>(ctx->action_args.size)
                    });
                    if (parsed.is_object()) {
                        args = std::move(parsed);
                    }
                } catch (...) {
                    // 解析失败给 {} (约定)
                }
            }
            it->second(args);
        } catch (...) {
            // handler 异常不外泄 C 边界 (主程序另有保护, 此处静默吞掉)
        }
    }

    bool empty() const noexcept {
        return handlers_.empty();
    }

    size_t size() const noexcept {
        return handlers_.size();
    }

private:

    std::unordered_map<std::string, Handler> handlers_;
    uint64_t                                 counter_ = 0;
};

} // namespace kit

/* ==================== 客户端插件实例上下文基类 ==================== */

/// diff overlay 的一段替换 (见 [ClientPluginBase::showDiffOverlay])
struct DiffSegment {
    std::string oldStr; ///< 替换前文本
    std::string newStr; ///< 替换后文本
};

/// 客户端插件实例上下文基类
///
/// 继承 `pluginxx::PluginBaseT<ClientIfaces>`: 拿 `host` / `iface` / `log` 与协程驱动
/// （`bridge()` / `spawn(...)`）。`start` 里的注册事务写成协程
/// （`finishLater(notify)` + `spawn` + 逐条 `co_await clientTableOpAsync(...)`）;
/// 展台类的查询 (客户端状态 / 区域尺寸 / 语言 …) 也都是协程形态 (`*Async`)。
class ClientPluginBase : public pluginxx::PluginBaseT<ClientIfaces> {
public:

    using Base = pluginxx::PluginBaseT<ClientIfaces>;

    kit::ActionController actions;

    ClientPluginBase() = default;

    virtual ~ClientPluginBase() {
        if (lifeToken_) {
            lifeToken_->store(false, std::memory_order_release);
        }
    }

    std::shared_ptr<std::atomic<bool>> lifeToken() const {
        return lifeToken_;
    }

    /// 客户端会话状态 JSON (**协程形态**; 也用 [refreshClientStateAsync] 缓存快照)
    asio::awaitable<std::string> clientStateAsync() const {
        if (!host || !iface.session || !iface.session->get_client_state) {
            co_return std::string{"{}"};
        }
        auto r = co_await detail::clientTableAwaitAsync(
            *this,
            [this](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                return iface.session->get_client_state(host, cb, ud, err);
            }
        );
        co_return r.ok() ? std::string{r.text()} : std::string{"{}"};
    }

    /// 客户端会话状态解析成对象 (**协程形态**; 解析失败给空对象)
    asio::awaitable<utilxx_base::Json> clientStateJsonAsync() const {
        auto text = co_await clientStateAsync();
        if (text.empty() || text == "{}") {
            co_return utilxx_base::Json::object();
        }
        try {
            co_return utilxx_base::Json::parse(text);
        } catch (...) {
            co_return utilxx_base::Json::object();
        }
    }

    /// 实例参数 JSON (**协程形态**)
    asio::awaitable<std::string> argsJsonAsync() const {
        if (!host || !iface.config || !iface.config->get_plugin_args) {
            co_return std::string{"{}"};
        }
        auto r = co_await detail::clientTableAwaitAsync(
            *this,
            [this](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                return iface.config->get_plugin_args(host, cb, ud, err);
            }
        );
        co_return r.ok() ? std::string{r.text()} : std::string{"{}"};
    }

    /// 实例配置文件路径 (**协程形态**)
    asio::awaitable<std::string> configPathAsync() const {
        if (!host || !iface.config || !iface.config->get_plugin_config_path) {
            co_return std::string{};
        }
        auto r = co_await detail::clientTableAwaitAsync(
            *this,
            [this](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                return iface.config->get_plugin_config_path(host, cb, ud, err);
            }
        );
        co_return r.ok() ? std::string{r.text()} : std::string{};
    }

    /// 当前语言代码 (**协程形态**)
    asio::awaitable<std::string> languageAsync() const {
        if (!host || !iface.config || !iface.config->get_language) {
            co_return std::string{"en"};
        }
        auto r = co_await detail::clientTableAwaitAsync(
            *this,
            [this](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                return iface.config->get_language(host, cb, ud, err);
            }
        );
        co_return (r.ok() && !r.text().empty()) ? std::string{r.text()} : std::string{"en"};
    }

    /// 设置语言 (**协程形态**; 返回是否已受理)
    asio::awaitable<bool> setLanguageAsync(std::string lang) const {
        if (!host || !iface.self || !iface.self->set_language) {
            co_return false;
        }
        auto sv = PluginStringView::from(lang);
        auto r  = co_await detail::clientTableAwaitAsync(
            *this,
            [this, &sv](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                return iface.self->set_language(host, &sv, cb, ud, err);
            }
        );
        co_return r.ok();
    }

    /// 显示 toast (只提交, 不等结果)
    void showToast(std::string_view text, int32_t level = 0) const {
        if (!host || !iface.ui || !iface.ui->show_toast) {
            return;
        }
        auto sv = PluginStringView::from(text.data(), text.size());
        iface.ui->show_toast(host, &sv, level, nullptr, nullptr, nullptr);
    }

    /// 懒注册 Info 栏段落 (**提交即返回**)
    ///
    /// 频道/动作回调这类不能写成协程的地方用它: 注册请求立刻提交, 句柄由主程序受理后
    /// 写回本实例的槽位。注册是异步的, 所以**同一轮**里 [infoSection] 可能还拿不到句柄 ——
    /// 内容推送本身也是提交即返回, 下一轮事件补上即可。
    ///
    /// - 同一个 id 重复调用只提交一次 (受理前重入直接返回)
    /// - 主程序不支持 Info 段落时静默降级 (永远拿不到句柄)
    void ensureInfoSection(std::string_view id, std::string_view propsJson) {
        if (id.empty() || !host || !iface.ui || !iface.ui->register_info_section) {
            return;
        }
        const std::string key{id};
        if (infoSections_.find(key) != infoSections_.end()) {
            return;
        }
        auto slot                     = std::make_shared<std::atomic<AgentxxInfoSection*>>(nullptr);
        infoSections_[key]            = slot;
        auto  idSv                    = PluginStringView::from(id.data(), id.size());
        auto  propsSv                 = PluginStringView::from(propsJson.data(), propsJson.size());
        // 完成回调在主程序 io 线程: 只把句柄写进原子槽位, 不碰其它实例状态
        auto* holder = new std::shared_ptr<std::atomic<AgentxxInfoSection*>>(slot);
        PluginxxString err{nullptr, 0};
        auto*          handle = iface.ui->register_info_section(
            host,
            &idSv,
            &propsSv,
            [](void* ud, int32_t status, const PluginxxStringView* payload) {
                std::unique_ptr<std::shared_ptr<std::atomic<AgentxxInfoSection*>>> keep(
                    static_cast<std::shared_ptr<std::atomic<AgentxxInfoSection*>>*>(ud)
                );
                if (status != PLUGINXX_OPERATOR_OK || payload == nullptr || payload->data == nullptr) {
                    return;
                }
                auto* section = clientHandleFromText<AgentxxInfoSection>(
                    std::string_view{payload->data, static_cast<size_t>(payload->size)}
                );
                (*keep)->store(section, std::memory_order_release);
            },
            holder,
            &err
        );
        if (err.data != nullptr) {
            PluginString::free(host, &err);
        }
        if (handle == nullptr) {
            // 未受理: 完成回调不会被调用, 包装块自己收
            delete holder;
            infoSections_.erase(key);
        }
    }

    /// 取 [ensureInfoSection] 注册出来的段落句柄 (尚未受理/不支持时为 nullptr)
    AgentxxInfoSection* infoSection(std::string_view id) const {
        const auto it = infoSections_.find(std::string{id});
        if (it == infoSections_.end() || !it->second) {
            return nullptr;
        }
        return it->second->load(std::memory_order_acquire);
    }

    /// 注销 [ensureInfoSection] 的段落 (**提交即返回**; 同时忘掉句柄)
    void clearInfoSection(std::string_view id) {
        AgentxxInfoSection* section = infoSection(id);
        infoSections_.erase(std::string{id});
        if (section == nullptr || !host || !iface.ui || !iface.ui->unregister_info_section) {
            return;
        }
        clientTableCall(host, iface, [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
            return iface.ui->unregister_info_section(host, section, cb, ud, err);
        });
    }

    /// 代发用户消息 (**协程形态**); 返回是否已受理
    asio::awaitable<bool> sendUserInputAsync(std::string sessionId, std::string text) const {
        if (!host || !iface.session || !iface.session->send_user_input) {
            co_return false;
        }
        auto sidSv = PluginStringView::from(sessionId);
        auto txtSv = PluginStringView::from(text);
        auto r     = co_await detail::clientTableAwaitAsync(
            *this,
            [this, &sidSv, &txtSv](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                return iface.session->send_user_input(host, &sidSv, &txtSv, cb, ud, err);
            }
        );
        co_return r.ok();
    }

    /// 请求取消当前轮次 (只提交, 不等结果)
    void requestCancel(std::string_view sessionId) const {
        if (!host || !iface.session || !iface.session->request_cancel) {
            return;
        }
        auto sidSv = PluginStringView::from(sessionId.data(), sessionId.size());
        iface.session->request_cancel(host, &sidSv, nullptr, nullptr, nullptr);
    }

    // ==================== 展示扩展便捷方法 ====================
    //
    // 这些方法是展示注册接口的薄封装: 用界面描述层 (cxx_pluginxx_ui, `pluginxx::ui`)
    // 的构建器 (`pluginxx::ui::build` / kit) 或直接构造组件项后提交给面板/Info 段落/
    // overlay, 避免各处手写 JSON 拼装。
    //
    // 更新类方法一律**提交即返回** (面板内容/状态栏文本/Info 段落是单向推送, 插件不需要
    // 结果), 因此它们可以从任何线程调用, 包括插件执行线程与渲染回调 —— 不会等 io 线程。
    //
    // 插件可用 `agentxx::ui` 指代描述层与扩展 kit (`agentxx::ui::kit`)。

    /// 组件树 → 提交给主程序的 JSON 形态 (`{"items":[…]}`)
    static std::string itemsJson(const std::vector<pluginxx::ui::Item>& items) {
        utilxx_base::Json j = utilxx_base::Json::object();
        j["items"]          = pluginxx::ui::dumpBlocks(items);
        return j.dump();
    }

    /// 更新面板内容 (组件树直接提交; 提交即返回)
    int32_t setPanelItems(AgentxxPanel* panel, const std::vector<pluginxx::ui::Item>& items) const {
        return setPanelJson(panel, itemsJson(items));
    }

    /// 更新面板内容 (原始 items JSON, 形态同 `{"items":[...]}` 或裸数组)
    int32_t setPanelJson(AgentxxPanel* panel, std::string_view itemsJson) const {
        if (!host || !iface.ui || !iface.ui->update_panel || panel == nullptr) {
            return AGENTXX_RESULT_ERR_UNSUPPORTED;
        }
        auto sv = PluginStringView::from(itemsJson.data(), itemsJson.size());
        clientTableCall(host, iface, [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
            return iface.ui->update_panel(host, panel, &sv, cb, ud, err);
        });
        return AGENTXX_RESULT_OK;
    }

    /// 更新 Info 栏段落内容 (组件树直接提交; 提交即返回)
    int32_t setInfoSectionItems(
        AgentxxInfoSection*                    section,
        const std::vector<pluginxx::ui::Item>& items
    ) const {
        if (!host || !iface.ui || !iface.ui->update_info_section || section == nullptr) {
            return AGENTXX_RESULT_ERR_UNSUPPORTED;
        }
        const std::string json = itemsJson(items);
        auto              sv   = PluginStringView::from(json.data(), json.size());
        clientTableCall(host, iface, [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
            return iface.ui->update_info_section(host, section, &sv, cb, ud, err);
        });
        return AGENTXX_RESULT_OK;
    }

    /// 更新状态栏项文本 (参数为 `{"text":"...","tooltip":"..."}` 形态的 JSON; 提交即返回)
    int32_t setStatusJson(AgentxxStatusItem* item, std::string_view json) const {
        if (!host || !iface.ui || !iface.ui->update_status_item || item == nullptr) {
            return AGENTXX_RESULT_ERR_UNSUPPORTED;
        }
        auto sv = PluginStringView::from(json.data(), json.size());
        clientTableCall(host, iface, [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
            return iface.ui->update_status_item(host, item, &sv, cb, ud, err);
        });
        return AGENTXX_RESULT_OK;
    }

    /// 更新状态栏项文本 (仅文本, 由本方法组装 JSON; 提交即返回)
    int32_t setStatusText(AgentxxStatusItem* item, std::string_view text) const {
        utilxx_base::Json json = utilxx_base::Json::object();
        json["text"]           = text;
        return setStatusJson(item, json.dump());
    }

    /// 打开自定义 overlay (组件树作为内容)
    /// - `extraJson` 为扩展 JSON (如 `{"size":"large"}`), 可空
    int32_t showItemsOverlay(
        std::string_view                       title,
        const std::vector<pluginxx::ui::Item>& items,
        std::string_view                       extraJson = {}
    ) const {
        return showOverlay(AGENTXX_OVERLAY_CUSTOM, title, itemsJson(items), extraJson);
    }

    /// 打开通用 overlay (type 见 AgentxxOverlayType; payload 语义随类型)
    int32_t showOverlay(
        int              type,
        std::string_view title,
        std::string_view payload,
        std::string_view extraJson = {}
    ) const {
        if (!host || !iface.ui || !iface.ui->open_overlay) {
            return AGENTXX_RESULT_ERR_UNSUPPORTED;
        }
        AgentxxOverlaySpec spec{};
        spec.version     = 1;
        spec.struct_size = sizeof(AgentxxOverlaySpec);
        spec.type        = type;
        spec.title       = PluginStringView::from(title.data(), title.size());
        spec.payload     = PluginStringView::from(payload.data(), payload.size());
        spec.extra_json  = PluginStringView::from(extraJson.data(), extraJson.size());
        clientTableCall(host, iface, [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
            return iface.ui->open_overlay(host, &spec, cb, ud, err);
        });
        return AGENTXX_RESULT_OK;
    }

    /// 打开 diff overlay (单段或多段, 每段一次替换; 主程序按逐行差异渲染)
    /// - `segments` 为空时打开一个"无变化"的 diff (不会失败)
    /// - 多段: 每段各渲染一份逐行差异, 段间以分隔行隔开, 文件路径只在首段展示
    /// - 单段调用按旧写法 `{path,old_str,new_str}` 下发, 老主程序也能正确渲染;
    ///   多段用的是 `edits` 字段, 老主程序不认识时只显示"(no changes)"
    ///   (插件与客户端版本需要一起升级时按单段逐次调用即可)
    int32_t showDiffOverlay(
        std::string_view                title,
        std::string_view                path,
        const std::vector<DiffSegment>& segments,
        std::string_view                extraJson = {}
    ) const {
        utilxx_base::Json payload = utilxx_base::Json::object();
        if (!path.empty()) {
            payload["path"] = path;
        }
        if (segments.size() == 1) {
            // 单段: 用旧写法 (老主程序也能正确渲染)
            payload["old_str"] = segments.front().oldStr;
            payload["new_str"] = segments.front().newStr;
        } else if (!segments.empty()) {
            utilxx_base::Json edits = utilxx_base::Json::array();
            for (const auto& seg : segments) {
                utilxx_base::Json one = utilxx_base::Json::object();
                one["old_str"]        = seg.oldStr;
                one["new_str"]        = seg.newStr;
                edits.push_back(std::move(one));
            }
            payload["edits"] = std::move(edits);
        } else {
            payload["old_str"] = std::string{};
            payload["new_str"] = std::string{};
        }
        return showOverlay(AGENTXX_OVERLAY_DIFF, title, payload.dump(), extraJson);
    }

    // ==================== 组件树就地构建 ====================

    /// 面板内容就地构建器 (作用域结束时自动提交)
    ///
    /// 用途: 插件刷新面板的常见写法是"重新构造整棵组件树并提交", 用本句柄可省掉
    /// 临时变量与显式提交调用:
    /// ```c++
    /// auto ui = panelItems(panel);
    /// ui.add(pluginxx::ui::build::title("System"));
    /// ui.add(pluginxx::ui::build::progress(cpu, 100, "%"));
    /// // 离开作用域时自动 update_panel (需要检查返回值时显式调用 commit())
    /// ```
    class PanelWriter {
    public:

        PanelWriter(const ClientPluginBase* base, AgentxxPanel* panel) :
            base_(base),
            panel_(panel) {}

        ~PanelWriter() {
            commit();
        }

        PanelWriter(const PanelWriter&)            = delete;
        PanelWriter& operator=(const PanelWriter&) = delete;

        PanelWriter(PanelWriter&& other) noexcept :
            base_(other.base_),
            panel_(other.panel_),
            items_(std::move(other.items_)),
            committed_(other.committed_) {
            other.committed_ = true; // 源对象不再提交 (树已移走)
        }

        /// 组件树 (用 `pluginxx::ui::build` / kit 构造组件后追加)
        std::vector<pluginxx::ui::Item>& items() {
            return items_;
        }

        std::vector<pluginxx::ui::Item>& operator*() {
            return items_;
        }

        /// 追加一个组件项
        PanelWriter& add(pluginxx::ui::Item item) {
            items_.push_back(std::move(item));
            return *this;
        }

        /// 追加一组组件项
        PanelWriter& add(std::vector<pluginxx::ui::Item> items) {
            for (auto& item : items) {
                items_.push_back(std::move(item));
            }
            return *this;
        }

        /// 提交 (幂等; 返回主程序状态码, `AGENTXX_RESULT_OK` = 成功)
        int32_t commit() {
            if (committed_) {
                return AGENTXX_RESULT_OK;
            }
            committed_ = true;
            if (base_ == nullptr) {
                return AGENTXX_RESULT_ERR_UNSUPPORTED;
            }
            return base_->setPanelItems(panel_, items_);
        }

    private:

        const ClientPluginBase*         base_  = nullptr;
        AgentxxPanel*                   panel_ = nullptr;
        std::vector<pluginxx::ui::Item> items_{};
        bool                            committed_ = false;
    };

    /// 面板内容就地构建 (见 [PanelWriter])
    PanelWriter panelItems(AgentxxPanel* panel) const {
        return PanelWriter(this, panel);
    }

    /// 工具消息装饰参数 (折叠头显示名/摘要 + 展开体组件树)
    struct DecorSpec {
        std::string                     displayName; ///< 折叠头显示名 (空 = 原始工具名)
        std::string                     summary;     ///< 折叠头一行摘要 (空 = 参数预览)
        std::vector<pluginxx::ui::Item> items;       ///< 展开体组件树
    };

    /// 更新某次工具调用的消息装饰 (见 [DecorSpec])
    /// - 空 `toolCallId` 或主程序不支持消息装饰时返回非 0 (插件应降级为不装饰)
    int32_t setToolDecor(std::string_view toolCallId, const DecorSpec& decor) const {
        if (!host || !iface.ui || !iface.ui->update_tool_decor) {
            return AGENTXX_RESULT_ERR_UNSUPPORTED;
        }
        if (toolCallId.empty()) {
            return AGENTXX_RESULT_ERR_ARG;
        }
        utilxx_base::Json j = utilxx_base::Json::object();
        if (!decor.displayName.empty()) {
            j["displayName"] = decor.displayName;
        }
        if (!decor.summary.empty()) {
            j["summary"] = decor.summary;
        }
        j["items"]             = pluginxx::ui::dumpBlocks(decor.items);
        const std::string json = j.dump();
        auto              sv   = PluginStringView::from(json.data(), json.size());
        auto              tid  = PluginStringView::from(toolCallId.data(), toolCallId.size());
        // 只提交不等结果: 装饰不是调用方要判断成败的东西 (失败在主程序侧记日志)
        const bool submitted = clientTableCall(
            host,
            iface,
            [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                return iface.ui->update_tool_decor(host, &tid, &sv, cb, ud, err);
            }
        );
        return submitted ? AGENTXX_RESULT_OK : AGENTXX_RESULT_ERR_FAILED;
    }

    /// 清除某次工具调用的消息装饰 (空 `toolCallId` = 清除本插件全部装饰)
    int32_t clearToolDecor(std::string_view toolCallId = {}) const {
        if (!host || !iface.ui || !iface.ui->update_tool_decor) {
            return AGENTXX_RESULT_ERR_UNSUPPORTED;
        }
        auto tid = PluginStringView::from(toolCallId.data(), toolCallId.size());
        auto sv  = PluginStringView::from("", 0);
        const bool submitted = clientTableCall(
            host,
            iface,
            [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                return iface.ui->update_tool_decor(host, &tid, &sv, cb, ud, err);
            }
        );
        return submitted ? AGENTXX_RESULT_OK : AGENTXX_RESULT_ERR_FAILED;
    }

    // ==================== 能力协商查询 ====================

    /// 刷新客户端状态快照 (**协程形态**)
    ///
    /// `get_client_state()` 是一次表调用, 只能在协程里等; 下面的 [clientStateSnapshot] /
    /// [hostSupports] / [uiCapabilities] 读的是这次刷新的结果, 所以**注册事务里先调一次**
    /// (之后再按需要刷新, 例如换语言后)。没刷新过时快照是空对象:
    /// [hostSupports] 返回 false、[uiCapabilities] 按"描述层全支持"处理。
    asio::awaitable<void> refreshClientStateAsync() {
        auto text = co_await clientStateAsync();
        auto parsed = utilxx_base::Json::object();
        if (!text.empty() && text != "{}") {
            try {
                parsed = utilxx_base::Json::parse(text);
            } catch (...) {
                parsed = utilxx_base::Json::object();
            }
        }
        clientStateCache_ = std::move(parsed);
        uiCapsCached_     = false;
    }

    /// 最近一次 [refreshClientStateAsync] 拿到的客户端状态快照 (未刷新过时是空对象)
    const utilxx_base::Json& clientStateSnapshot() const {
        return clientStateCache_;
    }

    /// 主程序是否声明了某个能力名 (查快照的 `interfaces`)
    /// - 能力名常量见 `agentxx/plugin/plugin_interfaces.h` 的 plugin_interfaces 命名空间
    ///   (如 `agentxx.client.components` / `agentxx.client.form`)
    /// - 读的是 [refreshClientStateAsync] 的快照: 注册事务里没刷新过时为 false
    ///   (老主程序本来也没有这些能力, 降级路径一致)
    bool hostSupports(std::string_view capability) const {
        if (capability.empty()) {
            return false;
        }
        const auto it = clientStateCache_.find("interfaces");
        if (it == clientStateCache_.end() || !it->is_array()) {
            return false;
        }
        for (const auto& entry : *it) {
            if (entry.is_string() && entry.get_string_view() == capability) {
                return true;
            }
        }
        return false;
    }

    /// 客户端界面能力段 (状态快照的 `ui` 段)
    ///
    /// 客户端如实上报自己能画什么: 支持的组件/控件、每个字符格相当于多少 u、默认
    /// 行距。插件据此选择组件, 例如终端不推图片/着色器:
    /// ```c++
    /// if (kit.supportsBlock("Table")) { ... } else { ... }
    /// ```
    /// 没刷新过快照 (或老主程序没这一段) 时按"描述层全支持"处理 —— 内容仍由客户端
    /// 按自身能力降级 (见描述层的 `adapt`), 不会画错。
    ///
    /// 解析结果按实例缓存 (客户端能力在生命周期内不变), 因此在渲染/拼装路径里反复
    /// 调用不会有 JSON 解析开销。
    const pluginxx::ui::Capabilities& uiCapabilities() const {
        if (!uiCapsCached_) {
            uiCapsCached_    = true;
            const auto it    = clientStateCache_.find("ui");
            uiCapsCache_     = (clientStateCache_.is_object() && it != clientStateCache_.end()
                            && it->is_object())
                                   ? pluginxx::ui::capabilitiesFromJson(*it)
                                   : pluginxx::ui::fullCapabilities();
        }
        return uiCapsCache_;
    }

    /// 客户端是否支持某个组件 (按能力段判断; 未上报时按支持处理)
    bool supportsBlock(std::string_view name) const {
        return uiCapabilities().supportsBlock(name);
    }

    /// kit 的环境参数 (客户端能力摘要)
    ///
    /// 传给 kit 组件时, kit 会按目标挑选更合适的变体 (例如目标不支持 `Image` 时
    /// 用文字行); 不传时产出中立描述, 由客户端渲染前的 `adapt` 统一处理。
    /// ```c++
    /// items.push_back(agentxx::ui::kit::listRow({{"title", "切歌次数"}}, kitEnv()));
    /// ```
    const pluginxx::ui::Capabilities* kitEnv() const {
        return &uiCapabilities();
    }

    /// 展示区域尺寸 (宽/高; 未上报时返回 0)
    /// - 区域 id 为面板/Info 段落的注册 id (`update_panel` / `update_info_section` 的 id)
    /// - 主程序在每次布局后更新快照, 值变化时会另发 `agentxx.client.ui_layout` 频道;
    ///   老主程序 (未声明 `agentxx.client.layout`) 该接口返回 0, 插件应降级为固定宽度
    struct RegionSize {
        int width  = 0;
        int height = 0;
    };

    /// 查一个展示区域当前尺寸 (**协程形态**; 每次都要问主程序, 所以要 `co_await`)
    asio::awaitable<RegionSize> regionSizeAsync(std::string regionId) const {
        RegionSize out;
        if (regionId.empty() || !host) {
            co_return out;
        }
        auto json = co_await clientStateJsonAsync();
        const auto it = json.find("regions");
        if (it == json.end() || !it->is_array()) {
            co_return out;
        }
        for (const auto& entry : *it) {
            if (!entry.is_object()) {
                continue;
            }
            if (entry.value("id", std::string{}) != regionId) {
                continue;
            }
            out.width  = entry.value("w", 0);
            out.height = entry.value("h", 0);
            break;
        }
        co_return out;
    }

    // ==================== 定时器 (`agentxx.client.timer`) ====================

    /// 定时器参数
    struct TimerOptions {
        /// 0 = 一次性; > 0 = 周期触发次数上限 (<=0 视为一次性)
        int repeat = 0;
        /// 关联的展示区域 id (面板/Info 段落 id; 空 = 不关联)
        std::string ownerId = "";
        /// 关联区域不可见时跳过回调 (高频刷新面板时建议开启)
        bool pauseWhenHidden = true;
    };

    /// 定时器句柄: 持有主程序句柄与 std::function; 析构时自动取消
    /// - 插件只需把 shared_ptr 保存在自己的实例上下文里 (实例销毁即取消)
    class TimerHandle {
    public:

        TimerHandle() = default;

        TimerHandle(
            const PluginxxHost*                    h,
            AgentxxTimer*                          t,
            std::shared_ptr<std::function<void()>> fn
        ) :
            host_(h),
            timer_(t),
            fn_(std::move(fn)) {}

        TimerHandle(const TimerHandle&)            = delete;
        TimerHandle& operator=(const TimerHandle&) = delete;

        ~TimerHandle() {
            reset();
        }

        void reset() {
            if (host_ && timer_ && host_->vtable && host_->vtable->query_interface) {
                PluginxxStringView iid = PluginStringView::from(AGENTXX_IFACE_CLIENT_TIMER);
                if (auto* table = static_cast<const AgentxxClientTimerIface*>(
                        host_->vtable->query_interface(host_, &iid)
                    );
                    table && table->cancel_timer) {
                    // 只提交, 不等结果 (在 io 线程上也是内联的)
                    table->cancel_timer(host_, timer_, nullptr, nullptr, nullptr);
                }
            }
            timer_ = nullptr;
            fn_.reset();
        }

        bool valid() const {
            return timer_ != nullptr;
        }

        AgentxxTimer* raw() const {
            return timer_;
        }

    private:

        const PluginxxHost*                    host_  = nullptr;
        AgentxxTimer*                          timer_ = nullptr;
        std::shared_ptr<std::function<void()>> fn_;
    };

    /// 注册定时器 (**协程形态**; 返回句柄, 失败返回 nullptr)
    /// - 失败: spec 非法 / 主程序动画等级 Disabled / 单实例定时器超上限;
    ///   插件应据返回值降级 (不注册即静态展示)
    /// - 回调在 client io 线程执行; 回调内可更新面板/Info/overlay 描述
    asio::awaitable<std::shared_ptr<TimerHandle>> registerTimerAsync(
        int                   intervalMs,
        std::function<void()> fn,
        TimerOptions          opts
    ) {
        if (!host || !iface.timer || !iface.timer->set_timer || !fn) {
            co_return nullptr;
        }
        auto             holder = std::make_shared<std::function<void()>>(std::move(fn));
        AgentxxTimerSpec spec{};
        spec.struct_size       = sizeof(AgentxxTimerSpec);
        spec.version           = 1;
        spec.interval_ms       = intervalMs;
        spec.repeat            = (opts.repeat > 0) ? opts.repeat : 0;
        spec.pause_when_hidden = opts.pauseWhenHidden ? 1 : 0;
        spec.owner_id          = PluginStringView::from(opts.ownerId.data(), opts.ownerId.size());
        spec.on_timer          = [](void* ud) {
            auto* f = static_cast<std::function<void()>*>(ud);
            if (f && *f) {
                (*f)();
            }
        };
        spec.user_data = holder.get();
        auto* timer    = clientHandleOf<AgentxxTimer>(co_await detail::clientTableAwaitAsync(
            *this,
            [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                return iface.timer->set_timer(host, &spec, cb, ud, err);
            }
        ));
        if (!timer) {
            co_return nullptr;
        }
        co_return std::make_shared<TimerHandle>(host, timer, std::move(holder));
    }

    /// 关联展示区域当前是否可见 (**协程形态**; 老主程序无该表返回 false)
    asio::awaitable<bool> isRegionVisibleAsync(std::string regionId) const {
        if (!host || !iface.timer || !iface.timer->is_visible || regionId.empty()) {
            co_return false;
        }
        auto id = PluginStringView::from(regionId);
        auto r  = co_await detail::clientTableAwaitAsync(
            *this,
            [this, &id](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                return iface.timer->is_visible(host, &id, cb, ud, err);
            }
        );
        co_return r.ok() && r.flag();
    }

    // ==================== 全局快捷键 (`agentxx.client.keybind`) ====================

    /// 快捷键句柄: 持有主程序句柄与 std::function; 析构时自动注销
    class KeybindHandle {
    public:

        KeybindHandle() = default;

        KeybindHandle(
            const PluginxxHost*                    h,
            AgentxxKeybind*                        b,
            std::shared_ptr<std::function<void()>> fn
        ) :
            host_(h),
            bind_(b),
            fn_(std::move(fn)) {}

        KeybindHandle(const KeybindHandle&)            = delete;
        KeybindHandle& operator=(const KeybindHandle&) = delete;

        ~KeybindHandle() {
            reset();
        }

        void reset() {
            if (host_ && bind_ && host_->vtable && host_->vtable->query_interface) {
                PluginxxStringView iid = PluginStringView::from(AGENTXX_IFACE_CLIENT_KEYBIND);
                if (auto* table = static_cast<const AgentxxClientKeybindIface*>(
                        host_->vtable->query_interface(host_, &iid)
                    );
                    table && table->unregister_keybind) {
                    table->unregister_keybind(host_, bind_, nullptr, nullptr, nullptr);
                }
            }
            bind_ = nullptr;
            fn_.reset();
        }

        bool valid() const {
            return bind_ != nullptr;
        }

    private:

        const PluginxxHost*                    host_ = nullptr;
        AgentxxKeybind*                        bind_ = nullptr;
        std::shared_ptr<std::function<void()>> fn_;
    };

    /// 注册全局快捷键 (**协程形态**; 返回句柄, 失败返回 nullptr)
    /// - keys 格式见 [AgentxxKeybindSpec]: `"ctrl+alt+k"` / `"f9"` 等;
    ///   无修饰键的可打印字符不参与匹配 (注册失败)
    /// - 冲突 (同键位已被占用) 时返回 nullptr, 插件应降级或改用其它键位
    asio::awaitable<std::shared_ptr<KeybindHandle>> registerKeybindAsync(
        std::string           keys,
        std::string           description,
        std::function<void()> fn
    ) {
        if (!host || !iface.keybind || !iface.keybind->register_keybind || !fn) {
            co_return nullptr;
        }
        auto               holder = std::make_shared<std::function<void()>>(std::move(fn));
        AgentxxKeybindSpec spec{};
        spec.struct_size = sizeof(AgentxxKeybindSpec);
        spec.version     = 1;
        spec.keys        = PluginStringView::from(keys.data(), keys.size());
        spec.description = PluginStringView::from(description.data(), description.size());
        spec.on_keybind  = [](void* ud) {
            auto* f = static_cast<std::function<void()>*>(ud);
            if (f && *f) {
                (*f)();
            }
        };
        spec.user_data = holder.get();
        auto* bind     = clientHandleOf<AgentxxKeybind>(co_await detail::clientTableAwaitAsync(
            *this,
            [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                return iface.keybind->register_keybind(host, &spec, cb, ud, err);
            }
        ));
        if (!bind) {
            co_return nullptr;
        }
        co_return std::make_shared<KeybindHandle>(host, bind, std::move(holder));
    }

    /// 当前已注册的快捷键列表 JSON (**协程形态**; 老主程序返回 "[]")
    asio::awaitable<std::string> keybindListJsonAsync() const {
        if (!host || !iface.keybind || !iface.keybind->list_keybinds) {
            co_return std::string{"[]"};
        }
        auto r = co_await detail::clientTableAwaitAsync(
            *this,
            [this](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                return iface.keybind->list_keybinds(host, cb, ud, err);
            }
        );
        co_return (r.ok() && !r.text().empty()) ? std::string{r.text()} : std::string{"[]"};
    }

private:

    std::shared_ptr<std::atomic<bool>> lifeToken_ = std::make_shared<std::atomic<bool>>(true);
    /// 懒注册的 Info 段落槽位 (id → 句柄; 句柄由主程序受理后写回, 只在插件执行线程上读写)
    std::map<std::string, std::shared_ptr<std::atomic<AgentxxInfoSection*>>> infoSections_;
    /// 界面能力段解析结果 (能力在客户端生命周期内不变; 见 [uiCapabilities])
    mutable bool                       uiCapsCached_ = false;
    mutable pluginxx::ui::Capabilities uiCapsCache_{};
    /// 客户端状态快照 ([refreshClientStateAsync] 填;[hostSupports] / [uiCapabilities] 读)
    utilxx_base::Json                  clientStateCache_ = utilxx_base::Json::object();
};

/// client 侧 create 阶段异常上报 (两侧统一用插件框架的 `pluginxx.log` 表)
inline void logClientCreateFailure(
    const PluginxxHost* host,
    std::string_view    plugin,
    std::string_view    msg
) noexcept {
    if (!host || !host->vtable || !host->vtable->query_interface) {
        return;
    }
    auto  iid = PluginStringView::fromCstr(PLUGINXX_IFACE_LOG);
    auto* iface
        = static_cast<const PluginxxLogIface*>(host->vtable->query_interface(host, &iid));
    if (!iface || !iface->log) {
        return;
    }
    std::string text = fmt::format("[{}] create failed: {}", plugin, msg);
    auto        sv   = PluginStringView::from(text.data(), text.size());
    iface->log(host, 4, &sv);
}

/// Client 侧只导出 stop 入口 (与 [AGENTXX_AGENT_PLUGIN_LIFECYCLE_EXPORT] 对称)。
#define AGENTXX_CLIENT_PLUGIN_LIFECYCLE_EXPORT(CtxType, StopFn) \
    PLUGINXX_EXPORT_PLUGIN_LIFECYCLE(agentxx_client_plugin_, CtxType, StopFn)

/// Client 侧插件入口导出 (与 [AGENTXX_AGENT_PLUGIN_EXPORT] 对称)。
///
/// `CreateFn` / `StopFn` 形如 `void(CtxType&, const PluginxxOperatorNotify*, PluginxxString*)`;
/// create 里做 UI 项/命令/订阅注册, stop 里撤销插件自管资源 (线程/定时器/订阅)。
/// 纯 UI 插件 (无 agent 侧入口) 只导出这四个入口即可。
#define AGENTXX_CLIENT_PLUGIN_EXPORT(CtxType, Name, Ver, Desc, CreateFn, StopFn) \
    PLUGINXX_EXPORT_PLUGIN(agentxx_client_plugin_, CtxType, Name, Ver, Desc, CreateFn, StopFn)
} // namespace plugin
} // namespace agentxx
