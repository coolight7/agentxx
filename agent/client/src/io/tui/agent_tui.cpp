#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx/agent/model_registry.h"
#include "fmt/format.h"
#include "ftxui/component/event.hpp"
#include "neograph/graph/cancel.h"

using namespace ftxui;

AgentTUI::AgentTUI(asio::any_io_executor ex,
                   std::shared_ptr<agentxx::agent::AgentContext> agentContext,
                   TUITheme theme)
    : agentContext_(std::move(agentContext)), theme_(theme),
      inputChannel_(std::make_shared<LineChannel>(ex, 64)),
      permissionChannel_(std::make_shared<BoolChannel>(ex, 4)) {}

AgentTUI::~AgentTUI() { stop(); }

void AgentTUI::postRedraw() {
  if (screen_) {
    screen_->PostEvent(Event::Custom);
  }
}

void AgentTUI::start() {
  running_ = true;
  uiThread_ = std::thread([this]() {
    auto screen = ScreenInteractive::Fullscreen();
    screen_ = &screen;

    auto input_option = InputOption();
    input_option.multiline = false;
    input_option.on_enter = [&]() {
      std::string text;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        text = inputText_;
        inputText_.clear();
      }
      if (!text.empty()) {
        {
          std::lock_guard<std::mutex> lock(mutex_);
          messages_.push_back({Message::Role::User, text});
          isStreaming_ = true;
        }
        inputChannel_->async_send(neograph_asio_error_code{}, std::move(text),
                                  [](neograph_asio_error_code) {});
      }
      postRedraw();
    };

    auto input = Input(&inputText_, "Type a message... (Enter to send)",
                       input_option);

    auto layout = Renderer(input, [&]() -> Element {
      std::lock_guard<std::mutex> lock(mutex_);

      auto messages = renderMessages() | flex | vscroll_indicator | frame |
                      yframe;

      auto input_bar = hbox({
          text(">>> ") | color(theme_.promptColor) | bold,
          input->Render() | flex,
      });

      auto main = vbox({
          messages,
          separator(),
          input_bar,
          renderStatusBar(),
      });

      if (pendingPermission_.has_value()) {
        return renderPermissionOverlay() | center;
      }
      if (showModelSelector_) {
        return renderModelSelectorOverlay() | center;
      }
      return main;
    });

    auto event_handler = CatchEvent(layout, [&](Event event) -> bool {
      if (event == Event::CtrlC) {
        running_ = false;
        screen.Exit();
        return true;
      }

      std::lock_guard<std::mutex> lock(mutex_);

      if (pendingPermission_.has_value()) {
        if (event == Event::Character('y') || event == Event::Character('Y')) {
          pendingPermission_.reset();
          permissionChannel_->async_send(neograph_asio_error_code{}, true,
                                         [](neograph_asio_error_code) {});
          postRedraw();
          return true;
        }
        if (event == Event::Character('n') || event == Event::Character('N') ||
            event == Event::Escape) {
          pendingPermission_.reset();
          permissionChannel_->async_send(neograph_asio_error_code{}, false,
                                         [](neograph_asio_error_code) {});
          postRedraw();
          return true;
        }
        return true;
      }

      if (showModelSelector_) {
        if (event == Event::ArrowUp) {
          if (selectedModelIndex_ > 0) {
            --selectedModelIndex_;
          }
          postRedraw();
          return true;
        }
        if (event == Event::ArrowDown) {
          if (selectedModelIndex_ + 1 < static_cast<int>(modelNames_.size())) {
            ++selectedModelIndex_;
          }
          postRedraw();
          return true;
        }
        if (event == Event::Return) {
          confirmModelSelection();
          postRedraw();
          return true;
        }
        if (event == Event::Escape) {
          showModelSelector_ = false;
          postRedraw();
          return true;
        }
        return true;
      }

      if (event == Event::F2) {
        openModelSelector();
        postRedraw();
        return true;
      }
      if (event == Event::Escape && isStreaming_) {
        cancelCurrentRun();
        postRedraw();
        return true;
      }
      return false;
    });

    screen.Loop(event_handler);
    screen_ = nullptr;
  });
}

