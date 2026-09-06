#pragma once

#include "agentxx/agent/config.h"
#include "agentxx/agent/conversation_types.h"
#include "agentxx/middlewares/summarization.h"
#include "agentxx/util/log.h"
#include "fmt/format.h"
#include "neograph/types.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if XX_IS_WIN_D
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <psapi.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <unistd.h>
#endif

namespace agentxx {
namespace bench {

// ---------------------------------------------------------------------------
// 1. 进程内存与 CPU 采样
// ---------------------------------------------------------------------------

struct ProcMemSample {
    double   rssMB     = 0.0; ///< 常驻物理内存 (MB)
    double   privateMB = 0.0; ///< 私有内存 (MB)
    uint64_t vmsizeKB  = 0;   ///< 虚拟内存大小 (KB)
    bool     valid     = false;
};

/// 采样指定进程内存 (pid=0 表示自身进程)
inline ProcMemSample sampleProcessMemory(uint32_t pid = 0) {
    ProcMemSample out;
#if XX_IS_WIN_D
    HANDLE h = nullptr;
    if (pid == 0 || pid == static_cast<uint32_t>(::GetCurrentProcessId())) {
        h = ::GetCurrentProcess();
    } else {
        h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    }
    if (h) {
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        if (::GetProcessMemoryInfo(
                h,
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                sizeof(pmc)
            )) {
            out.rssMB     = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
            out.privateMB = static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
            out.vmsizeKB  = static_cast<uint64_t>(pmc.PagefileUsage / 1024);
            out.valid     = true;
        }
        if (h != ::GetCurrentProcess()) {
            ::CloseHandle(h);
        }
    }
#else
    uint32_t actualPid = pid;
    if (actualPid == 0) {
        actualPid = static_cast<uint32_t>(getpid());
    }

    // 优先从 /proc/<pid>/status 解析精细数据 (VmRSS, RssAnon, RssShmem)
    std::string   statusPath = fmt::format("/proc/{}/status", actualPid);
    std::ifstream sfs(statusPath);
    if (sfs.is_open()) {
        std::string line;
        uint64_t    vmRssKB   = 0;
        uint64_t    rssAnonKB = 0;
        uint64_t    rssShmKB  = 0;
        uint64_t    vmSizeKB  = 0;
        bool        hasRss    = false;
        while (std::getline(sfs, line)) {
            if (line.rfind("VmRSS:", 0) == 0) {
                std::istringstream iss(line.substr(6));
                iss >> vmRssKB;
                hasRss = true;
            } else if (line.rfind("RssAnon:", 0) == 0) {
                std::istringstream iss(line.substr(8));
                iss >> rssAnonKB;
            } else if (line.rfind("RssShmem:", 0) == 0) {
                std::istringstream iss(line.substr(9));
                iss >> rssShmKB;
            } else if (line.rfind("VmSize:", 0) == 0) {
                std::istringstream iss(line.substr(7));
                iss >> vmSizeKB;
            }
        }
        if (hasRss) {
            out.rssMB     = static_cast<double>(vmRssKB) / 1024.0;
            out.privateMB = static_cast<double>(rssAnonKB + rssShmKB) / 1024.0;
            if (out.privateMB <= 0.0) {
                out.privateMB = out.rssMB;
            }
            out.vmsizeKB = vmSizeKB;
            out.valid    = true;
            return out;
        }
    }

    // 回退到 /proc/<pid>/statm
    std::string   statmPath = fmt::format("/proc/{}/statm", actualPid);
    std::ifstream mfs(statmPath);
    if (mfs.is_open()) {
        uint64_t sizePages = 0, residentPages = 0, sharedPages = 0;
        if (mfs >> sizePages >> residentPages >> sharedPages) {
            long pageSize = sysconf(_SC_PAGESIZE);
            if (pageSize <= 0) {
                pageSize = 4096;
            }
            out.rssMB = static_cast<double>(residentPages * pageSize) / (1024.0 * 1024.0);
            if (residentPages >= sharedPages) {
                out.privateMB = static_cast<double>((residentPages - sharedPages) * pageSize)
                                / (1024.0 * 1024.0);
            } else {
                out.privateMB = out.rssMB;
            }
            out.vmsizeKB = (sizePages * pageSize) / 1024;
            out.valid    = true;
        }
    }
#endif
    return out;
}

/// 采样多次取 RSS 中位数以抑抖动
inline ProcMemSample sampleMemoryMedian(uint32_t pid = 0, size_t times = 3, int intervalMs = 50) {
    std::vector<ProcMemSample> samples;
    for (size_t i = 0; i < times; ++i) {
        auto s = sampleProcessMemory(pid);
        if (s.valid) {
            samples.push_back(s);
        }
        if (i + 1 < times) {
            std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
        }
    }
    if (samples.empty()) {
        return sampleProcessMemory(pid);
    }
    std::sort(samples.begin(), samples.end(), [](const auto& a, const auto& b) {
        return a.rssMB < b.rssMB;
    });
    return samples[samples.size() / 2];
}

struct CpuWindow {
    uint32_t                              pid          = 0;
    uint64_t                              cpuTimeTicks = 0;
    std::chrono::steady_clock::time_point startTime;
    bool                                  valid = false;
};

/// 开始 CPU 采样窗口
inline CpuWindow cpuBegin(uint32_t pid = 0) {
    CpuWindow win;
    win.pid       = pid;
    win.startTime = std::chrono::steady_clock::now();
#if XX_IS_WIN_D
    HANDLE h = nullptr;
    if (pid == 0 || pid == static_cast<uint32_t>(::GetCurrentProcessId())) {
        h = ::GetCurrentProcess();
    } else {
        h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    }
    if (h) {
        FILETIME ftCreation{}, ftExit{}, ftKernel{}, ftUser{};
        if (::GetProcessTimes(h, &ftCreation, &ftExit, &ftKernel, &ftUser)) {
            uint64_t k
                = (static_cast<uint64_t>(ftKernel.dwHighDateTime) << 32) | ftKernel.dwLowDateTime;
            uint64_t u
                = (static_cast<uint64_t>(ftUser.dwHighDateTime) << 32) | ftUser.dwLowDateTime;
            win.cpuTimeTicks = k + u; // 100ns 单位
            win.valid        = true;
        }
        if (h != ::GetCurrentProcess()) {
            ::CloseHandle(h);
        }
    }
#else
    uint32_t actualPid = pid;
    if (actualPid == 0) {
        actualPid = static_cast<uint32_t>(getpid());
    }
    std::string   statPath = fmt::format("/proc/{}/stat", actualPid);
    std::ifstream fs(statPath);
    if (fs.is_open()) {
        std::string content((std::istreambuf_iterator<char>(fs)), std::istreambuf_iterator<char>());
        auto        lastParen = content.rfind(')');
        if (lastParen != std::string::npos && lastParen + 1 < content.size()) {
            std::istringstream iss(content.substr(lastParen + 1));
            // 字段从 3 开始: state(3), ppid(4), pgrp(5), session(6), tty(7), tpgid(8), flags(9),
            // minflt(10), cminflt(11), majflt(12), cmajflt(13), utime(14), stime(15)
            std::string state;
            uint64_t    ppid, pgrp, session, tty, tpgid, flags, minflt, cminflt, majflt, cmajflt;
            uint64_t    utime = 0, stime = 0;
            if (iss >> state >> ppid >> pgrp >> session >> tty >> tpgid >> flags >> minflt
                >> cminflt >> majflt >> cmajflt >> utime >> stime) {
                win.cpuTimeTicks = utime + stime;
                win.valid        = true;
            }
        }
    }
#endif
    return win;
}

/// 结束 CPU 采样窗口并返回平均利用率百分比 (例如 12.5 表示 12.5%)
inline double cpuEnd(const CpuWindow& win) {
    if (!win.valid) {
        return -1.0;
    }
    auto   now     = std::chrono::steady_clock::now();
    double wallSec = std::chrono::duration<double>(now - win.startTime).count();
    if (wallSec <= 0.001) {
        return 0.0;
    }

#if XX_IS_WIN_D
    HANDLE h = nullptr;
    if (win.pid == 0 || win.pid == static_cast<uint32_t>(::GetCurrentProcessId())) {
        h = ::GetCurrentProcess();
    } else {
        h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, win.pid);
    }
    if (h) {
        FILETIME ftCreation{}, ftExit{}, ftKernel{}, ftUser{};
        if (::GetProcessTimes(h, &ftCreation, &ftExit, &ftKernel, &ftUser)) {
            uint64_t k
                = (static_cast<uint64_t>(ftKernel.dwHighDateTime) << 32) | ftKernel.dwLowDateTime;
            uint64_t u
                = (static_cast<uint64_t>(ftUser.dwHighDateTime) << 32) | ftUser.dwLowDateTime;
            uint64_t ticks2 = k + u;
            if (h != ::GetCurrentProcess()) {
                ::CloseHandle(h);
            }
            if (ticks2 >= win.cpuTimeTicks) {
                double deltaSec = static_cast<double>(ticks2 - win.cpuTimeTicks) * 1e-7;
                return (deltaSec / wallSec) * 100.0;
            }
        }
        if (h != ::GetCurrentProcess()) {
            ::CloseHandle(h);
        }
    }
#else
    uint32_t actualPid = win.pid;
    if (actualPid == 0) {
        actualPid = static_cast<uint32_t>(getpid());
    }
    std::string   statPath = fmt::format("/proc/{}/stat", actualPid);
    std::ifstream fs(statPath);
    if (fs.is_open()) {
        std::string content((std::istreambuf_iterator<char>(fs)), std::istreambuf_iterator<char>());
        auto        lastParen = content.rfind(')');
        if (lastParen != std::string::npos && lastParen + 1 < content.size()) {
            std::istringstream iss(content.substr(lastParen + 1));
            std::string        state;
            uint64_t ppid, pgrp, session, tty, tpgid, flags, minflt, cminflt, majflt, cmajflt;
            uint64_t utime = 0, stime = 0;
            if (iss >> state >> ppid >> pgrp >> session >> tty >> tpgid >> flags >> minflt
                >> cminflt >> majflt >> cmajflt >> utime >> stime) {
                uint64_t ticks2 = utime + stime;
                if (ticks2 >= win.cpuTimeTicks) {
                    long clkTck = sysconf(_SC_CLK_TCK);
                    if (clkTck <= 0) {
                        clkTck = 100;
                    }
                    double deltaSec = static_cast<double>(ticks2 - win.cpuTimeTicks)
                                      / static_cast<double>(clkTck);
                    return (deltaSec / wallSec) * 100.0;
                }
            }
        }
    }
#endif
    return 0.0;
}

// ---------------------------------------------------------------------------
// 2. 插件路径探测与 5 常用插件列表
// ---------------------------------------------------------------------------

inline const std::vector<std::string>& getBench5PluginNames() {
    static const std::vector<std::string> kNames = {
        "agentxx_filesystem",
        "agentxx_execute_command",
        "agentxx_system",
        "agentxx_websearch",
        "agentxx_planning",
    };
    return kNames;
}

/// 定位插件目录 (优先可执行同目录的 plugins/<name>, 其次 cwd/plugins/<name>)
inline std::string resolveBenchPluginDir(const std::string& pluginName) {
    namespace fs = std::filesystem;
    std::error_code       ec;
    std::vector<fs::path> candidates;

#if XX_IS_WIN_D
    wchar_t buf[MAX_PATH];
    if (::GetModuleFileNameW(nullptr, buf, MAX_PATH) > 0) {
        candidates.push_back(fs::path(buf).parent_path() / "plugins" / pluginName);
    }
#else
    if (auto p = fs::read_symlink("/proc/self/exe", ec); !ec) {
        candidates.push_back(p.parent_path() / "plugins" / pluginName);
    }
#endif
    candidates.push_back(fs::current_path(ec) / "plugins" / pluginName);
    candidates.push_back(fs::current_path(ec) / "exec" / "plugins" / pluginName);
    candidates.push_back(
        fs::current_path(ec) / "agent" / "build" / "linux-release" / "exec" / "plugins" / pluginName
    );
    candidates.push_back(
        fs::current_path(ec) / "agent" / "build" / "linux-debug" / "exec" / "plugins" / pluginName
    );

    auto hasLibFile = [](const fs::path& dir) {
        std::error_code                     ec2;
        std::filesystem::directory_iterator it(dir, ec2);
        std::filesystem::directory_iterator end;
        for (; it != end; it.increment(ec2)) {
            auto ext = it->path().extension().string();
            if (ext == ".so" || ext == ".dll" || ext == ".dylib") {
                return true;
            }
        }
        return false;
    };

    for (const auto& c : candidates) {
        if (fs::is_directory(c, ec) && hasLibFile(c)) {
            return c.string();
        }
    }
    return "builtin://" + pluginName;
}

/// 查找 libagentxx 共享库绝对路径 (供 dlopen 使用)
inline std::string findSharedLibPath() {
    namespace fs = std::filesystem;
    std::error_code       ec;
    std::vector<fs::path> candidates;

#if XX_IS_WIN_D
    const std::vector<std::string> libNames = {"libagentxx.dll", "agentxx.dll"};
    wchar_t                        buf[MAX_PATH];
    if (::GetModuleFileNameW(nullptr, buf, MAX_PATH) > 0) {
        auto parent = fs::path(buf).parent_path();
        for (const auto& name : libNames) {
            candidates.push_back(parent / name);
        }
    }
#else
    const std::vector<std::string> libNames = {"libagentxx.so", "libagentxxd.so"};
    if (auto p = fs::read_symlink("/proc/self/exe", ec); !ec) {
        auto parent = p.parent_path();
        for (const auto& name : libNames) {
            candidates.push_back(parent / name);
        }
    }
#endif
    auto cwd = fs::current_path(ec);
    for (const auto& name : libNames) {
        candidates.push_back(cwd / name);
        candidates.push_back(cwd / "exec" / name);
        candidates.push_back(cwd / "agent" / "build" / "linux-release" / "exec" / name);
        candidates.push_back(cwd / "agent" / "build" / "linux-debug" / "exec" / name);
    }

    for (const auto& c : candidates) {
        if (fs::exists(c, ec) && !fs::is_directory(c, ec)) {
            return c.string();
        }
    }
    return "";
}

// ---------------------------------------------------------------------------
// 3. 固定消息模板 (user / assistant / tool 交替, 跨模式内容固定)
// ---------------------------------------------------------------------------

inline std::string getFixedToolResultPayload() {
    // 构造固定 3000B 包含中文与 ASCII 的载荷 (使单组约 2000 tokens, 50 组≈100K, 100 组≈200K)
    static const std::string kUnit
        = "RES-BENCH tool result fixed payload line: The quick brown fox jumps over the lazy dog. "
          "资源占用固定载荷，覆盖 unicode 折算分支与 ascii 折算分支。 "
          "Standard test vectors for memory and cpu benchmark verification. "
          "Fixed payload padding to reach deterministic byte length for reproducible measurements. "
          "1234567890!@#$%^&*()_+-=[]{}|;:,.<>?/`~ ";
    std::string out;
    out.reserve(3000);
    while (out.size() + kUnit.size() <= 3000) {
        out += kUnit;
    }
    if (out.size() < 3000) {
        out.append(3000 - out.size(), '=');
    }
    return out;
}

struct FixedGroup {
    neograph::ChatMessage userMsg;
    neograph::ChatMessage assistMsg;
    neograph::ChatMessage toolMsg;

