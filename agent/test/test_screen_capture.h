#pragma once

#include "agentxx/expand/screen_capture.h"
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "test_framework.h"

#if XX_IS_WIN_D
#include <windows.h>
#endif

#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_sc_passed
#define XX_TEST_FAILED g_sc_failed

inline agentxx::test::TestResult test_screen_capture() {
#if XX_IS_WIN_D
  int g_sc_passed = 0;
  int g_sc_failed = 0;

  agentxx::expand::ScreenCapture capture;

  int screen_count = capture.getScreenCount();
  TEST_INFO << "检测到 " << screen_count << " 个屏幕" << std::endl;
  if (screen_count <= 0) {
    g_sc_failed++;
    TEST_FAIL << "getScreenCount() 返回 <= 0" << std::endl;
    return agentxx::test::TestResult{g_sc_passed, g_sc_failed};
  }
  g_sc_passed++;
  TEST_PASS << "getScreenCount() = " << screen_count << std::endl;

  {
    auto frames = capture.captureAllScreens();
    TEST_INFO << "captureAllScreens() 返回 " << frames.size() << " 帧"
              << std::endl;
    if (frames.empty()) {
      g_sc_failed++;
      TEST_FAIL << "captureAllScreens() 返回空数组" << std::endl;
      return agentxx::test::TestResult{g_sc_passed, g_sc_failed};
    }

    for (size_t i = 0; i < frames.size(); ++i) {
      const auto &f = frames[i];
      TEST_INFO << "Frame[" << i << "]: " << f.width << "x" << f.height
                << " offset=(" << f.offsetX << "," << f.offsetY << ")"
                << " screenIndex=" << f.screenIndex << " name=" << f.screenName
                << " primary=" << (f.isPrimary ? "true" : "false")
                << " pixels=" << f.pixelData.size() << " bytes" << std::endl;

      if (f.width <= 0 || f.height <= 0) {
        g_sc_failed++;
        TEST_FAIL << "Frame[" << i << "] 宽高无效" << std::endl;
        return agentxx::test::TestResult{g_sc_passed, g_sc_failed};
      }
      if (f.pixelData.size() != static_cast<size_t>(f.width * f.height * 4)) {
        g_sc_failed++;
        TEST_FAIL << "Frame[" << i << "] 像素数据大小不匹配: 期望 "
                  << (f.width * f.height * 4) << " 实际 " << f.pixelData.size()
                  << std::endl;
        return agentxx::test::TestResult{g_sc_passed, g_sc_failed};
      }
    }
    g_sc_passed++;
    TEST_PASS << "captureAllScreens() 所有帧信息正确" << std::endl;
  }

  {
    auto frame = capture.captureScreen(0);
    TEST_INFO << "captureScreen(0): " << frame.width << "x" << frame.height
              << " offset=(" << frame.offsetX << "," << frame.offsetY
              << ") name=" << frame.screenName << std::endl;
    if (frame.width <= 0 || frame.height <= 0) {
      g_sc_failed++;
      TEST_FAIL << "captureScreen(0) 无效" << std::endl;
      return agentxx::test::TestResult{g_sc_passed, g_sc_failed};
    }
    if (frame.pixelData.empty()) {
      g_sc_failed++;
      TEST_FAIL << "captureScreen(0) 像素数据为空" << std::endl;
      return agentxx::test::TestResult{g_sc_passed, g_sc_failed};
    }
    g_sc_passed++;
    TEST_PASS << "captureScreen(0) 正确" << std::endl;
  }

  {
    auto frame = capture.captureMouseScreen();
    TEST_INFO << "captureMouseScreen(): " << frame.width << "x" << frame.height
              << " offset=(" << frame.offsetX << "," << frame.offsetY
              << ") name=" << frame.screenName << std::endl;
    if (frame.width <= 0 || frame.height <= 0) {
      g_sc_failed++;
      TEST_FAIL << "captureMouseScreen() 无效" << std::endl;
      return agentxx::test::TestResult{g_sc_passed, g_sc_failed};
    }
    if (frame.pixelData.empty()) {
      g_sc_failed++;
      TEST_FAIL << "captureMouseScreen() 像素数据为空" << std::endl;
      return agentxx::test::TestResult{g_sc_passed, g_sc_failed};
    }
    g_sc_passed++;
    TEST_PASS << "captureMouseScreen() 正确" << std::endl;
  }

  {
    auto frame = capture.captureScreen(9999);
    if (frame.width == 0 && frame.height == 0 && frame.pixelData.empty()) {
      g_sc_passed++;
      TEST_PASS << "captureScreen(9999) 无效索引正确返回空帧" << std::endl;
    } else {
      g_sc_failed++;
      TEST_FAIL << "captureScreen(9999) 应返回空帧" << std::endl;
      return agentxx::test::TestResult{g_sc_passed, g_sc_failed};
    }
  }

  {
    TEST_INFO << "测试流式订阅 (5 fps, 持续 2 秒)..." << std::endl;
    int frame_count = 0;
    bool streaming_started = capture.startStreaming(
        5, [&frame_count](const std::vector<agentxx::expand::ScreenFrame> &) {
          frame_count++;
        });

    if (!streaming_started) {
      g_sc_failed++;
      TEST_FAIL << "startStreaming() 返回 false" << std::endl;
      return agentxx::test::TestResult{g_sc_passed, g_sc_failed};
    }
    g_sc_passed++;
    TEST_PASS << "startStreaming() 成功启动" << std::endl;

    if (!capture.isStreaming()) {
      g_sc_failed++;
      TEST_FAIL << "isStreaming() 应返回 true" << std::endl;
      return agentxx::test::TestResult{g_sc_passed, g_sc_failed};
    }
    g_sc_passed++;
    TEST_PASS << "isStreaming() = true" << std::endl;

    std::this_thread::sleep_for(std::chrono::milliseconds(2200));

    capture.stopStreaming();

    if (capture.isStreaming()) {
      g_sc_failed++;
      TEST_FAIL << "stopStreaming() 后 isStreaming() 应返回 false" << std::endl;
      return agentxx::test::TestResult{g_sc_passed, g_sc_failed};
    }
    g_sc_passed++;
    TEST_PASS << "stopStreaming() 成功停止" << std::endl;

    TEST_INFO << "2 秒内收到 " << frame_count << " 帧 (期望约 10 帧)"
              << std::endl;
    if (frame_count < 5) {
      g_sc_failed++;
      TEST_FAIL << "流式订阅帧数过少: " << frame_count << std::endl;
      return agentxx::test::TestResult{g_sc_passed, g_sc_failed};
    }
    g_sc_passed++;
    TEST_PASS << "流式订阅帧率正常" << std::endl;
  }

  return agentxx::test::TestResult{g_sc_passed, g_sc_failed};
#else
  TEST_SKIP << "ScreenCapture 仅支持 Windows 平台" << std::endl;
  return agentxx::test::TestResult{0, 0};
#endif
}