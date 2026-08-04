#include "agentxx-client/io/tui/components/input_bar.h"
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
    option.transform       = [](InputState state) {
        if (state.is_placeholder) {
            state.element |= dim;
        }
        return state.element;
    };
    input_ = Input(&inputText_, option);
    Add(input_);
}

Element InputComponent::OnRender() {
    const auto& theme = *ctx_.theme;

    Element indicator;
    if (config_.isAwaitingInterrupt && config_.isAwaitingInterrupt()) {
        indicator = text("!") | bgcolor(theme.errorColor) | color(Color::White) | bold | blink;
    } else if (config_.isStreaming && config_.isStreaming()) {
        indicator = text("~") | color(theme.accentColor) | bold;
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
    if (event == Event::CtrlC) {
        if (!inputText_.empty()) {
            inputText_.clear();
            ctx_.postRedraw();
            return true;
        }
        if (config_.onCtrlC && config_.onCtrlC()) {
            return true;
        }
        return false;
    }

    std::string_view in = event.input();
    if (in == "\x1B\n" || in == "\x1B\r") {
        inputText_ += '\n';
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
