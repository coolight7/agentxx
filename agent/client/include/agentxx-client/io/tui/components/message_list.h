#pragma once

#include "agentxx-client/io/tui/components/interrupt_view.h"
#include "agentxx-client/io/tui/components/spinner.h"
#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx-client/io/tui/framework/ui_hit.h"
#include "agentxx-client/io/tui/lazy_scrollable.h"
#include "ftxui/component/component_base.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/box.hpp"
#include "utilxx_base/json.h"
#include <cstdint>
#include <functional>
#include <map>
#include <markdown/dom_builder.hpp>
#include <markdown/incremental.hpp>
#include <memory>
#include <set>
#include <string_view>
#include <vector>

// 前置声明: ClientToolDecor 仅以 const 引用出现在方法签名中,
// 不直接包含重量级 client_plugin_manager.h (客户端经 agent_tui.h 传递引入,
// 测试等仅含本头文件的编译单元依赖此前置声明)
namespace agentxx::plugin {
struct ClientToolDecor;
}

namespace agentxx::client {

/// 消息列表组件 (Flutter ListView.builder 风格)
///
/// 渲染架构: 封装 LazyScrollable, 经 itemCount/itemKey/quickHeight/buildItem
/// 四个回调描述列表, 仅按需懒构建子项:
/// - 有界 LRU 缓存 (条数 + 源字节双预算): 窗口外旧消息的渲染缓存被淘汰释放,
///   内存占用与对话长度解耦
/// - 视口局部布局/绘制: 仅对可见消息做 markdown 解析与布局, 不可见消息零成本
/// - 高度估算: 未进入视口的消息使用按文本量估算的高度, 进入视口后实测修正
/// - itemKey 以消息指针 + 廉价特征构成 (内容变化必然伴随消息指针变化,
///   见 TUISharedState::mutableMessage), 避免对全部消息文本逐帧哈希
/// - 流式增量项 (currentToken) 标记为不可缓存, 每帧重建后即释放
///
/// 事件处理:
/// - 滚轮: 由内部 LazyScrollable 处理
/// - 左键点击 Think/Tool 消息: 折叠/展开
/// - 左键点击中断输入消息的控件: 由 InterruptView 按中断 UI 描述通用处理
///   (值按钮/枚举/数值步进/输入框/勾选项/确认/取消); 键盘 (字符/Backspace/
///   方向键/Enter/Esc) 作用于最近点击激活的中断消息
class MessageListComponent : public ftxui::ComponentBase {
public:

    /// 中断控件命中区域 (转发自 InterruptView; 见其 HitBox 说明)
    using InterruptHitBox = agentxx::client::InterruptView::HitBox;

    /// 中断输入项表单状态 (转发自 InterruptView::FormState)
    using InterruptUIState = agentxx::client::InterruptView::FormState;

    /// decor 按钮命中检测 (UI 线程独占; 与中断控件命中区域同生命期):
    /// - OnRender 开头清空, 本帧 scrollable_->Render() 中构建可见 Tool 消息的
    ///   decor 按钮时填充 (buildMessageBlock → appendDecorItems)
    /// - box 经 shared_ptr 持有 (reflect 在布局 SetBox 时写回, 与中断控件同机制;
    ///   构建阶段记录值为空 Box, 点击读最新布局位置)
    /// - 点击命中后拷贝 (plugin/ownerId/actionId/argsJson) 经 dispatchAction 投递
    struct DecorHitBox {
        std::string plugin;
        std::string ownerId;
        /// 整行只有一个可点区域时的动作 id (多区域时按 regions 定位)
        std::string actionId;
        /// 整行只有一个可点区域时的参数 JSON
        std::string argsJson;
        /// 渲染时快照中的实例代次 (点击派发时复查; 重载同名插件后旧点击被丢弃)
        uint64_t                    generation = 0;
        std::shared_ptr<ftxui::Box> box;
        /// 行内可命中区域 (局部坐标; 命中后按坐标定位具体区域, 如表格单元格)
        std::vector<UiHitRegion>    regions;
    };

