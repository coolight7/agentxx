#include "agentxx-client/io/tui/components/input_bar.h"
#include "agentxx-client/io/tui/framework/tui_i18n.h"
#include "agentxx-client/io/tui/framework/tui_settings.h"
#include "fmt/format.h"
#include "ftxui/component/event.hpp"
#include "ftxui/screen/terminal.hpp"

using namespace ftxui;

InputComponent::InputComponent(TUICtx& ctx, Config config) :
    ctx_(ctx),
    config_(std::move(config)) {
    auto option            = InputOption();
    option.multiline       = true;
    option.insert          = true;
    option.cursor_position = 0;
    // 占位符绑定到成员字符串的引用: FTXUI Input 渲染时实时读取该引用,
    // 语言切换后仅需刷新成员 (见 refreshLanguage), 无需重建组件
    placeholderText_   = std::string(TuiI18n::instance().t("input.placeholder"));
    option.placeholder = StringRef(&placeholderText_);
    option.on_enter    = nullptr;
    option.transform   = [this](InputState state) {
        if (state.is_placeholder) {
            const auto& theme  = *ctx_.theme;
            state.element     |= color(theme.hintColor);
        }
        return state.element;
    };
    input_ = Input(&inputText_, option);
    Add(input_);

    // 会话运行加载动画 (braille 旋转点阵): 运行状态跟随 isStreaming,
    // 颜色/加粗与原静态 "~" 标记一致; 动画等级不足时组件内部降级为静态帧
    SpinnerComponent::Config spinnerCfg;
    // 动画门槛: >= High 才启用旋转动画 (与消息列表运行中 tool/think 头部
    // 加载动画同等级), 低于 High 时组件内部降级为静态首帧
    spinnerCfg.requiredLevel = AnimationLevel::High;
    spinnerCfg.isActive      = [this] {
        return config_.isStreaming && config_.isStreaming();
    };
    spinnerCfg.decorate = [this](Element element) {
        return element | color(ctx_.theme->accentColor) | bold;
    };
    spinner_ = std::make_shared<SpinnerComponent>(std::move(spinnerCfg));
    // 注册为子项: OnAnimation 经组件树转发至此, 帧循环才能持续推进
    Add(spinner_);
}

Element InputComponent::OnRender() {
    const auto& theme = *ctx_.theme;

    Element indicator;
    if (config_.isAwaitingInterrupt && config_.isAwaitingInterrupt()) {
        // 闪烁为 Low 级动画, 动画等级低于 Low (如 Disabled) 时仅静态高亮
        indicator = text("!") | bgcolor(theme.errorColor) | color(Color::White) | bold;
    } else if (config_.isStreaming && config_.isStreaming() && spinner_->animationEnabled()) {
        // 流式输出: 循环加载动画 (SpinnerComponent, braille 旋转点阵);
        // 动画等级不足时组件内部自动降级为静态帧
        indicator = spinner_->Render();
    } else {
        indicator = text(">") | color(theme.accentColor) | bold;
    }

    // 多模态文件选择按钮 [+ 📎︎︎ 附件] (仅当当前模型支持多模态输入时展示)
    Element attachButton = text("");
    if (config_.canAttach && config_.canAttach()) {
        attachButton = hbox({
            text(" "),
            text(std::string(TuiI18n::instance().t("input.attach"))) | color(theme.accentColor)
                | bold | reflect(attachButtonBox_),
        });
    } else {
        attachButtonBox_ = Box{};
    }

    // 待发附件挂载托盘 (Attachment Tray)
    Element trayElement = text("");
    attachmentDeleteBoxes_.assign(attachments_.size(), Box{});
    if (!attachments_.empty()) {
        Elements trayItems;
        trayItems.push_back(
            text(trf("input.attachTray", attachments_.size())) | bold | color(theme.accentColor)
        );
        for (size_t i = 0; i < attachments_.size(); ++i) {
            const auto& att     = attachments_[i];
            auto        icon    = agentxx::agent::MediaAttachment::mediaTypeIcon(att.type);
            auto        sizeStr = agentxx::util::formatSize(att.sizeBytes);
            auto delBtn = text(" ✕ ") | bgcolor(theme.buttonBgColor) | color(theme.systemColor)
                          | bold | reflect(attachmentDeleteBoxes_[i]);
            auto pill = hbox({
                            text(fmt::format(" [{} {} {} ", icon, att.displayName, sizeStr)),
                            delBtn,
                            text("] "),
                        })
                        | bgcolor(theme.buttonActiveBgColor) | color(theme.buttonActiveTextColor);
            trayItems.push_back(pill);
        }
        trayElement = hbox(std::move(trayItems)) | bgcolor(theme.inputBgColor) | xflex;
    }

    const int maxInputTotalLines = std::max(3, Terminal::Size().dimy / 2);

    Elements vboxChildren;
    if (!attachments_.empty()) {
        vboxChildren.push_back(text(" "));
        vboxChildren.push_back(trayElement);
        vboxChildren.push_back(separator() | color(theme.hintColor));
    }
    vboxChildren.push_back(text(" "));
    vboxChildren.push_back(hbox({
        text("  "),
        indicator,
        text("  "),
        input_->Render() | color(theme.inputTextColor) | flex,
        attachButton,
        text("  "),
    }));
    vboxChildren.push_back(text(" "));

    return hbox({
        text(" "),
        vbox(std::move(vboxChildren)) | bgcolor(theme.inputBgColor) | xflex
            | size(HEIGHT, GREATER_THAN, 3) | size(HEIGHT, LESS_THAN, maxInputTotalLines),
        text(" "),
    });
}

