#include "test_json_reflection.h"

#include "agentxx/util/json.h"
#include <string>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_jr_passed = 0;
int g_jr_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_jr_passed
#define XX_TEST_FAILED g_jr_failed

using namespace agentxx::util;

namespace {

// §5 反射当前 AGENTXX_HAS_CPP26_REFLECTION=0, 走 ADL toJson/fromJson 降级路径。
// 此处用业务类型手写 ADL 转换, 验证降级路径往返正确; 编译器未来开启
// -freflection (宏翻 1) 后零样板 reflectToJson 将直接可用, 本用例无需改动。
struct ServerCfg {
    std::string host;
    int         port     = 0;
    bool        useTls   = false;
    double      timeoutS = 0.0;
};

inline void toJson(Json& j, const ServerCfg& v) {
    j = Json::object({
        {"host",     v.host    },
        {"port",     v.port    },
        {"useTls",   v.useTls  },
        {"timeoutS", v.timeoutS}
    });
}

inline void fromJson(const Json& j, ServerCfg& v) {
    v.host     = j.value("host", std::string{});
    v.port     = j.value("port", 0);
    v.useTls   = j.value("useTls", false);
    v.timeoutS = j.value("timeoutS", 0.0);
}

struct EmptyCfg {};

inline void toJson(Json& j, const EmptyCfg&) {
    j = Json::object();
}

inline void fromJson(const Json&, EmptyCfg&) {}

} // namespace

void test_jr_macro_is_fallback() {
    // 当前构建未带 -freflection, 必须走降级路径 (任务 §5 约定)
    XX_TEST_EXPECT_EQ(AGENTXX_HAS_CPP26_REFLECTION, 0);
}

void test_jr_adl_roundtrip() {
    ServerCfg src{"example.com", 8080, true, 1.5};
    Json      j = reflectToJson(src);
    XX_TEST_EXPECT_TRUE(j.is_object());
    XX_TEST_EXPECT_EQ(j.value("host", std::string{}), std::string("example.com"));
    XX_TEST_EXPECT_EQ(j.value("port", 0), 8080);
    XX_TEST_EXPECT_EQ(j.value("useTls", false), true);
    XX_TEST_EXPECT_EQ(j.value("timeoutS", 0.0), 1.5);

    ServerCfg dst = reflectFromJson<ServerCfg>(j);
    XX_TEST_EXPECT_EQ(dst.host, std::string("example.com"));
    XX_TEST_EXPECT_EQ(dst.port, 8080);
    XX_TEST_EXPECT_EQ(dst.useTls, true);
    XX_TEST_EXPECT_EQ(dst.timeoutS, 1.5);

    // 空结构体往返
    Json empty = reflectToJson(EmptyCfg{});
    XX_TEST_EXPECT_TRUE(empty.is_object() && empty.empty());
    (void)reflectFromJson<EmptyCfg>(empty);
}

void test_jr_missing_key_default() {
    // 缺失键按 fromJson 默认值补齐 (不抛异常)
    Json      partial = Json::parse("{\"host\":\"h\"}");
    ServerCfg v       = reflectFromJson<ServerCfg>(partial);
    XX_TEST_EXPECT_EQ(v.host, std::string("h"));
    XX_TEST_EXPECT_EQ(v.port, 0);
    XX_TEST_EXPECT_EQ(v.useTls, false);
    XX_TEST_EXPECT_EQ(v.timeoutS, 0.0);
}

namespace agentxx {
namespace test {

TestResult testJsonReflection() {
    test_jr_macro_is_fallback();
    test_jr_adl_roundtrip();
    test_jr_missing_key_default();
    return TestResult{g_jr_passed, g_jr_failed};
}

} // namespace test
} // namespace agentxx
