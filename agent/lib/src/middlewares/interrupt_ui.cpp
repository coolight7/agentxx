#include "agentxx/middlewares/interrupt_ui.h"

#include "fmt/format.h"
#include "utilxx/diff_util.h"
#include "utilxx_base/string_util.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace agentxx {
namespace middleware {

namespace {

using utilxx_base::Json;

/// 读取数值字段 (缺失/类型不符时 has = false)
double jsonNumber(const Json& j, std::string_view key, bool& has) {
    has = false;
    if (!j.is_object()) {
        return 0.0;
    }
    auto it = j.find(key);
    if (it == j.end() || !it->is_number()) {
        return 0.0;
    }
    has = true;
    return it->get<double>();
}

/// 序列化辅助: 空字符串不写入 (描述尽量小, 且缺省语义明确)
void putIfNotEmpty(Json& j, std::string_view key, const std::string& v) {
    if (!v.empty()) {
        j[std::string{key}] = v;
    }
}

void putIfTrue(Json& j, std::string_view key, bool v) {
    if (v) {
        j[std::string{key}] = true;
    }
}

/// 读取候选项数组 (缺失/非数组返回空)
std::vector<InterruptUiOption> jsonOptions(const Json& j, std::string_view key) {
    std::vector<InterruptUiOption> out;
    if (!j.is_object()) {
        return out;
    }
    auto it = j.find(key);
    if (it == j.end() || !it->is_array()) {
        return out;
    }
    out.reserve(it->size());
    for (const auto& e : *it) {
        out.push_back(InterruptUiOption::fromJson(e));
    }
    return out;
}

/// 按显示宽度硬折行 (按 UTF-8 字符数计; width <= 0 不折行), 保留原换行
std::vector<std::string> wrapToWidth(std::string_view text, int width) {
    std::vector<std::string> out;
    if (text.empty()) {
        out.emplace_back();
        return out;
    }
    if (width <= 0) {
        out.emplace_back(text);
        return out;
    }
    size_t begin = 0;
    while (begin <= text.size()) {
        const auto eol  = text.find('\n', begin);
        const auto line = text.substr(
            begin,
            eol == std::string_view::npos ? std::string_view::npos : eol - begin
        );
        // 单行内按字符数切分 (行式前端对超长单行的处理: 直接按宽度切开)
        if (line.empty()) {
            out.emplace_back();
        } else {
            size_t offset = 0;
            while (offset < line.size()) {
                const auto remain = line.substr(offset);
                const auto count  = utilxx_base::utf8GetLength(remain);
                if (count <= static_cast<size_t>(width)) {
                    out.emplace_back(remain);
                    break;
                }
                auto cut = utilxx_base::findIndexByUtf8Length(remain, static_cast<size_t>(width));
                if (cut == 0 || cut > remain.size()) {
                    cut = remain.size();
                }
                out.emplace_back(remain.substr(0, cut));
                offset += cut;
            }
        }
        if (eol == std::string_view::npos) {
            break;
        }
        begin = eol + 1;
        if (begin == text.size()) {
            out.emplace_back();
            break;
        }
    }
    return out;
}

/// 控件候选项/默认值的纯文本摘要 (值 → 字符串)
std::string jsonValueText(const Json& v) {
    if (v.is_string()) {
        return v.get<std::string>();
    }
    if (v.is_null()) {
        return {};
    }
    if (v.is_boolean()) {
        return v.get<bool>() ? "true" : "false";
    }
    return v.dump();
}

/// 控件 → 纯文本说明行 ("标签: 候选/默认 (控件形态)")
std::string controlPlainText(const InterruptUiBlock& b) {
    std::string line;
    if (!b.label.empty()) {
        line  = b.label;
        line += ": ";
    } else {
        line  = b.id.empty() ? std::string{"-"} : b.id;
        line += ": ";
    }
    if (!b.options.empty()) {
        std::string options;
        for (size_t i = 0; i < b.options.size(); ++i) {
            if (i > 0) {
                options += " / ";
            }
            options += jsonValueText(b.options[i].value);
        }
        line += options;
    } else if (b.control == "checkbox") {
        line += b.defaultValue.is_boolean() && b.defaultValue.get<bool>() ? "[x]" : "[ ]";
    } else if (!b.defaultValue.is_null()) {
        line += jsonValueText(b.defaultValue);
    } else {
        line += "(empty)";
    }
    line += fmt::format(" ({})", b.control.empty() ? "unknown" : b.control);
    if (!b.help.empty()) {
        line += " - " + b.help;
    }
    return line;
}

} // namespace

// ---------------------------------------------------------------------------
// 分段 / 头行 / 候选项 / 块 / 描述: JSON 双向
// ---------------------------------------------------------------------------

InterruptUiSegment InterruptUiSegment::fromJson(const Json& j) {
    InterruptUiSegment seg;
    seg.text     = j.value("text", "");
    seg.labelKey = j.value("labelKey", "");
    seg.color    = j.value("color", "");
    seg.bold     = j.value("bold", false);
    seg.dim      = j.value("dim", false);
    return seg;
}

Json InterruptUiSegment::toJson() const {
    auto j = Json::object();
    putIfNotEmpty(j, "text", text);
    putIfNotEmpty(j, "labelKey", labelKey);
    putIfNotEmpty(j, "color", color);
    putIfTrue(j, "bold", bold);
    putIfTrue(j, "dim", dim);
    return j;
}

InterruptUiHeader InterruptUiHeader::fromJson(const Json& j) {
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

Json InterruptUiHeader::toJson() const {
    auto j = Json::object();
    if (!segments.empty()) {
        auto arr = Json::array();
        for (const auto& s : segments) {
            arr.push_back(s.toJson());
        }
        j["segments"] = std::move(arr);
    }
    return j;
}

InterruptUiOption InterruptUiOption::fromJson(const Json& j) {
    InterruptUiOption o;
    if (j.is_object()) {
        auto it = j.find("value");
        o.value = (it != j.end()) ? *it : Json{};
    }
    o.label    = j.value("label", "");
    o.labelKey = j.value("labelKey", "");
    o.color    = j.value("color", "");
    return o;
}

Json InterruptUiOption::toJson() const {
    auto j = Json::object();
    if (!value.is_null()) {
        j["value"] = value;
    }
    putIfNotEmpty(j, "label", label);
    putIfNotEmpty(j, "labelKey", labelKey);
    putIfNotEmpty(j, "color", color);
    return j;
}

InterruptUiBlock InterruptUiBlock::fromJson(const Json& j) {
    InterruptUiBlock b;
    b.kind = j.value("kind", "");
    // 原始 JSON 原样保留: 内容块支持扩展组件 (由前端按 `agentxx.ui.item` schema
    // 解析) 与自定义字段往返, 不经本结构的字段映射
    b.raw = j;

    b.text    = j.value("text", "");
    b.textKey = j.value("textKey", "");
    b.color   = j.value("color", "");
    b.bold    = j.value("bold", false);
    b.dim     = j.value("dim", false);
    b.wrap    = j.value("wrap", false);
    b.indent  = std::max(0, j.value("indent", 0));

    b.lines = std::max(0, j.value("lines", 1));

    b.path   = j.value("path", "");
    b.oldStr = j.value("oldStr", "");
    b.newStr = j.value("newStr", "");

    b.id       = j.value("id", "");
    b.control  = j.value("control", "");
    b.label    = j.value("label", "");
    b.labelKey = j.value("labelKey", "");
    b.help     = j.value("help", "");
    b.helpKey  = j.value("helpKey", "");

    b.options      = jsonOptions(j, "options");
    b.commitOnPick = j.value("commitOnPick", false);
    if (j.is_object()) {
        auto it = j.find("defaultValue");
        if (it != j.end()) {
            b.defaultValue = *it;
        }
    }

    b.integer = j.value("integer", false);
    {
        bool has   = false;
        b.minValue = jsonNumber(j, "min", has);
        b.hasMin   = has;
    }
    {
        bool has   = false;
        b.maxValue = jsonNumber(j, "max", has);
        b.hasMax   = has;
    }
    {
        bool has = false;
        auto v   = jsonNumber(j, "step", has);
        if (has && v > 0) {
            b.step = v;
        }
    }
    b.multiline = j.value("multiline", false);

    b.cancelLabel    = j.value("cancelLabel", "");
    b.cancelLabelKey = j.value("cancelLabelKey", "");

    // custom (预留字段: 解析/序列化往返保留, 渲染暂未实现)
    b.component = j.value("component", "");
    b.fallback  = j.value("fallback", "");
    if (j.is_object()) {
        auto it = j.find("props");
        if (it != j.end()) {
            b.props = *it;
        }
    }
    return b;
}

Json InterruptUiBlock::toJson() const {
    // 以解析时的原始 JSON 为底: 未知 kind 的扩展字段 (如表格的 columns/rows)
    // 原样保留, 已知字段再用当前值覆盖
    auto j = raw.is_object() ? raw : Json::object();
    putIfNotEmpty(j, "kind", kind);

    putIfNotEmpty(j, "text", text);
    putIfNotEmpty(j, "textKey", textKey);
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

    putIfNotEmpty(j, "path", path);
    putIfNotEmpty(j, "oldStr", oldStr);
    putIfNotEmpty(j, "newStr", newStr);

    putIfNotEmpty(j, "id", id);
    putIfNotEmpty(j, "control", control);
    putIfNotEmpty(j, "label", label);
    putIfNotEmpty(j, "labelKey", labelKey);
    putIfNotEmpty(j, "help", help);
    putIfNotEmpty(j, "helpKey", helpKey);

    if (!options.empty()) {
        auto arr = Json::array();
        for (const auto& o : options) {
            arr.push_back(o.toJson());
        }
        j["options"] = std::move(arr);
    }
    if (!defaultValue.is_null()) {
        j["defaultValue"] = defaultValue;
    }
    putIfTrue(j, "commitOnPick", commitOnPick);
    putIfTrue(j, "integer", integer);
    if (hasMin) {
        j["min"] = minValue;
    }
    if (hasMax) {
        j["max"] = maxValue;
    }
    if (step != 1.0) {
        j["step"] = step;
    }
    putIfTrue(j, "multiline", multiline);

    putIfNotEmpty(j, "cancelLabel", cancelLabel);
    putIfNotEmpty(j, "cancelLabelKey", cancelLabelKey);

    putIfNotEmpty(j, "component", component);
    if (!props.is_null()) {
        j["props"] = props;
    }
    putIfNotEmpty(j, "fallback", fallback);
    return j;
}

InterruptUi InterruptUi::fromJson(const Json& j) {
    InterruptUi ui;
    if (!j.is_object()) {
        return ui;
    }
    ui.version = j.value("version", 1);
    auto hIt   = j.find("header");
    if (hIt != j.end() && hIt->is_object()) {
        ui.header = InterruptUiHeader::fromJson(*hIt);
    }
    auto bIt = j.find("blocks");
    if (bIt != j.end() && bIt->is_array()) {
        ui.blocks.reserve(bIt->size());
        for (const auto& block : *bIt) {
            ui.blocks.push_back(InterruptUiBlock::fromJson(block));
        }
    }
    return ui;
}

Json InterruptUi::toJson() const {
    auto j       = Json::object();
    j["version"] = version;
    if (!header.segments.empty()) {
        j["header"] = header.toJson();
    }
    if (!blocks.empty()) {
        auto arr = Json::array();
        for (const auto& b : blocks) {
            arr.push_back(b.toJson());
        }
        j["blocks"] = std::move(arr);
    }
    return j;
}

// ---------------------------------------------------------------------------
// 结果契约与取值
// ---------------------------------------------------------------------------

Json makeInterruptResult(const Json& values) {
    // 结果恒为对象形态 {"values": {...}}; 非对象 values 归一化为空对象
    // (空对象 = 用户未提交/取消, 消费端按未应答处理)
    return Json{
        {"values", values.is_object() ? values : Json::object()},
    };
}

namespace {

/// 取结果值对象: 传入整体结果对象时下钻其 `values`; 传入 values 对象时原样返回
const Json* valuesObjectOf(const Json& j) {
    if (!j.is_object()) {
        return nullptr;
    }
    auto it = j.find("values");
    if (it != j.end() && it->is_object()) {
        return &(*it);
    }
    return &j;
}

const Json* valueOf(const Json& values, std::string_view id) {
    const auto* obj = valuesObjectOf(values);
    if (!obj || id.empty()) {
        return nullptr;
    }
    auto it = obj->find(id);
    return (it == obj->end()) ? nullptr : &(*it);
}

/// 字符串 → 数值 (整数/浮点)
bool parseNumberValue(std::string_view s, double& out) {
    auto trimmed = utilxx_base::removeBetweenSpace(s);
    if (trimmed.empty()) {
        return false;
    }
    double v = 0.0;
    if (utilxx_base::parseNumberFromString(trimmed, v).ec != std::errc{}) {
        return false;
    }
    out = v;
    return true;
}

} // namespace

bool interruptValueBool(const Json& values, std::string_view id, bool defaultValue) {
    const auto* v = valueOf(values, id);
    if (!v) {
        return defaultValue;
    }
    if (v->is_boolean()) {
        return v->get<bool>();
    }
    if (v->is_number()) {
        return v->get<double>() != 0.0;
    }
    if (v->is_string()) {
        // 与 preset::inputForm 的 bool 控件取值口径一致 ("true"/"yes"/"y"/"1")
        auto s = utilxx_base::toLower(utilxx_base::removeBetweenSpace(v->get<std::string>()));
        if (s == "true" || s == "yes" || s == "y" || s == "1") {
            return true;
        }
        if (s == "false" || s == "no" || s == "n" || s == "0") {
            return false;
        }
    }
    return defaultValue;
}

std::string
    interruptValueString(const Json& values, std::string_view id, std::string_view defaultValue) {
    const auto* v = valueOf(values, id);
    if (!v || v->is_null()) {
        return std::string{defaultValue};
    }
    if (v->is_string()) {
        return v->get<std::string>();
    }
    return jsonValueText(*v);
}

int64_t interruptValueInt(const Json& values, std::string_view id, int64_t defaultValue) {
    const auto* v = valueOf(values, id);
    if (!v) {
        return defaultValue;
    }
    if (v->is_number_integer() || v->is_number_unsigned()) {
        return static_cast<int64_t>(v->get<long long>());
    }
    if (v->is_number()) {
        return static_cast<int64_t>(v->get<double>());
    }
    if (v->is_string()) {
        double num = 0.0;
        if (parseNumberValue(v->get<std::string>(), num)) {
            return static_cast<int64_t>(num);
        }
    }
    return defaultValue;
}

double interruptValueDouble(const Json& values, std::string_view id, double defaultValue) {
    const auto* v = valueOf(values, id);
    if (!v) {
        return defaultValue;
    }
    if (v->is_number()) {
        return v->get<double>();
    }
    if (v->is_string()) {
        double num = 0.0;
        if (parseNumberValue(v->get<std::string>(), num)) {
            return num;
        }
    }
    return defaultValue;
}

// ---------------------------------------------------------------------------
// 纯文本降级
// ---------------------------------------------------------------------------

std::string interruptUiPlainText(const InterruptUi& ui, int width) {
    std::vector<std::string> lines;

    auto appendWrapped = [&](std::string_view text, int indent) {
        const std::string pad(static_cast<size_t>(std::max(0, indent)), ' ');
        const int         avail = (width > 0) ? std::max(1, width - std::max(0, indent)) : 0;
        for (const auto& line : wrapToWidth(text, avail)) {
            lines.push_back(pad + line);
        }
    };

    for (const auto& b : ui.blocks) {
        if (b.kind == "text" || b.kind == "markdown") {
            appendWrapped(b.text, b.indent);
        } else if (b.kind == "gap") {
            for (int i = 0; i < b.lines; ++i) {
                lines.emplace_back();
            }
        } else if (b.kind == "separator") {
            lines.push_back(std::string(static_cast<size_t>(std::max(0, b.indent)), ' ') + "---");
        } else if (b.kind == "diff") {
            if (!b.path.empty()) {
                lines.push_back(fmt::format(
                    "{}file: {}",
                    std::string(static_cast<size_t>(b.indent), ' '),
                    b.path
                ));
            }
            const auto diff = utilxx::computeLineDiff(b.oldStr, b.newStr);
            for (const auto& l : diff) {
                char prefix = ' ';
                if (l.type == utilxx::DiffLineType::Add) {
                    prefix = '+';
                } else if (l.type == utilxx::DiffLineType::Delete) {
                    prefix = '-';
                }
                appendWrapped(fmt::format("{}{}", prefix, l.text), b.indent);
            }
        } else if (b.kind == "control") {
            appendWrapped(controlPlainText(b), b.indent);
        } else if (b.kind == "custom") {
            // 预留块: 无 fallback 时输出组件名占位 (便于行式前端提示缺失)
            const auto text
                = !b.fallback.empty()
                      ? b.fallback
                      : (b.component.empty() ? std::string{"[unsupported block]"}
                                             : fmt::format("[custom component: {}]", b.component));
            appendWrapped(text, b.indent);
        }
        // submit: 仅交互语义, 纯文本不输出
        // 未知 kind: 忽略
    }
    return utilxx_base::stringJoin(lines, "\n");
}

} // namespace middleware
} // namespace agentxx
