///
/// `chatbot_media` —— QQ 富媒体上传的请求体与分片规划回归
///
/// 为什么单独一层用例: 这些地方错了不会报编译错, 也不会在本地看出问题 ——
/// 现象只有"机器人主动发文件失败: upload_part_finish 失败 (status 500)"。
/// 下面每条断言都对应一个**真实踩过的坑**:
/// - `block_size` 是字符串, 取成 0 → 分片规划退化成一片;
/// - `upload_part_finish` 少给 `block_size` / `md5` → 服务端 500;
/// - 直传请求体里混进 `upload_id` → 服务端走合并路径。
#include "agentxx-test/core/test_chatbot_media.h"

#include "agentxx_chatbot/platform/qq/qq_media_request.h"

#include "utilxx_base/json.h"

#include <string>

namespace {
int g_cm_passed = 0;
int g_cm_failed = 0;
} // namespace

#define XX_TEST_PASSED g_cm_passed
#define XX_TEST_FAILED g_cm_failed

namespace agentxx {
namespace test {

namespace {

using namespace agentxx::chatbot;
using utilxx_base::Json;

constexpr size_t kTenMb = 10485760;

/// 平台下发的预上传响应 (官方文档示例原样; `block_size` 与 `parts[].block_size`
/// 都是**字符串**, `parts[].index` 是整数)
constexpr const char* kPrepareResponse = R"JSON({
  "upload_id": "upload_a1b2c3d4e5f6",
  "block_size": "10485760",
  "parts": [
    {"index": 0, "presigned_url": "https://cos.example.com/upload?partNumber=1&sign=aaa",
     "block_size": "10485760"},
    {"index": 1, "presigned_url": "https://cos.example.com/upload?partNumber=2&sign=bbb",
     "block_size": "10485760"},
    {"index": 2, "presigned_url": "https://cos.example.com/upload?partNumber=3&sign=ccc",
     "block_size": "10485760"}
  ],
  "upload_config": {"concurrency": 1, "retry_timeout": 300, "retry_delay": 1}
})JSON";

QqFileDigests sampleDigests() {
    QqFileDigests digests;
    digests.md5     = "d41d8cd98f00b204e9800998ecf8427e";
    digests.sha1    = "da39a3ee5e6b4b0d3255bfef95601890afd80709";
    digests.md5_10m = "c4d8c5f3a2b1e0f9a8b7c6d5e4f3a2b1";
    return digests;
}

} // namespace

