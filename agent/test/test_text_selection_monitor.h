#pragma once

#include "agentxx/expand/text_selection_monitor.h"
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#if XX_IS_WIN_D
#include <windows.h>
#endif

std::shared_ptr<agentxx::expand::TextSelectionMonitor>
test_text_selection_monitor() {
#if XX_IS_WIN_D
  std::cout << "请在浏览器或其他程序中选中一段文本，本程序将自动捕获并输出。"
            << std::endl;

  auto monitor = std::make_shared<agentxx::expand::TextSelectionMonitor>();

  monitor->addListener([](const agentxx::expand::TextSelectionEvent &evt) {
    std::cout << "[选中文本] " << evt.text << std::endl;
  });

  if (!monitor->start()) {
    std::cerr << "[FAIL] 启动 TextSelectionMonitor 失败" << std::endl;
    return nullptr;
  }

  std::cout << "[OK] TextSelectionMonitor 已启动，等待文本选中事件..."
            << std::endl;
  return monitor;
#endif
  return nullptr;
}
