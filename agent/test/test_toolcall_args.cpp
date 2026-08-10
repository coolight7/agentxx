#include "test_toolcall_args.h"

#include "agentxx/nodes/toolcall.h"
#include "neograph/json.h"
#include <string>
#include <utility>

namespace agentxx {
namespace test {

int g_tca_passed = 0;
int g_tca_failed = 0;

#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_tca_passed
#define XX_TEST_FAILED g_tca_failed

namespace {

/// 构造参数 schema: { "type": "object", "properties": { name: propSchema } }
neograph::json makeParams(const std::pair<std::string, neograph::json>& prop) {
    return neograph::json{
        {"type",       "object"                                 },
        {"properties", neograph::json{{prop.first, prop.second}}},
    };
}

} // namespace

TestResult testToolcallArgs() {
    g_tca_passed = 0;
    g_tca_failed = 0;

    // ===================== string <-> 字符串数组 =====================
    // #1 字符串数组: 字符串参数自动包装为 [字符串]
    {
        neograph::ChatTool def;
        def.name       = "tool_a";
        def.parameters = makeParams({
            "paths",
            {{"type", "array"}, {"items", {{"type", "string"}}}}
        });
        auto args      = neograph::json{
                 {"paths", "a.txt"}
        };
        XX_TEST_EXPECT_TRUE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_TRUE(args["paths"].is_array());
        XX_TEST_EXPECT_EQ(args["paths"].size(), size_t{1});
        XX_TEST_EXPECT_EQ(args["paths"][0].get<std::string>(), std::string{"a.txt"});
    }

    // #2 未声明 items 的数组: 同样转换
    {
        neograph::ChatTool def;
        def.name       = "tool_b";
        def.parameters = makeParams({"tags", {{"type", "array"}}});
        auto args      = neograph::json{
                 {"tags", "x"}
        };
        XX_TEST_EXPECT_TRUE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_TRUE(args["tags"].is_array());
        XX_TEST_EXPECT_EQ(args["tags"][0].get<std::string>(), std::string{"x"});
    }

    // #3 对象数组 (items.type == object): 字符串不转换
    {
        neograph::ChatTool def;
        def.name       = "tool_c";
        def.parameters = makeParams({
            "commands",
            {{"type", "array"}, {"items", {{"type", "object"}}}}
        });
        auto args      = neograph::json{
                 {"commands", "click"}
        };
        XX_TEST_EXPECT_FALSE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_TRUE(args["commands"].is_string());
    }

    // #4 联合类型 {"type": ["array", "string"]}: 字符串转换为数组
    {
        neograph::ChatTool def;
        def.name = "tool_e";
        def.parameters
            = makeParams({"items", {{"type", neograph::json::array({"array", "string"})}}});
        auto args = neograph::json{
            {"items", "s"}
        };
        XX_TEST_EXPECT_TRUE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_TRUE(args["items"].is_array());
    }

    // #5 已是数组: 保持原样
    {
        neograph::ChatTool def;
        def.name       = "tool_f";
        def.parameters = makeParams({
            "tags",
            {{"type", "array"}, {"items", {{"type", "string"}}}}
        });
        auto args      = neograph::json{
                 {"tags", neograph::json::array({"a", "b"})}
        };
        XX_TEST_EXPECT_FALSE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_EQ(args["tags"].size(), size_t{2});
    }

    // ===================== string <-> number/integer =====================
    // #6 string "42" -> integer schema: 转为整数
    {
        neograph::ChatTool def;
        def.name       = "tool_g";
        def.parameters = makeParams({"count", {{"type", "integer"}}});
        auto args      = neograph::json{
                 {"count", "42"}
        };
        XX_TEST_EXPECT_TRUE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_TRUE(args["count"].is_number_integer());
        XX_TEST_EXPECT_EQ(args["count"].get<long long>(), 42);
    }

    // #7 string "3.14" -> number schema: 转为浮点
    {
        neograph::ChatTool def;
        def.name       = "tool_h";
        def.parameters = makeParams({"ratio", {{"type", "number"}}});
        auto args      = neograph::json{
                 {"ratio", "3.14"}
        };
        XX_TEST_EXPECT_TRUE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_TRUE(args["ratio"].is_number_float());
        XX_TEST_EXPECT_EQ(args["ratio"].get<double>(), 3.14);
    }

    // #8 string 负数/前导+号/首尾空白: 均按数值解析
    {
        neograph::ChatTool def;
        def.name       = "tool_i";
        def.parameters = makeParams({"a", {{"type", "integer"}}});
        auto args      = neograph::json{
                 {"a", "  -7  "}
        };
        XX_TEST_EXPECT_TRUE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_EQ(args["a"].get<long long>(), -7);

        def.parameters = makeParams({"b", {{"type", "integer"}}});
        args           = neograph::json{
                      {"b", "+5"}
        };
        XX_TEST_EXPECT_TRUE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_EQ(args["b"].get<long long>(), 5);
    }

    // #9 integer schema 不接受小数写法: "3.5" 保持字符串
    {
        neograph::ChatTool def;
        def.name       = "tool_j";
        def.parameters = makeParams({"count", {{"type", "integer"}}});
        auto args      = neograph::json{
                 {"count", "3.5"}
        };
        XX_TEST_EXPECT_FALSE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_TRUE(args["count"].is_string());
    }

    // #10 非数值字符串: 不转换
    {
        neograph::ChatTool def;
        def.name       = "tool_k";
        def.parameters = makeParams({"count", {{"type", "number"}}});
        auto args      = neograph::json{
                 {"count", "abc"}
        };
        XX_TEST_EXPECT_FALSE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_TRUE(args["count"].is_string());
    }

    // #11 空字符串 / 十六进制写法: 不转换 (避免 "0x10" 被解析为浮点 16.0)
    {
        neograph::ChatTool def;
        def.name       = "tool_l";
        def.parameters = makeParams({"n", {{"type", "number"}}});
        auto args      = neograph::json{
                 {"n", ""}
        };
        XX_TEST_EXPECT_FALSE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));

