#pragma once

/// HTTP 录制/回放夹具 (计划 LLM-13)
///
/// 目的: 让 provider 层的行为 (请求装配 + 响应/SSE 解析) 能在**没有真实上游、
/// 也没有手工写死响应**的情况下重复验证:
///
/// ```
///   录制: provider ──▶ HttpRecorder (本地服务) ──▶ 真实/模拟上游
///                              └──▶ 记录请求摘要 + 响应 → 固定装置文件
///   回放: provider ──▶ HttpPlayer (本地服务) ──▶ 按顺序回放固定装置里的响应
/// ```
///
/// 与现有测试夹具的分工:
/// - 假 provider (`fake_provider.h`): 在 provider **之上**, 覆盖重试/压缩/取消/工具循环;
/// - 本夹具: 在 provider **之下**, 覆盖真实 HTTP 编解码与 SSE 解析, 固定装置可入库;
/// - 手工 mock (各 provider 测试里的 Mock*Server): 覆盖个别响应形态, 保留不变。
///
/// 脱敏 (固定装置要能提交进仓库):
/// - 请求头只落白名单 (`content-type` / `accept` / `anthropic-version` / `user-agent`),
///   凭据类头 (`authorization` / `x-api-key` / `api-key` / `cookie`) 只记
///   `*_present: bool`, **不记取值**;
/// - 请求体与响应体都过 [agentxx::util::redactSecrets] 与调用方补充的
///   `secrets` 列表 (按字面替换); 请求摘要基于**原始**请求体计算, 不受脱敏影响。
///
/// 只用于测试: 不进生产代码, 也不进 lib。
#include "agentxx/util/diagnostics.h" // redactSecrets
#include "utilxx/http_client.h"
#include "utilxx/http_server.h"
#include "utilxx_base/hash.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"
#include <asio/awaitable.hpp>
#include <cstdint>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "agentxx/util/exception.h" // catchErrorAsync

namespace agentxx {
namespace test {

/// 一次 HTTP 交互 (请求摘要 + 响应)
struct HttpInteraction {
    std::string method;        ///< 请求方法 (目前录制/回放都按 POST/GET 处理)
    std::string path;          ///< 请求路径 (不含查询串)
    std::string requestDigest; ///< 请求摘要 (method + path + 原始请求体)
    size_t      requestBytes = 0;
    /// 脱敏后的请求体 (仅供人工核对/断言; 匹配时用摘要)
    std::string requestBody;
    /// 白名单请求头 + 凭据头的 "是否出现" 标记
    utilxx_base::Json requestHeaders = utilxx_base::Json::object();
    int               status        = 0;
    std::string       contentType;
    std::string       responseBody;
};

/// 请求摘要: `fnv1a64(method + " " + path + "\n" + body)` 的十六进制
/// - 只用于"同一请求"的稳定比对 (测试夹具), 不是安全用途
std::string
    httpRequestDigest(std::string_view method, std::string_view path, std::string_view body) {
    const auto mixed = fmt::format("{} {}\n{}", method, path, body);
    return fmt::format("{:016x}", utilxx_base::hash::fnv1a64(mixed));
}

/// 固定装置: 一组按时间顺序的交互 (可写入 JSON 文件入库)
struct HttpFixture {
    int                         version = 1;
    std::string                 note;
    std::vector<HttpInteraction> interactions;

    utilxx_base::Json toJson() const {
        auto arr = utilxx_base::Json::array();
        for (const auto& it : interactions) {
            arr.push_back(
                utilxx_base::Json{
                    {"method",         it.method        },
                    {"path",           it.path          },
                    {"requestDigest",  it.requestDigest },
                    {"requestBytes",   static_cast<int64_t>(it.requestBytes)},
                    {"requestHeaders", it.requestHeaders},
                    {"requestBody",    it.requestBody   },
                    {"status",         it.status        },
                    {"contentType",    it.contentType   },
                    {"responseBody",   it.responseBody  },
                }
            );
        }
        utilxx_base::Json root;
        root["version"]      = version;
        root["note"]         = note;
        root["interactions"] = std::move(arr);
        return root;
    }

