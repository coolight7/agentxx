#pragma once

#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx-client/io/tui/scrollable.h"
#include "ftxui/component/component_base.hpp"
#include "ftxui/dom/elements.hpp"
#include <markdown/dom_builder.hpp>
#include <memory>
#include <vector>

/// 消息列表组件: 封装 Scrollable + 消息缓存 + 折叠/展开交互
///
/// 渲染缓存 (统一替代原 messageCache_ + streamingMdBuilders_):
/// - 按消息签名 (64 位哈希) 判断是否需要重建
/// - 宽度变化时全部重建 (表格换行依赖宽度)
/// - 滑动窗口淘汰: 仅保留最近 kMaxCache 条的完整缓存
///
/// 事件处理:
/// - 滚轮: 由内部 Scrollable 处理
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

    struct MessageCache {
        ftxui::Element                                     element;
        int64_t                                            sig         = 0;
        int                                                cachedWidth = -1;
        std::vector<std::unique_ptr<markdown::DomBuilder>> mdBuilders;
    };

    struct ItemMeta {
        TUIMessage::Role role;
        bool             collapsible;
        int              messageIndex;
    };

    std::vector<ScrollItem> buildItems();
    ftxui::Element          buildMessageBlock(
                 const TUIMessage&                                   msg,
                 int                                                 maxWidth,
                 std::vector<std::unique_ptr<markdown::DomBuilder>>& mdBuilders
             );
    static int64_t messageSignature(const TUIMessage& msg);

    void           appendEditToolHeader(const TUIMessage& msg, ftxui::Elements& header);
    void           appendEditToolBody(const TUIMessage& msg, ftxui::Elements& lines);
    ftxui::Element renderEditToolDiff(std::string_view oldStr, std::string_view newStr);

    TUICtx&                     ctx_;
    std::shared_ptr<Scrollable> scrollable_;

    std::vector<MessageCache>                          cache_;
    size_t                                             prevMsgCount_ = 0;
    std::vector<std::unique_ptr<markdown::DomBuilder>> streamingMdBuilders_;
    std::vector<ItemMeta>                              itemMeta_;

    std::vector<ftxui::Box> collapsibleBoxes_;
    std::vector<size_t>     collapsibleIndices_;
    ftxui::Box              areaBox_;

    static constexpr size_t kMaxCache = 200;
};
