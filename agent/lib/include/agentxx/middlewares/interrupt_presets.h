#pragma once

#include "agentxx/middlewares/interrupt_ui.h"
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace middleware {

/// 预设模板与块构造 helper (生产者侧使用; 客户端只消费生成出的描述)
///
/// 设计意图: 中断描述本身**不含** "参数类型" 概念 (控件形态即语义), 但很多
/// 询问只需要 "若干类型化输入 + 确认" 这种常见形态 —— 该形态由本命名空间内的
/// 预设模板生成, 类型 → 控件的映射只存在于此处; 自定义版式的生产者可直接用
/// 下面的块构造 helper 拼装 [InterruptUi::blocks], 或直接构造 [InterruptUiBlock]。
///
/// 文案约定: 内置预设模板与固定文案只声明 **i18n 键** (labelKey/helpKey/textKey),
/// 字面文本留空 —— 文案由客户端词表提供, 避免同一文案在服务端与客户端两处
/// 重复维护; 调用方自定义的字面文本 (无对应键) 仍按字面渲染。
namespace preset {

/// 类型化输入项声明 (**仅生产者侧使用**, 不是协议字段)
struct InputSpec {
    /// 控件标签 (渲染在控件上方; 空 = 不渲染标签行)
    std::string label;
    /// 控件说明 (渲染在标签下方, 按宽度硬折行; 空 = 不渲染)
    std::string depict;
    /// 取值类型: bool / int / double / string / enum (空按 string 处理)
    std::string type;
    /// 默认值 (空 = 按类型取默认: bool=false, enum=首项, 数值=0, 文本=空)
    std::string defaultValue;
    /// type=enum 的候选值
    std::vector<std::string> enumValues;
};

// ---------------------------------------------------------------------------
// 块构造 helper (按需组合自定义描述)
// ---------------------------------------------------------------------------

/// 候选项构造 (value 为结果中的原始值)
InterruptUiOption
    option(std::string value, std::string label, std::string labelKey = {}, std::string color = {});

/// 文本行 (字面文本)
InterruptUiBlock textBlock(
    std::string text,
    std::string color  = {},
    int         indent = 0,
    bool        wrap   = false,
    bool        bold   = false,
    bool        dim    = false
);

/// 文本行 (i18n 键优先, 字面文本作回退)
InterruptUiBlock textBlockKey(
    std::string textKey,
    std::string text,
    std::string color  = {},
    int         indent = 0,
    bool        wrap   = false,
    bool        bold   = false,
    bool        dim    = false
);

/// markdown 富文本块 (客户端按 markdown 渲染; 行式前端打印原文)
InterruptUiBlock markdownBlock(std::string markdown, int indent = 0);

/// 差异对比块
InterruptUiBlock diffBlock(std::string path, std::string oldStr, std::string newStr);

/// 分隔线
InterruptUiBlock separatorBlock(int indent = 0);

/// 空行
InterruptUiBlock gapBlock(int lines = 1);

/// 确认/取消行 (label/labelKey 空 = 客户端 i18n 默认 "确认"/"取消")
InterruptUiBlock submitBlock(
    std::string label          = {},
    std::string labelKey       = {},
    std::string cancelLabel    = {},
    std::string cancelLabelKey = {}
);

/// 横排按钮控件 (commitOnPick = 点击即选中并提交; 一问一答形态)
InterruptUiBlock buttonControl(
    std::string                    id,
    std::vector<InterruptUiOption> options,
    std::string                    label        = {},
    std::string                    labelKey     = {},
    agentxx::util::Json            defaultValue = {},
    bool                           commitOnPick = true
);

/// 勾选控件 (布尔值)
InterruptUiBlock checkboxControl(
    std::string id,
    std::string label,
    std::string labelKey     = {},
    bool        defaultValue = false,
    std::string help         = {},
    std::string helpKey      = {}
);

/// 文本输入控件
InterruptUiBlock textControl(
    std::string id,
    std::string label        = {},
    std::string labelKey     = {},
    std::string defaultValue = {},
    bool        multiline    = false,
    std::string help         = {}
);

/// 数值控件 (- 输入 +; integer/min/max/step)
InterruptUiBlock numberControl(
    std::string id,
    std::string label        = {},
    std::string labelKey     = {},
    double      defaultValue = 0.0,
    bool        integer      = false,
    double      step         = 1.0
);

/// 竖排单选列表控件
InterruptUiBlock selectControl(
    std::string                    id,
    std::vector<InterruptUiOption> options,
    std::string                    label        = {},
    std::string                    labelKey     = {},
    agentxx::util::Json            defaultValue = {},
    std::string                    help         = {}
);

// ---------------------------------------------------------------------------
// 预设模板
// ---------------------------------------------------------------------------

/// 类型化输入表单 (预设模板: 类型 → 控件形态的映射收敛在此)
///
/// - 每个输入项: 标签行 (accent, bold) + 说明行 (hint, 硬折行) + 控件
///   (bool→buttons 是/否且点击即提交 / enum→select / int|double→number /
///    string→text) + 输入项间空行
/// - 控件 id: 单输入项为 `value`; 多输入项为 `value1`..`valueN`
/// - 结果形态: `{"values": {"value": "true"}}` / `{"values": {"value1": ..., "value2": ...}}`
/// - 输入项为空时只渲染确认/取消行 (仅确认类询问)
InterruptUi inputForm(const std::vector<InputSpec>& inputs);

/// 是/否确认卡片 (预设模板; 用于 repeat_toolcall 类 "是否继续" 询问)
struct ConfirmCardOptions {
    /// 标题行 (accent, bold; 空 = 不渲染)
    std::string title;
    /// 说明文本 (markdown 块; 空 = 不渲染)
    std::string text;
    /// "是"按钮标签 (字面文本; 与 [yesLabelKey] 同时给出时以键为准, 文本忽略)
    std::string yesLabel;
    /// "是"按钮标签的 i18n 键 (空且 [yesLabel] 也为空时取 i18n `interrupt.yes`)
    std::string yesLabelKey;
    /// "否"按钮标签 (字面文本; 与 [noLabelKey] 同时给出时以键为准, 文本忽略)
    std::string noLabel;
    /// "否"按钮标签的 i18n 键 (空且 [noLabel] 也为空时取 i18n `interrupt.no`)
    std::string noLabelKey;
    /// 结果控件 id (取值 "true"/"false"; 消费端按该 id 读结果)
    std::string controlId = "allow";
    /// 默认选中项 (false = 默认 "否", 安全语义)
    bool defaultValue = false;
    /// 是否附 "记住此选择" 勾选项 (id = "remember")
    bool remember = false;
};

InterruptUi confirmCard(const ConfirmCardOptions& opts);

/// 权限询问卡片 (预设模板; 由权限服务端构造, 客户端不感知 permission 语义)
///
/// - 头行: 权限标记 (i18n `interrupt.permissionBadge`) + 工具名 (accent) +
///   权限分类 (hint)
/// - 目标描述 (hint, 硬折行); 目标为目录 (规范化路径带尾斜杠) 时紧随生效范围提示行
///   (textKey = `interrupt.rememberDir`, hint, 硬折行): 点击授权或完全授权均表示
///   同时授权子目录与文件
///   + 空行 + "记住此选择" 勾选 (id = "remember", i18n `interrupt.remember`)
///   + "完全授权所有权限" 勾选 (id = "fullAuth", i18n `interrupt.fullAuth`)
///   + 空行 + 允许/拒绝一键按钮
///   (控件 id = "decision", 点击即提交, i18n `interrupt.allow`/`interrupt.deny`)
/// - 文案只给 i18n 键, 不携带字面文本 (见命名空间说明)
/// - 结果形态: `{"values": {"decision": "true", "remember": false, "fullAuth": false}}`
InterruptUi
    permissionCard(std::string_view toolName, std::string_view category, std::string_view target);

} // namespace preset
} // namespace middleware
} // namespace agentxx
