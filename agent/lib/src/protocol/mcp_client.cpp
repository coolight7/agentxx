#include "agentxx/protocol/mcp_client.h"

#include "agentxx/util/async_offload.h"
#include "agentxx/util/exception.h"
#include <fmt/format.h>
#include <thread>
#if AGENTXX_ENABLE_BOOST_PROCESS
#include "asio/readable_pipe.hpp"
#include "asio/writable_pipe.hpp"
#include "boost/process.hpp"
#else
#if XX_IS_LINUX_D || XX_IS_MACOS_D
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#elif XX_IS_WIN_D
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#endif

#include "agentxx/util/async_mutex.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include "asio/cancel_after.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/read_until.hpp"
#include "asio/redirect_error.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "asio/write.hpp"
#include <cctype>
#include <iostream>
#include <sstream>

namespace agentxx {
namespace server {

namespace {
/// 计算命名空间前缀后的对外 tool 名称 (namespace 非空时为 "namespace_name")
std::string makeNamespacedName(std::string_view toolNamespace, std::string_view name) {
    return toolNamespace.empty() ? std::string{name} : fmt::format("{}_{}", toolNamespace, name);
}

/// 解析 server/discover 结果
McpClient::DiscoverResult parseDiscoverResult(const json& r) {
    McpClient::DiscoverResult d;
    if (r.contains("supportedVersions") && r["supportedVersions"].is_array()) {
        for (const auto& v : r["supportedVersions"]) {
            if (v.is_string()) {
                d.supportedVersions.push_back(v.get<std::string>());
            }
        }
    }
    if (r.contains("capabilities") && r["capabilities"].is_object()) {
        d.capabilities = r["capabilities"];
    }
    if (r.contains("_meta") && r["_meta"].is_object()) {
        auto serverInfo = r["_meta"].value("io.modelcontextprotocol/serverInfo", json::object());
        if (serverInfo.is_object()) {
            d.serverName    = serverInfo.value("name", std::string{});
            d.serverVersion = serverInfo.value("version", std::string{});
        }
    }
    d.instructions = r.value("instructions", std::string{});
    return d;
}

/// HTTP header 值安全判定: 仅可见 ASCII (0x20-0x7E) 且无首尾空白
bool isPlainAsciiHeaderSafe(std::string_view s) {
    if (s.empty()) {
        return false;
    }
    if (s.front() == ' ' || s.front() == '\t' || s.back() == ' ' || s.back() == '\t') {
        return false;
    }
    for (char c : s) {
        auto uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc > 0x7E) {
            return false;
        }
    }
    return true;
}

/// RFC 9110 tchar (HTTP field-name token 字符)
bool isHttpTokenChar(char c) {
    auto uc = static_cast<unsigned char>(c);
    if ((uc >= 'a' && uc <= 'z') || (uc >= 'A' && uc <= 'Z') || (uc >= '0' && uc <= '9')) {
        return true;
    }
    switch (c) {
        case '!':
        case '#':
        case '$':
        case '%':
        case '&':
        case '\'':
        case '*':
        case '+':
        case '-':
        case '.':
        case '^':
        case '_':
        case '`':
        case '|':
        case '~':
            return true;
        default:
            return false;
    }
}

/// 判断字符串是否为 Base64 sentinel 模式 (=?base64?...?=)
bool isBase64Sentinel(std::string_view s) {
    return s.size() >= 10 && s.starts_with("=?base64?") && s.ends_with("?=");
}

/// 判断 HTTP 响应是否为 "会话过期/失效" 错误 (HTTP 401 + 错误体含 session 过期特征)。
/// 典型格式 (阿里云 FC / ModelScope 网关):
///   {"RequestId":"...","Code":"SessionExpired","Message":"session xxx is expired"}
/// 仅针对明确的 session 过期语义, 避免把普通 401 (如鉴权失败) 误判为会话过期
bool isSessionExpiredResponse(const agentxx::util::HttpResponse& resp) {
    if (resp.status != 401) {
        return false;
    }
    auto lower = agentxx::util::toLower(resp.body);
    return lower.find("sessionexpired") != std::string::npos
           || lower.find("session_expired") != std::string::npos
           || lower.find("session expired") != std::string::npos
           || lower.find("is expired") != std::string::npos;
}

/// 判断 modern probe (server/discover) 失败是否属于 "legacy 服务器正常回退" 特征。
/// legacy / streamable-http 服务器 (如 ModelScope 网关) 不支持 server/discover,
/// 会返回 4xx 协议错误 (如 "No valid session ID provided" / "request without
/// mcp-session-id header should be mcp initialize request") 或 JSON-RPC
/// -32601/-32602, 此时回退到 legacy initialize 握手是**预期流程**, 日志应降为
/// 信息级; 而连接失败/超时/TLS/响应截断等传输层错误属于真正异常, 保留 WARN。
bool isLegacyProbeFailure(std::string_view errmsg) {
    auto lower = agentxx::util::toLower(errmsg);
    // 明确的协议级特征 (旧版服务器对未知 modern 方法/无 session 请求的典型响应)
    static constexpr std::string_view kLegacyTags[] = {
        "no valid session",
        "session id",
        "must be mcp initialize",
        "method not found",
        "-32601",
        "-32602",
        "unsupported protocol version",
        "record not found",
        "http 400",
        "http 401",
        "http 403",
        "http 404",
        "http 405",
        "http 406",
        "http 409",
        "http 410",
    };
    for (auto tag : kLegacyTags) {
        if (lower.find(tag) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}
} // namespace

// ---------------------------------------------------------------------------
// StdioTransport — platform-specific subprocess transport state (PIMPL)
// ---------------------------------------------------------------------------

struct McpClient::StdioTransport {
#if defined(BOOST_PROCESS_V2_PROCESS_HPP)
    std::optional<boost::process::process> process;
    std::optional<asio::writable_pipe>     stdinPipe;
    std::optional<asio::readable_pipe>     stdoutPipe;
    std::atomic<bool>                      running{false};
    /// readerLoop 协程是否已结束 (close 时等待其结束再销毁 pipe, 避免 UAF)
    std::atomic<bool> readerDone{true};

    bool start(
        asio::any_io_executor           executor,
        const std::vector<std::string>& cmd,
        std::shared_ptr<McpClient>      client
    ) {
        stdinPipe.emplace(executor);
        stdoutPipe.emplace(executor);

        auto                     exe = boost::process::environment::find_executable(cmd[0]);
        std::vector<std::string> args(cmd.begin() + 1, cmd.end());

        process.emplace(
            executor,
            exe,
            args,
            boost::process::process_stdio{
                .in  = *stdinPipe,
                .out = *stdoutPipe,
            }
        );

        running.store(true);

        asio::co_spawn(
            executor,
            [this, client]() -> asio::awaitable<void> {
                co_await readerLoop(client);
            },
            asio::detached
        );

        return true;
    }

    asio::awaitable<void> readerLoop(std::shared_ptr<McpClient> client) {
        // 协程退出时标记 readerDone, 供 close() 等待后再销毁 pipe
        struct DoneGuard {
            std::atomic<bool>* d;

            ~DoneGuard() {
                d->store(true, std::memory_order_release);
            }
        } doneGuard{&readerDone};

        readerDone.store(false, std::memory_order_release);

        std::string buffer;
        while (running.load()) {
            neograph_asio_error_code ec;
            std::size_t              n = co_await asio::async_read_until(
                *stdoutPipe,
                asio::dynamic_buffer(buffer, 4096),
                '\n',
                asio::redirect_error(asio::use_awaitable, ec)
            );
            if (ec || n == 0) {
                if (ec && ec != asio::error::eof && ec != asio::error::operation_aborted) {
                    XX_LOGW("[McpClient] stdio reader error: {}", ec.message());
                }
                break;
            }
            std::string line = buffer.substr(0, n - 1);
            buffer.erase(0, n);
            if (line.empty()) {
                continue;
            }
            // 非法 JSON 行直接忽略 (日志警告), 不中断读取循环
            agentxx::util::catchError<bool>(
                [&]() -> bool {
                    auto response = json::parse(line);
                    client->deliverResponse(response);
                    return true;
                },
                [&](std::string) -> bool {
                    XX_LOGW("[McpClient] ignoring malformed stdout line: {}", line.substr(0, 128));
                    return false;
                }
            );
        }
        if (!buffer.empty()) {
            // 尾部残余数据非法 JSON 时静默忽略
            agentxx::util::catchError<bool>(
                [&]() -> bool {
                    auto response = json::parse(buffer);
                    client->deliverResponse(response);
                    return true;
                },
                [](std::string) -> bool {
                    return false;
                }
            );
        }
        running.store(false);
    }

    void close() {
        if (!process.has_value()) {
            return;
        }
        running.store(false);
        neograph_asio_error_code ec;
        if (stdinPipe.has_value()) {
            stdinPipe->close(ec);
        }
        if (stdoutPipe.has_value()) {
            // 关闭 stdoutPipe 使 readerLoop 挂起的 async_read_until 以错误返回并退出
            stdoutPipe->close(ec);
        }
        process->terminate(ec);
        process->wait(ec);
        // 等待 readerLoop 协程结束: 否则其仍挂在 async_read_until 上时销毁 pipe → UAF。
        // (关闭 pipe 后 readerLoop 会很快退出; 此处有界等待, 超时亦继续以免死等)
        for (int i = 0; i < 200 && !readerDone.load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        stdinPipe.reset();
        stdoutPipe.reset();
        process.reset();
    }

    ~StdioTransport() {
        close();
    }

#else
    std::atomic<bool> running{false};
#if XX_IS_LINUX_D || XX_IS_MACOS_D
    int stdinFd  = -1;
    int stdoutFd = -1;
    int childPid = -1;
#elif XX_IS_WIN_D
    HANDLE stdinHandle  = nullptr;
    HANDLE stdoutHandle = nullptr;
    HANDLE childProcess = nullptr;
#endif
    std::thread readerThread;

    bool start(
        asio::any_io_executor /*executor*/,
        const std::vector<std::string>& cmd,
        std::shared_ptr<McpClient>      client
    ) {
#if XX_IS_LINUX_D || XX_IS_MACOS_D
        int stdinPipe[2]  = {-1, -1};
        int stdoutPipe[2] = {-1, -1};

        if (::pipe(stdinPipe) != 0 || ::pipe(stdoutPipe) != 0) {
            return false;
        }

        pid_t pid = ::fork();
        if (pid < 0) {
            ::close(stdinPipe[0]);
            ::close(stdinPipe[1]);
            ::close(stdoutPipe[0]);
            ::close(stdoutPipe[1]);
            return false;
        }

        if (pid == 0) {
            ::close(stdinPipe[1]);
            ::close(stdoutPipe[0]);
            ::dup2(stdinPipe[0], STDIN_FILENO);
            ::dup2(stdoutPipe[1], STDOUT_FILENO);
            int maxFd = static_cast<int>(::sysconf(_SC_OPEN_MAX));
            for (int i = 3; i < maxFd; i++) {
                if (i != stdinPipe[0] && i != stdoutPipe[1]) {
                    ::close(i);
                }
            }
            std::vector<char*> argv;
            for (const auto& arg : cmd) {
                argv.push_back(const_cast<char*>(arg.data()));
            }
            argv.push_back(nullptr);
            ::execvp(argv[0], argv.data());
            ::_exit(127);
        }

        ::close(stdinPipe[0]);
        ::close(stdoutPipe[1]);
        stdinFd  = stdinPipe[1];
        stdoutFd = stdoutPipe[0];
        childPid = static_cast<int>(pid);

        int flags = ::fcntl(stdoutFd, F_GETFL, 0);
        ::fcntl(stdoutFd, F_SETFL, flags | O_NONBLOCK);

        running.store(true);
        readerThread = std::thread([this, client]() {
            readerLoop(client);
        });
        return true;

#elif XX_IS_WIN_D
        SECURITY_ATTRIBUTES sa;
        sa.nLength              = sizeof(SECURITY_ATTRIBUTES);
        sa.lpSecurityDescriptor = nullptr;
        sa.bInheritHandle       = TRUE;

        HANDLE parentStdinRd = nullptr, childStdinWr = nullptr;
        HANDLE childStdoutRd = nullptr, parentStdoutWr = nullptr;

        if (!CreatePipe(&parentStdinRd, &childStdinWr, &sa, 0)
            || !CreatePipe(&childStdoutRd, &parentStdoutWr, &sa, 0)) {
            return false;
        }

        SetHandleInformation(childStdinWr, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(childStdoutRd, HANDLE_FLAG_INHERIT, 0);

        std::string cmdLine;
        for (size_t i = 0; i < cmd.size(); i++) {
            if (i > 0) {
                cmdLine += " ";
            }
            cmdLine += cmd[i];
        }

        PROCESS_INFORMATION pi;
        STARTUPINFOA        si;
        ZeroMemory(&si, sizeof(si));
        si.cb          = sizeof(si);
        si.hStdError   = parentStdoutWr;
        si.hStdOutput  = parentStdoutWr;
        si.hStdInput   = parentStdinRd;
        si.dwFlags    |= STARTF_USESTDHANDLES;

        if (!CreateProcessA(
                nullptr,
                cmdLine.data(),
                nullptr,
                nullptr,
                TRUE,
                0,
                nullptr,
                nullptr,
                &si,
                &pi
            )) {
            CloseHandle(parentStdinRd);
            CloseHandle(childStdinWr);
            CloseHandle(childStdoutRd);
            CloseHandle(parentStdoutWr);
            return false;
        }

        CloseHandle(pi.hThread);
        CloseHandle(parentStdinRd);
        CloseHandle(parentStdoutWr);

        stdinHandle  = childStdinWr;
        stdoutHandle = childStdoutRd;
        childProcess = pi.hProcess;

        running.store(true);
        readerThread = std::thread([this, client]() {
            readerLoop(client);
        });
        return true;
#else
        return false;
#endif
    }

    void readerLoop(std::shared_ptr<McpClient> client) {
#if XX_IS_LINUX_D || XX_IS_MACOS_D
        std::string      buffer;
        constexpr size_t kBufSize = 4096;

        while (running.load()) {
            char    buf[kBufSize];
            ssize_t n = ::read(stdoutFd, buf, kBufSize);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                break;
            }
            if (n == 0) {
                break;
            }

            buffer.append(buf, static_cast<size_t>(n));

            size_t pos;
            while ((pos = buffer.find('\n')) != std::string::npos) {
                std::string line = buffer.substr(0, pos);
                buffer.erase(0, pos + 1);
                if (line.empty()) {
                    continue;
                }
                // 非法 JSON 行直接忽略 (日志警告), 不中断读取循环
                agentxx::util::catchError<bool>(
                    [&]() -> bool {
                        auto response = json::parse(line);
                        client->deliverResponse(response);
                        return true;
                    },
                    [&](std::string) -> bool {
                        XX_LOGW(
                            "[McpClient] ignoring malformed stdout line: {}",
                            line.substr(0, 128)
                        );
                        return false;
                    }
                );
            }
        }
        if (!buffer.empty()) {
            // 尾部残余数据非法 JSON 时静默忽略
            agentxx::util::catchError<bool>(
                [&]() -> bool {
                    auto response = json::parse(buffer);
                    client->deliverResponse(response);
                    return true;
                },
                [](std::string) -> bool {
                    return false;
                }
            );
        }
#elif XX_IS_WIN_D
        std::string buffer;
        char        buf[4096];

        while (running.load()) {
            DWORD bytesRead = 0;
            if (!ReadFile(stdoutHandle, buf, sizeof(buf) - 1, &bytesRead, nullptr)) {
                break;
            }
            if (bytesRead == 0) {
                break;
            }
            buffer.append(buf, static_cast<size_t>(bytesRead));
            size_t pos;
            while ((pos = buffer.find('\n')) != std::string::npos) {
                std::string line = buffer.substr(0, pos);
                buffer.erase(0, pos + 1);
                if (line.empty()) {
                    continue;
                }
                // 非法 JSON 行直接忽略, 不中断读取循环
                agentxx::util::catchError<bool>(
                    [&]() -> bool {
                        auto response = json::parse(line);
                        client->deliverResponse(response);
                        return true;
                    },
                    [](std::string) -> bool {
                        return false;
                    }
                );
            }
        }
#endif
        running.store(false);
    }

    void close() {
#if XX_IS_LINUX_D || XX_IS_MACOS_D
        if (childPid <= 0) {
            return;
        }
        running.store(false);
        ::kill(static_cast<pid_t>(childPid), SIGTERM);
        if (readerThread.joinable()) {
            readerThread.join();
        }
        int status = 0;
        ::waitpid(static_cast<pid_t>(childPid), &status, WNOHANG);
        if (stdinFd >= 0) {
            ::close(stdinFd);
        }
        if (stdoutFd >= 0) {
            ::close(stdoutFd);
        }
        stdinFd  = -1;
        stdoutFd = -1;
        childPid = -1;
#elif XX_IS_WIN_D
        if (childProcess == nullptr) {
            return;
        }
        running.store(false);
        TerminateProcess(childProcess, 0);
        if (readerThread.joinable()) {
            readerThread.join();
        }
        CloseHandle(childProcess);
        CloseHandle(stdinHandle);
        CloseHandle(stdoutHandle);
        childProcess = nullptr;
        stdinHandle  = nullptr;
        stdoutHandle = nullptr;
#endif
    }

    ~StdioTransport() {
        close();
    }
#endif
};

// ---------------------------------------------------------------------------
// McpClient
// ---------------------------------------------------------------------------

McpClient::McpClient(Config config) :
    config_(std::move(config)) {}

McpClient::~McpClient() {
    closeInternal();
}

std::string McpClient::effectiveProtocolVersion() const {
    return negotiatedVersion_.empty() ? config_.protocolVersion : negotiatedVersion_;
}

json McpClient::buildModernMeta() const {
    json meta;
    meta[std::string{agentxx::server::kMetaProtocolVersion}] = effectiveProtocolVersion();
    json info;
    info["name"]                                                = config_.clientName;
    info["version"]                                             = config_.clientVersion;
    meta[std::string{agentxx::server::kMetaClientInfo}]         = std::move(info);
    meta[std::string{agentxx::server::kMetaClientCapabilities}] = json::object();
    return meta;
}

json McpClient::withModernMeta(const json& params) const {
    json p = params;
    if (!p.is_object()) {
        p = json::object();
    }
    json meta = buildModernMeta();
    if (p.contains("_meta") && p["_meta"].is_object()) {
        // 合并用户提供的 _meta (如 progressToken), 协议保留键以协议为准
        for (const auto& item : p["_meta"].items()) {
            if (!meta.contains(item.first)) {
                meta[item.first] = item.second;
            }
        }
    }
    p["_meta"] = std::move(meta);
    return p;
}

std::string
    McpClient::pickMutualVersion(std::string_view requested, const json& serverSupportedVersions) {
    if (!serverSupportedVersions.is_array()) {
        return std::string{requested};
    }
    // 优先: 服务端支持列表中与请求一致
    for (const auto& v : serverSupportedVersions) {
        if (v.is_string() && v.get<std::string>() == requested) {
            return std::string{requested};
        }
    }
    // 其次: 服务端支持且本客户端也支持的最新版本
    for (const auto& sv : serverSupportedVersions) {
        if (!sv.is_string()) {
            continue;
        }
        auto serverVer = sv.get<std::string>();
        for (const auto& my : kSupportedProtocols) {
            if (my == serverVer) {
                return serverVer;
            }
        }
    }
    // 最后: 服务端声称支持的第一个版本 (未知版本时尝试跟随)
    for (const auto& v : serverSupportedVersions) {
        if (v.is_string()) {
            return v.get<std::string>();
        }
    }
    return std::string{requested};
}

asio::awaitable<std::expected<McpClient::InitializeResult, std::string>> McpClient::initialize() {
    if (initialized_.load()) {
        co_return std::unexpected{std::string{"already initialized"}};
    }
    if (config_.isStdio()) {
        auto executor = co_await asio::this_coro::executor;
        if (!startStdioSubprocess(executor)) {
            co_return std::unexpected{std::string{"failed to start subprocess"}};
        }
    }

    // 2026-07-28 (现代) 客户端: 先以 server/discover 探测服务端时代;
    // 旧版协议版本配置则直接走 initialize 握手
    if (config_.protocolVersion == kProtocol2026_07_28) {
        auto disc = co_await probeModern();
        if (disc.has_value()) {
            era_   = ProtocolEra::Modern;
            auto d = std::move(disc.value());
            // 协商最终版本: 优先配置版本, 否则服务端支持列表中双方共有的最新版本
            json versions = json::array();
            for (const auto& v : d.supportedVersions) {
                versions.push_back(v);
            }
            negotiatedVersion_ = pickMutualVersion(config_.protocolVersion, versions);

            InitializeResult info;
            info.protocolVersion = negotiatedVersion_;
            info.capabilities    = d.capabilities;
            info.serverName      = d.serverName;
            info.serverVersion   = d.serverVersion;
            serverInfo_          = info;
            initialized_.store(true);
            XX_LOGI(
                "[McpClient] connected (modern protocol {}): server={} {}",
                negotiatedVersion_,
                d.serverName,
                d.serverVersion
            );
            co_return std::move(info);
        }
        // legacy / streamable-http 服务器不支持 server/discover, probe 失败并
        // 回退到 legacy initialize 握手是**预期流程** (非异常): 协议级 4xx /
        // JSON-RPC 错误视为正常回退, 降为信息级日志; 连接失败/超时/TLS/响应
        // 截断等传输层异常保留 WARN 便于排查
        if (isLegacyProbeFailure(disc.error())) {
            XX_LOGI(
                "[McpClient] server does not support modern protocol probe ({}), "
                "falling back to legacy initialize (expected for legacy servers)",
                disc.error()
            );
        } else {
            XX_LOGW(
                "[McpClient] modern probe failed ({}), falling back to legacy initialize",
                disc.error()
            );
        }
        // 回退: 旧版服务端 (initialize 握手)
        era_               = ProtocolEra::Legacy;
        negotiatedVersion_ = std::string{kProtocol2025_11_25};
    } else {
        // 显式配置旧版协议: 直接 legacy 握手
        era_               = ProtocolEra::Legacy;
        negotiatedVersion_ = config_.protocolVersion;
    }

    // ---- Legacy initialize handshake ----
    json clientInfo;
    clientInfo["name"]    = config_.clientName;
    clientInfo["version"] = config_.clientVersion;

    json params;
    params["protocolVersion"] = negotiatedVersion_;
    params["capabilities"]    = json::object();
    params["clientInfo"]      = std::move(clientInfo);

    auto resp = co_await sendRequest("initialize", std::move(params));
    if (!resp.has_value()) {
        co_return std::unexpected{std::move(resp.error())};
    }

    auto& result = resp.value();
    if (!result.contains("result")) {
        std::string errMsg = "initialize returned no result";
        if (result.contains("error")) {
            errMsg = result["error"].value("message", errMsg);
        }
        co_return std::unexpected{std::move(errMsg)};
    }

    json             r = result["result"];
    InitializeResult info;
    info.protocolVersion = r.value("protocolVersion", std::string{kProtocol2024_11_05});
    if (r.contains("capabilities") && r["capabilities"].is_object()) {
        info.capabilities = r["capabilities"];
    }
    if (r.contains("serverInfo") && r["serverInfo"].is_object()) {
        info.serverName    = r["serverInfo"].value("name", std::string{});
        info.serverVersion = r["serverInfo"].value("version", std::string{});
    }

    auto negotiated      = negotiateProtocolVersion(negotiatedVersion_, r);
    info.protocolVersion = std::move(negotiated);
    negotiatedVersion_   = info.protocolVersion;

    if (!config_.isStdio()) {
        co_await sendRawNotification("notifications/initialized", json::object());
    }

    serverInfo_ = info;
    initialized_.store(true);
    co_return std::move(info);
}

asio::awaitable<std::expected<McpClient::DiscoverResult, std::string>> McpClient::discover() {
    if (closed_.load()) {
        co_return std::unexpected{std::string{"client is closed"}};
    }
    auto result = co_await probeModern();
    if (result.has_value() && era_ == ProtocolEra::Unknown) {
        era_ = ProtocolEra::Modern;
    }
    co_return result;
}

asio::awaitable<std::expected<McpClient::DiscoverResult, std::string>> McpClient::probeModern() {
    if (closed_.load()) {
        co_return std::unexpected{std::string{"client is closed"}};
    }

    // 探测请求使用配置版本 (通常 2026-07-28)
    negotiatedVersion_ = config_.protocolVersion;

    json params;
    params["_meta"] = buildModernMeta();

    // 现代探测请求直接走 raw 发送 (不依赖 era_ 状态)
    auto probe = [this](const json& p) -> asio::awaitable<std::expected<json, std::string>> {
        int64_t id = nextId_.fetch_add(1);
        if (config_.isHttp()) {
            co_return co_await sendModernHttpRequest(id, "server/discover", p);
        }
        co_return co_await sendStdioRequest(id, "server/discover", p);
    };

    auto resp = co_await probe(params);
    if (!resp.has_value()) {
        // HTTP 层失败 (连接/超时): 现代服务端不可能, 视为 legacy 回退
        co_return std::unexpected{std::move(resp.error())};
    }

    json j = resp.value();
    // 现代服务端错误识别
    if (j.contains("error") && j["error"].is_object()) {
        int code = j["error"].value("code", 0);
        if (code == kMcpUnsupportedProtocolVersion) {
            // 现代服务端但版本不受支持: 从 data.supported 挑选共同版本后重试
            json data   = j["error"].contains("data") ? j["error"]["data"] : json::object();
            auto mutual = pickMutualVersion(
                config_.protocolVersion,
                data.value("supported", json::array())
            );
            if (mutual != config_.protocolVersion) {
                negotiatedVersion_ = mutual;
                params["_meta"]    = buildModernMeta();
                auto retry         = co_await probe(params);
                if (retry.has_value() && retry->contains("result")) {
                    co_return parseDiscoverResult(retry->operator[]("result"));
                }
            }
            co_return std::unexpected{fmt::format(
                "unsupported protocol version (server supports: {})",
                data.value("supported", json::array()).dump()
            )};
        }
        // HeaderMismatch / MissingRequiredClientCapability: 现代服务端但请求有问题
        if (code == kMcpHeaderMismatch || code == kMcpMissingRequiredClientCapability) {
            co_return std::unexpected{fmt::format(
                "modern server rejected request: {}",
                j["error"].value("message", "unknown error")
            )};
        }
        // 其他错误 (如 -32601/-32602): legacy 服务端特征 → 回退
        co_return std::unexpected{fmt::format(
            "server responded with error {}: {}",
            code,
            j["error"].value("message", "unknown error")
        )};
    }

    if (!j.contains("result") || !j["result"].is_object()) {
        co_return std::unexpected{std::string{"server/discover returned no result"}};
    }
    co_return parseDiscoverResult(j["result"]);
}

asio::awaitable<void> McpClient::close() {
    closeInternal();
    co_return;
}

bool McpClient::isInitialized() const {
    return initialized_.load();
}

bool McpClient::isClosed() const {
    return closed_.load();
}

const McpClient::InitializeResult& McpClient::serverInfo() const {
    return serverInfo_;
}

const std::string& McpClient::protocolVersion() const {
    return negotiatedVersion_.empty() ? config_.protocolVersion : negotiatedVersion_;
}

asio::awaitable<std::expected<bool, std::string>> McpClient::ping() {
    auto resp = co_await sendRequest("ping", json::object());
    if (!resp.has_value()) {
        co_return std::unexpected{std::move(resp.error())};
    }
    co_return resp.value().contains("result");
}

asio::awaitable<std::expected<std::vector<McpToolDefinition>, std::string>> McpClient::listTools() {
    auto resp = co_await sendRequest("tools/list", json::object());
    if (!resp.has_value()) {
        co_return std::unexpected{std::move(resp.error())};
    }
    json result = resp.value();

    std::vector<McpToolDefinition> tools;
    if (!result.contains("result") || !result["result"].is_object()) {
        co_return tools;
    }

    json r = result["result"];
    if (!r.contains("tools") || !r["tools"].is_array()) {
        co_return tools;
    }

    for (const auto& t : r["tools"]) {
        McpToolDefinition def;
        def.name = t.value("name", std::string{});
        if (def.name.empty()) {
            continue;
        }
        def.description = t.value("description", std::string{});
        def.title       = t.value("title", std::string{});
        if (t.contains("inputSchema") && t["inputSchema"].is_object()) {
            def.inputSchema = t["inputSchema"];
        } else if (t.contains("input_schema") && t["input_schema"].is_object()) {
            def.inputSchema = t["input_schema"];
        }
        if (t.contains("outputSchema") && t["outputSchema"].is_object()) {
            def.outputSchema = t["outputSchema"];
        }
        if (t.contains("annotations") && t["annotations"].is_object()) {
            def.annotations = t["annotations"];
        }
        if (t.contains("execution") && t["execution"].is_object()) {
            def.execution = t["execution"];
        }
        // 2026-07-28: 拒绝 x-mcp-header 注解非法的工具 (HTTP 传输)
        if (config_.isHttp() && !isToolXMcpHeaderValid(def)) {
            XX_LOGW(
                "[McpClient] rejecting tool '{}' with invalid x-mcp-header annotation",
                def.name
            );
            continue;
        }
        cacheToolDefinition(def);
        tools.push_back(std::move(def));
    }
    co_return tools;
}

asio::awaitable<std::expected<json, std::string>>
    McpClient::callTool(std::string_view name, const json& arguments) {
    json params;
    params["name"]      = name;
    params["arguments"] = arguments;
    auto resp           = co_await sendRequest("tools/call", std::move(params));
    if (!resp.has_value()) {
        co_return std::unexpected{std::move(resp.error())};
    }
    json result = resp.value();

    // 2026-07-28: HeaderMismatch (Mcp-Param-* 缺失/不匹配) → 刷新工具缓存后重试一次
    if (era_ == ProtocolEra::Modern && result.contains("error") && result["error"].is_object()
        && result["error"].value("code", 0) == kMcpHeaderMismatch) {
        XX_LOGW(
            "[McpClient] tool call rejected with HeaderMismatch, refreshing tools cache and retrying"
        );
        auto refreshed = co_await listTools();
        if (refreshed.has_value()) {
            params["name"]      = name;
            params["arguments"] = arguments;
            auto retry          = co_await sendRequest("tools/call", std::move(params));
            if (retry.has_value()) {
                result = std::move(retry.value());
            }
        }
    }

    if (result.contains("error")) {
        co_return result;
    }

    if (!result.contains("result")) {
        co_return json::object();
    }

    json r = result["result"];
    if (!r.contains("content") && !r.contains("structuredContent")) {
        co_return r;
    }

    co_return r;
}

asio::awaitable<std::expected<std::vector<McpResourceDefinition>, std::string>>
    McpClient::listResources() {
    auto resp = co_await sendRequest("resources/list", json::object());
    if (!resp.has_value()) {
        co_return std::unexpected{std::move(resp.error())};
    }

    std::vector<McpResourceDefinition> resources;
    if (!resp.value().contains("result")) {
        co_return resources;
    }

    json r = resp.value()["result"];
    if (!r.contains("resources") || !r["resources"].is_array()) {
        co_return resources;
    }

    for (const auto& res : r["resources"]) {
        McpResourceDefinition def;
        def.uri = res.value("uri", std::string{});
        if (def.uri.empty()) {
            continue;
        }
        def.name        = res.value("name", std::string{});
        def.description = res.value("description", std::string{});
        def.mimeType    = res.value("mimeType", std::string{});
        resources.push_back(std::move(def));
    }
    co_return resources;
}

asio::awaitable<std::expected<McpResourceContent, std::string>>
    McpClient::readResource(std::string_view uri) {
    json params;
    params["uri"] = uri;
    auto resp     = co_await sendRequest("resources/read", std::move(params));
    if (!resp.has_value()) {
        co_return std::unexpected{std::move(resp.error())};
    }

    json result = resp.value();
    if (!result.contains("result")) {
        co_return std::unexpected{std::string{"no result in response"}};
    }

    json               r = result["result"];
    McpResourceContent content;
    content.uri = uri;
    if (r.contains("contents") && r["contents"].is_array() && !r["contents"].empty()) {
        json c           = r["contents"][0];
        content.uri      = c.value("uri", std::string{uri});
        content.mimeType = c.value("mimeType", std::string{});
        if (c.contains("text")) {
            content.text = c["text"].get<std::string>();
        } else if (c.contains("blob")) {
            content.text = c["blob"].get<std::string>();
        }
    }
    co_return content;
}

asio::awaitable<std::expected<std::vector<McpPromptDefinition>, std::string>>
    McpClient::listPrompts() {
    auto resp = co_await sendRequest("prompts/list", json::object());
    if (!resp.has_value()) {
        co_return std::unexpected{std::move(resp.error())};
    }

    std::vector<McpPromptDefinition> prompts;
    if (!resp.value().contains("result")) {
        co_return prompts;
    }

    json r = resp.value()["result"];
    if (!r.contains("prompts") || !r["prompts"].is_array()) {
        co_return prompts;
    }

    for (const auto& p : r["prompts"]) {
        McpPromptDefinition def;
        def.name = p.value("name", std::string{});
        if (def.name.empty()) {
            continue;
        }
        def.description = p.value("description", std::string{});
        if (p.contains("arguments") && p["arguments"].is_array()) {
            for (const auto& a : p["arguments"]) {
                McpPromptArgument arg;
                arg.name        = a.value("name", std::string{});
                arg.description = a.value("description", std::string{});
                arg.required    = a.value("required", false);
                def.arguments.push_back(std::move(arg));
            }
        }
        prompts.push_back(std::move(def));
    }
    co_return prompts;
}

asio::awaitable<std::expected<McpPromptResult, std::string>>
    McpClient::getPrompt(std::string_view name, const json& arguments) {
    json params;
    params["name"]      = name;
    params["arguments"] = arguments;
    auto resp           = co_await sendRequest("prompts/get", std::move(params));
    if (!resp.has_value()) {
        co_return std::unexpected{std::move(resp.error())};
    }

    json result = resp.value();
    if (!result.contains("result")) {
        co_return std::unexpected{std::string{"no result in response"}};
    }

    json            r = result["result"];
    McpPromptResult pr;
    pr.description = r.value("description", std::string{});
    if (r.contains("messages") && r["messages"].is_array()) {
        for (const auto& m : r["messages"]) {
            McpPromptMessage msg;
            msg.role = m.value("role", std::string{"user"});
            if (m.contains("content")) {
                msg.content = m["content"];
            }
            pr.messages.push_back(std::move(msg));
        }
    }
    co_return pr;
}

std::vector<std::unique_ptr<agentxx::tools::XXToolBase>>
    McpClient::createTools(std::weak_ptr<agentxx::agent::AgentContext> ctx) {
    std::vector<std::unique_ptr<agentxx::tools::XXToolBase>> tools;
    auto                                                     self = shared_from_this();
    return tools;
}

std::unique_ptr<McpClientTool>
    McpClient::createTool(McpToolDefinition def, std::weak_ptr<agentxx::agent::AgentContext> ctx) {
    return std::make_unique<McpClientTool>(
        shared_from_this(),
        std::move(def),
        std::move(ctx),
        config_.toolNamespace
    );
}

json McpClient::makeRequest(int64_t id, std::string_view method, const json& params) {
    json req;
    req["jsonrpc"] = "2.0";
    req["id"]      = id;
    req["method"]  = method;
    req["params"]  = params;
    return req;
}

std::optional<std::string> McpClient::getErrorFromResponse(const json& response) {
    if (response.contains("error") && response["error"].is_object()) {
        json        err  = response["error"];
        int         code = err.value("code", 0);
        std::string msg  = err.value("message", "unknown error");
        if (err.contains("data") && !err["data"].is_null()) {
            msg += fmt::format(" (data: {})", err["data"].dump());
        }
        return fmt::format("JSON-RPC error {}: {}", code, msg);
    }
    return std::nullopt;
}

std::string
    McpClient::negotiateProtocolVersion(std::string_view requested, const json& serverResult) {
    auto serverVersion = serverResult.value("protocolVersion", std::string{});
    if (serverVersion.empty()) {
        return std::string(kProtocol2024_11_05);
    }

    if (serverVersion == requested) {
        return serverVersion;
    }

    for (const auto& sv : kSupportedProtocols) {
        if (sv == serverVersion) {
            return serverVersion;
        }
    }

    if (serverVersion.find("2024") != std::string::npos
        || serverVersion.find("2025") != std::string::npos) {
        return serverVersion;
    }

    return std::string{requested};
}

asio::awaitable<std::expected<json, std::string>>
    McpClient::sendRequest(std::string_view method, const json& params) {
    if (closed_.load()) {
        co_return std::unexpected{std::string{"client is closed"}};
    }

    int64_t id = nextId_.fetch_add(1);

    // 2026-07-28 现代模式: 请求带 _meta + (HTTP) 标准请求头
    if (era_ == ProtocolEra::Modern) {
        auto modernParams = withModernMeta(params);
        if (config_.isHttp()) {
            auto result = co_await sendModernHttpRequest(id, method, modernParams);
            co_return std::move(result);
        } else if (config_.isStdio()) {
            auto result = co_await sendStdioRequest(id, method, modernParams);
            co_return std::move(result);
        }
        co_return std::unexpected{std::string{"no transport configured"}};
    }

    // Legacy 或 era 未知 (初始化探测阶段): 原逻辑
    if (config_.isHttp()) {
        auto result = co_await sendHttpRequest(id, method, params);
        co_return std::move(result);
    } else if (config_.isStdio()) {
        auto result = co_await sendStdioRequest(id, method, params);
        co_return std::move(result);
    }
    co_return std::unexpected{std::string{"no transport configured"}};
}

std::string McpClient::buildSseUrl(std::string_view serverUrl) {
    auto url = std::string(serverUrl);
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    return fmt::format("{}/sse", url);
}

std::vector<McpClient::SseEvent> McpClient::parseSseEvents(std::string_view body) {
    std::vector<SseEvent> events;
    std::string           curEvent;
    std::string           curData;

    auto stream = std::stringstream{};
    stream << body;
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.empty()) {
            if (!curEvent.empty() || !curData.empty()) {
                events.push_back({std::move(curEvent), std::move(curData)});
                curEvent.clear();
                curData.clear();
            }
        } else if (line.starts_with("event: ")) {
            curEvent = line.substr(7);
        } else if (line.starts_with("data: ")) {
            if (!curData.empty()) {
                curData += "\n";
            }
            curData += line.substr(6);
        }
    }
    if (!curEvent.empty() || !curData.empty()) {
        events.push_back({std::move(curEvent), std::move(curData)});
    }

    return events;
}

std::string McpClient::getQueryParam(std::string_view query, std::string_view key) {
    auto pos = query.find(key);
    if (pos == std::string_view::npos) {
        return {};
    }
    pos += key.size();
    if (pos >= query.size() || query[pos] != '=') {
        return {};
    }
    ++pos;
    auto end = query.find('&', pos);
    if (end == std::string_view::npos) {
        end = query.size();
    }
    return std::string(query.substr(pos, end - pos));
}

asio::awaitable<void> McpClient::discoverSseEndpoint() {
    if (sseDiscovered_.load()) {
        co_return;
    }

    std::string sseUrl = buildSseUrl(config_.serverUrl);

    auto headers = util::HeaderMap{};
    headers.set("Accept", "text/event-stream");

    std::string sseBody;
    try {
        co_await util::HttpClient::requestSseAsync(
            "GET",
            sseUrl,
            "",
            "",
            headers,
            util::HttpClient::RequestConfig{
                .readChunkTimeout = std::min(config_.initTimeout, std::chrono::milliseconds(800))
            },
            [&](std::string_view chunk) -> bool {
                sseBody.append(chunk);
                if (sseBody.find("\n\n") != std::string::npos) {
                    return true; // 收到初始 endpoint 事件后立即关闭连接
                }
                return false;
            }
        );
    } catch (...) {
        httpMessageUrl_ = config_.serverUrl;
        sseDiscovered_.store(true);
        co_return;
    }

    auto events = parseSseEvents(sseBody);
    for (const auto& ev : events) {
        if (ev.event == "endpoint") {
            std::string path = ev.data;
            auto [base, _]   = util::HttpClient::splitUrl(config_.serverUrl);

            auto qpos = path.find('?');
            if (qpos != std::string::npos) {
                auto query = path.substr(qpos + 1);
                auto sid   = getQueryParam(query, "sessionId");
                if (sid.empty()) {
                    sid = getQueryParam(query, "session_id");
                }
                if (sid.empty()) {
                    sid = getQueryParam(query, "mcp-session-id");
                }
                if (!sid.empty()) {
                    mcpSessionId_ = sid;
                }
            }

            httpMessageUrl_ = base + path;
            sseDiscovered_.store(true);
            XX_LOGI(
                "[McpClient] discovered message endpoint: {} (session={})",
                httpMessageUrl_,
                mcpSessionId_
            );
            co_return;
        }
    }

    httpMessageUrl_ = config_.serverUrl;
    sseDiscovered_.store(true);
    co_return;
}

util::HeaderMap McpClient::buildHttpHeaders() const {
    auto headers = config_.extraHeaders;
    if (config_.protocolVersion == kProtocol2025_11_25) {
        if (!headers.contains("MCP-Protocol-Version")) {
            headers.set("MCP-Protocol-Version", std::string{kProtocol2025_11_25});
        }
    }
    headers.set("Accept", "application/json, text/event-stream");
    if (!mcpSessionId_.empty()) {
        headers.set("Mcp-Session-Id", mcpSessionId_);
    }
    return headers;
}

std::string McpClient::encodeMcpHeaderValue(const json& value) {
    std::string s;
    if (value.is_string()) {
        s = value.get<std::string>();
    } else if (value.is_boolean()) {
        s = value.get<bool>() ? "true" : "false";
    } else if (value.is_number_integer()) {
        s = std::to_string(value.get<int64_t>());
    } else if (value.is_number_unsigned()) {
        s = std::to_string(value.get<uint64_t>());
    } else if (value.is_number_float()) {
        s = fmt::format("{}", value.get<double>());
    }
    // 非安全字符或与 sentinel 模式冲突时使用 Base64 编码
    if (!isPlainAsciiHeaderSafe(s) || isBase64Sentinel(s)) {
        return fmt::format("=?base64?{}?=", agentxx::util::base64Encode(s));
    }
    return s;
}

std::string McpClient::decodeMcpHeaderValue(std::string_view value) {
    if (isBase64Sentinel(value)) {
        auto inner = value.substr(9, value.size() - 11); // 去掉 =?base64? 与 ?=
        auto dec   = agentxx::util::base64Decode(inner);
        if (dec.has_value()) {
            return std::move(*dec);
        }
    }
    return std::string{value};
}

std::unordered_map<std::string, McpClient::XMcpHeaderInfo>
    McpClient::extractXMcpHeaders(const McpToolDefinition& def) {
    std::unordered_map<std::string, XMcpHeaderInfo> result; // headerName(小写) -> 信息
    if (!def.inputSchema.is_object()) {
        return result;
    }
    auto props = def.inputSchema.find("properties");
    if (props == def.inputSchema.end() || !(*props).is_object()) {
        return result;
    }
    for (const auto& item : (*props).items()) {
        const auto& param = item.first;
        const auto& pdef  = item.second;
        if (!pdef.is_object()) {
            continue;
        }
        auto it = pdef.find("x-mcp-header");
        if (it != pdef.end() && (*it).is_string()) {
            auto        headerName = (*it).get<std::string>();
            std::string lower;
            lower.reserve(headerName.size());
            for (char c : headerName) {
                lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
            result[lower] = XMcpHeaderInfo{headerName, param};
        }
    }
    return result;
}

bool McpClient::isToolXMcpHeaderValid(const McpToolDefinition& def) {
    if (!def.inputSchema.is_object()) {
        return true;
    }
    auto props = def.inputSchema.find("properties");
    if (props == def.inputSchema.end() || !(*props).is_object()) {
        return true;
    }
    std::unordered_set<std::string> seen;
    for (const auto& item : (*props).items()) {
        const auto& pdef = item.second;
        if (!pdef.is_object()) {
            continue;
        }
        auto it = pdef.find("x-mcp-header");
        if (it == pdef.end() || !(*it).is_string()) {
            continue;
        }
        auto headerName = (*it).get<std::string>();
        // 非空 + tchar token 语法
        if (headerName.empty()) {
            return false;
        }
        for (char c : headerName) {
            if (!isHttpTokenChar(c)) {
                return false;
            }
        }
        // 大小写不敏感唯一
        std::string lower;
        lower.reserve(headerName.size());
        for (char c : headerName) {
            lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        if (!seen.insert(lower).second) {
            return false;
        }
        // 仅允许原始类型 (integer/string/boolean)
        auto type = pdef.find("type");
        if (type != pdef.end() && (*type).is_string()) {
            auto t = (*type).get<std::string>();
            if (t != "string" && t != "integer" && t != "boolean") {
                return false;
            }
        }
    }
    return true;
}

void McpClient::cacheToolDefinition(const McpToolDefinition& def) {
    std::lock_guard lock(toolsCacheMutex_);
    toolsCache_[def.name] = def;
}

std::optional<McpToolDefinition> McpClient::cachedTool(std::string_view name) const {
    std::lock_guard lock(toolsCacheMutex_);
    auto            it = toolsCache_.find(std::string{name});
    if (it == toolsCache_.end()) {
        return std::nullopt;
    }
    return it->second;
}

util::HeaderMap
    McpClient::buildModernHttpHeaders(std::string_view method, const json& params) const {
    auto headers = config_.extraHeaders;
    headers.set("MCP-Protocol-Version", effectiveProtocolVersion());
    headers.set("Mcp-Method", std::string{method});
    headers.set("Accept", "application/json, text/event-stream");

    // Mcp-Name: tools/call / resources/read / prompts/get 必需
    if (method == "tools/call" || method == "resources/read" || method == "prompts/get") {
        std::string name;
        if (method == "resources/read") {
            name = params.value("uri", std::string{});
        } else {
            name = params.value("name", std::string{});
        }
        // 注意: 必须用圆括号 json(name) 构造字符串 JSON, 花括号 json{name} 会
        // 匹配 initializer_list 构造函数 → 构造数组 [name], is_string() 为 false,
        // 导致 header 值被错误地 Base64 编码成空串 (=?base64??=)
        headers.set("Mcp-Name", encodeMcpHeaderValue(json(name)));
    }

    // Mcp-Param-*: tools/call 中工具 schema 声明的 x-mcp-header 参数
    if (method == "tools/call") {
        auto toolName = params.value("name", std::string{});
        auto def      = cachedTool(toolName);
        if (def.has_value()) {
            auto annotated = extractXMcpHeaders(*def);
            if (!annotated.empty()) {
                json args = params.contains("arguments") && params["arguments"].is_object()
                                ? params["arguments"]
                                : json::object();
                for (const auto& [headerLower, info] : annotated) {
                    if (!args.contains(info.param) || args[info.param].is_null()) {
                        continue; // 无值 → 省略 header
                    }
                    headers.set(
                        fmt::format("Mcp-Param-{}", info.headerName),
                        encodeMcpHeaderValue(args[info.param])
                    );
                }
            }
        }
    }
    return headers;
}

asio::awaitable<std::expected<json, std::string>>
    McpClient::sendModernHttpRequest(int64_t id, std::string_view method, const json& params) {
    // 2026-07-28: 直接 POST serverUrl, 无 SSE discovery、无会话头
    auto req     = makeRequest(id, method, params);
    auto headers = buildModernHttpHeaders(method, params);

    auto doPost
        = [&]() -> asio::awaitable<std::expected<agentxx::util::HttpResponse, std::string>> {
        co_return co_await util::HttpClient::postAsync(
            config_.serverUrl,
            req,
            headers,
            util::HttpClient::RequestConfig{.readChunkTimeout = config_.requestTimeout}
        );
    };

    auto resp = co_await doPost();
    if (!resp.has_value()) {
        // 瞬时传输错误 (响应被截断/连接重置/超时) 自动重试, 语义同 sendHttpRequest
        for (int attempt = 0;
             !resp.has_value() && util::HttpClient::isTransientError(resp.error()) && attempt < 2;
             ++attempt) {
            XX_LOGW(
                "[McpClient] transient HTTP error on {} ({}), retrying ({}/2)",
                method,
                resp.error(),
                attempt + 1
            );
            resp = co_await doPost();
        }
        if (!resp.has_value()) {
            co_return std::unexpected{std::move(resp.error())};
        }
    }
    auto& httpResp = resp.value();

    // 非 2xx: 解析 JSON-RPC 错误 (现代服务端错误特征)
    if (httpResp.status / 100 != 2) {
        auto bodyJson = httpResp.bodyJson();
        if (bodyJson.has_value() && bodyJson->contains("error")
            && (*bodyJson)["error"].is_object()) {
            int code = (*bodyJson)["error"].value("code", 0);
            if (code == kMcpHeaderMismatch || code == kMcpMissingRequiredClientCapability
                || code == kMcpUnsupportedProtocolVersion) {
                co_return std::move(bodyJson.value());
            }
        }
        co_return std::unexpected{
            fmt::format("HTTP {}: {}", httpResp.status, httpResp.body.substr(0, 256))
        };
    }

    auto ct = httpResp.contentType();
    if (httpResp.isTextContentType(ct) && ct.find("event-stream") != std::string::npos) {
        // SSE 流响应: 找匹配 id 的 message 事件
        auto events = parseSseEvents(httpResp.body);
        for (const auto& ev : events) {
            if (ev.event == "message" || ev.event.empty()) {
                bool matched = false;
                json resultJson;
                co_await agentxx::util::catchErrorAsync<bool>(
                    [&]() -> asio::awaitable<bool> {
                        json j = json::parse(ev.data);
                        if (j.contains("id") && !j["id"].is_null()) {
                            auto respId  = j["id"];
                            bool idMatch = false;
                            if (respId.is_number_integer()) {
                                idMatch = respId.get<int64_t>() == id;
                            } else if (respId.is_string()) {
                                idMatch = respId.get<std::string>() == std::to_string(id);
                            }
                            if (idMatch) {
                                matched    = true;
                                resultJson = std::move(j);
                            }
                        }
                        co_return true;
                    },
                    [&](std::string) -> asio::awaitable<bool> {
                        XX_LOGW(
                            "[McpClient] ignoring malformed SSE data: {}",
                            ev.data.substr(0, 128)
                        );
                        co_return false;
                    }
                );
                if (matched) {
                    co_return resultJson;
                }
            }
        }
        co_return std::unexpected{fmt::format("no matching response in SSE stream for id {}", id)};
    }

    auto bodyJson = httpResp.bodyJson();
    if (!bodyJson.has_value()) {
        co_return std::unexpected{
            fmt::format("invalid JSON response: {}", httpResp.body.substr(0, 256))
        };
    }

    co_return std::move(bodyJson.value());
}

asio::awaitable<std::expected<json, std::string>>
    McpClient::sendHttpRequest(int64_t id, std::string_view method, const json& params) {
    if (!sseDiscovered_.load()) {
        co_await discoverSseEndpoint();
    }

    auto req = makeRequest(id, method, params);

    auto doPost
        = [&]() -> asio::awaitable<std::expected<agentxx::util::HttpResponse, std::string>> {
        // headers 每次请求时重建: 反映最新的 mcpSessionId_ (会话重建后复用本 lambda)
        auto hdrs = buildHttpHeaders();
        co_return co_await util::HttpClient::postAsync(
            httpMessageUrl_,
            req,
            hdrs,
            util::HttpClient::RequestConfig{.readChunkTimeout = config_.requestTimeout}
        );
    };

    // ---- 传输层瞬时错误 (响应被截断/连接重置/超时): 自动重试, 最多 2 次额外尝试 ----
    auto resp = co_await doPost();
    for (int attempt = 0;
         !resp.has_value() && util::HttpClient::isTransientError(resp.error()) && attempt < 2;
         ++attempt) {
        XX_LOGW(
            "[McpClient] transient HTTP error on {} ({}), retrying ({}/2)",
            method,
            resp.error(),
            attempt + 1
        );
        resp = co_await doPost();
    }
    if (!resp.has_value()) {
        co_return std::unexpected{std::move(resp.error())};
    }

    // ---- HTTP 401 + session 过期特征 (如 ModelScope 网关 SessionExpired):
    //      自动重建会话 (重新 initialize + notifications/initialized) 后重试一次。
    //      每个请求最多重建一次, 避免会话反复失效时无限递归。 ----
    if (isSessionExpiredResponse(resp.value())) {
        XX_LOGW(
            "[McpClient] HTTP 401 session expired on {}, rebuilding session and retrying once",
            method
        );
        if (co_await rebuildHttpSession()) {
            resp = co_await doPost();
            if (!resp.has_value() && util::HttpClient::isTransientError(resp.error())) {
                resp = co_await doPost();
            }
            if (!resp.has_value()) {
                co_return std::unexpected{std::move(resp.error())};
            }
        }
    }
    auto& httpResp = resp.value();

    if (mcpSessionId_.empty()) {
        auto sid = httpResp.findHeader("mcp-session-id");
        if (!sid.empty()) {
            mcpSessionId_ = sid;
        }
    }

    if (httpResp.status / 100 != 2) {
        co_return std::unexpected{
            fmt::format("HTTP {}: {}", httpResp.status, httpResp.body.substr(0, 256))
        };
    }

    auto ct = httpResp.contentType();
    if (httpResp.isTextContentType(ct) && ct.find("event-stream") != std::string::npos) {
        auto events = parseSseEvents(httpResp.body);
        for (const auto& ev : events) {
            if (ev.event == "message" || ev.event.empty()) {
                bool matched = false;
                json resultJson;
                // 非法 SSE data 记录日志并跳过, 不中断整个流的处理
                co_await agentxx::util::catchErrorAsync<bool>(
                    [&]() -> asio::awaitable<bool> {
                        json j = json::parse(ev.data);
                        if (j.contains("id") && !j["id"].is_null()) {
                            auto respId  = j["id"];
                            bool idMatch = false;
                            if (respId.is_number_integer()) {
                                idMatch = respId.get<int64_t>() == id;
                            } else if (respId.is_string()) {
                                idMatch = respId.get<std::string>() == std::to_string(id);
                            }
                            if (idMatch) {
                                matched    = true;
                                resultJson = std::move(j);
                            }
                        }
                        co_return true;
                    },
                    [&](std::string) -> asio::awaitable<bool> {
                        XX_LOGW(
                            "[McpClient] ignoring malformed SSE data: {}",
                            ev.data.substr(0, 128)
                        );
                        co_return false;
                    }
                );
                if (matched) {
                    co_return resultJson;
                }
            }
        }
        co_return std::unexpected{fmt::format("no matching response in SSE stream for id {}", id)};
    }

    auto bodyJson = httpResp.bodyJson();
    if (!bodyJson.has_value()) {
        co_return std::unexpected{
            fmt::format("invalid JSON response: {}", httpResp.body.substr(0, 256))
        };
    }

    json j   = bodyJson.value();
    auto err = getErrorFromResponse(j);
    if (err.has_value()) {
        co_return j;
    }

    if (j.contains("id") && !j["id"].is_null()) {
        auto respId  = j["id"];
        bool idMatch = false;
        if (respId.is_number_integer()) {
            idMatch = respId.get<int64_t>() == id;
        } else if (respId.is_string()) {
            idMatch = respId.get<std::string>() == std::to_string(id);
        }

        if (!idMatch) {
            XX_LOGW("[McpClient] response id mismatch: sent={}, got={}", id, respId.dump());
        }
    }

    co_return j;
}

asio::awaitable<bool> McpClient::rebuildHttpSession() {
    // 清空旧 session: 无 Mcp-Session-Id 的请求被服务器视为首次 initialize
    mcpSessionId_.clear();

    json params;
    params["protocolVersion"]
        = negotiatedVersion_.empty() ? std::string{kProtocol2025_11_25} : negotiatedVersion_;
    params["capabilities"] = json::object();
    json info;
    info["name"]         = config_.clientName;
    info["version"]      = config_.clientVersion;
    params["clientInfo"] = std::move(info);

    auto req     = makeRequest(nextId_.fetch_add(1), "initialize", std::move(params));
    auto headers = buildHttpHeaders(); // mcpSessionId_ 已清空 → 无 session header
    auto resp    = co_await util::HttpClient::postAsync(
        httpMessageUrl_,
        req,
        headers,
        util::HttpClient::RequestConfig{.readChunkTimeout = config_.initTimeout}
    );
    if (!resp.has_value()) {
        XX_LOGW("[McpClient] rebuild session failed: {}", resp.error());
        co_return false;
    }
    auto& r = resp.value();
    if (r.status / 100 != 2) {
        XX_LOGW("[McpClient] rebuild session failed: HTTP {}", r.status);
        co_return false;
    }
    auto sid = r.findHeader("mcp-session-id");
    if (sid.empty()) {
        XX_LOGW("[McpClient] rebuild session failed: no Mcp-Session-Id in response");
        co_return false;
    }
    mcpSessionId_ = std::string{sid};
    // 部分服务器 (如 ModelScope 网关) 要求 initialize 后补发
    // notifications/initialized 才算完成握手, 否则后续请求返回错误
    co_await sendRawNotification("notifications/initialized", json::object());
    XX_LOGI("[McpClient] session rebuilt: {}", mcpSessionId_);
    co_return true;
}

asio::awaitable<std::expected<json, std::string>>
    McpClient::sendStdioRequest(int64_t id, std::string_view method, const json& params) {
    auto req    = makeRequest(id, method, params);
    auto reqStr = fmt::format("{}\n", req.dump());

    auto promise = std::make_shared<PendingRequest>();
    auto future  = promise->promise.get_future();

    {
        std::lock_guard lock(pendingMutex_);
        pending_[id] = std::move(promise);
    }

#if defined(BOOST_PROCESS_V2_PROCESS_HPP)
    {
        auto                     wguard = co_await stdioWriteMutex_->lock();
        neograph_asio_error_code wec;
        co_await asio::async_write(
            *stdio_->stdinPipe,
            asio::buffer(reqStr),
            asio::redirect_error(asio::use_awaitable, wec)
        );
        if (wec) {
            std::lock_guard lock2(pendingMutex_);
            pending_.erase(id);
            co_return std::unexpected{
                fmt::format("write to subprocess stdin failed: {}", wec.message())
            };
        }
    }
#else
    {
        auto wguard = co_await stdioWriteMutex_->lock();
#if XX_IS_LINUX_D || XX_IS_MACOS_D
        const char* buf       = reqStr.data();
        size_t      remaining = reqStr.size();
        while (remaining > 0) {
            ssize_t n = ::write(stdio_->stdinFd, buf, remaining);
            if (n <= 0) {
                break;
            }
            buf       += n;
            remaining -= static_cast<size_t>(n);
        }
#elif XX_IS_WIN_D
        DWORD written = 0;
        WriteFile(
            stdio_->stdinHandle,
            reqStr.data(),
            static_cast<DWORD>(reqStr.size()),
            &written,
            nullptr
        );
#endif
    }
#endif

    auto executor = co_await asio::this_coro::executor;
    json response;

    // 带超时的轮询等待: 子进程卡死或不返回对应 id 时, 超过 requestTimeout 即返回错误,
    // 否则该协程将永久挂起 (原实现完全忽略 requestTimeout)
    const auto deadline = std::chrono::steady_clock::now() + config_.requestTimeout;
    bool       ready    = false;
    while (true) {
        if (future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            response = future.get();
            ready    = true;
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        asio::steady_timer timer(executor);
        timer.expires_after(std::chrono::milliseconds(5));
        co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ignoreEc_));
    }

    if (!ready) {
        {
            std::lock_guard lock(pendingMutex_);
            pending_.erase(id);
        }
        co_return std::unexpected{fmt::format(
            "stdio request `{}` timed out after {}ms",
            method,
            config_.requestTimeout.count()
        )};
    }

    auto err = getErrorFromResponse(response);
    if (err.has_value()) {
        co_return response;
    }
    co_return response;
}

asio::awaitable<std::expected<void, std::string>> McpClient::listen(
    const SubscriptionFilter&                     filter,
    std::function<void(const json& notification)> onNotification,
    std::function<void()>                         onEnded
) {
    if (closed_.load()) {
        co_return std::unexpected{std::string{"client is closed"}};
    }
    if (era_ != ProtocolEra::Modern) {
        // 2026-07-28 特性; legacy 服务端请用 resources/subscribe
        co_return std::unexpected{
            std::string{"subscriptions/listen requires protocol version 2026-07-28"}
        };
    }

    int64_t id       = nextId_.fetch_add(1);
    listenRequestId_ = id;

    json params;
    params["_meta"] = buildModernMeta();
    json notifications;
    if (filter.toolsListChanged) {
        notifications["toolsListChanged"] = true;
    }
    if (filter.promptsListChanged) {
        notifications["promptsListChanged"] = true;
    }
    if (filter.resourcesListChanged) {
        notifications["resourcesListChanged"] = true;
    }
    if (!filter.resourceSubscriptions.empty()) {
        json uris = json::array();
        for (const auto& u : filter.resourceSubscriptions) {
            uris.push_back(u);
        }
        notifications["resourceSubscriptions"] = std::move(uris);
    }
    params["notifications"] = std::move(notifications);

    auto req     = makeRequest(id, "subscriptions/listen", params);
    auto headers = buildModernHttpHeaders("subscriptions/listen", params);

    {
        std::lock_guard lock(notifyMutex_);
        notificationHandler_ = std::move(onNotification);
        subscriptionEnded_   = std::move(onEnded);
    }

    if (config_.isStdio()) {
        // stdio: 写请求后立即返回 (服务端仅在优雅结束时才回响应, 不能按普通
        // 请求等待 requestTimeout); ack/通知经 deliverResponse 分发 (读取线程)
        auto reqStr = fmt::format("{}\n", req.dump());
        bool ok     = co_await writeStdioLine(reqStr);
        if (!ok) {
            co_return std::unexpected{
                std::string{"failed to write subscriptions/listen to subprocess"}
            };
        }
        std::expected<void, std::string> okResult;
        co_return okResult;
    }

    if (!config_.isHttp()) {
        co_return std::unexpected{std::string{"no transport configured"}};
    }

    // HTTP: 驱动长连接 SSE 流, 直到服务端优雅结束或外部取消
    std::string       sseBuffer;
    std::atomic<bool> finished{false};

    auto flushEvents = [&](std::string_view block) -> bool {
        auto events = parseSseEvents(block);
        for (const auto& ev : events) {
            if (ev.event != "message" && !ev.event.empty()) {
                continue; // 忽略非 message 事件
            }
            json j;
            if (!agentxx::util::catchError<bool>(
                    [&]() -> bool {
                        j = json::parse(ev.data);
                        return true;
                    },
                    [](std::string) -> bool {
                        return false;
                    }
                )) {
                continue; // 非法 JSON (如 keepalive 注释) 忽略
            }
            if (j.contains("id") && !j["id"].is_null()) {
                // 服务端优雅结束: 回发的空 result
                bool idMatch = false;
                if (j["id"].is_number_integer()) {
                    idMatch = j["id"].get<int64_t>() == id;
                } else if (j["id"].is_string()) {
                    idMatch = j["id"].get<std::string>() == std::to_string(id);
                }
                if (idMatch) {
                    finished.store(true);
                    return true; // 结束流
                }
                continue;
            }
            // 通知 (ack / list_changed / resources/updated)
            std::function<void(const json&)> handler;
            {
                std::lock_guard lock(notifyMutex_);
                handler = notificationHandler_;
            }
            if (handler) {
                handler(j);
            }
        }
        return false;
    };

    auto result = co_await agentxx::util::catchErrorAsync<std::expected<void, std::string>>(
        [&]() -> asio::awaitable<std::expected<void, std::string>> {
            co_await util::HttpClient::requestSseAsync(
                "POST",
                config_.serverUrl,
                req.dump(),
                "application/json",
                headers,
                util::HttpClient::RequestConfig{.readChunkTimeout = config_.requestTimeout},
                [&](std::string_view chunk) -> bool {
                    sseBuffer.append(chunk);
                    // 按空行切分完整 SSE 事件
                    size_t pos;
                    while ((pos = sseBuffer.find("\n\n")) != std::string::npos) {
                        std::string block = sseBuffer.substr(0, pos + 2);
                        sseBuffer.erase(0, pos + 2);
                        if (flushEvents(block)) {
                            return true;
                        }
                    }
                    return false;
                }
            );
            std::expected<void, std::string> ok;
            co_return ok;
        },
        [&](std::string errmsg) -> asio::awaitable<std::expected<void, std::string>> {
            co_return std::unexpected{std::move(errmsg)};
        }
    );

    if (result.has_value() && finished.load()) {
        // 优雅结束
        std::function<void()> ended;
        {
            std::lock_guard lock(notifyMutex_);
            ended = std::move(subscriptionEnded_);
        }
        if (ended) {
            ended();
        }
    }
    co_return result;
}

asio::awaitable<bool> McpClient::writeStdioLine(const std::string& line) {
    auto wguard = co_await stdioWriteMutex_->lock();
#if defined(BOOST_PROCESS_V2_PROCESS_HPP)
    neograph_asio_error_code wec;
    co_await asio::async_write(
        *stdio_->stdinPipe,
        asio::buffer(line),
        asio::redirect_error(asio::use_awaitable, wec)
    );
    co_return !wec;
#else
#if XX_IS_LINUX_D || XX_IS_MACOS_D
    const char* buf       = line.data();
    size_t      remaining = line.size();
    while (remaining > 0) {
        ssize_t n = ::write(stdio_->stdinFd, buf, remaining);
        if (n <= 0) {
            co_return false;
        }
        buf       += n;
        remaining -= static_cast<size_t>(n);
    }
    co_return true;
#elif XX_IS_WIN_D
    DWORD written = 0;
    BOOL  ok      = WriteFile(
        stdio_->stdinHandle,
        line.data(),
        static_cast<DWORD>(line.size()),
        &written,
        nullptr
    );
    co_return ok == TRUE&& written == static_cast<DWORD>(line.size());
#else
    co_return false;
#endif
#endif
}

asio::awaitable<void> McpClient::sendRawNotification(std::string_view method, const json& params) {
    json req;
    req["jsonrpc"] = "2.0";
    req["method"]  = method;
    if (!params.is_null()) {
        req["params"] = params;
    }

    if (config_.isHttp()) {
        auto                  url  = httpMessageUrl_.empty() ? config_.serverUrl : httpMessageUrl_;
        [[maybe_unused]] auto resp = co_await util::HttpClient::postAsync(
            url,
            req,
            buildHttpHeaders(),
            util::HttpClient::RequestConfig{.readChunkTimeout = config_.requestTimeout}
        );
    } else if (config_.isStdio()) {
        auto reqStr = fmt::format("{}\n", req.dump());
        co_await writeStdioLine(reqStr);
    }
    co_return;
}

bool McpClient::startStdioSubprocess(asio::any_io_executor executor) {
    if (config_.serverCommand.empty()) {
        return false;
    }

    stdioWriteMutex_ = std::make_unique<util::AsyncMutex>(executor);

    bool ok = agentxx::util::catchError<bool>(
        [&]() -> bool {
            stdio_ = std::make_unique<StdioTransport>();
            if (!stdio_->start(executor, config_.serverCommand, shared_from_this())) {
                stdio_.reset();
                return false;
            }
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGW("[McpClient] failed to start subprocess: {}", errmsg);
            stdio_.reset();
            return false;
        }
    );
    return ok;
}

void McpClient::closeInternal() {
    bool expected = false;
    if (!closed_.compare_exchange_strong(expected, true)) {
        return;
    }

    initialized_.store(false);

    httpMessageUrl_.clear();
    mcpSessionId_.clear();
    sseDiscovered_.store(false);

    if (stdio_) {
        stdio_->close();
    }

    {
        std::lock_guard lock(notifyMutex_);
        notificationHandler_ = nullptr;
        subscriptionEnded_   = nullptr;
    }

    std::lock_guard lock(pendingMutex_);
    for (auto& [id, req] : pending_) {
        json errorResp;
        errorResp["jsonrpc"] = "2.0";
        errorResp["id"]      = id;
        errorResp["error"]   = jsonRpcError(-32000, "Client closed before response");
        // promise 可能已被接收方处理 (重复响应), set_value 抛 future_error 时忽略
        agentxx::util::catchError<bool>(
            [&]() -> bool {
                req->promise.set_value(std::move(errorResp));
                return true;
            },
            [](std::string) -> bool {
                return false;
            }
        );
    }
    pending_.clear();
}

void McpClient::deliverResponse(const json& response) {
    // 2026-07-28 通知 (subscriptions/listen 的 ack / list_changed 等): 无 id
    if (!response.contains("id") || response["id"].is_null()) {
        if (response.contains("method") && response["method"].is_string()) {
            std::function<void(const json&)> handler;
            {
                std::lock_guard lock(notifyMutex_);
                handler = notificationHandler_;
            }
            if (handler) {
                // stdio: 在子进程读取线程上调用回调
                agentxx::util::catchError<bool>(
                    [&]() -> bool {
                        handler(response);
                        return true;
                    },
                    [](std::string errmsg) -> bool {
                        XX_LOGW("[McpClient] notification handler error: {}", errmsg);
                        return false;
                    }
                );
            }
        }
        return;
    }
    json respId = response["id"];

    int64_t idVal = -1;
    if (respId.is_number_integer()) {
        idVal = respId.get<int64_t>();
    } else if (respId.is_number_unsigned()) {
        idVal = static_cast<int64_t>(respId.get<uint64_t>());
    } else if (respId.is_string()) {
        bool parsed = agentxx::util::catchError<bool>(
            [&]() -> bool {
                idVal = std::stoll(respId.get<std::string>());
                return true;
            },
            [](std::string) -> bool {
                return false;
            }
        );
        if (!parsed) {
            return;
        }
    } else {
        return;
    }

    std::shared_ptr<PendingRequest> req;
    {
        std::lock_guard lock(pendingMutex_);
        auto            it = pending_.find(idVal);
        if (it == pending_.end()) {
            return;
        }
        req = std::move(it->second);
        pending_.erase(it);
    }

    // promise 可能已被处理, set_value 抛 future_error 时忽略
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            req->promise.set_value(response);
            return true;
        },
        [](std::string) -> bool {
            return false;
        }
    );
}

// ---------------------------------------------------------------------------
// McpClientTool
// ---------------------------------------------------------------------------

McpClientTool::McpClientTool(
    std::shared_ptr<McpClient>                  client,
    McpToolDefinition                           def,
    std::weak_ptr<agentxx::agent::AgentContext> ctx,
    std::string                                 toolNamespace
) :
    agentxx::tools::XXToolBase(makeNamespacedName(toolNamespace, def.name), std::move(ctx)),
    client_(std::move(client)),
    def_(std::move(def)),
    toolNamespace_(std::move(toolNamespace)) {}

neograph::ChatTool McpClientTool::get_definition() const {
    neograph::ChatTool tool;
    tool.name        = namespacedName();
    tool.description = def_.description;
    tool.parameters  = def_.inputSchema;
    return tool;
}

asio::awaitable<std::string> McpClientTool::execute_async(const neograph::json& arguments) {
    // 工具调用整体超时 (配置项 toolCallTimeout, 毫秒; 0 = 不限制):
    // - 覆盖 callTool 的完整流程 (含 HeaderMismatch 重试等), 是总超时兜底
    // - 超时时取消底层请求 (HTTP/stdio), 返回超时错误; 外部取消按原语义传播
    // - 内部请求级超时 (requestTimeout) 仍独立生效, 默认 60s < 120s
    auto callWithTimeout = [&]() -> asio::awaitable<std::expected<json, std::string>> {
        co_return co_await client_->callTool(def_.name, arguments);
    };
    auto result = co_await agentxx::util::asyncWithTimeout<std::expected<json, std::string>>(
        callWithTimeout,
        client_->config_.toolCallTimeout,
        [this]() -> std::expected<json, std::string> {
            return std::unexpected{fmt::format(
                "MCP tool call [{}] timed out after {}ms",
                def_.name,
                client_->config_.toolCallTimeout.count()
            )};
        }
    );
    if (!result.has_value()) {
        throw std::runtime_error(
            fmt::format("MCP tool call [{}] failed: {}", def_.name, result.error())
        );
    }
    json resp = result.value();

    if (resp.contains("content") && resp["content"].is_array()) {
        std::string combined;
        for (const auto& c : resp["content"]) {
            std::string type = c.value("type", "");
            if (type == "text") {
                if (!combined.empty()) {
                    combined += "\n";
                }
                combined += c.value("text", "");
            } else if (type == "audio" || type == "image") {
                if (!combined.empty()) {
                    combined += "\n";
                }
                combined += fmt::format(
                    "[content type: {}, mimeType: {}]",
                    type,
                    c.value("mimeType", "")
                );
            } else if (type == "resource") {
                if (!combined.empty()) {
                    combined += "\n";
                }
                combined += fmt::format(
                    "[embedded resource: {}]",
                    c.value("resource", json::object()).value("uri", "unknown")
                );
            } else if (type == "resource_link") {
                if (!combined.empty()) {
                    combined += "\n";
                }
                combined += fmt::format("[resource link: {}]", c.value("uri", ""));
            }
        }
        if (!combined.empty()) {
            co_return combined;
        }
    }

    if (resp.contains("structuredContent") && !resp["structuredContent"].is_null()) {
        co_return fmt::format("[structuredContent]: {}", resp["structuredContent"].dump());
    }

    co_return resp.dump();
}

std::string McpClientTool::get_name() const {
    return namespacedName();
}

std::string McpClientTool::namespacedName() const {
    return makeNamespacedName(toolNamespace_, def_.name);
}

} // namespace server
} // namespace agentxx