bool InputComponent::OnEvent(Event event) {
    if (event.is_mouse()) {
        const auto& mouse = event.mouse();
        if (mouse.button == Mouse::Left && mouse.motion == Mouse::Released) {
            // 点击 [+ 📎︎︎ 附件] 按钮
            if (attachButtonBox_.Contain(mouse.x, mouse.y)) {
                if (config_.onOpenAttachPicker) {
                    config_.onOpenAttachPicker();
                }
                return true;
            }
            // 点击附件删除按钮 ✕
            for (size_t i = 0; i < attachmentDeleteBoxes_.size(); ++i) {
                if (attachmentDeleteBoxes_[i].Contain(mouse.x, mouse.y)) {
                    if (i < attachments_.size()) {
                        attachments_.erase(attachments_.begin() + i);
                        ctx_.postRedraw();
                        return true;
                    }
                }
            }
        }
    }

    if (event == Event::CtrlL) {
        if (!inputText_.empty() || !attachments_.empty()) {
            inputText_.clear();
            attachments_.clear();
            // 同步重置粘贴状态, 避免残留粘贴缓冲区在结束时被插入
            inPaste_ = false;
            pasteBuffer_.clear();
            ctx_.postRedraw();
            return true;
        }
        return false;
    }

    // ------------------------------------------------------------------
    // 括号粘贴 (bracketed paste) 拦截
    // ------------------------------------------------------------------
    // 终端启用 \x1B[?2004h 后, 粘贴内容以 \x1B[200~ ... \x1B[201~ 包裹到达。
    // 粘贴内容中的换行 (\r/\n) 会被解析为 Event::Return, 与真实回车无法区分,
    // 若不拦截, 多行粘贴会在首个换行处触发发送。此处将粘贴内容累积到缓冲区,
    // 在结束标记处一次性插入光标位置 (光标随后移动到粘贴内容末尾)。
    if (inPaste_) {
        const auto now = std::chrono::steady_clock::now();
        // 安全阀: 粘贴中断 (结束标记丢失) 超时后退出粘贴模式,
        // 当前事件按正常流程继续处理
        if (now - lastPasteEventTime_ > kPasteTimeout) {
            inPaste_ = false;
            pasteBuffer_.clear();
        } else if (event.input() == kPasteEndSeq) {
            // 粘贴结束: 整体插入
            inPaste_ = false;
            if (!pasteBuffer_.empty()) {
                input_->OnEvent(Event::Character(pasteBuffer_));
                ctx_.postRedraw();
            }
            pasteBuffer_.clear();
            return true;
        } else {
            // 累积粘贴内容, 原样保留每个换行。
            // 注意: 不做 CRLF 去重 —— FTXUI 解析层已把 \r 归一化为 \n,
            // 事件层面无法区分 "CRLF 的 \n" 与 "粘贴内容中真实的空行",
            // 去重会吞掉空行 (如代码块中的空行), 代价大于个别 Windows
            // 终端 (CRLF 粘贴) 多出的空行。
            pasteBuffer_        += event.input();
            lastPasteEventTime_  = now;
            return true;
        }
    }

    // 粘贴开始标记
    if (event.input() == kPasteStartSeq) {
        inPaste_ = true;
        pasteBuffer_.clear();
        lastPasteEventTime_ = std::chrono::steady_clock::now();
        return true;
    }

    std::string_view in = event.input();
    if (in == "\x1B\n" || in == "\x1B\r") {
        // Alt+Enter: 经 Input 组件自身的插入逻辑写入换行 (在光标处插入并后移光标)。
        // 直接 inputText_ += '\n' 不会同步 Input 内部的 cursor_position,
        // 导致光标停留在换行之前、后续输入被插到换行之前。
        input_->OnEvent(Event::Character("\n"));
        ctx_.postRedraw();
        return true;
    }

    if (event == Event::Return) {
        std::string text = inputText_;
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
        size_t start = 0;
        while (start < text.size() && (text[start] == '\n' || text[start] == '\r')) {
            ++start;
        }
        if (start > 0) {
            text = text.substr(start);
        }
        if (!text.empty() || !attachments_.empty()) {
            bool handled = false;
            if (config_.onSend) {
                handled = config_.onSend(std::move(text), attachments_);
            }
            if (handled) {
                inputText_.clear();
                attachments_.clear();
            }
        }
        ctx_.postRedraw();
        return true;
    }

    return input_->OnEvent(event);
}
