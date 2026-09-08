#include "test_json.h"

#include "agentxx/util/json.h"
#include <cmath>
#include <sstream>
#include <string>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_json_passed = 0;
int g_json_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_json_passed
#define XX_TEST_FAILED g_json_failed

using namespace agentxx::util;

namespace {

// 抛异常断言辅助 (test_framework.h 仅提供 EXPECT_TRUE/FALSE/EQ/GE,
// 异常路径用 try/catch 手写, 避免引入新宏)
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

void test_json_ctor_scalar() {
    // 默认/空值
    Json n;
    XX_TEST_EXPECT_TRUE(n.is_null());
    Json n2(nullptr);
    XX_TEST_EXPECT_TRUE(n2.is_null());

    // 布尔
    Json t(true);
    XX_TEST_EXPECT_TRUE(t.is_bool() && t.is_boolean());
    XX_TEST_EXPECT_EQ(t.get<bool>(), true);
    Json f(false);
    XX_TEST_EXPECT_EQ(f.get<bool>(), false);

    // 整数族 (全部收敛到 NumberInt/NumberUint)
    Json i(-7);
    XX_TEST_EXPECT_TRUE(i.is_number() && i.is_number_integer());
    XX_TEST_EXPECT_EQ(i.get<long long>(), (long long)-7);
    XX_TEST_EXPECT_EQ(i.get<int>(), -7);
    Json u(7u);
    XX_TEST_EXPECT_TRUE(u.is_number_unsigned());
    XX_TEST_EXPECT_EQ(u.get<unsigned long long>(), (unsigned long long)7);
    Json ll((long long)-1234567890123LL);
    XX_TEST_EXPECT_EQ(ll.get<long long>(), (long long)-1234567890123LL);
    Json ull(18446744073709551615ULL);
    XX_TEST_EXPECT_TRUE(ull.is_number_unsigned());
    XX_TEST_EXPECT_EQ(ull.get<unsigned long long>(), 18446744073709551615ULL);

    // 浮点
    Json d(1.5);
    XX_TEST_EXPECT_TRUE(d.is_number_float() && d.is_number());
    XX_TEST_EXPECT_EQ(d.get<double>(), 1.5);
    Json fl(2.5f);
    XX_TEST_EXPECT_TRUE(fl.is_number_float());
    XX_TEST_EXPECT_EQ(fl.get<float>(), 2.5f);

    // 字符串三形态
    Json s1("hi");
    XX_TEST_EXPECT_TRUE(s1.is_string());
    XX_TEST_EXPECT_EQ(s1.get<std::string>(), std::string("hi"));
    Json s2(std::string("bye"));
    XX_TEST_EXPECT_EQ(s2.get<std::string>(), std::string("bye"));
    Json s3(std::string_view("sv"));
    XX_TEST_EXPECT_EQ(s3.get<std::string_view>(), std::string_view("sv"));
    // get_string_view 无拷贝引用内部存储
    XX_TEST_EXPECT_EQ(s1.get_string_view(), std::string_view("hi"));
    // 空指针字符串视为空串 (不崩溃)
    Json sNull((const char*)nullptr);
    XX_TEST_EXPECT_TRUE(sNull.is_string());
    XX_TEST_EXPECT_EQ(sNull.get<std::string>(), std::string(""));

    // vector<string> 转数组
    Json vec(std::vector<std::string>{"a", "b"});
    XX_TEST_EXPECT_TRUE(vec.is_array() && vec.size() == 2);
    XX_TEST_EXPECT_EQ(vec[static_cast<size_t>(0)].get<std::string>(), std::string("a"));

    // is_primitive 语义: 非 null 非容器
    XX_TEST_EXPECT_TRUE(t.is_primitive() && i.is_primitive() && s1.is_primitive());
    XX_TEST_EXPECT_FALSE(n.is_primitive());
}

void test_json_factory_initlist() {
    // 工厂空容器
    Json o = Json::object();
    XX_TEST_EXPECT_TRUE(o.is_object() && o.empty());
    Json a = Json::array();
    XX_TEST_EXPECT_TRUE(a.is_array() && a.empty());

    // 工厂键值对 (保序)
    Json o2 = Json::object({
        {"b", 2},
        {"a", 1}
    });
    XX_TEST_EXPECT_EQ(o2.dump(), std::string("{\"b\":2,\"a\":1}"));
    Json a2 = Json::array({1, 2, 3});
    XX_TEST_EXPECT_EQ(a2.dump(), std::string("[1,2,3]"));

    // 智能初始化列表: 二元 [string, X] 全体即对象, 否则数组; 空即数组
    Json smartObj{
        {"k", 1},
        {"j", 2}
    };
    XX_TEST_EXPECT_TRUE(smartObj.is_object());
    XX_TEST_EXPECT_EQ(smartObj.dump(), std::string("{\"k\":1,\"j\":2}"));
    Json smartArr{1, 2, 3};
    XX_TEST_EXPECT_TRUE(smartArr.is_array());
    // 空花括号走默认构造 (Null), 与 neograph::json() 一致; 空数组请用 Json::array()
    Json smartEmpty{};
    XX_TEST_EXPECT_TRUE(smartEmpty.is_null());
    // 混合元素退化为数组
    Json mixed{
        {"k", 1},
        2
    };
    XX_TEST_EXPECT_TRUE(mixed.is_array());
}

void test_json_copy_move() {
    Json o = Json::object({
        {"x", 1   },
        {"s", "hi"}
    });
    Json cp(o);
    XX_TEST_EXPECT_TRUE(cp == o);
    Json mv(std::move(cp));
    XX_TEST_EXPECT_TRUE(mv == o);

    Json a = Json::array({1, 2});
    Json a2;
    a2 = a;
    XX_TEST_EXPECT_TRUE(a2 == a);
    Json a3;
    a3 = std::move(a2);
    XX_TEST_EXPECT_TRUE(a3 == a);

    // 标量赋值重载
    Json v;
    v = true;
    XX_TEST_EXPECT_EQ(v.get<bool>(), true);
    v = 42;
    XX_TEST_EXPECT_EQ(v.get<int>(), 42);
    v = 4u;
    XX_TEST_EXPECT_EQ(v.get<unsigned int>(), 4u);
    v = (long)-5;
    XX_TEST_EXPECT_EQ(v.get<long>(), (long)-5);
    v = (unsigned long)6;
    XX_TEST_EXPECT_EQ(v.get<unsigned long>(), (unsigned long)6);
    v = (long long)-8;
    XX_TEST_EXPECT_EQ(v.get<long long>(), (long long)-8);
    v = (unsigned long long)9;
    XX_TEST_EXPECT_EQ(v.get<unsigned long long>(), (unsigned long long)9);
    v = 1.25;
    XX_TEST_EXPECT_EQ(v.get<double>(), 1.25);
    v = 0.5f;
    XX_TEST_EXPECT_EQ(v.get<float>(), 0.5f);
    v = "s";
    XX_TEST_EXPECT_EQ(v.get<std::string>(), std::string("s"));
    v = std::string_view("sv");
    XX_TEST_EXPECT_EQ(v.get<std::string>(), std::string("sv"));
    v = std::string("std");
    XX_TEST_EXPECT_EQ(v.get<std::string>(), std::string("std"));
    v = std::vector<std::string>{"q"};
    XX_TEST_EXPECT_TRUE(v.is_array() && v.size() == 1);
    v = {
        {"k", 1}
    };
    XX_TEST_EXPECT_TRUE(v.is_object());
}

void test_json_parse_ok() {
    // 标量
    XX_TEST_EXPECT_TRUE(Json::parse("null").is_null());
    XX_TEST_EXPECT_EQ(Json::parse("true").get<bool>(), true);
    XX_TEST_EXPECT_EQ(Json::parse("-42").get<long long>(), (long long)-42);
    XX_TEST_EXPECT_EQ(
        Json::parse("18446744073709551615").get<unsigned long long>(),
        18446744073709551615ULL
    );
    XX_TEST_EXPECT_EQ(Json::parse("1.5").get<double>(), 1.5);
    XX_TEST_EXPECT_EQ(Json::parse("\"hi\"").get<std::string>(), std::string("hi"));

    // 嵌套保序对象 + 数组
    Json j = Json::parse("{\"b\":2,\"a\":[1,{\"x\":true}],\"s\":\"hi\"}");
    XX_TEST_EXPECT_TRUE(j.is_object());
    XX_TEST_EXPECT_EQ(j["b"].get<int>(), 2);
    XX_TEST_EXPECT_EQ(j["a"][static_cast<size_t>(1)].dump(), std::string("{\"x\":true}"));
    // dump 保序往返
    XX_TEST_EXPECT_EQ(j.dump(), std::string("{\"b\":2,\"a\":[1,{\"x\":true}],\"s\":\"hi\"}"));

    // 超长输入 (simdjson padded 安全)
    std::string big = "{\"k\":\"" + std::string(100000, 'x') + "\"}";
    Json        jb  = Json::parse(big);
    XX_TEST_EXPECT_EQ(jb["k"].get<std::string>().size(), (size_t)100000);

    // 转义特殊字符往返
    Json je = Json::parse("\"a\\\"b\\\\c\\n\\u4e2d\"");
    XX_TEST_EXPECT_EQ(je.get<std::string>(), std::string("a\"b\\c\n\xE4\xB8\xAD"));

    // istream 重载
    std::istringstream is("{\"v\":3}");
    XX_TEST_EXPECT_EQ(Json::parse(is)["v"].get<int>(), 3);
}

void test_json_parse_error() {
    // 畸形输入抛 parse_error (非其他异常)
    XX_TEST_EXPECT_THROW(Json::parse_error, Json::parse("{"));
    XX_TEST_EXPECT_THROW(Json::parse_error, Json::parse("[1,]"));
    XX_TEST_EXPECT_THROW(Json::parse_error, Json::parse(""));
    XX_TEST_EXPECT_THROW(Json::parse_error, Json::parse("nul"));
    // 空 istream 同样抛 parse_error
    std::istringstream empty("");
    XX_TEST_EXPECT_THROW(Json::parse_error, Json::parse(empty));
}

void test_json_access() {
    Json o = Json::object({
        {"a", 1  },
        {"s", "x"}
    });
    // 可写 [] 缺失插入 Null
    XX_TEST_EXPECT_TRUE(o["missing"].is_null());
    o["missing"] = 5;
    XX_TEST_EXPECT_EQ(o["missing"].get<int>(), 5);
    // 只读 [] 缺失返回全局 Null (不抛)
    const Json& co = o;
    XX_TEST_EXPECT_TRUE(co["absent"].is_null());
    // 非对象可写 [] 走 oob 兜底 (不崩溃; 写入丢失)
    Json num(1);
    (void)num["k"];
    // 越界下标只读返回 Null
    Json arr = Json::array({1, 2});
    XX_TEST_EXPECT_TRUE(arr[static_cast<size_t>(9)].is_null());
    XX_TEST_EXPECT_EQ(arr[0].get<int>(), 1);
    XX_TEST_EXPECT_EQ(arr[1].get<int>(), 2);

    // at 强检查
    XX_TEST_EXPECT_EQ(o.at("a").get<int>(), 1);
    XX_TEST_EXPECT_EQ(co.at("s").get<std::string>(), std::string("x"));
    XX_TEST_EXPECT_THROW(Json::out_of_range, o.at("nope"));
    XX_TEST_EXPECT_THROW(Json::type_error, num.at("k"));
    XX_TEST_EXPECT_EQ(arr.at(static_cast<size_t>(0)).get<int>(), 1);
    XX_TEST_EXPECT_THROW(Json::out_of_range, arr.at(static_cast<size_t>(7)));
    XX_TEST_EXPECT_THROW(Json::type_error, o.at(static_cast<size_t>(0)));

    // contains / find / front / back
    XX_TEST_EXPECT_TRUE(o.contains("a") && !o.contains("zzz"));
    XX_TEST_EXPECT_FALSE(num.contains("a"));
    XX_TEST_EXPECT_TRUE(o.find("a") != o.end());
    XX_TEST_EXPECT_TRUE(o.find("zzz") == o.end());
    XX_TEST_EXPECT_EQ(arr.front().get<int>(), 1);
    XX_TEST_EXPECT_EQ(arr.back().get<int>(), 2);
    XX_TEST_EXPECT_THROW(Json::out_of_range, Json::array().back());
    XX_TEST_EXPECT_THROW(Json::type_error, o.front());

    // value 默认值降级 (缺失/类型不匹配/非对象均不抛)
    XX_TEST_EXPECT_EQ(o.value("a", 0), 1);
    XX_TEST_EXPECT_EQ(o.value("nope", 42), 42);
    XX_TEST_EXPECT_EQ(o.value("a", std::string("d")), std::string("d")); // int->string 不匹配回退
    XX_TEST_EXPECT_EQ(o.value("s", std::string("d")), std::string("x"));
    XX_TEST_EXPECT_EQ(o.value("s", "lit"), std::string("x"));
    XX_TEST_EXPECT_EQ(o.value("nope", "lit"), std::string("lit"));
    XX_TEST_EXPECT_EQ(num.value("k", 7), 7);
    // 字符串转数字宽容性: get<long long> 接受三类数字
    Json f(2.9);
    XX_TEST_EXPECT_EQ(f.get<long long>(), (long long)2);
    XX_TEST_EXPECT_EQ(f.get<unsigned long long>(), (unsigned long long)2);
    XX_TEST_EXPECT_EQ(f.get<double>(), 2.9);

    // size/empty/clear/erase/push_back
    XX_TEST_EXPECT_EQ(o.size(), (size_t)3);
    XX_TEST_EXPECT_EQ(Json().size(), (size_t)0);
    XX_TEST_EXPECT_EQ(Json("abcd").size(), (size_t)4); // 字符串返回字节数
    Json clr(5);
    clr.clear();
    XX_TEST_EXPECT_EQ(clr.get<int>(), 0);
    Json clrS("hi");
    clrS.clear();
    XX_TEST_EXPECT_EQ(clrS.get<std::string>(), std::string(""));
    Json clrA = Json::array({1});
    clrA.clear();
    XX_TEST_EXPECT_TRUE(clrA.empty());
    XX_TEST_EXPECT_TRUE(o.erase("a") && !o.contains("a"));
    XX_TEST_EXPECT_FALSE(o.erase("a"));
    XX_TEST_EXPECT_FALSE(num.erase("a"));
    XX_TEST_EXPECT_TRUE(arr.erase(static_cast<size_t>(0)) && arr.size() == 1);
    XX_TEST_EXPECT_FALSE(arr.erase(static_cast<size_t>(9)));
    Json pb;
    pb.push_back(1); // Null 自动提升为数组
    pb.push_back(Json(2));
    XX_TEST_EXPECT_EQ(pb.dump(), std::string("[1,2]"));
    XX_TEST_EXPECT_THROW(Json::type_error, o.push_back(1));
}

void test_json_iter_items() {
    Json o = Json::object({
        {"b", 2},
        {"a", 1}
    });
    // begin/end 遍历值
    int sum = 0;
    for (auto it = o.begin(); it != o.end(); ++it) {
        sum += it->get<int>();
    }
    XX_TEST_EXPECT_EQ(sum, 3);
    // key()/value()
    std::string keys;
    for (auto it = o.begin(); it != o.end(); ++it) {
        keys += it.key();
    }
    XX_TEST_EXPECT_EQ(keys, std::string("ba")); // 保序
    // const 迭代
    const Json& co   = o;
    int         csum = 0;
    for (auto it = co.begin(); it != co.end(); ++it) {
        csum += (*it).get<int>();
    }
    XX_TEST_EXPECT_EQ(csum, 3);
    // mutable 转 const
    Json::iterator       mit = o.begin();
    Json::const_iterator cit = mit;
    XX_TEST_EXPECT_EQ(cit.key(), std::string("b"));

    // items() 可写结构化绑定: 修改经引用生效
    for (auto [k, v] : o.items()) {
        (void)k;
        if (v.is_number()) {
            v = v.get<int>() * 10;
        }
    }
    XX_TEST_EXPECT_EQ(o["b"].get<int>(), 20);
    XX_TEST_EXPECT_EQ(o["a"].get<int>(), 10);
    // items() const 值语义
    int csum2 = 0;
    for (const auto& [k, v] : co.items()) {
        (void)k;
        csum2 += v.get<int>();
    }
    XX_TEST_EXPECT_EQ(csum2, 30);
}

void test_json_dump() {
    // 紧凑默认
    Json        o = Json::object({
        {"s", "a\"b\\c\n"},
        {"n", 1          },
        {"f", 1.5        },
        {"t", true       },
        {"z", Json{}     }
    });
    std::string d = o.dump();
    XX_TEST_EXPECT_TRUE(d.find("\\\"") != std::string::npos);
    XX_TEST_EXPECT_TRUE(d.find("\\\\") != std::string::npos);
    XX_TEST_EXPECT_TRUE(d.find("\\n") != std::string::npos);
    // 控制字符 \u00xx 转义
    Json ctrl(std::string("\x01\x1F", 2));
    XX_TEST_EXPECT_EQ(ctrl.dump(), std::string("\"\\u0001\\u001f\""));
    // 浮点形态: 纯整数写法补 .0 (与 neograph dump 口径一致)
    XX_TEST_EXPECT_EQ(Json(1.0).dump(), std::string("1.0"));
    XX_TEST_EXPECT_TRUE(Json(1.5).dump().find("1.5") != std::string::npos);
    // 非有限浮点降级 null
    XX_TEST_EXPECT_EQ(Json(std::numeric_limits<double>::quiet_NaN()).dump(), std::string("null"));
    XX_TEST_EXPECT_EQ(Json(std::numeric_limits<double>::infinity()).dump(), std::string("null"));
    // 空容器
    XX_TEST_EXPECT_EQ(Json::array().dump(), std::string("[]"));
    XX_TEST_EXPECT_EQ(Json::object().dump(), std::string("{}"));

    // 格式化排版 (indent=2): 换行 + 缩进 + ": " 分隔
    Json        fmt    = Json::object({
        {"a",   1                  },
        {"arr", Json::array({1, 2})}
    });
    std::string pretty = fmt.dump(2);
    XX_TEST_EXPECT_TRUE(pretty.find('\n') != std::string::npos);
    XX_TEST_EXPECT_TRUE(pretty.find("\"a\": 1") != std::string::npos);
    XX_TEST_EXPECT_TRUE(pretty.find("  \"a\"") != std::string::npos);
    // 紧凑与格式化解析一致性
    XX_TEST_EXPECT_TRUE(Json::parse(pretty) == fmt);
    // 流输出
    std::ostringstream os;
    os << Json(1);
    XX_TEST_EXPECT_EQ(os.str(), std::string("1"));
}

void test_json_equal() {
    XX_TEST_EXPECT_TRUE(Json() == Json());
    XX_TEST_EXPECT_TRUE(Json(1) == Json(1));
    XX_TEST_EXPECT_TRUE(Json(1) != Json(2));
    // 1 与 1.0 类型不同不相等
    XX_TEST_EXPECT_TRUE(Json(1) != Json(1.0));
    XX_TEST_EXPECT_TRUE(Json("a") == Json("a"));
    XX_TEST_EXPECT_TRUE(Json::array({1, 2}) == Json::array({1, 2}));
    XX_TEST_EXPECT_TRUE(Json::array({1, 2}) != Json::array({2, 1}));
    // 对象键序不同不相等 (保序模型)
    Json o1 = Json::object({
        {"a", 1},
        {"b", 2}
    });
    Json o2 = Json::object({
        {"b", 2},
        {"a", 1}
    });
    XX_TEST_EXPECT_TRUE(o1 != o2);
    XX_TEST_EXPECT_TRUE(
        o1
        == Json::object({
            {"a", 1},
            {"b", 2}
    })
    );
}

namespace agentxx {
namespace test {

TestResult testJson() {
    test_json_ctor_scalar();
    test_json_factory_initlist();
    test_json_copy_move();
    test_json_parse_ok();
    test_json_parse_error();
    test_json_access();
    test_json_iter_items();
    test_json_dump();
    test_json_equal();
    return TestResult{g_json_passed, g_json_failed};
}

} // namespace test
} // namespace agentxx