    static HttpFixture fromJson(const utilxx_base::Json& json) {
        HttpFixture out;
        out.version = static_cast<int>(json.value("version", 1));
        out.note    = json.value("note", std::string{});
        if (json.contains("interactions") && json["interactions"].is_array()) {
            for (const auto& node : json["interactions"]) {
                HttpInteraction it;
                it.method        = node.value("method", std::string{});
                it.path          = node.value("path", std::string{});
                it.requestDigest = node.value("requestDigest", std::string{});
                it.requestBytes  = static_cast<size_t>(node.value("requestBytes", int64_t{0}));
                it.requestBody   = node.value("requestBody", std::string{});
                it.status        = static_cast<int>(node.value("status", 0));
                it.contentType   = node.value("contentType", std::string{});
                it.responseBody  = node.value("responseBody", std::string{});
                if (node.contains("requestHeaders")) {
                    it.requestHeaders = node["requestHeaders"];
                }
                out.interactions.push_back(std::move(it));
            }
        }
        return out;
    }

    /// 写入文件 (JSON 缩进 2; 父目录不存在时自动创建)
    bool saveToFile(const std::string& path, std::string* error = nullptr) const {
        std::error_code ec;
        if (const auto parent = std::filesystem::path{path}.parent_path(); !parent.empty()) {
            std::filesystem::create_directories(utilxx_base::utf8ToPath(parent.string()), ec);
        }
        std::ofstream out(utilxx_base::utf8ToPath(path), std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) {
                *error = "cannot open file for write: " + path;
            }
            return false;
        }
        out << toJson().dump(2) << "\n";
        return out.good();
    }

