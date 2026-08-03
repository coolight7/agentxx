#include "agentxx/protocol/mcp_client.h"

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
#include "asio/cancel_after.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/read_until.hpp"
#include "asio/redirect_error.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "asio/write.hpp"
#include <sstream>

namespace agentxx {
namespace server {

namespace {
/// 计算命名空间前缀后的对外 tool 名称 (namespace 非空时为 "namespace_name")
std::string makeNamespacedName(std::string_view toolNamespace, std::string_view name) {
    return toolNamespace.empty() ? std::string{name} : fmt::format("{}_{}", toolNamespace, name);
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
            try {
                auto response = json::parse(line);
                client->deliverResponse(response);
            } catch (const json::parse_error&) {
                XX_LOGW("[McpClient] ignoring malformed stdout line: {}", line.substr(0, 128));
            }
        }
        if (!buffer.empty()) {
            try {
                auto response = json::parse(buffer);
                client->deliverResponse(response);
            } catch (...) {
            }
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
                try {
                    auto response = json::parse(line);
                    client->deliverResponse(response);
                } catch (const json::parse_error&) {
                    XX_LOGW("[McpClient] ignoring malformed stdout line: {}", line.substr(0, 128));
                }
            }
        }
        if (!buffer.empty()) {
            try {
                auto response = json::parse(buffer);
                client->deliverResponse(response);
            } catch (...) {
            }
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
                try {
                    auto response = json::parse(line);
                    client->deliverResponse(response);
                } catch (...) {
                }
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

    json clientInfo;
    clientInfo["name"]    = config_.clientName;
    clientInfo["version"] = config_.clientVersion;

    json params;
    params["protocolVersion"] = config_.protocolVersion;
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

    auto negotiated      = negotiateProtocolVersion(config_.protocolVersion, r);
    info.protocolVersion = std::move(negotiated);

    if (!config_.isStdio()) {
        co_await sendRawNotification("notifications/initialized", json::object());
    }

    serverInfo_ = info;
    initialized_.store(true);
    co_return std::move(info);
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

    auto resp = co_await util::HttpClient::getAsync(
        sseUrl,
        headers,
        util::HttpClient::RequestConfig{.readChunkTimeout = config_.initTimeout}
    );

    if (!resp.has_value()) {
        XX_LOGW(
            "[McpClient] SSE discovery failed ({}), falling back to "
            "direct POST",
            resp.error()
        );
        httpMessageUrl_ = config_.serverUrl;
        sseDiscovered_.store(true);
        co_return;
    }

    auto sessionIdHdr = resp.value().findHeader("mcp-session-id");
    if (!sessionIdHdr.empty()) {
        mcpSessionId_ = sessionIdHdr;
    }

    auto events = parseSseEvents(resp.value().body);
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

asio::awaitable<std::expected<json, std::string>>
    McpClient::sendHttpRequest(int64_t id, std::string_view method, const json& params) {
    if (!sseDiscovered_.load()) {
        co_await discoverSseEndpoint();
    }

    auto req     = makeRequest(id, method, params);
    auto headers = buildHttpHeaders();

    auto resp = co_await util::HttpClient::postAsync(
        httpMessageUrl_,
        req,
        headers,
        util::HttpClient::RequestConfig{.readChunkTimeout = config_.requestTimeout}
    );

    if (!resp.has_value()) {
        co_return std::unexpected{std::move(resp.error())};
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
                try {
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
                            auto err = getErrorFromResponse(j);
                            if (err.has_value()) {
                                co_return j;
                            }
                            co_return j;
                        }
                    }
                } catch (const json::parse_error&) {
                    XX_LOGW("[McpClient] ignoring malformed SSE data: {}", ev.data.substr(0, 128));
                }
            }
        }
        co_return std::unexpected{
            fmt::format("no matching response in SSE stream for id {}", id)
        };
    }

    auto bodyJson = httpResp.bodyJson();
    if (!bodyJson.has_value()) {
        co_return std::unexpected{fmt::format("invalid JSON response: {}", httpResp.body.substr(0, 256))};
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
            co_return std::unexpected{fmt::format("write to subprocess stdin failed: {}", wec.message())};
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
        auto wguard = co_await stdioWriteMutex_->lock();
#if defined(BOOST_PROCESS_V2_PROCESS_HPP)
        neograph_asio_error_code wec;
        co_await asio::async_write(
            *stdio_->stdinPipe,
            asio::buffer(reqStr),
            asio::redirect_error(asio::use_awaitable, wec)
        );
#else
#if XX_IS_LINUX_D || XX_IS_MACOS_D
        ::write(stdio_->stdinFd, reqStr.data(), reqStr.size());
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
#endif
    }
    co_return;
}

bool McpClient::startStdioSubprocess(asio::any_io_executor executor) {
    if (config_.serverCommand.empty()) {
        return false;
    }

    stdioWriteMutex_ = std::make_unique<util::AsyncMutex>(executor);

    try {
        stdio_ = std::make_unique<StdioTransport>();
        if (!stdio_->start(executor, config_.serverCommand, shared_from_this())) {
            stdio_.reset();
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        XX_LOGW("[McpClient] failed to start subprocess: {}", e.what());
        stdio_.reset();
        return false;
    }
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

    std::lock_guard lock(pendingMutex_);
    for (auto& [id, req] : pending_) {
        json errorResp;
        errorResp["jsonrpc"] = "2.0";
        errorResp["id"]      = id;
        errorResp["error"]   = jsonRpcError(-32000, "Client closed before response");
        try {
            req->promise.set_value(std::move(errorResp));
        } catch (...) {
        }
    }
    pending_.clear();
}

void McpClient::deliverResponse(const json& response) {
    if (!response.contains("id")) {
        return;
    }
    json respId = response["id"];
    if (respId.is_null()) {
        return;
    }

    int64_t idVal = -1;
    if (respId.is_number_integer()) {
        idVal = respId.get<int64_t>();
    } else if (respId.is_number_unsigned()) {
        idVal = static_cast<int64_t>(respId.get<uint64_t>());
    } else if (respId.is_string()) {
        try {
            idVal = std::stoll(respId.get<std::string>());
        } catch (...) {
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

    try {
        req->promise.set_value(response);
    } catch (...) {
    }
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
    auto result = co_await client_->callTool(def_.name, arguments);
    if (!result.has_value()) {
        throw std::runtime_error(fmt::format("MCP tool call [{}] failed: {}", def_.name, result.error()));
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
                combined
                    += fmt::format("[content type: {}, mimeType: {}]", type, c.value("mimeType", ""));
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
