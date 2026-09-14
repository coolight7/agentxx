#include "agentxx/middlewares/interrupt_presets.h"

#include "agentxx/util/string_util.h"
#include "fmt/format.h"
#include <algorithm>

namespace agentxx {
namespace middleware {
namespace preset {

namespace {

/// 通用默认表单的控件 id (多项时追加序号: "value1".."valueN")
constexpr std::string_view kSingleInputId = "value";

/// 字符串 → 布尔 (空/非法按 defaultValue; "false"/"no"/"n"/"0" 为 false)
bool boolFromString(std::string_view s, bool defaultValue) {
    if (s.empty()) {
        return defaultValue;
    }
    auto v = agentxx::util::toLower(agentxx::util::removeBetweenSpace(s));
    if (v == "false" || v == "no" || v == "n" || v == "0" || v == "off") {
        return false;
    }
    return true;
}

/// 字符串 → 数值 (非法/空按 0)
double numberFromString(std::string_view s) {
    if (s.empty()) {
        return 0.0;
    }
    double v = 0.0;
    if (agentxx::util::parseNumberFromString(agentxx::util::removeBetweenSpace(s), v).ec
        != std::errc{}) {
        return 0.0;
    }
    return v;
}

} // namespace

InterruptUiOption
    option(std::string value, std::string label, std::string labelKey, std::string color) {
    InterruptUiOption o;
    o.value    = std::move(value);
    o.label    = std::move(label);
    o.labelKey = std::move(labelKey);
    o.color    = std::move(color);
    return o;
}

InterruptUiBlock
    textBlock(std::string text, std::string color, int indent, bool wrap, bool bold, bool dim) {
    InterruptUiBlock b;
    b.kind   = "text";
    b.text   = std::move(text);
    b.color  = std::move(color);
    b.indent = indent;
    b.wrap   = wrap;
    b.bold   = bold;
    b.dim    = dim;
    return b;
}

InterruptUiBlock textBlockKey(
    std::string textKey,
    std::string text,
    std::string color,
    int         indent,
    bool        wrap,
    bool        bold,
    bool        dim
) {
    InterruptUiBlock b;
    b.kind    = "text";
    b.textKey = std::move(textKey);
    b.text    = std::move(text);
    b.color   = std::move(color);
    b.indent  = indent;
    b.wrap    = wrap;
    b.bold    = bold;
    b.dim     = dim;
    return b;
}

InterruptUiBlock markdownBlock(std::string markdown, int indent) {
    InterruptUiBlock b;
    b.kind   = "markdown";
    b.text   = std::move(markdown);
    b.indent = indent;
    return b;
}

InterruptUiBlock diffBlock(std::string path, std::string oldStr, std::string newStr) {
    InterruptUiBlock b;
    b.kind   = "diff";
    b.path   = std::move(path);
    b.oldStr = std::move(oldStr);
    b.newStr = std::move(newStr);
    return b;
}

InterruptUiBlock separatorBlock(int indent) {
    InterruptUiBlock b;
    b.kind   = "separator";
    b.indent = indent;
    return b;
}

InterruptUiBlock gapBlock(int lines) {
    InterruptUiBlock b;
    b.kind  = "gap";
    b.lines = std::max(0, lines);
    return b;
}

InterruptUiBlock submitBlock(
    std::string label,
    std::string labelKey,
    std::string cancelLabel,
    std::string cancelLabelKey
) {
    InterruptUiBlock b;
    b.kind           = "submit";
    b.label          = std::move(label);
    b.labelKey       = std::move(labelKey);
    b.cancelLabel    = std::move(cancelLabel);
    b.cancelLabelKey = std::move(cancelLabelKey);
    return b;
}

InterruptUiBlock buttonControl(
    std::string                    id,
    std::vector<InterruptUiOption> options,
    std::string                    label,
    std::string                    labelKey,
    agentxx::util::Json            defaultValue,
    bool                           commitOnPick
) {
    InterruptUiBlock b;
    b.kind         = "control";
    b.control      = "buttons";
    b.id           = std::move(id);
    b.options      = std::move(options);
    b.label        = std::move(label);
    b.labelKey     = std::move(labelKey);
    b.defaultValue = std::move(defaultValue);
    b.commitOnPick = commitOnPick;
    return b;
}

InterruptUiBlock checkboxControl(
    std::string id,
    std::string label,
    std::string labelKey,
    bool        defaultValue,
    std::string help,
    std::string helpKey
) {
    InterruptUiBlock b;
    b.kind         = "control";
    b.control      = "checkbox";
    b.id           = std::move(id);
    b.label        = std::move(label);
    b.labelKey     = std::move(labelKey);
    b.help         = std::move(help);
    b.helpKey      = std::move(helpKey);
    b.defaultValue = defaultValue;
    return b;
}

InterruptUiBlock textControl(
    std::string id,
    std::string label,
    std::string labelKey,
    std::string defaultValue,
    bool        multiline,
    std::string help
) {
    InterruptUiBlock b;
    b.kind         = "control";
    b.control      = "text";
    b.id           = std::move(id);
    b.label        = std::move(label);
    b.labelKey     = std::move(labelKey);
    b.help         = std::move(help);
    b.defaultValue = std::move(defaultValue);
    b.multiline    = multiline;
    return b;
}

InterruptUiBlock numberControl(
    std::string id,
    std::string label,
    std::string labelKey,
    double      defaultValue,
    bool        integer,
    double      step
) {
    InterruptUiBlock b;
    b.kind         = "control";
    b.control      = "number";
    b.id           = std::move(id);
    b.label        = std::move(label);
    b.labelKey     = std::move(labelKey);
    b.defaultValue = defaultValue;
    b.integer      = integer;
    b.step         = (step > 0) ? step : 1.0;
    return b;
}

InterruptUiBlock selectControl(
    std::string                    id,
    std::vector<InterruptUiOption> options,
    std::string                    label,
    std::string                    labelKey,
    agentxx::util::Json            defaultValue,
    std::string                    help
) {
    InterruptUiBlock b;
    b.kind         = "control";
    b.control      = "select";
    b.id           = std::move(id);
    b.options      = std::move(options);
    b.label        = std::move(label);
    b.labelKey     = std::move(labelKey);
    b.help         = std::move(help);
    b.defaultValue = std::move(defaultValue);
    return b;
}

// ---------------------------------------------------------------------------
// 预设模板
// ---------------------------------------------------------------------------

InterruptUi inputForm(const std::vector<InputSpec>& inputs) {
    InterruptUi ui;
    // 头行留空: 客户端渲染通用默认前缀 (i18n 键 interrupt.header)

    const size_t total   = inputs.size();
    bool         hasPrev = false;
    for (size_t k = 0; k < total; ++k) {
        const auto& spec = inputs[k];
        if (hasPrev) {
            ui.blocks.push_back(gapBlock(1));
        }

        // 控件标签 (accent, 如 "[read] Repeated identical call")
        if (!spec.label.empty()) {
            ui.blocks.push_back(textBlock(spec.label, "accent", 2, false, true));
        }
        // 控件说明 (hint, 硬折行; 如受约束路径/询问原因)
        if (!spec.depict.empty()) {
            ui.blocks.push_back(textBlock(spec.depict, "hint", 2, true));
        }

        // 控件 id: 单项 = "value"; 多项 = "value1".."valueN" (结果 values 的键)
        const std::string id = (total == 1) ? std::string{kSingleInputId}
                                            : fmt::format("{}{}", kSingleInputId, k + 1);

        if (spec.type == "bool") {
            // 是/否一键按钮 (点击即提交: 一问一答形态)
            ui.blocks.push_back(buttonControl(
                id,
                {option("true", "Yes", "interrupt.yes"), option("false", "No", "interrupt.no")},
                {},
                {},
                // 默认值须与候选项 value 同型 (字符串), 否则无法命中默认选中项
                agentxx::util::Json(boolFromString(spec.defaultValue, false) ? "true" : "false"),
                true
            ));
        } else if (spec.type == "enum") {
            std::vector<InterruptUiOption> options;
            options.reserve(spec.enumValues.size());
            for (const auto& v : spec.enumValues) {
                options.push_back(option(v, v));
            }
            agentxx::util::Json def;
            if (!spec.defaultValue.empty()) {
                def = spec.defaultValue;
            } else if (!options.empty()) {
                def = options.front().value;
            }
            ui.blocks.push_back(selectControl(id, std::move(options), {}, {}, std::move(def)));
        } else if (spec.type == "int" || spec.type == "double") {
            ui.blocks.push_back(numberControl(
                id,
                {},
                {},
                numberFromString(spec.defaultValue),
                spec.type == "int",
                1.0
            ));
        } else {
            // string (含未声明类型): 文本输入框
            ui.blocks.push_back(textControl(id, {}, {}, spec.defaultValue));
        }

        hasPrev = true;
    }

    // 确认/取消行: 无输入项时即"仅确认"表单 (空 values = 用户已阅)
    ui.blocks.push_back(submitBlock());
    return ui;
}

InterruptUi confirmCard(const ConfirmCardOptions& opts) {
    InterruptUi ui;
    if (!opts.title.empty()) {
        ui.blocks.push_back(textBlock(opts.title, "accent", 2, false, true));
    }
    if (!opts.text.empty()) {
        ui.blocks.push_back(markdownBlock(opts.text, 2));
    }
    ui.blocks.push_back(gapBlock(1));

    if (opts.remember) {
        ui.blocks.push_back(
            checkboxControl("remember", "Remember this choice", "interrupt.remember")
        );
        ui.blocks.push_back(gapBlock(1));
    }

    // 是/否一键按钮 (点击即选中并提交整份表单)
    const std::string yesLabel = opts.yesLabel.empty() ? "Yes" : opts.yesLabel;
    const std::string yesKey   = opts.yesLabelKey.empty() ? "interrupt.yes" : opts.yesLabelKey;
    const std::string noLabel  = opts.noLabel.empty() ? "No" : opts.noLabel;
    const std::string noKey    = opts.noLabelKey.empty() ? "interrupt.no" : opts.noLabelKey;
    ui.blocks.push_back(buttonControl(
        opts.controlId.empty() ? std::string{"allow"} : opts.controlId,
        {option("true", yesLabel, yesKey), option("false", noLabel, noKey)},
        {},
        {},
        // 默认值须与候选项 value 同型 (字符串): "false" 默认选中"否" (安全语义)
        agentxx::util::Json(opts.defaultValue ? "true" : "false"),
        true
    ));
    return ui;
}

InterruptUi
    permissionCard(std::string_view toolName, std::string_view category, std::string_view target) {
    InterruptUi ui;
    // 头行: 权限标记 + 工具名 + 权限分类
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
    ui.blocks.push_back(textBlock(fmt::format("• {}", target), "hint", 2, true));
    ui.blocks.push_back(textBlockKey("interrupt.rememberDir", "", "hint", 2, true));
    ui.blocks.push_back(gapBlock(1));

    // 设置项: 记住此选择 (勾选后提交时按本次选择注册路径规则)
    // - 目录目标 (规范化路径带尾斜杠; 见 PermissionMiddlewareHandle::
    //   normalizePermissionPath): 规则按最长前缀匹配覆盖该目录及其全部子目录与
    //   文件, 在勾选项下方提示生效范围 (避免误解为只记住单个路径)
    const bool isDirTarget = !target.empty() && target.back() == '/';
    ui.blocks.push_back(checkboxControl(
        "remember",
        "Remember this choice",
        "interrupt.remember",
        false,
        isDirTarget ? "Also covers its subdirectories and files" : std::string{},
        isDirTarget ? "interrupt.rememberDir" : std::string{}
    ));

    // 设置项: 完全授权所有权限 (勾选并确认后不再询问权限, 允许任意权限访问;
    // 配置文件拒绝的路径仍然保持拒绝)
    ui.blocks.push_back(
        checkboxControl("fullAuth", "Fully authorize all permissions", "interrupt.fullAuth", false)
    );
    ui.blocks.push_back(gapBlock(1));

    // 一键取值按钮: 允许 / 拒绝 (点击即提交; 默认选中"拒绝" = 安全语义)
    ui.blocks.push_back(buttonControl(
        "decision",
        {option("true", "Allow", "interrupt.allow"),
         option("false", "Deny", "interrupt.deny", "error")},
        {},
        {},
        // 默认选中"拒绝" (安全语义; 字符串型与候选项 value 一致)
        agentxx::util::Json("false"),
        true
    ));
    return ui;
}

} // namespace preset
} // namespace middleware
} // namespace agentxx
