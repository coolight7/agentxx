#include "agentxx/plugin/client_plugin_manager.h"

#include "agentxx/plugin/api/client_plugin_api.h"
#include "agentxx/plugin/client_ui_content.h"
#include "asio/post.hpp"
#include "pluginxx/api/ui.h"
#include "pluginxx/host/table_op.h"
#include "pluginxx/host/ui_content.h"
#include "pluginxx/runtime/instance_base.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace agentxx {
namespace plugin {

namespace {

using utilxx_base::Json;

/// ABI 字符串视图 → std::string (入参是借用视图, 必须自己拷一份)
std::string viewToStr(const PluginxxStringView& view) {
    if (!view.data) {
        return {};
    }
    return std::string{view.data, static_cast<size_t>(view.size)};
}

/// std::string_view → ABI 字符串视图 (借用, 只在调用内有效)
PluginxxStringView strToView(std::string_view text) {
    return PluginxxStringView{text.data(), static_cast<uint64_t>(text.size())};
}

/// 安全解析 (失败给空 JSON)
Json parseSafe(std::string_view text) {
    if (text.empty()) {
        return Json{};
    }
    try {
        return Json::parse(text);
    } catch (...) {
        return Json{};
    }
}

/// 插件 id: client 侧的实例名就是插件 id (与 agent 侧同一约定)
std::string_view uiInstancePluginId(std::string_view instanceName) {
    return instanceName;
}

/// 短名归一为全名 `plugin.<插件 id>.<短名>`
/// - 已带自己的命名空间前缀 → 原样;
/// - 带别人的前缀 → 空串 (拒绝);
/// - 没前缀 → 补上。
std::string fullUiIdOf(std::string_view pluginId, std::string_view raw) {
    if (raw.empty()) {
        return {};
    }
    if (raw.rfind("plugin.", 0) == 0) {
        const std::string own = "plugin." + std::string{pluginId} + ".";
        if (raw.rfind(own, 0) != 0 || raw.size() == own.size()) {
            return {};
        }
        return std::string{raw};
    }
    return "plugin." + std::string{pluginId} + "." + std::string{raw};
}

/// 全名去掉 `plugin.<插件 id>.` 前缀
std::string shortUiIdOf(std::string_view pluginId, std::string_view full) {
    const std::string own = "plugin." + std::string{pluginId} + ".";
    if (full.rfind(own, 0) == 0) {
        return std::string{full.substr(own.size())};
    }
    return std::string{full};
}

/// 一次内容生成的完成等待 (插件调用 notify 时释放)
struct GenerateWaiter {
    std::function<void(int32_t, std::string_view)> finish;
};

/// 生成完成通知: 只负责取出文本并收尾 (不做注册表写入)
void PLUGINXX_CALL generateDoneTrampoline(
    void*                     ud,
    int32_t                   status,
    const PluginxxStringView* payload
) {
    std::unique_ptr<GenerateWaiter> waiter{static_cast<GenerateWaiter*>(ud)};
    if (!waiter) {
        return;
    }
    const std::string_view text = (payload && payload->data)
                                      ? std::string_view{
                                            payload->data,
                                            static_cast<size_t>(payload->size)
                                        }
                                      : std::string_view{};
    if (waiter->finish) {
        waiter->finish(status, text);
    }
}

/// 表成员的业务失败原因 (插件看到的就是这段文本)
const char* uiReasonOf(int32_t rc) {
    switch (rc) {
        case PLUGINXX_RESULT_ERR_ARG:
            return "arg";
        case PLUGINXX_RESULT_ERR_PERMISSION:
            return "namespace";
        case PLUGINXX_RESULT_ERR_NOT_DECLARED:
            return "unknown_slot";
        case PLUGINXX_RESULT_ERR_NOT_FOUND:
            return "not_found";
        default:
            return "state";
    }
}

/// 业务失败统一抛 [pluginxx::TableOpError]
[[noreturn]] void throwUiFailure(const char* what, int32_t rc) {
    throw pluginxx::TableOpError(std::string{what} + ": " + uiReasonOf(rc));
}

} // namespace

// =====================================================================
// 槽位目录与快照 (界面线程只读)
// =====================================================================

std::string clientUiFullName(std::string_view instanceName, std::string_view raw) {
    return fullUiIdOf(uiInstancePluginId(instanceName), raw);
}

pluginxx::host::UiSlotCatalog buildClientUiSlotCatalog() {
    std::vector<pluginxx::host::UiSlotDecl> slots;
    slots.push_back({"agentxx.client.status", "insert"});
    slots.push_back({"agentxx.client.panel", "insert"});
    slots.push_back({"agentxx.client.info", "insert"});
    std::vector<std::string> variables;
    variables.emplace_back(kClientUiVarState);
    variables.emplace_back(kClientUiVarRegions);
    return pluginxx::host::UiSlotCatalog(kClientUiSlotPrefix, std::move(slots), std::move(variables));
}

const ClientUiContentView* ClientUiSnapshot::contentOf(
    std::string_view slot,
    std::string_view id
) const {
    for (const auto& content : contents) {
        if (content.slot == slot && content.id == id) {
            return &content;
        }
    }
    return nullptr;
}

std::vector<const ClientUiContentView*> ClientUiSnapshot::contentsOn(std::string_view slot) const {
    std::vector<const ClientUiContentView*> out;
    for (const auto& content : contents) {
        if (content.slot == slot) {
            out.push_back(&content);
        }
    }
    return out;
}

const ClientUiEntryView* ClientUiSnapshot::entryOf(
    std::string_view type,
    std::string_view shortId
) const {
    for (const auto& entry : entries) {
        if (entry.type == type && entry.shortId == shortId) {
            return &entry;
        }
    }
    return nullptr;
}

// =====================================================================
// 注册表写入 (client io 线程)
// =====================================================================

std::shared_ptr<const ClientUiSnapshot> ClientPluginManager::uiContentSnapshot() const {
    std::lock_guard<std::mutex> lock(uiContentMutex_);
    return uiContentSnapshot_;
}

void ClientPluginManager::publishUiContentSnapshot() {
    auto snapshot = std::make_shared<ClientUiSnapshot>();
    snapshot->entries.reserve(uiContent_.entries().size());
    for (const auto& [id, entry] : uiContent_.entries()) {
        (void)id;
        ClientUiEntryView view;
        view.plugin   = entry.plugin;
        view.id       = entry.id;
        view.shortId  = shortUiIdOf(uiInstancePluginId(entry.plugin), entry.id);
        view.type     = entry.type;
        view.dataJson = entry.dataJson;
        view.order    = entry.order;
        view.seq      = entry.seq;
        snapshot->entries.push_back(std::move(view));
    }
    std::sort(
        snapshot->entries.begin(),
        snapshot->entries.end(),
        [](const ClientUiEntryView& a, const ClientUiEntryView& b) {
            if (a.type != b.type) {
                return a.type < b.type;
            }
            if (a.order != b.order) {
                return a.order < b.order;
            }
            return a.seq < b.seq;
        }
    );
    snapshot->contents.reserve(uiContent_.contents().size());
    for (const auto& [fullId, content] : uiContent_.contents()) {
        (void)fullId;
        ClientUiContentView view;
        view.plugin    = content.plugin;
        view.fullId    = content.fullId;
        view.id        = content.id;
        view.slot      = content.slot;
        view.title     = content.title;
        view.depict    = content.depict;
        view.depsJson  = content.depsJson;
        view.order     = content.order;
        view.rev       = content.rev;
        view.seq       = content.seq;
        view.hasResult = content.hasResult;
        view.json      = content.lastJson;
        if (!content.lastJson.empty()) {
            // 界面每帧都用它: 推快照时解析一次, 不在渲染线程里反复解析文本
            view.parsed = parseSafe(content.lastJson);
            if (!view.parsed.is_array() && !view.parsed.is_object()) {
                view.parsed = Json::array();
            }
        }
        view.data = content.lastData;
        snapshot->contents.push_back(std::move(view));
    }
    std::sort(
        snapshot->contents.begin(),
        snapshot->contents.end(),
        [](const ClientUiContentView& a, const ClientUiContentView& b) {
            if (a.slot != b.slot) {
                return a.slot < b.slot;
            }
            if (a.order != b.order) {
                return a.order < b.order;
            }
            return a.seq < b.seq;
        }
    );
    snapshot->slots.reserve(uiContent_.pluginSlots().size());
    for (const auto& [id, slot] : uiContent_.pluginSlots()) {
        (void)id;
        ClientUiSlotView view;
        view.plugin = slot.plugin;
        view.id     = slot.id;
        view.kind   = slot.kind;
        view.seq    = slot.seq;
        snapshot->slots.push_back(std::move(view));
    }
    std::sort(
        snapshot->slots.begin(),
        snapshot->slots.end(),
        [](const ClientUiSlotView& a, const ClientUiSlotView& b) { return a.seq < b.seq; }
    );
    {
        std::lock_guard<std::mutex> lock(uiContentMutex_);
        uiContentSnapshot_ = std::move(snapshot);
    }
}

std::string ClientPluginManager::clientUiVariableJson(std::string_view id) const {
    if (id == kClientUiVarState) {
        Json state;
        state["sessionId"]       = sessionId_;
        state["connState"]       = connState_;
        state["startupProgress"] = startupProgress_;
        return state.dump();
    }
    if (id == kClientUiVarRegions) {
        Json regions = Json::array();
        for (const auto& region : regionSizes()) {
            Json item;
            item["id"]     = region.id;
            item["w"]      = region.width;
            item["h"]      = region.height;
            regions.push_back(std::move(item));
        }
        return regions.dump();
    }
    return "null";
}

/// 生成函数要的上下文: 只放它声明的变量 (`deps`)
std::string clientUiContextJson(
    const pluginxx::host::UiContent&           content,
    const std::function<std::string(std::string_view)>& valueOf
) {
    Json ctx = Json::object();
    for (const auto& dep : content.deps) {
        ctx[dep] = parseSafe(valueOf(dep));
    }
    return ctx.dump();
}

// =====================================================================
// 声明项
// =====================================================================

int32_t ClientPluginManager::registerUiEntry(
    ClientPluginInstance*      inst,
    const PluginxxUiEntrySpec* spec
) {
    if (!inst || !spec) {
        return PLUGINXX_RESULT_ERR_ARG;
    }
    const std::string_view pluginId = uiInstancePluginId(inst->name);
    const std::string      type     = uiSlotCatalog_.fullName(viewToStr(spec->type));
    if (uiSlotCatalog_.kindOf(type) == pluginxx::host::UiSlotKind::Unknown) {
        XX_LOGW("[client_plugin] `{}` register_ui_item: unknown slot `{}`", inst->name, type);
        return PLUGINXX_RESULT_ERR_NOT_DECLARED;
    }
    const std::string fullId = fullUiIdOf(pluginId, viewToStr(spec->item_id));
    if (fullId.empty()) {
        return PLUGINXX_RESULT_ERR_PERMISSION;
    }
    const std::string dataJson = viewToStr(spec->data_json);
    if (!dataJson.empty() && !parseSafe(dataJson).is_object()) {
        return PLUGINXX_RESULT_ERR_ARG;
    }
    const std::string shortId = shortUiIdOf(pluginId, fullId);
    auto              it      = uiContent_.entries().find(fullId);
    if (it != uiContent_.entries().end()) {
        // 替换: 位置信息可以换, 注册顺序保留 (界面上的次序不跳)
        it->second.type     = type;
        it->second.dataJson = dataJson.empty() ? std::string{"{}"} : dataJson;
        it->second.order    = spec->order;
        it->second.rev += 1;
    } else {
        pluginxx::host::UiEntry entry;
        entry.plugin   = inst->name;
        entry.pluginId = std::string{pluginId};
        entry.id       = shortId.empty() ? fullId : fullId;
        entry.type     = type;
        entry.dataJson = dataJson.empty() ? std::string{"{}"} : dataJson;
        entry.order    = spec->order;
        entry.seq      = uiContent_.nextEntrySeq();
        uiContent_.entries().emplace(fullId, std::move(entry));
    }
    publishUiContentSnapshot();
    // 有新位置了: 让它的内容按当前变量生成一次 (没有内容时什么也不做)
    requestUiContent(fullId);
    return PLUGINXX_RESULT_OK;
}

int32_t ClientPluginManager::unregisterUiEntry(
    ClientPluginInstance* inst,
    std::string_view      itemId
) {
    if (!inst || itemId.empty()) {
        return PLUGINXX_RESULT_ERR_ARG;
    }
    const std::string fullId = fullUiIdOf(uiInstancePluginId(inst->name), itemId);
    if (fullId.empty()) {
        return PLUGINXX_RESULT_ERR_PERMISSION;
    }
    if (uiContent_.entries().erase(fullId) == 0) {
        return PLUGINXX_RESULT_ERR_NOT_FOUND;
    }
    publishUiContentSnapshot();
    return PLUGINXX_RESULT_OK;
}

std::string ClientPluginManager::listUiEntries(ClientPluginInstance* inst) const {
    return uiContent_.entriesJson(inst ? uiInstancePluginId(inst->name) : std::string_view{});
}

// =====================================================================
// 内容
// =====================================================================

int32_t ClientPluginManager::registerUiContent(
    ClientPluginInstance*        inst,
    const PluginxxUiContentSpec* spec
) {
    if (!inst || !spec || !spec->generate) {
        return PLUGINXX_RESULT_ERR_ARG;
    }
    const std::string_view pluginId = uiInstancePluginId(inst->name);
    const std::string      id       = viewToStr(spec->id);
    if (id.empty()) {
        return PLUGINXX_RESULT_ERR_ARG;
    }
    const std::string fullId = fullUiIdOf(pluginId, id);
    if (fullId.empty()) {
        return PLUGINXX_RESULT_ERR_PERMISSION;
    }
    // 槽位: 目录里的, 或插件自己声明过的
    std::string slot = uiSlotCatalog_.fullName(viewToStr(spec->slot));
    if (uiSlotCatalog_.kindOf(slot) == pluginxx::host::UiSlotKind::Unknown
        && uiContent_.pluginSlots().find(slot) == uiContent_.pluginSlots().end()) {
        XX_LOGW("[client_plugin] `{}` register_ui_content: unknown slot `{}`", inst->name, slot);
        return PLUGINXX_RESULT_ERR_NOT_DECLARED;
    }
    // 依赖: 只接受目录里的变量与 `plugin.*` (插件自己的变量空间)
    std::vector<std::string> deps;
    const std::string        depsJson = viewToStr(spec->deps_json);
    if (!depsJson.empty()) {
        const Json parsed = parseSafe(depsJson);
        if (!parsed.is_array()) {
            return PLUGINXX_RESULT_ERR_ARG;
        }
        for (const auto& dep : parsed) {
            if (!dep.is_string()) {
                return PLUGINXX_RESULT_ERR_ARG;
            }
            const std::string name = dep.get<std::string>();
            if (!uiSlotCatalog_.isKnownVariable(name) && name.rfind("plugin.", 0) != 0) {
                XX_LOGW(
                    "[client_plugin] `{}` register_ui_content: unknown variable `{}`",
                    inst->name,
                    name
                );
                return PLUGINXX_RESULT_ERR_ARG;
            }
            deps.push_back(name);
        }
    }
    const std::string shortId = shortUiIdOf(pluginId, fullId);
    auto              it      = uiContent_.contents().find(fullId);
    if (it != uiContent_.contents().end()) {
        pluginxx::host::UiContent& content = it->second;
        content.slot        = slot;
        content.title       = viewToStr(spec->title);
        content.depict      = viewToStr(spec->depict);
        content.routesJson  = viewToStr(spec->routes_json);
        content.depsJson    = depsJson.empty() ? std::string{"[]"} : depsJson;
        content.deps        = std::move(deps);
        content.order       = spec->order;
        content.minIntervalMs = spec->min_interval_ms;
        content.spec        = *spec;
    } else {
        pluginxx::host::UiContent content;
        content.plugin       = inst->name;
        content.pluginId     = std::string{pluginId};
        content.id           = shortId;
        content.fullId       = fullId;
        content.slot         = slot;
        content.title        = viewToStr(spec->title);
        content.depict       = viewToStr(spec->depict);
        content.routesJson   = viewToStr(spec->routes_json);
        content.depsJson     = depsJson.empty() ? std::string{"[]"} : depsJson;
        content.deps         = std::move(deps);
        content.order        = spec->order;
        content.minIntervalMs = spec->min_interval_ms;
        content.seq          = uiContent_.nextContentSeq();
        content.spec         = *spec;
        uiContent_.contents().emplace(fullId, std::move(content));
    }
    publishUiContentSnapshot();
    requestUiContent(fullId);
    return PLUGINXX_RESULT_OK;
}

int32_t ClientPluginManager::unregisterUiContent(
    ClientPluginInstance* inst,
    std::string_view      id
) {
    if (!inst || id.empty()) {
        return PLUGINXX_RESULT_ERR_ARG;
    }
    std::string fullId = fullUiIdOf(uiInstancePluginId(inst->name), id);
    if (fullId.empty()) {
        return PLUGINXX_RESULT_ERR_PERMISSION;
    }
    if (uiContent_.contents().erase(fullId) == 0) {
        return PLUGINXX_RESULT_ERR_NOT_FOUND;
    }
    uiGenerating_.erase(fullId);
    publishUiContentSnapshot();
    return PLUGINXX_RESULT_OK;
}

int32_t ClientPluginManager::pushUiContent(
    ClientPluginInstance* inst,
    std::string_view      id,
    std::string_view      contentJson
) {
    if (!inst || id.empty()) {
        return PLUGINXX_RESULT_ERR_ARG;
    }
    const std::string fullId = fullUiIdOf(uiInstancePluginId(inst->name), id);
    if (fullId.empty()) {
        return PLUGINXX_RESULT_ERR_PERMISSION;
    }
    if (uiContent_.findContent(fullId) == nullptr) {
        return PLUGINXX_RESULT_ERR_NOT_FOUND;
    }
    const Json body = parseSafe(contentJson);
    if (!body.is_object()) {
        return PLUGINXX_RESULT_ERR_ARG;
    }
    const std::string payload = uiContent_.buildContentPayload(fullId, contentJson, {});
    if (payload.empty()) {
        return PLUGINXX_RESULT_ERR_NOT_FOUND;
    }
    publishUiContentSnapshot();
    if (uiAdapter_) {
        if (const auto* content = uiContent_.findContent(fullId)) {
            uiAdapter_->onContentUpdated(content->slot, content->id);
        }
    }
    return PLUGINXX_RESULT_OK;
}

std::string ClientPluginManager::listUiContents(ClientPluginInstance* inst) const {
    return uiContent_.contentsJson(inst ? uiInstancePluginId(inst->name) : std::string_view{});
}

int32_t ClientPluginManager::updateUiSlot(
    ClientPluginInstance* inst,
    std::string_view      slotRaw,
    std::string_view      idsJson
) {
    if (!inst || slotRaw.empty()) {
        return PLUGINXX_RESULT_ERR_ARG;
    }
    const std::string slot = uiSlotCatalog_.fullName(slotRaw);
    std::vector<std::string> wanted;
    if (!idsJson.empty()) {
        const Json parsed = parseSafe(idsJson);
        if (!parsed.is_array()) {
            return PLUGINXX_RESULT_ERR_ARG;
        }
        for (const auto& item : parsed) {
            if (item.is_string()) {
                wanted.push_back(item.get<std::string>());
            }
        }
    }
    const std::string_view pluginId = uiInstancePluginId(inst->name);
    std::vector<std::string> targets;
    for (const auto& [fullId, content] : uiContent_.contents()) {
        (void)fullId;
        if (content.pluginId != pluginId || content.slot != slot) {
            continue;
        }
        if (!wanted.empty()) {
            const bool hit = std::any_of(
                wanted.begin(),
                wanted.end(),
                [&](const std::string& want) { return want == content.id; }
            );
            if (!hit) {
                continue;
            }
        }
        targets.push_back(content.fullId);
    }
    for (const auto& fullId : targets) {
        requestUiContent(fullId);
    }
    return PLUGINXX_RESULT_OK;
}

// =====================================================================
// 插件自建槽位
// =====================================================================

int32_t ClientPluginManager::defineUiSlot(
    ClientPluginInstance*     inst,
    const PluginxxUiSlotSpec* spec
) {
    if (!inst || !spec) {
        return PLUGINXX_RESULT_ERR_ARG;
    }
    const std::string_view pluginId = uiInstancePluginId(inst->name);
    const std::string      id       = viewToStr(spec->id);
    const std::string      own      = "plugin." + std::string{pluginId} + ".ui.";
    if (id.rfind(own, 0) != 0 || id.size() == own.size()) {
        return PLUGINXX_RESULT_ERR_PERMISSION;
    }
    const std::string kind = viewToStr(spec->kind);
    if (kind != "insert" && kind != "replace") {
        return PLUGINXX_RESULT_ERR_ARG;
    }
    auto it = uiContent_.pluginSlots().find(id);
    if (it != uiContent_.pluginSlots().end()) {
        if (it->second.pluginId != pluginId) {
            return PLUGINXX_RESULT_ERR_PERMISSION;
        }
        it->second.kind = kind;
    } else {
        pluginxx::host::UiSlot item;
        item.plugin   = inst->name;
        item.pluginId = std::string{pluginId};
        item.id       = id;
        item.kind     = kind;
        item.seq      = uiContent_.nextSlotSeq();
        uiContent_.pluginSlots().emplace(id, std::move(item));
    }
    publishUiContentSnapshot();
    return PLUGINXX_RESULT_OK;
}

int32_t ClientPluginManager::undefineUiSlot(
    ClientPluginInstance* inst,
    std::string_view      slot
) {
    if (!inst || slot.empty()) {
        return PLUGINXX_RESULT_ERR_ARG;
    }
    auto it = uiContent_.pluginSlots().find(slot);
    if (it == uiContent_.pluginSlots().end()) {
        return PLUGINXX_RESULT_ERR_NOT_FOUND;
    }
    if (it->second.pluginId != uiInstancePluginId(inst->name)) {
        return PLUGINXX_RESULT_ERR_PERMISSION;
    }
    const std::string id = it->second.id;
    uiContent_.pluginSlots().erase(it);
    // 挂在这个槽位上的内容一起失效
    for (auto contentIt = uiContent_.contents().begin();
         contentIt != uiContent_.contents().end();) {
        if (contentIt->second.slot == id) {
            uiGenerating_.erase(contentIt->first);
            contentIt = uiContent_.contents().erase(contentIt);
        } else {
            ++contentIt;
        }
    }
    publishUiContentSnapshot();
    return PLUGINXX_RESULT_OK;
}

std::string ClientPluginManager::listUiSlots(ClientPluginInstance* inst) const {
    return uiContent_.slotsJson(uiSlotCatalog_, inst ? uiInstancePluginId(inst->name) : std::string_view{});
}

// =====================================================================
// 生成 (投递到插件执行线程; 界面永远不等)
// =====================================================================

void ClientPluginManager::requestUiContent(std::string_view fullId) {
    auto* content = uiContent_.findContent(fullId);
    if (content == nullptr || !content->spec.generate) {
        return;
    }
    // 同一个内容同时只跑一次: 界面每帧都可能问, 没有这条会让生成函数堆积
    if (!uiGenerating_.emplace(std::string{fullId}).second) {
        return;
    }
    auto inst = find(content->plugin);
    if (!inst || !inst->enabled) {
        uiGenerating_.erase(std::string{fullId});
        return;
    }
    const std::string contextJson = clientUiContextJson(
        *content,
        [this](std::string_view dep) { return clientUiVariableJson(dep); }
    );
    auto generate  = content->spec.generate;
    void* userData = content->spec.user_data;
    auto  self     = shared_from_this();
    const std::string key{fullId};
    auto              waiter = new GenerateWaiter();
    waiter->finish = [self, key](int32_t status, std::string_view payload) {
        std::string text{payload};
        std::string error;
        if (status != PLUGINXX_OPERATOR_OK) {
            error = text.empty() ? std::string{"the generate function reported a failure"} : text;
            text.clear();
        } else if (text.empty()) {
            error = "the generate function answered nothing";
        }
        asio::post(
            self->ioExecutor(),
            [self, key, text = std::move(text), error = std::move(error)]() {
                self->finishUiGeneration(key, text, error);
            }
        );
    };
    const auto host = inst->hostView();
    const bool queued = pluginxx::dispatchToPlugin(
        inst,
        "client ui content " + key,
        [host, generate, userData, contextJson, waiter]() mutable {
            // 等待者归完成通知所有 (trampoline 里 delete): 本函数再持有就是双重释放
            std::unique_ptr<GenerateWaiter> holder{waiter};
            holder.release();
            const PluginxxStringView        ctxView{
                contextJson.data(),
                static_cast<uint64_t>(contextJson.size())
            };
            PluginxxOperatorNotify notify{&generateDoneTrampoline, waiter};
            PluginxxString         errorOut{};
            try {
                generate(host, &ctxView, userData, &notify, &errorOut);
            } catch (const std::exception& e) {
                if (errorOut.data) {
                    pluginxx::hostMemoryFree(errorOut.data);
                    errorOut = PluginxxString{};
                }
                // 异常也要收尾: 否则只有不了结的一次生成
                const std::string        reason{e.what()};
                const PluginxxStringView reasonView{reason.data(), reason.size()};
                generateDoneTrampoline(notify.host_ud, PLUGINXX_OPERATOR_FAILED, &reasonView);
                return;
            } catch (...) {
                if (errorOut.data) {
                    pluginxx::hostMemoryFree(errorOut.data);
                    errorOut = PluginxxString{};
                }
                const std::string        reason{"the generate function threw an unknown exception"};
                const PluginxxStringView reasonView{reason.data(), reason.size()};
                generateDoneTrampoline(notify.host_ud, PLUGINXX_OPERATOR_FAILED, &reasonView);
                return;
            }
            if (errorOut.data) {
                const std::string reason{errorOut.data, static_cast<size_t>(errorOut.size)};
                pluginxx::hostMemoryFree(errorOut.data);
                // 未受理: 插件不会再调 notify, 这里按失败收尾
                const PluginxxStringView reasonView{reason.data(), reason.size()};
                generateDoneTrampoline(notify.host_ud, PLUGINXX_OPERATOR_FAILED, &reasonView);
            }
        }
    );
    if (!queued) {
        delete waiter;
        uiGenerating_.erase(key);
    }
}

void ClientPluginManager::finishUiGeneration(
    std::string fullId,
    std::string jsonText,
    std::string error
) {
    uiGenerating_.erase(fullId);
    auto* content = uiContent_.findContent(fullId);
    if (content == nullptr) {
        return;
    }
    const std::string slot  = content->slot;
    const std::string shortId = content->id;
    if (!error.empty()) {
        XX_LOGW(
            "[client_plugin] `{}` content `{}` failed to generate: {}",
            content->plugin,
            fullId,
            error
        );
        // 失败保留上一份内容, 只推进版本号让界面知道试过了
        const std::string payload = uiContent_.buildContentPayload(fullId, {}, error);
        (void)payload;
        return;
    }
    const std::string payload = uiContent_.buildContentPayload(fullId, jsonText, {});
    if (payload.empty()) {
        return;
    }
    publishUiContentSnapshot();
    if (uiAdapter_) {
        uiAdapter_->onContentUpdated(slot, shortId);
    }
}

/// 停用 / 卸载: 声明项、内容与自建槽位一起撤销
void ClientPluginManager::detachUiContent(std::string_view instanceName) {
    for (auto it = uiContent_.contents().begin(); it != uiContent_.contents().end();) {
        if (it->second.plugin == instanceName) {
            uiGenerating_.erase(it->first);
            it = uiContent_.contents().erase(it);
        } else {
            ++it;
        }
    }
    uiContent_.detachOwner(instanceName);
    publishUiContentSnapshot();
}

} // namespace plugin
} // namespace agentxx