    /// 从文件读取 (不存在/非法 JSON 抛 std::runtime_error)
    static HttpFixture loadFromFile(const std::string& path) {
        std::ifstream in(utilxx_base::utf8ToPath(path), std::ios::binary);
        if (!in) {
            throw std::runtime_error{"http fixture not found: " + path};
        }
        std::ostringstream buf;
        buf << in.rdbuf();
        return fromJson(utilxx_base::Json::parse(buf.str()));
    }
};

namespace detail {

/// 固定装置里需要保留取值的请求头 (其余头一律不落盘)
inline const std::vector<std::string>& fixtureKeptHeaders() {
    static const std::vector<std::string> kKept = {
        "content-type",
        "accept",
        "anthropic-version",
        "user-agent",
    };
    return kKept;
}

/// 凭据类请求头: 只记"是否出现过", 不记取值
inline const std::vector<std::string>& credentialHeaders() {
    static const std::vector<std::string> kCred = {
        "authorization",
        "x-api-key",
        "api-key",
        "cookie",
        "proxy-authorization",
    };
    return kCred;
}

/// 按字面替换需要屏掉的取值 (调用方补充的 secrets)
inline std::string maskSecrets(std::string text, const std::vector<std::string>& secrets) {
    for (const auto& secret : secrets) {
        if (secret.empty()) {
            continue;
        }
        size_t pos = 0;
        while ((pos = text.find(secret, pos)) != std::string::npos) {
            text.replace(pos, secret.size(), "***");
            pos += 3;
        }
    }
    return text;
}

/// 文本脱敏: 先屏调用方给的取值, 再过通用凭据形态
inline std::string
    sanitizeText(std::string_view text, const std::vector<std::string>& secrets) {
    return agentxx::util::redactSecrets(maskSecrets(std::string{text}, secrets));
}

/// 请求头快照 (白名单取值 + 凭据存在标记)
inline utilxx_base::Json sanitizeHeaders(const utilxx::HttpServer::Request& req) {
    utilxx_base::Json out = utilxx_base::Json::object();
    for (const auto& field : req) {
        std::string name = utilxx_base::toLower(std::string{field.name_string()});
        std::string value{field.value()};
        for (const auto& kept : fixtureKeptHeaders()) {
            if (name == kept) {
                out[name] = value;
            }
        }
        for (const auto& cred : credentialHeaders()) {
            if (name == cred && !value.empty()) {
                out[name + "_present"] = true;
            }
        }
    }
    return out;
}

/// 起一个本地 HTTP 服务并在后台线程运行 (返回端口; 0 = 启动失败)
inline uint16_t
    startServerInThread(const std::shared_ptr<utilxx::HttpServer>& server, std::thread& thread) {
    thread = std::thread([s = server.get()]() { s->start(); });
    for (int i = 0; i < 200; ++i) {
        const auto port = server->port();
        if (port != 0) {
            return port;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return 0;
}

/// LLM 接口常用的路径 (路由是精确匹配, 这里把常见形态都注册上)
inline const std::vector<std::string>& llmPaths() {
    static const std::vector<std::string> kPaths = {
        "/",
        "/chat/completions",
        "/v1/chat/completions",
        "/messages",
        "/v1/messages",
        "/responses",
        "/v1/responses",
    };
    return kPaths;
}

/// 把处理器注册到全部常见路径的全部方法槽位
inline void registerOnLlmPaths(
    const std::shared_ptr<utilxx::HttpServer>&                server,
    const std::shared_ptr<utilxx::HttpServer::Handler>&       handler
) {
    for (const auto& path : llmPaths()) {
        for (int slot = 0; slot <= 3; ++slot) {
            server->router().add(path, slot, handler);
        }
    }
}

} // namespace detail

/// 录制器: 把请求转发给上游, 同时把 (请求摘要 + 响应) 记入固定装置
class HttpRecorder {
public:

    struct Options {
        /// 上游来源 (只取 origin, 如 `http://127.0.0.1:1234`; 路径按对端原样透传)
        std::string              upstreamOrigin;
        /// 额外需要屏掉的取值 (如测试用的 API key 文本)
        std::vector<std::string> secrets;
    };

    /// 启动录制服务; 端口分配失败返回 nullptr
    static std::shared_ptr<HttpRecorder> start(Options options) {
        auto recorder = std::shared_ptr<HttpRecorder>(new HttpRecorder(std::move(options)));
        if (!recorder->setup()) {
            return nullptr;
        }
        return recorder;
    }

    ~HttpRecorder() {
        stop();
    }

    void stop() {
        if (server_) {
            server_->stop();
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        server_.reset();
    }

    uint16_t port() const {
        return port_;
    }

    /// 本服务的来源 (`http://127.0.0.1:<port>`; provider 的 base_url 用它拼版本前缀)
    std::string baseUrl() const {
        return fmt::format("http://127.0.0.1:{}", port_);
    }

    size_t count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return interactions_.size();
    }

    HttpFixture fixture() const {
        std::lock_guard<std::mutex> lock(mutex_);
        HttpFixture                 out;
        out.version      = 1;
        out.note         = "由 http_recorder 录制 (请求头白名单 + 凭据只记存在标记, 正文已脱敏)";
        out.interactions = interactions_;
        return out;
    }

private:

    explicit HttpRecorder(Options options) : options_(std::move(options)) {}

    bool setup() {
        server_ = std::make_shared<utilxx::HttpServer>(
            utilxx::HttpServer::Config{.address = "127.0.0.1", .port = 0, .ioThreads = 1}
        );
        auto self    = this;
        auto handler = std::make_shared<utilxx::HttpServer::Handler>(
            [self](utilxx::HttpServer::Request& req, utilxx::HttpServer::Response& resp, std::string_view)
                -> asio::awaitable<void> {
                co_await self->handle(req, resp);
                co_return;
            }
        );
        detail::registerOnLlmPaths(server_, handler);
        port_ = detail::startServerInThread(server_, thread_);
        if (port_ == 0) {
            server_->stop();
            if (thread_.joinable()) {
                thread_.join();
            }
            server_.reset();
            return false;
        }
        XX_LOGI(
            "[http_recorder] recording: local={} -> upstream={}",
            baseUrl(),
            options_.upstreamOrigin
        );
        return true;
    }

    asio::awaitable<void> handle(
        utilxx::HttpServer::Request&  req,
        utilxx::HttpServer::Response& resp
    ) {
        namespace http = boost::beast::http;
        const std::string method{req.method_string()};
        const std::string path{utilxx::requestPath(req.target())};
        const std::string body = req.body();

        HttpInteraction it;
        it.method        = method;
        it.path          = path;
        it.requestBytes  = body.size();
        it.requestDigest = httpRequestDigest(method, path, body);
        it.requestBody   = detail::sanitizeText(body, options_.secrets);
        it.requestHeaders = detail::sanitizeHeaders(req);

        // 转发: 头逐一透传 (排除逐跳头), 路径按对端原样
        utilxx::HeaderMap headers;
        for (const auto& field : req) {
            const std::string name = utilxx_base::toLower(std::string{field.name_string()});
            if (name == "host" || name == "content-length" || name == "connection"
                || name == "transfer-encoding" || name == "accept-encoding") {
                continue;
            }
            headers.set(std::string{field.name_string()}, std::string{field.value()});
        }
        const std::string url         = options_.upstreamOrigin + path;
        const std::string contentType = std::string{headers.getSingle("content-type")};

        bool forwardedOk = false;
        co_await agentxx::util::catchErrorAsync<bool>(
            [&]() -> asio::awaitable<bool> {
                auto result = co_await utilxx::HttpClient::postAsync(
                    url,
                    body,
                    contentType.empty() ? std::string_view{"application/json"}
                                        : std::string_view{contentType},
                    headers,
                    utilxx::HttpClient::RequestConfig{}
                );
                if (result.has_value()) {
                    it.status       = result->status;
                    it.contentType  = std::string{result->headers.getSingle("content-type")};
                    it.responseBody = detail::sanitizeText(result->body, options_.secrets);
                    forwardedOk     = true;
                } else {
                    it.status       = 502;
                    it.contentType  = "application/json";
                    it.responseBody = utilxx_base::Json{
                        {"error",   "http_recorder upstream failed"},
                        {"message", detail::sanitizeText(result.error(), options_.secrets)},
                    }
                                          .dump();
                }
                co_return true;
            },
            [&](std::string errmsg) -> asio::awaitable<bool> {
                it.status       = 502;
                it.contentType  = "application/json";
                it.responseBody = utilxx_base::Json{
                    {"error",   "http_recorder upstream threw"},
                    {"message", detail::sanitizeText(errmsg, options_.secrets)},
                }
                                      .dump();
                co_return false;
            }
        );

        {
            std::lock_guard<std::mutex> lock(mutex_);
            interactions_.push_back(it);
        }
        XX_LOGD(
            "[http_recorder] recorded #{} {} {} (upstream ok={})",
            count(),
            method,
            path,
            forwardedOk
        );

        // 回给调用方 (provider) 的响应: 原样搬运状态码/类型/正文
        resp.result(static_cast<http::status>(it.status == 0 ? 502 : it.status));
        resp.set(
            http::field::content_type,
            it.contentType.empty() ? std::string{"application/json"} : it.contentType
        );
        resp.body() = it.responseBody;
        resp.prepare_payload();
        co_return;
    }

    Options                                                  options_;
    std::shared_ptr<utilxx::HttpServer>                      server_;
    std::thread                                              thread_;
    uint16_t                                                 port_ = 0;
    mutable std::mutex                                       mutex_;
    std::vector<HttpInteraction>                             interactions_;
};

/// 回放器: 按顺序把固定装置里的响应回给调用方 (provider)
class HttpPlayer {
public:

    struct Options {
        /// 严格顺序: 请求必须与固定装置里下一条完全一致 (方法 + 路径 + 请求摘要)
        /// - false = 只比方法与路径, 允许同一路径的请求连续回放
        bool strictOrder = true;
        /// 是否比对请求体摘要 (关闭后只看方法 + 路径)
        bool matchBody = true;
    };

    static std::shared_ptr<HttpPlayer> start(HttpFixture fixture, Options options = {}) {
        auto player = std::shared_ptr<HttpPlayer>(
            new HttpPlayer(std::move(fixture), std::move(options))
        );
        if (!player->setup()) {
            return nullptr;
        }
        return player;
    }

    ~HttpPlayer() {
        stop();
    }

    void stop() {
        if (server_) {
            server_->stop();
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        server_.reset();
    }

    uint16_t port() const {
        return port_;
    }

    std::string baseUrl() const {
        return fmt::format("http://127.0.0.1:{}", port_);
    }

    size_t servedCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return served_;
    }

    bool allServed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return served_ >= fixture_.interactions.size();
    }

    /// 回放过程中的不匹配记录 (空 = 全部按预期命中)
    std::vector<std::string> mismatches() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return mismatches_;
    }

private:

