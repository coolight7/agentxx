#pragma once

#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx-client/io/tui/lazy_scrollable.h"
#include "ftxui/component/component_base.hpp"
#include "ftxui/dom/elements.hpp"
#include <cstdint>
#include <map>
#include <markdown/dom_builder.hpp>
#include <markdown/incremental.hpp>
#include <memory>
#include <string_view>
#include <vector>

/// 消息列表组件 (Flutter ListView.builder 风格)
///
/// 渲染架构: 封装 LazyScrollable, 经 itemCount/itemKey/estimateHeight/buildItem
/// 四个回调描述列表, 仅按需懒构建子项:
/// - 有界 LRU 缓存 (条数 + 源字节双预算): 窗口外旧消息的渲染缓存被淘汰释放,
///   内存占用与对话长度解耦 —— 修复旧实现中渲染缓存随对话无限增长的问题
/// - 视口局部布局/绘制: 仅对可见消息做 markdown 解析与布局, 不可见消息零成本
/// - 高度估算: 未进入视口的消息使用按文本量估算的高度, 进入视口后实测修正
/// - itemKey 以消息指针 + 廉价特征构成 (内容变化必然伴随消息指针变化,
///   见 TUISharedState::mutableMessage), 避免旧实现对全部消息文本逐帧哈希
/// - 流式增量项 (currentToken) 标记为不可缓存, 每帧重建后即释放
///
/// 事件处理:
/// - 滚轮: 由内部 LazyScrollable 处理
/// - 左键点击 Thinking/Tool 消息: 折叠/展开
/// - 左键点击中断输入消息的控件 (是/否、±、枚举项、输入框、确认、取消):
///   切换选中 / 步进 / 聚焦编辑 / 确认 / 取消; 键盘 (字符/Backspace/方向键/
///   Enter/Esc) 作用于最近点击激活的中断消息
class MessageListComponent : public ftxui::ComponentBase {
public:

    /// 中断输入项 UI 状态 (UI 线程独占; 从 TUIMessage 迁出, 非消息内容)
    /// - 初始值按消息 InterruptData (inputType/inputDefault/inputEnums) 惰性计算
    /// - 经 mutateInterruptUiState 修改时 version 递增, 驱动 itemKey 变化
    ///   (编辑文本/校验提示变化影响渲染高度, 需使懒列表缓存失效重估)
    struct InterruptUIState {
        /// 数值/string 输入框编辑文本 (初始 = 默认值, 数值无默认时 "0"/"0.0")
        std::string editText;
        /// 输入框是否已编辑 (首次字符输入/步进后置 true):
        /// 初始默认值展示在输入框, 首次输入以新值覆盖默认 (与 stdio 逐行输入语义一致)
        bool edited = false;
        /// bool/enum 选中索引 (0=是/首项)
        int selected = 0;
        /// 权限询问: 是否记住本次选择 (确认后按本次允许/拒绝注册路径规则,
        /// 后续访问该路径或其子目录不再询问; 仅 rememberable 的权限询问显示开关)
        bool remember = false;
        /// 校验失败等提示 (显示于控件下方; 下次编辑时清除)
        std::string tip;
        /// 修改计数 (itemKey 失效用)
        uint64_t version = 0;
        // 结果回传通道不存于此: 经 attachInterruptChannel 注入 interruptChannels_
        // 映射 (同请求共享), 确认/取消时从映射取最新通道发送, 避免快照过期
    };

    /// 中断消息控件种类 (命中检测用)
    enum HitKind : uint8_t {
        kHitBoolYes  = 0, // bool "是" (点击即选中并激活)
        kHitBoolNo   = 1, // bool "否"
        kHitNumMinus = 2, // 数值 "-" 步进
        kHitNumPlus  = 3, // 数值 "+" 步进
        kHitEnumItem = 4, // 枚举项 (sub = 项索引)
        kHitEdit     = 5, // 输入框 (激活编辑)
        kHitConfirm  = 6, // 确认
        kHitCancel   = 7, // 取消整个中断请求
        kHitRemember = 8, // 权限询问 "记住" 开关 (切换 remember 状态)
    };

    /// 中断控件命中区域 (渲染时 reflect 填充, 供点击命中检测)
    struct InterruptHitBox {
        size_t  msgIndex = static_cast<size_t>(-1);
        uint8_t kind     = 0;
        int     sub      = 0;
        /// 指向控件 Box (布局时 reflect 填充): hits 在构建阶段 (布局前) 记录,
        /// 若值拷贝则拿到的是空 Box (reflect 在 SetBox 时才写回), 故持引用,
        /// 点击时读取的始终是最新布局位置
        std::shared_ptr<ftxui::Box> box;
    };

    explicit MessageListComponent(TUICtx& ctx);

    ftxui::Element OnRender() override;
    bool           OnEvent(ftxui::Event event) override;

    void setStickToBottom(bool v) {
        scrollable_->setStickToBottom(v);
    }

    bool isStickToBottom() const {
        return scrollable_->isStickToBottom();
    }

