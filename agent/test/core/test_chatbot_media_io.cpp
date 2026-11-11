///
/// `chatbot_media_io` —— QQ 富媒体上传的完整流程回归 (本地模拟服务端)
///
/// 为什么要有这一层: 上传协议的错误在本地完全看不出来 —— 编译通过、纯函数用例也通过,
/// 真机上却只得到一句"附件 'hello_test.txt' 上传失败: upload_part_finish 失败 (status 500)"。
/// 模拟服务端按官方文档校验请求 (尤其是分片完成必须带 `block_size` 与 `md5`),
/// 于是"少给字段"这类错误在这里直接失败, 不必等真机。
///
/// 对应官方文档 (2026-10 版):
/// - `POST /v2/groups/{id}/upload_prepare` → `upload_id` + `block_size`(**字符串**) +
///   `parts[]{index, presigned_url, block_size}`;
/// - `PUT <presigned_url>` 上传该片字节;
/// - `POST /v2/groups/{id}/upload_part_finish` `{upload_id, part_index, block_size, md5}`;
/// - `POST /v2/groups/{id}/files` `{upload_id, file_name, file_type, srv_send_msg}` 合并,
///   或 `{file_type, file_name, file_data, srv_send_msg}` 二进制直传。
#include "agentxx-test/core/test_chatbot_media_io.h"

#include "agentxx_chatbot/chatbot_net.h"
#include "agentxx_chatbot/chatbot_types.h"
#include "agentxx_chatbot/platform/qq/qq_api.h"
#include "agentxx_chatbot/platform/qq/qq_media.h"

#include "pluginxx/api/abi.h"
#include "pluginxx/api/net.h"
#include "utilxx/crypto.h"
#include "utilxx/http_server.h"
#include "utilxx/net_hub.h"
#include "utilxx_base/json.h"

#include <asio/this_coro.hpp>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
int g_cmi_passed = 0;
int g_cmi_failed = 0;
} // namespace

#define XX_TEST_PASSED g_cmi_passed
#define XX_TEST_FAILED g_cmi_failed

namespace agentxx {
namespace test {

namespace {

using namespace agentxx::chatbot;
using utilxx_base::Json;

namespace fs = std::filesystem;

bool endsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size()
           && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/// 一个文件在三段校验值之外的输入 (临时文件路径 + 文件名)
struct TempFile {
    std::string path;
    std::string name;
};

std::string makeTempDir() {
    auto dir = fs::temp_directory_path()
               / ("agentxx_media_io_" + std::to_string(
                      std::chrono::steady_clock::now().time_since_epoch().count()
                  ));
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir.string();
}

TempFile writeTempFile(const std::string& dir, std::string_view name, std::string_view content) {
    TempFile out;
    out.name = std::string{name};
    out.path = (fs::path{dir} / out.name).string();
    std::ofstream file(out.path, std::ios::binary | std::ios::trunc);
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    file.close();
    return out;
}

/// 附件下载用的二进制内容 (含非 UTF-8 字节与 NUL: 证明下载不经过 JSON/base64)
std::string blobContent() {
    std::string out = "\x00\x01\xFF\xFE";
    out += "blob-bytes";
    out.push_back(static_cast<char>(0x80));
    return out;
}

/// QQ 富媒体上传的模拟服务端
///
/// 行为按官方文档校验, 所以"请求体少字段"这类错误会像真机一样得到 5xx/4xx。
class MockQqServer {
public:

    /// 记一次请求 (按出现顺序)
    struct RequestRecord {
        std::string method;
        std::string path;
        std::string body;
        Json        json;
        bool        hasJson = false;
    };

    // ---- 可调的场景参数 (调用之间修改, 请求进行中不要改) ----
    /// 每个分片的字节数 (`block_size`, 以字符串下发, 与真实平台一致)
    int64_t blockSize = 4;
    /// 预上传不给分片清单 (触发二进制直传路径)
    bool noParts = false;
    /// 分片完成前 N 次回 500 (模拟官方错误码 40093001 那类瞬时失败)
    int transientFailureCount = 0;
    /// 分片完成一律回 400 (参数错误: 不该重试)
    bool rejectPartFinish = false;
    std::string uploadId = "upload_test";