    HttpPlayer(HttpFixture fixture, Options options) :
        fixture_(std::move(fixture)), options_(std::move(options)) {}

    bool setup() {
        server_ = std::make_shared<utilxx::HttpServer>(
            utilxx::HttpServer::Config{.address = "127.0.0.1", .port = 0, .ioThreads = 1}
        );
        auto self    = this;
        auto handler = std::make_shared<utilxx::HttpServer::Handler>(
            [self](utilxx::HttpServer::Request& req, utilxx::HttpServer::Response& resp, std::string_view)
                -> asio::awaitable<void> {
                self->handle(req, resp);
                co_return;
            }
        );
        detail::registerOnLlmPaths(server_, handler);
        port_ = detail::startServerInThread(server_, thread_);
        if (port_ == 0) {
            server_->stop();
            if (thread_.joinable()) {
                thread_.join();
            }
            server_.reset();
            return false;
        }
        XX_LOGI(
            "[http_player] replaying {} interaction(s) at {}",
            fixture_.interactions.size(),
            baseUrl()
        );
        return true;
    }

    void handle(utilxx::HttpServer::Request& req, utilxx::HttpServer::Response& resp) {
        namespace http = boost::beast::http;
        const std::string method{req.method_string()};
        const std::string path{utilxx::requestPath(req.target())};
        const std::string digest = httpRequestDigest(method, path, req.body());

        std::lock_guard<std::mutex> lock(mutex_);
        if (served_ >= fixture_.interactions.size()) {
            mismatches_.push_back(fmt::format(
                "unexpected extra request: {} {} (fixture has {} interaction(s))",
                method,
                path,
                fixture_.interactions.size()
            ));
            resp.result(http::status::bad_request);
            resp.set(http::field::content_type, "application/json");
            resp.body() = utilxx_base::Json{
                {"error",   "http_player: no more recorded interaction"},
                {"message", mismatches_.back()},
            }
                              .dump();
            resp.prepare_payload();
            return;
        }

        const auto& expected = fixture_.interactions[served_];
        bool        match    = expected.method == method && expected.path == path;
        if (match && options_.matchBody) {
            match = expected.requestDigest == digest;
        }
        if (!match) {
            mismatches_.push_back(fmt::format(
                "request #{} mismatch: expected {} {} digest={} ({} bytes), got {} {} digest={} "
                "({} bytes)",
                served_,
                expected.method,
                expected.path,
                expected.requestDigest,
                expected.requestBytes,
                method,
                path,
                digest,
                req.body().size()
            ));
            resp.result(http::status::bad_request);
            resp.set(http::field::content_type, "application/json");
            resp.body() = utilxx_base::Json{
                {"error",          "http_player: request does not match fixture"},
                {"expectedIndex",  served_                          },
                {"expectedDigest", expected.requestDigest           },
                {"actualDigest",   digest                           },
                {"message",        mismatches_.back()               },
            }
                              .dump();
            resp.prepare_payload();
            return;
        }

        resp.result(static_cast<http::status>(expected.status == 0 ? 200 : expected.status));
        resp.set(
            http::field::content_type,
            expected.contentType.empty() ? std::string{"application/json"} : expected.contentType
        );
        resp.body() = expected.responseBody;
        resp.prepare_payload();
        ++served_;
        if (options_.strictOrder) {
            // 严格顺序: 下一条必须是固定装置里的下一条 (上面的索引即顺序游标)
        }
    }

    HttpFixture               fixture_;
    Options                   options_;
    std::shared_ptr<utilxx::HttpServer> server_;
    std::thread               thread_;
    uint16_t                  port_ = 0;
    mutable std::mutex        mutex_;
    size_t                    served_ = 0;
    std::vector<std::string>  mismatches_;
};

} // namespace test
} // namespace agentxx
