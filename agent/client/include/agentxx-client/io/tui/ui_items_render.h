#pragma once

/// 通用 UI 块渲染 (插件装饰 items 与中断内容块**同一实现**)
///
/// 背景: 插件工具消息装饰 (tool decor items) 与中断描述的内容块
/// (text/markdown/diff/separator/gap) 展示语义高度重叠, 此前分别在
/// `message_list.cpp` 与 `interrupt_view.cpp` 各写一套 switch, 新增块类型要改
/// 多处, 且渲染与高度估算容易漂移 (同一项两处判定不一致会造成滚动位置错乱)。
/// 本模块是这些块类型的唯一实现:
/// - 渲染产出"行模型" ([UiRow]): 元素 + 行数 + 可选命中信息
/// - 高度估算 = 各行行数之和 ([measureUiItem] 与 [renderUiItem] 同一套判定)
///
/// 边界: 需要表单状态的交互控件 (中断 control 块) 与提交行由中断视图自行渲染,
/// 但复用同一 [UiRow] 行模型与文本折行 helper, 与之同处一个行序列。
#include "agentxx-client/io/tui/plugin_ui_items.h"
#include "agentxx-client/io/tui/tui_theme.h"
#include "agentxx/middlewares/interrupt_ui.h"
#include "agentxx/util/json.h"
#include "ftxui/dom/elements.hpp"
#include "markdown/dom_builder.hpp"
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace client {

/// 归一化 UI 项 (插件装饰 items 与中断内容块解析后的统一形态)
struct UiItem {
    /// 项类型: text / markdown / diff / separator / gap / button / diagram
    std::string kind;

    // ---- text / markdown ----
    /// 文本内容 (markdown 为源码)
    std::string text;
    /// 主题色名 (空 = normal): error/accent/hint/normal/thinking/tool
    /// (插件 items 的 role 字段亦映射到该色名, 见 uiRoleColor)
    std::string color;
    bool        bold = false;
    bool        dim  = false;
    /// 按可用宽度硬折行 (false = 单行, 超宽右缘裁剪)
    bool wrap = false;
    /// 左侧缩进空格数 (块自身缩进; 与渲染上下文的基础缩进叠加)
    int indent = 0;

    // ---- gap ----
    int lines = 1;

    // ---- diff ----
    std::string path;
    std::string oldStr;
    std::string newStr;

    // ---- button (插件可点按钮) ----
    bool             hasButton = false;
    PluginButtonDesc button;

    // ---- diagram (mermaid 状态图) ----
    std::string mermaid;
};

/// 渲染上下文
struct UiRenderCtx {
    /// 主题 (必填)
    const TUITheme* theme = nullptr;
    /// 可用显示宽度 (含缩进; <=0 = 不限宽, 由元素自身 flex 决定)
    int width = 0;
    /// 基础缩进列数 (插件装饰 items 为 4; 中断块为 0, 缩进由块自身声明)
    int indent = 0;
    /// 按钮归属的插件名 (可点性判断)
    std::string plugin;
    /// 按钮归属的 owner id (插件动作派发用; 中断块为空)
    std::string ownerId;
    /// 客户端插件 UI 注册表快照 (判断按钮是否有绑定回调; 可为空)
    const agentxx::plugin::ClientUiRegistry* registry = nullptr;
};

/// 渲染行: 一行元素 + 该行占用的显示行数 + 可选命中信息
///
/// - `lines` 通常为 1; markdown/diff 等整体渲染的块可为多行
/// - 命中信息由调用方转写为自己的命中表 (插件按钮 / 中断控件)
struct UiRow {
    ftxui::Element              element;
    size_t                      lines = 1;
    std::shared_ptr<ftxui::Box> box;
    /// 命中标识 (插件按钮 action_id; 中断控件 id)
    std::string hitId;
    /// 命中附加参数 (插件按钮 args json)
    std::string hitArgs;
    /// 命中归属 (插件 owner id; 中断块为空)
    std::string hitOwner;
    /// 命中子序号 (中断控件内部下标: 值按钮/枚举项/步进等)
    int hitSub = 0;
};

/// 渲染产物 (行序列 + 生命周期附件)
struct UiRenderResult {
    std::vector<UiRow> rows;
    /// markdown DomBuilder 生命周期 (Element 内部容器/链接 Box 指向它;
    /// 须由调用方随 Element 一同持有, 否则悬空)
    std::vector<std::unique_ptr<markdown::DomBuilder>> builders;
};

/// 插件装饰 item (JSON) → 归一化项
/// - 支持 kind: text / button / action / separator / diagram / diff / markdown
/// - 非对象 / 未知 kind 返回 nullopt (调用方忽略, 向前兼容)
std::optional<UiItem> uiItemFromPluginJson(const agentxx::util::Json& item);

/// 中断内容块 → 归一化项
/// - 支持 kind: text / markdown / diff / separator / gap
/// - 控件块 (control) / 提交行 (submit) / 自定义块 (custom) / 未知 kind
///   返回 nullopt (由中断视图自行处理)
std::optional<UiItem> uiItemFromInterruptBlock(const middleware::InterruptUiBlock& block);

/// 取项内容文本 (已应用基础缩进前的原文; 空 = 不渲染)
std::string uiItemText(const UiItem& item);

/// 项行数 (与 [renderUiItem] 同一套判定; 供高度估算, 不得构造 Element)
size_t measureUiItem(const UiItem& item, const UiRenderCtx& ctx);

/// 渲染项为行 (追加到 out.rows)
void renderUiItem(const UiItem& item, const UiRenderCtx& ctx, UiRenderResult& out);

} // namespace client
} // namespace agentxx