        args = neograph::json{
            {"n", "0x10"}
        };
        XX_TEST_EXPECT_FALSE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_TRUE(args["n"].is_string());
    }

    // #12 number schema 接受整数写法的字符串: "7" -> 7.0
    {
        neograph::ChatTool def;
        def.name       = "tool_m";
        def.parameters = makeParams({"n", {{"type", "number"}}});
        auto args      = neograph::json{
                 {"n", "7"}
        };
        XX_TEST_EXPECT_TRUE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_TRUE(args["n"].is_number());
    }

    // #13 number/integer -> string: 数值转为字符串
    {
        neograph::ChatTool def;
        def.name       = "tool_n";
        def.parameters = makeParams({"text", {{"type", "string"}}});
        auto args      = neograph::json{
                 {"text", 42}
        };
        XX_TEST_EXPECT_TRUE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_TRUE(args["text"].is_string());
        XX_TEST_EXPECT_EQ(args["text"].get<std::string>(), std::string{"42"});

        args = neograph::json{
            {"text", 3.5}
        };
        XX_TEST_EXPECT_TRUE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_EQ(args["text"].get<std::string>(), std::string{"3.5"});
    }

    // #14 联合类型 ["string", "number"]: string 与 number 均已合法, 不做转换
    {
        neograph::ChatTool def;
        def.name       = "tool_o";
        def.parameters = makeParams({"v", {{"type", neograph::json::array({"string", "number"})}}});
        auto args      = neograph::json{
                 {"v", "42"}
        };
        XX_TEST_EXPECT_FALSE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_TRUE(args["v"].is_string());

        args = neograph::json{
            {"v", 42}
        };
        XX_TEST_EXPECT_FALSE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_TRUE(args["v"].is_number());
    }

    // #15 联合类型 ["number", "integer"]: 按 number 解析, 小数写法可转换
    {
        neograph::ChatTool def;
        def.name = "tool_p";
        def.parameters
            = makeParams({"v", {{"type", neograph::json::array({"number", "integer"})}}});
        auto args = neograph::json{
            {"v", "3.5"}
        };
        XX_TEST_EXPECT_TRUE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_TRUE(args["v"].is_number_float());
        XX_TEST_EXPECT_EQ(args["v"].get<double>(), 3.5);
    }

    // ===================== bool <-> string =====================
    // #16 string "true"/"false" -> boolean schema: 转为布尔
    {
        neograph::ChatTool def;
        def.name       = "tool_q";
        def.parameters = makeParams({"flag", {{"type", "boolean"}}});
        auto args      = neograph::json{
                 {"flag", "true"}
        };
        XX_TEST_EXPECT_TRUE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_EQ(args["flag"].get<bool>(), true);

        args = neograph::json{
            {"flag", "false"}
        };
        XX_TEST_EXPECT_TRUE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_EQ(args["flag"].get<bool>(), false);
    }

    // #17 boolean schema 不接受其他字符串: "yes" 不转换
    {
        neograph::ChatTool def;
        def.name       = "tool_r";
        def.parameters = makeParams({"flag", {{"type", "boolean"}}});
        auto args      = neograph::json{
                 {"flag", "yes"}
        };
        XX_TEST_EXPECT_FALSE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_TRUE(args["flag"].is_string());
    }