    agent::ViewMessage viewUser;
    agent::ViewMessage viewTool;
    agent::ViewMessage viewAssist;
};

inline FixedGroup makeFixedGroup(size_t index) {
    FixedGroup  g;
    std::string idxStr      = fmt::format("{:06d}", index);
    std::string userContent = fmt::format(
        "RES-BENCH user turn {} | The quick brown fox jumps over the lazy dog. 请列出当前目录并读取 README 前 40 行。 #FIXED-9f3a",
        idxStr
    );
    std::string callId   = fmt::format("call-{}", idxStr);
    std::string toolArgs = "{\"path\":\"README.md\",\"line_offset\":0,\"line_limit\":40}";
    std::string toolContent
        = fmt::format("RES-BENCH tool result {} | {}", idxStr, getFixedToolResultPayload());
    std::string assistSummary = fmt::format(
        "RES-BENCH assist summary {} | 已成功读取 README.md 前 40 行内容，并完成分析任务。",
        idxStr
    );

    // 1. LLM 消息
    g.userMsg.role    = "user";
    g.userMsg.content = userContent;

    g.assistMsg.role    = "assistant";
    g.assistMsg.content = "";
    neograph::ToolCall tc;
    tc.id        = callId;
    tc.name      = "agentxx_filesystem_read";
    tc.arguments = toolArgs;
    g.assistMsg.tool_calls.push_back(std::move(tc));

    g.toolMsg.role         = "tool";
    g.toolMsg.tool_call_id = callId;
    g.toolMsg.tool_name    = "agentxx_filesystem_read";
    g.toolMsg.content      = toolContent;

    // 2. View 消息
    g.viewUser = agent::ViewMessage::makeText(agent::ViewMessage::Role::User, userContent);

    g.viewTool.role = agent::ViewMessage::Role::Tool;
    g.viewTool.text = toolArgs;
    agent::ViewMessage::ToolData td;
    td.toolName          = "agentxx_filesystem_read";
    td.toolCallId        = callId;
    td.toolResult        = toolContent;
    td.toolFinished      = true;
    g.viewTool.tool      = td;
    g.viewTool.collapsed = true;

    g.viewAssist = agent::ViewMessage::makeText(agent::ViewMessage::Role::Assistant, assistSummary);

    return g;
}

// ---------------------------------------------------------------------------
// 4. Token 计算与 100K/200K 组数校准
// ---------------------------------------------------------------------------

inline size_t countLlmTokens(const std::vector<neograph::ChatMessage>& msgs) {
    agentxx::middleware::SummarizationMiddlewareHandle handle(
        std::weak_ptr<agentxx::agent::AgentContext>{}
    );
    return handle.countTokens({}, msgs, false);
}

struct CalibratedCounts {
    size_t n100            = 0;
    size_t n200            = 0;
    size_t actualTokens100 = 0;
    size_t actualTokens200 = 0;
    size_t groupTokens     = 0;
};

inline const CalibratedCounts& getCalibratedCounts() {
    static CalibratedCounts counts = []() {
        CalibratedCounts                   c;
        auto                               g1       = makeFixedGroup(1);
        std::vector<neograph::ChatMessage> oneGroup = {g1.userMsg, g1.assistMsg, g1.toolMsg};
        c.groupTokens                               = countLlmTokens(oneGroup);
        if (c.groupTokens == 0) {
            c.groupTokens = 450; // 防除零
        }

        // 粗算组数
        size_t n1 = 100000 / c.groupTokens;
        size_t n2 = 200000 / c.groupTokens;

        // 微调 n100
        std::vector<neograph::ChatMessage> msgs100;
        msgs100.reserve(n1 * 3);
        for (size_t i = 0; i < n1; ++i) {
            auto g = makeFixedGroup(i + 1);
            msgs100.push_back(std::move(g.userMsg));
            msgs100.push_back(std::move(g.assistMsg));
            msgs100.push_back(std::move(g.toolMsg));
        }
        size_t tok1 = countLlmTokens(msgs100);
        while (tok1 < 98000) {
            ++n1;
            auto g = makeFixedGroup(n1);
            msgs100.push_back(std::move(g.userMsg));
            msgs100.push_back(std::move(g.assistMsg));
            msgs100.push_back(std::move(g.toolMsg));
            tok1 = countLlmTokens(msgs100);
        }
        c.n100            = n1;
        c.actualTokens100 = tok1;

        // 微调 n200
        std::vector<neograph::ChatMessage> msgs200 = msgs100;
        msgs200.reserve(n2 * 3);
        for (size_t i = n1; i < n2; ++i) {
            auto g = makeFixedGroup(i + 1);
            msgs200.push_back(std::move(g.userMsg));
            msgs200.push_back(std::move(g.assistMsg));
            msgs200.push_back(std::move(g.toolMsg));
        }
        size_t tok2 = countLlmTokens(msgs200);
        while (tok2 < 198000) {
            ++n2;
            auto g = makeFixedGroup(n2);
            msgs200.push_back(std::move(g.userMsg));
            msgs200.push_back(std::move(g.assistMsg));
            msgs200.push_back(std::move(g.toolMsg));
            tok2 = countLlmTokens(msgs200);
        }
        c.n200            = n2;
        c.actualTokens200 = tok2;

        return c;
    }();
    return counts;
}

// ---------------------------------------------------------------------------
// 5. 容器字节估算
// ---------------------------------------------------------------------------

inline size_t estimateViewMessagesBytes(const std::vector<agent::ViewMessage>& views) {
    size_t total = 0;
    for (const auto& v : views) {
        total += v.toJson().dump().size();
        total += v.id.size();
    }
    return total;
}

inline size_t estimateLlmMessagesBytes(const neograph::json& llmMsgs) {
    if (!llmMsgs.is_array()) {
        return 0;
    }
    return llmMsgs.dump().size();
}

} // namespace bench
} // namespace agentxx
