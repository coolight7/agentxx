#pragma once

#include "agentxx-client/io/tui/tui_theme.h"
#include "agentxx/agent/agent_io.h"
#include "agentxx/agent/context.h"
#include "agentxx/util/log.h"
#include "asio/as_tuple.hpp"
#include "asio/awaitable.hpp"
#include "asio/experimental/concurrent_channel.hpp"
#include "asio/use_awaitable.hpp"
#include "ftxui/component/component.hpp"
#include "ftxui/component/mouse.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/box.hpp"
#include "neograph/api.h"
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

/// TUI 日志接收器
/// - 注册到 agentxx::util::LogDispatcher 以接收 XX_LOG 系列日志
/// - 线程安全缓存日志行, 供右侧边栏日志窗口渲染
class TUILogSink : public agentxx::util::LogSink {
public:
  struct Line {
    agentxx::util::LogLevel level;
    std::string text;
  };

  void onLog(agentxx::util::LogLevel level,
             const std::string &message) override {
    std::function<void()> cb;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      lines_.push_back(Line{level, message});
      while (lines_.size() > maxLines_) {
        lines_.pop_front();
      }
      cb = onNewLog_;
    }
    if (cb) {
      cb();
    }
  }

  /// 拷贝当前缓存的日志行 (供渲染线程读取)
  std::vector<Line> snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {lines_.begin(), lines_.end()};
  }

  void setOnNewLog(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    onNewLog_ = std::move(cb);
  }

  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    lines_.clear();
  }

private:
  mutable std::mutex mutex_;
  std::deque<Line> lines_;
  size_t maxLines_ = 2000;
  std::function<void()> onNewLog_;
};

class AgentTUI : public agentxx::agent::AgentIOBase,
                 public std::enable_shared_from_this<AgentTUI> {
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

  /// 右侧边栏 tab (类似浏览器 tab)
  struct SidebarTab {
    std::string id;
    std::string title;
    std::function<ftxui::Element()> render;
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

  /// 右侧边栏状态
  std::vector<SidebarTab> sidebarTabs_;
  int activeTabIndex_ = 0;
  /// 各 tab 标题的渲染区域, 用于鼠标点击检测 (渲染时经 reflect 填充)
  std::vector<ftxui::Box> tabBoxes_;

  std::shared_ptr<agentxx::agent::AgentContext> agentContext_;
  TUITheme theme_;
  /// 本 TUI 绑定的会话 thread_id (按 thread_id 取会话状态)
  std::string threadId_;

  ftxui::ScreenInteractive *screen_ = nullptr;
  std::thread uiThread_;
  std::atomic<bool> running_{false};

  std::shared_ptr<LineChannel> inputChannel_;
  std::shared_ptr<BoolChannel> permissionChannel_;
  std::shared_ptr<TUILogSink> logSink_;

  /// 日志窗口 tab 的固定 id
  static constexpr const char *kLogTabId = "xx_logs";

  void postRedraw();
  ftxui::Element renderMessages();
  ftxui::Element renderPermissionOverlay();
  ftxui::Element renderModelSelectorOverlay();
  /// 输入框下方的状态栏: 左侧模型名, 右侧上下文占用
  ftxui::Element renderStatusBar();
  /// 右侧边栏: 顶部 tab 栏 + 当前 tab 内容; 无 tab 时不应调用
  ftxui::Element renderSidebar();
  /// 日志窗口内容
  ftxui::Element renderLogWindow();

  /// 打开模型选择器 (刷新可用模型列表)
  void openModelSelector();
  /// 确认选择当前高亮的模型
  void confirmModelSelection();
  /// 取消当前正在执行的轮次
  void cancelCurrentRun();

  /// 侧边栏 tab 管理
  void addSidebarTab(const std::string &id, const std::string &title,
                     std::function<ftxui::Element()> render);
  void removeSidebarTab(const std::string &id);
  bool hasSidebarTab(const std::string &id) const;
  /// F12: 插入/关闭日志窗口 tab
  void toggleLogWindow();
  /// 处理侧边栏 tab 的鼠标点击; 返回是否消费了该事件
  bool handleSidebarMouse(const ftxui::Mouse &mouse);

  /// 获取本 TUI 绑定的会话 (按 threadId_)
  std::shared_ptr<agentxx::agent::Session> currentSession();
  /// 解析本会话当前实际使用的模型显示名称
  std::string currentModelName();

public:
  explicit AgentTUI(asio::any_io_executor ex,
                    std::shared_ptr<agentxx::agent::AgentContext> agentContext,
                    std::string threadId = "session",
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
