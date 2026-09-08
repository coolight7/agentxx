#include "test_json_view.h"

#include "agentxx/util/json_view.h"
#include <string>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_jv_passed = 0;
int g_jv_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_jv_passed
#define XX_TEST_FAILED g_jv_failed

using namespace agentxx::util;

namespace {

template<typename Ex, typename Fn>
void expectThrow(Fn&& fn, int line) {
    try {
        fn();
        XX_TEST_FAILED++;
        TEST_FAIL << "line " << line << ": expected throw" << std::endl;
    } catch (const Ex&) {
        XX_TEST_PASSED++;
    } catch (...) {
        XX_TEST_FAILED++;
        TEST_FAIL << "line " << line << ": wrong exception type" << std::endl;
    }
}

#define XX_TEST_EXPECT_THROW(Ex, expr) \
    expectThrow<Ex>(                   \
        [&]() {                        \
            expr;                      \
        },                             \
        __LINE__                       \
    )

} // namespace

void test_jv_parse_type() {
    // 标量类型探测
    XX_TEST_EXPECT_TRUE(JsonView::parse("null").is_null());
    XX_TEST_EXPECT_TRUE(JsonView::parse("true").is_bool());
    XX_TEST_EXPECT_TRUE(JsonView::parse("-1").is_int64());
    XX_TEST_EXPECT_TRUE(JsonView::parse("18446744073709551615").is_uint64());
    XX_TEST_EXPECT_TRUE(JsonView::parse("1.5").is_double());
    XX_TEST_EXPECT_TRUE(JsonView::parse("1").is_number());
    XX_TEST_EXPECT_TRUE(JsonView::parse("1.5").is_number());
    XX_TEST_EXPECT_TRUE(JsonView::parse("\"s\"").is_string());
    XX_TEST_EXPECT_TRUE(JsonView::parse("[1]").is_array());
    XX_TEST_EXPECT_TRUE(JsonView::parse("{}").is_object());
    // bool 语义: 显式 operator bool 与 valid()
    XX_TEST_EXPECT_TRUE((bool)JsonView::parse("1"));
    XX_TEST_EXPECT_FALSE((bool)JsonView{});

    // 零堆分配语义: 字符串切片借用 tape 内存, 与输入内容一致
    auto v = JsonView::parse("{\"type\":\"content_block_delta\",\"n\":42}");
    XX_TEST_EXPECT_EQ(v["type"].get_string_view(), std::string_view("content_block_delta"));
    XX_TEST_EXPECT_EQ(v["n"].get_int64(), (int64_t)42);

    // 畸形抛 parse_error
    XX_TEST_EXPECT_THROW(Json::parse_error, JsonView::parse("{"));
    XX_TEST_EXPECT_THROW(Json::parse_error, JsonView::parse(""));

    // 超长输入
    std::string big(100000, 'x');
    std::string payload = "{\"k\":\"" + big + "\"}";
    auto        vb      = JsonView::parse(payload);
    XX_TEST_EXPECT_EQ(vb["k"].get_string_view().size(), (size_t)100000);
}

void test_jv_get() {
    auto v = JsonView::parse("{\"b\":true,\"i\":-7,\"u\":7,\"d\":2.5,\"s\":\"hi\"}");
    XX_TEST_EXPECT_EQ(v["b"].get_bool(), true);
    XX_TEST_EXPECT_EQ(v["b"].get<bool>(), true);
    XX_TEST_EXPECT_EQ(v["i"].get_int64(), (int64_t)-7);
    XX_TEST_EXPECT_EQ(v["u"].get_uint64(), (uint64_t)7);
    XX_TEST_EXPECT_EQ(v["d"].get_double(), 2.5);
    XX_TEST_EXPECT_EQ(v["s"].get_string_view(), std::string_view("hi"));
    XX_TEST_EXPECT_EQ(v["s"].get<std::string>(), std::string("hi"));
    XX_TEST_EXPECT_EQ(v["s"].get<std::string_view>(), std::string_view("hi"));
    // 数字宽容 get
    XX_TEST_EXPECT_EQ(v["d"].get<int>(), 2);
    XX_TEST_EXPECT_EQ(v["i"].get<double>(), -7.0);
    XX_TEST_EXPECT_EQ(v["u"].get<unsigned long long>(), (unsigned long long)7);
    XX_TEST_EXPECT_EQ(v["i"].get<long long>(), (long long)-7);
    XX_TEST_EXPECT_EQ(v["d"].get<float>(), 2.5f);
    // 类型不匹配抛 type_error
    XX_TEST_EXPECT_THROW(Json::type_error, v["s"].get_bool());
    XX_TEST_EXPECT_THROW(Json::type_error, v["b"].get_string_view());
    XX_TEST_EXPECT_THROW(Json::type_error, v["s"].get<int>());
    XX_TEST_EXPECT_THROW(Json::type_error, JsonView{}.get_bool());
}