    int contentWidth() const {
        return scrollable_->contentWidth();
    }

    /// 主题变化后清空缓存 (颜色已过时)
    void invalidateCache();

    /// 处理可折叠消息的鼠标点击 (供外部 CatchEvent 调用); 返回是否消费了事件
    bool handleCollapsibleClick(const ftxui::Mouse& mouse);

    /// 测试辅助: 最近一次渲染的中断控件命中区域
    const std::vector<InterruptHitBox>& interruptHitBoxes() const {
        return interruptHits_;
    }

    /// 测试辅助: 上一帧可折叠消息 (Thinking/Tool/System) 的命中区域
    /// (与 collapsibleIndices_ 对应; 供测试模拟点击折叠/展开)
    const std::vector<ftxui::Box>& collapsibleBoxes() const {
        return collapsibleBoxes_;
    }

    /// 连接失败 banner 的"重试"按钮命中区域 (渲染时 reflect 填充,
    /// 供 TUIClientAgentIO 全局鼠标事件检测点击)
    const ftxui::Box& retryButtonBox() const {
        return retryButtonBox_;
    }

    /// 测试辅助: 当前激活的中断消息索引 (npos = 无)
    size_t activeInterruptMsg() const {
        return activeInterruptMsg_;
    }

    // ---- 中断 UI 状态 (client 线程注入 / 组件内部维护) ----

    /// 注册中断请求结果回传通道 (client 线程经 enqueueUiAction 调用;
    /// 同请求的所有输入项共享同一通道)
    /// - rememberable: 该请求是否为可记住选择的权限询问 (渲染"记住"开关)
    void attachInterruptChannel(
        int64_t                                 wireId,
        std::shared_ptr<InterruptResultChannel> ch,
        bool                                    rememberable = false
    );

    /// 释放指定中断请求的通道映射与该请求全部 UI 状态 (中断流程结束时调用;
    /// 消息已固定状态, 状态行渲染不再需要编辑状态)
    void releaseInterruptChannel(int64_t wireId);

    /// 清空全部中断 UI 状态与通道映射 (消息整体替换/重连时调用)
    void clearInterruptUiState();

    /// 测试辅助: 指定消息的中断 UI 状态副本 (非 Interrupt 消息或无状态时返回默认)
    InterruptUIState interruptUiState(size_t msgIndex) const;

private:

    /// 最近一次渲染时记录的中断控件命中区域 (UI 线程独占; 渲染时填充,
    /// 点击时命中检测; 与 collapsibleBoxes_ 生命周期一致)
    std::vector<InterruptHitBox> interruptHits_;

    /// 连接失败 banner 的"重试"按钮命中区域 (UI 线程独占; buildBanner 渲染时
    /// reflect 填充, TUIClientAgentIO 全局鼠标事件检测点击)
    ftxui::Box retryButtonBox_;
    /// 当前激活编辑的中断消息索引 (点击输入框/控件时设置, Esc 清除)
    size_t activeInterruptMsg_ = static_cast<size_t>(-1);

    // ---- 中断控件 Box (渲染时 reflect 填充; 由 interruptHits_ 持 shared_ptr 引用) ----
    std::vector<std::shared_ptr<ftxui::Box>> enumBoxes_;

    // ---- 中断消息交互 ----
    /// 点击命中中断控件 (是/否、±、枚举项、输入框、确认、取消); 命中返回 true
    bool handleInterruptClick(const ftxui::Mouse& mouse);
    /// 键盘事件作用于当前激活的中断消息 (字符/Backspace/方向键/Enter/Esc)
    bool handleInterruptKey(ftxui::Event event);
    /// 将指定消息设为激活编辑状态 (bool/enum 为选中, 数值/string 为输入框)
    void setInterruptActive(size_t mi);
    /// 确认指定中断消息 (校验失败写 tip, 不关闭); 成功发送结果到通道
    void confirmInterrupt(size_t mi);
    /// 取消指定中断消息所属的整个中断请求 (所有未操作项标记 Cancelled)
    void cancelInterrupt(size_t mi);
    /// 数值步进: 以 delta (int: 1 / double: 1.0) 增减编辑值
    void stepInterrupt(size_t mi, double delta);

    // ---- 中断 UI 状态表 (UI 线程独占; key = (interruptId, inputIndex)) ----
    /// 中断输入项 key (消息中 interruptId + inputIndex 唯一确定一个输入项)
    struct InterruptKey {
        int64_t id    = 0;
        int     index = 0;

        bool operator<(const InterruptKey& o) const {
            return id != o.id ? id < o.id : index < o.index;
        }
    };

    /// 获取/惰性创建指定消息的 UI 状态 (按消息 InterruptData 初始化:
    /// editText=默认值(数值无默认时 "0"/"0.0"), selected=默认匹配项,
    /// ch=attachInterruptChannel 注入的通道)
    InterruptUIState& uiStateFor(const TUIMessage& msg);
    /// 修改指定消息的 UI 状态 (version 递增, 使 itemKey 变化)
    InterruptUIState& mutateInterruptUiState(const TUIMessage& msg);
    /// 由消息推导 UI 状态 key (非 Interrupt 消息返回 false)
    static bool interruptKeyOf(const TUIMessage& msg, InterruptKey& out);

