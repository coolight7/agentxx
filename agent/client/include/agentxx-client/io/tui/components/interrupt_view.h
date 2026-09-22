#pragma once

#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx-client/io/tui/framework/tui_state.h"
#include "agentxx-client/io/tui/ui_components.h"
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
/// - 描述 = 有序块列表 ([middleware::InterruptUi::blocks]): **全部块都交给共享
///   组件渲染层** ([ui_components.h], 与插件面板/Info/overlay 同一实现) ——
///   内容块 (text/markdown/diff/separator/gap)、扩展组件 (表格/树/横排/分组/
///   趋势图等)、控件块 (control) 与提交行 (submit) 都由它渲染
/// - 控件状态与交互 (点击/键盘/校验/取值) 全部复用共享表单层
///   ([UiFormState] + [handleFormControlHit]/[handleFormKeyInput]/
///   [validateForm]/[formValues]), 中断与插件表单的行为只有"结果去处"不同:
///   中断经结果通道回传 `{"values": {...}}`, 插件表单经动作通道回传
/// - **单一布局过程**: [layoutForm] 依描述 + 表单状态逐块渲染行模型
///   ([UiRow] 序列); 渲染 = 行序列包成 vbox, 高度估算 = 各行行数之和 ——
///   两者同源, 不存在"渲染与估算两套判定漂移"的问题
/// - 交互: 命中区域 = (消息下标, 块下标, 控件 id, 子序号) + 控件在行元素内的
///   区域矩形 ([UiHitRegion]); 语义由共享表单层处理
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

    /// 控件命中区域 (渲染时由共享组件层的行模型登记; 点击命中检测按"行元素框 +
    /// 行内区域"两级判定, 局部坐标口径与 [UiHitRegion] 一致)
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
        /// 控件所在行元素框 (布局时 reflect 填充; 为空表示尚未布局)
        std::shared_ptr<ftxui::Box> box;
        /// 控件在行元素内的区域 (局部坐标)
        UiHitRegion region;
    };

    /// 控件状态 (与共享表单层同一结构: 中断表单与插件表单共用渲染/交互/校验)
    using ControlState = UiFormControlState;

    /// 表单状态 (key = 控件 id; 与共享表单层同一结构)
    using FormState = UiFormState;

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

    /// 描述里的控件/提交行块 → 共享组件项 (附来源块下标, 渲染时用于命中归因)
    struct FormItem {
        /// 来源块下标 (ui.blocks 的下标)
        size_t blockIndex = 0;
        /// 组件项 (控件 = control; 提交行 = submit)
        agentxx::ui::Item item;
    };

    /// 取消息对应的 UI 描述 (描述必填; 缺失/非法返回空描述, 由 build 输出诊断行)
    middleware::InterruptUi resolveUi(const TUIMessage& msg) const;

    /// 消息 → 中断请求 id (非中断消息返回 false)
    static bool requestIdOf(const TUIMessage& msg, int64_t& out);

    /// 控件 id (空 id 回退 "value")
    static std::string controlIdOf(const middleware::InterruptUiBlock& block);

    /// 描述 → 控件/提交行组件项 (标签与说明的 i18n 键在此解析; 提交行缺省文案
    /// 用中断词表 "确认"/"✕", 与插件表单的 "提交"/"取消" 区分)
    std::vector<FormItem> formItems(const middleware::InterruptUi& ui) const;

    /// 组件项列表 (去掉块下标; 共享表单层接口用)
    static std::vector<agentxx::ui::Item> plainItems(const std::vector<FormItem>& items);
    /// 首个控件 id (无控件返回空串)
    static std::string firstControlId(const std::vector<agentxx::ui::Item>& items);

    /// 表单状态 (惰性创建并按描述初始化; 首个控件为键盘焦点)
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

    /// 登记某个块渲染出的控件命中区域 (行元素框 + 行内区域两级命中)
    /// - `firstRow`: 该块在 `out.rows` 中的首个行下标 (渲染前的行数)
    /// - 只登记控件区域 (Form) 与提交行区域 (FormSubmit); 描述里的普通动作区域
    ///   (如扩展组件内的按钮) 没有中断侧的结果去处, 不参与命中
    void registerBlockHits(
        size_t                msgIndex,
        size_t                blockIndex,
        const UiRenderResult& out,
        size_t                firstRow
    ) const;

    /// 头行渲染 (默认前缀 / 自定义分段)
    ftxui::Element buildHeader(const middleware::InterruptUi& ui) const;

    /// 状态行 (Confirmed/Cancelled/Expired; 描述缺失时输出诊断行)
    ftxui::Element buildStatusLine(const TUIMessage& msg) const;

    /// 记录命中区域 (msgIndex + 块下标 + 控件 id + 子序号; box 经 shared_ptr 持有)
    void
        hit(size_t                             msgIndex,
            size_t                             blockIndex,
            std::string                        controlId,
            int                                sub,
            const std::shared_ptr<ftxui::Box>& box,
            const UiHitRegion&                 region) const;

    /// 经结果通道回传 (通道缺失时静默丢弃并记日志)
    void sendSubmit(int64_t wireId, const InterruptFormSubmit& submit);

    /// 提交整份表单: 校验全部控件 (失败写各自 tip 不提交); 成功经通道回传
    /// `{"values": {控件 id: 值}}`
    void confirm(size_t msgIndex);

    /// 取消整个中断请求 (通道回传整体取消)
    void cancel(size_t msgIndex);

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