    ~MockQqServer() {
        stop();
    }

    bool start() {
        server_ = std::make_shared<utilxx::HttpServer>(
            utilxx::HttpServer::Config{.address = "127.0.0.1", .port = 0, .ioThreads = 1}
        );
        auto self    = this;
        auto handler = std::make_shared<utilxx::HttpServer::Handler>(
            [self](
                utilxx::HttpServer::Request&  req,
                utilxx::HttpServer::Response& resp,
                std::string_view
            ) -> asio::awaitable<void> {
                self->handle(req, resp);
                co_return;
            }
        );
        // 每个场景用自己的会话 id (G1/G2/G3), 避免缓存与请求计数互相干扰
        for (const auto* chat : {"G1", "G2", "G3"}) {
            const std::string prefix = std::string{"/v2/groups/"} + chat;
            server_->router().add(prefix + "/upload_prepare", 2, handler);
            server_->router().add(prefix + "/upload_part_finish", 2, handler);
            server_->router().add(prefix + "/files", 2, handler);
        }
        server_->router().add("/app/getAppAccessToken", 2, handler);
        // 附件下载 (GET 的索引是 0)
        server_->router().add("/download/blob", 0, handler);
        // 预签名地址也是本服务 (PUT 槽位 = 3)
        for (int i = 0; i < 8; ++i) {
            server_->router().add("/cos/p" + std::to_string(i), 3, handler);
        }

        thread_ = std::thread([server = server_.get()]() { server->start(); });
        for (int i = 0; i < 300; ++i) {
            const auto port = server_->port();
            if (port != 0) {
                port_ = port;
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        return false;
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

    std::string baseUrl() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

    /// 当前的请求计数 (用作"从这里开始看")
    size_t mark() {
        std::lock_guard guard(mutex_);
        return requests_.size();
    }

    std::vector<RequestRecord> since(size_t mark) {
        std::lock_guard guard(mutex_);
        if (mark >= requests_.size()) {
            return {};
        }
        return {requests_.begin() + static_cast<std::ptrdiff_t>(mark), requests_.end()};
    }

    /// 从 `mark` 开始, 路径以 `suffix` 结尾的请求
    std::vector<RequestRecord> sincePath(size_t mark, std::string_view suffix) {
        std::vector<RequestRecord> out;
        for (auto& one : since(mark)) {
            if (endsWith(one.path, suffix)) {
                out.push_back(std::move(one));
            }
        }
        return out;
    }

private:

    std::string prepareResponse(int64_t contentSize) {
        Json out          = Json::object();
        out["upload_id"]  = uploadId;
        out["block_size"] = std::to_string(blockSize);
        auto parts        = Json::array();
        if (!noParts) {
            const int64_t count
                = contentSize <= 0 ? 1 : (contentSize + blockSize - 1) / blockSize;
            for (int64_t i = 0; i < count; ++i) {
                Json part            = Json::object();
                part["index"]        = static_cast<int>(i);
                part["presigned_url"] = baseUrl() + "/cos/p" + std::to_string(i);
                part["block_size"]   = std::to_string(blockSize);
                parts.push_back(std::move(part));
            }
        }
        out["parts"] = std::move(parts);
        out["upload_config"] = Json{
            {"concurrency",  int64_t{1} },
            {"retry_timeout", int64_t{30}},
            {"retry_delay",  int64_t{0} },
        };
        return out.dump();
    }

    void handle(utilxx::HttpServer::Request& req, utilxx::HttpServer::Response& resp) {
        namespace http = boost::beast::http;

        RequestRecord record;
        record.method = std::string{req.method_string()};
        record.path   = std::string{utilxx::requestPath(req.target())};
        record.body   = req.body();
        if (!record.body.empty() && record.body.front() == '{') {
            try {
                record.json    = Json::parse(record.body);
                record.hasJson = record.json.is_object();
            } catch (const std::exception&) {
                record.hasJson = false;
            }
        }

        int         status = 200;
        std::string responseBody = "{}";
        {
            std::lock_guard guard(mutex_);
            if (record.path == "/app/getAppAccessToken") {
                responseBody = R"({"access_token":"tok_test","expires_in":"7200"})";
            } else if (endsWith(record.path, "/upload_part_finish")) {
                const auto index = record.hasJson ? qqJsonInt64Of(record.json, "part_index", -1) : -1;
                const auto id    = record.hasJson ? record.json.value("upload_id", std::string{})
                                                  : std::string{};
                // 官方文档标的是字符串; 发数字时这里按坏请求处理 (与真机一致)
                const std::string blockSizeText
                    = record.hasJson && record.json["block_size"].is_string()
                          ? record.json["block_size"].get<std::string>()
                          : std::string{};
                const std::string partMd5
                    = record.hasJson ? record.json.value("md5", std::string{}) : std::string{};
                const auto putPath = "/cos/p" + std::to_string(index);
                const auto stored  = putBodies_.find(putPath);
                if (rejectPartFinish) {
                    status       = 400;
                    responseBody = R"({"code":10000,"message":"参数错误"})";
                } else if (transientFailureCount > 0) {
                    --transientFailureCount;
                    status       = 500;
                    responseBody = R"({"code":40093001,"message":"文件上传失败，请重试"})";
                } else if (id.empty() || index < 0 || blockSizeText.empty() || partMd5.empty()) {
                    // 少给字段 —— 就是修复前真机上的 500
                    status       = 500;
                    responseBody = R"({"code":10000,"message":"缺少 upload_id/part_index/block_size/md5"})";
                } else if (stored == putBodies_.end()) {
                    status       = 500;
                    responseBody = R"({"code":10000,"message":"没有收到该分片"})";
                } else if (blockSizeText != std::to_string(stored->second.size())) {
                    status       = 500;
                    responseBody = R"({"code":10000,"message":"block_size 与该片实际字节数不符"})";
                } else if (partMd5 != utilxx::md5Hex(stored->second)) {
                    status       = 500;
                    responseBody = R"({"code":10000,"message":"md5 与该片内容不符"})";
                }
            } else if (endsWith(record.path, "/upload_prepare")) {
                // 文档把 file_size 标成字符串: 发数字时按坏请求处理
                if (!record.hasJson || !record.json["file_size"].is_string()) {
                    status       = 400;
                    responseBody = R"({"code":10000,"message":"file_size 必须是字符串"})";
                } else {
                    responseBody = prepareResponse(qqJsonInt64(record.json["file_size"]));
                }
            } else if (endsWith(record.path, "/files")) {
                const auto id = record.hasJson ? record.json.value("upload_id", std::string{})
                                               : std::string{};
                const auto data
                    = record.hasJson ? record.json.value("file_data", std::string{}) : std::string{};
                if (!id.empty()) {
                    responseBody = Json{{"file_info", "fi-merge-" + id}, {"ttl", 300}}.dump();
                } else if (!data.empty()) {
                    responseBody = Json{{"file_info", "fi-direct"}, {"ttl", 300}}.dump();
                } else {
                    status       = 400;
                    responseBody = R"({"code":10000,"message":"既没有 upload_id 也没有 file_data"})";
                }
            } else if (record.path == "/download/blob") {
                responseBody = blobContent();
            } else if (record.path.rfind("/cos/", 0) == 0) {
                putBodies_[record.path] = record.body;
                responseBody.clear();
            } else {
                status       = 404;
                responseBody = R"({"code":10404,"message":"没有这个接口"})";
            }
            requests_.push_back(std::move(record));
        }

        resp.result(static_cast<http::status>(status));
        resp.set(http::field::content_type, "application/json");
        resp.body() = responseBody;
        resp.prepare_payload();
    }

    std::shared_ptr<utilxx::HttpServer> server_;
    std::thread                         thread_;
    uint16_t                            port_ = 0;
    std::mutex                          mutex_;
    std::vector<RequestRecord>          requests_;
    /// 预签名地址 → 收到的分片字节 (分片完成时核对该片的 md5 与大小)
    std::map<std::string, std::string, std::less<>> putBodies_;
};

/* ==================== 测试用的插件网络表 ==================== */

void* PLUGINXX_CALL testHostAlloc(uint64_t size) {
    return std::malloc(static_cast<size_t>(size));
}

void PLUGINXX_CALL testHostFree(void* ptr) {
    std::free(ptr);
}

const void* PLUGINXX_CALL testHostQuery(const PluginxxHost*, const PluginxxStringView*) {
    return nullptr; // 测试只用网络表, 不需要别的接口表
}

const PluginxxHostVtable kTestHostVtable = {
    /* alloc */ &testHostAlloc,
    /* free */ &testHostFree,
    /* query_interface */ &testHostQuery,
};

std::string viewOf(const PluginxxStringView* view, std::string_view fallback = {}) {
    if (view == nullptr || view->data == nullptr) {
        return std::string{fallback};
    }
    return std::string{view->data, static_cast<size_t>(view->size)};
}

/// `<应用名>.net` 表的测试实现: 直接接一个本地 `NetHub`
///
/// 生产环境这张表由主程序实现 (属主 = 插件实例名, 事件发到广播频道); 测试里只要
/// "能跑通真实网络路径"就够: 属主固定, 事件丢弃, 完成回调就地执行 (QqApi 拿到的
/// NetResult 后自己投回它的执行器, 所以不依赖主程序线程)。
class TestNet {
public:

    TestNet() :
        TestNet(std::string{}) {}

    explicit TestNet(std::string dataDir) :
        hub_(utilxx::NetHub::create(
            [](const std::string&, const std::string&) {},
            [](std::function<void()> fn) {
                if (fn) {
                    fn();
                }
            }
        )) {
        host_.vtable = &kTestHostVtable;
        host_.opaque = this;
        iface_.version     = PLUGINXX_IFACE_NET_VERSION;
        iface_.struct_size = sizeof(PluginxxNetIface);
        iface_.call        = &TestNet::callThunk;
        iface_.call_async  = &TestNet::callAsyncThunk;
        iface_.cancel      = &TestNet::cancelThunk;
        // 附件下载落盘在 "<dataDir>/<属主>/data/downloads/"; 不设就没法下载
        hub_->setDataDir(std::move(dataDir));
        hub_->start();

        net_.host        = &host_;
        net_.net         = &iface_;
        net_.eventTopic  = agentxx::plugin::netEventTopic(kOwner);
    }

    ~TestNet() {
        if (hub_) {
            hub_->stop();
        }
    }

    TestNet(const TestNet&)            = delete;
    TestNet& operator=(const TestNet&) = delete;

    /// 交给 QqApi 的入口 (地址稳定, 活到本对象结束)
    const agentxx::chatbot::ChatbotNet* chatNet() const {
        return &net_;
    }

private:

    static constexpr const char* kOwner = "chatbot_media_io";

    static void setOut(const PluginxxHost* host, PluginxxString* out, const std::string& text) {
        if (out == nullptr) {
            return;
        }
        const size_t bytes = text.size() + 1;
        auto* mem = static_cast<char*>(host->vtable->alloc(bytes));
        if (mem == nullptr) {
            return;
        }
        std::memcpy(mem, text.data(), text.size());
        mem[text.size()] = '\0';
        out->data = mem;
        out->size = text.size();
    }

    static int32_t PLUGINXX_CALL callThunk(
        const PluginxxHost*       host,
        const PluginxxStringView* op,
        const PluginxxStringView* args,
        PluginxxString*           out
    ) {
        auto* self = static_cast<TestNet*>(host->opaque);
        std::string result;
        const int32_t rc = self->hub_->call(
            kOwner,
            viewOf(op),
            viewOf(args, "{}"),
            result
        );
        if (!result.empty()) {
            setOut(host, out, result);
        }
        return rc;
    }

    /// 完成通知的副本 (notify 本身是调用方栈上的结构, 调用返回就失效)
    struct NotifyCopy {
        PluginxxOperatorNotify notify{};
    };

    static int32_t PLUGINXX_CALL callAsyncThunk(
        const PluginxxHost*           host,
        const PluginxxStringView*     op,
        const PluginxxStringView*     args,
        const PluginxxOperatorNotify* notify,
        PluginxxString*               out,
        int64_t*                      outRequestId
    ) {
        if (notify == nullptr || notify->done == nullptr) {
            return PLUGINXX_NET_ERR_ARG;
        }
        auto* self = static_cast<TestNet*>(host->opaque);
        auto* copy = new NotifyCopy{*notify};
        std::string accepted;
        int64_t     requestId = 0;
        const int32_t rc      = self->hub_->callAsync(
            kOwner,
            viewOf(op),
            viewOf(args, "{}"),
            [copy](utilxx::NetDoneStatus status, const std::string& payload) {
                int32_t code = PLUGINXX_OPERATOR_FAILED;
                if (status == utilxx::NetDoneStatus::Ok) {
                    code = PLUGINXX_OPERATOR_OK;
                } else if (status == utilxx::NetDoneStatus::Canceled) {
                    code = PLUGINXX_OPERATOR_CANCELED;
                }
                PluginxxStringView view{payload.data(), payload.size()};
                copy->notify.done(
                    copy->notify.host_ud,
                    code,
                    payload.empty() ? nullptr : &view
                );
                delete copy;
            },
            accepted,
            &requestId
        );
        if (rc != PLUGINXX_NET_OK) {
            delete copy;
            if (!accepted.empty()) {
                setOut(host, out, accepted);
            }
            return rc;
        }
        if (outRequestId != nullptr) {
            *outRequestId = requestId;
        }
        if (!accepted.empty()) {
            setOut(host, out, accepted);
        }
        return rc;
    }

    static int32_t PLUGINXX_CALL cancelThunk(
        const PluginxxHost* host,
        int64_t             requestId,
        PluginxxString*     out
    ) {
        auto* self = static_cast<TestNet*>(host->opaque);
        std::string result;
        const int32_t rc = self->hub_->cancel(kOwner, requestId, result);
        if (!result.empty()) {
            setOut(host, out, result);
        }
        return rc;
    }

    PluginxxHost           host_{};
    PluginxxNetIface       iface_{};
    std::shared_ptr<utilxx::NetHub> hub_;
    agentxx::chatbot::ChatbotNet   net_;
};

} // namespace

asio::awaitable<TestResult> testChatbotMediaIo() {
    g_cmi_passed = 0;
    g_cmi_failed = 0;

    MockQqServer mock;
    XX_TEST_EXPECT_TRUE(mock.start());
    if (mock.baseUrl().empty()) {
        co_return TestResult{g_cmi_passed, g_cmi_failed};
    }

    const auto dir = makeTempDir();
    // 10 字节 / 分片 4 字节 → 3 片 (4, 4, 2)
    const auto multipart = writeTempFile(dir, "hello_test.txt", "0123456789");
    const auto single    = writeTempFile(dir, "hi.txt", "hi");
    const auto retried   = writeTempFile(dir, "retry.txt", "abcd");

    const auto executor = co_await asio::this_coro::executor;
    TestNet    netService((fs::path{dir} / "net_data").string());
    QqApi      api(netService.chatNet(), executor, "app_id", "app_secret", mock.baseUrl());
    QqMediaUploader uploader(api, "acc_1");

    // ===================== 1. 分片上传: 预上传 → PUT → 分片完成 → 合并 =====================
    {
        const auto mark = mock.mark();
        MediaRef   ref;
        ref.localPath = multipart.path;
        const auto result = co_await uploader.upload(ref, ChatType::Group, "G1");
        XX_TEST_EXPECT_TRUE(result.ok);
        XX_TEST_EXPECT_EQ(result.reason, std::string{});
        XX_TEST_EXPECT_EQ(result.fileInfo, std::string{"fi-merge-upload_test"});
        XX_TEST_EXPECT_EQ(result.fileType, 4);

        // 预上传: file_size 必须是字符串 (发数字真机会按 0 规划分片), 校验值齐全
        const auto prepare = mock.sincePath(mark, "/upload_prepare");
        XX_TEST_EXPECT_EQ(prepare.size(), size_t{1});
        if (prepare.size() == 1) {
            XX_TEST_EXPECT_TRUE(prepare[0].json["file_size"].is_string());
            XX_TEST_EXPECT_EQ(
                prepare[0].json.value("file_size", std::string{}),
                std::string{"10"}
            );
            XX_TEST_EXPECT_EQ(prepare[0].json.value("file_type", 0), 4);
            XX_TEST_EXPECT_EQ(
                prepare[0].json.value("file_name", std::string{}),
                std::string{"hello_test.txt"}
            );
            XX_TEST_EXPECT_EQ(
                prepare[0].json.value("md5", std::string{}),
                utilxx::md5Hex("0123456789")
            );
            // 10 字节不足 10 MB: md5_10m 等于全文 md5
            XX_TEST_EXPECT_EQ(
                prepare[0].json.value("md5_10m", std::string{}),
                utilxx::md5Hex("0123456789")
            );
            XX_TEST_EXPECT_EQ(prepare[0].json.value("sha1", std::string{}).size(), size_t{40});
        }

        // 三片各自上传了自己的区间 (4 / 4 / 2)
        const auto finish = mock.sincePath(mark, "/upload_part_finish");
        XX_TEST_EXPECT_EQ(finish.size(), size_t{3});
        for (size_t i = 0; i < finish.size(); ++i) {
            const std::string expected
                = std::string{"0123456789"}.substr(i * 4, i == 2 ? 2 : 4);
            XX_TEST_EXPECT_EQ(finish[i].json.value("upload_id", std::string{}), std::string{"upload_test"});
            XX_TEST_EXPECT_EQ(finish[i].json.value("part_index", -1), static_cast<int>(i));
            XX_TEST_EXPECT_TRUE(finish[i].json["block_size"].is_string());
            XX_TEST_EXPECT_EQ(
                finish[i].json.value("block_size", std::string{}),
                std::to_string(expected.size())
            );
            XX_TEST_EXPECT_EQ(
                finish[i].json.value("md5", std::string{}),
                utilxx::md5Hex(expected)
            );
        }

        // 合并: 带 upload_id / file_type, 不带 file_data
        const auto merge = mock.sincePath(mark, "/files");
        XX_TEST_EXPECT_EQ(merge.size(), size_t{1});
        if (merge.size() == 1) {
            XX_TEST_EXPECT_EQ(
                merge[0].json.value("upload_id", std::string{}),
                std::string{"upload_test"}
            );
            XX_TEST_EXPECT_EQ(merge[0].json.value("file_type", 0), 4);
            XX_TEST_EXPECT_FALSE(merge[0].json.value("srv_send_msg", true));
            XX_TEST_EXPECT_FALSE(merge[0].json.contains("file_data"));
        }

        // ===================== 2. 同一个文件再发: 命中缓存, 不再传 =====================
        const auto cacheMark = mock.mark();
        MediaRef   again;
        again.localPath   = multipart.path;
        const auto second = co_await uploader.upload(again, ChatType::Group, "G1");
        XX_TEST_EXPECT_TRUE(second.ok);
        XX_TEST_EXPECT_EQ(second.fileInfo, std::string{"fi-merge-upload_test"});
        XX_TEST_EXPECT_EQ(mock.since(cacheMark).size(), size_t{0});
    }

    // ===================== 3. 平台不给分片清单: 小文件走二进制直传 =====================
    {
        mock.noParts = true;
        const auto mark = mock.mark();
        MediaRef   ref;
        ref.localPath = single.path;
        const auto result = co_await uploader.upload(ref, ChatType::Group, "G2");
        mock.noParts      = false;
        XX_TEST_EXPECT_TRUE(result.ok);
        XX_TEST_EXPECT_EQ(result.fileInfo, std::string{"fi-direct"});

        const auto direct = mock.sincePath(mark, "/files");
        XX_TEST_EXPECT_EQ(direct.size(), size_t{1});
        if (direct.size() == 1) {
            // 直传不能带 upload_id (带了服务端走合并路径, 与 file_data 冲突)
            XX_TEST_EXPECT_FALSE(direct[0].json.contains("upload_id"));
            XX_TEST_EXPECT_EQ(direct[0].json.value("file_type", 0), 4);
            XX_TEST_EXPECT_TRUE(direct[0].json["file_data"].is_string());
            XX_TEST_EXPECT_FALSE(direct[0].json.value("srv_send_msg", true));
        }
        // 直传路径不该有分片请求
        XX_TEST_EXPECT_EQ(mock.sincePath(mark, "/upload_part_finish").size(), size_t{0});
    }

    // ===================== 4. 5xx 重试: 第一次失败, 第二次成功 =====================
    {
        mock.transientFailureCount = 1;
        const auto mark            = mock.mark();
        MediaRef   ref;
        ref.localPath = retried.path;
        const auto result = co_await uploader.upload(ref, ChatType::Group, "G3");
        XX_TEST_EXPECT_TRUE(result.ok);
        XX_TEST_EXPECT_EQ(result.fileInfo, std::string{"fi-merge-upload_test"});
        // 同一片重试了两次 (500 → 200)
        XX_TEST_EXPECT_EQ(mock.sincePath(mark, "/upload_part_finish").size(), size_t{2});
        // 分片只上传了一次: 重试只重发「分片完成」, 不重复传字节
        XX_TEST_EXPECT_EQ(mock.sincePath(mark, "/cos/p0").size(), size_t{1});
        XX_TEST_EXPECT_EQ(mock.sincePath(mark, "/files").size(), size_t{1});
    }

    // ===================== 5. 4xx 不重试: 直接失败并给出原因 =====================
    {
        uploader.clearCache();
        mock.rejectPartFinish = true;
        const auto mark       = mock.mark();
        MediaRef   ref;
        ref.localPath = retried.path;
        const auto result = co_await uploader.upload(ref, ChatType::Group, "G3");
        mock.rejectPartFinish = false;
        XX_TEST_EXPECT_FALSE(result.ok);
        XX_TEST_EXPECT_TRUE(result.reason.find("upload_part_finish") != std::string::npos);
        // 参数错误重试没有意义: 只尝试一次
        XX_TEST_EXPECT_EQ(mock.sincePath(mark, "/upload_part_finish").size(), size_t{1});
        // 失败时不会去合并
        XX_TEST_EXPECT_EQ(mock.sincePath(mark, "/files").size(), size_t{0});
    }

    // ===================== 6. 附件下载: 走主程序的 http.download 落盘 =====================
    {
        const std::string target = (fs::path{dir} / "downloaded.bin").string();
        const auto        error
            = netService.chatNet()->download(mock.baseUrl() + "/download/blob", target, 0, 20000);
        XX_TEST_EXPECT_EQ(error, std::string{});
        XX_TEST_EXPECT_TRUE(fs::exists(target));
        std::ifstream in(target, std::ios::binary);
        const std::string bytes{
            std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()
        };
        // 字节原样: 没经过 JSON / base64 转换
        XX_TEST_EXPECT_EQ(bytes, blobContent());

        // 404 与连不上都要给出可读原因 (调用方据此保留平台地址)
        const auto notFound = netService.chatNet()->download(
            mock.baseUrl() + "/download/missing",
            target + ".404",
            0,
            20000
        );
        XX_TEST_EXPECT_FALSE(notFound.empty());
        const auto unreachable
            = netService.chatNet()->download("http://127.0.0.1:1/x", target + ".noport", 0, 5000);
        XX_TEST_EXPECT_FALSE(unreachable.empty());
        XX_TEST_EXPECT_FALSE(fs::exists(target + ".404"));
    }

    mock.stop();
    std::error_code ec;
    fs::remove_all(dir, ec);

    co_return TestResult{g_cmi_passed, g_cmi_failed};
}

} // namespace test
} // namespace agentxx
