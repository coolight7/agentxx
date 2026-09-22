#pragma once

/// 组件树构建器 (页面/面板/overlay/中断描述的通用组装工具)
///
/// 用途: 让调用方用 C++ 链式调用产出 `agentxx.ui.item` schema 的 JSON, 而不是
/// 手写字符串拼接。插件 (agent 侧与 client 侧)、中断预设、宿主自身都能用 ——
/// 与具体前端无关, 也不改变任何插件接口 (只是数据层工具)。
///
/// 用法:
/// ```c++
/// agentxx::ui::Items ui;
/// ui.box("System", agentxx::ui::Items{}
///         .kv({{"Model", model}, {"Tokens", tokens}})
///         .meter(pct, 100, {.width = 24, .label = "CPU", .unit = "%"}), {.border = "round"})
///   .table({.columns = {{"File", "left", 0}, {"Size", "right", 8}}, .rows = rows})
///   .checkbox("detail", "显示细节", detailOn)
///   .submit("应用", "取消");
/// panel->update(ui.dump());     // 或 setPanelItems(panel, ui)
/// ```
///
/// 说明:
/// - 各方法返回自身引用以便链式调用; 容器方法接受临时 `Items` 对象 (内部按值合并)
/// - `dump()` 产出 `{"items":[...]}`, 与插件接口 `update_panel` / `update_info_section`
///   等接受的 JSON 形态一致
#include "agentxx/ui/item.h"
#include "agentxx/ui/text_width.h"
#include "utilxx_base/json.h"
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agentxx {
namespace ui {

using utilxx_base::Json;

/// 迷你趋势图参数
struct SparklineOpts {
    int                      height   = 1;
    /// 绘制风格: block(▁▂▃▄▅▆▇█) / bar(▏▎▍▌▋▊▉█) / auto
    std::string              style    = "block";
    std::optional<double>    min;
    std::optional<double>    max;
    /// 单色 (colors 为空时生效)
    std::string              color    = "accent";
    /// 按值分档配色 (由低到高)
    std::vector<std::string> colors;
    std::string              label;
    std::string              unit;
    bool                     showLast = false;
};

/// 计量条参数
struct MeterOpts {
    /// 条宽 (显示列; 0 = 默认 20 列)
    int                                     width   = 0;
    std::string                             label;
    std::string                             unit;
    std::string                             color;
    /// 阈值配色: {阈值, 色名} (由高到低匹配首个满足项)
    std::vector<std::pair<double, std::string>> thresholds;
    bool                                    showValue = true;
};

/// 横排 (row) 参数
struct RowOpts {
    int         gap   = 1;
    std::string align = "left";
};

/// 分组框 (box) 参数
struct BoxOpts {
    /// 边框风格: none / square / round / light
    std::string border     = "round";
    int         pad        = 0;
    std::string titleColor;
};

/// 表单描述 (控件 + 提交行; 定义在 [Items] 之后 —— 需要完整的 Items 类型)
struct FormSpec;

/// 表格列声明
struct TableColumnSpec {
    std::string title;
    std::string align = "left";
    /// 固定列宽 (显示列); 0 = 自适应/占据剩余宽度 (首个 0 宽列占剩余)
    int         width = 0;
    std::string color;
};

/// 表格描述
struct TableSpec {
    std::vector<TableColumnSpec>    columns;
    /// 单元格: 字符串, 或对象 {"text","color","action","args"}
    std::vector<std::vector<Json>>  rows;
    bool                            header = true;
};

/// 树节点描述
struct TreeNodeSpec {
    std::string               label;
    std::string               color;
    std::string               action;
    Json                      args;
    std::vector<TreeNodeSpec> children;
};

/// 树描述
struct TreeSpec {
    std::vector<TreeNodeSpec> nodes;
    bool                      connector = true;
};

/// 数值控件参数
struct NumberOpts {
    bool                  integer = false;
    std::optional<double> min;
    std::optional<double> max;
    double                step = 1.0;
    std::string           help;
};

/// 控件候选项
struct OptionSpec {
    /// 选中时写入结果的原始值 (缺省用 label)
    Json        value;
    std::string label;
    std::string color;
};

/// 组件树构建器
class Items {
public:

    Items() = default;

    /// 由已有组件项列表构造
    static Items fromList(std::vector<Item> items) {
        Items out;
        out.parsed_ = std::move(items);
        out.hasParsed_ = true;
        return out;
    }

    // ---------------- 文本类 ----------------

    /// 文本行
    Items& text(std::string_view s, std::string_view color = "normal") {
        Json& it  = push("text");
        it["text"] = std::string{s};
        if (!color.empty() && color != "normal") {
            it["color"] = std::string{color};
        }
        return *this;
    }

    /// 加粗文本行 (标题)
    Items& title(std::string_view s) {
        Json& it   = push("text");
        it["text"] = std::string{s};
        it["bold"] = true;
        it["color"] = "title";
        return *this;
    }

    /// 提示文本行 (减淡)
    Items& hint(std::string_view s) {
        Json& it   = push("text");
        it["text"] = std::string{s};
        it["color"] = "hint";
        return *this;
    }

    /// markdown 富文本
    Items& markdown(std::string_view md) {
        Json& it   = push("markdown");
        it["text"] = std::string{md};
        return *this;
    }

    /// 差异对比
    Items& diff(std::string_view path, std::string_view oldStr, std::string_view newStr) {
        Json& it    = push("diff");
        it["path"]    = std::string{path};
        it["old_str"] = std::string{oldStr};
        it["new_str"] = std::string{newStr};
        return *this;
    }

    /// 分隔线
    Items& separator() {
        push("separator");
        return *this;
    }

    /// 空行
    Items& gap(int lines = 1) {
        Json& it   = push("gap");
        it["lines"] = lines;
        return *this;
    }

    /// 状态点 + 文本
    Items& badge(std::string_view s, std::string_view color = "accent") {
        Json& it   = push("badge");
        it["text"]  = std::string{s};
        if (!color.empty()) {
            it["color"] = std::string{color};
        }
        return *this;
    }

    /// 内联 mermaid 状态图
    Items& diagram(std::string_view mermaid) {
        Json& it    = push("diagram");
        it["mermaid"] = std::string{mermaid};
        return *this;
    }

    // ---------------- 按钮与动作 ----------------

    /// 可点按钮
    /// - `action` 非空时点击派发该动作 id (插件需绑定动作处理器)
    Items& button(
        std::string_view label,
        std::string_view action = {},
        Json             args   = {},
        std::string_view color  = "normal"
    ) {
        Json& it     = push("button");
        it["label"]  = std::string{label};
        if (!action.empty()) {
            it["action"] = std::string{action};
        }
        if (!args.is_null()) {
            it["args"] = std::move(args);
        }
        if (!color.empty() && color != "normal") {
            it["color"] = std::string{color};
        }
        return *this;
    }

    /// 给最近一项补充点击动作 (对表格/树之外的结构化组件无效)
    Items& action(std::string_view actionId, Json args = {}) {
        syncParsed();
        if (!rawList_.empty()) {
            Json& last = rawList_.back();
            if (last.is_object()) {
                last["action"] = std::string{actionId};
                if (!args.is_null()) {
                    last["args"] = std::move(args);
                }
            }
        }
        return *this;
    }

    // ---------------- 结构与数据 ----------------

    /// 横排 (各列按顺序分配宽度; 列宽用 `w` 声明时需要手写 JSON, 此处按等分处理)
    Items& row(const Items& content, RowOpts opts = {}) {
        return row(std::vector<Items>{content}, opts);
    }

    /// 横排 (多列)
    Items& row(std::vector<Items> cols, RowOpts opts = {}) {
        Json& it     = push("row");
        it["gap"]    = opts.gap;
        it["align"]  = opts.align;
        Json arr     = Json::array();
        for (auto& col : cols) {
            for (const auto& sub : col.rawList()) {
                arr.push_back(sub);
            }
        }
        it["items"] = std::move(arr);
        return *this;
    }

    /// 分组框 (分组标题 + 可选边框 + 内边距)
    Items& box(std::string_view title, const Items& content, BoxOpts opts = {}) {
        Json& it = push("box");
        if (!title.empty()) {
            it["title"] = std::string{title};
        }
        if (!opts.border.empty()) {
            it["border"] = opts.border;
        }
        if (opts.pad > 0) {
            it["pad"] = opts.pad;
        }
        if (!opts.titleColor.empty()) {
            it["titleColor"] = opts.titleColor;
        }
        Json arr = Json::array();
        for (const auto& sub : content.rawList()) {
            arr.push_back(sub);
        }
        it["items"] = std::move(arr);
        return *this;
    }

    /// 可折叠分组 (展开状态由宿主维护, 键 = 归属 id + 组件 id)
    Items& collapse(
        std::string_view id,
        std::string_view title,
        bool             expanded,
        const Items&     content
    ) {
        Json& it     = push("collapse");
        it["id"]     = std::string{id};
        it["title"]  = std::string{title};
        it["expanded"] = expanded;
        Json arr     = Json::array();
        for (const auto& sub : content.rawList()) {
            arr.push_back(sub);
        }
        it["items"] = std::move(arr);
        return *this;
    }

    /// 键值对 (两列对齐; 值是已经格式化好的文本)
    Items& kv(std::initializer_list<std::pair<std::string, std::string>> entries) {
        Json& it = push("kv");
        Json  arr = Json::array();
        for (const auto& [k, v] : entries) {
            // 注意: Json 有 initializer_list 构造, `Json{x}` 会构造出"单元素数组";
            // 标量值一律用圆括号构造
            Json entry = Json::object();
            entry["k"] = k;
            entry["v"] = v;
            arr.push_back(std::move(entry));
        }
        it["items"] = std::move(arr);
        return *this;
    }

    /// 表格
    Items& table(TableSpec spec) {
        Json& it = push("table");
        it["header"] = spec.header;
        Json cols = Json::array();
        for (const auto& c : spec.columns) {
            Json cj = Json::object();
            if (!c.title.empty()) {
                cj["title"] = c.title;
            }
            if (c.align != "left") {
                cj["align"] = c.align;
            }
            cj["w"] = (c.width > 0) ? Json(c.width) : Json(std::string_view{"flex"});
            if (!c.color.empty()) {
                cj["color"] = c.color;
            }
            cols.push_back(std::move(cj));
        }
        it["columns"] = std::move(cols);
        Json rows = Json::array();
        for (const auto& row : spec.rows) {
            Json rj = Json::array();
            for (const auto& cell : row) {
                rj.push_back(cell);
            }
            rows.push_back(std::move(rj));
        }
        it["rows"] = std::move(rows);
        return *this;
    }

    /// 层级列表 (连接线 + 缩进; 节点可带点击动作)
    Items& tree(TreeSpec spec) {
        Json& it = push("tree");
        if (!spec.connector) {
            it["connector"] = false;
        }
        Json nodes = Json::array();
        for (const auto& n : spec.nodes) {
            nodes.push_back(dumpNode(n));
        }
        it["nodes"] = std::move(nodes);
        return *this;
    }

    /// 迷你趋势图
    Items& sparkline(const std::vector<double>& data, SparklineOpts opts = {}) {
        Json& it   = push("sparkline");
        Json arr = Json::array();
        for (double d : data) {
            arr.push_back(Json(d));
        }
        it["data"]   = std::move(arr);
        it["height"] = opts.height;
        if (opts.style != "block") {
            it["style"] = opts.style;
        }
        if (opts.min.has_value()) {
            it["min"] = *opts.min;
        }
        if (opts.max.has_value()) {
            it["max"] = *opts.max;
        }
        if (!opts.color.empty() && opts.color != "normal") {
            it["color"] = opts.color;
        }
        if (!opts.colors.empty()) {
            Json colors = Json::array();
            for (const auto& c : opts.colors) {
                colors.push_back(Json(c));
            }
            it["colors"] = std::move(colors);
        }
        if (!opts.label.empty()) {
            it["label"] = opts.label;
        }
        if (!opts.unit.empty()) {
            it["unit"] = opts.unit;
        }
        if (opts.showLast) {
            it["showLast"] = true;
        }
        return *this;
    }

    /// 条形计量 (阈值配色)
    Items& meter(double value, double total = 100.0, MeterOpts opts = {}) {
        Json& it   = push("meter");
        it["value"] = value;
        it["total"] = (total > 0) ? total : 100.0;
        if (opts.width > 0) {
            it["width"] = opts.width;
        }
        if (!opts.label.empty()) {
            it["label"] = opts.label;
        }
        if (!opts.unit.empty()) {
            it["unit"] = opts.unit;
        }
        if (!opts.color.empty()) {
            it["color"] = opts.color;
        }
        if (!opts.thresholds.empty()) {
            Json th = Json::array();
            for (const auto& [at, color] : opts.thresholds) {
                // 注意: Json 有 initializer_list 构造, `Json{x}` 会构造出"单元素数组"
                Json entry = Json::object();
                entry["at"]    = at;
                entry["color"] = color;
                th.push_back(std::move(entry));
            }
            it["thresholds"] = std::move(th);
        }
        return *this;
    }

    // ---------------- 交互控件 (与中断描述同语义) ----------------

    /// 勾选项
    Items& checkbox(std::string_view id, std::string_view label, bool defaultValue) {
        Json& it       = push("control");
        it["id"]       = std::string{id};
        it["control"]  = "checkbox";
        it["label"]    = std::string{label};
        it["default"]  = defaultValue;
        return *this;
    }

    /// 文本输入框
    Items& input(
        std::string_view id,
        std::string_view label,
        std::string_view defaultValue = {},
        std::string_view help         = {}
    ) {
        Json& it      = push("control");
        it["id"]      = std::string{id};
        it["control"] = "text";
        it["label"]   = std::string{label};
        it["default"] = std::string{defaultValue};
        if (!help.empty()) {
            it["help"] = std::string{help};
        }
        return *this;
    }

    /// 数值控件
    Items& number(
        std::string_view id,
        std::string_view label,
        double           defaultValue,
        NumberOpts       opts = {}
    ) {
        Json& it      = push("control");
        it["id"]      = std::string{id};
        it["control"] = "number";
        it["label"]   = std::string{label};
        it["default"] = defaultValue;
        if (opts.integer) {
            it["integer"] = true;
        }
        if (opts.min.has_value()) {
            it["min"] = *opts.min;
        }
        if (opts.max.has_value()) {
            it["max"] = *opts.max;
        }
        if (opts.step != 1.0) {
            it["step"] = opts.step;
        }
        if (!opts.help.empty()) {
            it["help"] = opts.help;
        }
        return *this;
    }

    /// 竖排单选列表
    Items& select(
        std::string_view              id,
        std::string_view              label,
        const std::vector<OptionSpec>& options,
        std::string_view              help = {}
    ) {
        Json& it = pushControl(id, label, "select", help);
        it["options"] = dumpOptions(options);
        return *this;
    }

    /// 横排按钮组
    /// - `commitOnPick` 为真时点击即选中并提交整份表单
    Items& buttons(
        std::string_view               id,
        std::string_view               label,
        const std::vector<OptionSpec>& options,
        bool                           commitOnPick = false
    ) {
        Json& it       = pushControl(id, label, "buttons", {});
        it["options"]  = dumpOptions(options);
        if (commitOnPick) {
            it["commitOnPick"] = true;
        }
        return *this;
    }

    /// 表单提交行 (确认 + 取消)
    Items& submit(std::string_view label = {}, std::string_view cancelLabel = {}) {
        Json& it = push("submit");
        if (!label.empty()) {
            it["label"] = std::string{label};
        }
        if (!cancelLabel.empty()) {
            it["cancelLabel"] = std::string{cancelLabel};
        }
        return *this;
    }

    // ---------------- 其他 ----------------

    /// 表单 (控件 + 提交行)
    ///
    /// 例:
    /// ```c++
    /// ui.form({
    ///     .title  = "Options",
    ///     .fields = {agentxx::ui::Items{}.checkbox("detail", "显示细节", on),
    ///                agentxx::ui::Items{}.number("level", "层级", 3)},
    /// });
    /// ```
    /// 提交/取消经动作通道回传: `__submit` + `{"values":{控件 id: 值}}` / `__cancel`
    /// (定义见 [FormSpec]; 在类外以实现"声明处允许不完整类型")
    Items& form(FormSpec spec);

    /// 给最近一项设置条件显示表达式 (预留字段: 当前仅写入描述, 渲染不消费)
    /// - 例: `ui.text("详情").when("expanded")`
    Items& when(std::string_view expr) {
        syncParsed();
        if (!rawList_.empty()) {
            Json& last = rawList_.back();
            if (last.is_object()) {
                last["when"] = std::string{expr};
            }
        }
        return *this;
    }

    /// 完全自绘组件 (本版只保留数据, 渲染降级为 `fallback`)
    Items& canvas(Json canvasJson, std::string_view fallback = {}) {
        Json it = std::move(canvasJson);
        if (!it.is_object()) {
            it = Json::object();
            it["kind"] = "canvas";
        }
        if (!fallback.empty()) {
            it["fallback"] = std::string{fallback};
        }
        pushJson(std::move(it));
        return *this;
    }

    /// 直接追加一条原始 JSON 描述 (未识别的字段由解析器忽略)
    Items& raw(Json itemJson) {
        pushJson(std::move(itemJson));
        return *this;
    }

    /// 合并另一棵树 (内容追加到本树末尾)
    Items& append(const Items& other) {
        syncParsed();
        for (const auto& sub : other.rawList()) {
            rawList_.push_back(sub);
        }
        return *this;
    }

    /// 是否为空 (无任何组件)
    bool empty() const {
        syncParsed();
        return rawList_.empty();
    }

    /// 组件数
    size_t size() const {
        syncParsed();
        return rawList_.size();
    }

    /// 内部数组 (只读)
    const std::vector<Json>& rawList() const {
        syncParsed();
        return rawList_;
    }

    /// 内部数组的 JSON 副本 (即 `{"items":[...]}` 的内层数组)
    Json array() const {
        syncParsed();
        Json out = Json::array();
        for (const auto& it : rawList_) {
            out.push_back(it);
        }
        return out;
    }

    /// 产出 `{"items":[...]}` (插件接口接受的形态)
    Json json() const {
        return Json::object({{"items", array()}});
    }

    /// 产出紧凑 JSON 文本 (供 C 接口的 PluginxxStringView 参数使用)
    std::string dump() const {
        return json().dump();
    }

private:

    /// 追加一项并返回其引用 (便于逐字段填写)
    Json& push(std::string_view kind) {
        syncParsed();
        Json item = Json::object();
        item["kind"] = std::string{kind};
        rawList_.push_back(std::move(item));
        return rawList_.back();
    }

    void pushJson(Json itemJson) {
        syncParsed();
        rawList_.push_back(std::move(itemJson));
    }

    Json& pushControl(
        std::string_view id,
        std::string_view label,
        std::string_view control,
        std::string_view help
    ) {
        Json& it      = push("control");
        it["id"]      = std::string{id};
        it["control"] = std::string{control};
        if (!label.empty()) {
            it["label"] = std::string{label};
        }
        if (!help.empty()) {
            it["help"] = std::string{help};
        }
        return it;
    }

    static Json dumpOptions(const std::vector<OptionSpec>& options) {
        Json arr = Json::array();
        for (const auto& o : options) {
            Json oj = Json::object();
            oj["value"] = o.value.is_null() ? Json(o.label) : o.value;
            oj["label"] = o.label;
            if (!o.color.empty()) {
                oj["color"] = o.color;
            }
            arr.push_back(std::move(oj));
        }
        return arr;
    }

    static Json dumpNode(const TreeNodeSpec& node) {
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
    }

    /// 由已解析的组件项构造时, 首次访问才转为 JSON (避免无谓的重复转换)
    void syncParsed() const {
        if (!hasParsed_) {
            return;
        }
        for (const auto& item : parsed_) {
            rawList_.push_back(dumpItem(item));
        }
        parsed_.clear();
        hasParsed_ = false;
    }

    mutable std::vector<Json> rawList_;
    mutable std::vector<Item> parsed_;
    mutable bool              hasParsed_ = false;
};

/// 表单描述 (控件 + 提交行)
///
/// 表单的控件语义与中断描述完全一致 (控件形态即语义, 值经宿主的表单状态维护),
/// 差别只在结果去处: 插件表单经动作通道回传 (`__submit` / `__cancel` /
/// `commitOnPick` 的控件 id), 中断描述经中断结果通道回传。
struct FormSpec {
    /// 分组标题 (空 = 不加分组框, 控件直接追加到当前树)
    std::string        title;
    /// 分组边框风格 (取值同 [BoxOpts::border]; 仅 title 非空时生效)
    std::string        border = "round";
    /// 控件列表 (checkbox / input / number / select / buttons 等)
    std::vector<Items> fields;
    /// 提交按钮文案 (空 = 前端按语言取默认文案)
    std::string        submitLabel;
    /// 取消按钮文案 (空 = 前端按语言取默认文案)
    std::string        cancelLabel;
    /// 是否追加提交行 (缺省追加; 纯展示型表单可置 false)
    bool               showSubmit = true;
};

inline Items& Items::form(FormSpec spec) {
    Items body;
    for (const auto& field : spec.fields) {
        body.append(field);
    }
    if (spec.showSubmit) {
        body.submit(spec.submitLabel, spec.cancelLabel);
    }
    if (!spec.title.empty()) {
        BoxOpts opts;
        opts.border = spec.border.empty() ? std::string{"round"} : spec.border;
        return box(spec.title, body, opts);
    }
    return append(body);
}

} // namespace ui
} // namespace agentxx
