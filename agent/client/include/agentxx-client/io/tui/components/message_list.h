#pragma once

#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx-client/io/tui/lazy_scrollable.h"
#include "ftxui/component/component_base.hpp"
#include "ftxui/dom/elements.hpp"
#include <markdown/dom_builder.hpp>
#include <memory>
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
class MessageListComponent : public ftxui::ComponentBase {
public:

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

private:

    // ---- LazyScrollable 回调 ----
    size_t        itemCount();
    uint64_t      itemKey(size_t index);
    int           estimateHeight(size_t index, int width);
    LazyBuiltItem buildItem(size_t index);
    bool          fillViewport(size_t index);

    // ---- 子项构建辅助 ----
    LazyBuiltItem buildMessageItem(const TUIMessage& msg);
    LazyBuiltItem buildStreamingItem(const TUIRenderState& st);
    ftxui::Element buildBanner();

    bool hasStreamingToken(const TUIRenderState& st) const;

    ftxui::Element buildMessageBlock(
                 const TUIMessage&                                   msg,
                 int                                                 maxWidth,
                 std::vector<std::unique_ptr<markdown::DomBuilder>>& mdBuilders
             );

    void           appendEditToolHeader(const TUIMessage& msg, ftxui::Elements& header);
    void           appendEditToolBody(const TUIMessage& msg, ftxui::Elements& lines);
    ftxui::Element renderEditToolDiff(std::string_view oldStr, std::string_view newStr);

    TUICtx&                          ctx_;
    std::shared_ptr<LazyScrollable>  scrollable_;

    // ---- 折叠消息命中检测 (由上一帧 visibleBoxes 反推) ----
    std::vector<ftxui::Box> collapsibleBoxes_;
    std::vector<size_t>     collapsibleIndices_;
    ftxui::Box              areaBox_;
};