    /// 多模态附件卡片命中检测 (UI 线程独占; 与 decorHits_ 同生命期):
    /// - OnRender 开头清空, 构建可见 User 消息附件卡片时填充
    /// - 点击命中后在文件管理器中定位显示对应文件 远端 dataUrl 先落盘到临时目录
    struct AttachmentHitBox {
        size_t                      msgIndex = static_cast<size_t>(-1);
        size_t                      attIndex = 0;
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

    /// 历史分页前插通知 (client 线程经 UI 动作队列调用, 帧间执行):
    /// - 转发给 LazyScrollable::notifyPrepended 做滚动锚定 (并行数组头插 +
    ///   按新增区估算行数下移偏移), 保证前插后视口内容稳定不跳动
    /// - anchor 为 false 时仅同步条数语义 (首屏填充场景无需锚定)
    void onHistoryPrepended(size_t count) {
        if (count > 0) {
            scrollable_->notifyPrepended(count);
        }
    }

    /// 重置历史分页锚定状态 (消息列表整体替换/会话切换时调用:
    /// 窗口已重建, 对旧窗口的偏移校正不再有意义)
    void resetHistoryPagination() {
        scrollable_->clearPrependAnchor();
    }

    int contentWidth() const {
        return scrollable_->contentWidth();
    }

    /// 测试辅助: 内容总高度 (行; 未测量子项按估算高度) —— 用于断言
    /// 估算算法合理性 (估算严重高估/低估会反映在总高度与滚动定位上)
    int totalHeight() const {
        return scrollable_->totalHeight();
    }

    /// 测试辅助: 当前滚动偏移 (行)
    int scrollOffset() const {
        return scrollable_->scrollOffset();
    }

    /// 主题变化后清空缓存 (颜色已过时)
    void invalidateCache();

    /// 清除消息列表可见项的鼠标选中高亮 (拖选松开复制完成后调用;
    /// 转发给 scrollable_ 的 resetSelectionHighlight)
    void clearSelectionHighlight() {
        scrollable_->resetSelectionHighlight();
    }

    /// 处理可折叠消息的鼠标点击 (供外部 CatchEvent 调用); 返回是否处理了事件
    /// - 普通消息 (Think/Tool/System/Tip): 切换该消息的 collapsed
    /// - 流式末尾正在输出的 Think 子项: 切换流式折叠覆盖态
    ///   (TUIRenderState::streamThinkOverride, 流提交落盘时据此保持用户选择)
    bool handleCollapsibleClick(const ftxui::Mouse& mouse);

    /// 测试辅助: 中断消息块的高度估算 (行数; 不含消息尾部空行)
    size_t interruptEstimate(size_t msgIndex, int width) const;

    /// 测试辅助: 最近一次渲染的中断控件命中区域 (转发自 InterruptView)
    const std::vector<InterruptHitBox>& interruptHitBoxes() const {
        return interruptView_.hitBoxes();
    }

    /// 测试辅助: 上一帧可折叠消息 (Think/Tool/System) 的命中区域
    /// (与 collapsibleIndices_ 对应; 供测试模拟点击折叠/展开)
    const std::vector<ftxui::Box>& collapsibleBoxes() const {
        return collapsibleBoxes_;
    }

    /// 测试辅助: 指定 collapsibleBoxes 下标是否为流式末尾 Think 命中区
    /// (流式子项不属于 st.messages, 点击切换流式折叠覆盖态而非消息 collapsed)
    bool collapsibleIsStream(size_t k) const {
        return k < collapsibleIsStream_.size() && collapsibleIsStream_[k] != 0;
    }

    /// 装配 [重试] 按钮点击回调 (连接失败 banner; 点击重新发起连接)
    void setOnRetryClick(std::function<void()> fn) {
        onRetryClick_ = std::move(fn);
    }

    /// 测试辅助: 上一帧 [重试] 按钮的命中区域 (未渲染时为空区域)
    ftxui::Box retryButtonBox() const;

