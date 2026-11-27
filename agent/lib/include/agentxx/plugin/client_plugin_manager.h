#pragma once

#include "agentxx/agent/config.h"
#include "agentxx/agent/io/client_event_sink.h"
#include "agentxx/plugin/api/client_plugin_api.h"
// 主程序侧 vtable/管理器实现使用 SDK 提供的跨边界字符串工具 (PluginStringView /
// PluginString); 该头后续会拆为 pluginxx/kit/kit.h (通用部分) + 本头的业务 helper
#include "agentxx/plugin/api/client_plugin_kit.h"
#include "agentxx/plugin/client_ui_content.h"
#include "agentxx/plugin/net_service.h"
#include "agentxx/plugin/plugin_framework.h"
#include "agentxx/plugin/plugin_interfaces.h"

#include "asio/any_io_executor.hpp"
#include "asio/awaitable.hpp"
#include "asio/thread_pool.hpp"
#include "pluginxx/api/ui.h"
#include "pluginxx/host/ui_content.h"
#include "pluginxx/host/ui_slots.h"
#include "pluginxx/runtime/plugin_thread.h"
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
#include <utility>
#include <vector>

namespace agentxx {
namespace plugin {

class ClientPluginManager;
class PluginUiAdapter;
class ClientPluginInstance;

/// 自定义 renderer 的主程序控制块。UI 线程可能还持有旧 COW 快照；关闭时先
/// 失效控制块，旧快照只能回退，不能再跳入已经卸载的插件代码。
struct ClientToolRendererLease {
    std::weak_ptr<ClientPluginInstance> instance;
    std::atomic<bool>                   alive{true};
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
    /// 执行回调 (异步操作契约: 动作 JSON 经它自己的 notify 交回, 可能稍后才回)
    AgentxxClientCommandExecuteFn execute = nullptr;
    void*                         ud      = nullptr;
};

/// 一次工具行渲染的输入 (主程序内部; 插件侧只有在生成函数的 ctx 里看到这些字段)
///
/// - 字符串全部只在本函数调用期间借用 (内容生成会立刻拷成 ctx JSON)
/// - 折叠 / 展开各请求一次, `part` 告诉生成函数这次要哪一部分
struct ClientToolRenderInput {
    std::string_view toolCallId;
    std::string_view toolName;
    std::string_view argsJson;
    std::string_view resultText;
    bool             finished = false;
    bool             error    = false;
    int              maxWidth = 0;
    bool             summary  = true; ///< true = 折叠头, false = 展开体
};

/// 主程序内置工具行的内容 (在 client io 线程同步执行; 不走插件 ABI)
///
/// 与插件内容的区别只在"谁来算": 插件内容走生成函数 (插件执行线程, 异步契约),
/// 内置工具行由主程序自己的函数当场算完 —— 两者都登记在 `agentxx.client.tool`
/// 槽位上, 命中与缓存走同一条路径。
struct ClientToolRowContent {
    /// 折叠头一行 (空串 = 本次没有内容, 走默认渲染)
    std::function<std::string(const ClientToolRenderInput&)> summary;
    /// 展开体块数组 JSON 文本 (空串 = 本次没有内容)
    std::function<std::string(const ClientToolRenderInput&)> body;
};

/// 工具行内容的渲染数据 (声明项 + 内容) ; 插件内容与内置工具行共用
///
/// - 插件内容: `generate` 是内容的生成函数, 在插件执行线程上跑, `lease`
///   保证卸载后不再进入插件代码
/// - 内置工具行: `row.summary` / `row.body` 由主程序在 client io 线程同步执行,
///   没有实例与租约
struct ClientToolContentBinding {
    ClientPluginManager*     manager = nullptr;
    std::string              plugin;      ///< 归属 (实例名 / [kBuiltinRendererOwner])
    std::string              toolName;    ///< 声明项 data 里的工具名
    std::string              displayName; ///< 折叠头显示名 (空 = 工具名)
    std::vector<std::string> deps;        ///< 内容声明的变量 (生成时只给这些)
    /// 插件内容的生成函数 (与 `PluginxxUiContentSpec::generate` 同一契约)
    void (PLUGINXX_CALL* generate)(
        const PluginxxHost*           host,
        const PluginxxStringView*     ctx_json,
        void*                         user_data,
        const PluginxxOperatorNotify* notify,
        PluginxxString*               error_out
    ) = nullptr;
    void*                    generateUd = nullptr;
    const PluginxxHost*      hostView   = nullptr; ///< 插件的主程序视图 (生成函数入参)
    ClientToolRowContent     row;                  ///< 内置工具行的同步内容 (空 = 插件内容)
    std::shared_ptr<ClientToolRendererLease> lease; ///< 实例租约 (卸载后不再渲染)
    std::atomic<bool>        active{false};

    /// 有折叠头内容可算
    bool hasSummary() const {
        return row.summary != nullptr || generate != nullptr;
    }

