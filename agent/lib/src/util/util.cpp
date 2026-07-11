#include "agentxx/util/util.h"
#include "agentxx/util/log.h"
#include <filesystem>
#include <fstream>
#include <iostream>

#if XX_IS_LINUX_D
#include <sys/utsname.h>

std::string agentxx::util::getSystemName() {
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
  if (!name.empty()) {
    return name;
  }

  // 备选：uname 系统调用
  struct utsname buf;
  if (uname(&buf) == 0) {
    return std::string(buf.sysname) + " " + buf.release;
  }
  return "Linux";
}

bool agentxx::util::isRunningInWSL() {
  try {
    return std::filesystem::exists("/proc/sys/fs/binfmt_misc/WSLInterop");
  } catch (const std::exception &e) {
    XX_LOGD("isRunningInWSL exception: {}", e.what());
  }
  return false;
}

#elif XX_IS_WIN_D
#include <windows.h>
std::string agentxx::util::getSystemName() {
  OSVERSIONINFOEXW info{};
  info.dwOSVersionInfoSize = sizeof(info);
  HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll");
  if (hNtDll) {
    typedef LONG(WINAPI * RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
    auto RtlGetVersion =
        (RtlGetVersionPtr)GetProcAddress(hNtDll, "RtlGetVersion");
    if (RtlGetVersion && RtlGetVersion((PRTL_OSVERSIONINFOW)&info) == 0) {
      return "Windows " + std::to_string(info.dwMajorVersion) + "." +
             std::to_string(info.dwMinorVersion) + " (build " +
             std::to_string(info.dwBuildNumber) + ")";
    }
  }
  return "Windows";
}

bool agentxx::util::isRunningInWSL() { return false; }
#endif