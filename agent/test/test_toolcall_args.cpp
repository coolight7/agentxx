#include "test_toolcall_args.h"

#include "agentxx/nodes/toolcall.h"
#include "fmt/format.h"
#include "neograph/json.h"
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace agentxx {
namespace test {

namespace {

// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_tca_passed = 0;
int g_tca_failed = 0;

} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
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

/// 常用测试参数 JSON 字符串 (重复检测用例共用)
const std::string KEY_A_TXT = R"({"path":"a.txt"})";

/// 构造带 tool_calls 的 assistant 消息 (llm 轮)
neograph::ChatMessage makeAssistantMsg(const std::vector<std::pair<std::string, std::string>>& calls
) {
    neograph::ChatMessage msg;
    msg.role    = "assistant";
    msg.content = "";
    for (const auto& [name, args] : calls) {
        neograph::ToolCall tc;
        tc.id        = fmt::format("call_{}_{}", name, args);
        tc.name      = name;
        tc.arguments = args;
        msg.tool_calls.push_back(std::move(tc));
    }
    return msg;
}

/// 构造 tool 结果消息 (与上一条 assistant 的 tool_calls 对应)
neograph::ChatMessage makeToolResultMsg() {
    neograph::ChatMessage msg;
    msg.role         = "tool";
    msg.content      = "ok";
    msg.tool_call_id = "call_x";
    msg.tool_name    = "some_tool";
    return msg;
}

/// 构造普通文本消息 (user/system)
neograph::ChatMessage makeTextMsg(std::string_view role) {
    neograph::ChatMessage msg;
    msg.role    = std::string{role};
    msg.content = "hello";
    return msg;
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

    // ===================== number(double) <-> integer =====================
    // #28 integer -> number: 整数参数按声明转为 double
    {
        neograph::ChatTool def;
        def.name       = "tool_ac";
        def.parameters = makeParams({"ratio", {{"type", "number"}}});
        auto args      = neograph::json{
                 {"ratio", 42}
        };
        XX_TEST_EXPECT_TRUE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_TRUE(args["ratio"].is_number_float());
        XX_TEST_EXPECT_EQ(args["ratio"].get<double>(), 42.0);
    }

    // #29 unsigned -> number: 无符号整数同样转为 double
    {
        neograph::ChatTool def;
        def.name       = "tool_ad";
        def.parameters = makeParams({"size", {{"type", "number"}}});
        auto args      = neograph::json{
                 {"size", 12345678901ULL}
        };
        XX_TEST_EXPECT_TRUE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_TRUE(args["size"].is_number_float());
        XX_TEST_EXPECT_EQ(args["size"].get<double>(), 12345678901.0);
    }

    // #30 number -> integer: 浮点值恰为整数值时无损转换 (3.0 -> 3)
    {
        neograph::ChatTool def;
        def.name       = "tool_ae";
        def.parameters = makeParams({"count", {{"type", "integer"}}});
        auto args      = neograph::json{
                 {"count", 3.0}
        };
        XX_TEST_EXPECT_TRUE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_TRUE(args["count"].is_number_integer());
        XX_TEST_EXPECT_EQ(args["count"].get<long long>(), 3);
    }

    // #31 number -> integer: 负数整值浮点同样转换 (-3.0 -> -3)
    {
        neograph::ChatTool def;
        def.name       = "tool_af";
        def.parameters = makeParams({"count", {{"type", "integer"}}});
        auto args      = neograph::json{
                 {"count", -3.0}
        };
        XX_TEST_EXPECT_TRUE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_TRUE(args["count"].is_number_integer());
        XX_TEST_EXPECT_EQ(args["count"].get<long long>(), -3);
    }

    // #32 number -> integer: 非整数值浮点保持原样 (3.5 无法无损转整数)
    {
        neograph::ChatTool def;
        def.name       = "tool_ag";
        def.parameters = makeParams({"count", {{"type", "integer"}}});
        auto args      = neograph::json{
                 {"count", 3.5}
        };
        XX_TEST_EXPECT_FALSE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_TRUE(args["count"].is_number_float());
        XX_TEST_EXPECT_EQ(args["count"].get<double>(), 3.5);
    }

    // #33 number -> integer: 超出 int64 表示范围保持原样 (1e20)
    {
        neograph::ChatTool def;
        def.name       = "tool_ah";
        def.parameters = makeParams({"count", {{"type", "integer"}}});
        auto args      = neograph::json{
                 {"count", 1e20}
        };
        XX_TEST_EXPECT_FALSE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_TRUE(args["count"].is_number_float());
    }

    // #34 联合类型 ["number", "integer"]: 整数已合法不转换; 整值浮点转为整数
    {
        neograph::ChatTool def;
        def.name = "tool_ai";
        def.parameters
            = makeParams({"v", {{"type", neograph::json::array({"number", "integer"})}}});
        auto args = neograph::json{
            {"v", 7}
        };
        // 整数同时满足两种声明, 不转换
        XX_TEST_EXPECT_FALSE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_TRUE(args["v"].is_number_integer());

        args = neograph::json{
            {"v", 2.0}
        };
        // 声明含 integer, 整值浮点转为整数后对两种声明均合法
        XX_TEST_EXPECT_TRUE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_TRUE(args["v"].is_number_integer());
        XX_TEST_EXPECT_EQ(args["v"].get<long long>(), 2);
    }

    // #35 联合类型 ["string", "number"]: 数值参数已合法, 不做数值间转换
    {
        neograph::ChatTool def;
        def.name       = "tool_aj";
        def.parameters = makeParams({"v", {{"type", neograph::json::array({"string", "number"})}}});
        auto args      = neograph::json{
                 {"v", 5}
        };
        XX_TEST_EXPECT_FALSE(agentxx::nodes::ToolcallWrapNode::autoFixArgsType(def, args));
        XX_TEST_EXPECT_TRUE(args["v"].is_number_integer());
        XX_TEST_EXPECT_EQ(args["v"].get<long long>(), 5);
    }

    // ===================== 连续重复调用检测 =====================
    using nodes::ToolcallWrapNode;

    // #36 makeRepeatCallKey: 相同 tool + 相同参数得到相同 key
    {
        const auto key1 = ToolcallWrapNode::makeRepeatCallKey("tool_a", R"({"p":1})");
        const auto key2 = ToolcallWrapNode::makeRepeatCallKey("tool_a", R"({"p":1})");
        XX_TEST_EXPECT_EQ(key1, key2);
    }
    // #37 makeRepeatCallKey: 不同 tool / 不同参数得到不同 key
    {
        const auto base  = ToolcallWrapNode::makeRepeatCallKey("tool_a", R"({"p":1})");
        const auto other = ToolcallWrapNode::makeRepeatCallKey("tool_b", R"({"p":1})");
        XX_TEST_EXPECT_TRUE(base != other);
        // 参数内容不同 (长度也不同): key 必须不同
        XX_TEST_EXPECT_TRUE(base != ToolcallWrapNode::makeRepeatCallKey("tool_a", R"({"p":2})"));
        // key 格式: {toolName}_{arg长度}_{hash}, 以 toolName_ 开头且包含参数长度段
        XX_TEST_EXPECT_TRUE(base.starts_with("tool_a_7_"));
    }
    // #38 makeRepeatCallKey: 长度相同但内容不同的参数 (降低哈希碰撞误判的验证口径)
    {
        const auto k1 = ToolcallWrapNode::makeRepeatCallKey("tool_a", "abcdefgh");
        const auto k2 = ToolcallWrapNode::makeRepeatCallKey("tool_a", "abcdefgi");
        XX_TEST_EXPECT_TRUE(k1 != k2);
    }

    // ---- findConsecutiveRepeatCallKeys ----
    constexpr size_t T5 = 5; // 与 AgentConfig::toolcallRepeatCheckThreshold 默认值一致

    // #39 空入参 / 阈值为 0 (禁用) 防御
    {
        auto msgs = std::vector<neograph::ChatMessage>{};
        XX_TEST_EXPECT_TRUE(ToolcallWrapNode::findConsecutiveRepeatCallKeys(msgs, 0, T5).empty());
        auto assistant = makeAssistantMsg({
            {"t", "{}"}
        });
        msgs.push_back(assistant);
        XX_TEST_EXPECT_TRUE(ToolcallWrapNode::findConsecutiveRepeatCallKeys(msgs, 0, T5).empty());
        auto last = msgs.size() - 1;
        XX_TEST_EXPECT_TRUE(ToolcallWrapNode::findConsecutiveRepeatCallKeys(msgs, last, 0).empty());
    }
    // #40 单次调用不触发 (未达阈值)
    {
        auto msgs = std::vector<neograph::ChatMessage>{};
        msgs.push_back(makeTextMsg("user"));
        msgs.push_back(makeAssistantMsg({
            {"read_file", KEY_A_TXT}
        }));
        auto last = msgs.size() - 1;
        XX_TEST_EXPECT_TRUE(ToolcallWrapNode::findConsecutiveRepeatCallKeys(msgs, last, T5).empty()
        );
    }
    // #41 连续 5 次相同调用 (llm -> tool 交替): 触发且返回正确 key
    {
        auto msgs = std::vector<neograph::ChatMessage>{};
        msgs.push_back(makeTextMsg("user"));
        for (size_t i = 0; i < 4; ++i) {
            msgs.push_back(makeAssistantMsg({
                {"read_file", KEY_A_TXT}
            }));
            msgs.push_back(makeToolResultMsg());
        }
        msgs.push_back(makeAssistantMsg({
            {"read_file", KEY_A_TXT}
        }));
        auto last = msgs.size() - 1;
        auto hit  = ToolcallWrapNode::findConsecutiveRepeatCallKeys(msgs, last, T5);
        XX_TEST_EXPECT_EQ(hit.size(), size_t{1});
        XX_TEST_EXPECT_TRUE(
            hit.count(ToolcallWrapNode::makeRepeatCallKey("read_file", KEY_A_TXT)) > 0
        );
    }
    // #42 连续 4 次不触发 (差一次达阈值)
    {
        auto msgs = std::vector<neograph::ChatMessage>{};
        msgs.push_back(makeTextMsg("user"));
        for (size_t i = 0; i < 3; ++i) {
            msgs.push_back(makeAssistantMsg({
                {"read_file", KEY_A_TXT}
            }));
            msgs.push_back(makeToolResultMsg());
        }
        msgs.push_back(makeAssistantMsg({
            {"read_file", KEY_A_TXT}
        }));
        auto last = msgs.size() - 1;
        XX_TEST_EXPECT_TRUE(ToolcallWrapNode::findConsecutiveRepeatCallKeys(msgs, last, T5).empty()
        );
    }
    // #43 中间有用户消息打断: 仅统计最后一条用户消息之后的连续部分
    {
        auto msgs = std::vector<neograph::ChatMessage>{};
        for (size_t i = 0; i < 6; ++i) {
            msgs.push_back(makeAssistantMsg({
                {"read_file", KEY_A_TXT}
            }));
            msgs.push_back(makeToolResultMsg());
        }
        msgs.push_back(makeTextMsg("user")); // 用户介入, 打断连续性
        for (size_t i = 0; i < 2; ++i) {
            msgs.push_back(makeAssistantMsg({
                {"read_file", KEY_A_TXT}
            }));
            msgs.push_back(makeToolResultMsg());
        }
        msgs.push_back(makeAssistantMsg({
            {"read_file", KEY_A_TXT}
        }));
        auto last = msgs.size() - 1;
        // 仅统计用户消息之后的 3 次 (< 5), 不触发
        XX_TEST_EXPECT_TRUE(ToolcallWrapNode::findConsecutiveRepeatCallKeys(msgs, last, T5).empty()
        );

        // 用户消息之后补足到 5 次: 触发
        for (size_t i = 0; i < 2; ++i) {
            msgs.push_back(makeAssistantMsg({
                {"read_file", KEY_A_TXT}
            }));
            msgs.push_back(makeToolResultMsg());
        }
        last     = msgs.size() - 1;
        auto hit = ToolcallWrapNode::findConsecutiveRepeatCallKeys(msgs, last, T5);
        XX_TEST_EXPECT_EQ(hit.size(), size_t{1});
        XX_TEST_EXPECT_TRUE(
            hit.count(ToolcallWrapNode::makeRepeatCallKey("read_file", KEY_A_TXT)) > 0
        );
    }
    // #44 system 消息同样打断连续性
    {
        auto msgs = std::vector<neograph::ChatMessage>{};
        for (size_t i = 0; i < 6; ++i) {
            msgs.push_back(makeAssistantMsg({
                {"run", R"({"cmd":"ls"})"}
            }));
            msgs.push_back(makeToolResultMsg());
        }
        msgs.push_back(makeTextMsg("system"));
        msgs.push_back(makeAssistantMsg({
            {"run", R"({"cmd":"ls"})"}
        }));
        auto last = msgs.size() - 1;
        XX_TEST_EXPECT_TRUE(ToolcallWrapNode::findConsecutiveRepeatCallKeys(msgs, last, T5).empty()
        );
    }
    // #45 不带 tool_calls 的最终回复 assistant 打断连续性
    {
        auto msgs = std::vector<neograph::ChatMessage>{};
        for (size_t i = 0; i < 6; ++i) {
            msgs.push_back(makeAssistantMsg({
                {"read_file", KEY_A_TXT}
            }));
            msgs.push_back(makeToolResultMsg());
        }
        neograph::ChatMessage finalReply;
        finalReply.role    = "assistant";
        finalReply.content = "done";
        msgs.push_back(finalReply);
        msgs.push_back(makeTextMsg("user"));
        msgs.push_back(makeAssistantMsg({
            {"read_file", KEY_A_TXT}
        }));
        auto last = msgs.size() - 1;
        XX_TEST_EXPECT_TRUE(ToolcallWrapNode::findConsecutiveRepeatCallKeys(msgs, last, T5).empty()
        );
    }
    // #46 并行 tool_calls: 同一 assistant 内多个相同调用均计数
    {
        auto msgs = std::vector<neograph::ChatMessage>{};
        msgs.push_back(makeTextMsg("user"));
        const auto grepKey = R"({"pattern":"x"})";
        for (size_t i = 0; i < 2; ++i) {
            msgs.push_back(makeAssistantMsg({
                {"grep", grepKey},
                {"grep", grepKey},
            }));
            msgs.push_back(makeToolResultMsg());
        }
        auto last = msgs.size() - 1;
        // 两轮各 2 次, 共 4 次: 阈值 4 时触发
        auto hit = ToolcallWrapNode::findConsecutiveRepeatCallKeys(msgs, last, 4);
        XX_TEST_EXPECT_EQ(hit.size(), size_t{1});
        XX_TEST_EXPECT_TRUE(hit.count(ToolcallWrapNode::makeRepeatCallKey("grep", grepKey)) > 0);
        // 阈值 5 时不触发
        XX_TEST_EXPECT_TRUE(ToolcallWrapNode::findConsecutiveRepeatCallKeys(msgs, last, 5).empty());
    }
    // #47 不同参数/不同工具分开计数, 仅达标者入选
    // (b.txt 与 a.txt 同轮出现: 避免纯新 key 轮触发零重叠截断, 见 #48)
    {
        auto msgs = std::vector<neograph::ChatMessage>{};
        msgs.push_back(makeTextMsg("user"));
        for (size_t i = 0; i < 5; ++i) {
            msgs.push_back(makeAssistantMsg({
                {"read_file", KEY_A_TXT}
            }));
            msgs.push_back(makeToolResultMsg());
        }
        for (size_t i = 0; i < 2; ++i) {
            msgs.push_back(makeAssistantMsg({
                {"read_file", KEY_A_TXT            },
                {"read_file", R"({"path":"b.txt"})"},
            }));
            msgs.push_back(makeToolResultMsg());
        }
        msgs.push_back(makeAssistantMsg({
            {"read_file", KEY_A_TXT        },
            {"list_dir",  R"({"path":"."})"},
        }));
        auto last = msgs.size() - 1;
        auto hit  = ToolcallWrapNode::findConsecutiveRepeatCallKeys(msgs, last, T5);
        // a.txt 达标 (8 次); b.txt (2 次) 与 list_dir (1 次) 不入选
        XX_TEST_EXPECT_EQ(hit.size(), size_t{1});
        XX_TEST_EXPECT_TRUE(
            hit.count(ToolcallWrapNode::makeRepeatCallKey("read_file", KEY_A_TXT)) > 0
        );
    }
    // #48 提前终止 - 全新 key 的 assistant 截断重复计数:
    // 某条 assistant 的所有 tool_call key 均未出现过时, 该条开启了全新调用,
    // 重复计数至多延续到这里 (即使更早还有相同调用也不计入)
    {
        auto msgs = std::vector<neograph::ChatMessage>{};
        msgs.push_back(makeTextMsg("user"));
        for (size_t i = 0; i < 3; ++i) {
            msgs.push_back(makeAssistantMsg({
                {"read_file", KEY_A_TXT}
            }));
            msgs.push_back(makeToolResultMsg());
        }
        // 全新调用的一轮: 截断回溯
        msgs.push_back(makeAssistantMsg({
            {"write_file", R"({"path":"b.txt"})"}
        }));
        msgs.push_back(makeToolResultMsg());
        for (size_t i = 0; i < 3; ++i) {
            msgs.push_back(makeAssistantMsg({
                {"read_file", KEY_A_TXT}
            }));
            msgs.push_back(makeToolResultMsg());
        }
        auto last = msgs.size() - 1;
        // write_file 轮之后仅 3 次 (< 5); 更早的 3 次已被截断, 不触发
        XX_TEST_EXPECT_TRUE(ToolcallWrapNode::findConsecutiveRepeatCallKeys(msgs, last, T5).empty()
        );

        // 截断点之后的连续部分补足到 5 次: 触发
        for (size_t i = 0; i < 2; ++i) {
            msgs.push_back(makeAssistantMsg({
                {"read_file", KEY_A_TXT}
            }));
            msgs.push_back(makeToolResultMsg());
        }
        last     = msgs.size() - 1;
        auto hit = ToolcallWrapNode::findConsecutiveRepeatCallKeys(msgs, last, T5);
        XX_TEST_EXPECT_EQ(hit.size(), size_t{1});
        XX_TEST_EXPECT_TRUE(
            hit.count(ToolcallWrapNode::makeRepeatCallKey("read_file", KEY_A_TXT)) > 0
        );
    }
    // #49 提前终止 - 回溯上限 threshold 条 assistant:
    // 某 key 若真的连续出现达 threshold 次, 最近 threshold 条 assistant 每条必含该 key,
    // 因此检查满 threshold 条仍未确定循环时即可终止 (更早的消息不可能补足缺口)
    {
        auto msgs = std::vector<neograph::ChatMessage>{};
        msgs.push_back(makeTextMsg("user"));
        // 4 轮 assistant 各含 a.txt/b.txt 两种调用: a/b 各出现 4 次 (< 5)
        for (size_t i = 0; i < 4; ++i) {
            msgs.push_back(makeAssistantMsg({
                {"read_file", KEY_A_TXT            },
                {"read_file", R"({"path":"b.txt"})"},
            }));
            msgs.push_back(makeToolResultMsg());
        }
        auto last = msgs.size() - 1;
        // a/b 均不足 5 次, 不触发; 链内消息数超过回溯上限时同样正确终止
        XX_TEST_EXPECT_TRUE(ToolcallWrapNode::findConsecutiveRepeatCallKeys(msgs, last, T5).empty()
        );
    }
    // #50 同轮并行即可直接达到阈值 (无需历史)
    {
        auto msgs = std::vector<neograph::ChatMessage>{};
        msgs.push_back(makeTextMsg("user"));
        const auto grepKey = R"({"pattern":"x"})";
        msgs.push_back(makeAssistantMsg({
            {"grep", grepKey},
            {"grep", grepKey},
            {"grep", grepKey},
            {"grep", grepKey},
            {"grep", grepKey},
        }));
        auto last = msgs.size() - 1;
        auto hit  = ToolcallWrapNode::findConsecutiveRepeatCallKeys(msgs, last, T5);
        XX_TEST_EXPECT_EQ(hit.size(), size_t{1});
        XX_TEST_EXPECT_TRUE(hit.count(ToolcallWrapNode::makeRepeatCallKey("grep", grepKey)) > 0);
    }
    return TestResult{g_tca_passed, g_tca_failed};
}

} // namespace test
} // namespace agentxx
