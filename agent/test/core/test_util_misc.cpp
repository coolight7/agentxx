#include "test_util_misc.h"

#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx/agent/io/agent_io_transport.h"
#include "agentxx/agent/io/wire_protocol.h"
#include "agentxx/util/container_util.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/http_header.h"
#include "agentxx/util/path_sanitize.h"
#include "agentxx/util/stream.h"
#include "agentxx/util/util.h"
#include <chrono>
#include <set>
#include <stdexcept>
#include <thread>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_um_passed = 0;
int g_um_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_um_passed
#define XX_TEST_FAILED g_um_failed

namespace agentxx {
namespace test {

// ---------------------------------------------------------------------------
// HeaderMap
// ---------------------------------------------------------------------------

void test_header_map_basic() {
    agentxx::util::HeaderMap hm;

    XX_TEST_EXPECT_TRUE(hm.empty());
    XX_TEST_EXPECT_FALSE(hm.contains("Content-Type"));

    // set 单值
    hm.set("Content-Type", "application/json");
    XX_TEST_EXPECT_FALSE(hm.empty());
    XX_TEST_EXPECT_TRUE(hm.contains("content-type")); // 忽略大小写
    XX_TEST_EXPECT_TRUE(hm.contains("CONTENT-TYPE"));
    XX_TEST_EXPECT_EQ(hm.getSingle("Content-Type"), std::string_view("application/json"));
    XX_TEST_EXPECT_EQ(hm.getSingle("CONTENT-TYPE"), std::string_view("application/json"));

    // 覆盖
    hm.set("content-type", "text/plain");
    XX_TEST_EXPECT_EQ(hm.getSingle("Content-Type"), std::string_view("text/plain"));

    // set 多值
    hm.set("Accept", std::vector<std::string>{"application/json", "text/html"});
    auto it = hm.get("Accept");
    XX_TEST_EXPECT_EQ(it->second.size(), (size_t)2);
    XX_TEST_EXPECT_EQ(it->second[0], std::string("application/json"));
    XX_TEST_EXPECT_EQ(it->second[1], std::string("text/html"));
    // getSingle 返回首值
    XX_TEST_EXPECT_EQ(hm.getSingle("accept"), std::string_view("application/json"));
}

void test_header_map_get_creates() {
    agentxx::util::HeaderMap hm;
    // get 不存在的 name: 插入空 vector 并返回迭代器
    auto it = hm.get("X-New-Header");
    XX_TEST_EXPECT_TRUE(it->second.empty());
    XX_TEST_EXPECT_TRUE(hm.contains("x-new-header"));

    // 获取不存在的单值: 空串
    XX_TEST_EXPECT_EQ(hm.getSingle("X-Missing"), std::string_view(""));
    // 已存在但值为空: 空串
    XX_TEST_EXPECT_EQ(hm.getSingle("X-New-Header"), std::string_view(""));
}

void test_header_map_ctor_with_data() {
    agentxx::util::IgnoreCaseMap<std::vector<std::string>> data;
    data["Content-Type"] = {"application/json"};
    agentxx::util::HeaderMap hm{data};
    XX_TEST_EXPECT_TRUE(hm.contains("content-type"));
    XX_TEST_EXPECT_EQ(hm.getSingle("CONTENT-TYPE"), std::string_view("application/json"));
}

// ---------------------------------------------------------------------------
// catchError
// ---------------------------------------------------------------------------

void test_catch_error_success() {
    auto result = agentxx::util::catchError<int>(
        []() -> int {
            return 42;
        },
        [](std::string errmsg) -> int {
            XX_TEST_EXPECT_TRUE(false); // 不应调用
            return -1;
        }
    );
    XX_TEST_EXPECT_EQ(result, 42);
}

void test_catch_error_std_exception() {
    std::string gotErr;
    auto        result = agentxx::util::catchError<int>(
        []() -> int {
            throw std::runtime_error("boom");
        },
        [&](std::string errmsg) -> int {
            gotErr = std::move(errmsg);
            return -1;
        }
    );
    XX_TEST_EXPECT_EQ(result, -1);
    XX_TEST_EXPECT_TRUE(gotErr.find("boom") != std::string::npos);
}

void test_catch_error_unknown() {
    std::string gotErr;
    auto        result = agentxx::util::catchError<int>(
        []() -> int {
            throw 123; // 非 std::exception
        },
        [&](std::string errmsg) -> int {
            gotErr = std::move(errmsg);
            return -1;
        }
    );
    XX_TEST_EXPECT_EQ(result, -1);
    XX_TEST_EXPECT_EQ(gotErr, std::string("unknown exception"));
}

// ---------------------------------------------------------------------------
// 系统工具
// ---------------------------------------------------------------------------

void test_system_utils() {
    // getSystemName 不应为空
    auto name = agentxx::util::getSystemName();
    XX_TEST_EXPECT_FALSE(name.empty());

    // isRunningInWSL 应返回 bool (不崩溃)
    (void)agentxx::util::isRunningInWSL();
}

// ---------------------------------------------------------------------------
// 文件异步 I/O 可用性 (util/util.h)
// ---------------------------------------------------------------------------

void test_async_file_io_support() {
    // 自动探测: 结果按进程缓存, 重复调用应一致 (首次调用即触发探测)
    const bool detected = agentxx::util::isAsyncFileIoSupported();
    XX_TEST_EXPECT_EQ(agentxx::util::isAsyncFileIoSupported(), detected);
    XX_TEST_EXPECT_EQ(agentxx::util::isAsyncFileIoSupported(), detected);

    // 强制关闭: 判断结果直接为 false (测试据此覆盖同步兜底实现)
    agentxx::util::setAsyncFileIoSupported(false);
    XX_TEST_EXPECT_FALSE(agentxx::util::isAsyncFileIoSupported());

    // 强制开启: 覆盖为 true
    agentxx::util::setAsyncFileIoSupported(true);
    XX_TEST_EXPECT_TRUE(agentxx::util::isAsyncFileIoSupported());

    // 恢复自动探测: 回到探测值 (与强制开启的结果无必然关系)
    agentxx::util::resetAsyncFileIoSupported();
    XX_TEST_EXPECT_EQ(agentxx::util::isAsyncFileIoSupported(), detected);

    // 重复恢复幂等
    agentxx::util::resetAsyncFileIoSupported();
    XX_TEST_EXPECT_EQ(agentxx::util::isAsyncFileIoSupported(), detected);
}

// ---------------------------------------------------------------------------
// util/stream.h: Throttle (节流) / Debounce (防抖)
// ---------------------------------------------------------------------------
void test_stream_throttle_debounce() {
    using namespace std::chrono;

    // --- Throttle: 最小放行间隔 ---
    {
        agentxx::util::Throttle throttle(seconds{1});
        // 首次调用恒放行
        XX_TEST_EXPECT_TRUE(throttle.try_acquire());
        // 间隔内不放行
        XX_TEST_EXPECT_FALSE(throttle.try_acquire());
        // 剩余时间在 (0, interval] 内
        auto remaining = throttle.time_until_acquire();
        XX_TEST_EXPECT_TRUE(remaining > std::chrono::steady_clock::duration::zero());
        XX_TEST_EXPECT_TRUE(remaining <= seconds{1});
        // 等待满间隔后放行
        std::this_thread::sleep_for(milliseconds{1100});
        XX_TEST_EXPECT_TRUE(throttle.try_acquire());
    }
    {
        // force() 计为一次放行: 之后立即 try_acquire 不放行
        agentxx::util::Throttle throttle(seconds{1});
        throttle.force();
        XX_TEST_EXPECT_FALSE(throttle.try_acquire());
        // 从未放行时 time_until_acquire 为 0
        agentxx::util::Throttle fresh(seconds{1});
        XX_TEST_EXPECT_TRUE(
            fresh.time_until_acquire() == std::chrono::steady_clock::duration::zero()
        );
    }

    // --- Debounce: 静默满 wait 后才 ready, 期间触发重置计时 ---
    {
        agentxx::util::Debounce debounce(milliseconds{200});
        // 未触发过: 不 ready
        XX_TEST_EXPECT_FALSE(debounce.ready());
        debounce.trigger();
        std::this_thread::sleep_for(milliseconds{100});
        debounce.trigger(); // 重置计时
        std::this_thread::sleep_for(milliseconds{150});
        // 距最后一次触发 150ms < 200ms: 不 ready
        XX_TEST_EXPECT_FALSE(debounce.ready());
        std::this_thread::sleep_for(milliseconds{100});
        // 静默满 200ms: ready
        XX_TEST_EXPECT_TRUE(debounce.ready());
        debounce.reset();
        XX_TEST_EXPECT_FALSE(debounce.ready());
    }
}

// ---------------------------------------------------------------------------
// util/container_util.h 异构关联容器操作测试
// ---------------------------------------------------------------------------
void test_container_util_heterogeneous() {
    // 1. eraseHeterogeneous
    {
        std::map<std::string, int, std::less<>> m;
        m["hello"] = 42;
        m["world"] = 100;

        XX_TEST_EXPECT_TRUE(agentxx::util::eraseHeterogeneous(m, std::string_view("hello")));
        XX_TEST_EXPECT_EQ(m.size(), (size_t)1);
        XX_TEST_EXPECT_FALSE(agentxx::util::eraseHeterogeneous(m, std::string_view("non_exist")));
        XX_TEST_EXPECT_EQ(m.size(), (size_t)1);

        std::set<std::string, std::less<>> s{"apple", "banana"};
        XX_TEST_EXPECT_TRUE(agentxx::util::eraseHeterogeneous(s, std::string_view("apple")));
        XX_TEST_EXPECT_FALSE(agentxx::util::eraseHeterogeneous(s, std::string_view("orange")));
        XX_TEST_EXPECT_EQ(s.size(), (size_t)1);
    }

    // 2. insertHeterogeneous
    {
        std::map<std::string, std::string, std::less<>> m;
        // string_view 参数
        auto [it1, ok1] = agentxx::util::insertHeterogeneous(m, "k1", "v1");
        XX_TEST_EXPECT_TRUE(ok1);
        XX_TEST_EXPECT_EQ(it1->second, std::string("v1"));

        // 重复 key 不覆盖
        auto [it2, ok2] = agentxx::util::insertHeterogeneous(m, "k1", "v2_ignored");
        XX_TEST_EXPECT_FALSE(ok2);
        XX_TEST_EXPECT_EQ(it2->second, std::string("v1"));

        // 右值 string (move 复用)
        std::string moveKey = "k2";
        std::string moveVal = "v2";
        auto [it3, ok3]
            = agentxx::util::insertHeterogeneous(m, std::move(moveKey), std::move(moveVal));
        XX_TEST_EXPECT_TRUE(ok3);
        XX_TEST_EXPECT_EQ(it3->second, std::string("v2"));

        // set 测试
        std::set<std::string, std::less<>> s;
        auto [sit1, sok1] = agentxx::util::insertHeterogeneous(s, "item1");
        XX_TEST_EXPECT_TRUE(sok1);
        auto [sit2, sok2] = agentxx::util::insertHeterogeneous(s, "item1");
        XX_TEST_EXPECT_FALSE(sok2);
    }

    // 3. insertOrAssignHeterogeneous / overwriteHeterogeneous
    {
        std::map<std::string, int, std::less<>> m;
        // 新建插入
        auto [it1, inserted1]
            = agentxx::util::insertOrAssignHeterogeneous(m, std::string_view("score"), 100);
        XX_TEST_EXPECT_TRUE(inserted1);
        XX_TEST_EXPECT_EQ(it1->second, 100);

        // 已存在时覆盖
        auto [it2, inserted2]
            = agentxx::util::insertOrAssignHeterogeneous(m, std::string_view("score"), 200);
        XX_TEST_EXPECT_FALSE(inserted2);
        XX_TEST_EXPECT_EQ(it2->second, 200);
        XX_TEST_EXPECT_EQ(m["score"], 200);

        // overwriteHeterogeneous 别名测试
        auto [it3, inserted3] = agentxx::util::overwriteHeterogeneous(m, "score", 300);
        XX_TEST_EXPECT_FALSE(inserted3);
        XX_TEST_EXPECT_EQ(it3->second, 300);
        XX_TEST_EXPECT_EQ(m["score"], 300);

        // 右值 string 覆盖
        std::string rvalKey   = "rkey";
        auto [it4, inserted4] = agentxx::util::overwriteHeterogeneous(m, std::move(rvalKey), 400);
        XX_TEST_EXPECT_TRUE(inserted4);
        XX_TEST_EXPECT_EQ(it4->second, 400);
    }

    // 4. getOrCreateHeterogeneous
    {
        std::map<std::string, std::vector<int>, std::less<>> m;
        // 不存在则默认构造插入
        auto& vec = agentxx::util::getOrCreateHeterogeneous(m, std::string_view("numbers"));
        XX_TEST_EXPECT_TRUE(vec.empty());
        vec.push_back(1);
        vec.push_back(2);

        // 再次获取得到相同引用
        auto& vec2 = agentxx::util::getOrCreateHeterogeneous(m, "numbers");
        XX_TEST_EXPECT_EQ(vec2.size(), (size_t)2);
        XX_TEST_EXPECT_EQ(vec2[0], 1);
        XX_TEST_EXPECT_EQ(vec2[1], 2);
    }
}

// ---------------------------------------------------------------------------
// 路径段安全化 (path_sanitize.h)
// ---------------------------------------------------------------------------

void test_path_segment_sanitize() {
    using agentxx::util::sanitizeFsSegment;
    using agentxx::util::truncateFsSegment;
    using agentxx::util::truncateFsSegmentWithHash;

    // 非法字符替换为 '_' (长度不变)
    XX_TEST_EXPECT_EQ(
        sanitizeFsSegment("a/b\\c:d*e?f\"g<h>i|j"),
        std::string("a_b_c_d_e_f_g_h_i_j")
    );
    XX_TEST_EXPECT_EQ(
        sanitizeFsSegment("a\x01\x1f"
                          "b"),
        std::string("a__b")
    );
    // 合法字符 (含 UTF-8 中文) 原样保留
    XX_TEST_EXPECT_EQ(sanitizeFsSegment("会话-01.ok"), std::string("会话-01.ok"));
    XX_TEST_EXPECT_EQ(sanitizeFsSegment(""), std::string(""));
    XX_TEST_EXPECT_EQ(sanitizeFsSegment("..."), std::string("..."));

    // 纯截断: 超长才截断, 不追加尾缀
    XX_TEST_EXPECT_EQ(truncateFsSegment("abcdef", 10), std::string("abcdef"));
    XX_TEST_EXPECT_EQ(truncateFsSegment("abcdef", 6), std::string("abcdef"));
    XX_TEST_EXPECT_EQ(truncateFsSegment("abcdef", 3), std::string("abc"));
    XX_TEST_EXPECT_EQ(truncateFsSegment("abcdef", 0), std::string(""));

    // 截断 + 哈希尾缀: 长度不超过上限, 前部可读, 尾缀 8 位 hex
    {
        std::string longSeg(100, 'x');
        auto        out = truncateFsSegmentWithHash(longSeg, 48);
        XX_TEST_EXPECT_EQ(out.size(), (size_t)48);
        XX_TEST_EXPECT_EQ(out.substr(0, 39), std::string(39, 'x'));
        XX_TEST_EXPECT_EQ(out[39], '_');
        XX_TEST_EXPECT_TRUE(
            out.substr(40).find_first_not_of("0123456789abcdef") == std::string::npos
        );
        // 确定性: 相同输入相同输出
        XX_TEST_EXPECT_EQ(truncateFsSegmentWithHash(longSeg, 48), out);
        // 不同输入 (前部相同) 尾缀不同, 避免截断后碰撞到同一目录
        std::string other = longSeg;
        other.back()      = 'y';
        XX_TEST_EXPECT_TRUE(truncateFsSegmentWithHash(other, 48) != out);
        // 未超长 / 上限过小: 原样 / 退化为纯截断
        XX_TEST_EXPECT_EQ(truncateFsSegmentWithHash("abcdef", 48), std::string("abcdef"));
        XX_TEST_EXPECT_EQ(truncateFsSegmentWithHash("abcdef", 9), std::string("abcdef"));
        XX_TEST_EXPECT_EQ(truncateFsSegmentWithHash("abcdef", 3), std::string("abc"));
        // 哈希源与截断内容不同的情况 (调用方传原始文本)
        XX_TEST_EXPECT_TRUE(
            truncateFsSegmentWithHash(longSeg, 48, "src-a")
            != truncateFsSegmentWithHash(longSeg, 48, "src-b")
        );
    }
}

void test_windows_reserved_name() {
    using agentxx::util::isWindowsReservedName;

    // 保留设备名 (大小写不敏感, 忽略扩展名)
    XX_TEST_EXPECT_TRUE(isWindowsReservedName("CON"));
    XX_TEST_EXPECT_TRUE(isWindowsReservedName("con"));
    XX_TEST_EXPECT_TRUE(isWindowsReservedName("Con"));
    XX_TEST_EXPECT_TRUE(isWindowsReservedName("con.txt"));
    XX_TEST_EXPECT_TRUE(isWindowsReservedName("NUL"));
    XX_TEST_EXPECT_TRUE(isWindowsReservedName("com1"));
    XX_TEST_EXPECT_TRUE(isWindowsReservedName("COM9"));
    XX_TEST_EXPECT_TRUE(isWindowsReservedName("lpt9.log"));
    // 非保留名
    XX_TEST_EXPECT_FALSE(isWindowsReservedName("COM10"));
    XX_TEST_EXPECT_FALSE(isWindowsReservedName("CONS"));
    XX_TEST_EXPECT_FALSE(isWindowsReservedName("console"));
    XX_TEST_EXPECT_FALSE(isWindowsReservedName("session"));
    XX_TEST_EXPECT_FALSE(isWindowsReservedName(""));
    XX_TEST_EXPECT_FALSE(isWindowsReservedName("会话"));
}

void test_md5_and_device_id() {
    using agentxx::util::getDeviceId;
    using agentxx::util::md5Hex;

    // 标准 MD5 校验向量
    XX_TEST_EXPECT_EQ(md5Hex(""), std::string("d41d8cd98f00b204e9800998ecf8427e"));
    XX_TEST_EXPECT_EQ(md5Hex("hello"), std::string("5d41402abc4b2a76b9719d911017c592"));
    XX_TEST_EXPECT_EQ(
        md5Hex("The quick brown fox jumps over the lazy dog"),
        std::string("9e107d9d372bb6826bd81d3542a419d6")
    );

    // getDeviceId
    auto devId = getDeviceId();
    XX_TEST_EXPECT_EQ(devId.size(), 32);
    for (char c : devId) {
        bool validHex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        XX_TEST_EXPECT_TRUE(validHex);
    }
    // 进程内缓存验证
    XX_TEST_EXPECT_EQ(getDeviceId(), devId);
}

void test_wire_list_dir_protocol() {
    using namespace agentxx::agent;
    using namespace agentxx::agent::io;

    // 1. WireListDir 序列化与反序列化
    WireListDir req;
    req.reqId             = 42;
    req.path              = "/home/test/workspace";
    req.allowedExtensions = {".png", ".jpg", ".wav"};

    auto reqJson = toJson(req);
    XX_TEST_EXPECT_EQ(reqJson["type"].get<std::string>(), std::string(MsgType::ListDir));
    XX_TEST_EXPECT_EQ(reqJson["reqId"].get<uint64_t>(), uint64_t{42});
    XX_TEST_EXPECT_EQ(reqJson["path"].get<std::string>(), std::string("/home/test/workspace"));

    auto deserReq = listDirFromJson(reqJson);
    XX_TEST_EXPECT_EQ(deserReq.reqId, uint64_t{42});
    XX_TEST_EXPECT_EQ(deserReq.path, std::string("/home/test/workspace"));
    XX_TEST_EXPECT_EQ(deserReq.allowedExtensions.size(), size_t{3});

    // 2. WireListDirResult 序列化与反序列化
    WireListDirResult res;
    res.reqId      = 42;
    res.ok         = true;
    res.currentDir = "/home/test/workspace";
    res.parentDir  = "/home/test";

    WireDirEntry de1;
    de1.name      = "src";
    de1.fullPath  = "/home/test/workspace/src";
    de1.isDir     = true;
    de1.supported = true;
    res.entries.push_back(std::move(de1));

    WireDirEntry de2;
    de2.name      = "photo.png";
    de2.fullPath  = "/home/test/workspace/photo.png";
    de2.isDir     = false;
    de2.supported = true;
    de2.sizeBytes = 2048;
    de2.mediaType = MediaType::Image;
    res.entries.push_back(std::move(de2));

    auto resJson = toJson(res);
    XX_TEST_EXPECT_EQ(resJson["type"].get<std::string>(), std::string(MsgType::ListDirResult));
    XX_TEST_EXPECT_TRUE(resJson["ok"].get<bool>());
    XX_TEST_EXPECT_EQ(resJson["entries"].size(), size_t{2});

    auto deserRes = listDirResultFromJson(resJson);
    XX_TEST_EXPECT_EQ(deserRes.reqId, uint64_t{42});
    XX_TEST_EXPECT_TRUE(deserRes.ok);
    XX_TEST_EXPECT_EQ(deserRes.entries.size(), size_t{2});
    XX_TEST_EXPECT_EQ(deserRes.entries[0].name, std::string("src"));
    XX_TEST_EXPECT_TRUE(deserRes.entries[0].isDir);
    XX_TEST_EXPECT_EQ(deserRes.entries[1].name, std::string("photo.png"));
    XX_TEST_EXPECT_FALSE(deserRes.entries[1].isDir);
    XX_TEST_EXPECT_EQ(deserRes.entries[1].sizeBytes, uint64_t{2048});
    XX_TEST_EXPECT_EQ(deserRes.entries[1].mediaType, MediaType::Image);

    // 3. 顶层 serialize / deserialize 验证
    WireMessage wireMsg  = res;
    std::string text     = serialize(wireMsg);
    auto        deserMsg = deserialize(text);
    XX_TEST_EXPECT_TRUE(deserMsg.has_value());
    auto* p = std::get_if<WireListDirResult>(&deserMsg.value());
    XX_TEST_EXPECT_TRUE(p != nullptr);
    if (p) {
        XX_TEST_EXPECT_EQ(p->reqId, uint64_t{42});
        XX_TEST_EXPECT_EQ(p->entries.size(), size_t{2});
    }
}

void test_cross_device_determination() {
    agentxx::client::TUICtx ctx;

    // 内置模式: remoteUrl 为空 -> 始终判定为同设备 (false)
    ctx.remoteUrl      = "";
    ctx.clientDeviceId = "device_aaa";
    ctx.serverDeviceId = "device_bbb";
    XX_TEST_EXPECT_FALSE(ctx.isServerDifferentDevice());

    // 远程模式: remoteUrl 非空
    ctx.remoteUrl = "ws://192.168.1.100:8080";

    // 相同 deviceId -> 同一设备 (false)
    ctx.clientDeviceId = "device_same_12345";
    ctx.serverDeviceId = "device_same_12345";
    XX_TEST_EXPECT_FALSE(ctx.isServerDifferentDevice());

    // 不同 deviceId -> 跨设备 (true)
    ctx.clientDeviceId = "device_client_aaa";
    ctx.serverDeviceId = "device_server_bbb";
    XX_TEST_EXPECT_TRUE(ctx.isServerDifferentDevice());

    // 未收到 deviceId (空值) -> 判定为同设备 (false, 防误开 tab)
    ctx.clientDeviceId = "";
    ctx.serverDeviceId = "device_server_bbb";
    XX_TEST_EXPECT_FALSE(ctx.isServerDifferentDevice());
}

TestResult testUtilMisc() {
    g_um_passed = 0;
    g_um_failed = 0;

    test_header_map_basic();
    test_header_map_get_creates();
    test_header_map_ctor_with_data();
    test_catch_error_success();
    test_catch_error_std_exception();
    test_catch_error_unknown();
    test_system_utils();
    test_async_file_io_support();
    test_stream_throttle_debounce();
    test_container_util_heterogeneous();
    test_path_segment_sanitize();
    test_windows_reserved_name();
    test_md5_and_device_id();
    test_wire_list_dir_protocol();
    test_cross_device_determination();

    return TestResult{g_um_passed, g_um_failed};
}

} // namespace test
} // namespace agentxx