void test_jv_nav() {
    auto v = JsonView::parse("{\"a\":{\"b\":[10,20]},\"s\":\"hi\"}");
    // 只读导航: 缺失返回无效视图 (不抛)
    XX_TEST_EXPECT_TRUE(v["a"]["b"][static_cast<size_t>(0)].get_int64() == 10);
    XX_TEST_EXPECT_TRUE(v["a"]["b"][1].get_int64() == 20);
    XX_TEST_EXPECT_FALSE((bool)v["missing"]);
    XX_TEST_EXPECT_FALSE((bool)v["missing"]["deep"]);
    XX_TEST_EXPECT_FALSE((bool)v["a"]["b"][static_cast<size_t>(9)]);
    // 非容器下标返回无效
    XX_TEST_EXPECT_FALSE((bool)v["s"][static_cast<size_t>(0)]);
    XX_TEST_EXPECT_FALSE((bool)v["s"]["k"]);

    // at 强检查
    XX_TEST_EXPECT_EQ(v.at("s").get_string_view(), std::string_view("hi"));
    XX_TEST_EXPECT_THROW(Json::out_of_range, v.at("nope"));
    XX_TEST_EXPECT_THROW(Json::type_error, v["s"].at("k"));
    XX_TEST_EXPECT_EQ(v["a"]["b"].at(static_cast<size_t>(1)).get_int64(), (int64_t)20);
    XX_TEST_EXPECT_THROW(Json::out_of_range, v["a"]["b"].at(static_cast<size_t>(5)));
    XX_TEST_EXPECT_THROW(Json::type_error, v["s"].at(static_cast<size_t>(0)));

    // contains/size/empty
    XX_TEST_EXPECT_TRUE(v.contains("a") && !v.contains("zzz"));
    XX_TEST_EXPECT_FALSE(v["s"].contains("a"));
    XX_TEST_EXPECT_EQ(v["a"]["b"].size(), (size_t)2);
    XX_TEST_EXPECT_EQ(v.size(), (size_t)2);
    XX_TEST_EXPECT_EQ(v["s"].size(), (size_t)2); // 字符串返回字节数
    XX_TEST_EXPECT_FALSE(v.empty());
    XX_TEST_EXPECT_TRUE(JsonView::parse("[]").empty());
    XX_TEST_EXPECT_TRUE(JsonView::parse("{}").empty());

    // 子视图可跨语句持有 (Storage shared_ptr 延续 tape 生命周期)
    JsonView child;
    {
        auto parent = JsonView::parse("{\"k\":\"held\"}");
        child       = parent["k"];
    }
    XX_TEST_EXPECT_EQ(child.get_string_view(), std::string_view("held"));
}

void test_jv_value_default() {
    auto v = JsonView::parse("{\"i\":3,\"s\":\"x\"}");
    XX_TEST_EXPECT_EQ(v.value("i", 0), 3);
    XX_TEST_EXPECT_EQ(v.value("missing", 42), 42);
    XX_TEST_EXPECT_EQ(v.value("s", std::string("d")), std::string("x"));
    XX_TEST_EXPECT_EQ(v.value("i", std::string("d")), std::string("d")); // 类型不匹配回退
    XX_TEST_EXPECT_EQ(v.value("s", "lit"), std::string("x"));
    XX_TEST_EXPECT_EQ(v.value("missing", "lit"), std::string("lit"));
    XX_TEST_EXPECT_EQ(v.value("missing", std::string_view("sv")), std::string_view("sv"));
}

void test_jv_to_json() {
    // 物化为可修改 Json: 深层结构一致, 修改不影响原视图
    auto v = JsonView::parse("{\"a\":[1,{\"x\":true}],\"s\":\"hi\"}");
    Json j = v.to_json();
    XX_TEST_EXPECT_TRUE(j.is_object());
    XX_TEST_EXPECT_EQ(j.dump(), std::string("{\"a\":[1,{\"x\":true}],\"s\":\"hi\"}"));
    j["s"] = "changed";
    XX_TEST_EXPECT_EQ(j["s"].get<std::string>(), std::string("changed"));
    XX_TEST_EXPECT_EQ(v["s"].get_string_view(), std::string_view("hi"));
    // 无效视图物化为 Null
    XX_TEST_EXPECT_TRUE(JsonView{}.to_json().is_null());
    // 标量物化
    XX_TEST_EXPECT_EQ(JsonView::parse("7").to_json().get<int>(), 7);
}

void test_jv_parser_reuse() {
    // 外部 parser 复用重载: 同一 parser 串行解析 (调用方保证使用期间不再次 parse)
    simdjson::dom::parser parser;
    {
        auto v1 = JsonView::parse("{\"t\":1}", parser);
        XX_TEST_EXPECT_EQ(v1["t"].get_int64(), (int64_t)1);
    }
    {
        auto v2 = JsonView::parse("{\"t\":2}", parser);
        XX_TEST_EXPECT_EQ(v2["t"].get_int64(), (int64_t)2);
    }
    // SSE 高频 chunk 路由范式 (§4.3): 先 View 路由命中后物化
    auto chunk = JsonView::parse(
        "{\"type\":\"content_block_delta\",\"delta\":{\"text\":\"hel\"}}",
        parser
    );
    XX_TEST_EXPECT_EQ(
        chunk.value("type", std::string_view("")),
        std::string_view("content_block_delta")
    );
    XX_TEST_EXPECT_EQ(chunk["delta"]["text"].get_string_view(), std::string_view("hel"));
    Json m = chunk.to_json();
    XX_TEST_EXPECT_EQ(m["delta"]["text"].get<std::string>(), std::string("hel"));
}

namespace agentxx {
namespace test {

TestResult testJsonView() {
    test_jv_parse_type();
    test_jv_get();
    test_jv_nav();
    test_jv_value_default();
    test_jv_to_json();
    test_jv_parser_reuse();
    return TestResult{g_jv_passed, g_jv_failed};
}

} // namespace test
} // namespace agentxx