    // #18 bool -> string schema: 转为 "true"/"false"
    {
        neograph::ChatTool def;
        def.name       = "tool_s";
        def.parameters = makeParams({"text", {{"type", "string"}}});
        auto args      = neograph::json{
                 {"text", true}
        };
        XX_TEST_EXPECT_TRUE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_EQ(args["text"].get<std::string>(), std::string{"true"});

        args = neograph::json{
            {"text", false}
        };
        XX_TEST_EXPECT_TRUE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_EQ(args["text"].get<std::string>(), std::string{"false"});
    }

    // ===================== [单字符串数组] -> string =====================
    // #19 string schema 传入单元素字符串数组: 解包为字符串
    {
        neograph::ChatTool def;
        def.name       = "tool_t";
        def.parameters = makeParams({"text", {{"type", "string"}}});
        auto args      = neograph::json{
                 {"text", neograph::json::array({"hello"})}
        };
        XX_TEST_EXPECT_TRUE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_TRUE(args["text"].is_string());
        XX_TEST_EXPECT_EQ(args["text"].get<std::string>(), std::string{"hello"});
    }

    // #20 多元素数组: 不转换
    {
        neograph::ChatTool def;
        def.name       = "tool_u";
        def.parameters = makeParams({"text", {{"type", "string"}}});
        auto args      = neograph::json{
                 {"text", neograph::json::array({"a", "b"})}
        };
        XX_TEST_EXPECT_FALSE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_TRUE(args["text"].is_array());
    }

    // ===================== 边界场景 =====================
    // #21 字符串参数 (schema 也是 string): 不转换
    {
        neograph::ChatTool def;
        def.name       = "tool_v";
        def.parameters = makeParams({"content", {{"type", "string"}}});
        auto args      = neograph::json{
                 {"content", "hello"}
        };
        XX_TEST_EXPECT_FALSE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_TRUE(args["content"].is_string());
    }

    // #22 参数未传入: 不转换不报错
    {
        neograph::ChatTool def;
        def.name       = "tool_w";
        def.parameters = makeParams({"tags", {{"type", "array"}}});
        auto args      = neograph::json{
                 {"other", "x"}
        };
        XX_TEST_EXPECT_FALSE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
    }

    // #23 数字数组 (items.type == number): 字符串不包装
    {
        neograph::ChatTool def;
        def.name       = "tool_x";
        def.parameters = makeParams({
            "scores",
            {{"type", "array"}, {"items", {{"type", "number"}}}}
        });
        auto args      = neograph::json{
                 {"scores", "42"}
        };
        XX_TEST_EXPECT_FALSE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_TRUE(args["scores"].is_string());
    }

    // #24 args 非对象: 不转换不报错
    {
        neograph::ChatTool def;
        def.name       = "tool_y";
        def.parameters = makeParams({"tags", {{"type", "array"}}});
        auto args      = neograph::json{"not_object"};
        XX_TEST_EXPECT_FALSE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
    }

    // #25 parameters 非对象: 不转换不报错
    {
        neograph::ChatTool def;
        def.name       = "tool_z";
        def.parameters = neograph::json::array();
        auto args      = neograph::json{
                 {"tags", "x"}
        };
        XX_TEST_EXPECT_FALSE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
    }

    // #26 多参数混合: 每个参数独立按 schema 转换, 其他参数不受影响
    {
        neograph::ChatTool def;
        def.name       = "tool_aa";
        def.parameters = neograph::json{
            {"type",       "object"},
            {"properties",
             neograph::json{
                 {"tags", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                 {"name", {{"type", "string"}}},
                 {"count", {{"type", "integer"}}},
                 {"ratio", {{"type", "string"}}},
             }                     },
        };
        auto args = neograph::json{
            {"tags",  "t1"},
            {"name",  "n" },
            {"count", "3" },
            {"ratio", 0.5 }
        };
        XX_TEST_EXPECT_TRUE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_TRUE(args["tags"].is_array());
        XX_TEST_EXPECT_EQ(args["tags"][0].get<std::string>(), std::string{"t1"});
        XX_TEST_EXPECT_EQ(args["name"].get<std::string>(), std::string{"n"});
        XX_TEST_EXPECT_TRUE(args["count"].is_number_integer());
        XX_TEST_EXPECT_EQ(args["count"].get<long long>(), 3);
        XX_TEST_EXPECT_TRUE(args["ratio"].is_string());
        XX_TEST_EXPECT_EQ(args["ratio"].get<std::string>(), std::string{"0.5"});
    }

    // #27 多元素字符串数组不受影响
    {
        neograph::ChatTool def;
        def.name       = "tool_ab";
        def.parameters = makeParams({
            "paths",
            {{"type", "array"}, {"items", {{"type", "string"}}}}
        });
        auto args      = neograph::json{
                 {"paths", neograph::json::array({"a", "b", "c"})}
        };
        XX_TEST_EXPECT_FALSE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_EQ(args["paths"].size(), size_t{3});
    }

    return TestResult{g_tca_passed, g_tca_failed};
}

} // namespace test
} // namespace agentxx
