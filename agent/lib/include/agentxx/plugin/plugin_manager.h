#ifndef AGENTXX_PLUGIN_PLUGIN_MANAGER_H
#define AGENTXX_PLUGIN_PLUGIN_MANAGER_H

#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "agentxx/plugin/plugin_framework.h"
#include "agentxx/plugin/plugin_interfaces.h"
#include "agentxx/plugin/plugin_mount.h"
#include "agentxx/plugin/tool_registry.h"
#include "agentxx/tools/tool.h"
#include "asio/awaitable.hpp"
#include "asio/thread_pool.hpp"
#include "pluginxx/host/domain_hooks.h"
#include "pluginxx/host/host_core.h"
#include "pluginxx/host/manifest.h"
#include "utilxx_base/json.h"
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace agent {
class AgentContext;
}
namespace plugin {

class PluginInstance;
class PluginManager;
class PluginMount;
class PluginTool;
struct GraphTypeSlot;

/* ==================== 内置插件清单 (跨 TU 导出声明, 保持 C 链接) ==================== */
/// - 由 agent/plugins/builtin_plugins.cpp.in 编译时收集内置插件列表, 并添加实现函数
const PluginxxBuiltinInfo*     get_builtin_plugins(uint64_t* count);
const PluginxxBuiltinManifest* get_builtin_manifests(uint64_t* count);

/// 一个已加载的插件实例 (进程一份)
///
/// 本类只放**与 agent 无关**的东西: 加载元信息、插件上下文、生命周期入口。
/// 凡是落在某个 agent 上的东西 (工具/权限/钩子/提示词/功能点/图/资源/挂载状态)
/// 都在 [PluginMount::Entry] 里 —— 同一个实例可以同时挂在多个 agent 上, 各自一份。
class PluginInstance : public PluginInstanceBase {
public:

    /// 继承 PluginInstanceBase 的公共字段 (name/version/path/configPath/args/depends/
    /// dlHandle/pluginCtx/enabled/inflight 等), 见
    /// [pluginxx/runtime/instance_base.h](/agent/third_party/cxx_pluginxx/include/pluginxx/runtime/instance_base.h)
    /// 接口声明 (plugin.yaml `interfaces`; 加载时随 manifest 解析传入,
    /// 直连库路径为空) —— 经 list() 暴露供展示/排查
    PluginManifestInterfaces interfaces;

    /// 清单声明的资源 (plugin.yaml 的 skill/memory/mcp)
    /// - 由加载阶段记下来, 在**每次挂载**时应用到该 agent 的资源应用器 (每挂载一份)
    plugin::PluginManifestResources manifestResources;

    /// 已声明能力记录 (名称 / 启动回调 / 取消回调 / 上下文)
    /// - 类型本体在 cxx_pluginxx (能力表是通用表), 登记与撤销由主程序核心统一维护
    ///   (见 [pluginxx::PluginHostCore])
    using CapabilityRegistration = pluginxx::PluginCapabilityRegistration;

    /// 插件侧挂载入口 (加载时解析; 缺失 = 该插件无 agent 侧能力)
    AgentxxPluginAgentMountFn   mountFn   = nullptr;
    AgentxxPluginAgentUnmountFn unmountFn = nullptr;

    /// 本实例的全部挂载 (弱引用; 诊断与"这个插件挂在谁身上"用)
    std::vector<std::weak_ptr<PluginMount>> mounts;

    /// 加载总耗时 (毫秒; 由 PluginManager 记录)
    uint64_t loadMs = 0;

    // ---- 挂载记录的只读视图 (诊断 / 测试 / 清单用; 每次调用返回合并后的拷贝) ----
    //
    // 注册记录本身按挂载分开保存在 [PluginMount::Entry] 里 (同一实例挂到多个 agent
    // 时各一份); 这些入口把全部挂载的记录合并成一份, 只适合展示与断言, 不要用它做
    // 撤销判断。空 = 没有任何挂载记录。

    /// 是否至少有一个挂载成功了本实例
    bool anyMounted() const;
    /// 全部挂载上登记的工具名
    std::vector<std::string> allToolNames() const;
    /// 全部挂载上的工具对象
    std::vector<std::shared_ptr<PluginTool>> allToolObjects() const;
    /// 全部挂载上登记的钩子处理器句柄
    std::vector<int64_t> allHookHandles() const;
    /// 全部挂载上登记的图节点类型槽位
    std::vector<std::shared_ptr<GraphTypeSlot>> allGraphSlots() const;

