#include "agentxx/middlewares/interrupt_ui.h"

namespace agentxx {
namespace middleware {

namespace {

/// 读取字符串字段 (缺失/类型不符返回空)
std::string jsonString(const agentxx::util::Json& j, std::string_view key) {
    if (!j.is_object()) {
        return {};
    }
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) {
        return {};
    }
    return it->get<std::string>();
}

bool jsonBool(const agentxx::util::Json& j, std::string_view key, bool defaultValue) {
    if (!j.is_object()) {
        return defaultValue;
    }
    auto it = j.find(key);
    if (it == j.end() || !it->is_boolean()) {
        return defaultValue;
    }
    return it->get<bool>();
}

int jsonInt(const agentxx::util::Json& j, std::string_view key, int defaultValue) {
    if (!j.is_object()) {
        return defaultValue;
    }
    auto it = j.find(key);
    if (it == j.end() || !it->is_number()) {
        return defaultValue;
    }
    return it->get<int>();
}

std::vector<std::string> jsonStringArray(const agentxx::util::Json& j, std::string_view key) {
    std::vector<std::string> out;
    if (!j.is_object()) {
        return out;
    }
    auto it = j.find(key);
    if (it == j.end() || !it->is_array()) {
        return out;
    }
    out.reserve(it->size());
    for (const auto& e : *it) {
        out.push_back(e.is_string() ? e.get<std::string>() : std::string{});
    }
    return out;
}

/// 序列化辅助: 空字符串/空列表不写入 (描述尽量小, 且缺省语义明确)
void putIfNotEmpty(agentxx::util::Json& j, std::string_view key, const std::string& v) {
    if (!v.empty()) {
        j[std::string{key}] = v;
    }
}

void putIfTrue(agentxx::util::Json& j, std::string_view key, bool v) {
    if (v) {
        j[std::string{key}] = true;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// 分段 / 头行 / 按钮 / 项 / 描述: JSON 双向
// ---------------------------------------------------------------------------

InterruptUiSegment InterruptUiSegment::fromJson(const agentxx::util::Json& j) {
    InterruptUiSegment seg;
    seg.text     = jsonString(j, "text");
    seg.labelKey = jsonString(j, "labelKey");
    seg.color    = jsonString(j, "color");
    seg.bold     = jsonBool(j, "bold", false);
    seg.dim      = jsonBool(j, "dim", false);
    return seg;
}

agentxx::util::Json InterruptUiSegment::toJson() const {
    auto j = agentxx::util::Json::object();
    putIfNotEmpty(j, "text", text);
    putIfNotEmpty(j, "labelKey", labelKey);
    putIfNotEmpty(j, "color", color);
    putIfTrue(j, "bold", bold);
    putIfTrue(j, "dim", dim);
    return j;
}

InterruptUiHeader InterruptUiHeader::fromJson(const agentxx::util::Json& j) {
    InterruptUiHeader header;
    header.progress = jsonBool(j, "progress", false);
    header.label    = jsonBool(j, "label", true);
    if (j.is_object()) {
        auto it = j.find("segments");
        if (it != j.end() && it->is_array()) {
            for (const auto& s : *it) {
                header.segments.push_back(InterruptUiSegment::fromJson(s));
            }
        }
    }
    return header;
}

agentxx::util::Json InterruptUiHeader::toJson() const {
    auto j = agentxx::util::Json::object();
    putIfTrue(j, "progress", progress);
    if (!label) {
        j["label"] = false;
    }
    if (!segments.empty()) {
        auto arr = agentxx::util::Json::array();
        for (const auto& s : segments) {
            arr.push_back(s.toJson());
        }
        j["segments"] = std::move(arr);
    }
    return j;
}

InterruptUiButton InterruptUiButton::fromJson(const agentxx::util::Json& j) {
    InterruptUiButton b;
    b.value    = jsonString(j, "value");
    b.label    = jsonString(j, "label");
    b.labelKey = jsonString(j, "labelKey");
    b.color    = jsonString(j, "color");
    return b;
}

agentxx::util::Json InterruptUiButton::toJson() const {
    auto j = agentxx::util::Json::object();
    putIfNotEmpty(j, "value", value);
    putIfNotEmpty(j, "label", label);
    putIfNotEmpty(j, "labelKey", labelKey);
    putIfNotEmpty(j, "color", color);
    return j;
}

InterruptUiItem InterruptUiItem::fromJson(const agentxx::util::Json& j) {
    InterruptUiItem item;
    item.kind     = jsonString(j, "kind");
    item.text     = jsonString(j, "text");
    item.labelKey = jsonString(j, "labelKey");
    item.color    = jsonString(j, "color");
    item.bold     = jsonBool(j, "bold", false);
    item.dim      = jsonBool(j, "dim", false);
    item.wrap     = jsonBool(j, "wrap", false);
    item.indent   = std::max(0, jsonInt(j, "indent", 0));

    item.lines = std::max(1, jsonInt(j, "lines", 1));

    item.id            = jsonString(j, "id");
    item.defaultToggle = jsonBool(j, "defaultToggle", false);

    item.inputType    = jsonString(j, "inputType");
    item.defaultValue = jsonString(j, "defaultValue");
    item.enumValues   = jsonStringArray(j, "enumValues");
    item.view         = jsonString(j, "view");
    if (j.is_object()) {
        auto it = j.find("buttons");
        if (it != j.end() && it->is_array()) {
            for (const auto& b : *it) {
                item.buttons.push_back(InterruptUiButton::fromJson(b));
            }
        }
    }

    item.path   = jsonString(j, "path");
    item.oldStr = jsonString(j, "oldStr");
    item.newStr = jsonString(j, "newStr");
    return item;
}

agentxx::util::Json InterruptUiItem::toJson() const {
    auto j = agentxx::util::Json::object();
    putIfNotEmpty(j, "kind", kind);
    putIfNotEmpty(j, "text", text);
    putIfNotEmpty(j, "labelKey", labelKey);
    putIfNotEmpty(j, "color", color);
    putIfTrue(j, "bold", bold);
    putIfTrue(j, "dim", dim);
    putIfTrue(j, "wrap", wrap);
    if (indent > 0) {
        j["indent"] = indent;
    }
    if (kind == "gap" && lines > 1) {
        j["lines"] = lines;
    }
    putIfNotEmpty(j, "id", id);
    putIfTrue(j, "defaultToggle", defaultToggle);
    putIfNotEmpty(j, "inputType", inputType);
    putIfNotEmpty(j, "defaultValue", defaultValue);
    if (!enumValues.empty()) {
        auto arr = agentxx::util::Json::array();
        for (const auto& e : enumValues) {
            arr.push_back(e);
        }
        j["enumValues"] = std::move(arr);
    }
    putIfNotEmpty(j, "view", view);
    if (!buttons.empty()) {
        auto arr = agentxx::util::Json::array();
        for (const auto& b : buttons) {
            arr.push_back(b.toJson());
        }
        j["buttons"] = std::move(arr);
    }
    putIfNotEmpty(j, "path", path);
    putIfNotEmpty(j, "oldStr", oldStr);
    putIfNotEmpty(j, "newStr", newStr);
    return j;
}

InterruptUi InterruptUi::fromJson(const agentxx::util::Json& j) {
    InterruptUi ui;
    if (!j.is_object()) {
        return ui;
    }
    ui.version = jsonInt(j, "version", 1);
    auto hIt   = j.find("header");
    if (hIt != j.end() && hIt->is_object()) {
        ui.header = InterruptUiHeader::fromJson(*hIt);
    }
    auto iIt = j.find("items");
    if (iIt != j.end() && iIt->is_array()) {
        ui.items.reserve(iIt->size());
        for (const auto& item : *iIt) {
            ui.items.push_back(InterruptUiItem::fromJson(item));
        }
    }
    ui.values  = jsonStringArray(j, "values");
    ui.options = jsonStringArray(j, "options");
    return ui;
}

agentxx::util::Json InterruptUi::toJson() const {
    auto j       = agentxx::util::Json::object();
    j["version"] = version;
    if (!header.segments.empty() || header.progress || !header.label) {
        j["header"] = header.toJson();
    }
    if (!items.empty()) {
        auto arr = agentxx::util::Json::array();
        for (const auto& item : items) {
            arr.push_back(item.toJson());
        }
        j["items"] = std::move(arr);
    }
    auto toArray = [](const std::vector<std::string>& v) {
        auto arr = agentxx::util::Json::array();
        for (const auto& s : v) {
            arr.push_back(s);
        }
        return arr;
    };
    if (!values.empty()) {
        j["values"] = toArray(values);
    }
    if (!options.empty()) {
        j["options"] = toArray(options);
    }
    return j;
}

// ---------------------------------------------------------------------------
// 内置描述: 默认 (通用兜底) / 权限询问卡片
// ---------------------------------------------------------------------------

InterruptUi InterruptUi::defaultUi() {
    InterruptUi ui;
    ui.header.progress = true;
    ui.header.label    = true;

    // 描述文本: text 留空 → 客户端取消息 inputDepict (无描述时不渲染该行)
    InterruptUiItem desc;
    desc.kind   = "text";
    desc.color  = "hint";
    desc.indent = 2;
    ui.items.push_back(std::move(desc));

    // 输入控件: 类型/默认值/枚举候选留空 → 客户端取消息字段 (模板语义)
    InterruptUiItem input;
    input.kind = "input";
    input.id   = "value";
    ui.items.push_back(std::move(input));

    InterruptUiItem submit;
    submit.kind = "submit";
    ui.items.push_back(std::move(submit));

    ui.values = {"value"};
    return ui;
}

InterruptUi InterruptUi::permissionUi(
    std::string_view toolName,
    std::string_view category,
    std::string_view target
) {
    InterruptUi ui;
    // 头行: 权限标记 + 工具名 + 权限分类 (不使用默认进度头, 权限询问恒为单输入项)
    ui.header.label    = false;
    ui.header.progress = false;
    ui.header.segments.push_back(
        InterruptUiSegment{.text = "! [Permission] ", .color = "error", .bold = true}
    );
    if (!toolName.empty()) {
        ui.header.segments.push_back(
            InterruptUiSegment{.text = std::string{toolName}, .color = "accent", .bold = true}
        );
    }
    if (!category.empty()) {
        ui.header.segments.push_back(
            InterruptUiSegment{.text = " " + std::string{category}, .color = "hint"}
        );
    }

    // 目标描述 (受约束路径等): 硬折行, 避免无空格长路径不换行/被压为 0 宽
    InterruptUiItem desc;
    desc.kind   = "text";
    desc.color  = "hint";
    desc.indent = 2;
    desc.wrap   = true;
    desc.text   = std::string{target};
    ui.items.push_back(std::move(desc));

    InterruptUiItem gap1;
    gap1.kind = "gap";
    ui.items.push_back(std::move(gap1));

    // 设置项: 记住此选择 (勾选后确认时按本次选择注册路径规则)
    InterruptUiItem remember;
    remember.kind       = "toggle";
    remember.id         = "remember";
    remember.labelKey   = "interrupt.remember";
    // 字面回退文本 (无 i18n 词表的前端/缺键时使用)
    remember.text = "Remember this choice";
    ui.items.push_back(std::move(remember));

    InterruptUiItem gap2;
    gap2.kind = "gap";
    ui.items.push_back(std::move(gap2));

    // 一键取值按钮: 允许 / 拒绝 (点击即确认该输入项)
    InterruptUiItem value;
    value.kind = "input";
    value.id   = "value";
    value.view = "buttons";
    value.inputType = "bool";
    InterruptUiButton allow;
    allow.value    = "true";
    allow.labelKey = "interrupt.allow";
    allow.label    = "Allow";
    InterruptUiButton deny;
    deny.value    = "false";
    deny.labelKey = "interrupt.deny";
    deny.label    = "Deny";
    value.buttons.push_back(std::move(allow));
    value.buttons.push_back(std::move(deny));
    ui.items.push_back(std::move(value));

    ui.values  = {"value"};
    ui.options = {"remember"};
    return ui;
}

agentxx::util::Json
    makeInterruptResult(const agentxx::util::Json& values, const agentxx::util::Json& options) {
    if (!options.is_object() || options.empty()) {
        return values.is_array() ? values : agentxx::util::Json::array();
    }
    return agentxx::util::Json{
        {"values",  values.is_array() ? values : agentxx::util::Json::array()},
        {"options", options                                             },
    };
}

} // namespace middleware
} // namespace agentxx
