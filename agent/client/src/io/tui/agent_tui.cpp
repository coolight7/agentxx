#include "agentxx-client/io/tui/agent_tui.h"
#include "ftxui/component/event.hpp"

using namespace ftxui;

AgentTUI::AgentTUI(asio::any_io_executor ex)
    : inputChannel_(std::make_shared<LineChannel>(ex, 64)),
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
          text(">>> ") | color(Color::Green) | bold,
          input->Render() | flex,
      });

      auto main = vbox({
                    messages,
                    separator(),
          input_bar,
      });

      if (pendingPermission_.has_value()) {
        return renderPermissionOverlay() | center;
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
          text("> ") | color(Color::Cyan) | bold,
          paragraph(msg.text) | color(Color::Cyan),
      }));
      break;
    case Message::Role::Assistant:
      elements.push_back(paragraph(msg.text));
      break;
    case Message::Role::Thinking:
      elements.push_back(hbox({
          text("[Thinking] ") | color(Color::Yellow) | dim,
          paragraph(msg.text) | color(Color::Yellow) | dim,
      }));
      break;
    case Message::Role::System:
      elements.push_back(paragraph(msg.text) | color(Color::Red));
      break;
    }
    elements.push_back(text(""));
  }

  if (isStreaming_ && !currentToken_.empty()) {
    if (currentTokenRole_ == Message::Role::Thinking) {
      elements.push_back(hbox({
          text("[Thinking] ") | color(Color::Yellow) | dim,
          paragraph(currentToken_) | color(Color::Yellow) | dim,
      }));
    } else {
      elements.push_back(paragraph(currentToken_));
    }
  }

  if (elements.empty()) {
    return vbox({
        filler(),
        text("Agentxx TUI") | bold | color(Color::Cyan) | center,
        text("Type a message to start. Ctrl+C to quit.") | dim | center,
        filler(),
    });
  }
  return vbox(std::move(elements));
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
         border | size(WIDTH, LESS_THAN, 60) |
         color(Color::Red);
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