TestResult testChatbotMedia() {
    g_cm_passed = 0;
    g_cm_failed = 0;

    // ===================== 1. 字符串数字必须能取出来 =====================
    {
        XX_TEST_EXPECT_EQ(qqJsonInt64(Json::parse("\"10485760\"")), int64_t{10485760});
        XX_TEST_EXPECT_EQ(qqJsonInt64(Json::parse("10485760")), int64_t{10485760});
        // 带首尾空白也认 (平台出现过 `" 5242880"`)
        XX_TEST_EXPECT_EQ(qqJsonInt64(Json::parse("\" 5242880 \"")), int64_t{5242880});
        // 不是数字的一律算解析失败, 交给调用方的默认值
        XX_TEST_EXPECT_EQ(qqJsonInt64(Json::parse("\"10 MB\"")), int64_t{0});
        XX_TEST_EXPECT_EQ(qqJsonInt64(Json::parse("null")), int64_t{0});

        const auto config = Json::parse(R"JSON({"retry_delay": "2", "concurrency": 3})JSON");
        XX_TEST_EXPECT_EQ(qqJsonInt64Of(config, "retry_delay", 1), int64_t{2});
        XX_TEST_EXPECT_EQ(qqJsonInt64Of(config, "concurrency", 1), int64_t{3});
        // 键缺失 / 值非法 → 用调用方给的默认值
        XX_TEST_EXPECT_EQ(qqJsonInt64Of(config, "retry_timeout", 300), int64_t{300});
        XX_TEST_EXPECT_EQ(qqJsonInt64Of(Json::parse("[]"), "retry_delay", 7), int64_t{7});
    }

    // ===================== 2. 分片规划: 字符串 block_size 要真的切开 =====================
    // 25 MB 文件按 10 MB 分片: 平台给 3 片, 最后一片只有 5 MB (越界部分要裁掉)
    {
        const auto plan = qqPlanUploadParts(Json::parse(kPrepareResponse), 25 * 1024 * 1024);
        XX_TEST_EXPECT_EQ(plan.size(), size_t{3});
        if (plan.size() == 3) {
            XX_TEST_EXPECT_EQ(plan[0].index, 0);
            XX_TEST_EXPECT_EQ(plan[0].offset, size_t{0});
            XX_TEST_EXPECT_EQ(plan[0].sizeBytes, kTenMb);
            XX_TEST_EXPECT_EQ(
                plan[0].url,
                std::string{"https://cos.example.com/upload?partNumber=1&sign=aaa"}
            );
            XX_TEST_EXPECT_EQ(plan[1].index, 1);
            XX_TEST_EXPECT_EQ(plan[1].offset, kTenMb);
            XX_TEST_EXPECT_EQ(plan[1].sizeBytes, kTenMb);
            XX_TEST_EXPECT_EQ(plan[2].index, 2);
            XX_TEST_EXPECT_EQ(plan[2].offset, kTenMb * 2);
            // 只有 5 MB 可传: 不能按平台预告的 10 MB 去切片 (会读到空数据)
            XX_TEST_EXPECT_EQ(plan[2].sizeBytes, size_t{5} * 1024 * 1024);
        }
    }

    // ===================== 3. 小文件 (就是报错的那次): 只传一片 =====================
    // 平台按 10 MB 预告了 3 片, 但 hello_test.txt 只有 128 字节 → 只留第一片
    {
        const auto plan = qqPlanUploadParts(Json::parse(kPrepareResponse), 128);
        XX_TEST_EXPECT_EQ(plan.size(), size_t{1});
        if (plan.size() == 1) {
            XX_TEST_EXPECT_EQ(plan[0].index, 0);
            XX_TEST_EXPECT_EQ(plan[0].offset, size_t{0});
            XX_TEST_EXPECT_EQ(plan[0].sizeBytes, size_t{128});
        }
    }

    // ===================== 4. block_size 给成数字也照常工作 =====================
    {
        const auto response = Json::parse(R"JSON({
          "upload_id": "u1",
          "block_size": 1048576,
          "parts": [
            {"index": 0, "presigned_url": "https://cos/1", "block_size": 1048576},
            {"index": 1, "presigned_url": "https://cos/2"}
          ]
        })JSON");
        const auto plan = qqPlanUploadParts(response, 2 * 1024 * 1024);
        XX_TEST_EXPECT_EQ(plan.size(), size_t{2});
        if (plan.size() == 2) {
            XX_TEST_EXPECT_EQ(plan[0].sizeBytes, size_t{1024 * 1024});
            XX_TEST_EXPECT_EQ(plan[1].offset, size_t{1024 * 1024});
            // 第二片没给 block_size: 回退到文件级 block_size
            XX_TEST_EXPECT_EQ(plan[1].sizeBytes, size_t{1024 * 1024});
        }
    }

    // ===================== 5. 没给分片清单: 交给调用方走二进制直传 =====================
    {
        XX_TEST_EXPECT_TRUE(
            qqPlanUploadParts(Json::parse(R"JSON({"upload_id": "u1"})JSON"), 100).empty()
        );
        XX_TEST_EXPECT_TRUE(
            qqPlanUploadParts(Json::parse(R"JSON({"upload_id": "u1", "parts": []})JSON"), 100)
                .empty()
        );
        // 空文件没有可分片的内容
        XX_TEST_EXPECT_TRUE(qqPlanUploadParts(Json::parse(kPrepareResponse), 0).empty());
        // 没有 URL 的分片必须丢掉, 否则会去 PUT 空地址
        XX_TEST_EXPECT_TRUE(
            qqPlanUploadParts(
                Json::parse(
                    R"JSON({"upload_id":"u1","block_size":"10","parts":[{"index":0}]})JSON"
                ),
                10
            )
                .empty()
        );
    }

    // ===================== 6. 序号从 1 开始 / 乱序: 原样回传序号, 偏移按升序累加 =====================
    {
        // 有的灰度环境 `parts[].index` 从 1 开始; 回给 `upload_part_finish` 的必须是平台给的值
        const auto fromOne = qqPlanUploadParts(
            Json::parse(R"JSON({
              "upload_id": "u1",
              "block_size": "10",
              "parts": [
                {"index": 1, "presigned_url": "https://cos/1", "block_size": "10"},
                {"index": 2, "presigned_url": "https://cos/2", "block_size": "10"}
              ]
            })JSON"),
            20
        );
        XX_TEST_EXPECT_EQ(fromOne.size(), size_t{2});
        if (fromOne.size() == 2) {
            XX_TEST_EXPECT_EQ(fromOne[0].index, 1);
            XX_TEST_EXPECT_EQ(fromOne[0].offset, size_t{0});
            XX_TEST_EXPECT_EQ(fromOne[1].index, 2);
            XX_TEST_EXPECT_EQ(fromOne[1].offset, size_t{10});
        }

        // 平台把分片顺序给乱了: 偏移必须按序号升序排, 否则每片都传到错的位置
        const auto unsorted = qqPlanUploadParts(
            Json::parse(R"JSON({
              "upload_id": "u1",
              "block_size": "10",
              "parts": [
                {"index": 1, "presigned_url": "https://cos/2", "block_size": "10"},
                {"index": 0, "presigned_url": "https://cos/1", "block_size": "10"}
              ]
            })JSON"),
            20
        );
        XX_TEST_EXPECT_EQ(unsorted.size(), size_t{2});
        if (unsorted.size() == 2) {
            XX_TEST_EXPECT_EQ(unsorted[0].index, 0);
            XX_TEST_EXPECT_EQ(unsorted[0].offset, size_t{0});
            XX_TEST_EXPECT_EQ(unsorted[0].url, std::string{"https://cos/1"});
            XX_TEST_EXPECT_EQ(unsorted[1].index, 1);
            XX_TEST_EXPECT_EQ(unsorted[1].offset, size_t{10});
        }
    }

    // ===================== 7. 预上传请求体: file_size 必须是字符串 =====================
    {
        auto body = qqBuildPrepareBody(4, 128, "hello_test.txt", sampleDigests());
        // 传数字时服务端按 0 规划分片 (文档标的是 string)
        XX_TEST_EXPECT_TRUE(body["file_size"].is_string());
        XX_TEST_EXPECT_EQ(body.value("file_size", std::string{}), std::string{"128"});
        XX_TEST_EXPECT_TRUE(body["file_type"].is_number_integer());
        XX_TEST_EXPECT_EQ(body.value("file_type", 0), 4);
        XX_TEST_EXPECT_EQ(body.value("file_name", std::string{}), std::string{"hello_test.txt"});
        XX_TEST_EXPECT_EQ(
            body.value("md5", std::string{}),
            std::string{"d41d8cd98f00b204e9800998ecf8427e"}
        );
        XX_TEST_EXPECT_EQ(
            body.value("sha1", std::string{}),
            std::string{"da39a3ee5e6b4b0d3255bfef95601890afd80709"}
        );
        XX_TEST_EXPECT_EQ(
            body.value("md5_10m", std::string{}),
            std::string{"c4d8c5f3a2b1e0f9a8b7c6d5e4f3a2b1"}
        );
    }

    // ===================== 8. 分片完成请求体: 四项齐全 (少了就 500) =====================
    {
        auto body = qqBuildPartFinishBody("upload_a1b2c3d4e5f6", 1, 5242880, "5d41402abc4b2a76b9719d911017c592");
        XX_TEST_EXPECT_EQ(body.value("upload_id", std::string{}), std::string{"upload_a1b2c3d4e5f6"});
        XX_TEST_EXPECT_TRUE(body["part_index"].is_number_integer());
        XX_TEST_EXPECT_EQ(body.value("part_index", -1), 1);
        // 该片实际字节数, 按字符串给
        XX_TEST_EXPECT_TRUE(body["block_size"].is_string());
        XX_TEST_EXPECT_EQ(body.value("block_size", std::string{}), std::string{"5242880"});
        XX_TEST_EXPECT_EQ(
            body.value("md5", std::string{}),
            std::string{"5d41402abc4b2a76b9719d911017c592"}
        );
    }

    // ===================== 9. 合并请求体: 带 file_type, 不带 file_data =====================
    {
        auto body = qqBuildMergeBody("upload_a1b2c3d4e5f6", "hello_test.txt", 4);
        XX_TEST_EXPECT_EQ(body.value("upload_id", std::string{}), std::string{"upload_a1b2c3d4e5f6"});
        XX_TEST_EXPECT_EQ(body.value("file_name", std::string{}), std::string{"hello_test.txt"});
        XX_TEST_EXPECT_EQ(body.value("file_type", 0), 4);
        XX_TEST_EXPECT_FALSE(body.value("srv_send_msg", true));
        XX_TEST_EXPECT_FALSE(body.contains("file_data"));
    }

    // ===================== 10. 二进制直传请求体: 不能带 upload_id =====================
    {
        auto body = qqBuildDirectUploadBody("hello_test.txt", 1, "aGVsbG8=");
        XX_TEST_EXPECT_EQ(body.value("file_name", std::string{}), std::string{"hello_test.txt"});
        XX_TEST_EXPECT_EQ(body.value("file_type", 0), 1);
        XX_TEST_EXPECT_EQ(body.value("file_data", std::string{}), std::string{"aGVsbG8="});
        XX_TEST_EXPECT_FALSE(body.value("srv_send_msg", true));
        // 带上 upload_id 服务端会走合并路径, 与 file_data 冲突
        XX_TEST_EXPECT_FALSE(body.contains("upload_id"));
        XX_TEST_EXPECT_FALSE(body.contains("url"));
        // 直传上限 (官方文档: 5 MB)
        XX_TEST_EXPECT_EQ(qqDirectUploadLimit, size_t{5} * 1024 * 1024);
    }

    return TestResult{g_cm_passed, g_cm_failed};
}

} // namespace test
} // namespace agentxx
