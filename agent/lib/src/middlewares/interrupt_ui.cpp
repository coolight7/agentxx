#include "agentxx/middlewares/interrupt_ui.h"

#include "pluginxx/ui.h"
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

/// 缩进 (按终端列数) → 描述层的 Padding (长度 u)
///
/// 描述层没有"缩进"字段 (终端专有概念, 见 plan §3.5): 缩进由容器表达,
/// 客户端渲染时按能力段把 u 换算成列/行。
pluginxx::ui::Item withIndentColumns(pluginxx::ui::Item item, int indentCols) {
    if (indentCols <= 0) {
        return item;
    }
    pluginxx::ui::Item padding = pluginxx::ui::build::node("Padding");
    padding.padding            = pluginxx::ui::Edges{
        static_cast<double>(indentCols) * pluginxx::ui::gen::kDefaultCellWidth,
        0.0,
        0.0,
        0.0,
    };
    padding.hasPadding = true;
    padding.children.push_back(std::move(item));
    return padding;
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

    // custom: 组件项数组 + 降级文本 (无内容时输出 fallback)
    b.fallback = j.value("fallback", "");
    if (j.is_object()) {
        auto it = j.find("props");
        if (it != j.end()) {
            b.props = *it;
        }
    }

    b.dataUrl   = j.value("dataUrl", j.value("data_url", ""));
    b.alt       = j.value("alt", "");
    b.maxHeight = static_cast<int>(j.value("maxHeight", j.value("max_height", 0)));

    {
        bool   has = false;
        double v   = jsonNumber(j, "value", has);
        if (has) {
            b.progressValue = v;
        }
        double t = jsonNumber(j, "total", has);
        if (has) {
            b.progressTotal = t;
        }
    }

    b.pathMode = j.value("mode", j.value("pathMode", "file"));
    if (j.contains("filter") && j["filter"].is_array()) {
        for (const auto& f : j["filter"]) {
            if (f.is_string()) {
                b.filter.push_back(f.get<std::string>());
            }
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

    putIfNotEmpty(j, "dataUrl", dataUrl);
    putIfNotEmpty(j, "alt", alt);
    if (maxHeight > 0) {
        j["maxHeight"] = maxHeight;
    }
    if (kind == "progress" || progressValue > 0 || progressTotal > 0) {
        j["value"] = progressValue;
        j["total"] = progressTotal;
    }
    if (control == "path") {
        putIfNotEmpty(j, "mode", pathMode);
        if (!filter.empty()) {
            j["filter"] = filter;
        }
    }

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
        // 与 preset::inputForm 的 bool 控件取值规则一致 ("true"/"yes"/"y"/"1")
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
// 组件桥接 (中断块 ↔ 组件项)
// ---------------------------------------------------------------------------

std::optional<pluginxx::ui::Item> itemOf(const InterruptUiBlock& block) {
    using pluginxx::ui::Item;
    using pluginxx::ui::TextValue;

    // 文案: i18n 键优先, 字面文本作回退 (描述层统一用 TextValue 表达)
    const auto textOf = [](const std::string& key, const std::string& text) {
        TextValue value;
        value.key      = key;
        value.fallback = text;
        return value;
    };
    // 空行数 (中断描述里按终端行算) → 长度 u: 一行 ≈ 能力段默认格高
    const auto gapSizeOf = [](int lines) {
        return pluginxx::ui::SizeValue::of(
            static_cast<double>(std::max(0, lines)) * pluginxx::ui::gen::kDefaultCellHeight
        );
    };

    const int indentCols = std::max(0, block.indent);
    const auto withIndent = [indentCols](Item item) {
        return withIndentColumns(std::move(item), indentCols);
    };

    Item item;
    item.tone = block.color;
    item.bold   = block.bold;
    item.dim    = block.dim;
    item.wrap   = block.wrap;

    if (block.kind == "text") {
        item.kind = "Text";
        item.text = textOf(block.textKey, block.text);
        return withIndent(std::move(item));
    }
    if (block.kind == "markdown") {
        item.kind     = "Markdown";
        item.markdown = block.text;
        return withIndent(std::move(item));
    }
    if (block.kind == "diff") {
        item.kind   = "Diff";
        item.path   = block.path;
        item.oldStr = block.oldStr;
        item.newStr = block.newStr;
        return withIndent(std::move(item));
    }
    if (block.kind == "image") {
        // 组件层没有"图片"元素: 降级为一行文本 —— 优先替代文本, 其次路径, 最后数据源
        // (都没有时输出占位, 不静默丢内容)
        item.kind          = "Text";
        std::string detail = block.alt;
        if (detail.empty()) {
            detail = block.path;
        }
        if (detail.empty() && !block.dataUrl.empty()) {
            detail = "内嵌图片";
        }
        if (detail.empty() && !block.fallback.empty()) {
            detail = block.fallback;
        }
        item.text = textOf(
            "",
            detail.empty() ? std::string{"[图片]"} : fmt::format("[图片: {}]", detail)
        );
        return withIndent(std::move(item));
    }
    if (block.kind == "progress") {
        item.kind       = "Text";
        std::string lbl = block.label.empty() ? (block.text.empty() ? "" : block.text) : block.label;
        std::string textStr;
        if (block.progressTotal > 0.0) {
            if (!lbl.empty()) {
                textStr = fmt::format("[进度: {}/{} {}]", block.progressValue, block.progressTotal, lbl);
            } else {
                textStr = fmt::format("[进度: {}/{}]", block.progressValue, block.progressTotal);
            }
        } else {
            if (!lbl.empty()) {
                textStr = fmt::format("[进度: {} {}]", block.progressValue, lbl);
            } else {
                textStr = fmt::format("[进度: {}]", block.progressValue);
            }
        }
        item.text = textOf(block.labelKey.empty() ? block.textKey : block.labelKey, textStr);
        return withIndent(std::move(item));
    }
    if (block.kind == "separator") {
        item.kind = "Divider";
        return withIndent(std::move(item));
    }
    if (block.kind == "gap") {
        item.kind = "Gap";
        item.size = gapSizeOf(block.lines);
        return withIndent(std::move(item));
    }
    if (block.kind == "control") {
        if (block.control == "path") {
            item.kind          = "Text";
            std::string prompt = block.label.empty() ? block.id : block.label;
            item.text          = textOf(
                "",
                fmt::format(
                    "{}: [路径输入 ({})]",
                    prompt,
                    block.pathMode.empty() ? "file" : block.pathMode
                )
            );
            return withIndent(std::move(item));
        }
        item.kind      = "Control";
        item.id        = block.id;
        item.control   = block.control;
        item.label     = textOf(block.labelKey, block.label);
        item.help      = textOf(block.helpKey, block.help);
        item.valueJson = block.defaultValue.is_null() ? std::string{} : block.defaultValue.dump();
        item.integer   = block.integer;
        item.multiline = block.multiline;
        item.hasMin    = block.hasMin;
        item.minValue  = block.minValue;
        item.hasMax    = block.hasMax;
        item.maxValue  = block.maxValue;
        item.hasStep   = block.step > 0.0;
        item.step      = block.step;
        for (const auto& opt : block.options) {
            pluginxx::ui::ControlOption out;
            out.valueJson = opt.value.is_null() ? std::string{} : opt.value.dump();
            out.label = textOf(
                opt.labelKey,
                opt.label.empty() ? jsonValueText(opt.value) : opt.label
            );
            out.tone = opt.color;
            item.options.push_back(std::move(out));
        }
        // 注: `commitOnPick` 不进描述层 —— 它是"点击候选项即提交整份表单"的
        // **域内约定** (中断表单提交整份表单; 描述层只表达"值变化即派发")
        return withIndent(std::move(item));
    }
    if (block.kind == "submit") {
        // 确认/取消行: 域内用两个按钮表达 —— 描述层没有"表单提交"这一层
        // (见 plan §3.4), 点击后经动作通道回传 __submit / __cancel, 由中断表单处理
        const auto makeButton
            = [](pluginxx::ui::TextValue label, std::string_view actionId, std::string_view variant) {
                  Item out = pluginxx::ui::build::button(
                      std::string_view{},
                      pluginxx::ui::build::dispatch(std::string{actionId}),
                      variant
                  );
                  out.label = std::move(label);
                  return out;
              };
        Item confirm = makeButton(
            textOf(block.labelKey.empty() ? std::string{"interrupt.confirm"} : block.labelKey,
                   block.label),
            kInterruptSubmitActionId,
            "primary"
        );
        Item cancel = makeButton(
            textOf(block.cancelLabelKey.empty() ? std::string{"interrupt.cancel"}
                                                : block.cancelLabelKey,
                   block.cancelLabel),
            kInterruptCancelActionId,
            "secondary"
        );
        Item row   = pluginxx::ui::build::row({std::move(confirm), std::move(cancel)});
        row.hasGap = true;
        row.gap    = pluginxx::ui::SizeValue::of(pluginxx::ui::gen::kDefaultCellWidth * 2.0);
        row.main   = "start";
        return withIndent(std::move(row));
    }
    if (block.kind == "custom") {
        // 自定义块已并入描述层: `props.items` 作为组件树展开, 其余按 fallback 文本
        if (block.props.is_object() && block.props.contains("items")) {
            auto parsed = pluginxx::ui::parseBlocks(block.props);
            if (!parsed.empty()) {
                Item row     = pluginxx::ui::build::column({});
                row.children = std::move(parsed);
                return withIndent(std::move(row));
            }
        }
        if (!block.fallback.empty()) {
            return withIndent(pluginxx::ui::build::caption(block.fallback));
        }
        return std::nullopt;
    }
    // 扩展组件 (表格/树/横排/分组/趋势图等): 块描述即组件描述, 按原始 JSON 解析
    if (block.raw.is_object() && block.raw.contains("kind")) {
        auto parsed = pluginxx::ui::parseBlock(block.raw);
        if (parsed.known) {
            return parsed;
        }
    }
    return std::nullopt;
}

InterruptUiBlock blockOf(const pluginxx::ui::Item& item) {
    using pluginxx::ui::TextValue;

    // 缩进在描述层没有字段, 用 Padding 容器表达 (itemOf 的 indent → Padding 是反向):
    // 这里解包还原成域的 `indent` 列数, 否则本函数会把带缩进的块当成"未知组件"
    // 原样放进 raw, 域的 text/color 等字段全部丢失 (往返与纯文本降级都会受影响)
    // - 只解包"纯左缩进"的容器 (withIndent 的产物); 其余边有留白时按普通容器保留
    if (item.kind == "Padding" && item.children.size() == 1) {
        constexpr double kCellWidth = pluginxx::ui::gen::kDefaultCellWidth;
        const double     left       = item.hasPadding ? item.padding.left : 0.0;
        const bool       pureIndent = left > 0.0 && item.padding.top <= 0.0
                                && item.padding.right <= 0.0 && item.padding.bottom <= 0.0;
        if (pureIndent && kCellWidth > 0.0) {
            InterruptUiBlock out = blockOf(item.children.front());
            out.indent += static_cast<int>(std::lround(left / kCellWidth));
            return out;
        }
    }

    // TextValue → (i18n 键, 字面文本)
    const auto splitText = [](const TextValue& value, std::string& key, std::string& text) {
        key  = value.key;
        text = value.fallback;
    };

    InterruptUiBlock b;
    b.kind  = item.kind.empty() ? std::string{"text"} : item.kind;
    b.color = item.tone;
    b.bold   = item.bold;
    b.dim    = item.dim;
    b.wrap   = item.wrap;

    if (item.kind == "Text" || item.kind.empty()) {
        b.kind = "text";
        splitText(item.text, b.textKey, b.text);
    } else if (item.kind == "Markdown") {
        b.kind = "markdown";
        b.text = item.markdown;
    } else if (item.kind == "Diff") {
        b.kind   = "diff";
        b.path   = item.path;
        b.oldStr = item.oldStr;
        b.newStr = item.newStr;
    } else if (item.kind == "Divider") {
        b.kind = "separator";
    } else if (item.kind == "Gap") {
        b.kind  = "gap";
        b.lines = std::max(
            0,
            static_cast<int>(
                std::lround(item.size.value / pluginxx::ui::gen::kDefaultCellHeight)
            )
        );
    } else if (item.kind == "Control") {
        b.kind   = "control";
        b.id     = item.id;
        b.control = item.control;
        splitText(item.label, b.labelKey, b.label);
        splitText(item.help, b.helpKey, b.help);
        b.defaultValue = item.valueJson.empty() ? Json{} : Json::parse(item.valueJson);
        b.integer      = item.integer;
        b.multiline    = item.multiline;
        b.hasMin       = item.hasMin;
        b.minValue     = item.minValue;
        b.hasMax       = item.hasMax;
        b.maxValue     = item.maxValue;
        b.step         = (item.hasStep && item.step > 0.0) ? item.step : 1.0;
        b.commitOnPick = false; // 域内约定, 不写入描述
        for (const auto& opt : item.options) {
            InterruptUiOption out;
            out.value = opt.valueJson.empty() ? Json{} : Json::parse(opt.valueJson);
            splitText(opt.label, out.labelKey, out.label);
            out.color = opt.tone;
            b.options.push_back(std::move(out));
        }
    } else {
        // 扩展组件: 整项按描述层的 JSON 形态原样带走 (渲染/纯文本按 raw 解释)
        b.raw = pluginxx::ui::dumpItem(item);
    }
    return b;
}

// ---------------------------------------------------------------------------
// 纯文本降级
// ---------------------------------------------------------------------------

std::string interruptUiPlainText(const InterruptUi& ui, int width) {
    // 统一走描述层的纯文本降级: 与 TUI 渲染同一套组件语义 (表格/树/图表不再被丢弃)
    std::vector<pluginxx::ui::Item> items;
    items.reserve(ui.blocks.size());
    for (const auto& block : ui.blocks) {
        if (block.kind == "submit") {
            continue; // 提交行只有交互语义, 纯文本不输出
        }
        if (auto item = itemOf(block)) {
            items.push_back(std::move(*item));
            continue;
        }
        if (!block.fallback.empty()) {
            items.push_back(withIndentColumns(
                pluginxx::ui::build::caption(block.fallback),
                std::max(0, block.indent)
            ));
        }
    }
    return pluginxx::ui::plainText(items, width);
}

} // namespace middleware
} // namespace agentxx
