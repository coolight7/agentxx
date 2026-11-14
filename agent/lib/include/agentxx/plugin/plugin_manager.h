#ifndef AGENTXX_PLUGIN_PLUGIN_MANAGER_H
#define AGENTXX_PLUGIN_PLUGIN_MANAGER_H

#include "agentxx/agent/async_work.h"
#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "agentxx/plugin/net_service.h"
#include "agentxx/plugin/plugin_framework.h"
#include "agentxx/plugin/plugin_interfaces.h"
#include "agentxx/plugin/plugin_mount.h"
#include "agentxx/plugin/runtime_state.h"
#include "agentxx/plugin/tool_registry.h"
#include "agentxx/tools/tool.h"
#include "asio/awaitable.hpp"
#include "asio/thread_pool.hpp"
#include "pluginxx/host/domain_services.h"
#include "pluginxx/host/host_core.h"
#include "pluginxx/host/manifest.h"
#include "utilxx_base/json.h"
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace agent {
class AgentContext;
class AgentHost;
class BaseAgent;
} // namespace agent
namespace events {
class EventBus;
} // namespace events
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
/// 凡是落在某个 agent 上的东西 (工具/权限/提示词/功能埋点/图/资源/事件订阅/挂载状态)
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

    asio::awaitable<agentxx::util::ToolResult> execute_async(const utilxx_base::Json& arguments) override;

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
/// - 通用部分 (broadcasts/capabilities/scheduler/coroutine_runtime/tasks/cancel/
///   plugins/config/json/log 十张通用表的状态与实现) 继承自
///   [pluginxx::PluginHostCore], 主程序数据经 [pluginxx::DomainServices] 提供;
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
                      public pluginxx::DomainServices {
public:

    /// 本主程序向插件提供的接口表数量 (文档里的数字有常量可校验)
    /// - 10 张通用表 (由插件框架实现: log/json/config/plugins/broadcasts/
    ///   scheduler/coroutine_runtime/tasks/cancel/capabilities)
    /// - 10 张 agent 业务表 (`agentxx.agent.*`: tools/permission/session/
    ///   context/model/prompt/resources/graph/mount/runtime)
    /// - `net` 由本主程序实现, 单独一张 (`agentxx.net`), 不计入上面两组
    /// - 增删接口表时同时更新本常量与 `docs/zh-cn/design/plugins.md` §8 与根 `AGENTS.md`
    ///   (测试模块 `boundaries` 会校验三者一致)
    inline static constexpr size_t kInterfaceTableCount = 10 + 10;

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
        /// 自定义图节点类型登记数
        size_t graphNodeCount = 0;
        /// 事件订阅数
        size_t eventSubCount = 0;
        /// 已声明权限限制的工具数
        size_t permissionToolCount = 0;
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
    /// - 解析失败时调用方转成 `AGENTXX_RESULT_ERR_NO_AGENT` (注册类) 或 `-1` (读取类)
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

    /// 通知型功能埋点清单 (单挂载 = 该挂载; 多挂载 = 逐挂载一段的数组)
    std::string hooksJson();

    void onTurnBegin();
    void onTurnEnd();
    void flushPendingCleanup();

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
    /// 加载失败原因, 返回一句可直接照做的建议:
    /// - `path` 为空;
    /// - `builtin://<name>` 的内置清单不存在;
    /// - 路径不存在 (相对路径按进程当前工作目录解析);
    /// - 目录下没有 `plugin.yaml` 清单 / 清单 YAML 语法错误 / 清单 `entry`
    ///   指向的库文件不存在;
    /// - 库文件缺少主程序入口符号 (agent 侧 `agentxx_plugin_agent_{create,start,stop}`);
    /// - 以上都没问题时返回一句“若加载仍失败多半是 API 代次不匹配, 需按当前 API 版本
    ///   重新编译插件” (代次判断只能靠 `get_info`, 不在本函数的静态检查范围);
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
    /// 指定语言 (pluginxx::DomainServices 要求; 同时服务 config 表的 set_language)
    void setLanguage(std::string_view lang) override;

    std::string getModelConfigJson();

    // ==================== pluginxx::DomainServices 实现 ====================
    // 通用表 (log/json/config/plugins/broadcasts/scheduler/coroutine_runtime/tasks/
    // cancel/capabilities) 的实现位于 cxx_pluginxx, 需要主程序数据的入口经这些方法取数
    // (见 pluginxx/host/domain_services.h)。实现体在 plugin_manager_domain_services.cpp。

    /// 广播主题**不做任何补齐** (基类默认就是原样返回)
    /// - 命名约定统一为全名: `agentxx.agent.*` / `agentxx.client.*` / `plugin.<插件名>.*`
    /// - 写侧校验在 [publish] 与业务表入口, 这里不参与
    std::string qualifyEventTopic(std::string_view topic) override;

    // ==================== 广播 ====================
    //
    // 插件面的发布 / 订阅 / 读当前值 / 结束都走框架的 `<应用名>.broadcasts` 表
    // (由 PluginHostCore 实现); 这里是主程序自用的几个入口。

    /// 发布一条广播 (主程序代发; 同时转给中枢, 让等这个事件的人能收到)
    ///
    /// - 频道未声明时自动补一条 (`cache: latest` + `publish: owner`);
    /// - 失败只记日志 (广播是通知面, 不能让业务流程因它失败)。
    void publishBroadcastValue(std::string_view id, std::string_view payloadJson);

    /// 挂上 agent 事件总线: 总线上的应用话题镜像到同名广播频道 (**幂等**)
    ///
    /// 插件只能经 `<应用名>.broadcasts` 表订阅, 而 `agentxx.agent.*` / `agentxx.client.*`
    /// 这类应用话题由主程序发在事件总线上, 中间需要这一跳。缺了它
    /// `agentxx.agent.host.client_attached` 一类约定事件送不到插件
    /// (双端插件靠它重发状态快照, 见 `agentxx/event/events.h` 的 `Topic`),
    /// 客户端上行的 `agentxx.client.plugin.<插件名>.*` 也收不到。
    ///
    /// - 只镜像**已经声明**为广播频道的名字 (插件订阅时会按需声明), 其余一次查找就跳过;
    /// - 只接受 `std::string` 载荷 (约定事件与插件数据的 JSON 原文), 强类型事件不参与;
    /// - 与插件发布方向 (`plugin.*`) 前缀不重叠, 不会形成环。
    void attachEventBus(const std::shared_ptr<agentxx::events::EventBus>& bus);

    /// 确保频道已声明 (幂等; 已声明时保留原声明)
    void ensureBroadcast(
        std::string_view    id,
        pluginxx::BroadcastCache_e cache          = pluginxx::BroadcastCache_e::Latest,
        bool                anyonePublish  = false,
        bool                pushOnSubscribe = false
    );

    /// 声明内置广播频道 (构造时调一次)
    ///
    /// 插件在加载阶段 (start) 就会订阅这些频道, 那时事件中枢还没建 (中枢的按需声明
    /// 来不及), 因此由管理器先声明好。幂等。
    void declareBuiltinBroadcasts();

    /// 从频道 id 里取插件名 (`plugin.<插件名>.rest` → `<插件名>`; 其他形式返回空)
    static std::string pluginNameOfBroadcast(std::string_view id);

    // =====================================================================
    // 功能埋点 (point): 一个 id = 能力 (实现面) + 同名广播频道 (通知面)
    // =====================================================================

    /// 声明本主程序的全部功能埋点 (构造函数里调一次; 幂等)
    ///
    /// 清单在 `agentxx/plugin/api/agentxx_point_ids.h`, 与这里的声明表必须一致
    /// (测试模块 `boundaries` 核对)。
    void declareAppPoints();

    /// 声明本主程序的应用能力 (构造函数里调一次; 幂等)
    ///
    /// 清单在 `agentxx/plugin/api/agentxx_capability_ids.h`。插件框架要求 `agentxx.*`
    /// 这类官方 id 必须由应用先声明, 插件才能登记实现行; 声明形状是
    /// `impls: many` + `merge: first` + 只开放 impl 阶段 (第一个给出结果的实现生效)。
    void declareAppCapabilities();

    /// 发一次功能埋点 (**IO 线程**)
    /// - 先发通知面广播, 再按 `agentId` 调实现行; 没有实现行时按 `OK` + `null` 完成;
    /// - `agentId` 为空 = 不按作用域过滤 (只有通知面);
    /// - 完成回调 `cb` 可为空 (通知型调用点不需要结果)。
    PluginxxOperatorHandle* emitPointAsync(
        std::string_view         pointId,
        std::string_view         agentId,
        std::string_view         payloadJson,
        PluginxxOperatorCallback cb,
        void*                    ud,
        PluginxxString*          error
    );

    /// 发一次通知型功能埋点 (不等结果, 失败只记日志)
    void publishPoint(
        std::string_view pointId,
        std::string_view agentId,
        std::string_view payloadJson
    );

    /// 发一次功能埋点并等实现行的结果 (IO 线程)
    ///
    /// - 实现行 = 框架能力表里的行 (`register_capability`, 按 `scope` 过滤, 按优先级排);
    /// - 返回 `std::nullopt` = 没人给出结果 (没实现行 / 全部回了空 / 失败);
    /// - 返回值是第一个给出非空结果的行那价 JSON (框架已经做好排序与合并)。
    asio::awaitable<std::optional<std::string>> emitPoint(
        std::string_view agentId,
        std::string_view pointId,
        std::string_view payloadJson
    );

    // =====================================================================
    // 网络能力 (`<应用名>.net` 表; 实现在通用网络服务 utilxx::NetHub)
    // =====================================================================

    /// 网络服务 (懒创建; 首个网络调用时起网络线程)
    std::shared_ptr<PluginNetService> netService();

    /// 同步网络操作 (返回插件面结果码 `PLUGINXX_RESULT_*` 通用段)
    int32_t netCall(
        std::string_view              owner,
        std::string_view              op,
        std::string_view              argsJson,
        std::string*                  outJson,
        std::weak_ptr<PluginInstance> guard = {}
    );

    /// 异步网络操作 (受理成功写 requestId, 结果恰好一次经 cb 给出)
    int32_t netCallAsync(
        std::string_view              owner,
        std::string_view              op,
        std::string_view              argsJson,
        PluginxxOperatorCallback      cb,
        void*                         ud,
        std::string*                  outJson,
        int64_t*                      outRequestId,
        std::weak_ptr<PluginInstance> guard = {}
    );

    /// 取消一个进行中的网络请求
    int32_t netCancel(std::string_view owner, int64_t requestId, std::string* outJson);

    /// 收掉某个插件的全部网络资源 (禁用/卸载时调; 未完成请求按"已取消"结算)
    void netShutdownPlugin(std::string_view owner);

    /// 停掉网络服务 (管理器关闭时)
    void netShutdownAll();

    /// 把一个函数投递到本管理器的 io 线程 (没有 io 线程时就地执行)
    void postToPluginIo(std::function<void()> fn);

    /// 把一条网络事件发给插件 (频道 `plugin.<插件 id>.net`; 必须在本管理器 io 线程调)
    void publishNetEvent(std::string_view owner, std::string_view payloadJson);

    /// 频道未声明时按名字补一条声明 (幂等)
    ///
    /// - `plugin.<插件名>.*`: 属主就是名字里的插件 (它才能发布);
    /// - 其他名字: 按主程序频道声明, 任何人可发 (跨端频道由另一端声明)。
    ///
    /// 用在“运行时替插件接受一条主题”的地方 (`register_trigger` / `runtime_emit_event`),
    /// 免得插件发布自己的频道时被“属主不是它”拒掉。
    void ensureTopicBroadcast(std::string_view id);

    /// 结束一条广播 (属主/应用可结束; 失败只记日志)
    void endBroadcastValue(std::string_view id, std::string_view reasonJson);

    /// 插件发布广播的入口 (表入口的落点;**遮蔽基类同名方法**以多一步转给中枢)
    int publishBroadcastFrom(
        PluginInstance*  inst,
        std::string_view broadcastId,
        std::string_view payloadJson,
        std::string*     error
    );

    /// 插件订阅广播的入口 (表入口的落点; 遮蔽基类同名方法)
    ///
    /// 只多做一件事: 应用命名空间 (`agentxx.*`) 的频道本端还没人声明时先补一条
    /// (跨端频道由另一端声明, 本进程看不到) —— 否则这种订阅会被拒。
    /// `plugin.*` 频道不代劳: 声明属主是插件自己。
    PluginxxSubscription* subscribeBroadcast(
        PluginInstance*          inst,
        const PluginxxScopeRef*  scope,
        std::string_view         broadcastId,
        void(PLUGINXX_CALL* handler)(const PluginxxStringView* payloadJson, void* ud),
        void*                    ud,
        std::string_view         optionsJson  = {},
        void(PLUGINXX_CALL* onEnd)(const PluginxxStringView* reasonJson, void* ud) = nullptr
    );
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
    std::string language() override;

    std::string pluginsJson() override;
    std::string pluginJson(std::string_view name) override;

    // ==================== agentxx 业务表的数据来源 ====================
    // 会话工作目录、取消查询、工具提示词属主程序业务, 由 agentxx 自己的接口表
    // (agentxx.agent.session / agentxx.agent.prompt) 提供, 不再进通用表。

    /// 指定 agent 的会话工作目录 (会话有 worktree 绑定时优先; 未加载返回空串)
    std::string sessionWorkDir(const PluginxxScopeRef* agent, std::string_view sessionId);
    /// 指定 agent 的会话是否已取消 (advisory 查询; 无会话/无取消令牌返回 false)
    bool isSessionCanceled(const PluginxxScopeRef* agent, std::string_view sessionId);
    /// 工具提示词配置 JSON (`{"depict": ..., "args": {...}}`; 未配置返回空串)
    std::string toolPromptJson(std::string_view toolName);

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
    // pluginxx/host/lifecycle.h); 插件框架不认识"工具/权限/图/提示词/资源",
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

    /// 禁用事务开始后: 从全部挂载上拆掉该实例 (调插件 unmount, 释放 per-agent 状态)
    asio::awaitable<void>
        onInstanceDisabledAsync(const std::shared_ptr<PluginInstance>& inst) override;

    /// 卸载事务开始时: 同样从全部挂载上拆掉该实例
    /// - 在插件 stop 与 `releaseInstanceResources` 之前, 插件自管的根此时还在,
    ///   实例也还没进入 Closed, 插件 unmount 回调能正常执行
    asio::awaitable<void>
        onInstanceUnloadingAsync(const std::shared_ptr<PluginInstance>& inst) override;

    /// 启用事务完成后: 重新挂到全部挂载 (agent 级注册都在插件的 mount 回调里)
    asio::awaitable<void>
        onInstanceEnabledAsync(const std::shared_ptr<PluginInstance>& inst) override;

private:

    /// 把实例从**全部挂载**上拆下来 (禁用 / 卸载共用)
    asio::awaitable<void>
        detachInstanceFromMountsAsync(const std::shared_ptr<PluginInstance>& inst);

    // 运行时入口由 vtable 层调用 (plugin_manager_vtable.cpp 里的 xx_runtime_*):
    // 它们是自由函数, 因此这一段开放为 public, 但只对本文件所在库有意义 ——
    // 插件拿不到 PluginManager 类型 (跨边界只有 C ABI)。
    friend class PluginInstance;
    friend class PluginMount;

public:

    // =====================================================================
    // 会话运行时 (agentxx.agent.runtime): 根 / 通道 / 触发器 / 定时器
    // =====================================================================
    //
    // 全部入口都在主程序 IO 线程执行 (vtable 层经 ioCallSyncKeep 投递), 因此
    // 下面的记录不需要锁。插件禁用/卸载时按 owner 整批回收。
    //
    // 取容器的方式: 经主 agent 上下文里的 `host` 弱引用拿到 AgentHost (启动方
    // 建容器时写入)。拿不到就是"本进程没有容器", 入口返回 ERR_UNSUPPORTED。

    /// 本进程的 agent 容器 (没有则空)
    std::shared_ptr<agent::AgentHost> runtimeHost() const;

    // ---------------- 根 ----------------

    /// 挂载一个带自己配置的根 (异步 init; 返回即拿到 id)
    int32_t runtimeMountRoot(
        PluginInstance*                   inst,
        const AgentxxPluginMountRootSpec* spec,
        std::string&                      outRootId,
        std::string&                      err
    );

    /// 卸载根 (连带关闭其通道)
    int32_t runtimeUnmountRoot(PluginInstance* inst, std::string_view rootId);

    /// 把手里的根 id 解析成记录 (不存在或不是本实例的返回空)
    RuntimeRoot* runtimeRootOf(const PluginInstance* inst, std::string_view rootId);

    /// 建一个会话通道的记录 (尚未启动)
    int32_t runtimeOpenChannel(
        PluginInstance*                 inst,
        const AgentxxPluginChannelSpec* spec,
        int64_t*                        outChannelId,
        std::string&                    err
    );

    /// 关闭通道 (不等待收尾)
    int32_t runtimeCloseChannel(PluginInstance* inst, int64_t channelId);

    RuntimeChannel* runtimeChannelOf(const PluginInstance* inst, int64_t channelId);

    /// 在容器上卸载根并释放记录 (根 init 失败与 stop 共用)
    void runtimeDropRoot(RuntimeRoot& root, bool unmountFromHost);

    /// 关闭一个通道: 先通知端点, 再停驱动壳, 最后释放记录里的对象
    void runtimeCloseChannelNow(RuntimeChannel& channel, std::string_view reason);

    /// 根就绪后把等待中的通道全部启动 (失败时给它们发错误事件)
    void runtimeFlushWaitingChannels(RuntimeRoot& root);

    /// 启动一个通道 (建驱动壳 + 登记会话占用; 根未就绪时先排队)
    /// - `return` 0 成功 (已启动或已排队)
    int32_t runtimeStartChannel(RuntimeRoot& root, RuntimeChannel& channel, std::string& err);

    /// 派生根配置 (`config_json` + `mount_plugins`)
    std::shared_ptr<agentxx::agent::AgentConfig> deriveRootConfig(
        const AgentxxPluginMountRootSpec& spec,
        const std::string&                rootId,
        const PluginInstance&             inst,
        std::string&                      err
    );

    /// 按 owner 回收全部运行时对象 (禁用/卸载/关闭时调用; 幂等)
    void releaseRuntimeOfOwner(std::string_view owner);

    /// 回收指定实例建立的通道与触发器/定时器 (根按实例名称回收)
    void releaseRuntimeChannelsOf(PluginInstance* inst);

    /// 运行时清单 JSON (诊断/`list_runtime`)
    std::string listRuntimeJson(PluginInstance* inst) const;

    /// 通道状态 JSON (诊断/`get_channel_state`)
    std::string runtimeChannelStateJson(PluginInstance* inst, int64_t channelId) const;

    /// 通道会话的 LLM 上下文 (JSON 数组文本)
    std::string channelContextMessages(PluginInstance* inst, int64_t channelId) const;

    /// 投递一条输入 (输入 JSON 形状同 `send_input`)
    int32_t runtimeSendInput(
        PluginInstance*  inst,
        int64_t          channelId,
        std::string_view inputJson,
        std::string&     err
    );

    /// 取消通道当前轮次
    int32_t runtimeCancel(PluginInstance* inst, int64_t channelId);

    // ---------------- 后台任务 (async work) ----------------

    /// 提交结果 (入口需要同时给出错误码、任务 id 与可读原因)
    struct AsyncSubmitOutcome {
        int32_t     code = AGENTXX_RESULT_OK;
        std::string asyncId;
        std::string error;
    };

    /// 提交一个后台任务 (归属记为 `plugin:<实例名>`)
    AsyncSubmitOutcome
        runtimeSubmitAsyncWork(PluginInstance* inst, agent::AsyncWorkSpec work);

    /// 取消一条后台任务 (只能取消本实例提交的)
    int32_t runtimeCancelAsyncWork(PluginInstance* inst, std::string_view asyncId);

    /// 单条任务 JSON (只能看本实例提交的; 不存在返回 ERR_NOT_FOUND)
    std::string runtimeGetAsyncWork(PluginInstance* inst, std::string_view asyncId);

    /// 任务清单 JSON (`sessionId` 空 = 本实例提交的全部)
    std::string runtimeListAsyncWork(PluginInstance* inst, std::string_view sessionId);

    /// 切换通道的模型
    int32_t runtimeSelectModel(PluginInstance* inst, int64_t channelId, std::string_view model);

    /// 应答一个中断
    int32_t runtimeRespondInterrupt(
        PluginInstance*  inst,
        int64_t          channelId,
        int64_t          interruptId,
        std::string_view valuesJson,
        std::string&     err
    );

    // 触发器 / 定时器 (阶段四)
    // ----------------
    int32_t runtimeRegisterTrigger(
        PluginInstance*                 inst,
        const AgentxxPluginTriggerSpec* spec,
        int64_t*                        outTriggerId,
        std::string&                    err
    );
    int32_t runtimeUnregisterTrigger(PluginInstance* inst, int64_t triggerId);
    int32_t runtimeEmitEvent(PluginInstance* inst, std::string_view topic, std::string_view payload);

    /// 一条事件到达 (触发器订阅回调; 在主程序事件线程 = IO 线程)
    void onTriggerEvent(int64_t triggerId, std::string_view payload);

    /// 合并窗口到期 / 背压触发: 把缓冲拼成一条投出去
    void flushTrigger(int64_t triggerId);

    int32_t runtimeRegisterTimer(
        PluginInstance*               inst,
        const AgentxxPluginTimerSpec* spec,
        int64_t*                      outTimerId,
        std::string&                  err
    );
    int32_t runtimeUnregisterTimer(PluginInstance* inst, int64_t timerId);

    /// 给定时器排下一次触发 (delay 到期后调 [fireTimer])
    void scheduleTimer(int64_t timerId, std::chrono::milliseconds delay);

    /// 定时器到期: 按策略投递, 周期性的再排下一次
    void fireTimer(int64_t timerId);

    /// 把一个通道事件交给插件 (填 C 结构并调用回调; 通道已关时丢弃)
    void deliverChannelEvent(
        const RuntimeChannel& channel,
        int32_t               kind,
        int64_t               interruptId,
        std::string_view      text,
        const utilxx_base::Json* payload
    );

    /// 本实例建立的根 / 通道 / 触发器 / 定时器 (只在 IO 线程读写)
    std::vector<std::shared_ptr<RuntimeRoot>>    runtimeRoots_;
    std::vector<std::shared_ptr<RuntimeChannel>> runtimeChannels_;
    std::vector<std::shared_ptr<RuntimeTrigger>> runtimeTriggers_;
    std::vector<std::shared_ptr<RuntimeTimer>>   runtimeTimers_;
    /// 运行时句柄自增序号 (根用 agentId, 其余用数字句柄; 不复用已分配值)
    int64_t nextRuntimeHandle_ = 1;

    /// 本管理器是否正在驱动插件 `mount` 回调 (期间挂根会被拒)
    int mountingDepth_ = 0;

private:

    /// 引用对应的 agent 上下文 (空引用 = 主 agent)
    /// - 引用无效 (未知 id / 过期序号) 返回空: 调用方按"拒绝"处理
    /// - 会话/事件这类**落在具体 agent 上**的查询必须用它, 否则多 agent 下会串到主 agent
    std::shared_ptr<agentxx::agent::AgentContext>
        agentContextOf(const PluginxxScopeRef* agent) const;

    /// 主 agent 上下文 (主程序级配置来源)
    std::weak_ptr<agentxx::agent::AgentContext> primaryAgent_;

    /// 一条已挂上镜像的总线 (总线通常与 agent 同生命周期, 这里只弱引用)
    struct BusBridge {
        std::weak_ptr<agentxx::events::EventBus> bus;
        /// 总线前缀订阅 id (`agentxx.agent.` / `agentxx.client.`)
        size_t agentPrefixId  = 0;
        size_t clientPrefixId = 0;
    };

    /// 已挂上镜像的总线; 访问前取 [busBridgeMutex_]
    std::vector<BusBridge> busBridges_;
    std::mutex             busBridgeMutex_;

    /// 总线话题 → 广播频道 ([attachEventBus] 前缀订阅的落点)
    void mirrorBusValue(std::string_view topic, std::string_view payloadJson);

    /// 撤掉全部总线镜像订阅 (析构时调; 总线已销毁时是空操作)
    void detachEventBuses();

    /// 插件发布 → 各 agent 总线 (服务端端点据此转发给客户端)
    void forwardBroadcastToBuses(std::string_view id, std::string_view payloadJson);

    /// 异步投递一条值到一条总线 (`bus` 为空时跳过)
    static void publishOnBus(
        const std::shared_ptr<agentxx::events::EventBus>& bus,
        std::string_view                                  topic,
        std::string_view                                  payloadJson
    );

    /// offload 工作线程池 (由创建方注入; 空时按需创建私有池)
    std::shared_ptr<asio::thread_pool> workerPool_;

    /// 网络服务 (懒创建; 与 [netMutex_] 一起使用)
    std::shared_ptr<PluginNetService> netService_;
    std::mutex                        netMutex_;

    /// 保证 [workerPool_] 可用 (未注入/原有池不可用时的备用池; 只创建一次)
    void ensureWorkerPool();

    /// 全部挂载 (强引用: 由本管理器保活; agent 关闭时显式拆)
    std::vector<std::shared_ptr<PluginMount>> mounts_;

    /// 挂载序号分配器 (每次成功建立挂载 +1)
    uint64_t nextMountSeq_ = 1;
};

} // namespace plugin
} // namespace agentxx

#endif // AGENTXX_PLUGIN_PLUGIN_MANAGER_H
