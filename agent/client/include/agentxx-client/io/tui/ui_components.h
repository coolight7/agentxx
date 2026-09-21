#pragma once

/// 通用 UI 组件渲染 (唯一实现: 插件面板 / Info 段落 / 工具消息装饰 / 通用 overlay /
/// 中断描述内容块全部经此渲染)
///
/// 背景: 这些接入点此前各写一份 items 解析与渲染, 新增一种组件要改多处, 且高度
/// 估算与渲染容易漂移 (同一项两处判定不一致会造成滚动位置错乱)。本模块是
/// `agentxx.ui.item` schema 在终端里的**唯一渲染实现**:
/// - 解析一次 (`agentxx::ui::parseItem*`, 见 [item.h](/agent/lib/include/agentxx/ui/item.h))
/// - 渲染产出"行模型" ([UiRow]): 元素 + 行数 + 元素内可命中区域
/// - 高度估算 = 各行行数之和; 由 [measureItem] 走同一条渲染路径得出, 不存在两套判定
///
/// 命中口径:
/// - 行模型里的 [UiHitRegion] 使用**行元素局部坐标**; 顶层组件行在
///   [renderItem] 中附加 `reflect`, 命中时先定位到行, 再按局部坐标判定具体区域
/// - 滚动容器内的命中不要用 `reflect`, 应经 [Scrollable::hitTestItem] 定位子项
///   (滚动容器测量子项时会用临时大框布局, 视口外子项的反射框会残留)
///
/// 相关: 插件按钮样式见 [plugin_ui_items.h]; 主题色见 [tui_theme.h]
#include "agentxx-client/io/tui/framework/ui_hit.h"
#include "agentxx-client/io/tui/plugin_ui_items.h"
#include "agentxx-client/io/tui/tui_theme.h"
#include "agentxx/middlewares/interrupt_ui.h"
#include "agentxx/ui/item.h"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"
#include "markdown/dom_builder.hpp"
#include "utilxx_base/json.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace client {

/// 表单提交动作 id (插件面板/overlay 表单经动作通道回传)
/// - 提交: `actionId = "__submit"`, 参数 `{"values":{控件id:值}}`
/// - 取消: `actionId = "__cancel"`
/// - `commitOnPick` 控件点击即回传: `actionId = 控件 id`, 参数同上
inline constexpr std::string_view kFormSubmitActionId = "__submit";
inline constexpr std::string_view kFormCancelActionId = "__cancel";

/// 单个控件的表单状态 (UI 线程独占; 非界面描述的一部分)
struct UiFormControlState {
    /// 是否已按描述初始化 (未初始化时渲染按描述缺省值, 忽略下面的字段)
    bool initialized = false;
    /// 文本/数值输入框当前文本 (初始为描述声明的默认值)
    std::string editText;
    /// 输入框是否已被编辑 (首次输入替换默认值)
    bool edited = false;
    /// buttons/select 的选中下标
    int selected = 0;
    /// checkbox 的勾选状态
    bool checked = false;
    /// 校验失败提示 (显示在控件下方; 下次编辑时清除)
    std::string tip;
};

/// 一份表单的状态 (按控件 id 索引; 中断表单与插件表单共用同一结构与渲染)
struct UiFormState {
    /// 各控件状态 (key = 控件 id)
    std::map<std::string, UiFormControlState, std::less<>> controls;
    /// 键盘作用的控件 id (点击控件时更新; 无控件时为空)
    std::string focusedId;
    /// 修改计数 (驱动缓存失效与高度重估)
    uint64_t version = 0;

    /// 取控件状态 (不存在返回 nullptr)
    const UiFormControlState* find(std::string_view id) const {
        auto it = controls.find(id);
        return (it == controls.end()) ? nullptr : &it->second;
    }

    /// 取控件状态 (不存在则按 defaultValue 创建)
    UiFormControlState& ensure(std::string_view id) {
        return controls[std::string{id}];
    }
};

/// 分隔线风格 (面性弹窗与普通列表的视觉语言不同)
enum class UiSeparatorStyle : uint8_t {
    /// 细横线 "─" (默认; 消息列表 / 侧边栏等有边框的界面)
    Line = 0,
    /// 整行浅色背景区块 (面性风格弹窗: 不使用任何框线)
    Block = 1,
};

/// 渲染上下文
struct UiRenderCtx {
    /// 主题 (必填; 为空时不渲染)
    const TUITheme* theme = nullptr;
    /// 可用显示宽度 (含缩进; <=0 = 不限宽, 由元素自身 flex 决定)
    int width = 0;
    /// 基础缩进列数 (插件装饰 items 为 4; 中断块为 0, 缩进由块自身声明)
    int indent = 0;
    /// 归属插件名 (按钮可点性判断与命中归属)
    std::string plugin;
    /// 归属 owner id (插件动作派发用; 中断块为空)
    std::string ownerId;
    /// 客户端插件 UI 注册表快照 (判断按钮是否有绑定回调; 可为空)
    const agentxx::plugin::ClientUiRegistry* registry = nullptr;
    /// 折叠状态查询: 键 = 组件 id, 返回是否展开; 为空时用描述里的 `expanded` 字段
    std::function<bool(const std::string& id, bool defaultValue)> collapseExpanded;
    /// 表单状态 (控件渲染; 为空时按描述缺省值渲染静态形态, 不可交互)
    const UiFormState* form = nullptr;
    /// 分隔线风格 (面性弹窗传 Block)
    UiSeparatorStyle separatorStyle = UiSeparatorStyle::Line;
};

