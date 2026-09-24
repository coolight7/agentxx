#pragma once

#include "agentxx/agent/config.h"
#include "agentxx/agent/io/client_event_sink.h"
#include "agentxx/plugin/api/client_plugin_api.h"
// 宿主侧 vtable/管理器实现使用 SDK 提供的跨边界字符串工具 (PluginStringView /
// PluginString); 该头后续会拆为 pluginxx/kit/kit.h (通用部分) + 本头的领域 helper
#include "agentxx/plugin/api/plugin_kit.h"
#include "agentxx/plugin/plugin_framework.h"
#include "agentxx/plugin/plugin_interfaces.h"

#include "asio/any_io_executor.hpp"
#include "asio/awaitable.hpp"
#include "asio/thread_pool.hpp"
#include "utilxx_base/json.h"
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace agentxx {
namespace plugin {

class ClientPluginManager;
class PluginUiAdapter;
class ClientPluginInstance;

/// 自定义 renderer 的宿主控制块。UI 线程可能还持有旧 COW 快照；关闭时先
/// 失效控制块，旧快照只能回退，不能再跳入已经卸载的插件代码。
struct ClientToolRendererLease {
    std::weak_ptr<ClientPluginInstance> instance;
    std::atomic<bool>                   alive{true};
};

/// 状态栏项注册记录 (UI 注册表快照条目; 所有字段为宿主拷贝, 可跨线程读取)
struct ClientStatusItem {
    std::string plugin;    ///< 所属插件名
    std::string id;        ///< 全局唯一 id
    std::string text;      ///< 当前文本 (纯文本形态; 渲染至少显示它)
    int         align = 0; ///< 0=左侧 1=右侧
    int         order = 0; ///< 组内排序 (小在前)
    /// 富展示描述 (可选): `{"segments":[{text,color}],"sparkline":{...},"meter":{...}}`
    ///
    /// 状态栏只有一行高度, 因此这些片段按"单行组件"渲染 (迷你趋势图高度强制为 1,
    /// 计量条取一行); `text` 作为无法渲染富内容时的降级文本。空对象 = 只用 `text`。
    utilxx_base::Json rich = utilxx_base::Json::object();
    /// 内容版本号 (每次 `update_status_item` 递增; 供缓存 key/诊断使用)
    uint64_t version = 0;
};

/// 面板注册记录 (UI 注册表快照条目)
struct ClientPanel {
    std::string       plugin;                             ///< 所属插件名
    std::string       id;                                 ///< 全局唯一 id
    std::string       title;                              ///< tab 标题
    utilxx_base::Json items = utilxx_base::Json::array(); ///< {"items":[{...}]} 内容
    /// 内容版本号 (每次 `update_panel` 递增; 供缓存 key/诊断使用)
    uint64_t version = 0;
};

/// Info 栏段落注册记录 (UI 注册表快照条目)
/// - 渲染在侧边栏 Info tab 内 (段落标题 + items, 与面板 items schema 一致),
///   供插件把摘要/状态信息注入 Info 栏 (如 codegraph 索引状态、系统资源占用)
struct ClientInfoSection {
    std::string       plugin;                             ///< 所属插件名
    std::string       id;                                 ///< 全局唯一 id
    std::string       title;                              ///< 段落标题 (空 = 无标题)
    utilxx_base::Json items = utilxx_base::Json::array(); ///< {"items":[{...}]} 内容
    /// 内容版本号 (每次 `update_info_section` 递增; 供缓存 key/诊断使用)
    uint64_t version = 0;
};

/// 工具消息装饰注册记录 (UI 注册表快照条目; update_tool_decor 写入)
/// - 插件对某次工具调用 (toolCallId) 的语义层渲染声明: 折叠头显示名/一行摘要
///   + 展开体 items (text/diagram kind); TUI 按通用渲染器展示, 无任何工具特化
/// - 生命周期: 插件卸载/禁用时自动摘除 (enable 时恢复), 会话切换由插件自行清理
struct ClientToolDecor {
    std::string       plugin;      ///< 所属插件名
    std::string       toolCallId;  ///< 目标工具调用 id
    std::string       displayName; ///< 折叠头显示名 (空 = 原始 toolName)
    std::string       summary;     ///< 折叠头一行摘要 (空 = 回退参数预览)
    utilxx_base::Json items = utilxx_base::Json::array(); ///< 展开体 items ({"items":[...]})
    /// 内容版本号 (每次更新递增; 计入 TUI 块缓存 key —— 消息指针不变时
    /// 装饰更新仍需触发该消息块重建)
    uint64_t version = 0;
};

/// 命令注册记录
struct ClientCommand {
    std::string plugin; ///< 所属插件名
    std::string name;   ///< 命令名 (用户输入 "/{name}" 触发)
    std::string description;
    int32_t(PLUGINXX_CALL* execute)(
        void*                     ud,
        const PluginxxStringView* args_json,
        PluginxxString*           action_out,
        PluginxxString*           error_out
    )        = nullptr;
    void* ud = nullptr;
};

/// 工具特化渲染器注册记录 (UI 注册表快照条目)
struct ClientToolRenderReg {
    std::string                              plugin;
    std::string                              toolName;
    AgentxxToolRenderFn                      renderFn = nullptr;
    void*                                    userData = nullptr;
    std::shared_ptr<ClientToolRendererLease> lease;
    /// 宿主内置渲染器标记 (lib 内置工具无对应插件, 由宿主自身注册;
    /// 见 [ClientPluginManager::registerBuiltinToolRenderer]):
    /// - true 时无插件实例与 lease, 渲染时不做租约/启用状态复查
    /// - 归属名固定为 [kBuiltinRendererOwner]; 不随插件禁用/卸载失效
    bool        builtin = false;
    std::string templateJson;
    std::string templateDisplayName;
    std::string templateSummaryKey;
    std::string templateSummaryTemplate;
};

/// 宿主内置渲染器的保留归属名 (写入 [ClientToolRenderReg::plugin] 与语义渲染
/// 缓存条目, 非插件名): lib 内置工具 (如 agentxx_share_store/agentxx_subagent)
/// 没有对应插件, 其特化渲染由宿主自身注册
inline constexpr std::string_view kBuiltinRendererOwner = "agentxx.core";

/// 通用动作绑定记录 (UI 注册表快照条目; bind_action_handler 写入)
/// - targetId 空串 = 本实例兜底 (方案 A fallback: 精确匹配优先, 未命中回落 "")
/// - 内容与绑定正交: 按钮可先渲染后绑定, 渲染时按快照有无绑定决定是否可点
struct ClientActionBinding {
    std::string       targetId; ///< 归属键 (section_id/panel_id/tool_call_id); "" = 兜底
    std::string       plugin;   ///< 所属插件名
    AgentxxUiActionFn cb = nullptr;
    void*             ud = nullptr;
};

/// 展示区域尺寸快照条目 (面板/Info 段落的可用宽高; UI 线程写, io 线程读)
struct ClientRegionSize {
    std::string id;            ///< 归属 id (面板 id / 段落 id)
    int         width  = 0;    ///< 可用显示宽度 (列)
    int         height = 0;    ///< 当前内容行数 (行)
};

/// 全局快捷键注册记录 (UI 注册表快照条目)
/// - 派发: UI 线程按"当前按键的规范化描述"查到本项后投递到 client io 线程执行回调
///   (UI 线程不进入插件代码); 同一键位只允许一个注册者 (先注册者优先)
struct ClientKeybind {
    std::string plugin;              ///< 所属插件名
    std::string keys;                ///< 规范化后的键位描述 (见 [normalizeKeybindSpec])
    std::string description;         ///< 说明文本 (帮助/列表展示)
    void(PLUGINXX_CALL* handler)(void* ud) = nullptr;
    void* ud = nullptr;
};

/// 快捷键注册被拒记录 (键位已被占用; 供设置弹窗展示冲突情况)
///
/// - 记录时机: `register_keybind` 因"键位已被别的插件占用"返回 NULL。
///   同插件重复注册同一键位不记录 —— 那不是冲突, 是插件自身行为。
/// - 清理时机: 占用方注销/禁用/卸载该键位 (键位空出), 或请求方禁用/卸载
///   (记录作废)。同一 (键位, 请求方) 只保留一条, 重新启用后重试不累积。
/// - 用途: 用户在快捷键列表里看到"某个键位被谁占用、哪个插件没抢到",
///   从而解释插件文档里的快捷键为什么没生效。
struct ClientKeybindConflict {
    std::string keys;   ///< 键位描述 (已规范化, 见 [normalizeKeybindSpec])
    std::string plugin; ///< 尝试注册但被拒的插件
    std::string owner;  ///< 当前占用该键位的插件
};

/// 快捷键描述规范化: 小写化 + 修饰键固定顺序 (ctrl/alt/shift/super) + 别名归一
/// (`control`→`ctrl` / `cmd`/`win`/`meta`→`super` / `return`→`enter`);
/// 非法描述 (空 / 未知修饰键 / 缺少主键 / 多个主键 / 无修饰键的可打印单字符) 返回空串。
///
/// 宿主与界面侧必须用同一口径: 界面把按键事件转成同格式描述后按字符串比较
/// (见 TUI 侧 `keybindOfEvent`)。
std::string normalizeKeybindSpec(std::string_view keys);

/// UI 注册表快照 (UI 线程渲染读取; COW shared_ptr 语义)
struct ClientUiRegistry {
    std::vector<ClientStatusItem>    statusItems;
    std::vector<ClientPanel>         panels;
    std::vector<ClientInfoSection>   infoSections;
    std::vector<ClientCommand>       commands;
    std::vector<ClientKeybind>       keybinds;
    /// 快捷键冲突记录 (注册受阻于已占用的键位; 见 [ClientKeybindConflict])
    /// - 与 [keybinds] 同一 COW 快照: UI 线程一次快照读到"已注册"与"未抢到"两侧信息
    std::vector<ClientKeybindConflict> keybindConflicts;
    std::vector<ClientToolDecor>     toolDecors;
    std::vector<ClientToolRenderReg> toolRenderers;
    /// 宿主内置工具特化渲染器 (lib 内置工具无插件归属, 由宿主自身注册;
    /// 见 [ClientPluginManager::registerBuiltinToolRenderer])
    /// - 与 toolRenderers 分表存放: 匹配优先级低于插件注册项 (插件可覆盖内置渲染)
    /// - 随 UI 注册表快照 (COW) 一并拷贝; 生命周期 = 进程, 不随插件禁用/卸载变化
    std::vector<ClientToolRenderReg> builtinToolRenderers;
    /// 通用动作绑定 (COW 快照: UI 线程渲染时查"该按钮是否可点", 无锁读)
    std::vector<ClientActionBinding> actionBindings;
    /// 插件名 → 实例代次 (重载同名插件后代次改变)。UI 渲染时把代次记进按钮命中框,
    /// 点击派发时由 io 线程复查: 代次不匹配说明实例已重载, 旧点击只能丢弃,
    /// 不得转交同名新实例。
    std::map<std::string, uint64_t, std::less<>> instanceGenerations;

    /// 查询插件实例代次 (0 = 未知/未登记)
    uint64_t generationOf(std::string_view plugin) const {
        auto it = instanceGenerations.find(plugin);
        return it == instanceGenerations.end() ? 0 : it->second;
    }
};

/// 插件单条 UI 描述 JSON 的字节上限 (1 MiB)
inline constexpr size_t kUiJsonMaxBytes = 1024 * 1024;

/// UI 描述 JSON 体积校验 (越界记日志并返回 false; 调用方按"拒绝更新"处理)
///
/// 插件推送的面板/段落/状态栏/装饰/overlay 描述以及工具渲染结果都会进入 UI
/// 注册表快照或渲染缓存, 超大描述同时撑大内存与每帧解析耗时, 因此在入口处
/// 直接拒绝; 组件层自身的上限 (嵌套深度/元素数/文本长度) 见
/// [agentxx::ui::ParseLimits]。
///
/// - `args`:
///     - [json] 待校验的 JSON 文本 (空串视为通过)
///     - [what] 描述用途 (日志用, 如 "update_panel")
///     - [plugin] 插件名 (日志用)
bool acceptUiJsonSize(std::string_view json, std::string_view what, std::string_view plugin);

/// 工具特化渲染统一结果
struct ClientToolRenderResult {
    std::string       displayName;
    std::string       summary;
    utilxx_base::Json items   = utilxx_base::Json::array();
    bool              matched = false;
    bool              isDecor = false; ///< 是否来自动态 toolDecors (update_tool_decor)
    /// 命中"按 tool_name 注册的自定义 renderer"但语义结果尚未计算出来:
    /// 调用方本次用通用回退渲染, 并按 [ClientToolRenderRequest] 提交一次请求。
    /// 自定义 renderer 不在 UI 线程执行 (只在 client IO 线程)。
    bool pendingRender = false;
    /// pendingRender=true 时的 renderer 归属插件 (空 = 未知)
    std::string pendingPlugin;
    /// decor 归因 (isDecor=true 时有效; UI 侧组装 owner_id=toolCallId 用):
    /// button 派发 owner_id 统一为 tool_call_id (以 toolCallId 作 owner_id,
    /// 插件 bind 一次永久生效, 见方案 A); 非 decor 时为空
    std::string decorPlugin;
    std::string decorToolCallId;
};

/// 工具语义渲染结果条目 (宿主拥有; 写入后不再修改, UI 线程只读快照)
/// - 由 client io 线程在持有 renderer lease 时执行插件回调, 并把
///   displayName/summary/items 拷成宿主字符串/JSON 后写入
/// - 插件卸载/禁用/重载时按插件失效, 旧快照因此回退通用渲染
struct ClientToolRenderEntry {
    std::string       key;            ///< 缓存键 (toolCallId, 空则 "#toolName")
    std::string       plugin;         ///< 产出该结果的插件名
    uint64_t          generation = 0; ///< 产出时的实例代次
    uint64_t          inputHash  = 0; ///< 输入特征 (args/结果/宽度等)
    bool              matched    = false;
    std::string       displayName;
    std::string       summary;
    utilxx_base::Json items = utilxx_base::Json::array();
};

/// 工具语义渲染请求 (UI 线程构造; 所有字段为拥有型拷贝, 不在 UI 线程进入插件代码)
struct ClientToolRenderRequest {
    std::string toolCallId;
    std::string toolName;
    std::string argsJson;
    std::string resultText;
    bool        isFinished = false;
    bool        isError    = false;
    int         maxWidth   = 0;

    /// 缓存键: 优先按 tool_call_id (每次调用独立), 无 id 时退回 tool_name
    static std::string keyFor(std::string_view toolCallId, std::string_view toolName);

    /// 输入特征: 任一输入变化都必须重新渲染 (含宽度, 渲染器按宽度换行)
    uint64_t inputHash() const;

    /// 输入特征 (直接吃 string_view, 不做任何拷贝) —— 供"先算特征查缓存,
    /// 确认需要投递时才拷贝请求"的路径使用 (见 renderClientTool / queryToolRender):
    /// 大 args/result 文本的拷贝只在真的提交渲染请求时发生
    static uint64_t hashInputs(
        std::string_view toolName,
        std::string_view argsJson,
        std::string_view resultText,
        bool             isFinished,
        bool             isError,
        int              maxWidth
    );
};

/// 工具语义渲染缓存 (client io 线程写, UI 线程读; 内部短锁)
/// - `lookup`/`version` 只读宿主拷贝, 不调用插件
/// - `store` 递增该键版本号, UI 据此重建消息块
/// - `invalidatePlugin`/`clear` 丢弃条目并递增版本号 (旧快照回退通用渲染)
class ClientToolRenderCache {
public:

    /// 容量上限 (条目数与版本记录数)。超过上限按最旧写入顺序淘汰:
    /// 条目淘汰后该键回到"未命中 → 重新请求渲染"; 版本记录随条目一并回收,
    /// 因此 `version(key)` 对已淘汰键返回 0 (视为从未渲染, UI 缓存键变化后
    /// 重建为通用回退, 再次渲染完成后回到语义内容)。默认 512 远超单屏
    /// 可见块数, 正常会话不触发；长时间会话不再按 tool_call_id 无限增长。
    explicit ClientToolRenderCache(size_t maxEntries = 512) :
        maxEntries_(maxEntries == 0 ? 1 : maxEntries) {}

    std::shared_ptr<const ClientToolRenderEntry>
        lookup(const std::string& key, uint64_t inputHash) const;

    /// 该键的内容版本号 (0 = 从未写入; 计入 TUI 消息块缓存 key)
    uint64_t version(const std::string& key) const;

    /// 写入条目 (按 entry.key); 返回写入后的不可变快照
    std::shared_ptr<const ClientToolRenderEntry> store(ClientToolRenderEntry entry);

    /// 丢弃某插件的全部条目 (禁用/卸载/重载/会话切换)
    void invalidatePlugin(std::string_view plugin);

    void clear();

    /// 标记"该键的这次输入特征已有渲染请求在执行" (返回 false = 已存在)。
    /// UI 每帧都会重新查询, 没有这个去重会让未命中期间每帧重复投递请求。
    bool beginRequest(const std::string& key, uint64_t inputHash);
    void endRequest(const std::string& key, uint64_t inputHash);

    /// 该键的这次输入特征是否已有请求在执行 (零拷贝查询, 供调用方在构造
    /// 请求对象前判断"是否值得拷贝输入并投递")
    bool inFlight(const std::string& key, uint64_t inputHash) const;

    /// 条目数 (测试/诊断)
    size_t size() const;

private:

    /// 淘汰最旧条目 (调用方持锁)。FIFO 近似 LRU: UI 每帧都会查询可见块的
    /// version/lookup, 但按键写入顺序淘汰已足够 (可见块数远小于上限)。
    void evictLocked();

    const size_t                                                                  maxEntries_;
    mutable std::mutex                                                            mutex_;
    std::unordered_map<std::string, std::shared_ptr<const ClientToolRenderEntry>> entries_;
    std::unordered_map<std::string, uint64_t>                                     versions_;
    /// 条目写入顺序 (仅记录当前在 entries_ 中的键, 每键一条)
    std::deque<std::string> order_;
    /// 键 → 尚未完成的请求的输入特征
    std::unordered_map<std::string, uint64_t> pending_;
};

/// 渲染客户端工具特化内容 (折叠头/展开体; UI 线程可调, 不进入插件代码)
/// 查询顺序:
/// 1. toolDecors (按 toolCallId 匹配动态实例级装饰, 如 planning 推送)
/// 2. toolRenderers / builtinToolRenderers (按 toolName; 插件注册项优先) 预设模版
///    (纯宿主计算, 直接在 UI 线程算)
/// 3. 同上的自定义 renderer: 只读 `cache` 中的语义结果;
///    未命中返回 matched=false + pendingRender=true (调用方提交渲染请求)
/// 4. 若均未命中, 返回 matched = false
ClientToolRenderResult renderClientTool(
    const ClientUiRegistry*      reg,
    const ClientToolRenderCache* cache,
    std::string_view             toolCallId,
    std::string_view             toolName,
    std::string_view             argsJson,
    std::string_view             resultText,
    bool                         isFinished,
    bool                         isError,
    int                          maxWidth
);

/// 定时器宿主句柄实现 (仅宿主内部; 与
/// [client_plugin_api.h](/agent/lib/include/agentxx/plugin/api/client_plugin_api.h)
/// 的 `AgentxxTimer` 不透明类型对应)
///
/// - 计时器挂在 client io 执行器上 (asio::steady_timer), 回调在 io 线程执行
/// - `alive` 为取消标记: 取消后即使有尚未返回的等待也不会再回调插件
/// - `repeat` 为剩余触发次数 (0 = 一次性), 周期定时器每次触发后递减
/// - `armedAt` 用于**同帧合并**: io 线程繁忙/事件积压导致多个周期同时到期时,
///   只回调一次并丢弃已错过的周期 (不追赶式连续回调)
struct ClientTimerImpl {
    asio::steady_timer    timer;
    /// 归属实例 (弱引用: 回调前升级, 实例已卸载则丢弃本次触发)
    std::weak_ptr<ClientPluginInstance> inst{};
    std::string                         ownerId;         ///< 关联展示区域 id (可空)
    int32_t                             intervalMs  = 50; ///< 触发间隔 (已按宿主下限收敛)
    int32_t                             repeat      = 0;  ///< 剩余触发次数 (0 = 一次性)
    bool                                repeatMode  = false; ///< true = 周期 (按 repeat 计数)
    bool                                pauseHidden = false; ///< 关联区域不可见时跳过回调
    /// 上次续期时刻 (判定"迟到"用; 见 [armTimer])
    std::chrono::steady_clock::time_point armedAt{};
    /// 因同帧合并被丢弃的触发次数 (诊断用)
    uint64_t                            dropped = 0;
    void(PLUGINXX_CALL* cb)(void*) = nullptr;
    void*       ud    = nullptr;
    bool        alive = true;

    explicit ClientTimerImpl(asio::any_io_executor ex) :
        timer(std::move(ex)) {}
};

/// client 插件实例 (宿主侧状态)
/// - 与 agent 侧 PluginInstance 对称: 同一动态库可被 agent 与 client 两个
///   管理器各自 dlopen (引用计数), 实例状态彼此独立, 互通一律走 wire
/// - 所有注册残留 (status item/panel/command/订阅) 记录于此, 卸载时统一清理
/// - 仅 client io 线程读写 (inflight 为原子, 跨线程递增/递减)
class ClientPluginInstance : public PluginInstanceBase {
public:

    /// 继承 PluginInstanceBase 的公共字段 (name/version/path/configPath/args/depends/
    /// dlHandle/pluginCtx/enabled/inflight 等), 见
    /// [instance_base.h](/agent/third_party/cxx_pluginxx/include/pluginxx/runtime/instance_base.h)
    /// 接口声明 (plugin.yaml `interfaces`; 加载时随 manifest 解析传入,
    /// 直连库路径为空) —— 宿主限制依据, 经 list() 暴露供展示/排查
    PluginManifestInterfaces interfaces;

    /// 事件订阅记录 (卸载自动退订; 仅 io 线程)
    /// - shared_ptr 存储: 订阅节点地址稳定 (vector 扩容/erase 不悬垂);
    ///   dispatch 时拷贝 shared_ptr 保活, 派发中退订/卸载不 UAF
    struct Subscription {
        int32_t event                                                                  = 0;
        void(PLUGINXX_CALL* handler)(const PluginxxStringView* payload_json, void* ud) = nullptr;
        void* ud                                                                       = nullptr;
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
    /// 全局快捷键注册信息 (同命令: disable 时随 UI 注册表摘除, enable 由 start 重声明)
    std::vector<ClientKeybind>     keybindRegs;
    /// 工具消息装饰 (disable 保留, enable 恢复; 无句柄 —— 以 plugin+toolCallId 键控)
    std::vector<ClientToolDecor> toolDecorRegs;
    /// 工具特化渲染器 (disable 保留, enable 恢复; 以 plugin+toolName 键控)
    std::vector<ClientToolRenderReg> toolRenderRegs;
    /// 通用动作绑定 (disable 保留, enable 恢复; 以 plugin+targetId 键控)
    std::vector<ClientActionBinding> actionRegs;
    /// 事件订阅登记 (client 自有事件表 `agentxx.client.events` 的订阅)
    ///
    /// 命名与基类的通用订阅登记区分: 基类 `PluginInstanceBase::subscriptions`
    /// 存的是通用事件表 (`agentxx.agent.events`, 按主题) 的句柄; 本成员存的是
    /// client 事件表 (按事件枚举) 的订阅记录。两者语义不同, 因此这里用独立名字,
    /// 避免派生成员隐藏基类成员导致通用表实现读到错误的类型。
    std::vector<std::shared_ptr<Subscription>> clientSubscriptions; ///< 已订阅事件 (disable 保留)
    /// 定时器 (client 自有 `agentxx.client.timer` 表): 实例级运行时资源
    /// - 与注册信息不同, 定时器**不跨 disable 保留**: 禁用/卸载时全部取消
    ///   (见 [ClientPluginManager::detachDomainRegistrations]), enable 后由插件
    ///   start 事务重新注册
    std::vector<std::shared_ptr<ClientTimerImpl>> timers;
    /// 全局快捷键句柄 (disable 期随注册表摘除, 句柄保活到实例析构)
    std::vector<std::shared_ptr<AgentxxKeybind>> keybindHandles;
    std::vector<std::shared_ptr<void>> statusItemHandles; ///< 状态栏项宿主句柄 (enable 期)
    std::vector<std::shared_ptr<void>> panelHandles;      ///< 面板宿主句柄 (enable 期)
    std::vector<std::shared_ptr<void>> infoSectionHandles; ///< Info 段落宿主句柄 (enable 期)
    std::vector<std::shared_ptr<void>> subHandles;         ///< 订阅句柄保活

    /// 管理器弱引用 (host vtable 回调取用)
    std::weak_ptr<ClientPluginManager> manager{};
    /// 与公共实例控制块对齐，供 Client 裸指针回调升级 owner。
    std::weak_ptr<ClientPluginInstance> self{};

    explicit ClientPluginInstance(std::string in_name) :
        PluginInstanceBase(std::move(in_name)) {}

    /// 析构时 dlclose (与 agent 侧 PluginInstance 一致; 调用方保证无执行中回调:
    /// unloadAsync 等 inflight 归零后移除, shutdownAll 进程退出路径约定无执行中)
    ~ClientPluginInstance();

    /// 本端 destroy 入口符号名 (client 侧)
    const char* pluginDestroySymbol() const noexcept override {
        return AGENTXX_PLUGIN_CLIENT_SYMBOL_DESTROY;
    }

    /// 日志前缀 (与 agent 侧插件宿主同进程共存时区分来源)
    std::string_view logTag() const noexcept override {
        return "[client_plugin] ";
    }
};

/// 事件订阅宿主句柄实现 (仅宿主内部; 与
/// [plugin_api.h](/agent/lib/include/agentxx/plugin/api/plugin_api.h) 的
/// C 不透明类型对应, 命名避免与 agent 侧 PluginManager 的全局定义 ODR 冲突)
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
                            public pluginxx::PluginHostLifecycle<ClientPluginInstance>,
                            public std::enable_shared_from_this<ClientPluginManager> {
public:

    struct PluginListView {
        std::string              name;
        std::string              version;
        std::string              description;
        std::string              path;
        std::string              configPath;
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

    /// 当前 UI 适配器 (supportedInterfaces() 声明宿主接口集; 任意线程, 装配后不可变)
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

    /// 卸载插件 (按名称; 等全部执行中回调完成后才 dlclose)
    /// - 默认超时较 agent 侧短 (UI 交互路径不希望长时间挂起)
    asio::awaitable<bool> unloadAsync(
        std::string_view          name,
        std::chrono::milliseconds timeout = std::chrono::seconds{10}
    ) {
        co_return co_await pluginxx::PluginHostLifecycle<ClientPluginInstance>::unloadAsync(
            name,
            timeout
        );
    }

    /// 禁用插件 (UI 项摘除/命令停用; 立即生效)
    /// - 级联: 必选依赖本插件的插件一同禁用 (依赖者先禁用)
    /// - 被级联禁用的插件不置 userDisabled (用户显式 enable 依赖方时可级联恢复)
    /// - 具体行为由 pluginxx::PluginHostLifecycle 提供 (disable → disableImpl)

    /// 加载配置中应于 client 侧生效的插件 (yaml `plugins` 段, 经 sides 过滤):
    /// - sides == Client 或 sides == Auto: 尝试加载 (Auto 下无 client 入口则跳过)
    /// - sides == Agent: 跳过 (属于 agent 侧)
    /// - 按 manifest depends 拓扑排序加载 (依赖者排在被依赖者之后)
    asio::awaitable<void>
        loadConfiguredClientPlugins(const std::vector<agentxx::agent::PluginConfig>& plugins);

    // ==================== 查询 ====================

    std::vector<PluginListView> list() const;

    /// 因接口要求未满足而被跳过的插件 (name → 缺失接口描述; io 线程;
    /// 加载阶段写入, 供展示层/排查 "为什么没加载" —— 跳过的插件不会出现在
    /// list() 中, 原因单独记录)
    const std::map<std::string, std::string>& skippedPlugins() const {
        return skippedPlugins_;
    }

    /// 宿主当前支持的接口名集合 (由 uiAdapter->supportedInterfaces() 声明;
    /// io 线程; 限制检查与 EVT_READY / get_client_state 的
    /// interfaces 数组共用本结果 —— 单一事实来源)
    InterfaceSet hostSupportedInterfaces() const;

    // ==================== UI 注册表 (任意线程) ====================

    /// 注册表快照 (短锁拷贝 shared_ptr; UI 线程渲染无锁读取)
    std::shared_ptr<const ClientUiRegistry> uiRegistrySnapshot() const;

    /// 工具语义渲染缓存 (任意线程可读; 内部短锁)。UI 线程只读其中的宿主拷贝,
    /// 自定义 renderer 的插件回调一律在 client io 线程执行。
    std::shared_ptr<ClientToolRenderCache> toolRenderCache() const {
        return toolRenderCache_;
    }

    /// 取工具语义渲染结果 (UI 线程):
    /// - 命中缓存 (同输入特征) → 返回宿主拥有的结果, 不进入插件代码
    /// - 未命中 → 拷贝输入并投递到 client io 线程执行自定义 renderer, 本次返回
    ///   nullptr (调用方用通用回退); 结果到达后经 uiAdapter->onToolRenderUpdated
    ///   通知 UI 重建该消息块
    std::shared_ptr<const ClientToolRenderEntry>
        requestToolRender(const ClientToolRenderRequest& req);

    /// 该键的这次输入特征是否已有渲染请求在执行 (UI 线程; 零拷贝)。
    /// UI 侧查询工具渲染时先问一次: 在途则本帧用通用回退, 不必为投递
    /// 拷贝 args/result 大文本 (见 message_list 的 queryToolRender)
    bool toolRenderInFlight(std::string_view key, uint64_t inputHash) const;

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
    /// {"sessionId","connState","startupProgress","interfaces":[...],
    ///  "agentPlugins":[{"name","version","interfaces":[...]},...]}
    /// - agentPlugins: 服务端已加载的 agent 侧插件结构化列表 (来自宿主约定
    ///   事件 server_plugins / WireHelloAck.plugins); 空数组 = 未知 (服务端
    ///   未提供), 插件不得据此断言"对端未加载"
    std::string clientStateJson() const;

    // ==================== ClientEventSink 实现 (io 线程) ====================
    // 端点事件 → JSON payload → 分发到订阅了对应事件的插件回调

    void onReady() override;
    void onConnStateChanged(std::string_view state, std::string_view progress) override;
    void onUserInput(std::string_view sessionId, std::string_view text) override;
    void onDelta(const agentxx::agent::WireDelta& delta) override;
    void onTurnResult(const agentxx::agent::WireTurnResult& result) override;
    void onSessionSwitched(std::string_view sessionId) override;
    void onPluginData(const agentxx::agent::WirePluginData& data) override;

    // ==================== 内部 (host vtable 回调) ====================
    // 以下为 vtable 实现的强类型入口, 须 io 线程调用 (vtable 内部经 ioCallSync
    // 跨线程投递, 插件无感)

    /// 注册状态栏项; 返回宿主句柄 (nullptr = 宿主不支持或 id 冲突)
    void* registerStatusItem(
        ClientPluginInstance* inst,
        PluginxxStringView    id,
        PluginxxStringView    json,
        int                   align,
        int                   order
    );

    void* registerStatusItem(
        ClientPluginInstance* inst,
        std::string_view      id,
        std::string_view      json,
        int                   align,
        int                   order
    ) {
        return registerStatusItem(inst, strToSv(id), strToSv(json), align, order);
    }

    /// 更新状态栏项; 返回 0 成功
    int updateStatusItem(ClientPluginInstance* inst, void* item, PluginxxStringView json);

    int updateStatusItem(ClientPluginInstance* inst, void* item, std::string_view json) {
        return updateStatusItem(inst, item, strToSv(json));
    }

    void unregisterStatusItem(ClientPluginInstance* inst, void* item);
    /// 注册面板; 返回宿主句柄 (nullptr = 宿主不支持或 id 冲突)
    void* registerPanel(
        ClientPluginInstance* inst,
        PluginxxStringView    id,
        PluginxxStringView    props_json
    );

    void* registerPanel(
        ClientPluginInstance* inst,
        std::string_view      id,
        std::string_view      props_json
    ) {
        return registerPanel(inst, strToSv(id), strToSv(props_json));
    }

    /// 更新面板内容; 返回 0 成功
    int updatePanel(ClientPluginInstance* inst, void* panel, PluginxxStringView items_json);

    int updatePanel(ClientPluginInstance* inst, void* panel, std::string_view items_json) {
        return updatePanel(inst, panel, strToSv(items_json));
    }

    void unregisterPanel(ClientPluginInstance* inst, void* panel);
    /// 注册 Info 栏段落; 返回宿主句柄 (nullptr = 宿主不支持或 id 冲突)
    void* registerInfoSection(
        ClientPluginInstance* inst,
        PluginxxStringView    id,
        PluginxxStringView    props_json
    );

    void* registerInfoSection(
        ClientPluginInstance* inst,
        std::string_view      id,
        std::string_view      props_json
    ) {
        return registerInfoSection(inst, strToSv(id), strToSv(props_json));
    }

    /// 更新 Info 栏段落内容; 返回 0 成功
    int updateInfoSection(ClientPluginInstance* inst, void* section, PluginxxStringView items_json);

    int updateInfoSection(ClientPluginInstance* inst, void* section, std::string_view items_json) {
        return updateInfoSection(inst, section, strToSv(items_json));
    }

    void unregisterInfoSection(ClientPluginInstance* inst, void* section);
    /// 更新/删除工具消息装饰 (io 线程); 返回 0 成功
    /// - tool_call_id 空 = 操作本插件全部; decor_json 空串 = 删除
    int updateToolDecor(
        ClientPluginInstance* inst,
        PluginxxStringView    tool_call_id,
        PluginxxStringView    decor_json
    );

    int updateToolDecor(
        ClientPluginInstance* inst,
        std::string_view      tool_call_id,
        std::string_view      decor_json
    ) {
        return updateToolDecor(inst, strToSv(tool_call_id), strToSv(decor_json));
    }

    /// 注册工具特化渲染器 (io 线程); 返回 0 成功
    int registerToolRenderer(ClientPluginInstance* inst, const AgentxxToolRenderSpec* spec);
    /// 注销工具特化渲染器 (io 线程); 返回 0 成功
    int unregisterToolRenderer(ClientPluginInstance* inst, PluginxxStringView tool_name);

    int unregisterToolRenderer(ClientPluginInstance* inst, std::string_view tool_name) {
        return unregisterToolRenderer(inst, strToSv(tool_name));
    }

    /// 注册宿主内置工具特化渲染器 (非插件; lib 内置工具无对应插件, 由宿主自身声明)
    /// - 存入 [ClientUiRegistry::builtinToolRenderers], 归属名固定为
    ///   [kBuiltinRendererOwner]; 匹配优先级低于插件注册项 (插件可覆盖内置渲染)
    /// - renderFn 为进程生命周期函数 (宿主自身实现), userData 由调用方保证存活;
    ///   无插件实例/lease 语义: 不受插件禁用/卸载影响, 渲染前不做租约复查
    /// - 任意线程可调 (uiMutex_ 保护); 同一 tool_name 重复注册覆盖旧项
    /// 返回 0 成功
    int registerBuiltinToolRenderer(
        std::string_view    toolName,
        AgentxxToolRenderFn fn,
        void*               userData = nullptr
    );

    /// 绑定动作处理器 (io 线程; cb 空则失败; 同 (plugin,targetId) 覆盖)
    /// - targetId 空串 = 本实例兜底 (方案 A fallback)
    /// - 同步写 uiRegistry_ (COW) + inst->actionRegs (disable/enable 恢复用)
    /// 返回 0 成功
    int bindActionHandler(
        ClientPluginInstance* inst,
        PluginxxStringView    target_id,
        AgentxxUiActionFn     on_action,
        void*                 user_data
    );

    int bindActionHandler(
        ClientPluginInstance* inst,
        std::string_view      target_id,
        AgentxxUiActionFn     on_action,
        void*                 user_data
    ) {
        return bindActionHandler(inst, strToSv(target_id), on_action, user_data);
    }

    /// 解绑动作处理器 (不存在忽略, 返回 0)
    int unbindActionHandler(ClientPluginInstance* inst, PluginxxStringView target_id);

    int unbindActionHandler(ClientPluginInstance* inst, std::string_view target_id) {
        return unbindActionHandler(inst, strToSv(target_id));
    }

    /// UI 线程调 (点击命中后), 内部 postToIo 到 io 线程二次校验后派发:
    /// 插件存在且 enabled → 精确 bindings[ownerId] 命中否则回落 [""] →
    /// 快照 cb/ud 与实例当前一致 → InflightGuard 后直调 cb
    /// - 任意线程可调 (UI 线程点击路径)
    /// - generation: 点击时 UI 快照中的实例代次 (0 = 不校验)。实例重载后旧点击
    ///   代次不匹配 → 丢弃, 不转交同名新实例
    void dispatchAction(
        std::string plugin,
        std::string ownerId,
        std::string actionId,
        std::string argsJson,
        uint64_t    generation = 0
    );

    /// 通用 overlay 打开 (io 线程; 校验 version/type, 拷贝字符串后经
    /// adapter->onOverlayOpen 投递 UI 线程); 返回 0 成功
    int openOverlay(ClientPluginInstance* inst, const AgentxxOverlaySpec* spec);
    /// 通用 overlay 关闭 (io 线程; 经 adapter->onOverlayClose)
    void closeOverlay(ClientPluginInstance* inst);

    // ==================== 展示区域尺寸 (任意线程) ====================
    //
    // 用途: 面板/Info 段落由宿主布局, 插件拿不到可用宽度; 宿主在每次布局后把各归属的
    // 可用宽高记入快照, 值变化时向订阅 AGENTXX_CLIENT_EVT_UI_LAYOUT 的插件投递事件,
    // 并把它放进 get_client_state().regions 供按需查询 (SDK: ClientPluginBase::regionSize)。

    /// 上报某归属的可用尺寸 (UI 线程调用; 值未变化时不做任何事)
    /// - 首次上报总是会投递一次事件 (当前值从"未知"变为已知)
    /// - 事件投递到 io 线程执行 (订阅回调的线程约定不变)
    void reportRegionSize(const std::string& id, int width, int height);

    /// 当前区域尺寸快照 (任意线程; 按 id 升序)
    std::vector<ClientRegionSize> regionSizes() const;

    /// 注册命令; 返回 0 成功 (名字冲突返回非 0)
    int registerCommand(
        ClientPluginInstance* inst,
        PluginxxStringView    name,
        PluginxxStringView    description,
        int32_t(PLUGINXX_CALL*
                    exec)(void*, const PluginxxStringView*, PluginxxString*, PluginxxString*),
        void* ud
    );

    int registerCommand(
        ClientPluginInstance* inst,
        std::string_view      name,
        std::string_view      description,
        int32_t(PLUGINXX_CALL*
                    exec)(void*, const PluginxxStringView*, PluginxxString*, PluginxxString*),
        void* ud
    ) {
        return registerCommand(inst, strToSv(name), strToSv(description), exec, ud);
    }

    int unregisterCommand(ClientPluginInstance* inst, PluginxxStringView name);

    int unregisterCommand(ClientPluginInstance* inst, std::string_view name) {
        return unregisterCommand(inst, strToSv(name));
    }

    /// 事件订阅; 返回句柄 (宿主持有; 卸载自动退订)
    PluginxxSubscription* subscribe(
        ClientPluginInstance* inst,
        int32_t               event,
        void(PLUGINXX_CALL* handler)(const PluginxxStringView*, void*),
        void* ud
    );
    void unsubscribe(PluginxxSubscription* sub);

    // ==================== 定时器 (`agentxx.client.timer`, io 线程) ====================
    //
    // 插件侧定时器只能经宿主驱动: 定时器挂在 client io 执行器上, 回调在 io 线程
    // 执行 (插件代码不在 UI 线程运行这一约束因此不变)。门控: 宿主动画等级为
    // Disabled 时拒绝注册; 关联区域不可见的定时器可声明 pause_when_hidden 跳过回调。

    /// 注册定时器 (io 线程); 返回宿主句柄 (nullptr = 校验失败/动画关闭/超上限)
    AgentxxTimer* setTimer(ClientPluginInstance* inst, const AgentxxTimerSpec* spec);
    /// 取消定时器 (io 线程; 空句柄/已取消忽略)
    void cancelTimer(ClientPluginInstance* inst, AgentxxTimer* timer);
    /// 关联区域当前是否可见 (io 线程; 未知区域返回 0)
    bool isRegionVisible(std::string_view ownerId) const;

    /// 上报展示区域可见性 (UI 线程; 值未变化时不做任何事)
    /// - 面板: 该面板是否为当前激活 tab; Info 段落: 侧边栏是否显示 Info tab;
    ///   overlay: 是否正在显示
    /// - 供 `pause_when_hidden` 门控与 `is_visible` 查询使用 (未知区域按不可见处理)
    void reportRegionVisible(const std::string& id, bool visible);

    /// 区域可见性快照 (任意线程; 按 id 升序; 仅包含显式上报过的区域)
    std::map<std::string, bool, std::less<>> regionVisibility() const;

    /// 动画等级门控 (装配方在启动与设置变化时调用; 默认 true = 允许定时器)
    /// - false: 新的 `set_timer` 一律失败 (插件据返回值降级为静态展示)
    void setAnimationEnabled(bool enabled) {
        animationEnabled_.store(enabled, std::memory_order_relaxed);
    }

    bool animationEnabled() const {
        return animationEnabled_.load(std::memory_order_relaxed);
    }

    // ==================== 全局快捷键 (`agentxx.client.keybind`) ====================

    /// 注册快捷键 (io 线程); 返回宿主句柄 (nullptr = 键位非法/已被占用/超上限)
    AgentxxKeybind* registerKeybind(ClientPluginInstance* inst, const AgentxxKeybindSpec* spec);
    /// 注销快捷键 (io 线程; 空句柄/已注销忽略)
    void unregisterKeybind(ClientPluginInstance* inst, AgentxxKeybind* bind);
    /// 快捷键是否已注册 (UI 线程判断是否拦截按键; 短锁)
    bool hasKeybind(std::string_view keys) const;
    /// 当前快捷键列表 (任意线程; 按键位升序)
    std::vector<ClientKeybind> keybinds() const;
    /// 投递快捷键触发到 client io 线程执行 (任意线程; 未注册/插件禁用时忽略)
    /// - keys 须为 [normalizeKeybindSpec] 口径 (界面把按键事件转成该口径)
    void postKeybindInvocation(std::string keys);

    /// 自描述
    std::string getOwnInfoJson(ClientPluginInstance* inst);
    std::string getPluginArgsJson(ClientPluginInstance* inst);
    std::string getPluginConfigPath(ClientPluginInstance* inst);

    std::string getLanguage() const {
        return language_.empty() ? "en" : language_;
    }

    void setLanguage(std::string_view lang) {
        language_ = agent::normalizeLanguage(lang);
    }

    /// 会话操作 (代理到端点)
    void sendUserInputToPeer(
        ClientPluginInstance* inst,
        PluginxxStringView    sessionId,
        PluginxxStringView    text
    );

    void sendUserInputToPeer(
        ClientPluginInstance* inst,
        std::string_view      sessionId,
        std::string_view      text
    ) {
        sendUserInputToPeer(inst, strToSv(sessionId), strToSv(text));
    }

    void requestCancelToPeer(ClientPluginInstance* inst, PluginxxStringView sessionId);

    void requestCancelToPeer(ClientPluginInstance* inst, std::string_view sessionId) {
        requestCancelToPeer(inst, strToSv(sessionId));
    }

    /// 跨端数据 (client → agent): 经端点 WirePluginDataUp 发送
    int sendPluginDataToPeer(
        ClientPluginInstance* inst,
        PluginxxStringView    event,
        PluginxxStringView    json
    );

    int sendPluginDataToPeer(
        ClientPluginInstance* inst,
        std::string_view      event,
        std::string_view      json
    ) {
        return sendPluginDataToPeer(inst, strToSv(event), strToSv(json));
    }

    /// 宿主 vtable (静态函数表 `g_clientHostVtable`; 覆写 lifecycle 骨架的同名接缝)
    const PluginxxHostVtable* hostVtable() override;

protected:

    // =====================================================================
    // pluginxx::PluginHostLifecycle 宿主接缝
    // =====================================================================
    //
    // 装载以外的生命周期骨架 (启停/禁用启用/卸载/级联依赖/关闭等待/destroy)
    // 与宿主领域无关, 已下沉到 cxx_pluginxx
    // (见 pluginxx/host/lifecycle.h); 内核不认识 client 侧的 UI 注册表,
    // 因此这些领域动作经下列覆写注入。
    //
    // 注意: 本类**不**复用装载骨架 —— client 侧的装载有独立语义 (dlopen 卸载到
    // 内部线程池执行、接口协商限制、agent/client 双入口探测), 见
    // [loadNativeAsync]。

    /// 管理器自引用 (骨架的异步事务与空闲收尾需要在此期间保活管理器)
    std::shared_ptr<pluginxx::PluginHostLifecycle<ClientPluginInstance>> selfRef() override {
        return shared_from_this();
    }

    /// 生成 client 侧实例对象 (领域自引用/管理器弱引用)
    /// - 元信息/lifecycle 入口/生命周期控制块/宿主控制块由 [attachInstance] 装配
    std::shared_ptr<ClientPluginInstance> createInstance(std::string name) override;

    /// client 侧插件入口符号名 (内核不硬编码宿主专名, 见 pluginxx/api/entry.h)
    ///
    /// 本类自带装载实现 (见上方说明), 但仍覆写本接缝: 一来与内核默认保持一处定义,
    /// 二来 [AGENTXX_PLUGIN_CLIENT_SYMBOL_*] 与插件导出宏共用同一批字符串常量。
    pluginxx::PluginEntrySymbols entrySymbols() const override {
        return {
            AGENTXX_PLUGIN_CLIENT_SYMBOL_GET_INFO,
            AGENTXX_PLUGIN_CLIENT_SYMBOL_CREATE,
            AGENTXX_PLUGIN_CLIENT_SYMBOL_START,
            AGENTXX_PLUGIN_CLIENT_SYMBOL_STOP,
        };
    }

    /// 日志前缀 (与 agent 侧插件宿主同进程共存时区分来源)
    std::string_view logTag() const noexcept override {
        return "[client_plugin] ";
    }

    /// 摘除实例的领域注册: UI 注册表 (状态栏项/面板/Info 段/命令/装饰/渲染器/
    /// 动作绑定) + client 事件订阅 + adapter 通知 + 语义渲染缓存失效
    void detachDomainRegistrations(ClientPluginInstance* inst) override;

    /// 实例加载完成: 登记实例代次 (UI 点击据此复查, 防旧点击转交同名新实例)
    void onInstanceLoaded(ClientPluginInstance& inst) override;

    /// 实例从插件表摘除: 清除实例代次登记
    void onInstanceUnloaded(ClientPluginInstance& inst) override;

    /// 卸载级联只统计"启用中"的依赖者 (client 侧既有行为: 已禁用的依赖者保持加载)
    bool cascadeUnloadEnabledOnly() const noexcept override {
        return true;
    }

private:

    friend class ClientPluginInstance;

    /// 事件分发: 遍历全部插件订阅, 匹配 event → InflightGuard → handler
    /// (io 线程; payload 为宿主构造的 JSON 字符串)
    void dispatchEvent(int event, const std::string& payloadJson);

    /// 命令动作解析与分发 (io 线程; execute 已返回):
    /// {"action":"send","text"} → adapter->sendPluginMessage
    /// {"action":"toast","text","level"} → adapter->onToast
    /// {"action":"none"}/{}/非法 → 记日志
    void dispatchCommandAction(const std::string& actionJson);

    /// 执行一次工具语义渲染 (仅 client io 线程; 见 [requestToolRender]):
    /// 从当前注册表取自定义 renderer (插件注册项优先, 未命中再查宿主内置项),
    /// 插件渲染器需复查 lease/实例状态后代次, 持 lease 调用插件回调;
    /// 宿主内置渲染器 (无插件归属) 直接调用。输出统一拷成宿主对象写入缓存
    /// 并通知 UI 重绘。
    void performToolRender(ClientToolRenderRequest req, std::string key, uint64_t inputHash);

    /// 登记/清除插件实例代次 (io 线程; 供 UI 点击携带与复查; 见
    /// [ClientUiRegistry::instanceGenerations])
    void setRegistryGeneration(std::string_view plugin, uint64_t generation, bool present);

    /// 内部线程池 (dlopen/entry 卸载执行; shutdownAll 时 join)
    std::unique_ptr<asio::thread_pool> pool_;

    std::shared_ptr<PluginUiAdapter> uiAdapter_; ///< 注入后不可变 (io 线程读写)

    /// 工具消息装饰版本号序列 (io 线程递增; 计入 ClientToolDecor.version,
    /// 供 TUI 块缓存 key 感知装饰更新)
    uint64_t toolDecorVersionSeq_ = 1;

    /// UI 注册表 (COW: io 线程写, 任意线程快照读)
    mutable std::mutex                      uiMutex_;
    std::shared_ptr<const ClientUiRegistry> uiRegistry_;

    /// 工具语义渲染缓存 (client io 线程写, UI 线程读; 见
    /// [ClientToolRenderCache])。自定义 renderer 的插件回调只在 client io
    /// 线程执行, UI 线程只消费这里已经拷成宿主对象的语义结果。
    std::shared_ptr<ClientToolRenderCache> toolRenderCache_
        = std::make_shared<ClientToolRenderCache>();

    /// 会话上下文 (io 线程)
    std::string sessionId_ = "session";
    std::string connState_ = "connecting";
    std::string startupProgress_;

    /// 服务端已加载的 agent 侧插件结构化信息 (io 线程写读): 来自宿主约定
    /// 事件 `agentxx_host.server_plugins` (WirePluginData; 载荷
    /// [{"name","version","interfaces":[...]},...]); 空数组 = 未知 (服务端
    /// 未提供)。client 插件经 get_client_state("agentPlugins") 查询对端
    /// 可用性与声明的接口, 对端缺失时可降级提示, 避免上行数据被静默丢弃
    /// 的"操作成功"假象
    struct ServerPluginInfo {
        std::string              name;
        std::string              version;
        std::vector<std::string> interfaces; ///< 该插件声明的接口 (require∪optional)
    };

    std::vector<ServerPluginInfo> serverPlugins_;    /// PLUGIN_DATA 无订阅者警告去重 (仅 io 线程; 每插件名只警告一次):
    /// 收到 WirePluginData 但无任何 client 插件订阅 EVT_PLUGIN_DATA 时,
    /// 多半是对端插件未在本地加载 —— 提示一次便于排查, 不随事件频率刷屏
    std::set<std::string> pluginDataNoSubWarned_;

    /// 因接口要求未满足被跳过的插件 (io 线程; 见 skippedPlugins())
    std::map<std::string, std::string> skippedPlugins_{};

    /// 展示区域尺寸快照 (UI 线程写, 任意线程读; 见 reportRegionSize)
    mutable std::mutex                                          regionMutex_;
    std::map<std::string, ClientRegionSize, std::less<>>        regionSizes_;

    /// 展示区域可见性快照 (UI 线程写, 任意线程读; 见 reportRegionVisible)
    /// - 供 `pause_when_hidden` 定时器门控与 `agentxx.client.timer` 的 is_visible 使用
    std::map<std::string, bool, std::less<>>                    regionVisibility_;

    /// 动画等级门控 (装配方经 setAnimationEnabled 更新; 默认允许定时器)
    std::atomic<bool> animationEnabled_{true};

    std::string                        language_ = "en";
};

/// UI 适配器抽象接口 (UI 无关语义层 → 具体 UI 实现)
///
/// 实现方 (TUI/CLI/未来 GUI) 职责:
/// - supportedInterfaces(): 声明支持的接口名集合 ("client.panel" 等, 常量见
///   [plugin_interfaces.h](/agent/lib/include/agentxx/plugin/plugin_interfaces.h)
///   plugin_interfaces; 宿主据此装配 "client.ui" 接口表、
///   子能力限制判定与插件加载限制)
/// - 各回调在 client io 线程调用, 实现必须快速返回; 涉及 UI 线程独占操作
///   (组件树修改/重绘) 须自行跨线程投递 (如 TUI 的 enqueueUiAction)
/// - 注册表数据 (text/items) 由 ClientPluginManager 持有, UI 渲染经
///   uiRegistrySnapshot() 读取; 本接口回调仅作"注册/更新/移除"信号
class PluginUiAdapter {
public:

    virtual ~PluginUiAdapter() = default;

    /// 声明支持的接口名集合 (plugin_interfaces 常量; 决定 agentxx.client.ui 接口表
    /// 内哪些成员非 NULL 与加载限制判定)
    virtual InterfaceSet supportedInterfaces() const = 0;

    /// ---- 信号回调 (client io 线程; 快速返回) ----

    /// 状态栏项注册/更新/移除 (props: {"text","tooltip"})
    virtual void onStatusItemRegistered(
        const std::string& /*id*/,
        const utilxx_base::Json& /*props*/,
        int /*align*/,
        int /*order*/
    ) {}

    virtual void
        onStatusItemUpdated(const std::string& /*id*/, const utilxx_base::Json& /*props*/) {}

    virtual void onStatusItemRemoved(const std::string& /*id*/) {}

    /// 面板注册/更新/移除 (props: {"title"}; items: {"items":[...]})
    virtual void onPanelRegistered(const std::string& /*id*/, const utilxx_base::Json& /*props*/) {}

    virtual void onPanelUpdated(const std::string& /*id*/, const utilxx_base::Json& /*items*/) {}

    virtual void onPanelRemoved(const std::string& /*id*/) {}

    /// Info 栏段落注册/更新/移除 (props: {"title"}; items: {"items":[...]})
    virtual void
        onInfoSectionRegistered(const std::string& /*id*/, const utilxx_base::Json& /*props*/) {}

    virtual void
        onInfoSectionUpdated(const std::string& /*id*/, const utilxx_base::Json& /*items*/) {}

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

    /// 通用 overlay 打开 (client io 线程; 快速返回, 实现须自行跨线程投递)
    /// - type: AgentxxOverlayType (0=MERMAID 1=TEXT 2=DIFF 3=CUSTOM)
    /// - 单模态 last-wins: 替换当前 overlay (含核心弹窗)
    virtual void onOverlayOpen(
        const std::string& /*plugin*/,
        int /*type*/,
        const std::string& /*title*/,
        const std::string& /*payload*/,
        const std::string& /*extraJson*/
    ) {}

    /// 通用 overlay 关闭 (client io 线程; plugin 仅记日志/鉴权预留)
    virtual void onOverlayClose(const std::string& /*plugin*/) {}

    /// 工具语义渲染结果已写入缓存 (client io 线程; 触发重绘即可 ——
    /// 渲染从 toolRenderCache() 读取语义快照, 不在 UI 线程执行插件回调)
    virtual void
        onToolRenderUpdated(const std::string& /*toolCallId*/, const std::string& /*toolName*/) {}
};

} // namespace plugin
} // namespace agentxx
