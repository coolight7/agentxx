#include "agentxx/util/util.h"
#include "agentxx/util/log.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>

#if XX_IS_LINUX_D
#include <sys/utsname.h>

static std::optional<std::string> systemName_;
static std::optional<bool> isRunningInWSL_;

std::string agentxx::util::getSystemName() {
  if (systemName_.has_value()) {
    return *systemName_;
  }
  std::ifstream f("/etc/os-release");
  std::string line, name;
  while (std::getline(f, line)) {
    if (line.rfind("PRETTY_NAME=", 0) == 0) {
      // 去掉 PRETTY_NAME="..." 两侧的引号
      name = line.substr(13);
      if (!name.empty() && name.front() == '"')
        name.erase(0, 1);
      if (!name.empty() && name.back() == '"')
        name.pop_back();
      break;
    }
  }
  f.close();
  systemName_ = name;
  if (!name.empty()) {
    return name;
  }

  // 备选：uname 系统调用
  struct utsname buf;
  if (uname(&buf) == 0) {
    systemName_ = std::string(buf.sysname) + " " + buf.release;
    return *systemName_;
  }
  systemName_ = "Linux";
  return *systemName_;
}

bool agentxx::util::isRunningInWSL() {
  try {
    if (isRunningInWSL_.has_value()) {
      return *isRunningInWSL_;
    }
    isRunningInWSL_ =
        std::filesystem::exists("/proc/sys/fs/binfmt_misc/WSLInterop");
    return *isRunningInWSL_;
  } catch (const std::exception &e) {
    isRunningInWSL_ = false;
    XX_LOGD("isRunningInWSL exception: {}", e.what());
  }
  return false;
}

#elif XX_IS_WIN_D
#include <windows.h>
std::string agentxx::util::getSystemName() {
  if (systemName_.has_value()) {
    return *systemName_;
  }

  OSVERSIONINFOEXW info{};
  info.dwOSVersionInfoSize = sizeof(info);
  HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll");
  if (hNtDll) {
    typedef LONG(WINAPI * RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
    auto RtlGetVersion =
        (RtlGetVersionPtr)GetProcAddress(hNtDll, "RtlGetVersion");
    if (RtlGetVersion && RtlGetVersion((PRTL_OSVERSIONINFOW)&info) == 0) {
      systemName_ = "Windows " + std::to_string(info.dwMajorVersion) + "." +
                    std::to_string(info.dwMinorVersion) + " (build " +
                    std::to_string(info.dwBuildNumber) + ")";
      return *systemName_;
    }
  }
  systemName_ = "Windows";
  return *systemName_;
}

bool agentxx::util::isRunningInWSL() { return false; }

#else

std::string agentxx::util::getSystemName() {
#if XX_IS_WIN_D
  return "Windows";
#elif XX_IS_LINUX_D
  return "Linux";
#elif XX_IS_MACOS_D
  return "macOS";
#elif XX_IS_ANDROID_D
  return "Android";
#elif XX_IS_IOS_D
  return "iOS";
#else
  return "Unknown";
#endif
}

bool agentxx::util::isRunningInWSL() { return false; }

#endif