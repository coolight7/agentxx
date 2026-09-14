#include "agentxx/middlewares/interrupt_ui.h"

#include "fmt/format.h"

namespace agentxx {
namespace middleware {

namespace {

/// 通用默认描述的单项控件 id (多项时追加序号: "value1".."valueN")
constexpr std::string_view kSingleInputId = "value";

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
    if (!header.segments.empty()) {
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
// 内置描述: 默认 (按输入项展开的自包含表单) / 权限询问卡片
// ---------------------------------------------------------------------------

InterruptUi InterruptUi::defaultUi(const std::vector<InterruptUiInputSpec>& inputs) {
    InterruptUi ui;
    // 头行留空: 客户端渲染通用默认前缀 (i18n 键 interrupt.header)

    const size_t total   = inputs.size();
    bool         hasPrev = false;
    for (size_t k = 0; k < total; ++k) {
        const auto& spec = inputs[k];
        if (hasPrev) {
            InterruptUiItem gap;
            gap.kind = "gap";
            ui.items.push_back(std::move(gap));
        }

        // 控件标签 (accent, 如 "[read] Repeated identical call")
        if (!spec.label.empty()) {
            InterruptUiItem label;
            label.kind   = "text";
            label.text   = spec.label;
            label.color  = "accent";
            label.bold   = true;
            label.indent = 2;
            ui.items.push_back(std::move(label));
        }

        // 控件说明 (hint, 硬折行; 如受约束路径/询问原因)
        if (!spec.depict.empty()) {
            InterruptUiItem desc;
            desc.kind   = "text";
            desc.text   = spec.depict;
            desc.color  = "hint";
            desc.indent = 2;
            desc.wrap   = true;
            ui.items.push_back(std::move(desc));
        }

        // 输入控件 (自包含: 类型/默认值/枚举候选来自声明)
        InterruptUiItem input;
        input.kind         = "input";
        input.id           = (total == 1) ? std::string{kSingleInputId}
                                          : fmt::format("{}{}", kSingleInputId, k + 1);
        input.inputType    = spec.type;
        input.defaultValue = spec.defaultValue;
        input.enumValues   = spec.enumValues;
        ui.items.push_back(std::move(input));
        ui.values.push_back(ui.items.back().id);

        hasPrev = true;
    }

    InterruptUiItem submit;
    submit.kind = "submit";
    ui.items.push_back(std::move(submit));
    return ui;
}

InterruptUi InterruptUi::permissionUi(
    std::string_view toolName,
    std::string_view category,
    std::string_view target
) {
    InterruptUi ui;
    // 头行: 权限标记 + 工具名 + 权限分类 (权限询问恒为单控件表单)
    InterruptUiSegment badge;
    badge.text     = "! [Permission] ";
    badge.labelKey = "interrupt.permissionBadge";
    badge.color    = "error";
    badge.bold     = true;
    ui.header.segments.push_back(std::move(badge));
    if (!toolName.empty()) {
        InterruptUiSegment tool;
        tool.text  = std::string{toolName};
        tool.color = "accent";
        tool.bold  = true;
        ui.header.segments.push_back(std::move(tool));
    }
    if (!category.empty()) {
        InterruptUiSegment cat;
        cat.text  = " " + std::string{category};
        cat.color = "hint";
        ui.header.segments.push_back(std::move(cat));
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

    // 设置项: 记住此选择 (勾选后提交时按本次选择注册路径规则)
    InterruptUiItem remember;
    remember.kind        = "toggle";
    remember.id          = "remember";
    remember.labelKey    = "interrupt.remember";
    // 字面回退文本 (无 i18n 词表的前端/缺键时使用)
    remember.text = "Remember this choice";
    ui.items.push_back(std::move(remember));

    InterruptUiItem gap2;
    gap2.kind = "gap";
    ui.items.push_back(std::move(gap2));

    // 一键取值按钮: 允许 / 拒绝 (点击即提交)
    InterruptUiItem value;
    value.kind      = "input";
    value.id        = std::string{kSingleInputId};
    value.view      = "buttons";
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

    ui.values  = {std::string{kSingleInputId}};
    ui.options = {"remember"};
    return ui;
}

agentxx::util::Json
    makeInterruptResult(const agentxx::util::Json& values, const agentxx::util::Json& options) {
    // 恒为对象形态 (无勾选项时 options 为空对象): 消费端按同一结构解析,
    // 不再存在"纯数组"形态的分支
    return agentxx::util::Json{
        {"values",  values.is_array() ? values : agentxx::util::Json::array()},
        {"options", options.is_object() ? options : agentxx::util::Json::object()},
    };
}

} // namespace middleware
} // namespace agentxx
