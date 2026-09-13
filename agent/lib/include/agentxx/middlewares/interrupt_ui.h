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
/// 缺省形态 (segments 为空) 由客户端按语言渲染成进度前缀 + 输入项标签;
/// 自定义形态 (segments 非空) 完全由服务端描述, 客户端不再附加语义
struct InterruptUiHeader {
    /// 显示本次询问的输入项进度 (i/n; 取消息的 inputIndex/inputTotal)
    bool progress = false;
    /// 追加该输入项的标签 (消息的 inputLabel)
    bool label = true;
    /// 自定义分段 (非空 = 完全按分段渲染, 不再用客户端默认前缀)
    std::vector<InterruptUiSegment> segments;

    static InterruptUiHeader fromJson(const agentxx::util::Json& j);
    agentxx::util::Json      toJson() const;
};

/// 值按钮 (input 项 view="buttons" 时的一键取值按钮)
/// - 点击 = 选中该按钮并确认该输入项 (允许/拒绝、是/否等一问一答形态)
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

/// 中断 UI 项 (声明式; 客户端通用渲染, 不感知具体工具/节点)
///
/// kind:
/// - `text`      文本行 (role/color 着色, indent 缩进, wrap 按宽度硬折行)
/// - `gap`       空行 (lines 行)
/// - `toggle`    勾选项 (整行可点; 值进入结果 options, 如"记住此选择")
/// - `input`     输入项控件 (可编辑/可选; 值进入结果 values)
/// - `submit`    确认/取消按钮行
/// - `separator` 分隔线
/// - `diff`      差异对比 (与工具装饰 diff 项同渲染)
///
/// 模板语义: inputType/defaultValue/enumValues/text 留空时取消息字段
/// (inputType/inputDefault/inputEnums/inputDepict), 使同一份描述可用于
/// 多输入项的中断请求 (每项消息渲染各自的值域)
struct InterruptUiItem {
    /// 项类型 (见上; 未知 kind 客户端忽略, 保证向前兼容)
    std::string kind;

    // ---- text (text 项的内容 / toggle 项的标签) ----
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

    // ---- toggle ----
    /// 项 id (结果 options 的键)
    std::string id;
    /// 初始勾选状态
    bool defaultToggle = false;

    // ---- input ----
    /// 输入类型: bool/int/double/string/enum (空 = 取消息 inputType)
    std::string inputType;
    /// 默认值 (空 = 取消息 inputDefault)
    std::string defaultValue;
    /// 枚举候选 (空 = 取消息 inputEnums)
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
struct InterruptUi {
    /// schema 版本 (当前 1; 客户端按已知字段解析, 未知字段忽略)
    int version = 1;
    /// 头行
    InterruptUiHeader header;
    /// 项列表 (按顺序渲染)
    std::vector<InterruptUiItem> items;
    /// 结果 values 顺序 (空 = 取首个 input 项的值)
    std::vector<std::string> values;
    /// 结果 options 的 id 顺序 (空 = 取全部 toggle 项)
    std::vector<std::string> options;

    bool empty() const {
        return items.empty() && header.segments.empty();
    }

    static InterruptUi   fromJson(const agentxx::util::Json& j);
    agentxx::util::Json  toJson() const;

    /// 默认描述 (无描述时的通用兜底; 服务端与客户端同语义):
    /// 进度+标签头行 / 描述文本(取消息 inputDepict) / 按消息类型的输入控件 / 确认取消行
    static InterruptUi defaultUi();

    /// 权限询问卡片描述 (由权限服务端构造, 客户端不感知 permission):
    /// 头行 "! [Permission] {toolName} {category}" + 目标描述(硬折行) +
    /// "记住此选择" 勾选项 + 允许/拒绝一键按钮
    /// - 允许/拒绝即确认 (无单独确认行); 勾选后确认时按本次选择注册路径规则
    static InterruptUi permissionUi(
        std::string_view toolName,
        std::string_view category,
        std::string_view target
    );
};

/// 中断结果组装 (客户端确认后回传的 JSON 形态; 与 agent 侧解析口径一致)
/// - options 为空: 纯值数组 (兼容未声明勾选项的中断与旧服务端)
/// - options 非空: {"values":[...], "options":{"id":bool,...}}
///
/// - `args`:
///     - [values]  输入项值数组 (按 inputIndex 顺序)
///     - [options] 勾选项映射 (描述 result.options 声明的 id → 布尔值)
agentxx::util::Json
    makeInterruptResult(const agentxx::util::Json& values, const agentxx::util::Json& options);

} // namespace middleware
} // namespace agentxx
