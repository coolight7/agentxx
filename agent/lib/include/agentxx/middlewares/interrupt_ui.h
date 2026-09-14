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
    agentxx::util::Json      toJson() const;
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

/// 值按钮 (input 项 view="buttons" 时的一键取值按钮)
/// - 点击 = 选中该按钮并提交表单 (允许/拒绝、是/否等一问一答形态)
struct InterruptUiButton {
    /// 点击写入的输入值 (bool 形态固定 "true"/"false")
    std::string value;
    /// 字面标签
    std::string label;
    /// 客户端 i18n 键 (优先)
    std::string labelKey;
    /// 文本色覆盖 (空 = 按钮样式自带; 如 "error" 用于高危操作)
    std::string color;

    static InterruptUiButton fromJson(const agentxx::util::Json& j);
    agentxx::util::Json     toJson() const;
};

/// 通用默认描述的输入项声明
/// - 字段与 [InterruptHandleArg::InterruptHandleInputItem] 一致 (本头文件不反向
///   依赖 middleware.h, 故单独声明同构结构; 生产方为一次询问填同一份内容)
struct InterruptUiInputSpec {
    /// 控件标签 (渲染在控件上方; 空 = 不渲染标签行)
    std::string label;
    /// 控件说明文本 (渲染在标签下方, 按宽度硬折行; 空 = 不渲染)
    std::string depict;
    /// 取值类型: bool / int / double / string / enum
    std::string type;
    /// 默认值 (空 = 客户端按类型取默认)
    std::string defaultValue;
    /// type=enum 的候选值
    std::vector<std::string> enumValues;
};

/// 中断 UI 项 (声明式; 客户端通用渲染, 不感知具体工具/节点)
///
/// kind:
/// - `text`      文本行 (role/color 着色, indent 缩进, wrap 按宽度硬折行)
/// - `gap`       空行 (lines 行)
/// - `toggle`    勾选项 (整行可点; 值进入结果 options, 如"记住此选择")
/// - `input`     输入控件 (值进入结果 values; 一条描述内可有多个 = 多个控件)
/// - `submit`    确认/取消按钮行
/// - `separator` 分隔线
/// - `diff`      差异对比 (与工具装饰 diff 项同渲染)
///
/// **表单语义**: 一条中断消息 = 一份表单, items 中可含**多个** input 项
/// (每个 input 项 = 一个控件)。各字段自包含 (客户端不读取消息上的输入项
/// 字段), 故生产者必须为每个 input 项声明 inputType/defaultValue/enumValues
/// (或提供 view/buttons)。结果 values 的顺序由 [InterruptUi::values] 声明
/// (留空 = 按 items 中 input 项出现顺序)。
struct InterruptUiItem {
    /// 项类型 (见上; 未知 kind 客户端忽略, 保证向前兼容)
    std::string kind;

    // ---- text / toggle 标签 / input 控件标签 ----
    std::string text;
    /// 文本项/标签的 i18n 键 (客户端优先解析, 缺键回退 text)
    std::string labelKey;
    /// 主题色名 (error/accent/hint/normal/thinking/tool; 空 = normal)
    std::string color;
    bool        bold   = false;
    bool        dim    = false;
    /// 按可用宽度硬折行 (false = 单行, 超宽右缘裁剪)
    bool wrap = false;
    /// 左侧缩进空格数
    int indent = 0;

    // ---- gap ----
    int lines = 1;

    // ---- toggle / input ----
    /// 项 id: toggle 项 = 结果 options 的键; input 项 = 结果 values 顺序声明的键
    std::string id;
    /// 初始勾选状态 (toggle)
    bool defaultToggle = false;

    // ---- input (自包含) ----
    /// 输入类型: bool/int/double/string/enum
    std::string inputType;
    /// 默认值 (空 = 客户端按类型取默认)
    std::string defaultValue;
    /// 枚举候选 (inputType=enum)
    std::vector<std::string> enumValues;
    /// 控件形态: buttons/number/text/list (空 = 按 inputType 推导)
    std::string view;
    /// view=buttons 时的按钮列表 (空 = 客户端内置的是/否按钮)
    std::vector<InterruptUiButton> buttons;

    // ---- diff ----
    std::string path;
    std::string oldStr;
    std::string newStr;

    static InterruptUiItem fromJson(const agentxx::util::Json& j);
    agentxx::util::Json    toJson() const;
};

/// 中断 UI 描述 (整份下发; 服务端声明, 客户端通用渲染)
///
/// 一条中断请求对应**一份表单** (客户端渲染为一条消息), items 中可含多个
/// input 控件, 用户一次提交全部值。
///
/// 结果契约 (`{"values":[...], "options":{...}}`, 见 [makeInterruptResult]):
/// - `values`: 输入控件值数组, 顺序 = [values] 声明的 id 顺序 (未声明 = items 中
///   input 项顺序); 与 [InterruptHandleArg::inputs] 的顺序一一对应 (同一值契约)
/// - `options`: 勾选项映射 (toggle 项 id → 布尔值)
struct InterruptUi {
    /// schema 版本 (当前 1; 客户端按已知字段解析, 未知项类型/未知字段忽略)
    int version = 1;
    /// 头行
    InterruptUiHeader header;
    /// 项列表 (按顺序渲染; 可含多个 input 项)
    std::vector<InterruptUiItem> items;
    /// 结果 values 顺序 (input 项 id 列表; 空 = 取 items 中 input 项顺序)
    std::vector<std::string> values;
    /// 结果 options 的 id 顺序 (空 = 取全部 toggle 项)
    std::vector<std::string> options;

    bool empty() const {
        return items.empty() && header.segments.empty();
    }

    static InterruptUi   fromJson(const agentxx::util::Json& j);
    agentxx::util::Json  toJson() const;

    /// 通用默认描述: 按输入项声明展开成自包含表单
    /// - 每个输入项渲染为: 标签行 (label, accent) + 说明行 (depict, hint, 硬折行)
    ///   + 输入控件 (类型/默认值/枚举候选取声明) + 输入项间空行
    /// - 结果 values 顺序 = 输入项顺序 (单项 id = "value"; 多项 id = "value1".."valueN")
    /// - 输入项为空时 (如子代理委派类的"仅确认"询问) 只渲染确认/取消行
    static InterruptUi defaultUi(const std::vector<InterruptUiInputSpec>& inputs);

    /// 权限询问卡片描述 (由权限服务端构造, 客户端不感知 permission):
    /// 头行 "! [Permission] {toolName} {category}" + 目标描述(硬折行) +
    /// "记住此选择" 勾选项 + 允许/拒绝一键按钮
    /// - 允许/拒绝即提交 (无单独确认行); 勾选后提交时按本次选择注册路径规则
    static InterruptUi permissionUi(
        std::string_view toolName,
        std::string_view category,
        std::string_view target
    );
};

/// 中断结果组装 (客户端提交后回传的 JSON 形态; 与 agent 侧解析口径一致)
///
/// 结果**恒为对象形态**: `{"values":[...], "options":{"id":bool,...}}`
/// - values: 输入控件值数组 (顺序 = 描述声明的 values/控件顺序; 取消或全部未填时为空数组)
/// - options: 勾选项映射 (描述 result.options 声明的 id → 布尔值; 无勾选项时为空对象)
///
/// - `args`:
///     - [values]  输入控件值数组
///     - [options] 勾选项映射 (描述 result.options 声明的 id → 布尔值)
agentxx::util::Json
    makeInterruptResult(const agentxx::util::Json& values, const agentxx::util::Json& options);

} // namespace middleware
} // namespace agentxx
