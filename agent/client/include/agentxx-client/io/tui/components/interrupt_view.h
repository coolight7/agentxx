#pragma once

#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx-client/io/tui/framework/tui_state.h"
#include "agentxx/middlewares/interrupt_ui.h"
#include "ftxui/component/event.hpp"
#include "ftxui/component/mouse.hpp"
#include "ftxui/dom/elements.hpp"
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace agentxx {
namespace client {

/// 中断消息的通用视图 (渲染 / 高度估算 / 交互 / 结果组装; UI 线程独占)
///
/// 设计目标: 中断询问的形态完全由**声明的 UI 描述**决定
/// (TUIMessage::InterruptData::ui, schema 见
/// [interrupt_ui.h](/agent/lib/include/agentxx/middlewares/interrupt_ui.h)),
/// 本类不包含任何具体工具/节点 (含 permission) 的分支 —— 权限卡片、普通输入
/// 询问、后续新增的询问形态都走同一套渲染与交互实现:
/// - 渲染: 描述项 (text/gap/toggle/input/submit/separator/diff) → ftxui Element;
///   text/button/diff 等复用 plugin_ui_items 的共享 helper (与工具装饰同实现)
/// - 估算: 同一份描述逐项求行数 (与渲染同一套判定, 避免两处布局知识漂移)
/// - 交互: 命中区域 = (消息下标, 描述项 id, 子序号), 语义按项类型处理
/// - 结果: 输入项值 + 勾选项映射 (options) 经结果通道回传 client 线程;
///   规则注册等持久化语义完全由 agent 侧消费结果完成 (客户端不参与)
///
/// UI 状态 (编辑文本/选中项/勾选项/校验提示) 存于本类的状态表, key =
/// (中断请求 id, 输入项序号), 与消息内容 (可被复制/重放) 分离。
///
/// 约束: 一条中断消息对应一个输入项 (AgentIO 每个 InterruptHandleArg 输入项
/// 建一条消息), 故描述中的首个 input 项 (或 result.values[0] 指定的项) 为可
/// 交互输入项; 多余的 input 项忽略并在日志中提示。
class InterruptView {
public:

    /// 控件命中区域 (渲染时经 reflect 填充; 点击命中检测读取最新布局位置,
    /// 构建阶段记录的值是空 Box —— reflect 在布局 SetBox 时才写回)
    struct HitBox {
        /// 消息下标 (0-based, 对应 TUIRenderState::messages)
        size_t msgIndex = 0;
        /// 描述项 id (input 项为 "value", 勾选项为描述的 id, 确认/取消为 "submit")
        std::string itemId;
        /// 子序号: 值按钮下标 / 枚举项下标 / 数值控件 (0=减,1=加,2=输入框) /
        /// 提交行 (0=确认,1=取消)
        int sub = 0;
        /// 控件 Box (布局时 reflect 填充)
        std::shared_ptr<ftxui::Box> box;
    };

    /// 中断输入项的表单状态 (UI 线程独占; 非消息内容)
    struct FormState {
        /// 文本/数值输入框的当前文本 (初始 = 描述/消息的默认值)
        std::string editText;
        /// 输入框是否已被编辑 (首次输入替换默认值, 与输入框激活语义一致)
        bool edited = false;
        /// 值按钮/枚举项的选中下标
        int selected = 0;
        /// 勾选项状态 (描述项 id → 是否勾选)
        std::map<std::string, bool> toggles;
        /// 校验失败提示 (显示于控件下方; 下次编辑时清除)
        std::string tip;
        /// 修改计数 (驱动消息列表缓存失效与高度重估)
        uint64_t version = 0;
    };

    explicit InterruptView(TUICtx& ctx);

    /// 帧开头调用: 清空上一帧的命中区域 (本帧构建可见中断消息时重新填充)
    void beginFrame();

    /// 注册中断请求的结果回传通道 (client 线程经 enqueueUiAction 调用;
    /// 同请求的所有输入项共享同一通道)
    void attachChannel(int64_t wireId, std::shared_ptr<InterruptResultChannel> ch);

    /// 释放中断请求的通道映射与该请求全部 UI 状态 (中断流程结束时调用)
    void releaseChannel(int64_t wireId);

    /// 清空全部 UI 状态与通道映射 (消息整体替换/重连时调用)
    void clear();

    /// 是否为等待用户操作 (可交互) 的中断消息
    static bool isWaiting(const TUIMessage& msg);

    /// 当前激活 (键盘作用) 的中断消息下标; npos = 无
    size_t activeMsg() const;

    /// 设置激活消息 (点击控件时调用; 消息不存在/不可交互时忽略)
    void setActiveMsg(size_t msgIndex);

    /// 清除激活状态 (Esc / 中断结束)
    void clearActive();

    /// 表单状态修改计数 (消息列表 itemKey 的附加特征: 编辑值/勾选变化需使
    /// 懒列表缓存失效并重估高度; 无状态返回 0)
    uint64_t stateVersion(const TUIMessage& msg) const;

    /// 高度估算 (行数; 不含消息尾部空行) —— 与 build 使用同一套项判定
    size_t estimate(const TUIMessage& msg, int width) const;

    /// 构建中断消息块 (Waiting 时渲染描述项与控件, 否则渲染状态行)
    ftxui::Element build(const TUIMessage& msg, size_t msgIndex, int maxWidth);

    /// 鼠标点击命中处理 (命中并处理返回 true)
    /// - [areaBox] 消息列表内容区 (限制命中范围, 未布局时为空 Box)
    bool handleClick(const ftxui::Mouse& mouse, const ftxui::Box& areaBox);

    /// 键盘事件 (作用于当前激活的中断消息; 处理返回 true)
    bool handleKey(ftxui::Event event);

    /// 测试辅助: 最近一次渲染的命中区域
    const std::vector<HitBox>& hitBoxes() const {
        return hits_;
    }

    /// 测试辅助/外部查询: 指定消息的表单状态副本 (必要时按描述惰性初始化;
    /// 非中断消息返回默认值)
    FormState formState(size_t msgIndex);

private:

    /// 中断输入项 key (消息中 interruptId + inputIndex 唯一确定一个输入项)
    struct Key {
        int64_t id    = 0;
        int     index = 0;

        bool operator<(const Key& o) const {
            return id != o.id ? id < o.id : index < o.index;
        }
    };

    /// 解析后的描述项 (描述字段为空时按消息字段回退 —— 模板语义)
    struct Resolved {
        std::string kind;
        // text
        std::string text;
        std::string labelKey;
        std::string color;
        bool        bold   = false;
        bool        dim    = false;
        bool        wrap   = false;
        int         indent = 0;
        // gap
        int lines = 1;
        // toggle / input
        std::string                                id;
        bool                                       defaultToggle = false;
        std::string                                inputType;
        std::string                                defaultValue;
        std::vector<std::string>                   enums;
        std::string                                view;
        std::vector<middleware::InterruptUiButton> buttons;
        // diff
        std::string path, oldStr, newStr;

        /// 是否为可交互输入项 (kind == input)
        bool isInput() const {
            return kind == "input";
        }
    };

    /// 数值输入控件子命中序号: 0=减, 1=加, 2=输入框 (文本输入框恒为 0)
    static constexpr int kSubNumMinus = 0;
    static constexpr int kSubNumPlus  = 1;
    static constexpr int kSubEdit     = 2;
    /// 提交行子命中序号: 0=确认, 1=取消 (整体取消中断请求)
    static constexpr int kSubSubmitConfirm = 0;
    static constexpr int kSubSubmitCancel  = 1;
    /// 提交行/勾选项在描述项未声明 id 时使用的固定 id
    static constexpr std::string_view kItemIdSubmit = "submit";

    /// 取消息对应的 UI 描述 (消息自带描述优先; 缺失/空描述回退通用默认模板)
    middleware::InterruptUi resolveUi(const TUIMessage& msg) const;

    /// 由描述项解析出渲染所需的字段 (含消息字段回退与按钮内置回退)
    Resolved resolveItem(const middleware::InterruptUiItem& item, const TUIMessage& msg) const;

    /// 取交互输入项 (result.values[0] 指定的项优先, 否则首个 input 项; 无则 nullptr)
    const middleware::InterruptUiItem*
        findInputItem(const middleware::InterruptUi& ui, const TUIMessage& msg) const;
    /// 同上的解析版本 (无输入项返回 false)
    bool resolveInputItem(const TUIMessage& msg, Resolved& out) const;

    /// 消息 → 中断输入项 key (非中断消息返回 false)
    static bool keyOf(const TUIMessage& msg, Key& out);

    /// 表单状态 (惰性创建并按描述/消息初始化)
    FormState&       uiStateFor(const TUIMessage& msg);
    /// 修改表单状态 (version 递增, 使消息列表缓存失效)
    FormState&       mutateUiState(const TUIMessage& msg);

    /// 勾选项当前值 (未设置时取 defaultValue)
    bool toggleValue(const TUIMessage& msg, std::string_view id, bool defaultValue) const;

    /// 输入类型 → 默认控件形态 (bool=buttons / enum=list / int|double=number / 其余=text)
    static std::string defaultViewFor(std::string_view inputType);

    /// 内置的是/否值按钮 (描述未声明 buttons 时用于 bool 输入项)
    static std::vector<middleware::InterruptUiButton> builtinBoolButtons();

    /// 文本解析: labelKey 优先 (客户端词表), 缺键回退字面文本
    std::string resolveLabel(std::string_view labelKey, std::string_view text) const;

    /// 头行渲染 (默认进度前缀 / 自定义分段 + 输入项标签)
    ftxui::Element buildHeader(const TUIMessage& msg, const middleware::InterruptUi& ui) const;

    /// 渲染单个描述项到 rows (prev 为前一个已渲染项, 决定提交行是否与控件同行)
    void appendItemRows(
        const TUIMessage&    msg,
        size_t               msgIndex,
        const Resolved&      item,
        const Resolved*      prev,
        int                  maxWidth,
        ftxui::Elements&     rows
    );

    /// 单个描述项的估算行数 (与 appendItemRows 同一套判定)
    size_t estimateItemLines(
        const TUIMessage& msg,
        const Resolved&   item,
        const Resolved*   prev,
        int               width
    ) const;

    /// 状态行 (Confirmed/Cancelled/Expired)
    ftxui::Element buildStatusLine(const TUIMessage& msg) const;

    /// 值按钮渲染 (复用插件按钮配色; active 时高亮)
    ftxui::Element renderValueButton(const middleware::InterruptUiButton& btn, bool active) const;

    /// 记录命中区域 (msgIndex + 项 id + 子序号; box 经 shared_ptr 持有)
    void hit(size_t msgIndex, std::string itemId, int sub, const std::shared_ptr<ftxui::Box>& box);

    /// 经结果通道回传 (通道缺失时静默丢弃并记日志)
    void sendResult(
        int64_t                       wireId,
        int                           inputIndex,
        std::optional<std::string>    value,
        const agentxx::util::Json&    options
    );

    /// 确认输入项 (校验失败写 tip 不关闭); 成功经通道回传值 + 勾选项
    void confirm(size_t msgIndex);

    /// 取消整个中断请求 (同请求所有未操作项标记 Cancelled, 通道回传整体取消)
    void cancel(size_t msgIndex);

    /// 数值步进 (int/double)
    void step(size_t msgIndex, double delta);

    TUICtx& ctx_;

    /// 最近一次渲染的命中区域 (UI 线程独占; beginFrame 清空, 构建期填充)
    std::vector<HitBox> hits_;

    /// 当前激活 (键盘作用) 的中断消息下标
    size_t activeMsg_ = static_cast<size_t>(-1);

    /// 中断请求 → 结果通道 (client 线程注入; 同请求共享)
    std::map<int64_t, std::shared_ptr<InterruptResultChannel>> channels_;

    /// 输入项表单状态表 (key = 中断请求 id + 输入项序号)
    std::map<Key, FormState> states_;
};

} // namespace client
} // namespace agentxx
