#pragma once

#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx-client/io/tui/framework/tui_state.h"
#include "agentxx-client/io/tui/ui_items_render.h"
#include "agentxx/middlewares/interrupt_ui.h"
#include "ftxui/component/event.hpp"
#include "ftxui/component/mouse.hpp"
#include "ftxui/dom/elements.hpp"
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace client {

/// 中断表单的通用视图 (渲染 / 高度估算 / 交互 / 结果组装; UI 线程独占)
///
/// 设计目标: 中断询问的形态完全由**声明的 UI 描述**决定
/// (TUIMessage::InterruptData::ui, schema 见
/// [interrupt_ui.h](/agent/lib/include/agentxx/middlewares/interrupt_ui.h)),
/// 本类不含任何具体工具/节点 (含 permission) 的分支 —— 权限卡片、普通询问、
/// 后续新增的询问形态都走同一套渲染与交互实现:
/// - 描述 = 有序块列表 ([middleware::InterruptUi::blocks]): 内容块
///   (text/markdown/diff/separator/gap) 交给共享块渲染层
///   ([ui_items_render.h], 与插件装饰 items 同一实现), 控件块 (control) 与本
///   类的提交行 (submit) 由本类渲染 (需要表单状态)
/// - **单一布局过程**: [layoutForm] 依描述 + 表单状态逐块生成行模型
///   ([UiRow] 序列); 渲染 = 行序列包成 vbox, 高度估算 = 各行行数之和 ——
///   两者同源, 不存在"渲染与估算两套判定漂移"的问题
/// - 交互: 命中区域 = (消息下标, 块下标, 控件 id, 子序号), 语义按控件形态处理
/// - 结果: 控件 id → 值 的对象 (`{"values": {...}}`), 经结果通道回传 client
///   线程; 规则注册等业务语义完全由 agent 侧消费结果完成 (客户端不参与)
///
/// 约束: **一条中断消息 = 一份表单** (一次中断请求渲染为一条消息), 描述中的
/// 全部 control 块都是可交互控件, 用户一次提交全部值。
///
/// UI 状态 (各控件编辑文本/选中项/勾选项/校验提示) 存于本类的状态表,
/// key = 中断请求 id, 与消息内容 (可被复制/重放) 分离; 控件状态按 **控件 id**
/// 索引 (而非块下标), 版式变化不影响状态归属。
class InterruptView {
public:

    /// 控件命中区域 (渲染时经 reflect 填充; 点击命中检测读取最新布局位置,
    /// 构建阶段记录的值是空 Box —— reflect 在布局 SetBox 时才写回)
    struct HitBox {
        /// 消息下标 (0-based, 对应 TUIRenderState::messages)
        size_t msgIndex = 0;
        /// 描述块下标 (ui.blocks 的下标; 点击派发按它定位具体块, 与控件 id 无关)
        size_t blockIndex = 0;
        /// 控件 id (control 块为描述声明的 id; 提交行固定 "submit")
        std::string controlId;
        /// 子序号:
        /// - buttons / select: 候选项下标
        /// - number: 0=减, 1=加, 2=输入框
        /// - checkbox / text: 0
        /// - submit: 0=确认, 1=取消
        int sub = 0;
        /// 控件 Box (布局时 reflect 填充)
        std::shared_ptr<ftxui::Box> box;
    };

    /// 单个控件的表单状态 (UI 线程独占; 非消息内容)
    struct ControlState {
        /// text/number 输入框的当前文本 (初始 = 描述声明的默认值)
        std::string editText;
        /// 输入框是否已被编辑 (首次输入替换默认值, 与输入框激活语义一致)
        bool edited = false;
        /// buttons/select 的选中下标
        int selected = 0;
        /// checkbox 的勾选状态
        bool checked = false;
        /// 校验失败提示 (显示于该控件下方; 下次编辑时清除)
        std::string tip;
    };

    /// 中断表单状态 (UI 线程独占; 非消息内容)
    struct FormState {
        /// 各控件状态 (key = 控件 id)
        std::map<std::string, ControlState, std::less<>> controls;
        /// 键盘作用的控件 id (点击控件时更新; 无控件时为空)
        std::string focusedId;
        /// 修改计数 (驱动消息列表缓存失效与高度重估)
        uint64_t version = 0;

        /// 取指定控件状态 (不存在返回静态默认值)
        const ControlState& control(std::string_view id) const;
    };

    explicit InterruptView(TUICtx& ctx);

    /// 帧开头调用: 清空上一帧的命中区域 (本帧构建可见中断消息时重新填充)
    void beginFrame();

    /// 注册中断请求的结果回传通道 (client 线程经 enqueueUiAction 调用)
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

    /// 高度估算 (行数; 不含消息尾部空行) —— 与 build 同一套布局过程
    size_t estimate(const TUIMessage& msg, int width) const;

    /// 构建中断消息块 (Waiting 时渲染描述块与控件, 否则渲染状态行)
    /// - [mdBuilders] markdown 渲染器生命周期 (Element 内部容器指向它;
    ///   调用方 (消息列表) 须随 Element 持有, 否则悬空)
    ftxui::Element build(
        const TUIMessage&                                   msg,
        size_t                                              msgIndex,
        int                                                 maxWidth,
        std::vector<std::unique_ptr<markdown::DomBuilder>>& mdBuilders
    );

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

    /// 数值输入控件子命中序号: 0=减, 1=加, 2=输入框 (文本输入框恒为 0)
    static constexpr int kSubNumMinus = 0;
    static constexpr int kSubNumPlus  = 1;
    static constexpr int kSubNumEdit  = 2;
    /// 提交行子命中序号: 0=确认, 1=取消 (整体取消中断请求)
    static constexpr int kSubSubmitConfirm = 0;
    static constexpr int kSubSubmitCancel  = 1;
    /// 提交行在描述块未声明 id 时使用的固定命中 id
    static constexpr std::string_view kItemIdSubmit = "submit";

    /// 取消息对应的 UI 描述 (描述必填; 缺失/非法返回空描述, 由 build 输出诊断行)
    middleware::InterruptUi resolveUi(const TUIMessage& msg) const;

    /// 消息 → 中断请求 id (非中断消息返回 false)
    static bool requestIdOf(const TUIMessage& msg, int64_t& out);

    /// 控件 id (空 id 回退 "value")
    static std::string controlIdOf(const middleware::InterruptUiBlock& block);

    /// 按描述初始化单个控件状态 (默认值/选中项/勾选态)
    static void initControlState(const middleware::InterruptUiBlock& block, ControlState& state);

    /// 表单状态 (惰性创建并按描述初始化)
    FormState& uiStateFor(const TUIMessage& msg);
    /// 修改表单状态 (version 递增, 使消息列表缓存失效)
    FormState& mutateUiState(const TUIMessage& msg);

    /// 取消息的表单状态 (不创建; 不存在返回 nullptr) —— 估算路径使用
    const FormState* stateOf(const TUIMessage& msg) const;

    /// 文本解析: labelKey/textKey 优先 (客户端词表), 缺键回退字面文本
    std::string resolveLabel(std::string_view labelKey, std::string_view text) const;

    /// **单一布局过程**: 描述 + 表单状态 → 行模型 (UiRenderResult)
    /// - [state] 为空时按描述默认值布局 (估算路径)
    /// - [registerHits] 为真时把控件命中区域写入 hits_ (渲染路径)
    /// - [msgIndex] 仅命中登记使用 (估算路径可传 0)
    void layoutForm(
        const TUIMessage& msg,
        size_t            msgIndex,
        const FormState*  state,
        int               width,
        bool              registerHits,
        UiRenderResult&   out
    ) const;

    /// 控件块布局 (标签/说明 + 控件行 + 校验提示行; 命中区域按 [registerHits] 登记)
    void layoutControl(
        size_t                              msgIndex,
        size_t                              blockIndex,
        const middleware::InterruptUiBlock& block,
        const ControlState&                 state,
        int                                 width,
        bool                                registerHits,
        UiRenderResult&                     out
    ) const;

    /// 提交行布局 (确认/取消)
    void layoutSubmit(
        size_t                              msgIndex,
        size_t                              blockIndex,
        const middleware::InterruptUiBlock& block,
        bool                                registerHits,
        UiRenderResult&                     out
    ) const;

    /// 头行渲染 (默认前缀 / 自定义分段)
    ftxui::Element buildHeader(const middleware::InterruptUi& ui) const;

    /// 状态行 (Confirmed/Cancelled/Expired; 描述缺失时输出诊断行)
    ftxui::Element buildStatusLine(const TUIMessage& msg) const;

    /// 值按钮渲染 (复用插件按钮配色; active 时高亮)
    ftxui::Element renderValueButton(const middleware::InterruptUiOption& opt, bool active) const;

    /// 记录命中区域 (msgIndex + 块下标 + 控件 id + 子序号; box 经 shared_ptr 持有)
    void
        hit(size_t                             msgIndex,
            size_t                             blockIndex,
            std::string                        controlId,
            int                                sub,
            const std::shared_ptr<ftxui::Box>& box) const;

    /// 经结果通道回传 (通道缺失时静默丢弃并记日志)
    void sendSubmit(int64_t wireId, const InterruptFormSubmit& submit);

    /// 提交整份表单: 校验全部控件 (失败写各自 tip 不提交); 成功经通道回传
    /// `{"values": {控件 id: 值}}`
    void confirm(size_t msgIndex);

    /// 取消整个中断请求 (通道回传整体取消)
    void cancel(size_t msgIndex);

    /// 数值步进 (作用于指定控件; delta = ±step)
    void step(size_t msgIndex, std::string_view controlId, double delta);

    TUICtx& ctx_;

    /// 最近一次渲染的命中区域 (UI 线程独占; beginFrame 清空, 构建期填充;
    /// 布局函数为 const, 故此处 mutable)
    mutable std::vector<HitBox> hits_;

    /// 当前激活 (键盘作用) 的中断消息下标
    size_t activeMsg_ = static_cast<size_t>(-1);

    /// 中断请求 → 结果通道 (client 线程注入)
    std::map<int64_t, std::shared_ptr<InterruptResultChannel>> channels_;

    /// 中断请求 → 表单状态 (key = 中断请求 id, 一份表单一条状态)
    std::map<int64_t, FormState> states_;
};

} // namespace client
} // namespace agentxx