    /// 有展开体内容可算
    bool hasBody() const {
        return row.body != nullptr || generate != nullptr;
    }
};

/// 工具行渲染记录 (UI 注册表快照条目)
///
/// - **只有一份来源**: `agentxx.client.tool` 上注册的内容 (声明项 + 内容);
///   主程序内置工具行 (lib 内置工具没有对应插件) 也登记在同一张表里,
///   见 [ClientPluginManager::registerBuiltinToolContent]
/// - `displayName` 在注册时归一 (空 → 工具名), 因此命中项的显示名恒非空 ——
///   UI 线程同步读得到, 首帧即可显示
/// - 折叠头与展开体共用 [binding], 各自缓存
struct ClientToolRenderReg {
    std::string plugin;
    std::string toolName;
    std::string displayName; ///< 归一后恒非空
    /// 内容数据 (插件内容或内置工具行; 空 = 不可用)
    std::shared_ptr<ClientToolContentBinding> binding;
};

/// 主程序内置渲染器的保留归属名 (写入 [ClientToolRenderReg::plugin] 与语义渲染
/// 缓存条目, 非插件名): lib 内置工具 (如 agentxx_share_store/agentxx_subagent)
/// 没有对应插件, 其特化渲染由主程序自身注册
inline constexpr std::string_view kBuiltinRendererOwner = "agentxx.core";

/// 通用动作绑定记录 (UI 注册表快照条目; bind_action_handler 写入)
/// - targetId 空串 = 本实例的默认绑定 (方案 A fallback: 精确匹配优先, 未命中回落 "")
/// - 内容与绑定正交: 按钮可先渲染后绑定, 渲染时按快照有无绑定决定是否可点
struct ClientActionBinding {
    std::string       targetId; ///< 归属键 (section_id/panel_id/tool_call_id); "" = 默认绑定
    std::string       plugin;   ///< 所属插件名
    AgentxxUiActionFn cb = nullptr;
    void*             ud = nullptr;
};

/// 展示区域尺寸快照条目 (面板/Info 段落的可用宽高; UI 线程写, io 线程读)
struct ClientRegionSize {
    std::string id;         ///< 归属 id (面板 id / 段落 id)
    int         width  = 0; ///< 可用显示宽度 (列)
    int         height = 0; ///< 当前内容行数 (行)
};

/// 全局快捷键注册记录 (UI 注册表快照条目)
/// - 派发: UI 线程按"当前按键的规范化描述"查到本项后投递到 client io 线程执行回调
///   (UI 线程不进入插件代码); 同一键位只允许一个注册者 (先注册者优先)
struct ClientKeybind {
    std::string plugin;      ///< 所属插件名
    std::string keys;        ///< 规范化后的键位描述 (见 [normalizeKeybindSpec])
    std::string description; ///< 说明文本 (帮助/列表展示)
    void(PLUGINXX_CALL* handler)(void* ud) = nullptr;
    void* ud                               = nullptr;
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
/// 主程序与界面侧必须用同一种格式: 界面把按键事件转成同格式描述后按字符串比较
/// (见 TUI 侧 `keybindOfEvent`)。
std::string normalizeKeybindSpec(std::string_view keys);

/// UI 注册表快照 (UI 线程渲染读取; COW shared_ptr 语义)
struct ClientUiRegistry {
    std::vector<ClientCommand>     commands;
    std::vector<ClientKeybind>     keybinds;
    /// 快捷键冲突记录 (注册受阻于已占用的键位; 见 [ClientKeybindConflict])
    /// - 与 [keybinds] 同一 COW 快照: UI 线程一次快照读到"已注册"与"未抢到"两侧信息
    std::vector<ClientKeybindConflict> keybindConflicts;
    std::vector<ClientToolDecor>       toolDecors;
    /// 工具行内容 (插件内容与主程序内置工具行都在这里; 按 toolName 匹配)
    std::vector<ClientToolRenderReg>   toolRenderers;
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
/// [pluginxx::ui::ParseLimits]。
///
/// - `args`:
///     - [json] 待校验的 JSON 文本 (空串视为通过)
///     - [what] 描述用途 (日志用, 如 "push_ui_content")
///     - [plugin] 插件名 (日志用)
bool acceptUiJsonSize(std::string_view json, std::string_view what, std::string_view plugin);

/// 工具特化渲染的部分 (折叠内容 / 展开内容)
///
/// 两部分各有自己的缓存条目、输入特征与版本号: 折叠内容到达时只让折叠条失效,
/// 展开体不受影响; 反过来也一样。缓存键在 [ClientToolRenderRequest::keyFor] 里
/// 按部分追加后缀 (`#c` / `#e`)。
enum class ClientToolRenderPart : uint8_t {
    Collapsed = 0, ///< 折叠内容 (一行文本)
    Expanded  = 1, ///< 展开内容 (items)
};

/// 折叠内容查询结果 (UI 线程可调, 不进入插件代码)
struct ClientToolCollapsedResult {
    /// 折叠内容 (不含分隔符 `" · "`; 空 = 无内容)
    /// - `pending == false` 且为空: 没注册折叠内容回调, 或回调返回了空内容 → 走默认渲染
    /// - `pending == true`: 自定义内容还在 client io 线程算, 此处可能是上一份内容
    std::string text;
    /// 命中注册项但结果尚未算出 (调用方应提交一次渲染请求)
    bool        pending = false;
    std::string pendingPlugin; ///< `pending` 时的渲染器归属插件 (诊断)
    bool        isDecor = false; ///< `text` 来自 `update_tool_decor` 的 summary
};

/// 展开内容查询结果 (UI 线程可调; 只在展开态调用)
struct ClientToolExpandedResult {
    /// 展开内容 items (界面描述层 schema; 空 = 无内容, 走默认展开体)
    utilxx_base::Json items = utilxx_base::Json::array();
    bool              pending = false; ///< 同 [ClientToolCollapsedResult::pending]
    std::string       pendingPlugin;
    bool              isDecor = false; ///< `items` 来自 `update_tool_decor`
    /// decor 来源 (isDecor=true 时有效): 按钮派发 owner 统一为 tool_call_id,
    /// 插件 bind 一次永久生效; 非 decor 时为空
    std::string decorPlugin;
    std::string decorToolCallId;
};

/// 工具语义渲染结果条目 (主程序拥有; 写入后不再修改, UI 线程只读快照)
///
/// - 由 client io 线程在持有 renderer lease 时执行插件回调, 并把结果拷成主程序
///   文本/JSON 后写入
/// - 按部分只有一侧非空 (`text` 用于折叠部分, `items` 用于展开部分)
/// - 插件卸载/禁用/重载时按插件失效, 旧快照因此回退默认渲染
struct ClientToolRenderEntry {
    std::string       key;            ///< 缓存键 (含部分后缀, 见 keyFor)
    std::string       plugin;         ///< 产出该结果的插件名
    uint64_t          generation = 0; ///< 产出时的实例代次
    uint64_t          inputHash  = 0; ///< 输入特征 (args/结果/宽度等)
    bool              matched    = false; ///< 回调有返回内容 (false = 本次无自定义内容)
    std::string       text;            ///< 折叠内容 (折叠部分)
    utilxx_base::Json items = utilxx_base::Json::array(); ///< 展开内容 (展开部分)
};

/// 工具语义渲染请求 (UI 线程构造; 所有字段为拥有型拷贝, 不在 UI 线程进入插件代码)
struct ClientToolRenderRequest {
    std::string          toolCallId;
    std::string          toolName;
    std::string          argsJson;
    std::string          resultText;
    ClientToolRenderPart part       = ClientToolRenderPart::Collapsed;
    bool                 isFinished = false;
    bool                 isError    = false;
    int                  maxWidth   = 0;

    /// 缓存键: 优先按 tool_call_id (每次调用独立), 无 id 时退回 tool_name;
    /// 按部分追加后缀 (`#c` 折叠内容 / `#e` 展开内容)
    static std::string keyFor(
        std::string_view     toolCallId,
        std::string_view     toolName,
        ClientToolRenderPart part
    );

    /// 输入特征: 任一输入变化都必须重新渲染 (含宽度, 渲染器按宽度换行)
    uint64_t inputHash() const;

    /// 输入特征 (直接吃 string_view, 不做任何拷贝) —— 供"先算特征查缓存,
    /// 确认需要投递时才拷贝请求"的路径使用 (见 renderClientToolCollapsed / queryToolCollapsed):
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
/// - `lookup`/`version` 只读主程序拷贝, 不调用插件
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