    std::map<InterruptKey, InterruptUIState> interruptUi_;

    /// 中断请求信息: wireId → 通道 + 权限询问标记 (client 线程注入; 同请求共享)
    struct InterruptChannelInfo {
        std::shared_ptr<InterruptResultChannel> ch;
        /// 是否为可记住选择的权限询问 (渲染"记住"开关)
        bool rememberable = false;
    };

    std::map<int64_t, InterruptChannelInfo> interruptChannels_;

    // ---- LazyScrollable 回调 ----
    size_t        itemCount();
    uint64_t      itemKey(size_t index);
    size_t        estimateHeight(size_t index, int width);
    LazyBuiltItem buildItem(size_t index);
    bool          fillViewport(size_t index);

    // ---- 子项构建辅助 ----
    LazyBuiltItem  buildMessageItem(const TUIMessage& msg, size_t index);
    LazyBuiltItem  buildStreamingItem(const TUIRenderState& st);
    ftxui::Element buildBanner();

    bool hasStreamingToken(const TUIRenderState& st) const;

    ftxui::Element buildMessageBlock(
        const TUIMessage&                                   msg,
        size_t                                              msgIndex,
        int                                                 maxWidth,
        std::vector<std::unique_ptr<markdown::DomBuilder>>& mdBuilders
    );

    /// 中断消息控件区 (仅 Waiting 状态; 渲染控件并把命中区域记入 interruptHits_)
    ftxui::Element buildInterruptControl(const TUIMessage& msg, size_t msgIndex);
    /// 中断消息状态行 (Confirmed/Cancelled/Expired)
    ftxui::Element buildInterruptStatusLine(const TUIMessage& msg);

    void           appendEditToolHeader(const TUIMessage& msg, ftxui::Elements& header);
    void           appendEditToolBody(const TUIMessage& msg, ftxui::Elements& lines);
    ftxui::Element renderEditToolDiff(std::string_view oldStr, std::string_view newStr);

    TUICtx&                         ctx_;
    std::shared_ptr<LazyScrollable> scrollable_;

    // ---- 流式增量 markdown 渲染器 ----
    // 流式输出期间避免每帧对整段累积文本全量重解析 (O(n^2) -> 稳定块缓存 O(n)):
    // 已闭合的顶层块作为独立可缓存子项 (LazyScrollable 仅布局可见子项, 避免
    // 每帧对整篇内容全量布局), 每帧仅重建末尾仍在增长的块。
    std::unique_ptr<markdown::IncrementalRenderer> streamRenderer_;
    /// 当前流式渲染模式: true=增量 (稳定块拆分为多个可缓存子项);
    /// false=降级 (动画等级不足, 整段 paragraph 单子项)
    bool streamUseIncremental_ = false;
    /// 流式期间 (增量模式) 头部项数: thinking 时 1 (显示 "[Thinking] 时长"), 其余 0
    size_t streamHeaderCount_ = 0;
    /// 已 feed 到渲染器的 token 字节数 (检测增量追加/新流)
    size_t streamFedLen_ = 0;
    /// 流身份缓存 (对应 TUIRenderState::currentTokenEpoch):
    /// 与 fedLen_ 联合判定 "同一流仅追加增量" (epoch 相同且长度增长)
    /// 或 "新流需重建渲染器" (epoch 变化), 替代逐帧前缀比较
    uint64_t streamEpoch_ = ~0ULL;
    /// 流式代次: 每重建一次渲染器递增, 用于流式子项 key 防跨流串用缓存
    uint64_t streamGen_ = 0;

    /// 增量模式的流式区子项数 = 头部 + 稳定块 + (尾部块存在 ? 1 : 0)
    size_t streamItemCount() const;
    /// 同步渲染器与当前 flow 状态 (每帧由 itemCount 调用一次):
    /// 检测新流/流结束/动画降级, 增量 feed 新 token, harvest 稳定块
    void syncStream(const TUIRenderState& st);
    /// 构建 thinking 头部项 (缓存, key 稳定)
    LazyBuiltItem buildStreamingHeader(const TUIRenderState& st);
    /// 构建第 bi 个稳定块项 (可缓存; 构建一次后由 LazyScrollable 缓存)
    LazyBuiltItem buildStreamingStable(const TUIRenderState& st, size_t bi);
    /// 构建尾部 (仍增长) 块项 (每帧重建)
    LazyBuiltItem buildStreamingFrontier(const TUIRenderState& st);

    // ---- 折叠消息命中检测 (由上一帧 visibleBoxes 反推) ----
    std::vector<ftxui::Box> collapsibleBoxes_;
    std::vector<size_t>     collapsibleIndices_;
    ftxui::Box              areaBox_;
};