void AgentTUI::stop() {
  running_ = false;
  if (screen_) {
    screen_->Exit();
  }
  if (uiThread_.joinable()) {
    uiThread_.join();
  }
}

ftxui::Element AgentTUI::renderMessages() {
  Elements elements;
  for (const auto &msg : messages_) {
    switch (msg.role) {
    case Message::Role::User:
      elements.push_back(hbox({
          text("> ") | color(theme_.userColor) | bold,
          paragraph(msg.text) | color(theme_.userColor),
      }));
      break;
    case Message::Role::Assistant:
      elements.push_back(paragraph(msg.text) | color(theme_.assistantColor));
      break;
    case Message::Role::Thinking:
      elements.push_back(hbox({
          text("[Thinking] ") | color(theme_.thinkingColor) | dim,
          paragraph(msg.text) | color(theme_.thinkingColor) | dim,
      }));
      break;
    case Message::Role::System:
      elements.push_back(paragraph(msg.text) | color(theme_.systemColor));
      break;
    }
    elements.push_back(text(""));
  }

  if (isStreaming_ && !currentToken_.empty()) {
    if (currentTokenRole_ == Message::Role::Thinking) {
      elements.push_back(hbox({
          text("[Thinking] ") | color(theme_.thinkingColor) | dim,
          paragraph(currentToken_) | color(theme_.thinkingColor) | dim,
      }));
    } else {
      elements.push_back(paragraph(currentToken_) |
                         color(theme_.assistantColor));
    }
  }

  if (elements.empty()) {
    return vbox({
        filler(),
        text("Agentxx TUI") | bold | color(theme_.accentColor) | center,
        text("Type a message to start. [F2] switch model, [Esc] cancel, "
             "[Ctrl+C] quit.") |
            dim | center,
        filler(),
    });
  }
  return vbox(std::move(elements));
}

ftxui::Element AgentTUI::renderStatusBar() {
  std::string modelName = "<none>";
  if (agentContext_ && agentContext_->modelRegistry) {
    auto name = agentContext_->modelRegistry->getCurrentModelName();
    if (false == name.empty()) {
      modelName = name;
    }
  }
  auto modelInfo = hbox({
      text(" model: ") | color(theme_.hintColor),
      text(modelName) | color(theme_.accentColor) | bold,
      text(" [F2] ") | color(theme_.hintColor),
  });

  size_t ctx = 0;
  size_t maxCtx = 0;
  if (agentContext_ && agentContext_->contextStats) {
    ctx = agentContext_->contextStats->contextTokens.load();
    maxCtx = agentContext_->contextStats->maxContextTokens.load();
  }
  std::string ctxText;
  if (maxCtx > 0) {
    const double pct = 100.0 * static_cast<double>(ctx) /
                       static_cast<double>(maxCtx);
    ctxText = fmt::format(" ctx: {}/{} ({:.1f}%) ", ctx, maxCtx, pct);
  } else {
    ctxText = fmt::format(" ctx: {} ", ctx);
  }
  auto ctxInfo = text(ctxText) | color(theme_.statusColor);

  return hbox({
      modelInfo,
      filler(),
      ctxInfo,
  });
}

ftxui::Element AgentTUI::renderPermissionOverlay() {
  const auto &req = pendingPermission_.value();
  return vbox({
             text(" Permission Request ") | bold | inverted,
             separator(),
             hbox({text(" Tool    : ") | bold, text(req.toolName)}),
             hbox({text(" Category: ") | bold, text(req.category)}),
             hbox({text(" Target  : ") | bold, text(req.target)}),
             separator(),
             text(" [y] Allow  [n/Esc] Deny ") | center,
         }) |
         border | size(WIDTH, LESS_THAN, 60) | color(theme_.systemColor);
}

