/// 客户端声明式界面: 槽位表、变量与界面线程读的快照
///
/// 客户端与 agent 侧共用同一套声明式界面规则 (`pluginxx/api/ui.h` + `pluginxx/host/ui_content.h`),
/// 差在槽位与变量是各自的项目数据:
///
/// - 槽位只有几个固定分类, **具体是谁**由内容 id 表达 —— `status` 上 id `cpu` 就是那个
///   状态栏项, `panel` 上 id `plan` 就是那个面板; 这样插件既能注册多个同类位置, 又不需要
///   为每个面板声明一个槽位;
/// - 变量是主程序自己有的数据 (`agentxx.client.state` / `agentxx.client.regions`), 内容
///   在 `deps_json` 里声明要哪些, 生成时只拿到这些;
/// - 界面线程**只读快照** ([ClientUiSnapshot]): 注册表归 client io 线程, 渲染不会跑插件代码。
#ifndef AGENTXX_PLUGIN_CLIENT_UI_CONTENT_H
#define AGENTXX_PLUGIN_CLIENT_UI_CONTENT_H

#include "pluginxx/host/ui_slots.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace plugin {

/// 客户端槽位 (短名可直接写在 `PluginxxUiEntrySpec::type` / `PluginxxUiContentSpec::slot`)
inline constexpr std::string_view kClientUiSlotStatus = "agentxx.client.status";
inline constexpr std::string_view kClientUiSlotPanel  = "agentxx.client.panel";
inline constexpr std::string_view kClientUiSlotInfo   = "agentxx.client.info";

/// 客户端槽位前缀 (短名 → 全名的规则由 `pluginxx::host::UiSlotCatalog` 给出)
inline constexpr std::string_view kClientUiSlotPrefix = "agentxx.client.";

/// 变量: 会话状态 (`{"sessionId","connState","startupProgress"}`)
inline constexpr std::string_view kClientUiVarState   = "agentxx.client.state";
/// 变量: 展示区域尺寸快照 (`[{"id","w","h"},...]`)
inline constexpr std::string_view kClientUiVarRegions = "agentxx.client.regions";

/// 客户端槽位目录 (三个保留槽位 + 两个变量; 插件自建槽位由注册表另存)
pluginxx::host::UiSlotCatalog buildClientUiSlotCatalog();

/// 短名归一为全名 `plugin.<插件 id>.<短名>` (client 侧的插件 id 就是实例名)
///
/// - 已带自己的命名空间前缀 → 原样返回;
/// - 带别人的前缀 → 空串 (拒绝);
/// - 没有前缀 → 补上。
///
/// 旧注册项 (`register_status_item` 一类) 也走这条规则, 这样两类接口给同一个名字时
/// 落在同一条内容上。
std::string clientUiFullName(std::string_view instanceName, std::string_view raw);

/// 一条声明项 (界面线程读的拷贝)
struct ClientUiEntryView {
    std::string plugin;   ///< 实例名
    std::string id;       ///< 全名 (plugin.<插件 id>.<短名>)
    std::string shortId;  ///< 短名
    std::string type;     ///< 槽位全名
    std::string dataJson; ///< 声明携带的数据 (JSON 对象)
    int32_t     order = 0;
    uint64_t    seq   = 0;
};

/// 一份内容 (界面线程读的拷贝; 只有最近一次成功结果)
struct ClientUiContentView {
    std::string plugin;   ///< 实例名
    std::string fullId;   ///< plugin.<插件 id>.<短名>
    std::string id;       ///< 短名 (与声明项同名时画在同一个位置)
    std::string slot;     ///< 槽位全名
    std::string title;    ///< 展示名 (可空)
    std::string depict;   ///< 一行说明 (可空)
    std::string depsJson; ///< 声明的变量 (JSON 数组)
    int32_t     order = 0;
    int32_t     rev   = 0;  ///< 结果版本号 (每次生成 / 推送 +1)
    uint64_t    seq   = 0;
    bool        hasResult = false; ///< 还没有成功结果时界面不画它
    std::string json;              ///< 最近一次成功的内容 (界面描述层的 JSON)
    /// 同 [json] 解析后的结果 (推快照时解析一次; 界面每帧直接交给块树渲染器,
    /// 不再逐帧解析文本)。解析失败时是空数组 (= 什么都不画)。
    utilxx_base::Json parsed = utilxx_base::Json::array();
    std::string       data;    ///< 同一次的附带数据 (原样保留)
};

/// 插件自己声明的槽位 (界面线程读的拷贝)
struct ClientUiSlotView {
    std::string plugin;
    std::string id;   ///< plugin.<插件 id>.ui.<名字>
    std::string kind; ///< insert / replace
    uint64_t    seq = 0;
};

/// 声明式界面的只读快照 (client io 线程写, 界面线程读)
struct ClientUiSnapshot {
    std::vector<ClientUiEntryView> entries;  ///< 按 (type, order, seq) 排序
    std::vector<ClientUiContentView> contents; ///< 按 (slot, order, seq) 排序
    std::vector<ClientUiSlotView> slots;       ///< 按 (seq) 排序

    /// 某个槽位上、某个短名的内容 (没有返回 nullptr)
    const ClientUiContentView* contentOf(std::string_view slot, std::string_view id) const;

    /// 某个槽位上的全部内容 (按 order / seq 排序后的原顺序)
    std::vector<const ClientUiContentView*> contentsOn(std::string_view slot) const;

    /// 某个槽位上、某个短名的声明项 (没有返回 nullptr)
    const ClientUiEntryView* entryOf(std::string_view type, std::string_view shortId) const;
};

} // namespace plugin
} // namespace agentxx

#endif /* AGENTXX_PLUGIN_CLIENT_UI_CONTENT_H */
