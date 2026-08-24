#pragma once

#include "agentxx/agent/config.h"
#include "agentxx/agent/io/client_event_sink.h"
#include "agentxx/plugin/client_plugin_api.h"
#include "agentxx/plugin/plugin_common.h" /* PluginManifestInterfaces / InterfaceSet */
#include "asio/any_io_executor.hpp"
#include "asio/awaitable.hpp"
#include "asio/thread_pool.hpp"
#include "neograph/json.h"
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace agentxx {
namespace plugin {

class ClientPluginManager;
class PluginUiAdapter;

/// 状态栏项注册记录 (UI 注册表快照条目; 所有字段为宿主拷贝, 可跨线程读取)
struct ClientStatusItem {
    std::string plugin;    ///< 所属插件名
    std::string id;        ///< 全局唯一 id
    std::string text;      ///< 当前文本
    int         align = 0; ///< 0=左侧 1=右侧
    int         order = 0; ///< 组内排序 (小在前)
};

/// 面板注册记录 (UI 注册表快照条目)
struct ClientPanel {
    std::string    plugin;                          ///< 所属插件名
    std::string    id;                              ///< 全局唯一 id
    std::string    title;                           ///< tab 标题
    neograph::json items = neograph::json::array(); ///< {"items":[{...}]} 内容
};

/// Info 栏段落注册记录 (UI 注册表快照条目)
/// - 渲染在侧边栏 Info tab 内 (段落标题 + items, 与面板 items schema 一致),
///   供插件把摘要/状态信息注入 Info 栏 (如 codegraph 索引状态、系统资源占用)
struct ClientInfoSection {
    std::string    plugin;                          ///< 所属插件名
    std::string    id;                              ///< 全局唯一 id
    std::string    title;                           ///< 段落标题 (空 = 无标题)
    neograph::json items = neograph::json::array(); ///< {"items":[{...}]} 内容
};

/// 命令注册记录
struct ClientCommand {
    std::string plugin; ///< 所属插件名
    std::string name;   ///< 命令名 (用户输入 "/{name}" 触发)
    std::string description;
    char* (*execute)(void* ud, AgentxxPluginStringView args_json, char** error_out) = nullptr;
    void* ud                                                                        = nullptr;
};

/// UI 注册表快照 (UI 线程渲染读取; COW shared_ptr 语义)
struct ClientUiRegistry {
    std::vector<ClientStatusItem>  statusItems;
    std::vector<ClientPanel>       panels;
    std::vector<ClientInfoSection> infoSections;
    std::vector<ClientCommand>     commands;
};

/// client 插件实例 (宿主侧状态)
/// - 与 agent 侧 PluginInstance 对称: 同一动态库可被 agent 与 client 两个
///   管理器各自 dlopen (引用计数), 实例状态彼此独立, 互通一律走 wire
/// - 所有注册残留 (status item/panel/command/订阅) 记录于此, 卸载时统一清理
/// - 仅 client io 线程读写 (inflight 为原子, 跨线程递增/递减)
class ClientPluginInstance {
public:

    std::string name;
    std::string version;
    std::string description;
    std::string path; ///< 加载的库路径
    /// 插件配置参数 (yaml `plugins` 条目 args; 宿主原样保存, 经 vtable
    /// get_plugin_args 整体返回给插件, 不解析其字段语义)
    neograph::json args = neograph::json::object();
    /// 必选依赖 (插件名): 未安装则加载失败; 卸载/禁用时级联
    std::vector<std::string> depends;
    /// 可选依赖 (插件名): 未安装仅警告, 不影响加载
    std::vector<std::string> optionalDepends;
    /// 接口声明 (plugin.yaml `interfaces`; 加载时随 manifest 解析传入,
    /// 直连库路径为空) —— 宿主门禁依据, 经 list() 暴露供展示/排查
    PluginManifestInterfaces interfaces;
    void*                    dlHandle  = nullptr; ///< dlopen/LoadLibrary 句柄
    void*                    pluginCtx = nullptr; ///< entry 输出的插件私有上下文
    bool                     enabled   = true; ///< 是否启用 (禁用: UI 项摘除/命令停用)
    bool userDisabled    = false; ///< 是否被用户显式禁用 (区别于级联禁用)
    bool unloadRequested = false; ///< 已请求卸载 (防重复)

    /// 本插件专属宿主句柄 (vtable 为宿主静态函数表, opaque 指向本实例)
    AgentxxClientHost host{};

    /// 在途回调计数 (原子, 跨线程: 事件 handler/命令 execute)
    std::atomic<size_t> inflight{0};

    /// 事件订阅记录 (卸载自动退订; 仅 io 线程)
    /// - shared_ptr 存储: 订阅节点地址稳定 (vector 扩容/erase 不悬垂);
    ///   dispatch 时拷贝 shared_ptr 保活, 派发中退订/卸载不 UAF
    struct Subscription {
        int event                                                       = 0;
        void (*handler)(AgentxxPluginStringView payload_json, void* ud) = nullptr;
        void* ud                                                        = nullptr;
        bool  alive = true; ///< 已退订标记 (unsubscribe 置 false, 卸载清理用)
    };

    /// 注册残留 (卸载时统一清理; 仅 io 线程)
    /// - disable 时【完整注册信息】(statusItemRegs/panelRegs/infoSectionRegs/
    ///   commandRegs) 保留, enable 可恢复; 仅 unload/进程销毁时随实例释放
    /// - 活跃句柄 (statusItemHandles/panelHandles/infoSectionHandles/subHandles)
    ///   为宿主对象 (id/plugin 等), enable 期间有效
    std::vector<ClientStatusItem>  statusItemRegs;  ///< 状态栏项注册信息 (disable 保留)
    std::vector<ClientPanel>       panelRegs;       ///< 面板注册信息 (disable 保留)
    std::vector<ClientInfoSection> infoSectionRegs; ///< Info 段落注册信息 (disable 保留)
    std::vector<ClientCommand>     commandRegs;     ///< 命令注册信息 (disable 保留)
    std::vector<std::shared_ptr<Subscription>> subscriptions; ///< 已订阅事件 (disable 保留)
    std::vector<std::shared_ptr<void>> statusItemHandles; ///< 状态栏项宿主句柄 (enable 期)
    std::vector<std::shared_ptr<void>> panelHandles;      ///< 面板宿主句柄 (enable 期)
    std::vector<std::shared_ptr<void>> infoSectionHandles; ///< Info 段落宿主句柄 (enable 期)
    std::vector<std::shared_ptr<void>> subHandles;         ///< 订阅句柄保活

    /// 管理器弱引用 (host vtable 回调取用)
    std::weak_ptr<ClientPluginManager> manager{};

    explicit ClientPluginInstance(std::string in_name) :
        name(std::move(in_name)) {}

    /// 析构时 dlclose (与 agent 侧 PluginInstance 一致; 调用方保证无在途回调:
    /// unloadAsync 等 inflight 归零后移除, shutdownAll 进程退出路径约定无在途)
    ~ClientPluginInstance();

    /// 在途计数 RAII (事件 handler / 命令 execute 入口调用)
    struct InflightGuard {
        ClientPluginInstance* inst;

        explicit InflightGuard(ClientPluginInstance* i) :
            inst(i) {
            if (inst) {
                inst->inflight.fetch_add(1, std::memory_order_acq_rel);
            }
        }

        ~InflightGuard() {
            if (inst) {
                inst->inflight.fetch_sub(1, std::memory_order_acq_rel);
            }
        }
    };
};

/// 事件订阅宿主句柄实现 (仅宿主内部; 与 plugin_api.h 的 C 不透明类型对应,
/// 命名避免与 agent 侧 PluginManager 的全局定义 ODR 冲突)
/// - sub 为强引用: 订阅对象从 subscriptions 摘除后仍被本句柄保活,
///   unload 回调内退订不会解引用已释放内存
struct ClientSubscriptionImpl {
    ClientPluginInstance*                               inst = nullptr;
    std::shared_ptr<ClientPluginInstance::Subscription> sub;
};

/// client 插件管理器 (全局唯一; 挂 client 端点侧)
///
/// 生命周期: load → (status/panel/command 生效) → disable/enable → unload
/// - 所有写操作须在 client io 线程 (与 agent 侧 PluginManager 同一无锁模型);
///   非 io 线程调用请自行 post (见 postToIo)
/// - 卸载顺序: 摘除全部注册 (adapter 通知) → 等 inflight==0 → 调 unload 回调
///   → dlclose
/// - UI 注册表: io 线程写, UI 线程经 uiRegistrySnapshot() 读 (COW, 短锁)
/// - 命令执行: 任意线程可 hasCommand/postCommandInvocation; execute 回调在
///   client io 线程同步调用, 返回值动作 JSON 由宿主解析并分发到 UI 适配器
class ClientPluginManager : public agentxx::agent::ClientEventSink,
                            public std::enable_shared_from_this<ClientPluginManager> {
public:

    struct PluginListView {
        std::string              name;
        std::string              version;
        std::string              description;
        std::string              path;
        bool                     enabled  = true;
        size_t                   inflight = 0;
        std::vector<std::string> statusItems;
        std::vector<std::string> panels;
        std::vector<std::string> infoSections;
        std::vector<std::string> commands;
        std::vector<std::string> depends;
        std::vector<std::string> optionalDepends;
        /// 接口声明 (plugin.yaml `interfaces`; 空 = 未声明)
        std::vector<std::string> requiredInterfaces;
        std::vector<std::string> optionalInterfaces;
    };

    explicit ClientPluginManager(asio::any_io_executor ex);
    ~ClientPluginManager();

    ClientPluginManager(const ClientPluginManager&)            = delete;
    ClientPluginManager& operator=(const ClientPluginManager&) = delete;

    // ==================== 装配 ====================

    /// 注入 UI 适配器 (UI 无关语义层 → 具体 UI 实现; 模式启动时调用一次)
    void setUiAdapter(std::shared_ptr<PluginUiAdapter> adapter);

    /// 当前 UI 适配器 (vtable 查询 uiCaps 用; 任意线程, 装配后不可变)
    std::shared_ptr<PluginUiAdapter> uiAdapter() const {
        return uiAdapter_;
    }

    /// 当前会话 sessionId (get_client_state 数据源; mode_runners 启动时注入,
    /// 会话切换时经 ClientEventSink::onSessionSwitched 自动更新)
    void setSessionId(std::string sessionId);

    // ==================== 生命周期 (须 client io 线程) ====================

    /// 加载 client 插件动态库 (io 线程协程; dlopen 卸载到内部线程池执行)
    /// - cfg: 插件配置 (yaml `plugins` 条目; 传 args 给插件, 不解析字段语义);
    ///   为 nullptr 时 args 为空对象 (测试/直连路径)
    /// - allowMissingEntry=true (sides==Auto): entry 符号缺失视为"纯 agent
    ///   插件"跳过 (info 日志, 不报错); false (sides==Client/直连): 缺失报错
    /// - 探测与加载合并为一次 dlopen (避免探测 dlopen→close 后正式加载再
    ///   dlopen 的重复加载/卸载)
    /// - 返回插件实例; 加载失败返回 nullptr (错误记日志)
    asio::awaitable<std::shared_ptr<ClientPluginInstance>> loadNativeAsync(
        std::string                         path,
        const agentxx::agent::PluginConfig* cfg               = nullptr,
        bool                                allowMissingEntry = false
    );

    /// 卸载插件 (按名称; 等全部在途回调完成后才 dlclose)
    asio::awaitable<bool> unloadAsync(std::string_view name);

    /// 禁用插件 (UI 项摘除/命令停用; 立即生效)
    /// - 级联: 必选依赖本插件的插件一同禁用 (依赖者先禁用)
    /// - 被级联禁用的插件不置 userDisabled (用户显式 enable 依赖方时可级联恢复)
    void disable(std::string_view name);

    /// 启用插件 (重新注册保存的 status item/panel/command/订阅)
    void enable(std::string_view name);

    /// 加载配置中应于 client 侧生效的插件 (yaml `plugins` 段, 经 sides 过滤):
    /// - sides == Client 或 sides == Auto: 尝试加载 (Auto 下无 client 入口则跳过)
    /// - sides == Agent: 跳过 (属于 agent 侧)
    /// - 按 manifest depends 拓扑排序加载 (依赖者排在被依赖者之后)
    asio::awaitable<void>
        loadConfiguredClientPlugins(const std::vector<agentxx::agent::PluginConfig>& plugins);

    /// 同步卸载全部插件 (进程退出路径; 不等在途回调, 调用方须保证无在途回调)
    void shutdownAll();

    // ==================== 查询 ====================

    std::vector<PluginListView>           list() const;
    std::shared_ptr<ClientPluginInstance> find(std::string_view name) const;

    /// 因接口要求未满足而被跳过的插件 (name → 缺失接口描述; io 线程;
    /// 加载阶段写入, 供展示层/排查 "为什么没加载" —— 跳过的插件不会出现在
    /// list() 中, 原因单独记录)
    const std::map<std::string, std::string>& skippedPlugins() const {
        return skippedPlugins_;
    }

    /// 宿主当前支持的接口名集合 (由 uiAdapter->uiCaps() 位图映射; io 线程;
    /// 门禁检查与 EVT_READY / get_client_state 的 interfaces 数组共用本结果)
    InterfaceSet hostSupportedInterfaces() const;

    // ==================== UI 注册表 (任意线程) ====================

    /// 注册表快照 (短锁拷贝 shared_ptr; UI 线程渲染无锁读取)
    std::shared_ptr<const ClientUiRegistry> uiRegistrySnapshot() const;

    /// 命令是否存在 (UI 线程判断是否拦截 "/" 输入; 短锁)
    bool hasCommand(std::string_view name) const;

    /// 投递命令调用到 client io 线程执行 (任意线程):
    /// - io 线程: 查表 → InflightGuard → execute 回调 → 解析动作 JSON →
    ///   分发到 UI 适配器 (send → adapter->sendPluginMessage; toast →
    ///   adapter->onToast; none/非法 → 记日志)
    void postCommandInvocation(std::string name, std::string argsJson);

    /// 同步执行命令 (仅 io 线程; CLI 模式输入循环直接调用, 等价于
    /// postCommandInvocation 的 io 线程路径)
    void invokeCommand(const std::string& name, const std::string& argsJson);

    // ==================== 会话上下文 (io 线程) ====================

    /// 当前 client 状态 JSON (get_client_state 数据源):
    /// {"sessionId","connState","startupProgress","uiCaps","agentPlugins"}
    /// - agentPlugins: 服务端已加载的 agent 侧插件名列表 (来自宿主约定事件
    ///   server_plugins / WireHelloAck.plugins); 空数组 = 未知 (旧版服务端
    ///   未提供), 插件不得据此断言"对端未加载"
    std::string clientStateJson() const;

    // ==================== io 线程投递 ====================

    bool isIoThread() const;
    void postToIo(std::function<void()> fn) const;

    // ==================== ClientEventSink 实现 (io 线程) ====================
    // 端点事件 → JSON payload → 分发到订阅了对应事件的插件回调

    void onReady() override;
    void onConnStateChanged(std::string_view state, std::string_view progress) override;
    void onUserInput(std::string_view sessionId, std::string_view text) override;
    void onDelta(const agentxx::agent::Delta& delta) override;
    void onTurnResult(const agentxx::agent::WireTurnResult& result) override;
    void onSessionSwitched(std::string_view sessionId) override;
    void onPluginData(const agentxx::agent::WirePluginData& data) override;

    // ==================== 内部 (host vtable 回调) ====================
    // 以下为 vtable 实现的强类型入口, 须 io 线程调用 (vtable 内部经 ioCallSync
    // 跨线程投递, 插件无感)

    /// 注册状态栏项; 返回宿主句柄 (nullptr = 宿主不支持或 id 冲突)
    void* registerStatusItem(
        ClientPluginInstance* inst,
        const char*           id,
        const char*           json,
        int                   align,
        int                   order
    );
    /// 更新状态栏项; 返回 0 成功
    int  updateStatusItem(ClientPluginInstance* inst, void* item, const char* json);
    void unregisterStatusItem(ClientPluginInstance* inst, void* item);
    /// 注册面板; 返回宿主句柄 (nullptr = 宿主不支持或 id 冲突)
    void* registerPanel(ClientPluginInstance* inst, const char* id, const char* props_json);
    /// 更新面板内容; 返回 0 成功
    int  updatePanel(ClientPluginInstance* inst, void* panel, const char* items_json);
    void unregisterPanel(ClientPluginInstance* inst, void* panel);
    /// 注册 Info 栏段落; 返回宿主句柄 (nullptr = 宿主不支持或 id 冲突)
    void* registerInfoSection(ClientPluginInstance* inst, const char* id, const char* props_json);
    /// 更新 Info 栏段落内容; 返回 0 成功
    int  updateInfoSection(ClientPluginInstance* inst, void* section, const char* items_json);
    void unregisterInfoSection(ClientPluginInstance* inst, void* section);
    /// 注册命令; 返回 0 成功 (名字冲突返回非 0)
    int registerCommand(
        ClientPluginInstance* inst,
        const char*           name,
        const char*           description,
        char* (*exec)(void*, AgentxxPluginStringView, char**),
        void* ud
    );
    int unregisterCommand(ClientPluginInstance* inst, const char* name);
    /// 事件订阅; 返回句柄 (宿主持有; 卸载自动退订)
    AgentxxSubscription* subscribe(
        ClientPluginInstance* inst,
        int                   event,
        void (*handler)(AgentxxPluginStringView, void*),
        void* ud
    );
    void unsubscribe(AgentxxSubscription* sub);
    /// 自描述
    std::string getOwnInfoJson(ClientPluginInstance* inst);
    std::string getPluginArgsJson(ClientPluginInstance* inst);
    /// 会话操作 (代理到端点)
    void sendUserInputToPeer(ClientPluginInstance* inst, const char* sessionId, const char* text);
    void requestCancelToPeer(ClientPluginInstance* inst, const char* sessionId);
    /// 跨端数据 (client → agent): 经端点 WirePluginDataUp 发送
    int sendPluginDataToPeer(ClientPluginInstance* inst, const char* event, const char* json);

    /// 宿主 vtable (静态函数表; 供 ClientPluginInstance::host 使用)
    static const AgentxxClientHostVtable* hostVtable();

private:

    friend class ClientPluginInstance;

    /// 等待插件在途计数归零 (io 线程协程轮询); 超时返回 false
    asio::awaitable<bool> waitInflightZero(
        const std::shared_ptr<ClientPluginInstance>& inst,
        std::chrono::milliseconds                    timeout
    );

    /// 插件卸载清理: 摘除注册/退订/adapter 通知 (io 线程)
    /// - keepInfo=true: disable 路径, 注册信息保留 (enable 可恢复)
    /// - keepInfo=false: unload/shutdown 路径, 彻底清理
    void detachAll(ClientPluginInstance* inst, bool keepInfo = true);

    /// 禁用/启用内部实现 (级联递归用; userInitiated=false 表示级联, 不改 userDisabled)
    void disableImpl(std::string_view name, bool userInitiated);
    void enableImpl(std::string_view name, bool userInitiated);

    /// 卸载单个插件 (shutdownAll 用; 先递归卸载必选依赖者, 再处理自己)
    /// - 依赖图级联 (先子后父): 脚本类插件 (depends 引擎) 先卸载, 引擎最后
    ///   dlclose, 与 agent 侧 shutdownPlugin 语义一致
    void shutdownClientPlugin(const std::shared_ptr<ClientPluginInstance>& inst);

    /// 收集反向必选依赖 (depends 含 target 的插件名; io 线程)
    std::vector<std::string> reverseRequiredDeps(const std::string& target, bool onlyEnabled) const;

    /// 事件分发: 遍历全部插件订阅, 匹配 event → InflightGuard → handler
    /// (io 线程; payload 为宿主构造的 JSON 字符串)
    void dispatchEvent(int event, const std::string& payloadJson);

    /// 命令动作解析与分发 (io 线程; execute 已返回):
    /// {"action":"send","text"} → adapter->sendPluginMessage
    /// {"action":"toast","text","level"} → adapter->onToast
    /// {"action":"none"}/{}/非法 → 记日志
    void dispatchCommandAction(const std::string& actionJson);

    /// 内部线程池 (dlopen/entry 卸载执行; shutdownAll 时 join)
    std::unique_ptr<asio::thread_pool> pool_;

    std::shared_ptr<PluginUiAdapter> uiAdapter_; ///< 注入后不可变 (io 线程读写)

    /// 插件表 <name, instance>
    std::map<std::string, std::shared_ptr<ClientPluginInstance>, std::less<>> plugins_{};

    /// UI 注册表 (COW: io 线程写, 任意线程快照读)
    mutable std::mutex                      uiMutex_;
    std::shared_ptr<const ClientUiRegistry> uiRegistry_;

    /// 会话上下文 (io 线程)
    std::string sessionId_ = "session";
    std::string connState_ = "connecting";
    std::string startupProgress_;

    /// 服务端已加载的 agent 侧插件名列表 (io 线程写读): 来自宿主约定事件
    /// `agentxx_host.server_plugins` (WirePluginData); 空数组 = 未知 (旧版
    /// 服务端未提供)。client 插件经 get_client_state("agentPlugins") 查询,
    /// 对端缺失时可降级提示, 避免上行数据被静默丢弃的"操作成功"假象
    std::vector<std::string> serverPlugins_;
    /// PLUGIN_DATA 无订阅者警告去重 (仅 io 线程; 每插件名只警告一次):
    /// 收到 WirePluginData 但无任何 client 插件订阅 EVT_PLUGIN_DATA 时,
    /// 多半是对端插件未在本地加载 —— 提示一次便于排查, 不随事件频率刷屏
    std::set<std::string> pluginDataNoSubWarned_;

    /// 因接口要求未满足被跳过的插件 (io 线程; 见 skippedPlugins())
    std::map<std::string, std::string> skippedPlugins_{};

    asio::any_io_executor ioExecutor_{};
    std::thread::id       ioThreadId_{};
};

/// UI 适配器抽象接口 (UI 无关语义层 → 具体 UI 实现)
///
/// 实现方 (TUI/CLI/未来 GUI) 职责:
/// - uiCaps(): 声明支持的 UI 能力位图 (宿主注册前检查)
/// - 各回调在 client io 线程调用, 实现必须快速返回; 涉及 UI 线程独占操作
///   (组件树修改/重绘) 须自行跨线程投递 (如 TUI 的 enqueueUiAction)
/// - 注册表数据 (text/items) 由 ClientPluginManager 持有, UI 渲染经
///   uiRegistrySnapshot() 读取; 本接口回调仅作"注册/更新/移除"信号
class PluginUiAdapter {
public:

    virtual ~PluginUiAdapter() = default;

    /// 声明支持的 UI 能力位图 (AGENTXX_UI_CAP_*)
    virtual uint32_t uiCaps() const = 0;

    /* ---- 信号回调 (client io 线程; 快速返回) ---- */

    /// 状态栏项注册/更新/移除 (props: {"text","tooltip"})
    virtual void onStatusItemRegistered(
        const std::string& /*id*/,
        const neograph::json& /*props*/,
        int /*align*/,
        int /*order*/
    ) {}

    virtual void onStatusItemUpdated(const std::string& /*id*/, const neograph::json& /*props*/) {}

    virtual void onStatusItemRemoved(const std::string& /*id*/) {}

    /// 面板注册/更新/移除 (props: {"title"}; items: {"items":[...]})
    virtual void onPanelRegistered(const std::string& /*id*/, const neograph::json& /*props*/) {}

    virtual void onPanelUpdated(const std::string& /*id*/, const neograph::json& /*items*/) {}

    virtual void onPanelRemoved(const std::string& /*id*/) {}

    /// Info 栏段落注册/更新/移除 (props: {"title"}; items: {"items":[...]})
    virtual void
        onInfoSectionRegistered(const std::string& /*id*/, const neograph::json& /*props*/) {}

    virtual void onInfoSectionUpdated(const std::string& /*id*/, const neograph::json& /*items*/) {}

    virtual void onInfoSectionRemoved(const std::string& /*id*/) {}

    /// toast 提示 (level: 0=info 1=warning 2=error)
    virtual void onToast(const std::string& /*text*/, int /*level*/) {}

    /// send 动作: 代发用户消息 (io 线程; 与用户输入同排队语义)
    virtual void sendPluginMessage(const std::string& /*text*/) {}

    /// 请求取消当前会话轮次 (io 线程; 与用户按 Esc 等价)
    virtual void requestCancel(const std::string& /*sessionId*/) {}

    /// 跨端数据: client → agent (io 线程; 经端点 WirePluginDataUp 发送)
    /// 返回 true 表示已发送 (未连接等失败返回 false)
    virtual bool sendPluginData(
        const std::string& /*plugin*/,
        const std::string& /*event*/,
        const std::string& /*json*/
    ) {
        return false;
    }
};

} // namespace plugin
} // namespace agentxx