ftxui::Element AgentTUI::renderModelSelectorOverlay() {
  Elements items;
  items.push_back(text(" Select Model ") | bold | inverted);
  items.push_back(separator());
  if (modelNames_.empty()) {
    items.push_back(text(" (no models available) ") | dim);
  }
  for (size_t i = 0; i < modelNames_.size(); ++i) {
    auto entry = text(" " + modelNames_[i] + " ");
    if (static_cast<int>(i) == selectedModelIndex_) {
      entry = entry | inverted | color(theme_.accentColor) | bold;
    }
    items.push_back(entry);
  }
  items.push_back(separator());
  items.push_back(text(" [Up/Down] Move  [Enter] Select  [Esc] Cancel ") |
                  center | dim);
  return vbox(std::move(items)) | border | size(WIDTH, LESS_THAN, 50) |
         color(theme_.accentColor);
}

void AgentTUI::openModelSelector() {
  modelNames_.clear();
  selectedModelIndex_ = 0;
  if (agentContext_ && agentContext_->modelRegistry) {
    modelNames_ = agentContext_->modelRegistry->listModelNames();
    const auto current = agentContext_->modelRegistry->getCurrentModelName();
    for (size_t i = 0; i < modelNames_.size(); ++i) {
      if (modelNames_[i] == current) {
        selectedModelIndex_ = static_cast<int>(i);
        break;
      }
    }
  }
  showModelSelector_ = true;
}

void AgentTUI::confirmModelSelection() {
  if (selectedModelIndex_ >= 0 &&
      selectedModelIndex_ < static_cast<int>(modelNames_.size())) {
    if (agentContext_ && agentContext_->modelRegistry) {
      agentContext_->modelRegistry->setCurrentModel(
          modelNames_[selectedModelIndex_]);
    }
  }
  showModelSelector_ = false;
}

void AgentTUI::cancelCurrentRun() {
  if (agentContext_) {
    auto token = agentContext_->getCancelToken();
    if (token) {
      token->cancel();
    }
  }
  if (!currentToken_.empty()) {
    messages_.push_back({currentTokenRole_, currentToken_});
    currentToken_.clear();
  }
  messages_.push_back({Message::Role::System, "[Cancelled by user]"});
  isStreaming_ = false;
}

void AgentTUI::onToken(const std::string &token, const std::string &kind) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto role = (kind == "thinking") ? Message::Role::Thinking
                                     : Message::Role::Assistant;
    if (currentTokenRole_ != role && !currentToken_.empty()) {
      messages_.push_back({currentTokenRole_, currentToken_});
      currentToken_.clear();
    }
    currentTokenRole_ = role;
    currentToken_ += token;
    isStreaming_ = true;
  }
  postRedraw();
}

void AgentTUI::onDisplay(const std::string &level,
                         const std::string &content) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    messages_.push_back({Message::Role::System, content});
  }
  postRedraw();
}

asio::awaitable<std::optional<std::string>> AgentTUI::getInput() {
  auto [ec, line] = co_await inputChannel_->async_receive(
      asio::as_tuple(asio::use_awaitable));
  if (ec) {
    co_return std::nullopt;
  }
  co_return std::optional<std::string>(std::move(line));
}

asio::awaitable<bool>
AgentTUI::promptPermission(const std::string &toolName,
                           const std::string &category,
                           const std::string &target) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pendingPermission_ = PermissionRequest{toolName, category, target};
  }
  postRedraw();

  auto [ec, allowed] = co_await permissionChannel_->async_receive(
      asio::as_tuple(asio::use_awaitable));
  if (ec) {
    co_return false;
  }
  co_return allowed;
}

void AgentTUI::onInterrupt(const std::string &node, const std::string &value,
                           const std::string &handleName) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string msg = "Interrupted at: " + node + "\nValue: " + value;
    if (!handleName.empty()) {
      msg += "\nHandle: " + handleName;
    }
    messages_.push_back({Message::Role::System, msg});
  }
  postRedraw();
}

void AgentTUI::resetTokenState() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!currentToken_.empty()) {
      messages_.push_back({currentTokenRole_, currentToken_});
      currentToken_.clear();
    }
    isStreaming_ = false;
  }
  postRedraw();
}
