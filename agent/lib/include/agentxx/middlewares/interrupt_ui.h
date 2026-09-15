#pragma once

#include "agentxx/util/json.h"
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace middleware {

/// 中断 UI 分段 (头行的分段文本, 例如 "! [Permission] " + 工具名 + 权限分类)
struct InterruptUiSegment {
    /// 字面文本 (labelKey 为空时使用; 客户端渲染语言不匹配时的回退)
    std::string text;
    /// 客户端 i18n 键 (客户端优先解析; 其他前端/缺键时回退 text)
    std::string labelKey;
    /// 主题色名: error / accent / hint / normal / thinking / tool (空 = normal)
    std::string color;
    bool        bold = false;
    bool        dim  = false;

    static InterruptUiSegment fromJson(const agentxx::util::Json& j);
    agentxx::util::Json       toJson() const;
};

/// 中断头行描述
///
/// - `segments` 非空: 完全按分段渲染 (客户端不附加任何语义)
/// - `segments` 为空: 客户端渲染通用默认前缀 (i18n 键 `interrupt.header`)
struct InterruptUiHeader {
    /// 自定义分段 (空 = 客户端默认前缀)
    std::vector<InterruptUiSegment> segments;

    static InterruptUiHeader fromJson(const agentxx::util::Json& j);
    agentxx::util::Json      toJson() const;
};

/// 控件候选项 (kind=control 的 buttons / select 控件使用)
///
/// - `value` 为选中后写入结果的**原始值** (字符串/数值/布尔均可)
/// - `labelKey` 非空时客户端优先解析该 i18n 键, 缺键回退 `label`
struct InterruptUiOption {
    /// 选中时写入结果的原始值 (缺失按空字符串)
    agentxx::util::Json value;
    /// 字面标签
    std::string label;
    /// 客户端 i18n 键 (优先)
    std::string labelKey;
    /// 文本色覆盖 (空 = 按控件样式; 如 "error" 用于高危操作)
    std::string color;

    static InterruptUiOption fromJson(const agentxx::util::Json& j);
    agentxx::util::Json      toJson() const;
};

/// 中断描述块 (客户端按 blocks 顺序渲染; 内容块与控件块可任意混排)
///
/// kind (块类型):
/// - `text`      文本行 (`text`/`textKey` + color/bold/dim/wrap/indent)
/// - `markdown`  markdown 富文本块 (客户端按 markdown 渲染; 行式前端打印原文)
/// - `diff`      差异对比 (`path`/`oldStr`/`newStr`)
/// - `separator` 分隔线 (`indent`)
/// - `gap`       空行 (`lines` 行)
/// - `control`   交互控件 (值进入结果 `values[id]`, 见下)
/// - `submit`    确认/取消行 (`label`/`labelKey` = 确认, `cancelLabel`/`cancelLabelKey` = 取消)
/// - `custom`    自定义渲染块 (**字段预留, 暂未实现**)
///
/// control (控件形态; 形态即语义, **没有** "参数类型" 概念):
/// - `buttons`  横排按钮 (`options`; `commitOnPick` = 点击即选中并提交整份表单)
/// - `select`   竖排单选列表 (`options`; 键盘 ↑↓ 选择, 提交行提交)
/// - `text`     文本输入框 (`multiline` 预留)
/// - `number`   数值控件 (`integer`/`min`/`max`/`step`)
/// - `checkbox` 勾选项 (布尔值)
///
/// 未知 kind / 未知 control: 客户端渲染 fallback 文本或诊断行, 不使整份描述失效
/// (向前兼容, 且不静默丢内容)。
struct InterruptUiBlock {
    /// 块类型 (见上)
    std::string kind;

    // ---- text / markdown: 文本内容 ----
    /// 字面文本 (markdown 块为 markdown 源码)
    std::string text;
    /// `text` 块的 i18n 键 (客户端优先解析, 缺键回退 `text`)
    std::string textKey;

    // ---- text 样式 ----
    /// 主题色名 (error/accent/hint/normal/thinking/tool; 空 = normal)
    std::string color;
    bool        bold = false;
    bool        dim  = false;
    /// 按可用宽度硬折行 (false = 单行, 超宽右缘裁剪)
    bool wrap = false;
    /// 左侧缩进空格数
    int indent = 0;

    // ---- gap ----
    int lines = 1;

    // ---- diff ----
    std::string path;
    std::string oldStr;
    std::string newStr;

    // ---- control: 标识与标签 ----
    /// 控件 id (结果 `values` 的键; 同一描述内必须唯一)
    std::string id;
    /// 控件形态 (buttons/select/text/number/checkbox; 见上)
    std::string control;
    /// 控件标签 (渲染在控件上方; 空 = 不渲染标签行)
    std::string label;
    /// 控件标签的 i18n 键 (优先)
    std::string labelKey;
    /// 控件说明 (渲染在标签下方, 按宽度硬折行; 空 = 不渲染)
    std::string help;
    /// 控件说明的 i18n 键 (优先)
    std::string helpKey;

    // ---- control: 取值 ----
    /// 候选项 (buttons/select)
    std::vector<InterruptUiOption> options;
    /// 缺省值 (checkbox = 布尔; number = 数值; 其余 = 候选项值/文本)
    agentxx::util::Json defaultValue;
    /// buttons: 点击即选中并提交整份表单 (一问一答形态)
    bool commitOnPick = false;

    // ---- control: number ----
    /// 仅接受整数 (校验/步进口径)
    bool integer = false;
    /// 下界 (hasMin = false 表示不限)
    bool   hasMin   = false;
    double minValue = 0.0;
    /// 上界 (hasMax = false 表示不限)
    bool   hasMax   = false;
    double maxValue = 0.0;
    /// 步进量 (<=0 按 1)
    double step = 1.0;

    // ---- control: text ----
    /// 多行输入框 (**预留字段, 当前按单行渲染**)
    bool multiline = false;

    // ---- submit ----
    /// 取消标签 (空 = 客户端 i18n 默认 "取消")
    std::string cancelLabel;
    /// 取消标签的 i18n 键 (优先)
    std::string cancelLabelKey;

    // ---- custom (预留字段, 暂未实现) ----
    /// 自定义渲染组件名 (客户端注册的渲染器键)
    ///
    /// TODO(自定义渲染): 客户端侧组件渲染器注册与派发尚未实现 —— 当前客户端
    /// 对 `custom` 块只渲染 `fallback` 文本 (无 fallback 时输出诊断行);
    /// 后续接入客户端插件渲染器后, 按组件名 + props 渲染, fallback 作为降级。
    std::string component;
    /// 组件属性 (原样透传给客户端渲染器)
    agentxx::util::Json props;
    /// 客户端无该组件时的降级文本 (行式前端/未知组件时打印)
    std::string fallback;

    static InterruptUiBlock fromJson(const agentxx::util::Json& j);
    agentxx::util::Json     toJson() const;
};

/// 中断 UI 描述 (整份下发; 服务端声明, 客户端通用渲染)
///
/// 一条中断请求对应**一份表单** (客户端渲染为一条消息): blocks 内可含多个
/// 控件块, 用户一次提交全部值。
///
/// 结果契约 (`{"values": {控件 id: 值}}`, 见 [makeInterruptResult]):
/// - 键 = 描述中 `control` 块的 id; 值 = 控件当前值
///   (checkbox = 布尔 / number = 数值 / buttons|select = 候选项 value / text = 字符串)
/// - 空对象 = 用户未提交 (取消/中断过期): 消费端按未应答处理
///
/// **无历史版本兼容**: 本结构即 schema 首版 (`version = 1`), 客户端不检测版本;
/// 描述缺失/非法按契约错误处理 (渲染诊断行, 不可交互)。
struct InterruptUi {
    /// schema 版本 (当前 1; 仅作后续破坏性变更的标记位, 客户端不做版本分支)
    int version = 1;
    /// 头行
    InterruptUiHeader header;
    /// 描述块列表 (按顺序渲染)
    std::vector<InterruptUiBlock> blocks;

    bool empty() const {
        return blocks.empty() && header.segments.empty();
    }

    static InterruptUi  fromJson(const agentxx::util::Json& j);
    agentxx::util::Json toJson() const;
};

/// 中断结果组装 (客户端提交后回传的 JSON 形态; 与 agent 侧解析口径一致)
///
/// 结果**恒为对象形态**: `{"values": {控件 id: 值}}`
/// - 非对象 values 归一化为空对象 (取消/未提交时为空对象)
agentxx::util::Json makeInterruptResult(const agentxx::util::Json& values);

/// 结果值容错读取 (结果对象或其内层 `values` 对象均可传入)
///
/// - 对象含 `values` 对象时自动下钻 (兼容整体结果对象与纯 values 对象两种口径)
/// - 未命中 id / 类型不符时返回 defaultValue
/// - 布尔口径: 布尔值直取; 字符串 "true"/"yes"/"y"/"1" 视为 true
///   (与 [preset::inputForm] 生成的 bool 控件取值口径一致)
bool interruptValueBool(
    const agentxx::util::Json& values,
    std::string_view           id,
    bool                       defaultValue = false
);
std::string interruptValueString(
    const agentxx::util::Json& values,
    std::string_view           id,
    std::string_view           defaultValue = {}
);
int64_t interruptValueInt(
    const agentxx::util::Json& values,
    std::string_view           id,
    int64_t                    defaultValue = 0
);
double interruptValueDouble(
    const agentxx::util::Json& values,
    std::string_view           id,
    double                     defaultValue = 0.0
);

/// 描述降级为纯文本 (行式前端/日志/FFI 文本宿主用)
///
/// - text/markdown: 原文 (markdown 不解析, 保留源码)
/// - diff: 统一 diff 文本 (路径行 + 上下文/增删行)
/// - separator: "---"; gap: 空行
/// - control: "标签: 候选项/默认值 (控件形态)" 说明行
/// - submit: 不输出 (仅交互语义)
/// - custom: fallback 文本
///
/// - `args`:
///     - [width] 折行宽度 (<=0 不折行)
std::string interruptUiPlainText(const InterruptUi& ui, int width = 0);

} // namespace middleware
} // namespace agentxx
