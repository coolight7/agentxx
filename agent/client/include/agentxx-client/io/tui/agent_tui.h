#pragma once

#include "agentxx-client/io/tui/tui_theme.h"
#include "agentxx/agent/agent_io.h"
#include "agentxx/agent/context.h"
#include "asio/as_tuple.hpp"
#include "asio/awaitable.hpp"
#include "asio/experimental/concurrent_channel.hpp"
#include "asio/use_awaitable.hpp"
#include "ftxui/component/component.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"
#include "neograph/api.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class AgentTUI : public agentxx::agent::AgentIOBase {
public:
  using LineChannel = asio::experimental::concurrent_channel<void(
      neograph_asio_error_code, std::string)>;
  using BoolChannel = asio::experimental::concurrent_channel<void(
      neograph_asio_error_code, bool)>;

private:
  struct Message {
    enum class Role { User, Assistant, Thinking, System };
    Role role;
    std::string text;
  };

  struct PermissionRequest {
    std::string toolName;
    std::string category;
    std::string target;
  };

  std::mutex mutex_;
  std::vector<Message> messages_;
  std::string currentToken_;
  Message::Role currentTokenRole_ = Message::Role::Assistant;
  bool isStreaming_ = false;

  std::string inputText_;
  std::optional<PermissionRequest> pendingPermission_;

  /// 模型选择器状态
  bool showModelSelector_ = false;
  int selectedModelIndex_ = 0;
  std::vector<std::string> modelNames_;

  std::shared_ptr<agentxx::agent::AgentContext> agentContext_;
  TUITheme theme_;

  ftxui::ScreenInteractive *screen_ = nullptr;
  std::thread uiThread_;
  std::atomic<bool> running_{false};

  std::shared_ptr<LineChannel> inputChannel_;
  std::shared_ptr<BoolChannel> permissionChannel_;

  void postRedraw();
  ftxui::Element renderMessages();
  ftxui::Element renderPermissionOverlay();
  ftxui::Element renderModelSelectorOverlay();
  /// 输入框下方的状态栏: 左侧模型名, 右侧上下文占用
  ftxui::Element renderStatusBar();

  /// 打开模型选择器 (刷新可用模型列表)
  void openModelSelector();
  /// 确认选择当前高亮的模型
  void confirmModelSelection();
  /// 取消当前正在执行的轮次
  void cancelCurrentRun();

public:
  explicit AgentTUI(asio::any_io_executor ex,
                    std::shared_ptr<agentxx::agent::AgentContext> agentContext,
                    TUITheme theme = TUITheme::darkTheme());
  ~AgentTUI() override;

  void start();
  void stop();

  void onToken(const std::string &token, const std::string &kind) override;
  void onDisplay(const std::string &level,
                 const std::string &content) override;
  asio::awaitable<std::optional<std::string>> getInput() override;
  asio::awaitable<bool>
  promptPermission(const std::string &toolName, const std::string &category,
                   const std::string &target) override;
  void onInterrupt(const std::string &node, const std::string &value,
                   const std::string &handleName) override;

  void resetTokenState();
};