/// 渲染行: 一行元素 + 该行占用的显示行数 + 元素内的可命中区域
///
/// - `lines` 通常为 1; markdown/diff/表格/容器等整体渲染的块可为多行
/// - `regions` 使用本行元素的局部坐标; `box` 由 [renderItem] 附加 reflect 后
///   在布局阶段填充为屏幕区域 (无命中区域时为空指针)
struct UiRow {
    ftxui::Element                element;
    size_t                        lines = 1;
    std::shared_ptr<ftxui::Box>   box;
    std::vector<UiHitRegion>      regions;
};

/// 渲染产物 (行序列 + 生命周期附件)
struct UiRenderResult {
    std::vector<UiRow> rows;
    /// markdown DomBuilder 生命周期 (Element 内部容器/链接 Box 指向它;
    /// 须由调用方随 Element 一同持有, 否则悬空)
    std::vector<std::unique_ptr<markdown::DomBuilder>> builders;
};

/// 渲染一组组件项 (每一项可能产出多行; 含可点内容的行附加 reflect 命中框)
void renderItems(
    const std::vector<agentxx::ui::Item>& items,
    const UiRenderCtx&                    ctx,
    UiRenderResult&                       out
);

/// 渲染单个组件项 (追加到 out.rows)
void renderItem(
    const agentxx::ui::Item& item,
    const UiRenderCtx&       ctx,
    UiRenderResult&          out
);

/// 渲染 items JSON (`{"items":[...]}` 或裸数组)
void renderItemJson(const utilxx_base::Json& json, const UiRenderCtx& ctx, UiRenderResult& out);

/// 单个组件项的行数 (与 [renderItem] 同一实现: 内部渲染一次后取行数之和)
/// - 供高度估算使用 (不可见子项的高度), 不会少算或多算容器类组件
size_t measureItem(const agentxx::ui::Item& item, const UiRenderCtx& ctx);

/// 一组组件项的行数
size_t measureItems(const std::vector<agentxx::ui::Item>& items, const UiRenderCtx& ctx);

/// 取组件项使用的语义色 (与 [uiRoleColor] 同一映射)
ftxui::Color itemColor(std::string_view color, const TUITheme& theme);

// ---------------------------------------------------------------------------
// 表单交互 (面板/Info 段落/overlay 的表单共用; 中断表单沿用自身状态结构)
// ---------------------------------------------------------------------------

/// 表单交互结果
enum class UiFormAction : uint8_t {
    None    = 0, ///< 未产生变化
    Changed = 1, ///< 状态已变化 (调用方应重绘)
    Submit  = 2, ///< 请求提交 (调用方组装值并经动作通道回传)
    Cancel  = 3, ///< 请求取消
};

/// 处理控件区域命中 (更新表单状态)
/// - checkbox: 翻转勾选态; buttons/select: 选中 `sub` 指定的候选项;
///   number: sub 0/1 为减/加, sub 2 为聚焦输入框; text: 聚焦输入框
/// - `commitOnPick` 的候选项点击后返回 `Submit`
/// - 未命中 (控件不存在 / sub 越界) 返回 `None`
UiFormAction handleFormControlHit(
    const std::vector<agentxx::ui::Item>& items,
    UiFormState&                          form,
    std::string_view                      controlId,
    int                                   sub
);

/// 处理提交行命中 (id 为 `__submit` / `__cancel`)
UiFormAction handleFormSubmitHit(std::string_view actionId);

/// 键盘输入作用于当前焦点控件 (返回 true 表示已消费该事件)
/// - 可打印字符追加到输入框; Backspace 删除一个字符; Delete 清空;
///   Tab / Shift+Tab 在控件间移动焦点; Escape 释放焦点
/// - 回车与提交由调用方处理 (本方法不消费回车)
bool handleFormKeyInput(
    const std::vector<agentxx::ui::Item>& items,
    UiFormState&                          form,
    const ftxui::Event&                   event
);

/// 校验全部控件 (number 的范围/步进; 失败时写各控件 `tip` 并返回 false)
bool validateForm(const std::vector<agentxx::ui::Item>& items, UiFormState& form);

/// 组装提交值: `{"values": {控件 id: 值}}`
/// - checkbox → 布尔; buttons/select → 候选项 value; number → 数值; text → 字符串
/// - 未初始化的控件按描述缺省值取值
utilxx_base::Json formValues(const std::vector<agentxx::ui::Item>& items, UiFormState& form);

/// 按组件描述初始化表单状态 (缺省值/选中项/勾选态/编辑文本)
/// - 已初始化的控件不会被覆盖 (保留用户输入); 递归处理容器内的控件
/// - 宿主在创建或刷新一份表单时调用一次 (之后用户交互只改状态表)
void initFormState(UiFormState& form, const std::vector<agentxx::ui::Item>& items);

/// 收集组件树中的全部控件 id (含容器内; 供表单状态清理使用)
std::vector<std::string> collectControlIds(const std::vector<agentxx::ui::Item>& items);

/// 中断描述块 → 组件项
/// - 内容块 (text/markdown/diff/separator/gap) 按字段映射
/// - `control` / `submit` / `custom` 按共享控件语义映射 (字段一一对应)
/// - 其他 kind 视为扩展组件: 按块的原始 JSON (`InterruptUiBlock::raw`) 解析
///   (例如表格/树/横排/分组等, schema 同 `agentxx.ui.item`)
/// - 无法识别时返回 nullopt (调用方按 fallback 文本降级)
std::optional<agentxx::ui::Item> itemFromInterruptBlock(const middleware::InterruptUiBlock& block);

} // namespace client
} // namespace agentxx
