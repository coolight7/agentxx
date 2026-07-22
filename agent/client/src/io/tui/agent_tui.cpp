#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx/agent/model_registry.h"
#include "fmt/format.h"
#include "ftxui/component/event.hpp"
#include "neograph/graph/cancel.h"

using namespace ftxui;

AgentTUI::AgentTUI(asio::any_io_executor ex,
                   std::shared_ptr<agentxx::agent::AgentContext> agentContext,
                   std::string threadId, TUITheme theme)
    : agentContext_(std::move(agentContext)), theme_(theme),
      threadId_(std::move(threadId)),
      inputChannel_(std::make_shared<LineChannel>(ex, 64)),
      permissionChannel_(std::make_shared<BoolChannel>(ex, 4)),
      logSink_(std::make_shared<TUILogSink>()) {}

AgentTUI::~AgentTUI() { stop(); }

void AgentTUI::postRedraw() {
  if (screen_) {
    screen_->PostEvent(Event::Custom);
  }
}

void AgentTUI::start() {
  running_ = true;
  if (logSink_) {
    // 使用 weak_ptr 避免日志线程在 TUI 析构后回调到已销毁对象
    std::weak_ptr<AgentTUI> weakSelf = shared_from_this();
    logSink_->setOnNewLog([weakSelf]() {
      if (auto self = weakSelf.lock()) {
        self->postRedraw();
      }
    });
    agentxx::util::LogDispatcher::instance().addSink(logSink_);
  }
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

      Element body = main;
      if (false == sidebarTabs_.empty()) {
        body = hbox({
            main | flex,
            renderSidebar(),
        });
      }

      if (pendingPermission_.has_value()) {
        return renderPermissionOverlay() | center;
      }
      if (showModelSelector_) {
        return renderModelSelectorOverlay() | center;
      }
      return body;
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
      if (event == Event::F12) {
        toggleLogWindow();
        postRedraw();
        return true;
      }
      if (event.is_mouse()) {
        if (handleSidebarMouse(event.mouse())) {
          postRedraw();
          return true;
        }
        return false;
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
  if (logSink_) {
    agentxx::util::LogDispatcher::instance().removeSink(logSink_);
    logSink_->setOnNewLog(nullptr);
  }
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
  std::string modelName = currentModelName();
  if (modelName.empty()) {
    modelName = "<none>";
  }
  auto modelInfo = hbox({
      text(" model: ") | color(theme_.hintColor),
      text(modelName) | color(theme_.accentColor) | bold,
      text(" [F2] ") | color(theme_.hintColor),
  });

  size_t ctx = 0;
  size_t maxCtx = 0;
  if (auto session = currentSession()) {
    if (session->contextStats) {
      ctx = session->contextStats->contextTokens.load();
      maxCtx = session->contextStats->maxContextTokens.load();
    }
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

ftxui::Element AgentTUI::renderSidebar() {
  // tab 栏: 每个 tab 标题经 reflect 记录渲染区域, 供鼠标点击检测
  tabBoxes_.assign(sidebarTabs_.size(), ftxui::Box{});
  Elements tabs;
  for (size_t i = 0; i < sidebarTabs_.size(); ++i) {
    auto label = text(" " + sidebarTabs_[i].title + " ");
    if (static_cast<int>(i) == activeTabIndex_) {
      label = label | inverted | color(theme_.accentColor) | bold;
    } else {
      label = label | color(theme_.statusColor);
    }
    tabs.push_back(label | reflect(tabBoxes_[i]));
  }
  auto tabBar = hbox(std::move(tabs)) | xframe;

  Element content = text(" ");
  if (activeTabIndex_ >= 0 &&
      activeTabIndex_ < static_cast<int>(sidebarTabs_.size())) {
    content = sidebarTabs_[activeTabIndex_].render();
  }

  return vbox({
             tabBar,
             separator(),
             content | flex | vscroll_indicator | frame,
         }) |
         size(WIDTH, LESS_THAN, 56) | size(WIDTH, GREATER_THAN, 28) | border;
}

ftxui::Element AgentTUI::renderLogWindow() {
  auto lines = logSink_ ? logSink_->snapshot()
                        : std::vector<TUILogSink::Line>{};
  Elements elements;
  for (const auto &line : lines) {
    ftxui::Color c = theme_.assistantColor;
    std::string prefix;
    switch (line.level) {
    case agentxx::util::LogLevel::Debug:
      c = theme_.hintColor;
      prefix = "[D] ";
      break;
    case agentxx::util::LogLevel::Info:
      c = theme_.statusColor;
      prefix = "[I] ";
      break;
    case agentxx::util::LogLevel::Warn:
      c = theme_.thinkingColor;
      prefix = "[W] ";
      break;
    case agentxx::util::LogLevel::Error:
      c = theme_.systemColor;
      prefix = "[E] ";
      break;
    case agentxx::util::LogLevel::Out:
      c = theme_.assistantColor;
      prefix = "";
      break;
    }
    elements.push_back(paragraph(prefix + line.text) | color(c));
  }
  if (elements.empty()) {
    return text(" (no logs) ") | dim;
  }
  return vbox(std::move(elements));
}

void AgentTUI::addSidebarTab(const std::string &id, const std::string &title,
                             std::function<ftxui::Element()> render) {
  for (auto &tab : sidebarTabs_) {
    if (tab.id == id) {
      tab.title = title;
      tab.render = std::move(render);
      return;
    }
  }
  sidebarTabs_.push_back(SidebarTab{id, title, std::move(render)});
  activeTabIndex_ = static_cast<int>(sidebarTabs_.size()) - 1;
}

void AgentTUI::removeSidebarTab(const std::string &id) {
  for (size_t i = 0; i < sidebarTabs_.size(); ++i) {
    if (sidebarTabs_[i].id == id) {
      sidebarTabs_.erase(sidebarTabs_.begin() + i);
      if (activeTabIndex_ >= static_cast<int>(sidebarTabs_.size())) {
        activeTabIndex_ = static_cast<int>(sidebarTabs_.size()) - 1;
      }
      return;
    }
  }
}

bool AgentTUI::hasSidebarTab(const std::string &id) const {
  for (const auto &tab : sidebarTabs_) {
    if (tab.id == id) {
      return true;
    }
  }
  return false;
}

void AgentTUI::toggleLogWindow() {
  if (hasSidebarTab(kLogTabId)) {
    removeSidebarTab(kLogTabId);
  } else {
    addSidebarTab(kLogTabId, "Logs", [this]() { return renderLogWindow(); });
  }
}

bool AgentTUI::handleSidebarMouse(const ftxui::Mouse &mouse) {
  for (size_t i = 0; i < sidebarTabs_.size() && i < tabBoxes_.size(); ++i) {
    if (false == tabBoxes_[i].Contain(mouse.x, mouse.y)) {
      continue;
    }
    if (mouse.button == Mouse::Left && mouse.motion == Mouse::Released) {
      // 左键点击: 切换到该 tab
      activeTabIndex_ = static_cast<int>(i);
      return true;
    }
    if (mouse.button == Mouse::Right && mouse.motion == Mouse::Released) {
      // 右键点击: 关闭该 tab
      removeSidebarTab(sidebarTabs_[i].id);
      return true;
    }
  }
  return false;
}

void AgentTUI::openModelSelector() {
  modelNames_.clear();
  selectedModelIndex_ = 0;
  if (agentContext_ && agentContext_->modelRegistry) {
    modelNames_ = agentContext_->modelRegistry->listModelNames();
    const auto current = currentModelName();
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
    if (auto session = currentSession()) {
      session->setModelName(modelNames_[selectedModelIndex_]);
    }
  }
  showModelSelector_ = false;
}

void AgentTUI::cancelCurrentRun() {
  if (auto session = currentSession()) {
    auto token = session->getCancelToken();
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

std::shared_ptr<agentxx::agent::Session> AgentTUI::currentSession() {
  if (agentContext_ && agentContext_->sessions) {
    return agentContext_->sessions->getOrCreate(threadId_);
  }
  return nullptr;
}

std::string AgentTUI::currentModelName() {
  std::string selected;
  if (auto session = currentSession()) {
    selected = session->getModelName();
  }
  if (agentContext_ && agentContext_->modelRegistry) {
    return agentContext_->modelRegistry->resolveModelName(selected);
  }
  return selected;
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