    std::weak_ptr<PluginInstance> self{};
    std::weak_ptr<PluginManager>  manager{};

    explicit PluginInstance(std::string in_name) :
        PluginInstanceBase(std::move(in_name)) {}

    ~PluginInstance();

    /// 本端 destroy 入口符号名 (agent 侧)
    const char* pluginDestroySymbol() const noexcept override {
        return AGENTXX_PLUGIN_AGENT_SYMBOL_DESTROY;
    }
};

/// 插件工具 (本挂载上注册的一个工具)
class PluginTool : public agentxx::tools::XXToolBase {
public:

    PluginTool(
        std::weak_ptr<agentxx::agent::AgentContext> agentContext,
        std::shared_ptr<PluginInstance>             instance,
        AgentxxPluginToolSpec                       spec
    );

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const utilxx_base::Json& arguments) override;

    std::shared_ptr<PluginInstance> instance() const {
        return instance_.lock();
    }

    const AgentxxPluginToolSpec& spec() const {
        return spec_;
    }

private:

    std::string                   name_;
    std::string                   description_;
    std::string                   parametersJson_;
    AgentxxPluginToolSpec         spec_;
    utilxx_base::Json             parameters_;
    std::weak_ptr<PluginInstance> instance_;
};

/// 插件管理器 (agent 侧主程序, 进程一份)
///
/// - 通用部分 (log/json/config/plugins/events/scheduler/coroutine_runtime/tasks/
///   cancel/capabilities 十张通用表的状态与实现) 继承自
///   [pluginxx::PluginHostCore], 业务数据经 [pluginxx::DomainHooks] 提供;
/// - 加载/启停/禁用启用/卸载/级联依赖骨架继承自
///   [pluginxx::PluginHostLifecycle] (与主程序业务无关, 见 pluginxx/host/lifecycle.h);
/// - 本类只保留**与 agent 无关**的部分: 实例表、挂载表、插件侧接口表的 vtable 分发、
///   以及上面两层需要主程序数据的扩展点。落在某个 agent 上的注册与撤销全部在
///   [PluginMount] 里。
///
/// 因此 `unloadAsync` / `shutdownAsync` / `shutdownAll` / `disable` / `enable` /
/// `detachAll` / `detachInstanceRegistrations` / `clearPluginOwnedRegistrations` /
/// `stopForDisable` / `startForEnable` / `hasPendingClose` 都是继承来的。
class PluginManager : public pluginxx::PluginHostLifecycle<PluginInstance>,
                      public std::enable_shared_from_this<PluginManager>,
                      public pluginxx::DomainHooks {
public:

    /// 本主程序向插件提供的接口表数量 (文档里的数字有常量可校验)
    /// - 10 张通用表 (`pluginxx.*`, 由插件框架实现: log/json/config/plugins/events/
    ///   scheduler/coroutine_runtime/tasks/cancel/capabilities)
    /// - 11 张 agent 业务表 (`agentxx.agent.*`: tools/permission/hooks/session/
    ///   context/model/prompt/resources/graph/feature/mount)
    /// - 增删接口表时同时更新本常量与 `docs/zh-cn/design/plugins.md` §8 与根 `AGENTS.md`
    ///   (测试模块 `boundaries` 会校验三者一致)
    inline static constexpr size_t kInterfaceTableCount = 10 + 11;

    /// 清单行 (诊断与界面展示; 主程序视图)
    struct PluginListView {
        std::string              name;
        std::string              version;
        std::string              description;
        std::string              path;
        std::string              configPath;
        bool                     enabled  = true;
        size_t                   inflight = 0;
        std::vector<std::string> tools;
        std::vector<std::string> capabilities;
        std::vector<std::string> depends;
        std::vector<std::string> optionalDepends;
        std::vector<std::string> requiredInterfaces;
        std::vector<std::string> optionalInterfaces;

        // ---- 诊断字段 (加载快照) ----
        /// 是否被用户显式禁用 (区别于依赖级联禁用)
        bool userDisabled = false;
        /// 是否因必选依赖不可用被级联禁用
        bool blockedByDependencies = false;
        /// 加载总耗时 (dlopen + create + start + 注册收尾), 毫秒; 0 表示未记录
        uint64_t loadMs = 0;
        /// 钩子处理器数 (已注册且生效; 禁用/卸下后为 0)
        size_t hookCount = 0;
        /// 各钩子处理器的生效优先级 (按派发顺序; 与 [hookCount] 等长, 便于一眼看出顺序)
        std::vector<int32_t> hookPriorities;
        /// 自定义图节点类型登记数
        size_t graphNodeCount = 0;
        /// 事件订阅数
        size_t eventSubCount = 0;
        /// 已声明权限限制的工具数
        size_t permissionToolCount = 0;
        /// 本实例声明的功能点数 / 登记的实现数
        size_t featurePointCount = 0;
        size_t featureImplCount  = 0;
        /// 统一注册清单: 各项的合计 (0 = 已回到基线)
        size_t registrationTotal = 0;
        /// 其中的提示词键占用数
        size_t promptKeyCount = 0;
        /// 其中的 skill 目录 / memory 文件 / MCP 命名空间数 (资源应用器)
        size_t skillDirCount     = 0;
        size_t memoryFileCount   = 0;
        size_t mcpNamespaceCount = 0;
        /// 是否占用执行图定义 (独占 slot)
        bool ownsGraphDefinition = false;
        /// 该实例挂了哪些 agent (诊断: 一眼看出"装在进程里但没挂到本 agent")
        std::vector<std::string> mountedAgents;
        /// 当前已挂载的 agent 数 (= [mountedAgents] 的长度; 0 = 没有任何挂载)
        size_t mountCount = 0;
        /// 清单声明的资源条数 (skill 目录 + memory 文件 + MCP 服务)
        size_t manifestResourceCount = 0;
    };

    /// 建立插件管理器
    ///
    /// - `primaryAgent`: 主程序级配置的来源 (语言 / 模型 / 工作目录这类**没有 agent
    ///   参数**的接口取它的值)。多 agent 场景下由启动方传入根 agent;
    /// - 单个 agent 自建管理器时也用它 (见 `BaseAgent` 的 `plugin_system` 步骤)。
    explicit PluginManager(std::weak_ptr<agentxx::agent::AgentContext> primaryAgent);
    ~PluginManager();

    PluginManager(const PluginManager&)            = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    // =====================================================================
    // 挂载表 (一个 agent 一个挂载)
    // =====================================================================

    /// 全部挂载 (快照; 顺序 = 建立顺序)
    std::vector<std::shared_ptr<PluginMount>> mounts() const;

    /// 按 agentId 找挂载 (没有返回 nullptr)
    std::shared_ptr<PluginMount> findMountById(std::string_view agentId) const;

    /// 把插件给的引用解析成挂载
    /// - 引用为空 / 长度不足 / id 不匹配 / seq 不匹配 → nullptr
    /// - 解析失败时调用方转成 `AGENTXX_PLUGIN_ERR_NO_AGENT` (注册类) 或 `-1` (读取类)
    PluginMount* mountOf(const PluginxxScopeRef* ref) const;

    /// 为某个 agent 建立挂载 (幂等: 同一 agentId 已有挂载时复用并补挂新实例)
    ///
    /// 顺序: 逐实例 [PluginMount::mountOneAsync] —— 先应用清单资源, 再驱动插件
    /// 的 `mount` 回调。
    asio::awaitable<std::shared_ptr<PluginMount>>
        mountAgentAsync(std::shared_ptr<agentxx::agent::AgentContext> ctx);

    /// 同步建立一份挂载 (只建挂载记录, **不驱动**插件 `mount` 回调)
    ///
    /// - 用途: 单元测试、以及"没有插件 mount 回调、只经主程序侧接口登记"的自建场景;
    /// - 真实挂载一律走 [mountAgentAsync] (它会逐实例驱动插件的 `mount` 回调);
    /// - 同一 agentId 已存在挂载时直接返回已有挂载 (幂等)。
    std::shared_ptr<PluginMount> attachMountSync(std::shared_ptr<agentxx::agent::AgentContext> ctx);

    /// 拆掉某个 agent 的挂载 (调用方: agent 关闭 / 主程序关闭)
    asio::awaitable<void> unmountAgentAsync(std::string_view agentId);

    /// 拆掉全部挂载 (主程序关闭; 返回后实例表仍在, 由 shutdownAsync 收尾)
    asio::awaitable<void> unmountAllAgentsAsync();

    /// 挂载一个实例到指定挂载 (disable/enable 与运行期加载共用)
    asio::awaitable<bool> mountOneAsync(
        const std::shared_ptr<PluginMount>& mount,
        std::shared_ptr<PluginInstance>     inst
    );

    /// 从指定挂载拆掉一个实例
    asio::awaitable<void> unmountOneAsync(
        const std::shared_ptr<PluginMount>& mount,
        std::shared_ptr<PluginInstance>     inst
    );

    /// 单挂载场景的便捷入口 (测试 / 单 agent 程序 / 诊断)
    ///
    /// 多 agent 场景请直接用对应的 [PluginMount]: 这些入口把"本进程里所有挂载"
    /// 看成一份 —— 恰有一个挂载时等价于那个挂载, 多个挂载时按注释说明汇总或取第一个。
    /// 插件侧接口一律按引用精确落到单一挂载 (不会走这些入口)。
    PluginMount* soleMount() const;

    /// 单挂载便捷: 挂载标识 (没有挂载时返回空串)
    std::string agentId() const;
    /// 单挂载便捷: 挂载序号 (没有挂载时返回 0)
    uint64_t mountSequence() const;

    /// 主 agent 的工具注册表 (与 `primaryAgentContext()->toolRegistry` 同一份)
    std::shared_ptr<ToolRegistry> registry() const;

    /// 各挂载上该实例的生效注册汇总
    PluginMount::RegistrationInventory registrationInventory(const PluginInstance& inst) const;

    /// 执行图定义的占用者 (多挂载时取第一个非空占用者)
    std::string graphDefinitionOwner() const;

    /// 钩子处理器清单 (单挂载 = 该挂载; 多挂载 = 逐挂载一段的数组)
    std::string hooksJson();

    /// 某个点上的处理器 (多挂载时按挂载顺序拼接)
    std::vector<PluginMount::HookHandlerView> handlersOf(AgentxxPluginHookPoint point) const;

    /// 登记一个 core 层处理器 (单挂载时生效; 多挂载时投到第一个挂载并记警告)
    int64_t addCoreHookHandler(
        AgentxxPluginHookPoint point,
        int32_t                priority,
        std::string            module,
        AgentxxPluginHookSpec  spec
    );

    /// 撤销一个 core 层处理器 (逐挂载尝试, 命中即返回)
    bool removeCoreHookHandler(int64_t handle);

    void onTurnBegin();
    void onTurnEnd();
    void flushPendingCleanup();

    /// 功能点清单 (单挂载 = 该挂载; 多挂载 = 合并数组)
    std::string listFeaturePoints();

    /// 全部挂载的摘要 JSON 数组 (`agentxx.agent.mount.list`)
    std::string listMountsJson();

    /// 按 agentId 查一个挂载的摘要 (`agentxx.agent.mount.get`; 没有返回空串)
    std::string mountInfoJson(std::string_view agentId);

    // =====================================================================
    // 加载 (旧签名; 内部转成插件框架的 PluginLoadOptions 后交给主程序生命周期骨架)
    // =====================================================================
    //
    // 注意: 这里声明的是"主程序配置类型 → 插件框架加载参数"的适配层, 因此与骨架里
    // 同名但形参类型不同的加载入口是重载关系 (未加 using 引入, 骨架版本只在
    // 本类内部经限定名调用)。

    asio::awaitable<std::shared_ptr<PluginInstance>> loadNativeAsync(
        std::string                             path,
        const agentxx::agent::PluginConfig*     cfg                 = nullptr,
        bool                                    allowClientOnlySkip = false,
        const plugin::PluginManifestResources&  resources           = {},
        const plugin::PluginManifestInterfaces& interfaces          = {}
    );

    asio::awaitable<std::shared_ptr<PluginInstance>> loadBuiltinAsync(
        std::string                             name,
        std::string                             path,
        std::vector<std::string>                depends,
        std::vector<std::string>                optionalDepends,
        const agentxx::agent::PluginConfig*     cfg        = nullptr,
        const plugin::PluginManifestResources&  resources  = {},
        const plugin::PluginManifestInterfaces& interfaces = {}
    );

    asio::awaitable<std::shared_ptr<PluginInstance>> loadPluginAsync(
        std::string                         path,
        const agentxx::agent::PluginConfig* cfg                 = nullptr,
        bool                                allowClientOnlySkip = false
    );

    asio::awaitable<void>
        loadConfiguredPlugins(const std::vector<agentxx::agent::PluginConfig>& plugins);

    /// 实例清单 (主程序视图; 工具的 `tools` 字段取全部挂载的并集)
    std::vector<PluginListView> list() const;

    /// 某个挂载上的插件行 (只列挂在本 agent 上的实例)
    std::vector<PluginListView> listForMount(const PluginMount& mount) const;

    /// 加载失败时的"怎么改"提示
    ///
    /// 插件框架只报"哪一步失败了", 不解释"你该怎么改"。本函数按路径形态巡检常见的
    /// 加载失败原因, 返回一句可直接照做的建议 (没发现问题返回空串):
    /// - `path` 为空;
    /// - `builtin://<name>` 的内置清单不存在;
    /// - 路径不存在 (相对路径按进程当前工作目录解析);
    /// - 目录下没有 `plugin.yaml` 清单 / 清单 YAML 语法错误 / 清单 `entry`
    ///   指向的库文件不存在;
    /// - 库文件缺少主程序入口符号 (agent 侧 `agentxx_plugin_agent_{create,start,stop}`);
    /// - 只读检查 (不改变任何状态)。
    std::string diagnosePluginPath(std::string_view path) const;

    // =====================================================================
    // 主程序级查询 (没有 agent 参数的那些接口表项取主 agent)
    // =====================================================================

    std::string listPluginsJson();
    std::string getPluginJson(const std::string& name);

    std::string getConfigJson();
    std::string getToolPromptJson(const std::string& toolName);
    std::string getPluginArgsJson(PluginInstance* inst);
    std::string getPluginConfigPath(PluginInstance* inst);
    std::string getLanguage();
    /// 指定语言 (pluginxx::DomainHooks 要求; 同时服务 config 表的 set_language)
    void setLanguage(std::string_view lang) override;

    std::string getSessionWorkDir();
    std::string getSessionWorkDir(std::string_view sessionId);
    std::string getModelConfigJson();

    // ==================== pluginxx::DomainHooks 实现 ====================
    // 通用表 (log/json/config/plugins/events/scheduler/coroutine_runtime/tasks/cancel/
    // capabilities) 的实现位于 cxx_pluginxx, 需要主程序数据的入口经这些方法取数
    // (见 pluginxx/host/domain_hooks.h)。实现体在 plugin_manager_domain_hooks.cpp。

    /// 事件后端 (包装 agentxx::events::EventBus; 无总线时返回 nullptr)
    /// - 非空引用: 该挂载所属 agent 的总线 (会话/轮次事件)
    /// - 空引用: 主 agent 的总线 (进程级事件)
    std::shared_ptr<pluginxx::EventSource> eventSource(const PluginxxScopeRef* agent) override;
    /// 事件主题命名空间补齐 (不以 `plugin.`/`client.` 开头时补 `plugin.`)
    std::string qualifyEventTopic(std::string_view topic) override;
    /// 阻塞工作委托到工作线程池 (插件 offload: 阻塞工具 / 插件自己的耗时工作)
    /// - 返回 false 表示没有任何可用的线程池 (插件侧据此报 offload 失败)
    bool postToWorkerThread(std::function<void()> fn) override;

    /// 注入 offload 工作线程池 (**创建方注入, 不依赖主 agent 上下文**)
    /// - AgentHost 创建管理器后注入进程共享池; BaseAgent 自建管理器时注入
    ///   本 agent 的 [AgentContext::threadPool]
    /// - 未注入时依次退化: 主 agent 上下文的池 -> 管理器自建池 (见 [postToWorkerThread]);
    ///   这样 agent 上下文未挂载/已释放/其池已停止时, 插件阻塞工作仍能执行
    void setWorkerThreadPool(std::shared_ptr<asio::thread_pool> pool);

    std::string configJson() override;
    std::string toolPromptJson(std::string_view toolName) override;
    std::string sessionWorkDir(const PluginxxScopeRef* agent, std::string_view sessionId) override;
    std::string language() override;

    std::string pluginsJson() override;
    std::string pluginJson(std::string_view name) override;

    /// 会话是否已被取消 (cancel 表; 引用指定 agent)
    bool isSessionCancelled(const PluginxxScopeRef* agent, std::string_view sessionId) override;

    /// 主 agent 的上下文 (主程序级配置来源)
    std::shared_ptr<agentxx::agent::AgentContext> primaryAgentContext() const {
        return primaryAgent_.lock();
    }

protected:

    // =====================================================================
    // pluginxx::PluginHostLifecycle 的扩展点 (由本类覆写)
    // =====================================================================
    //
    // 加载/启停/禁用启用/卸载/级联依赖骨架在 cxx_pluginxx (见
    // pluginxx/host/lifecycle.h); 插件框架不认识"工具/权限/钩子/图/提示词/资源",
    // 因此这些业务动作经下列覆写注入。

    /// 自引用 (插件框架的异步事务与空闲收尾需要在此期间保活)
    std::shared_ptr<pluginxx::PluginHostLifecycle<PluginInstance>> selfRef() override {
        return shared_from_this();
    }

    /// 生成 agent 侧实例对象 (骨架随后补齐元信息/生命周期入口/主程序控制块)
    std::shared_ptr<PluginInstance> createInstance(std::string name) override;

    /// 交给插件的主程序 vtable (进程内稳定静态表; 定义在 plugin_manager_vtable.cpp)
    const PluginxxHostVtable* hostVtable() override;

    /// agent 侧插件入口符号名 (插件框架不硬编码主程序专名, 见 pluginxx/api/entry.h)
    ///
    /// 与插件侧导出宏 AGENTXX_PLUGIN_AGENT_EXPORT 生成的符号一致; `destroy` 不在此列
    /// (由 PluginInstance::pluginDestroySymbol 给出)。
    pluginxx::PluginEntrySymbols entrySymbols() const override {
        return {
            AGENTXX_PLUGIN_AGENT_SYMBOL_GET_INFO,
            AGENTXX_PLUGIN_AGENT_SYMBOL_CREATE,
            AGENTXX_PLUGIN_AGENT_SYMBOL_START,
            AGENTXX_PLUGIN_AGENT_SYMBOL_STOP,
        };
    }

    /// 摘除业务注册: 让本实例从**全部挂载**上摘掉 (禁用 / 卸载路径)
    void detachDomainRegistrations(PluginInstance* inst) override;

    /// 摘除实例专属资源所有权: 各挂载资源应用器上的启用标记
    void detachDomainOwnedResources(PluginInstance* inst) override;

    /// 清空由插件 start 事务重新声明的业务记录 (各挂载上的注册记录)
    void clearDomainRegistrations(PluginInstance* inst) override;

    /// 加载阶段记录清单声明的资源 (不立即应用: 每个挂载各应用一次)
    void applyDeclaredResources(
        PluginInstance&                        inst,
        const plugin::PluginManifestResources& resources
    ) override;

    /// 卸载时释放实例级资源: 各挂载上的工具对象 + 清单资源所有权
    void releaseInstanceResources(PluginInstance& inst) override;

    /// 实例加载完成: 解析插件侧的挂载/卸载入口
    /// - 动态库插件: 从已打开的句柄按名查找 (缺失 = 无 agent 侧能力)
    /// - 内置插件: 用框架下发的内置描述里的 `mount`/`unmount` 字段
    void onInstanceLoaded(PluginInstance& inst) override;

    /// 启用状态变化通知 (各挂载资源应用器的启用标记)
    void onInstanceEnabledChanged(PluginInstance& inst, bool enabled) override;

    /// 禁用事务完成后: 从全部挂载上拆掉该实例 (调插件 unmount, 释放 per-agent 状态)
    asio::awaitable<void>
        onInstanceDisabledAsync(const std::shared_ptr<PluginInstance>& inst) override;

    /// 启用事务完成后: 重新挂到全部挂载 (agent 级注册都在插件的 mount 回调里)
    asio::awaitable<void>
        onInstanceEnabledAsync(const std::shared_ptr<PluginInstance>& inst) override;

private:

    friend class PluginInstance;
    friend class PluginMount;

    /// 引用对应的 agent 上下文 (空引用 = 主 agent)
    /// - 引用无效 (未知 id / 过期序号) 返回空: 调用方按"拒绝"处理
    /// - 会话/事件这类**落在具体 agent 上**的查询必须用它, 否则多 agent 下会串到主 agent
    std::shared_ptr<agentxx::agent::AgentContext>
        agentContextOf(const PluginxxScopeRef* agent) const;

    /// 主 agent 上下文 (主程序级配置来源)
    std::weak_ptr<agentxx::agent::AgentContext> primaryAgent_;

    /// offload 工作线程池 (由创建方注入; 空时按需创建私有池)
    std::shared_ptr<asio::thread_pool> workerPool_;

    /// 保证 [workerPool_] 可用 (未注入/原有池不可用时的兜底; 只创建一次)
    void ensureWorkerPool();

    /// 全部挂载 (强引用: 由本管理器保活; agent 关闭时显式拆)
    std::vector<std::shared_ptr<PluginMount>> mounts_;

    /// 挂载序号分配器 (每次成功建立挂载 +1)
    uint64_t nextMountSeq_ = 1;
};

} // namespace plugin
} // namespace agentxx

#endif // AGENTXX_PLUGIN_PLUGIN_MANAGER_H