    /// 处理 decor 按钮点击 (供 MessageListComponent::OnEvent 与外部调用):
    /// 命中 decorHits_ 后经 pluginManager->dispatchAction 投递 io 线程派发
    bool handleDecorButtonClick(const ftxui::Mouse& mouse);

    /// 处理连接失败 banner 的 [重试] 按钮点击 (命中 bannerHits_ 时触发 onRetryClick_)
    bool handleRetryClick(const ftxui::Mouse& mouse);

    /// 处理多模态附件卡片点击: 在文件管理器中定位显示对应文件
    bool handleAttachmentClick(const ftxui::Mouse& mouse);

    /// 测试辅助: 最近一次渲染的 decor 按钮命中区域
    const std::vector<DecorHitBox>& decorHitBoxes() const {
        return decorHits_;
    }

    /// 测试辅助: 最近一次渲染的附件卡片命中区域
    const std::vector<AttachmentHitBox>& attachmentHitBoxes() const {
        return attachmentHits_;
    }

    /// 测试辅助: 当前激活的中断消息索引 (npos = 无)
    size_t activeInterruptMsg() const {
        return interruptView_.activeMsg();
    }

    // ---- 中断 UI 状态 (client 线程注入 / 组件内部维护) ----

    /// 注册中断请求结果回传通道 (client 线程经 enqueueUiAction 调用;
    /// 同请求的所有输入项共享同一通道)
    void attachInterruptChannel(int64_t wireId, std::shared_ptr<InterruptResultChannel> ch);

    /// 释放指定中断请求的通道映射与该请求全部 UI 状态 (中断流程结束时调用;
    /// 消息已固定状态, 状态行渲染不再需要编辑状态)
    void releaseInterruptChannel(int64_t wireId);

    /// 清空全部中断 UI 状态与通道映射 (消息整体替换/重连时调用)
    void clearInterruptUiState();

    /// 测试辅助: 指定消息的中断表单状态副本 (必要时按描述惰性初始化;
    /// 非 Interrupt 消息返回默认值)
    InterruptUIState interruptUiState(size_t msgIndex) {
        return interruptView_.formState(msgIndex);
    }

private:

    std::vector<DecorHitBox> decorHits_;

    std::vector<AttachmentHitBox> attachmentHits_;

    /// 连接失败 banner 的 [重试] 按钮命中登记 (UI 线程独占)
    /// - banner 元素可能跨帧缓存 (itemKey 未变时不重建), 因此命中项也跨帧保留:
    ///   其 Box 由同一 Element 内的 reflect 每帧更新, 仍指向屏幕上的实际位置
    /// - 清空时机 (见 OnRender/buildBanner): banner 不在本帧列表中 (已有消息) 时,
    ///   或 banner 重建时 —— 因此 [重试] 按钮消失后不再占用那块区域
    agentxx::client::UiHitMap bannerHits_;

    /// [重试] 按钮点击回调 (连接失败后重新发起连接; 由 TUIClientAgentIO 装配)
    std::function<void()> onRetryClick_;

    /// 中断输入项通用视图 (渲染/估算/交互/结果组装; 形态由中断 UI 描述数据决定,
    /// 本组件不再含任何具体询问 (含权限) 的特化分支)
    agentxx::client::InterruptView interruptView_;

    // ---- LazyScrollable 回调 ----
    size_t        itemCount();
    uint64_t      itemKey(size_t index);
    /// 未进入视口条目的**粗略**高度估算 (行; O(1) 或轻量线性扫描, 不做渲染):
    /// 只服务总高度/滚动条长度, 进入视口后由布局实测修正。
    /// 不查插件语义渲染 (queryToolRender)、不 measureItems、不做中断表单 layoutForm
    size_t        quickHeight(size_t index, int width);
    LazyBuiltItem buildItem(size_t index);
    bool          fillViewport(size_t index);