    /// 取该键"最近一次写入"的条目, **不校验输入特征** (UI 线程; 只读主程序拷贝)
    ///
    /// 用途: 特化渲染结果尚未就绪时沿用该键上一次的结果 —— 同一调用取上一份
    /// 结果 (结果到达使输入特征变化的那一帧继续显示屏幕上已有的内容)。
    ///
    /// 注意: 只用于过渡展示; 输入不同 (参数/宽度/完成状态变化) 的条目不等于本次结果。
    std::shared_ptr<const ClientToolRenderEntry> lookupLatest(const std::string& key) const;

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

/// 查询行为规范 (折叠部分; UI 线程可调, 不进入插件代码):
/// 1. `toolDecors` 命中且 summary 非空 → 用它 (`isDecor = true`)
/// 2. 否则命中按 tool_name 注册的渲染器 (插件项优先于内置项) 且它给了折叠回调:
///    缓存命中 → 用条目内容 (`matched == false` 即回调返回空 → 默认渲染);
///    未命中 → `pending = true`, 内容取同调用同部分的上一份结果 (没有就为空)
/// 3. 否则 → 没注册 (`pending = false`, 内容为空) → 默认渲染
ClientToolCollapsedResult renderClientToolCollapsed(
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

/// 查询行为规范 (展开部分, 只在展开态调用; 判定顺序同折叠部分):
/// 1. `toolDecors` 命中且 items 非空 → 用它 (`isDecor = true`)
/// 2. 否则命中注册项且它给了展开回调: 缓存命中 → 用条目 items; 未命中 →
///    `pending = true` + 同调用同部分的上一份 items
/// 3. 否则 → 没注册 → 默认展开体 (参数 JSON + 运行中提示或结果文本)
ClientToolExpandedResult renderClientToolExpanded(
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

/// 工具行内容文档 → 折叠头那一行文本 (`blocks` 的纯文本首行)
///
/// 插件内容生成结果与内置工具行共用; 文档为空时返回空串 (界面走默认渲染)。
std::string toolRowCollapsedText(const utilxx_base::Json& doc);

/// 工具行内容文档 → 展开体块数组 JSON 文本 (`blocks` 数组 dumps; 没有块返回空串)
std::string toolRowExpandedJson(const utilxx_base::Json& doc);

/// 生效显示名: `update_tool_decor` 的 displayName > 注册表注册名
///
/// - 两者都空时返回空串 (调用方回退渲染消息里的原始工具名)
/// - `toolCallId` 为空时只查注册表 (占位行/中断分段没有调用 id)
/// - 显示名是哪一注册时确定的静态信息, 同步返回, 不投递渲染请求
/// - `return` 指向注册表快照内部 (快照存活期内有效)
std::string_view toolDisplayNameFor(
    const ClientUiRegistry* reg,
    std::string_view        toolCallId,
    std::string_view        toolName
);

/// 定时器主程序句柄实现 (仅主程序内部; 与
/// [client_plugin_api.h](/agent/lib/include/agentxx/plugin/api/client_plugin_api.h)
/// 的 `AgentxxTimer` 不透明类型对应)
///
/// - 计时器挂在 client io 执行器上 (asio::steady_timer); 到点后回调派发到插件执行线程
///   (默认就是这条 io 线程, 见 [startPluginThread])
/// - `alive` 为取消标记: 取消后即使有尚未返回的等待也不会再回调插件
/// - `repeat` 为剩余触发次数 (0 = 一次性), 周期定时器每次触发后递减
/// - `armedAt` 用于**同帧合并**: io 线程繁忙/事件积压导致多个周期同时到期时,
///   只回调一次并丢弃已错过的周期 (不追赶式连续回调)
struct ClientTimerImpl {
    asio::steady_timer timer;
    /// 归属实例 (弱引用: 回调前升级, 实例已卸载则丢弃本次触发)
    std::weak_ptr<ClientPluginInstance> inst{};
    std::string                         ownerId; ///< 关联展示区域 id (可空)
    int32_t intervalMs  = 50;                    ///< 触发间隔 (已按主程序下限限制)
    int32_t repeat      = 0;                     ///< 剩余触发次数 (0 = 一次性)
    bool    repeatMode  = false;                 ///< true = 周期 (按 repeat 计数)
    bool    pauseHidden = false;                 ///< 关联区域不可见时跳过回调
    /// 上次续期时刻 (判定"迟到"用; 见 [armTimer])
    std::chrono::steady_clock::time_point armedAt{};
    /// 上一次触发的回调是否还在执行 (插件执行线程上跑)
    ///
    /// 回调耗时超过 interval 时, io 线程按周期算出的一批到期**不排队追补**: 未返回前
    /// 的新到期直接跳过 (见 [fireTimerTick])。标志由插件线程写、io 线程读。
    std::shared_ptr<std::atomic<bool>> inFlight = std::make_shared<std::atomic<bool>>(false);
    /// 因同帧合并或回调未返回被丢弃的触发次数 (诊断用)
    uint64_t dropped               = 0;
    void(PLUGINXX_CALL* cb)(void*) = nullptr;
    void* ud                       = nullptr;
    bool  alive                    = true;

    explicit ClientTimerImpl(asio::any_io_executor ex) :
        timer(std::move(ex)) {}
};

/// client 插件实例 (主程序侧状态)
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
    /// 直连库路径为空) —— 主程序限制依据, 经 list() 暴露供展示/排查
    PluginManifestInterfaces interfaces;

    /// 清单声明的插件形态 (`kind`; 见 pluginxx/host/kind_registry.h)
    /// - 空 = 按 `native` 处理 (直连动态库路径 / 老清单没写 kind)
    /// - 不是 `native` = 由运行器插件解释执行, 本身没有动态库
    std::string kind;

    /// 运行器是否已接手这个实例 (只有 `kind != native` 会置真)
    /// 禁用 / 卸载时据此决定要不要先让运行器把脚本卸掉
    bool kindLoaded = false;

    /// 注册残留 (卸载时统一清理; 仅 io 线程)
    /// - disable 时【完整注册信息】(commandRegs) 保留, enable 可恢复;
    ///   仅 unload/进程销毁时随实例释放
    std::vector<ClientCommand> commandRegs; ///< 命令注册信息 (disable 保留)
    /// 全局快捷键注册信息 (同命令: disable 时随 UI 注册表摘除, enable 由 start 重声明)
    std::vector<ClientKeybind> keybindRegs;
    /// 工具消息装饰 (disable 保留, enable 恢复; 无句柄 —— 以 plugin+toolCallId 键控)
    std::vector<ClientToolDecor> toolDecorRegs;
    /// 通用动作绑定 (disable 保留, enable 恢复; 以 plugin+targetId 键控)
    std::vector<ClientActionBinding> actionRegs;
    /// 定时器 (client 自有 `agentxx.client.timer` 表): 实例级运行时资源
    /// - 与注册信息不同, 定时器**不跨 disable 保留**: 禁用/卸载时全部取消
    ///   (见 [ClientPluginManager::detachDomainRegistrations]), enable 后由插件
    ///   start 事务重新注册
    std::vector<std::shared_ptr<ClientTimerImpl>> timers;
    /// 全局快捷键句柄 (disable 期随注册表摘除, 句柄保活到实例析构)
    std::vector<std::shared_ptr<AgentxxKeybind>> keybindHandles;

    /// 管理器弱引用 (host vtable 回调取用)
    std::weak_ptr<ClientPluginManager> manager{};
    /// 与公共实例控制块保持一致，供 Client 裸指针回调升级 owner。
    std::weak_ptr<ClientPluginInstance> self{};

    explicit ClientPluginInstance(std::string in_name) :
        PluginInstanceBase(std::move(in_name)) {}

    /// 析构时 dlclose (与 agent 侧 PluginInstance 一致; 调用方保证无执行中回调:
    /// unloadAsync 等 inflight 归零后移除, shutdownAll 进程退出路径约定无执行中)
    ~ClientPluginInstance();


    /// 日志前缀 (与 agent 侧插件主程序同进程共存时区分来源)
    std::string_view logTag() const noexcept override {
        return "[client_plugin] ";
    }
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
///   client io 线程调用, 动作 JSON 经它的完成回调交回 (可立即或稍后), 主程序解析
///   并分发到 UI 适配器
class ClientPluginManager : public agentxx::agent::ClientEventSink,
                            public pluginxx::PluginHostLifecycle<ClientPluginInstance>,
                            public pluginxx::DomainServices,
                            public std::enable_shared_from_this<ClientPluginManager> {
public:

    /// 主程序向客户端插件提供的接口表数量 (文档里的数字有常量可校验)
    /// - 10 张通用表 (由插件框架实现: log/json/config/plugins/broadcasts/
    ///   scheduler/tasks/cancel/capabilities; 两侧同一套)
    /// - 6 张自有表 (`agentxx.client.*`: ui/session/wire/self/timer/keybind)
    /// - `net` 由本主程序实现, 单独一张 (`agentxx.net`), 不计入上面两组
    /// - 增删接口表时同时更新本常量与 `docs/zh-cn/design/plugins.md` §9 与根 `AGENTS.md`
    ///   (测试模块 `boundaries` 会校验三者一致)
    inline static constexpr size_t kInterfaceTableCount = 10 + 6;

    struct PluginListView {
        std::string              name;
        std::string              version;
        std::string              description;
        std::string              path;
        std::string              configPath;
        bool                     enabled  = true;
        size_t                   inflight = 0;
        std::vector<std::string> commands;
        std::vector<std::string> depends;
        std::vector<std::string> optionalDepends;
        /// 接口声明 (plugin.yaml `interfaces`; 空 = 未声明)
        std::vector<std::string> requiredInterfaces;
        std::vector<std::string> optionalInterfaces;
    };

    /// 工作池默认线程数 (`plugin.thread_pool_workers` 未配时用它)
    static constexpr size_t kDefaultWorkerThreads = 3;

    /// - `ex`: client io 执行器 (全部状态变更在这条线上)
    /// - `workerThreads`: 工作池线程数 (0 = [kDefaultWorkerThreads])
    explicit ClientPluginManager(asio::any_io_executor ex, size_t workerThreads = 0);
    ~ClientPluginManager();

    ClientPluginManager(const ClientPluginManager&)            = delete;
    ClientPluginManager& operator=(const ClientPluginManager&) = delete;

    // ==================== 加载 ====================

    /// 注入 UI 适配器 (UI 无关语义层 → 具体 UI 实现; 模式启动时调用一次)
    void setUiAdapter(std::shared_ptr<PluginUiAdapter> adapter);

    /// 当前 UI 适配器 (supportedInterfaces() 声明主程序接口集; 任意线程, 加载后不可变)
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

    /// 加载一个由运行器插件执行的形态 (`kind` 不是 `native`)
    ///
    /// - 没有动态库: 解析清单后建一个没有生命周期入口的实例, 再让运行器接手;
    /// - 清单里的 `kind` 必须能在形态表里找到, 且该形态由已加载的插件提供
    ///   (没装运行器时报"缺运行器", 不会崩);
    /// - 运行器拿到的是**目标实例的主程序视图**作为调用方, 脚本里的注册因此挂在
    ///   目标插件上, 停用 / 卸载它时整批撤销;
    /// - 加载失败时回滚 (实例出表 + 释放名称), 与动态库插件同一条路径。
    asio::awaitable<std::shared_ptr<ClientPluginInstance>> loadInterpretedAsync(
        std::string                         path,
        std::string                         kind,
        const agentxx::agent::PluginConfig* cfg = nullptr
    );

    /// 让运行器加载 / 卸载一个实例的脚本 (载荷与回答见 `pluginxx/api/kinds.h`)
    asio::awaitable<bool> startInterpretedInstance(
        const std::shared_ptr<ClientPluginInstance>& inst,
        std::string&                                 reason
    );
    asio::awaitable<bool>
        stopInterpretedInstance(const std::shared_ptr<ClientPluginInstance>& inst);

    /// 调一次运行器插件的能力, 等它的回答
    /// - `caller` 作为调用方实例 (运行器据此把脚本注册挂到目标插件上);
    /// - 返回 `{ok, 文本}`; 未受理 / 超时 / 能力失败都算 ok=false, 文本给原因。
    asio::awaitable<std::pair<bool, std::string>> invokeKindCapability(
        ClientPluginInstance* caller,
        std::string           capability,
        std::string           argsJson,
        uint32_t              timeoutMs
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

    /// 主程序当前支持的接口名集合 (由 uiAdapter->supportedInterfaces() 声明;
    /// io 线程; 限制检查与 EVT_READY / get_client_state 的
    /// interfaces 数组共用本结果 —— 单一事实来源)
    InterfaceSet hostSupportedInterfaces() const;

    // ==================== UI 注册表 (任意线程) ====================

    /// 注册表快照 (短锁拷贝 shared_ptr; UI 线程渲染无锁读取)
    std::shared_ptr<const ClientUiRegistry> uiRegistrySnapshot() const;

    /// 工具语义渲染缓存 (任意线程可读; 内部短锁)。UI 线程只读其中的主程序拷贝,
    /// 自定义 renderer 的插件回调在插件执行线程上跑 (默认 = client io 线程,
    /// 见 [startPluginThread])。
    std::shared_ptr<ClientToolRenderCache> toolRenderCache() const {
        return toolRenderCache_;
    }

    // ==================== 声明式界面 (io 线程写, 界面线程读快照) ====================
    //
    // 与 agent 侧共用共享库的声明式界面规则 (`pluginxx/api/ui.h` + `host/ui_content.h`):
    // 插件先登记声明项 (只带数据), 再给同一 id 登记一份内容 (generate 函数按变量现算)
    // 或直接 push 一份。客户端的槽位只有 status / panel / info 三个分类, 具体是谁
    // 由内容 id 表达 (见 `client_ui_content.h`)。
    //
    // 界面线程只读 [uiContentSnapshot]: 渲染不跑插件代码, 也没有任何同步等待;
    // 内容到得晚时界面先用上一份, 没有就画主程序自己的默认界面。

    /// 声明式界面快照 (任意线程)
    std::shared_ptr<const ClientUiSnapshot> uiContentSnapshot() const;

    /// 声明项 / 内容 / 槽位的注册入口 (io 线程; 返回 `PLUGINXX_RESULT_*`)
    int32_t registerUiEntry(ClientPluginInstance* inst, const PluginxxUiEntrySpec* spec);
    int32_t unregisterUiEntry(ClientPluginInstance* inst, std::string_view itemId);
    std::string listUiEntries(ClientPluginInstance* inst) const;
    int32_t registerUiContent(ClientPluginInstance* inst, const PluginxxUiContentSpec* spec);
    int32_t unregisterUiContent(ClientPluginInstance* inst, std::string_view id);
    int32_t
        pushUiContent(ClientPluginInstance* inst, std::string_view id, std::string_view contentJson);
    std::string listUiContents(ClientPluginInstance* inst) const;
    int32_t
        updateUiSlot(ClientPluginInstance* inst, std::string_view slot, std::string_view idsJson);
    int32_t defineUiSlot(ClientPluginInstance* inst, const PluginxxUiSlotSpec* spec);
    int32_t undefineUiSlot(ClientPluginInstance* inst, std::string_view slot);
    std::string listUiSlots(ClientPluginInstance* inst) const;

    /// 取工具语义渲染结果 (UI 线程):
    /// - 命中缓存 (同输入特征) → 返回主程序拥有的结果, 不进入插件代码
    /// - 未命中 → 拷贝输入并派发到插件执行线程执行自定义 renderer, 本次返回
    ///   nullptr (调用方用通用回退); 结果到达后经 uiAdapter->onToolRenderUpdated
    ///   通知 UI 重建该消息块
    std::shared_ptr<const ClientToolRenderEntry>
        requestToolRender(const ClientToolRenderRequest& req);

    /// 该键的这次输入特征是否已有渲染请求在执行 (UI 线程; 零拷贝)。
    /// UI 侧查询工具渲染时先问一次: 请求尚未返回时本帧用通用回退, 不必为投递
    /// 拷贝 args/result 大文本 (见 message_list 的 queryToolCollapsed / queryToolExpanded)
    bool toolRenderInFlight(std::string_view key, uint64_t inputHash) const;

    /// 命令是否存在 (UI 线程判断是否拦截 "/" 输入; 短锁)
    bool hasCommand(std::string_view name) const;

    /// 投递命令调用到 client io 线程执行 (任意线程):
    /// - io 线程: 查表 → InflightGuard → execute 回调 → (结果经它的 notify 交回,
    ///   可能立即也可能稍后) 解析动作 JSON → 分发到 UI 适配器
    ///   (send → adapter->sendPluginMessage; toast → adapter->onToast;
    ///   none/非法 → 记日志)
    void postCommandInvocation(std::string name, std::string argsJson);

    /// 同步执行命令 (仅 io 线程; CLI 模式输入循环直接调用, 等价于
    /// postCommandInvocation 的 io 线程路径)
    void invokeCommand(const std::string& name, const std::string& argsJson);

    // ==================== 会话上下文 (io 线程) ====================

    /// 当前 client 状态 JSON (get_client_state 数据源):
    /// {"sessionId","connState","startupProgress","interfaces":[...],
    ///  "ui":{...},"regions":[{"id","w","h"},...],
    ///  "agentPlugins":[{"name","version","interfaces":[...]},...]}
    /// - ui: 界面能力段 (描述层; 适配器未发布时无该字段, 见
    ///   [PluginUiAdapter::uiCapabilitiesJson])
    /// - agentPlugins: 服务端已加载的 agent 侧插件结构化列表 (来自主程序约定
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
    // 以下为 vtable 实现的强类型入口, 须 io 线程调用 (vtable 入口经
    // `startTableOp` 受理, 工作体在本管理器 io 线程执行)

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

    /// 工具行内容 (Tool Renderer)
    ///
    /// 注册主程序内置工具行内容 (非插件; lib 内置工具无对应插件, 由主程序自身声明)
    /// - 与插件内容同一份存储: `agentxx.client.tool` 槽位上的一条声明项 + 内容,
    ///   归属名固定为 [kBuiltinRendererOwner]; 匹配优先级低于插件内容 (插件可覆盖)
    /// - 生成函数 (summary / body) 在 client io 线程同步执行, 不走插件 ABI;
    ///   无插件实例/租约语义: 不受插件禁用/卸载影响
    /// - `displayName` 空 → 用 `toolName`; 两个函数都可空 (= 只改名)
    /// - 任意线程可调 (uiMutex_ 保护); 同一 tool_name 重复注册覆盖旧项
    /// 返回 0 成功
    int registerBuiltinToolContent(
        std::string_view    toolName,
        std::string_view    displayName,
        ClientToolRowContent content
    );

    /// 绑定动作处理器 (io 线程; cb 空则失败; 同 (plugin,targetId) 覆盖)
    /// - targetId 空串 = 本实例的默认绑定 (方案 A fallback)
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
    // 用途: 面板/Info 段落由主程序布局, 插件拿不到可用宽度; 主程序在每次布局后把各归属的
    // 可用宽高记入快照, 值变化时向 `agentxx.client.ui_layout` 频道发布一条通知,
    // 并把它放进 get_client_state().regions 供按需查询 (SDK: ClientPluginBase::regionSize)。

    /// 上报某归属的可用尺寸 (UI 线程调用; 值未变化时不做任何事)
    /// - 首次上报总是会发布一条通知 (当前值从"未知"变为已知)
    /// - 发布投递到 io 线程执行 (订阅回调的线程约定不变)
    void reportRegionSize(const std::string& id, int width, int height);

    /// 当前区域尺寸快照 (任意线程; 按 id 升序)
    std::vector<ClientRegionSize> regionSizes() const;

    /// 注册命令; 返回 0 成功 (名字冲突返回非 0)
    int registerCommand(
        ClientPluginInstance*        inst,
        PluginxxStringView           name,
        PluginxxStringView           description,
        AgentxxClientCommandExecuteFn exec,
        void*                        ud
    );

    int registerCommand(
        ClientPluginInstance*        inst,
        std::string_view             name,
        std::string_view             description,
        AgentxxClientCommandExecuteFn exec,
        void*                        ud
    ) {
        return registerCommand(inst, strToSv(name), strToSv(description), exec, ud);
    }

    int unregisterCommand(ClientPluginInstance* inst, PluginxxStringView name);

    int unregisterCommand(ClientPluginInstance* inst, std::string_view name) {
        return unregisterCommand(inst, strToSv(name));
    }

    /// 广播注册表 (插件的通知面: 客户端频道 + 跨端插件数据 + 网络事件)
    /// - 实现在插件框架的 `PluginHostCore` (与本平台管理器同一份; 构造时就建好)
    ///   见 [pluginxx::PluginHostCore::broadcasts]

    /// 确保频道已声明 (幂等; 已声明时保留原声明)
    void ensureClientBroadcast(
        std::string_view           id,
        pluginxx::BroadcastCache_e cache         = pluginxx::BroadcastCache_e::Latest,
        bool                       anyonePublish = false,
        bool                       pushOnSubscribe = false
    );

    /// 发一条客户端频道值 (主程序代发; 失败只记日志)
    void publishClientChannelValue(std::string_view id, std::string_view payloadJson);

    /// 插件订阅广播的入口 (表入口的落点; **遮蔽基类同名方法**)
    ///
    /// 只多做一件事: 频道本端还没人声明时先补一条声明 —— 客户端频道跨进程
    /// 使用 (`plugin.<插件名>.*` / `agentxx.client.plugin.<插件名>.*` 常常由另一端
    /// 声明, 本进程看不到), 不补声明则插件在 start 里的订阅会被拒。
    PluginxxSubscription* subscribeBroadcast(
        ClientPluginInstance*    inst,
        const PluginxxScopeRef*  scope,
        std::string_view         broadcastId,
        void(PLUGINXX_CALL* handler)(const PluginxxStringView* payloadJson, void* ud),
        void*                    ud,
        std::string_view         optionsJson  = {},
        void(PLUGINXX_CALL* onEnd)(const PluginxxStringView* reasonJson, void* ud) = nullptr
    );

    // ==================== 定时器 (`agentxx.client.timer`, io 线程) ====================
    //
    // 插件侧定时器只能经主程序驱动: 定时器挂在 client io 执行器上, 到点后回调经
    // `dispatchToPlugin` 投到插件执行线程 (默认也是这条 io 线程; 插件代码永远
    // 不在 UI 线程运行)。门控: 主程序动画等级为
    // Disabled 时拒绝注册; 关联区域不可见的定时器可声明 pause_when_hidden 跳过回调。

    /// 注册定时器 (io 线程); 返回主程序句柄 (nullptr = 校验失败/动画关闭/超上限)
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

    /// 动画等级门控 (加载方在启动与设置变化时调用; 默认 true = 允许定时器)
    /// - false: 新的 `set_timer` 一律失败 (插件据返回值降级为静态展示)
    void setAnimationEnabled(bool enabled) {
        animationEnabled_.store(enabled, std::memory_order_relaxed);
    }

    bool animationEnabled() const {
        return animationEnabled_.load(std::memory_order_relaxed);
    }

    // ==================== 全局快捷键 (`agentxx.client.keybind`) ====================

    /// 注册快捷键 (io 线程); 返回主程序句柄 (nullptr = 键位非法/已被占用/超上限)
    AgentxxKeybind* registerKeybind(ClientPluginInstance* inst, const AgentxxKeybindSpec* spec);
    /// 注销快捷键 (io 线程; 空句柄/已注销忽略)
    void unregisterKeybind(ClientPluginInstance* inst, AgentxxKeybind* bind);
    /// 快捷键是否已注册 (UI 线程判断是否拦截按键; 短锁)
    bool hasKeybind(std::string_view keys) const;
    /// 当前快捷键列表 (任意线程; 按键位升序)
    std::vector<ClientKeybind> keybinds() const;
    /// 投递快捷键触发到 client io 线程执行 (任意线程; 未注册/插件禁用时忽略)
    /// - keys 须为 [normalizeKeybindSpec] 的格式 (界面把按键事件转成该格式)
    void postKeybindInvocation(std::string keys);

    /// 自描述
    std::string getOwnInfoJson(ClientPluginInstance* inst);
    std::string getPluginArgsJson(ClientPluginInstance* inst);
    std::string getPluginConfigPath(ClientPluginInstance* inst);

    std::string getLanguage() const {
        return language_.empty() ? "en" : language_;
    }

    /// 指定语言 (规范化后保存; 侧边栏与插件提示词按它选语言)
    /// - 同时覆盖 [pluginxx::DomainServices::setLanguage] (通用 config 表的写入口)
    void setLanguage(std::string_view lang) override {
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
    /// - topic 必须是全名 `agentxx.client.plugin.<名字>.<事件>` (不限定插件名, 建议用自己的)
    int sendPluginDataToPeer(
        ClientPluginInstance* inst,
        PluginxxStringView    topic,
        PluginxxStringView    json
    );

    int sendPluginDataToPeer(
        ClientPluginInstance* inst,
        std::string_view      topic,
        std::string_view      json
    ) {
        return sendPluginDataToPeer(inst, strToSv(topic), strToSv(json));
    }

    /// 主程序 vtable (静态函数表 `g_clientHostVtable`; 覆写 lifecycle 骨架的同名扩展点)
    const PluginxxHostVtable* hostVtable() override;

protected:

    // =====================================================================
    // pluginxx::PluginHostLifecycle 的扩展点 (由本类覆写)
    // =====================================================================
    //
    // 加载以外的生命周期骨架 (启停/禁用启用/卸载/级联依赖/关闭等待/destroy)
    // 与主程序业务无关, 已下沉到 cxx_pluginxx
    // (见 pluginxx/host/lifecycle.h); 插件框架不认识 client 侧的 UI 注册表,
    // 因此这些业务动作经下列覆写注入。
    //
    // 注意: 本类**不**复用加载骨架 —— client 侧的加载有独立语义 (dlopen 卸载到
    // 内部线程池执行、接口协商限制、agent/client 双入口探测), 见
    // [loadNativeAsync]。

    /// 管理器自引用 (骨架的异步事务与空闲收尾需要在此期间保活管理器)
    std::shared_ptr<pluginxx::PluginHostLifecycle<ClientPluginInstance>> selfRef() override {
        return shared_from_this();
    }

    /// 生成 client 侧实例对象 (业务自引用/管理器弱引用)
    /// - 元信息/lifecycle 入口/生命周期控制块/主程序控制块由 [attachInstance] 加载
    std::shared_ptr<ClientPluginInstance> createInstance(std::string name) override;

    /// client 侧插件入口符号名 (插件框架不硬编码主程序专名, 见 pluginxx/api/entry.h)
    ///
    /// 本类自带加载实现 (见上方说明), 但仍覆写本扩展点: 一来与插件框架默认保持一处定义,
    /// 二来 [AGENTXX_CLIENT_PLUGIN_SYMBOL_*] 与插件导出宏共用同一批字符串常量。
    pluginxx::PluginEntrySymbols entrySymbols() const override {
        return {
            AGENTXX_CLIENT_PLUGIN_SYMBOL_GET_INFO,
            AGENTXX_CLIENT_PLUGIN_SYMBOL_CREATE,
            AGENTXX_CLIENT_PLUGIN_SYMBOL_STOP,
            AGENTXX_CLIENT_PLUGIN_SYMBOL_DESTROY,
        };
    }

    /// client 侧的 API 版本 (与 agent 侧不同代: 本端自己抬代次, 两端各算各的)
    int32_t hostApiVersion() const noexcept override {
        return AGENTXX_CLIENT_PLUGIN_API_VERSION;
    }

    /// 日志前缀 (与 agent 侧插件主程序同进程共存时区分来源)
    std::string_view logTag() const noexcept override {
        return "[client_plugin] ";
    }

    /// 摘除实例的业务注册: UI 注册表 (状态栏项/面板/Info 段/命令/装饰/渲染器/
    /// 动作绑定) + 广播订阅 (由框架按归属回收) + adapter 通知 + 语义渲染缓存失效
    void detachDomainRegistrations(ClientPluginInstance* inst) override;

    /// 实例加载完成: 登记实例代次 (UI 点击据此复查, 防旧点击转交同名新实例)
    void onInstanceLoaded(ClientPluginInstance& inst) override;

    /// 实例从插件表摘除: 清除实例代次登记
    void onInstanceUnloaded(ClientPluginInstance& inst) override;

    /// 停用收尾: 解释执行的形态要先把脚本停掉 (脚本自己的计时器 / 回调一并回收),
    /// 再启用时由 [onInstanceEnabledAsync] 重新交给运行器加载
    asio::awaitable<void>
        onInstanceDisabledAsync(const std::shared_ptr<ClientPluginInstance>& inst) override;

    /// 卸载开始后 (插件 stop/destroy 之前): 同样先把脚本停掉 —— 此刻实例还是
    /// 正常状态, 运行器仍能代表它做卸载
    asio::awaitable<void>
        onInstanceUnloadingAsync(const std::shared_ptr<ClientPluginInstance>& inst) override;

    /// 启用收尾: 把脚本重新交给运行器加载 (脚本 start 里重新声明注册)
    asio::awaitable<void>
        onInstanceEnabledAsync(const std::shared_ptr<ClientPluginInstance>& inst) override;

    /// 卸载级联只统计"启用中"的依赖者 (client 侧既有行为: 已禁用的依赖者保持加载)
    bool cascadeUnloadEnabledOnly() const noexcept override {
        return true;
    }

private:

    friend class ClientPluginInstance;

    /// 插件执行线程 (所有 client 插件回调在这条线上串行执行)
    ///
    /// - **声明在最前**: 成员逆序析构, 它的 stop() 最后跑 —— 实例表/注册表/线程池
    ///   都已拆完, 线程才停;
    /// - **构造即起** (没有 client 插件也建有线程);
    /// - 从插件线程自身销毁管理器时 `stop()` 会 detach 而不 join (见
    ///   [pluginxx::PluginExecutorThread::stop])。
    pluginxx::PluginExecutorThread pluginThread_;

    /// 发一条客户端频道值 (io 线程; 幂等声明 + 发布, 失败只记日志)
    void publishClientChannel(std::string_view channel, const std::string& payloadJson);

    /// 声明客户端频道 (`cache: latest` / 未缓存; 在构造函数里调一次; 幂等)
    void declareClientChannels();

    /// 按频道名的形态补声明 (**幂等**; 已声明时保持原声明):
    /// - `agentxx.client.plugin.<插件名>.<事件>`: 属主是该插件, 两端都可发, 不缓存
    /// - `plugin.<插件名>.<名字>`: 属主是该插件, 只有属主可发, `cache: latest`
    /// - 其他 (`agentxx.client.*` 等应用频道): `cache: latest`, 任何人可发
    ///
    /// 用于跨进程频道 (另一端声明, 本进程看不到) 与主程序代发的网络事件。
    void ensureClientChannelDeclared(std::string_view id);

    /// 命令动作解析与分发 (io 线程; execute 已返回):
    /// {"action":"send","text"} → adapter->sendPluginMessage
    /// {"action":"toast","text","level"} → adapter->onToast
    /// {"action":"none"}/{}/非法 → 记日志
    void dispatchCommandAction(const std::string& actionJson);

    /// 执行一次工具语义渲染 (仅 client io 线程; 见 [requestToolRender]):
    /// 从当前注册表取自定义渲染器 (插件注册项优先, 未命中再查主程序内置项),
    /// 插件渲染器需复查 lease/实例状态后代次, 持 lease 调用插件回调;
    /// 主程序内置渲染器 (无插件归属) 直接调用。按请求的 `part` 只调对应部分的
    /// 回调, 结果到达 (回调内立即完成或稍后) 时拷成主程序文本/JSON 写入缓存
    /// 并通知 UI 重绘。
    void performToolRender(ClientToolRenderRequest req, std::string key, uint64_t inputHash);

    /// 登记/清除插件实例代次 (io 线程; 供 UI 点击携带与复查; 见
    /// [ClientUiRegistry::instanceGenerations])
    void setRegistryGeneration(std::string_view plugin, uint64_t generation, bool present);

    /// 重建声明式界面快照 (io 线程; 注册表改动后调一次)
    void publishUiContentSnapshot();

    /// 一个变量的当前值 (JSON 文本; 未知变量给 `null`)
    /// - `scope` 非空时给工具行变量 (这次调用的数据)
    std::string clientUiVariableJson(
        std::string_view         id,
        const ClientUiToolScope* scope = nullptr
    ) const;

    /// 让某个内容按当前变量生成一次 (io 线程; 投递到插件执行线程, 界面不等)
    ///
    /// - `scope` 非空时: 工具行变量取这次调用的值 (同一个内容可按调用各生成一份)
    /// - `onResult` 非空时: 结果在本进程 io 线程交给它, 不写这个内容自己的最近结果
    ///   (工具行走这条: 结果进按调用分键的渲染缓存) ; 此时 `dedupe` 由调用方决定
    void requestUiContent(
        std::string_view   fullId,
        const ClientUiToolScope* scope = nullptr,
        std::function<void(int32_t, std::string_view)> onResult = {},
        bool               dedupe = true
    );

    /// 一次生成结束 (io 线程): 写注册表 + 推快照 + 通知适配器
    void finishUiGeneration(std::string fullId, std::string json, std::string error);

    /// 停用 / 卸载: 声明项、内容、自建槽位一起撤销 (io 线程)
    void detachUiContent(std::string_view instanceName);

    /// 工具行内容 → 按调用渲染的适配数据
    ///
    /// 插件向 `agentxx.client.tool` 注册一份内容后, 主程序把它接成一条“按工具名”的
    /// 渲染器 (`ClientToolRenderReg`): 每次工具调用各生成一份, 结果仍进按 (调用,
    /// 折叠/展开) 分键的缓存。内容撤销后**不释放**它 (已投递的生成回调还会读),
    /// 只把 `active` 置假; 同一个全名再注册时复用同一份。
    /// - `inst` 为空 = 主程序内置工具行 (归属 `agentxx.core`, 无实例与租约)

    /// 内容注册 / 变更后同步工具行渲染器 (io 线程; 非 `agentxx.client.tool` 槽位直接返回)
    void syncToolContentRenderer(ClientPluginInstance* inst, std::string_view fullId);

    /// 内容撤销时摘下工具行渲染器 (保留适配数据; io 线程)
    void removeToolContentRenderer(std::string_view fullId);

    /// 工具行内容的生成上下文 (只放内容声明的变量; 工具变量来自这次调用)
    std::string toolContentContextJson(
        const ClientToolContentBinding& binding,
        const ClientUiToolScope&        scope
    ) const;

    /// 插件内容的生成入口 (投到插件执行线程后调用; 结果经 [toolContentDone] 交回)
    static int32_t toolContentRender(
        void*                         user_data,
        const ClientToolRenderInput*  input,
        const PluginxxOperatorNotify* notify,
        PluginxxString*               error_out,
        bool                          summary
    );
    /// 生成结果 → 渲染回调结果 (折叠头取纯文本一行, 展开体取 `blocks` 数组)
    static void PLUGINXX_CALL toolContentDone(
        void*                     user_data,
        int32_t                   status,
        const PluginxxStringView* payload
    );

    /// 本管理器唯一的工作池 (构造即建; dlopen/dlclose、插件阻塞工作共用)
    ///
    /// 与 agent 侧同一模型: 一个端只有一个池, 数量由 `plugin.thread_pool_workers`
    /// 决定 (`shutdownAll` 时 join)。
    std::shared_ptr<asio::thread_pool> workerPool_;

    std::shared_ptr<PluginUiAdapter> uiAdapter_; ///< 注入后不可变 (io 线程读写)

    /// 客户端界面能力段 (加载时从适配器读取一次; 见
    /// [PluginUiAdapter::uiCapabilitiesJson]) —— `get_client_state()` 的 `ui` 段,
    /// 空对象 = 该客户端不发布 (插件按描述层全支持处理)
    utilxx_base::Json uiCapabilitiesJson_ = utilxx_base::Json::object();

    /// 工具消息装饰版本号序列 (io 线程递增; 计入 ClientToolDecor.version,
    /// 供 TUI 块缓存 key 知道装饰更新)
    uint64_t toolDecorVersionSeq_ = 1;

    /// UI 注册表 (COW: io 线程写, 任意线程快照读)
    mutable std::mutex                      uiMutex_;
    std::shared_ptr<const ClientUiRegistry> uiRegistry_;

    /// 工具语义渲染缓存 (client io 线程写, UI 线程读; 见
    /// [ClientToolRenderCache])。自定义渲染器的插件回调只在 client io
    /// 线程执行, UI 线程只处理这里已经拷成主程序对象的语义结果。
    /// 条目按部分分条 (`#c` 折叠内容 / `#e` 展开内容), 各自版本号与输入特征独立。
    std::shared_ptr<ClientToolRenderCache> toolRenderCache_
        = std::make_shared<ClientToolRenderCache>();

    /// 会话上下文 (io 线程)
    std::string sessionId_ = "session";
    std::string connState_ = "connecting";
    std::string startupProgress_;

    /// 服务端已加载的 agent 侧插件结构化信息 (io 线程写读): 来自主程序约定
    /// 事件 `agentxx.agent.host.server_plugins` (WirePluginData; 载荷
    /// [{"name","version","interfaces":[...]},...]); 空数组 = 未知 (服务端
    /// 未提供)。client 插件经 get_client_state("agentPlugins") 查询对端
    /// 可用性与声明的接口, 对端缺失时可降级提示, 避免上行数据被静默丢弃
    /// 的"操作成功"假象
    struct ServerPluginInfo {
        std::string              name;
        std::string              version;
        std::vector<std::string> interfaces; ///< 该插件声明的接口 (require∪optional)
    };

    /// 服务端已加载的 agent 侧插件 (空 = 服务端未提供)
    std::vector<ServerPluginInfo> serverPlugins_;

    /// PLUGIN_DATA 无订阅者警告去重 (仅 io 线程; 每插件名只警告一次)
    /// - 只看插件族主题 (`plugin.<插件名>.*`): 收到但本端没有任何 client 插件
    ///   订阅时, 多半是对端插件未在本地加载 —— 提示一次便于排查, 不随事件频率刷屏;
    ///   主程序约定事件 (`agentxx.agent.host.*`) 不属于任何插件, 不进这里也不提示
    std::set<std::string> pluginDataNoSubWarned_;

    /// 因接口要求未满足被跳过的插件 (io 线程; 见 skippedPlugins())
    std::map<std::string, std::string> skippedPlugins_{};

    /// 展示区域尺寸快照 (UI 线程写, 任意线程读; 见 reportRegionSize)
    mutable std::mutex                                   regionMutex_;
    std::map<std::string, ClientRegionSize, std::less<>> regionSizes_;

    /// 声明式界面注册表 (client io 线程写; 共享库提供存储 / 快照 / 版本号 / 摘除)
    pluginxx::host::UiRegistry     uiContent_;
    pluginxx::host::UiSlotCatalog  uiSlotCatalog_;
    /// 界面线程读的快照 (io 线程重建, 短锁拷贝 shared_ptr)
    mutable std::mutex                      uiContentMutex_;
    std::shared_ptr<const ClientUiSnapshot> uiContentSnapshot_;
    /// 正在生成的内容 (全 id; 同一内容同时只跑一次)
    std::set<std::string> uiGenerating_;

    /// 工具行内容的适配数据 (全 id → 绑定; 内容撤销后保留并置 `active=false`,
    /// 因为已投递的生成回调还会读它; 同一全名再注册时复用)
    std::map<std::string, std::shared_ptr<ClientToolContentBinding>, std::less<>>
        toolContentBindings_;

    /// 主程序内置工具行的同步内容 (全 id → 内容; 进程生命周期, 不随插件卸载变化)
    std::map<std::string, ClientToolRowContent, std::less<>> builtinToolRows_;

    /// 展示区域可见性快照 (UI 线程写, 任意线程读; 见 reportRegionVisible)
    /// - 供 `pause_when_hidden` 定时器门控与 `agentxx.client.timer` 的 is_visible 使用
    std::map<std::string, bool, std::less<>> regionVisibility_;

    /// 动画等级门控 (加载方经 setAnimationEnabled 更新; 默认允许定时器)
    std::atomic<bool> animationEnabled_{true};

    std::string language_ = "en";

public:

    // =====================================================================
    // 网络能力 (`<应用名>.net` 表; 实现在通用网络服务 utilxx::NetHub)
    // =====================================================================
    //
    // 与 agent 侧同一个 `PluginNetService`, 属主 id = client 插件实例名。
    // 客户端目前还没有广播表 (见 `docs/zh-cn/design/plugins.md`), 所以
    // `plugin.<插件 id>.net` 频道还不存在: **网络事件暂时只记调试日志**,
    // `http.fetch` / `http.download` / `http.serve` 这类按请求/服务的工作正常。
    // 客户端的广播表与事件发布在 client 侧广播改造时接上 (同一个 hook 点)。

    /// 网络服务 (懒创建; 首个网络调用时起网络线程)
    std::shared_ptr<PluginNetService> netService();

    /// 同步网络操作 (返回插件面结果码 `PLUGINXX_RESULT_*` 通用段)
    int32_t netCall(
        std::string_view               owner,
        std::string_view               op,
        std::string_view               argsJson,
        std::string*                   outJson,
        std::weak_ptr<ClientPluginInstance> guard = {}
    );

    /// 异步网络操作 (受理成功写 requestId, 结果恰好一次经 cb 给出)
    int32_t netCallAsync(
        std::string_view               owner,
        std::string_view               op,
        std::string_view               argsJson,
        PluginxxOperatorCallback       cb,
        void*                          ud,
        std::string*                   outJson,
        int64_t*                       outRequestId,
        std::weak_ptr<ClientPluginInstance> guard = {}
    );

    /// 取消一个进行中的网络请求
    int32_t netCancel(std::string_view owner, int64_t requestId, std::string* outJson);

    /// 收掉某个插件的全部网络资源 (禁用/卸载时调; 未完成请求按"已取消"结算)
    void netShutdownPlugin(std::string_view owner);

    /// 停掉网络服务 (管理器关闭时)
    void netShutdownAll();

    /// 网络事件频道 (`plugin.<插件 id>.net`)
    void publishNetEvent(std::string_view owner, std::string_view payloadJson);

    // ==================== pluginxx::DomainServices ====================
    //
    // 通用表 (log/json/config/plugins/broadcasts/scheduler/
    // tasks/cancel/capabilities) 的实现位于 cxx_pluginxx；其中需要主程序数据的
    // 入口经本接口取数。与 agent 侧 `PluginManager` 同一套约定，数据源是 client
    // 自己的配置/插件清单。

    /// 把一段阻塞工作投递到客户端插件工作线程池
    bool postToWorkerThread(std::function<void()> fn) override;

    /// 加载线程: dlopen / create 不占客户端 io 线程
    ///
    /// 插件 `create` 是构造期 (允许读配置/建索引), 它跑在插件执行线程之外 —— 客户端侧
    /// 没有自带线程池的嵌入方 (测试) 会在调用线程上直接执行。
    asio::awaitable<std::string> runLoaderTask(std::function<std::string()> fn) override;

    /// 主程序配置 JSON (`get_config` 的业务部分)
    std::string configJson() override;

    /// 当前语言代码 (空串由调用方回退为 "en")
    std::string language() override;

    /// 指定语言 (就地写入; 覆盖 [pluginxx::DomainServices::setLanguage])
    /// 具体实现见上方 [setLanguage]

    /// 全部 client 插件 JSON 数组 (`list_plugins`)
    std::string pluginsJson() override;

    /// 单个 client 插件 JSON 对象 (`get_plugin` / `get_own_info`)
    std::string pluginJson(std::string_view name) override;

private:

    /// 网络服务 (懒创建; 与 [netMutex_] 一起使用)
    std::shared_ptr<PluginNetService> netService_;
    std::mutex                        netMutex_;
};

/// UI 适配器抽象接口 (UI 无关语义层 → 具体 UI 实现)
///
/// 实现方 (TUI/CLI/未来 GUI) 职责:
/// - supportedInterfaces(): 声明支持的接口名集合 ("client.toast" 等, 常量见
///   [plugin_interfaces.h](/agent/lib/include/agentxx/plugin/plugin_interfaces.h)
///   plugin_interfaces; 主程序据此加载 "client.ui" 接口表、
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

    /// 客户端界面能力段 (`get_client_state()` 的 `ui` 段)
    ///
    /// 客户端**如实上报**自己能画什么 (支持的组件/控件、每个字符格相当于多少 u、
    /// 默认行距)。插件据此选择组件 (SDK 见 `plugin_kit.h` 的
    /// `ClientPluginBase::uiCapabilities()` / `supportsBlock()`); 内容仍由客户端按
    /// 自身能力降级 (描述层的 adapt), 因此老插件不会画错。
    ///
    /// - 取值来源: `pluginxx::ui::capabilitiesToJson(<本客户端的能力>)`
    /// - 返回空串 = 不发布该段 (插件按"描述层全支持"处理); 非对象 JSON 会被忽略
    /// - 加载后不可变 (与 supportedInterfaces 同一约定), 启动时读取一次
    virtual std::string uiCapabilitiesJson() const {
        return {};
    }

    /// ---- 信号回调 (client io 线程; 快速返回) ----

    /// 界面内容注册 (slot 短名 + 短 id + 展示名)
    ///
    /// 客户端要"按内容建控件"的地方用它, 例如侧边栏按 `panel` 槽位上的内容建 tab;
    /// 只要重绘的地方用 [onContentUpdated] 就够。同一份内容重复注册会再来一次,
    /// 调用方要按 id 去重。
    virtual void onContentRegistered(
        const std::string& /*slot*/,
        const std::string& /*shortId*/,
        const std::string& /*title*/
    ) {}

    /// 界面内容撤销 (插件注销 / 停用 / 卸载)
    virtual void
        onContentRemoved(const std::string& /*slot*/, const std::string& /*shortId*/) {}

    /// toast 提示 (level: 0=info 1=warning 2=error)
    virtual void onToast(const std::string& /*text*/, int /*level*/) {}

    /// send 动作: 代发用户消息 (io 线程; 与用户输入同排队语义)
    virtual void sendPluginMessage(const std::string& /*text*/) {}

    /// 请求取消当前会话轮次 (io 线程; 与用户按 Esc 等价)
    virtual void requestCancel(const std::string& /*sessionId*/) {}

    /// 跨端数据: client → agent (io 线程; 经端点 WirePluginDataUp 发送)
    /// - topic 是全名 (`agentxx.client.plugin.<插件名>.<事件>` /
    ///   `agentxx.client.host.*`), 由调用方写全
    /// 返回 true 表示已发送 (未连接等失败返回 false)
    virtual bool sendPluginData(const std::string& /*topic*/, const std::string& /*json*/) {
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

    /// 一份声明式内容有了新结果 (client io 线程; 触发重绘即可)
    /// - 渲染从 `ClientPluginManager::uiContentSnapshot()` 读, 不在 UI 线程执行插件回调
    virtual void onContentUpdated(const std::string& /*slot*/, const std::string& /*shortId*/) {}
};

} // namespace plugin
} // namespace agentxx
