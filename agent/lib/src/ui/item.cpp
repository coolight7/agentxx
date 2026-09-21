#include "agentxx/ui/item.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>

namespace agentxx {
namespace ui {

using utilxx_base::Json;

namespace {

// ---------------------------------------------------------------------------
// 字段读取 (容错: 类型不符时取缺省值, 不抛异常)
// ---------------------------------------------------------------------------

/// 取对象的某个字段 (不存在返回 nullptr; 存在但为 null 也返回 nullptr)
const Json* field(const Json& j, std::string_view key) {
    if (!j.is_object() || !j.contains(key)) {
        return nullptr;
    }
    const Json& v = j.at(key);
    return v.is_null() ? nullptr : &v;
}

std::string readString(const Json& j, std::string_view key, std::string_view def = {}) {
    const Json* v = field(j, key);
    if (v == nullptr) {
        return std::string{def};
    }
    if (v->is_string()) {
        return std::string{v->get_string_view()};
    }
    if (v->is_bool()) {
        return v->get<bool>() ? std::string{"true"} : std::string{"false"};
    }
    if (v->is_number()) {
        return v->dump();
    }
    return std::string{def};
}

/// 依次尝试多个键 (新写法在前, 旧写法在后)
std::string readStringAny(
    const Json&                    j,
    std::initializer_list<std::string_view> keys,
    std::string_view               def = {}
) {
    for (auto key : keys) {
        const Json* v = field(j, key);
        if (v != nullptr && (v->is_string() || v->is_number() || v->is_bool())) {
            return readString(j, key, def);
        }
    }
    return std::string{def};
}

double readDouble(const Json& j, std::string_view key, double def) {
    const Json* v = field(j, key);
    if (v == nullptr || !v->is_number()) {
        return def;
    }
    return v->get<double>();
}

int readInt(const Json& j, std::string_view key, int def) {
    const Json* v = field(j, key);
    if (v == nullptr) {
        return def;
    }
    if (v->is_number()) {
        return static_cast<int>(v->get<double>());
    }
    return def;
}

bool readBool(const Json& j, std::string_view key, bool def) {
    const Json* v = field(j, key);
    if (v == nullptr) {
        return def;
    }
    if (v->is_bool()) {
        return v->get<bool>();
    }
    return def;
}

/// 数字字段回退: 主键缺失时尝试备用键
double readDoubleAny(const Json& j, std::initializer_list<std::string_view> keys, double def) {
    for (auto key : keys) {
        if (field(j, key) != nullptr) {
            return readDouble(j, key, def);
        }
    }
    return def;
}

std::string clampTextImpl(std::string_view text, size_t maxBytes) {
    if (text.size() <= maxBytes) {
        return std::string{text};
    }
    // 按字节截断时不能把 UTF-8 码点切一半: 回退到最后一个完整码点边界
    size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return std::string{text.substr(0, cut)};
}

/// 判定 kind 字符串是否被本版识别
bool isKnownKind(std::string_view kind) {
    return kind == "text" || kind == "markdown" || kind == "diff" || kind == "separator"
           || kind == "gap" || kind == "button" || kind == "action" || kind == "progress"
           || kind == "meter" || kind == "badge" || kind == "diagram" || kind == "row"
           || kind == "box" || kind == "collapse" || kind == "table" || kind == "tree"
           || kind == "kv" || kind == "sparkline" || kind == "control" || kind == "submit"
           || kind == "canvas" || kind == "custom";
}

std::string alignOf(const Json& j, std::initializer_list<std::string_view> keys) {
    const std::string a = readStringAny(j, keys, "left");
    return (a == "center" || a == "right" || a == "stretch") ? a : std::string{"left"};
}

// ---------------------------------------------------------------------------
// 子结构解析
// ---------------------------------------------------------------------------

void parseTreeNode(const Json& j, TreeNode& node, const ParseLimits& limits, int depth, size_t& budget) {
    if (!j.is_object() || budget == 0) {
        return;
    }
    --budget;
    node.label = clampTextImpl(readStringAny(j, {"label", "text"}), limits.maxTextBytes);
    node.color = readString(j, "color", "normal");
    node.action = readStringAny(j, {"action"});
    if (const Json* args = field(j, "args")) {
        node.args = *args;
    }
    const Json* children = field(j, "children");
    if (children != nullptr && children->is_array() && depth + 1 <= limits.maxDepth) {
        for (const auto& child : *children) {
            if (budget == 0) {
                break;
            }
            node.children.emplace_back();
            parseTreeNode(child, node.children.back(), limits, depth + 1, budget);
        }
    }
}

TableCell parseTableCell(const Json& j, const ParseLimits& limits) {
    TableCell cell;
    if (j.is_string()) {
        cell.text = clampTextImpl(j.get_string_view(), limits.maxTextBytes);
        return cell;
    }
    if (!j.is_object()) {
        return cell;
    }
    cell.text   = clampTextImpl(readStringAny(j, {"text", "label", "value"}), limits.maxTextBytes);
    cell.color  = readString(j, "color", readString(j, "role", ""));
    cell.action = readStringAny(j, {"action"});
    if (const Json* args = field(j, "args")) {
        cell.args = *args;
    }
    return cell;
}

std::vector<Item>
    parseItemArray(const Json& arr, const ParseLimits& limits, int depth) {
    std::vector<Item> out;
    if (!arr.is_array()) {
        return out;
    }
    const size_t n = std::min(arr.size(), limits.maxItems);
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(parseItem(arr[i], limits, depth));
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// TreeNode / Item 成员
// ---------------------------------------------------------------------------

size_t TreeNode::count() const {
    size_t total = 1;
    for (const auto& child : children) {
        total += child.count();
    }
    return total;
}

bool Item::interactive() const {
    if (kind == "control" || kind == "submit") {
        return true;
    }
    if (!action.empty()) {
        return true;
    }
    for (const auto& child : items) {
        if (child.interactive()) {
            return true;
        }
    }
    for (const auto& row : rows) {
        for (const auto& cell : row) {
            if (!cell.action.empty()) {
                return true;
            }
        }
    }
    for (const auto& node : nodes) {
        if (!node.action.empty()) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 解析
// ---------------------------------------------------------------------------

Item parseItem(const Json& json, const ParseLimits& limits, int depth) {
    Item item;
    if (!json.is_object()) {
        item.known = false;
        return item;
    }
    item.kind = readString(json, "kind", "text");
    if (!isKnownKind(item.kind)) {
        item.known    = false;
        item.fallback = clampTextImpl(readString(json, "fallback"), limits.maxTextBytes);
        return item;
    }
    // 旧写法归一化: action 等同 button, progress 等同 meter
    if (item.kind == "action") {
        item.kind = "button";
    }
    if (item.kind == "progress") {
        item.kind = "meter";
    }

    item.id     = clampTextImpl(readString(json, "id"), 256);
    item.indent = std::clamp(readInt(json, "indent", 0), 0, 200);
    item.color  = readString(json, "color", readString(json, "role", "normal"));
    item.bold   = readBool(json, "bold", item.color == "title");
    item.dim    = readBool(json, "dim", item.color == "hint");
    item.fallback = clampTextImpl(readString(json, "fallback"), limits.maxTextBytes);
    item.action   = clampTextImpl(readStringAny(json, {"action"}), 256);
    if (const Json* args = field(json, "args")) {
        item.args = *args;
    }
    // 文本类缺省折行 (与历史渲染语义一致); 结构化组件不折行
    item.wrap = readBool(json, "wrap", item.kind == "text");

    if (item.kind == "text") {
        item.text = clampTextImpl(readStringAny(json, {"text", "content"}), limits.maxTextBytes);
        return item;
    }
    if (item.kind == "markdown") {
        item.text = clampTextImpl(readString(json, "text"), limits.maxTextBytes);
        return item;
    }
    if (item.kind == "diff") {
        item.path   = readString(json, "path");
        item.oldStr = clampTextImpl(
            readStringAny(json, {"old_str", "oldStr", "old"}),
            limits.maxTextBytes
        );
        item.newStr = clampTextImpl(
            readStringAny(json, {"new_str", "newStr", "new"}),
            limits.maxTextBytes
        );
        return item;
    }
    if (item.kind == "separator") {
        return item;
    }
    if (item.kind == "gap") {
        item.lines = std::clamp(readInt(json, "lines", 1), 0, 50);
        return item;
    }
    if (item.kind == "button") {
        // 旧写法: kind=button 用 action_id; kind=action 用 id; 新写法统一用 action
        item.label = clampTextImpl(
            readStringAny(json, {"label", "text"}, "Button"),
            limits.maxTextBytes
        );
        if (item.label.empty()) {
            item.label = "Button";
        }
        if (item.action.empty()) {
            item.action = readString(json, "action_id", readString(json, "id", ""));
        }
        item.prefix = readString(json, "prefix");
        item.style  = readString(json, "style", "solid");
        return item;
    }
    if (item.kind == "badge") {
        item.text = clampTextImpl(readStringAny(json, {"text", "label"}), limits.maxTextBytes);
        return item;
    }
    if (item.kind == "diagram") {
        item.mermaid = clampTextImpl(readString(json, "mermaid"), limits.maxTextBytes);
        return item;
    }
    if (item.kind == "meter") {
        const bool legacyProgress = readString(json, "kind", "text") == "progress";
        item.value     = readDouble(json, "value", 0.0);
        item.total     = readDouble(json, "total", legacyProgress ? 1.0 : 100.0);
        if (legacyProgress) {
            // 旧进度条 value 为 0..1 的比例: 归一化到 meter 的绝对值口径
            item.value = item.value * 100.0;
            item.total = 100.0;
        }
        if (item.total <= 0) {
            item.total = 100.0;
        }
        item.width     = std::max(0, readInt(json, "width", 0));
        item.label     = readStringAny(json, {"label"});
        item.unit      = readString(json, "unit", legacyProgress ? "%" : "");
        item.showValue = readBool(json, "showValue", true);
        if (const Json* th = field(json, "thresholds"); th != nullptr && th->is_array()) {
            for (const auto& t : *th) {
                if (!t.is_object()) {
                    continue;
                }
                MeterThreshold mt;
                mt.at    = readDouble(t, "at", 0.0);
                mt.color = readString(t, "color", "accent");
                item.thresholds.push_back(std::move(mt));
            }
            std::stable_sort(
                item.thresholds.begin(),
                item.thresholds.end(),
                [](const MeterThreshold& a, const MeterThreshold& b) {
                    return a.at > b.at; // 由高到低: 匹配首个满足项
                }
            );
        }
        return item;
    }
    if (item.kind == "sparkline") {
        if (const Json* data = field(json, "data"); data != nullptr && data->is_array()) {
            const size_t n = std::min(data->size(), limits.maxDataPoints);
            item.data.reserve(n);
            for (size_t i = 0; i < n; ++i) {
                const Json& v = (*data)[i];
                if (v.is_number()) {
                    item.data.push_back(v.get<double>());
                }
            }
        }
        item.height     = std::clamp(readInt(json, "height", 1), 1, 8);
        item.sparkStyle = readString(json, "style", "block");
        if (field(json, "min") != nullptr) {
            item.hasMin   = true;
            item.minValue = readDouble(json, "min", 0.0);
        }
        if (field(json, "max") != nullptr) {
            item.hasMax   = true;
            item.maxValue = readDouble(json, "max", 0.0);
        }
        item.showLast = readBool(json, "showLast", false);
        item.label    = readStringAny(json, {"label"});
        item.unit     = readString(json, "unit", "");
        if (const Json* colors = field(json, "colors");
            colors != nullptr && colors->is_array()) {
            for (const auto& c : *colors) {
                if (c.is_string()) {
                    item.colors.emplace_back(c.get_string_view());
                }
            }
        }
        return item;
    }
    if (item.kind == "row") {
        item.gap   = std::clamp(readInt(json, "gap", 1), 0, 20);
        item.align = alignOf(json, {"align"});
        if (const Json* colWidth = field(json, "w")) {
            if (colWidth->is_number()) {
                item.columnWidth = std::max(0, static_cast<int>(colWidth->get<double>()));
            } else if (colWidth->is_string() && colWidth->get_string_view() == "flex") {
                item.columnFlex = true;
            }
        }
        if (depth + 1 <= limits.maxDepth) {
            if (const Json* items = field(json, "items")) {
                item.items = parseItemArray(*items, limits, depth + 1);
            }
        }
        return item;
    }
    if (item.kind == "box") {
        item.title      = clampTextImpl(readStringAny(json, {"title"}), limits.maxTextBytes);
        item.border     = readString(json, "border", "none");
        item.titleColor = readString(json, "titleColor", "");
        item.pad        = std::clamp(readInt(json, "pad", 0), 0, 8);
        if (const Json* colWidth = field(json, "w")) {
            if (colWidth->is_number()) {
                item.columnWidth = std::max(0, static_cast<int>(colWidth->get<double>()));
            } else if (colWidth->is_string() && colWidth->get_string_view() == "flex") {
                item.columnFlex = true;
            }
        }
        if (depth + 1 <= limits.maxDepth) {
            if (const Json* items = field(json, "items")) {
                item.items = parseItemArray(*items, limits, depth + 1);
            }
        }
        return item;
    }
    if (item.kind == "collapse") {
        item.title    = clampTextImpl(readStringAny(json, {"title", "label"}), limits.maxTextBytes);
        item.expanded = readBool(json, "expanded", true);
        if (depth + 1 <= limits.maxDepth) {
            if (const Json* items = field(json, "items")) {
                item.items = parseItemArray(*items, limits, depth + 1);
            }
        }
        return item;
    }
    if (item.kind == "table") {
        item.header = readBool(json, "header", true);
        if (const Json* cols = field(json, "columns"); cols != nullptr && cols->is_array()) {
            const size_t n = std::min(cols->size(), static_cast<size_t>(limits.maxTableColumns));
            for (size_t i = 0; i < n; ++i) {
                const Json& cj = (*cols)[i];
                TableColumn col;
                if (cj.is_string()) {
                    col.title = clampTextImpl(cj.get_string_view(), limits.maxTextBytes);
                } else {
                    col.title = clampTextImpl(readStringAny(cj, {"title", "text"}), limits.maxTextBytes);
                    col.align = alignOf(cj, {"align"});
                    col.color = readString(cj, "color", "");
                    if (const Json* w = field(cj, "w")) {
                        if (w->is_number()) {
                            col.width = std::max(0, static_cast<int>(w->get<double>()));
                        } else if (w->is_string() && w->get_string_view() == "flex") {
                            col.flex = true;
                        }
                    }
                }
                item.columns.push_back(std::move(col));
            }
        }
        if (const Json* rows = field(json, "rows"); rows != nullptr && rows->is_array()) {
            const size_t n = std::min(rows->size(), limits.maxTableRows);
            item.rows.reserve(n);
            for (size_t i = 0; i < n; ++i) {
                const Json& rj = (*rows)[i];
                std::vector<TableCell> row;
                if (rj.is_array()) {
                    const size_t m = std::min(rj.size(), static_cast<size_t>(limits.maxTableColumns));
                    row.reserve(m);
                    for (size_t k = 0; k < m; ++k) {
                        row.push_back(parseTableCell(rj[k], limits));
                    }
                }
                item.rows.push_back(std::move(row));
            }
        }
        // 未声明 columns 时按首行推断列数 (只有 rows 的简写)
        if (item.columns.empty() && !item.rows.empty()) {
            for (size_t i = 0; i < item.rows[0].size(); ++i) {
                TableColumn col;
                col.flex = (i == 0);
                item.columns.push_back(std::move(col));
            }
        }
        return item;
    }
    if (item.kind == "tree") {
        item.connector = readBool(json, "connector", true);
        size_t budget  = limits.maxTreeNodes;
        if (const Json* nodes = field(json, "nodes"); nodes != nullptr && nodes->is_array()) {
            for (const auto& nj : *nodes) {
                if (budget == 0) {
                    break;
                }
                item.nodes.emplace_back();
                parseTreeNode(nj, item.nodes.back(), limits, depth + 1, budget);
            }
        }
        return item;
    }
    if (item.kind == "kv") {
        item.sep      = readString(json, "sep", " : ");
        item.keyWidth = std::max(0, readInt(json, "kw", 0));
        if (const Json* pairs = field(json, "items"); pairs != nullptr && pairs->is_array()) {
            const size_t n = std::min(pairs->size(), limits.maxItems);
            for (size_t i = 0; i < n; ++i) {
                const Json& pj = (*pairs)[i];
                if (!pj.is_object()) {
                    continue;
                }
                KeyValuePair kv;
                kv.key = clampTextImpl(readStringAny(pj, {"k", "key"}), limits.maxTextBytes);
                // 值可以是任意 JSON: 数值/布尔转文本, 对象取 text 字段
                if (const Json* v = field(pj, "v"); v != nullptr) {
                    kv.value = clampTextImpl(
                        v->is_string() ? std::string{v->get_string_view()} : v->dump(),
                        limits.maxTextBytes
                    );
                } else if (const Json* v2 = field(pj, "value"); v2 != nullptr) {
                    kv.value = clampTextImpl(
                        v2->is_string() ? std::string{v2->get_string_view()} : v2->dump(),
                        limits.maxTextBytes
                    );
                }
                kv.keyColor   = readString(pj, "kColor", readString(pj, "keyColor", ""));
                kv.valueColor = readString(pj, "vColor", readString(pj, "valueColor", ""));
                item.pairs.push_back(std::move(kv));
            }
        }
        return item;
    }
    if (item.kind == "control") {
        item.control      = readString(json, "control", "text");
        item.controlLabel = clampTextImpl(readStringAny(json, {"label"}), limits.maxTextBytes);
        item.help         = clampTextImpl(readString(json, "help"), limits.maxTextBytes);
        if (const Json* def = field(json, "default")) {
            item.defaultValue = *def;
        }
        item.commitOnPick = readBool(json, "commitOnPick", false);
        item.integer      = readBool(json, "integer", false);
        if (field(json, "min") != nullptr) {
            item.hasNumMin = true;
            item.numMin    = readDouble(json, "min", 0.0);
        }
        if (field(json, "max") != nullptr) {
            item.hasNumMax = true;
            item.numMax    = readDouble(json, "max", 0.0);
        }
        item.step      = readDouble(json, "step", 1.0);
        item.multiline = readBool(json, "multiline", false);
        if (const Json* opts = field(json, "options"); opts != nullptr && opts->is_array()) {
            const size_t n = std::min(opts->size(), limits.maxItems);
            for (size_t i = 0; i < n; ++i) {
                const Json& oj = (*opts)[i];
                ControlOption opt;
                if (oj.is_string()) {
                    opt.value = oj;
                    opt.label = std::string{oj.get_string_view()};
                } else if (oj.is_object()) {
                    if (const Json* v = field(oj, "value")) {
                        opt.value = *v;
                    }
                    opt.label = clampTextImpl(
                        readStringAny(oj, {"label", "text"}),
                        limits.maxTextBytes
                    );
                    if (opt.label.empty() && opt.value.is_string()) {
                        opt.label = std::string{opt.value.get_string_view()};
                    }
                    opt.color = readString(oj, "color", "");
                } else {
                    continue;
                }
                item.options.push_back(std::move(opt));
            }
        }
        // 旧写法: control 块用 text 存放候选值以外的默认文本
        if (item.controlLabel.empty()) {
            item.controlLabel = clampTextImpl(readString(json, "text"), limits.maxTextBytes);
        }
        return item;
    }
    if (item.kind == "submit") {
        item.label       = clampTextImpl(readStringAny(json, {"label"}), limits.maxTextBytes);
        item.cancelLabel = clampTextImpl(readString(json, "cancelLabel"), limits.maxTextBytes);
        return item;
    }
    if (item.kind == "custom") {
        item.component = readString(json, "component", "");
        if (const Json* props = field(json, "props")) {
            item.props = *props;
        }
        // props.items 是组件树写法; 顺带把 items 暴露出来便于调用方统一处理
        if (item.props.is_object()) {
            if (const Json* propsItems = field(item.props, "items");
                propsItems != nullptr && propsItems->is_array()
                && depth + 1 <= limits.maxDepth) {
                item.items = parseItemArray(*propsItems, limits, depth + 1);
            }
        }
        return item;
    }
    // canvas: 原样保留, 渲染走 fallback (本版不做自绘)
    item.canvas = json;
    return item;
}

Item parseItem(const Json& json) {
    return parseItem(json, kDefaultParseLimits, 0);
}
std::vector<Item> parseItems(const Json& json, const ParseLimits& limits) {
    return parseItemArray(json, limits, 0);
}

std::vector<Item> parseItemList(const Json& json, const ParseLimits& limits) {
    if (json.is_array()) {
        return parseItemArray(json, limits, 0);
    }
    if (json.is_object()) {
        if (const Json* items = field(json, "items")) {
            return parseItemArray(*items, limits, 0);
        }
    }
    return {};
}

std::string clampText(std::string_view text, size_t maxBytes) {
    return clampTextImpl(text, maxBytes);
}

// ---------------------------------------------------------------------------
// 序列化
// ---------------------------------------------------------------------------

Json dumpItem(const Item& item) {
    Json out = Json::object();
    out["kind"] = item.kind;
    if (!item.id.empty()) {
        out["id"] = item.id;
    }
    if (item.indent != 0) {
        out["indent"] = item.indent;
    }
    if (item.color != "normal") {
        out["color"] = item.color;
    }
    if (item.bold) {
        out["bold"] = true;
    }
    if (item.dim) {
        out["dim"] = true;
    }
    if (item.wrap) {
        out["wrap"] = true;
    }
    if (!item.fallback.empty()) {
        out["fallback"] = item.fallback;
    }
    if (!item.action.empty()) {
        out["action"] = item.action;
    }
    if (!item.args.is_null()) {
        out["args"] = item.args;
    }

    if (item.kind == "text" || item.kind == "markdown" || item.kind == "badge") {
        if (!item.text.empty()) {
            out["text"] = item.text;
        }
    } else if (item.kind == "gap") {
        out["lines"] = item.lines;
    } else if (item.kind == "button") {
        out["label"] = item.label;
        if (!item.prefix.empty()) {
            out["prefix"] = item.prefix;
        }
        if (item.style != "solid") {
            out["style"] = item.style;
        }
    } else if (item.kind == "diff") {
        out["path"]    = item.path;
        out["old_str"] = item.oldStr;
        out["new_str"] = item.newStr;
    } else if (item.kind == "diagram") {
        out["mermaid"] = item.mermaid;
    } else if (item.kind == "meter") {
        out["value"] = item.value;
        out["total"] = item.total;
        if (item.width > 0) {
            out["width"] = item.width;
        }
        if (!item.label.empty()) {
            out["label"] = item.label;
        }
        if (!item.unit.empty()) {
            out["unit"] = item.unit;
        }
        if (!item.thresholds.empty()) {
            Json th = Json::array();
            for (const auto& t : item.thresholds) {
                th.push_back(Json::object({{"at", Json{t.at}}, {"color", Json{t.color}}}));
            }
            out["thresholds"] = std::move(th);
        }
    } else if (item.kind == "sparkline") {
        Json data = Json::array();
        for (double d : item.data) {
            data.push_back(Json{d});
        }
        out["data"] = std::move(data);
        if (item.height != 1) {
            out["height"] = item.height;
        }
        if (item.sparkStyle != "block") {
            out["style"] = item.sparkStyle;
        }
        if (item.hasMin) {
            out["min"] = item.minValue;
        }
        if (item.hasMax) {
            out["max"] = item.maxValue;
        }
        if (item.showLast) {
            out["showLast"] = true;
        }
        if (!item.label.empty()) {
            out["label"] = item.label;
        }
        if (!item.unit.empty()) {
            out["unit"] = item.unit;
        }
        if (!item.colors.empty()) {
            Json colors = Json::array();
            for (const auto& c : item.colors) {
                colors.push_back(Json{c});
            }
            out["colors"] = std::move(colors);
        }
    } else if (item.kind == "row" || item.kind == "box" || item.kind == "collapse") {
        if (item.kind == "row") {
            if (item.gap != 1) {
                out["gap"] = item.gap;
            }
            if (item.align != "left") {
                out["align"] = item.align;
            }
        } else if (item.kind == "box") {
            if (!item.title.empty()) {
                out["title"] = item.title;
            }
            if (item.border != "none") {
                out["border"] = item.border;
            }
            if (item.pad != 0) {
                out["pad"] = item.pad;
            }
            if (!item.titleColor.empty()) {
                out["titleColor"] = item.titleColor;
            }
        } else {
            if (!item.title.empty()) {
                out["title"] = item.title;
            }
            if (!item.expanded) {
                out["expanded"] = false;
            }
        }
        Json sub = Json::array();
        for (const auto& child : item.items) {
            sub.push_back(dumpItem(child));
        }
        out["items"] = std::move(sub);
    } else if (item.kind == "table") {
        out["header"] = item.header;
        Json cols     = Json::array();
        for (const auto& c : item.columns) {
            Json cj = Json::object();
            if (!c.title.empty()) {
                cj["title"] = c.title;
            }
            if (c.align != "left") {
                cj["align"] = c.align;
            }
            if (c.flex) {
                cj["w"] = "flex";
            } else if (c.width > 0) {
                cj["w"] = c.width;
            }
            if (!c.color.empty()) {
                cj["color"] = c.color;
            }
            cols.push_back(std::move(cj));
        }
        out["columns"] = std::move(cols);
        Json rows      = Json::array();
        for (const auto& row : item.rows) {
            Json rj = Json::array();
            for (const auto& cell : row) {
                Json cj = Json::object();
                cj["text"] = cell.text;
                if (!cell.color.empty()) {
                    cj["color"] = cell.color;
                }
                if (!cell.action.empty()) {
                    cj["action"] = cell.action;
                }
                if (!cell.args.is_null()) {
                    cj["args"] = cell.args;
                }
                rj.push_back(std::move(cj));
            }
            rows.push_back(std::move(rj));
        }
        out["rows"] = std::move(rows);
    } else if (item.kind == "tree") {
        if (!item.connector) {
            out["connector"] = false;
        }
        // 树节点序列化
        std::function<Json(const TreeNode&)> dumpNode = [&](const TreeNode& node) {
            Json nj = Json::object();
            nj["label"] = node.label;
            if (!node.color.empty() && node.color != "normal") {
                nj["color"] = node.color;
            }
            if (!node.action.empty()) {
                nj["action"] = node.action;
            }
            if (!node.args.is_null()) {
                nj["args"] = node.args;
            }
            if (!node.children.empty()) {
                Json children = Json::array();
                for (const auto& child : node.children) {
                    children.push_back(dumpNode(child));
                }
                nj["children"] = std::move(children);
            }
            return nj;
        };
        Json nodes = Json::array();
        for (const auto& node : item.nodes) {
            nodes.push_back(dumpNode(node));
        }
        out["nodes"] = std::move(nodes);
    } else if (item.kind == "kv") {
        Json pairs = Json::array();
        for (const auto& p : item.pairs) {
            Json pj = Json::object();
            pj["k"] = p.key;
            pj["v"] = p.value;
            if (!p.keyColor.empty()) {
                pj["kColor"] = p.keyColor;
            }
            if (!p.valueColor.empty()) {
                pj["vColor"] = p.valueColor;
            }
            pairs.push_back(std::move(pj));
        }
        out["items"] = std::move(pairs);
        if (item.sep != " : ") {
            out["sep"] = item.sep;
        }
        if (item.keyWidth > 0) {
            out["kw"] = item.keyWidth;
        }
    } else if (item.kind == "control") {
        out["control"] = item.control;
        if (!item.controlLabel.empty()) {
            out["label"] = item.controlLabel;
        }
        if (!item.help.empty()) {
            out["help"] = item.help;
        }
        if (!item.defaultValue.is_null()) {
            out["default"] = item.defaultValue;
        }
        if (item.commitOnPick) {
            out["commitOnPick"] = true;
        }
        if (item.integer) {
            out["integer"] = true;
        }
        if (item.hasNumMin) {
            out["min"] = item.numMin;
        }
        if (item.hasNumMax) {
            out["max"] = item.numMax;
        }
        if (item.step != 1.0) {
            out["step"] = item.step;
        }
        if (item.multiline) {
            out["multiline"] = true;
        }
        if (!item.options.empty()) {
            Json opts = Json::array();
            for (const auto& o : item.options) {
                Json oj = Json::object();
                oj["value"] = o.value.is_null() ? Json{o.label} : o.value;
                oj["label"] = o.label;
                if (!o.color.empty()) {
                    oj["color"] = o.color;
                }
                opts.push_back(std::move(oj));
            }
            out["options"] = std::move(opts);
        }
    } else if (item.kind == "submit") {
        if (!item.label.empty()) {
            out["label"] = item.label;
        }
        if (!item.cancelLabel.empty()) {
            out["cancelLabel"] = item.cancelLabel;
        }
    } else if (item.kind == "custom") {
        if (!item.component.empty()) {
            out["component"] = item.component;
        }
        if (!item.props.is_null()) {
            out["props"] = item.props;
        }
    } else if (item.kind == "canvas") {
        // canvas 原样保留 (本版只解析与降级, 不做自绘)
        if (!item.canvas.is_null()) {
            out = item.canvas;
        }
    }
    return out;
}

Json dumpItems(const std::vector<Item>& items) {
    Json out = Json::array();
    for (const auto& item : items) {
        out.push_back(dumpItem(item));
    }
    return out;
}

// ---------------------------------------------------------------------------
// 纯文本降级
// ---------------------------------------------------------------------------

namespace {

/// 数字转文本 (整数不带小数点; 保留至多一位小数)
std::string numText(double v) {
    if (std::isfinite(v) == 0) {
        return "0";
    }
    if (std::fabs(v - std::round(v)) < 1e-9) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0f", v);
        return buf;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", v);
    return buf;
}

/// 折行 (宽度 <= 0 不折行; 宽字符安全)
std::vector<std::string> wrapLines(std::string_view text, int width) {
    std::vector<std::string> out;
    if (text.empty()) {
        return out;
    }
    if (width <= 0) {
        std::string cur;
        for (size_t i = 0; i <= text.size(); ++i) {
            if (i == text.size() || text[i] == '\n') {
                out.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(text[i]);
            }
        }
        return out;
    }
    size_t lineStart = 0;
    while (lineStart <= text.size()) {
        const size_t nl  = text.find('\n', lineStart);
        const size_t end = (nl == std::string_view::npos) ? text.size() : nl;
        std::string_view line = text.substr(lineStart, end - lineStart);
        while (!line.empty()) {
            int              used = 0;
            std::string_view head = prefixByWidth(line, width, used);
            if (head.empty()) {
                break;
            }
            out.emplace_back(head);
            line.remove_prefix(head.size());
        }
        if (line.empty() && end == lineStart) {
            out.emplace_back();
        }
        if (nl == std::string_view::npos) {
            break;
        }
        lineStart = nl + 1;
    }
    return out;
}

/// 单行文本追加 (宽度 > 0 时按宽度折行)
void appendText(std::vector<std::string>& out, std::string_view text, int indent, int width) {
    const std::string pad(static_cast<size_t>(std::max(0, indent)), ' ');
    const int         w = (width > 0) ? std::max(1, width - indent) : 0;
    for (auto& line : wrapLines(text, w)) {
        out.push_back(pad + line);
    }
}

/// 迷你趋势图的 8 级块字符
const char* const kSparkBlocks[8] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};

std::string sparkBlock(double ratio) {
    const int idx = std::clamp(static_cast<int>(ratio * 8.0), 0, 7);
    return kSparkBlocks[idx];
}

/// 表格单元格文本 (纯文本降级只取文本, 不体现动作)
std::string cellText(const TableCell& cell) {
    return cell.text;
}

void plainTextItem(const Item& item, int indent, int width, std::vector<std::string>& out);

void plainTextItems(
    const std::vector<Item>& items,
    int                      indent,
    int                      width,
    std::vector<std::string>& out
) {
    for (const auto& item : items) {
        plainTextItem(item, indent, width, out);
    }
}

void plainTextItem(const Item& item, int indent, int width, std::vector<std::string>& out) {
    const int ind = indent + std::max(0, item.indent);
    if (!item.known) {
        if (!item.fallback.empty()) {
            appendText(out, item.fallback, ind, width);
        }
        return;
    }
    if (item.kind == "text" || item.kind == "markdown") {
        appendText(out, item.text, ind, width);
        return;
    }
    if (item.kind == "separator") {
        appendText(out, "---", ind, width);
        return;
    }
    if (item.kind == "gap") {
        for (int i = 0; i < item.lines; ++i) {
            out.emplace_back();
        }
        return;
    }
    if (item.kind == "diff") {
        if (!item.path.empty()) {
            appendText(out, "file: " + item.path, ind, width);
        }
        std::vector<std::string> oldLines;
        std::vector<std::string> newLines;
        for (auto& l : wrapLines(item.oldStr, 0)) {
            oldLines.push_back(l);
        }
        for (auto& l : wrapLines(item.newStr, 0)) {
            newLines.push_back(l);
        }
        const size_t maxLines = std::max(oldLines.size(), newLines.size());
        for (size_t k = 0; k < maxLines; ++k) {
            const std::string& o = (k < oldLines.size()) ? oldLines[k] : std::string{};
            const std::string& n = (k < newLines.size()) ? newLines[k] : std::string{};
            if (k < oldLines.size() && k < newLines.size() && o == n) {
                appendText(out, " " + o, ind, width);
                continue;
            }
            if (k < oldLines.size()) {
                appendText(out, "-" + o, ind, width);
            }
            if (k < newLines.size()) {
                appendText(out, "+" + n, ind, width);
            }
        }
        return;
    }
    if (item.kind == "button") {
        std::string label = item.label.empty() ? std::string{"Button"} : item.label;
        appendText(out, item.prefix + "[" + label + "]", ind, width);
        return;
    }
    if (item.kind == "badge") {
        appendText(out, "● " + item.text, ind, width);
        return;
    }
    if (item.kind == "diagram") {
        appendText(out, item.mermaid.empty() ? std::string{"[diagram]"} : item.mermaid, ind, width);
        return;
    }
    if (item.kind == "meter") {
        const double total = (item.total > 0) ? item.total : 100.0;
        const double ratio = std::clamp(item.value / total, 0.0, 1.0);
        const int    w     = (item.width > 0) ? item.width : 10;
        const int    filled = static_cast<int>(ratio * w + 0.5);
        std::string  bar;
        bar.reserve(static_cast<size_t>(w));
        for (int i = 0; i < w; ++i) {
            bar += (i < filled) ? '#' : '-';
        }
        const double pct  = ratio * 100.0;
        std::string  line = "[" + bar + "] " + numText(pct) + (item.unit.empty() ? "%" : item.unit);
        if (!item.label.empty()) {
            line = item.label + " " + line;
        }
        appendText(out, line, ind, width);
        return;
    }
    if (item.kind == "sparkline") {
        std::string line;
        if (!item.label.empty()) {
            line += item.label + " ";
        }
        if (!item.data.empty()) {
            const auto [minIt, maxIt] = std::minmax_element(item.data.begin(), item.data.end());
            const double lo = item.hasMin ? item.minValue : *minIt;
            const double hi = item.hasMax ? item.maxValue : *maxIt;
            const double span = (hi > lo) ? (hi - lo) : 0.0;
            for (double v : item.data) {
                const double r = (span > 0) ? ((v - lo) / span) : 0.5;
                line += sparkBlock(r);
            }
            if (item.showLast || item.unit.empty()) {
                line += " " + numText(item.data.back()) + item.unit;
            }
        } else {
            line += "[sparkline]";
        }
        appendText(out, line, ind, width);
        return;
    }
    if (item.kind == "kv") {
        size_t keyW = static_cast<size_t>(std::max(0, item.keyWidth));
        for (const auto& p : item.pairs) {
            keyW = std::max(keyW, static_cast<size_t>(displayWidth(p.key)));
        }
        for (const auto& p : item.pairs) {
            appendText(out, padRightToWidth(p.key, static_cast<int>(keyW)) + item.sep + p.value, ind, width);
        }
        return;
    }
    if (item.kind == "row") {
        std::string line;
        for (size_t i = 0; i < item.items.size(); ++i) {
            std::vector<std::string> sub;
            plainTextItem(item.items[i], 0, 0, sub);
            std::string joined;
            for (size_t k = 0; k < sub.size(); ++k) {
                if (k > 0) {
                    joined += " ";
                }
                joined += sub[k];
            }
            if (joined.empty()) {
                continue;
            }
            if (!line.empty()) {
                line += " | ";
            }
            line += joined;
        }
        appendText(out, line, ind, width);
        return;
    }
    if (item.kind == "box" || item.kind == "collapse") {
        const bool collapsed = (item.kind == "collapse") && !item.expanded;
        if (!item.title.empty()) {
            appendText(out, collapsed ? ("▸ " + item.title) : item.title, ind, width);
        }
        if (!collapsed) {
            plainTextItems(item.items, ind + 2, width, out);
        }
        return;
    }
    if (item.kind == "table") {
        if (item.columns.empty()) {
            return;
        }
        std::vector<size_t> widths(item.columns.size(), 0);
        for (size_t c = 0; c < item.columns.size(); ++c) {
            widths[c] = static_cast<size_t>(displayWidth(item.columns[c].title));
        }
        for (const auto& row : item.rows) {
            for (size_t c = 0; c < row.size() && c < widths.size(); ++c) {
                widths[c] = std::max(widths[c], static_cast<size_t>(displayWidth(cellText(row[c]))));
            }
        }
        auto emitRow = [&](const std::vector<std::string>& cells) {
            std::string line;
            for (size_t c = 0; c < cells.size(); ++c) {
                if (c > 0) {
                    line += "  ";
                }
                std::string cell = cells[c];
                if (item.columns[c].align == "right") {
                    const size_t pad = (widths[c] > displayWidth(cell))
                                           ? (widths[c] - static_cast<size_t>(displayWidth(cell)))
                                           : 0;
                    line += std::string(pad, ' ') + cell;
                } else {
                    line += padRightToWidth(cell, static_cast<int>(widths[c]));
                }
            }
            appendText(out, line, ind, width);
        };
        if (item.header) {
            std::vector<std::string> titles;
            titles.reserve(item.columns.size());
            for (const auto& c : item.columns) {
                titles.push_back(c.title);
            }
            emitRow(titles);
            std::string sepLine;
            for (size_t c = 0; c < widths.size(); ++c) {
                if (c > 0) {
                    sepLine += "  ";
                }
                sepLine += std::string(widths[c], '-');
            }
            appendText(out, sepLine, ind, width);
        }
        for (const auto& row : item.rows) {
            std::vector<std::string> cells;
            cells.reserve(item.columns.size());
            for (size_t c = 0; c < item.columns.size(); ++c) {
                cells.push_back((c < row.size()) ? cellText(row[c]) : std::string{});
            }
            emitRow(cells);
        }
        return;
    }
    if (item.kind == "tree") {
        std::function<void(const std::vector<TreeNode>&, const std::string&)> emit =
            [&](const std::vector<TreeNode>& nodes, const std::string& prefix) {
                for (size_t i = 0; i < nodes.size(); ++i) {
                    const auto& node = nodes[i];
                    const bool  last = (i + 1 == nodes.size());
                    std::string line = item.connector
                                           ? prefix + (last ? "└─ " : "├─ ") + node.label
                                           : prefix + node.label;
                    appendText(out, line, ind, width);
                    if (!node.children.empty()) {
                        emit(node.children, item.connector ? prefix + (last ? "   " : "│  ") : prefix);
                    }
                }
            };
        emit(item.nodes, "");
        return;
    }
    if (item.kind == "control") {
        std::string line = item.controlLabel.empty() ? std::string{"控件"} : item.controlLabel;
        line += ": ";
        if (item.control == "checkbox") {
            const bool checked = item.defaultValue.is_bool() && item.defaultValue.get<bool>();
            line += checked ? "[x]" : "[ ]";
        } else if (item.control == "buttons" || item.control == "select") {
            for (size_t i = 0; i < item.options.size(); ++i) {
                if (i > 0) {
                    line += " / ";
                }
                line += item.options[i].label;
            }
        } else {
            line += item.defaultValue.is_null() ? std::string{"(空)"} : item.defaultValue.dump();
        }
        line += " (" + item.control + ")";
        appendText(out, line, ind, width);
        if (!item.help.empty()) {
            appendText(out, item.help, ind + 2, width);
        }
        return;
    }
    if (item.kind == "submit") {
        return; // 交互语义, 纯文本不表达
    }
    if (item.kind == "custom") {
        if (!item.items.empty()) {
            plainTextItems(item.items, ind, width, out);
            return;
        }
        if (!item.fallback.empty()) {
            appendText(out, item.fallback, ind, width);
            return;
        }
        appendText(out, "[custom:" + item.component + "]", ind, width);
        return;
    }
    if (item.kind == "canvas") {
        if (!item.fallback.empty()) {
            appendText(out, item.fallback, ind, width);
            return;
        }
        appendText(out, "[canvas]", ind, width);
        return;
    }
    if (!item.fallback.empty()) {
        appendText(out, item.fallback, ind, width);
    }
}

} // namespace

std::string plainText(const std::vector<Item>& items, int width) {
    std::vector<std::string> lines;
    plainTextItems(items, 0, width, lines);
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) {
            out += '\n';
        }
        out += lines[i];
    }
    return out;
}

std::string plainText(const Json& json, int width) {
    return plainText(parseItemList(json), width);
}

} // namespace ui
} // namespace agentxx
