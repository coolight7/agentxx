// 本文件由 tools/gen_ui.dart 生成，请勿手工修改。
// 定义来源：schema/ui.def.json / schema/kit.def.json；扩展 kit 定义：agent/schema/agentxx-ui-kit.def.json
#pragma once

// kit：共享便捷组件。只装配、不含逻辑，也不引用客户端专属块。
// 参数用 utilxx_base::Json 传（对象），键即组件参数名：
//   agentxx::ui::kit::listRow({{"title", "切歌次数"}, {"trailing", "3"}})
// 传 env（客户端能力摘要）时按目标选择更合适的变体；不传 env 时产出中立描述，
// 由客户端的 adapt() 收口。

#include <pluginxx/ui/item.h>
#include <pluginxx/ui/kit_runtime.h>

#include <utilxx_base/json.h>

#include <cstddef>
#include <map>
#include <string>
#include <string_view>

namespace agentxx {
namespace ui {
namespace kit {

/// kit 版本
inline constexpr int kKitVersion = 1;

/// 组件模板（键 = 组件名，值是 {variants, params} 的 JSON 文本）
inline pluginxx::ui::Json kitTemplate(const std::string_view name) {
    static const std::map<std::string_view, std::string_view> kTable = {
        {"title",
         R"KIT({"variants":[{"template":{"kind":"Text","text":"$text","type":"title"}}],"params":{"text":null}})KIT"},
        {"hint",
         R"KIT({"variants":[{"template":{"kind":"Text","text":"$text","type":"caption","tone":"hint"}}],"params":{"text":null}})KIT"},
        {"text",
         R"KIT({"variants":[{"template":{"kind":"Text","text":"$text","tone":"$tone","mono":"$mono"}}],"params":{"text":null,"tone":"normal","mono":false}})KIT"},
        {"badge",
         R"KIT({"variants":[{"template":{"kind":"Badge","text":"$text","tone":"$tone"}}],"params":{"text":null,"tone":"accent"}})KIT"},
        {"icon",
         R"KIT({"variants":[{"requires":["Icon"],"template":{"kind":"Icon","name":"$name","glyph":"$glyph","size":"$size","tone":"$tone"}},{"template":{"kind":"Text","text":"$glyph","mono":true,"tone":"$tone"}}],"params":{"name":null,"glyph":null,"size":null,"tone":"normal"}})KIT"},
        {"gap",
         R"KIT({"variants":[{"template":{"kind":"Gap","size":"$size"}}],"params":{"size":null}})KIT"},
        {"divider",
         R"KIT({"variants":[{"template":{"kind":"Divider"}}],"params":{}})KIT"},
        {"button",
         R"KIT({"variants":[{"template":{"kind":"Button","label":"$label","variant":"$variant","icon":"$icon","disabled":"$disabled","action":"$action"}}],"params":{"label":null,"variant":"secondary","icon":null,"disabled":false,"action":null}})KIT"},
        {"actionsRow",
         R"KIT({"variants":[{"template":{"kind":"Row","gap":12,"children":{"$map":"buttons","wrap":{"kind":"Expanded","children":["$item"]}}}}],"params":{"buttons":null}})KIT"},
        {"card",
         R"KIT({"variants":[{"template":{"kind":"Block","title":"$title","variant":"$variant","padding":"$padding","margin":"$margin","children":"$children"}}],"params":{"title":null,"variant":"card","padding":{"horizontal":8,"vertical":4},"margin":null,"children":null}})KIT"},
        {"listRow",
         R"KIT({"variants":[{"template":{"kind":"Block","variant":"inset","padding":{"horizontal":8,"vertical":4},"action":"$action","children":[{"kind":"Row","gap":8,"cross":"center","children":[{"kind":"Expanded","children":[{"kind":"Column","gap":4,"children":[{"kind":"Text","text":"$title"},{"$require":"subtitle","kind":"Text","text":"$subtitle","type":"caption","tone":"hint"}]}]},{"$require":"trailing","kind":"Text","text":"$trailing","tone":"hint"}]}]}}],"params":{"title":null,"subtitle":null,"trailing":null,"action":null}})KIT"},
        {"section",
         R"KIT({"variants":[{"template":{"kind":"Column","gap":8,"children":[{"kind":"Text","text":"$title","type":"title"},{"kind":"Column","children":"$rows"}]}}],"params":{"title":null,"rows":null}})KIT"},
        {"kv",
         R"KIT({"variants":[{"template":{"kind":"KV","pairs":"$pairs","sep":"$sep","keyWidth":"auto"}}],"params":{"pairs":null,"sep":null}})KIT"},
        {"table",
         R"KIT({"variants":[{"template":{"kind":"Table","header":"$header","columns":"$columns","rows":"$rows"}}],"params":{"columns":null,"rows":null,"header":true}})KIT"},
        {"tree",
         R"KIT({"variants":[{"template":{"kind":"Tree","connector":"$connector","nodes":"$nodes"}}],"params":{"nodes":null,"connector":true}})KIT"},
        {"sparkline",
         R"KIT({"variants":[{"template":{"kind":"Sparkline","data":"$data","height":"$height","glyphStyle":"$glyphStyle","showLast":"$showLast","tone":"$tone"}}],"params":{"data":null,"height":1,"glyphStyle":"block","showLast":true,"tone":"accent"}})KIT"},
        {"progressRow",
         R"KIT({"variants":[{"template":{"kind":"Row","gap":8,"cross":"center","children":[{"$require":"label","kind":"Text","text":"$label"},{"kind":"Expanded","children":[{"kind":"Progress","value":"$value","total":"$total","unit":"$unit"}]}]}}],"params":{"label":null,"value":null,"total":100,"unit":"%"}})KIT"},
        {"toolCallRow",
         R"KIT({"variants":[{"template":{"kind":"Block","variant":"inset","padding":{"horizontal":8,"vertical":4},"action":"$action","children":[{"kind":"Row","gap":8,"cross":"center","children":[{"kind":"Expanded","children":[{"kind":"Column","gap":4,"children":[{"kind":"Text","text":"$name"},{"$require":"depict","kind":"Text","text":"$depict","type":"caption","tone":"hint"}]}]},{"$require":"status","kind":"Badge","text":"$status","tone":"$statusTone"}]}]}}],"params":{"name":null,"depict":null,"status":null,"statusTone":"hint","action":null}})KIT"},
        {"thinkingBlock",
         R"KIT({"variants":[{"template":{"kind":"Block","variant":"inset","padding":{"horizontal":8,"vertical":4},"children":[{"kind":"Column","gap":4,"children":[{"$require":"label","kind":"Text","text":"$label","type":"caption","tone":"hint"},{"kind":"Text","text":"$content","tone":"thinking"}]}]}}],"params":{"content":null,"label":null}})KIT"},
        {"sessionStats",
         R"KIT({"variants":[{"template":{"kind":"Block","variant":"inset","padding":{"horizontal":8,"vertical":4},"children":[{"kind":"Column","gap":4,"children":[{"$require":"title","kind":"Text","text":"$title","type":"title"},{"kind":"Table","header":true,"columns":"$columns","rows":"$rows"}]}]}}],"params":{"title":null,"columns":null,"rows":null}})KIT"},
        {"pathDiffRow",
         R"KIT({"variants":[{"requires":["Diff"],"template":{"kind":"Block","variant":"inset","padding":{"horizontal":8,"vertical":4},"children":[{"kind":"Column","gap":4,"children":[{"$require":"title","kind":"Text","text":"$title","type":"caption","tone":"hint","mono":true},{"kind":"Diff","path":"$path","oldStr":"$oldStr","newStr":"$newStr"}]}]}},{"template":{"kind":"Block","variant":"inset","padding":{"horizontal":8,"vertical":4},"children":[{"kind":"Column","gap":4,"children":[{"kind":"Text","text":"$path","type":"caption","tone":"hint","mono":true},{"kind":"Text","text":"${oldStr}\n${newStr}","mono":true}]}]}}],"params":{"path":null,"oldStr":null,"newStr":null,"title":null}})KIT"},
        {"diagramBlock",
         R"KIT({"variants":[{"requires":["Diagram"],"template":{"kind":"Block","variant":"inset","padding":{"horizontal":8,"vertical":4},"children":[{"kind":"Column","gap":4,"children":[{"$require":"title","kind":"Text","text":"$title","type":"caption","tone":"hint"},{"kind":"Diagram","mermaid":"$mermaid"}]}]}},{"template":{"kind":"Block","variant":"inset","padding":{"horizontal":8,"vertical":4},"children":[{"kind":"Text","text":"$mermaid","mono":true,"tone":"hint"}]}}],"params":{"mermaid":null,"title":null}})KIT"},
        {"interruptRow",
         R"KIT({"variants":[{"template":{"kind":"Row","gap":8,"main":"end","children":[{"kind":"Expanded","children":[{"kind":"Button","label":"$confirmLabel","variant":"primary","action":"$confirmAction"}]},{"kind":"Expanded","children":[{"kind":"Button","label":"$cancelLabel","variant":"ghost","action":"$cancelAction"}]}]}}],"params":{"confirmLabel":null,"cancelLabel":null,"confirmAction":"__submit","cancelAction":"__cancel"}})KIT"},
    };
    const auto it = kTable.find(name);
    if (it == kTable.end()) {
        return pluginxx::ui::Json::object();
    }
    return pluginxx::ui::Json::parse(it->second);
}

/// 按格换算成 u（count 列），env 为空时用库默认格大小
inline double cols(const int count, const pluginxx::ui::Capabilities* env = nullptr) {
    return static_cast<double>(count) *
           (env != nullptr ? env->cell.width : pluginxx::ui::gen::kDefaultCellWidth);
}
/// 按格换算成 u（count 行），env 为空时用库默认格大小
inline double rows(const int count, const pluginxx::ui::Capabilities* env = nullptr) {
    return static_cast<double>(count) *
           (env != nullptr ? env->cell.height : pluginxx::ui::gen::kDefaultCellHeight);
}

/// kit 组件（参数说明见生成的 docs/kit.md）

/// 标题行
/// 参数：text(text, 必填)
inline pluginxx::ui::Item title(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("title", params, env, &kitTemplate);
}

/// 次要说明行
/// 参数：text(text, 必填)
inline pluginxx::ui::Item hint(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("hint", params, env, &kitTemplate);
}

/// 正文行
/// 参数：text(text, 必填)、tone(tone, 默认 normal)、mono(bool, 默认 false)
inline pluginxx::ui::Item text(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("text", params, env, &kitTemplate);
}

/// 状态小标签
/// 参数：text(text, 必填)、tone(tone, 默认 accent)
inline pluginxx::ui::Item badge(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("badge", params, env, &kitTemplate);
}

/// 图标（目标不支持 Icon 时退化成 glyph 文本）
/// 参数：name(string)、glyph(string)、size(size)、tone(tone, 默认 normal)
inline pluginxx::ui::Item icon(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("icon", params, env, &kitTemplate);
}

/// 竖直留白（缺省用客户端默认行距）
/// 参数：size(size, 默认 gap)
inline pluginxx::ui::Item gap(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("gap", params, env, &kitTemplate);
}

/// 分隔线
/// 参数：
inline pluginxx::ui::Item divider(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("divider", params, env, &kitTemplate);
}

/// 按钮
/// 参数：label(text, 必填)、variant(enum:buttonVariant, 默认 secondary)、icon(string)、disabled(bool, 默认 false)、action(action)
inline pluginxx::ui::Item button(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("button", params, env, &kitTemplate);
}

/// 一排等宽按钮（按钮列表里的每一项占一等份）
/// 参数：buttons(items, 必填)
inline pluginxx::ui::Item actionsRow(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("actionsRow", params, env, &kitTemplate);
}

/// 内容块（覆盖基础 kit：终端卡片的留白更紧）
/// 参数：title(text)、variant(string, 默认 card)、padding(edges, 默认 {horizontal: 8, vertical: 4})、margin(edges)、children(items)
inline pluginxx::ui::Item card(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("card", params, env, &kitTemplate);
}

/// 卡片里的一行（覆盖基础 kit：终端行更紧，右侧文字用说明色调）
/// 参数：title(text, 必填)、subtitle(text)、trailing(text)、action(action)
inline pluginxx::ui::Item listRow(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("listRow", params, env, &kitTemplate);
}

/// 小节标题 + 若干行
/// 参数：title(text, 必填)、rows(items, 必填)
inline pluginxx::ui::Item section(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("section", params, env, &kitTemplate);
}

/// 键值块（键列按最长键自适应）
/// 参数：pairs(pairs, 必填)、sep(string)
inline pluginxx::ui::Item kv(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("kv", params, env, &kitTemplate);
}

/// 表格（未指定的列宽由客户端自动分配）
/// 参数：columns(columns, 必填)、rows(rows)、header(bool, 默认 true)
inline pluginxx::ui::Item table(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("table", params, env, &kitTemplate);
}

/// 层级列表
/// 参数：nodes(nodes, 必填)、connector(bool, 默认 true)
inline pluginxx::ui::Item tree(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("tree", params, env, &kitTemplate);
}

/// 迷你趋势图
/// 参数：data(numbers, 必填)、height(int, 默认 1)、glyphStyle(enum:sparkStyle, 默认 block)、showLast(bool, 默认 true)、tone(tone, 默认 accent)
inline pluginxx::ui::Item sparkline(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("sparkline", params, env, &kitTemplate);
}

/// 一行进度（覆盖基础 kit：agentxx 默认按百分比显示数值）
/// 参数：label(text)、value(float, 必填)、total(float, 默认 100)、unit(string, 默认 %)
inline pluginxx::ui::Item progressRow(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("progressRow", params, env, &kitTemplate);
}

/// 工具调用行：工具名 + 一行摘要 + 可选状态标签
/// 参数：name(text, 必填)、depict(text)、status(text)、statusTone(tone, 默认 hint)、action(action)
inline pluginxx::ui::Item toolCallRow(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("toolCallRow", params, env, &kitTemplate);
}

/// 思考 / 说明段落：可选小标题 + 次要色调的正文
/// 参数：content(text, 必填)、label(text)
inline pluginxx::ui::Item thinkingBlock(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("thinkingBlock", params, env, &kitTemplate);
}

/// 统计表：可选标题 + 表格（列宽由客户端自动分配）
/// 参数：title(text)、columns(columns, 必填)、rows(rows, 必填)
inline pluginxx::ui::Item sessionStats(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("sessionStats", params, env, &kitTemplate);
}

/// 路径差异：可选标题 + 差异对比（目标不支持 Diff 时退化成等宽文本）
/// 参数：path(string, 必填)、oldStr(string, 必填)、newStr(string, 必填)、title(text)
inline pluginxx::ui::Item pathDiffRow(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("pathDiffRow", params, env, &kitTemplate);
}

/// 状态图：可选标题 + mermaid 描述（目标不支持 Diagram 时退化成等宽源码）
/// 参数：mermaid(string, 必填)、title(text)
inline pluginxx::ui::Item diagramBlock(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("diagramBlock", params, env, &kitTemplate);
}

/// 确认 / 取消行（动作名缺省用中断域内约定：__submit / __cancel）
/// 参数：confirmLabel(text, 必填)、cancelLabel(text, 必填)、confirmAction(action, 默认 __submit)、cancelAction(action, 默认 __cancel)
inline pluginxx::ui::Item interruptRow(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("interruptRow", params, env, &kitTemplate);
}

} // namespace kit
} // namespace ui
} // namespace agentxx
