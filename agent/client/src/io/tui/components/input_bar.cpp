#include "agentxx-client/io/tui/components/input_bar.h"
#include "agentxx-client/io/tui/framework/tui_settings.h"
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
    option.placeholder     = "Type a message... (Enter:Send, Alt+Enter:Newline)";
    option.on_enter        = nullptr;
    option.transform       = [this](InputState state) {
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

    const int maxInputTotalLines = std::max(3, Terminal::Size().dimy / 2);
    return hbox({
        text(" "),
        vbox({
            text(" "),
            hbox({
                text("  "),
                indicator,
                text("  "),
                input_->Render() | color(theme.inputTextColor) | flex,
                text("  "),
            }),
            text(" "),
        }) | bgcolor(theme.inputBgColor)
            | xflex | size(HEIGHT, GREATER_THAN, 3) | size(HEIGHT, LESS_THAN, maxInputTotalLines),
        text(" "),
    });
}

bool InputComponent::OnEvent(Event event) {
    if (event == Event::CtrlL) {
        if (!inputText_.empty()) {
            inputText_.clear();
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
        if (!text.empty()) {
            if (config_.onSend) {
                config_.onSend(std::move(text));
            }
            inputText_.clear();
        }
        ctx_.postRedraw();
        return true;
    }

    return input_->OnEvent(event);
}
