#pragma once

#include "agentxx/agent/agent_io.h"
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

  ftxui::ScreenInteractive *screen_ = nullptr;
  std::thread uiThread_;
  std::atomic<bool> running_{false};

  std::shared_ptr<LineChannel> inputChannel_;
  std::shared_ptr<BoolChannel> permissionChannel_;

  void postRedraw();
  ftxui::Element renderMessages();
  ftxui::Element renderPermissionOverlay();

public:
  explicit AgentTUI(asio::any_io_executor ex);
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