    /// 流式末尾正在输出的 Think 当前生效的折叠状态 (UI 线程独占):
    /// 用户点击覆盖态 (TUIRenderState::streamThinkOverride) 优先, 未点击时按
    /// TailThinkingMode 设置。
    /// 供 syncStream/itemKey/quickHeight/buildStreamingItem 统一判定渲染形态
    /// (折叠=单行预览子项, 展开=多行/增量子项)
    ///
    /// - `args`:
    ///     - [st] 本帧状态快照 (覆盖态随帧内一致, 点击写入后下一帧生效)
    bool streamThinkCollapsed(const TUIRenderState& st) const;
    /// 切换流式末尾 Think 折叠状态 (点击命中流式区时调用): 把切换结果写入
    /// 共享状态 (TUIRenderState::streamThinkOverride), 供 client 线程在流提交
    /// 落盘时读取 —— 用户手动展开过的思考流结束后保持展开
    void toggleStreamThinkCollapsed();
    /// 清除流式末尾 Think 的用户点击覆盖态 (流结束/无流式内容时调用;
    /// 无覆盖态时不写状态)
    void clearStreamThinkOverride();

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

    /// 当前帧是否存在 "正在运行" 的条目 (runSpinner_ 的 isActive 判定):
    /// - 流式输出中且角色为 Think (流式区 [Think] 头部)
    /// - 存在未完成的 Tool 消息 (!toolFinished)
    /// 仅在 UI 线程调用 (渲染/OnAnimation), 直接读取本帧快照
    bool hasRunningToolOrThink() const;

    /// 运行中条目头部的折叠标记字符 (原 "+/-" 静态标识的位置):
    /// - 动画等级 >= High: 返回 runSpinner_ 当前动画帧 (braille 点阵, 同输入框
    ///   前缀加载动画; 由调用方按消息角色着色并补后续文本)
    /// - 否则: 返回静态 +/- 字符 (expanded ? '-' : '+')
    /// 注意: 返回的 Element 需每帧重建 (见 buildMessageItem/buildStreamingHeader
    /// 的 cacheable 处理), 缓存的旧帧快照不会随动画推进更新
    ftxui::Element runningHeaderMark(bool expanded) const;

    /// 历史分页预取判定 (滚轮事件处理后调用): 滚动接近已加载窗口顶部且
    /// 还有更早历史时经 ctx_.requestMoreHistory 发起分页请求。
    /// - 请求去重由实现方 (TUIClientAgentIO::requestOlderHistory) 保证,
    ///   此处仅做廉价条件过滤
    void maybeRequestMoreHistory();
    /// 触发预取的距顶阈值 (行): 距窗口顶部不足该行数即提前拉取下一页,
    /// 用户连续上滑时页面在到达顶部前已就位 (标准聊天应用体验)
    static constexpr int kHistoryPrefetchRows = 8;

    void           appendEditToolBody(const TUIMessage& msg, ftxui::Elements& lines);
    ftxui::Element renderEditToolDiff(std::string_view oldStr, std::string_view newStr);
    /// 插件装饰工具体通用渲染 (items: text/button/diagram/separator/diff; 内容由插件定义)
    /// - button 走通用 action_id 派发: owner=tool_call_id (decor 按钮以 toolCallId
    ///   作 owner_id, 插件 bind 一次永久生效, 见方案 A fallback)
    /// - decor 按钮命中挂载到 decorHits_ (与 InterruptView 的控件命中区域/
    ///   collapsibleBoxes_ 同生命期:
    ///   OnRender 清空 + 构建期填充, OnEvent/命中检测读取); 全局 CatchEvent 侧经
    ///   pluginDecorHits() 读取 (MessageListComponent 有独立事件流, 不能只靠全局)
    /// - text/button 解析与配色走 plugin_ui_items 共享 helper; diff 走 renderPluginDiff
    void appendDecorItems(
        const utilxx_base::Json&                            items,
        const std::string&                                  plugin,
        const std::string&                                  ownerId,
        ftxui::Elements&                                    lines,
        int                                                 maxWidth,
        std::vector<std::unique_ptr<markdown::DomBuilder>>& mdBuilders
    );
    void appendDecorToolBody(
        const agentxx::plugin::ClientToolDecor&             decor,
        ftxui::Elements&                                    lines,
        int                                                 maxWidth,
        std::vector<std::unique_ptr<markdown::DomBuilder>>& mdBuilders
    );

    TUICtx&                         ctx_;
    std::shared_ptr<LazyScrollable> scrollable_;

    /// 运行中 tool/think 头部加载动画 (动画等级 >= High 时替代 "+/-" 静态标识):
    /// 复用输入框前缀的 SpinnerComponent (braille 旋转点阵), requiredLevel=High;
    /// 必须经 Add() 注册为本组件子项 —— FTXUI 的 OnAnimation 由根组件沿组件树
    /// 转发给已注册的子组件, 未入树的组件收不到动画回调, 帧循环无法推进。
    /// decorate 留空: 渲染处按消息角色 (tool/thinking 颜色) 在外部着色
    std::shared_ptr<SpinnerComponent> runSpinner_;

    /// 初始化加载提示的加载动画 (Connecting 状态 startupProgress 行首):
    /// - requiredLevel=High: 动画等级不足时降级为静态 "~" (与需求一致)
    /// - isActive 绑定 Connecting 且 startupProgress 非空 (仅启动阶段运行)
    /// - 注册为子项以接收 OnAnimation, 与 runSpinner_ 同款 braille 点阵
    std::shared_ptr<SpinnerComponent> startupSpinner_;

    // ---- 流式增量 markdown 渲染器 ----
    // 流式输出期间避免每帧对整段累积文本全量重解析 (O(n^2) -> 稳定块缓存 O(n)):
    // 已闭合的顶层块作为独立可缓存子项 (LazyScrollable 仅布局可见子项, 避免
    // 每帧对整篇内容全量布局), 每帧仅重建末尾仍在增长的块。
    std::unique_ptr<markdown::IncrementalRenderer> streamRenderer_;
    /// 当前流式渲染模式: true=增量 (稳定块拆分为多个可缓存子项);
    /// false=降级 (动画等级不足, 整段 paragraph 单子项)
    bool streamUseIncremental_ = false;
    /// 流式期间 (增量模式) 头部项数: thinking 时 1 (显示 "[Think] 时长"), 其余 0
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

    /// 上报视角内"工具消息装饰区域"的可见性 (UI 线程; 每帧一次)
    ///
    /// 插件用 `tool_call_id` 作区域 id 注册 `pause_when_hidden` 定时器
    /// (装饰的按钮/控件归因同样是 tool_call_id) 时, 该区域是否可见由本函数上报;
    /// 只有**曾经登记过装饰**的 id 才参与上报 (否则宿主的可见性表会随会话长度
    /// 无界增长, 且绝大多数 tool_call_id 无人关心)。
    /// - `args`:
    ///     - [vboxes] 上一帧各子项可见区域 (LazyScrollable::visibleBoxes)
    void reportDecorVisibility(const std::vector<ftxui::Box>& vboxes);

    /// 上一帧上报为"可见"的装饰区域 id (值未变化时不做任何上报, 见上)
    std::set<std::string> decorVisibleOwners_;

    // ---- 折叠消息命中检测 (由上一帧 visibleBoxes 反推) ----
    std::vector<ftxui::Box> collapsibleBoxes_;
    std::vector<size_t>     collapsibleIndices_;
    /// 与 collapsibleBoxes_/collapsibleIndices_ 一一对应: 该命中区是否属于
    /// 流式末尾 Think 子项 (流式区子项索引 >= st.messages.size(), 点击切换
    /// TUIRenderState::streamThinkOverride; 普通消息点击切换 msg.collapsed)。
    /// 用 char 而非 bool (避免 vector<bool> 代理引用语义)
    std::vector<char> collapsibleIsStream_;
    ftxui::Box        areaBox_;
};

} // namespace agentxx::client
