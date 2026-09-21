// 客户端 UI 组件描述 schema 测试 (解析/校验/序列化/纯文本降级/列宽)
//
// 覆盖场景:
// - 解析: 各 kind 字段归一化、旧写法兼容、未知 kind 与非法输入
// - 往返: dump → parse 后关键字段不变 (含 canvas 原样保留)
// - 上限: 嵌套深度、单层元素数、表格行列、文本长度
// - 纯文本降级: 表格列对齐、树连接线、键值、趋势图、计量条、控件说明、容器
// - 列宽: 宽字符/组合字符计数、按列宽截断与补齐
#include "agentxx-test/core/test_ui_items.h"

#include "agentxx/ui/build.h"
#include "agentxx/ui/item.h"
#include "agentxx/ui/text_width.h"
#include <string>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_ui_items_passed = 0;
int g_ui_items_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_ui_items_passed
#define XX_TEST_FAILED g_ui_items_failed

namespace agentxx {
namespace test {

using utilxx_base::Json;
using agentxx::ui::Item;

namespace {

/// 解析单条 JSON (解析失败返回 known=false 的项)
Item parseOne(const char* json) {
    return agentxx::ui::parseItem(Json::parse(json));
}

/// 解析 JSON 数组 (解析失败时记录问题输入并返回空列表)
std::vector<Item> parseMany(const char* json) {
    try {
        return agentxx::ui::parseItems(Json::parse(json));
    } catch (const std::exception& e) {
        std::string shown;
        for (char c : std::string_view{json}) {
            if (c == '\n') {
                shown += "\\n";
            } else {
                shown.push_back(c);
            }
        }
        TEST_FAIL << "parseMany failed: " << e.what() << " | len=" << shown.size()
                  << " input=" << shown << std::endl;
        return {};
    }
}

/// 子串存在性 (纯文本降级断言用)
bool has(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

} // namespace

TestResult testUiItems() {
    // ---------------- 文本类 ----------------
    {
        auto item = parseOne(R"({"kind":"text","text":"hello","role":"title"})");
        XX_TEST_EXPECT_TRUE(item.known);
        XX_TEST_EXPECT_EQ(item.kind, std::string{"text"});
        XX_TEST_EXPECT_EQ(item.text, std::string{"hello"});
        XX_TEST_EXPECT_EQ(item.color, std::string{"title"});
        XX_TEST_EXPECT_TRUE(item.bold); // title 隐含加粗 (与历史渲染语义一致)
        XX_TEST_EXPECT_TRUE(item.wrap); // 文本缺省折行
    }
    {
        // 缺省 kind = text
        auto item = parseOne(R"({"text":"no kind"})");
        XX_TEST_EXPECT_EQ(item.kind, std::string{"text"});
        XX_TEST_EXPECT_EQ(item.text, std::string{"no kind"});
    }
    {
        auto item = parseOne(R"({"kind":"gap","lines":3})");
        XX_TEST_EXPECT_EQ(item.lines, 3);
        // 上限: 空行数被限制在 50 以内
        auto big = parseOne(R"({"kind":"gap","lines":9999})");
        XX_TEST_EXPECT_EQ(big.lines, 50);
    }
    {
        auto item = parseOne(R"({"kind":"markdown","text":"# title"})");
        XX_TEST_EXPECT_EQ(item.text, std::string{"# title"});
        XX_TEST_EXPECT_FALSE(item.wrap); // 结构化/富文本不折行, 由渲染库自行处理
    }
    {
        auto item = parseOne(R"({"kind":"diff","path":"a.cpp","old_str":"x","new_str":"y"})");
        XX_TEST_EXPECT_EQ(item.path, std::string{"a.cpp"});
        XX_TEST_EXPECT_EQ(item.oldStr, std::string{"x"});
        XX_TEST_EXPECT_EQ(item.newStr, std::string{"y"});
    }
    {
        // 旧写法: oldStr/newStr 驼峰
        auto item = parseOne(R"({"kind":"diff","path":"p","oldStr":"a","newStr":"b"})");
        XX_TEST_EXPECT_EQ(item.oldStr, std::string{"a"});
        XX_TEST_EXPECT_EQ(item.newStr, std::string{"b"});
    }

    // ---------------- 未知 kind 与非法输入 ----------------
    {
        auto item = parseOne(R"({"kind":"totally-unknown","fallback":"降级文本"})");
        XX_TEST_EXPECT_FALSE(item.known);
        XX_TEST_EXPECT_EQ(item.fallback, std::string{"降级文本"});
    }
    {
        auto item = agentxx::ui::parseItem(Json::array()); // 非对象
        XX_TEST_EXPECT_FALSE(item.known);
    }

    // ---------------- 按钮 (三种历史写法) ----------------
    {
        auto item = parseOne(R"({"kind":"button","label":"Go","action":"run"})");
        XX_TEST_EXPECT_EQ(item.kind, std::string{"button"});
        XX_TEST_EXPECT_EQ(item.label, std::string{"Go"});
        XX_TEST_EXPECT_EQ(item.action, std::string{"run"});
    }
    {
        // 旧写法: kind=button + action_id
        auto item = parseOne(R"({"kind":"button","label":"Go","action_id":"run2"})");
        XX_TEST_EXPECT_EQ(item.action, std::string{"run2"});
    }
    {
        // 遗留写法: kind=action + id 作为动作
        auto item = parseOne(R"({"kind":"action","id":"rebuild","label":"Rebuild"})");
        XX_TEST_EXPECT_EQ(item.kind, std::string{"button"});
        XX_TEST_EXPECT_EQ(item.action, std::string{"rebuild"});
        XX_TEST_EXPECT_EQ(item.label, std::string{"Rebuild"});
    }
    {
        // label 缺失时回退默认 (避免渲染空按钮)
        auto item = parseOne(R"({"kind":"button"})");
        XX_TEST_EXPECT_EQ(item.label, std::string{"Button"});
    }

    // ---------------- 进度条归一化为计量条 ----------------
    {
        auto item = parseOne(R"({"kind":"progress","value":0.5})");
        XX_TEST_EXPECT_EQ(item.kind, std::string{"meter"});
        XX_TEST_EXPECT_EQ(item.value, 0.5 * 100.0);
        XX_TEST_EXPECT_EQ(item.total, 100.0);
        XX_TEST_EXPECT_EQ(item.unit, std::string{"%"});
    }
    {
        auto item
            = parseOne(R"({"kind":"meter","value":72,"total":100,"width":24,"unit":"%",
                           "thresholds":[{"at":60,"color":"thinking"},{"at":80,"color":"error"}]})");
        XX_TEST_EXPECT_EQ(item.value, 72.0);
        XX_TEST_EXPECT_EQ(item.width, 24);
        XX_TEST_EXPECT_EQ(item.thresholds.size(), size_t{2});
        // 阈值按由高到低排序 (匹配首个满足项)
        XX_TEST_EXPECT_EQ(item.thresholds[0].at, 80.0);
        XX_TEST_EXPECT_EQ(item.thresholds[0].color, std::string{"error"});
    }

    // ---------------- 迷你趋势图 ----------------
    {
        auto item = parseOne(R"({"kind":"sparkline","data":[1,2,3],"height":2,"min":0,"max":10,
                                 "showLast":true,"colors":["normal","error"]})");
        XX_TEST_EXPECT_EQ(item.data.size(), size_t{3});
        XX_TEST_EXPECT_EQ(item.height, 2);
        XX_TEST_EXPECT_TRUE(item.hasMin);
        XX_TEST_EXPECT_TRUE(item.hasMax);
        XX_TEST_EXPECT_EQ(item.minValue, 0.0);
        XX_TEST_EXPECT_EQ(item.maxValue, 10.0);
        XX_TEST_EXPECT_TRUE(item.showLast);
        XX_TEST_EXPECT_EQ(item.colors.size(), size_t{2});
    }
    {
        // 数据点超上限时截断
        std::string json = R"({"kind":"sparkline","data":[)";
        for (int i = 0; i < 5000; ++i) {
            if (i > 0) {
                json += ",";
            }
            json += "1";
        }
        json += "]}";
        auto item = agentxx::ui::parseItem(Json::parse(json));
        XX_TEST_EXPECT_EQ(item.data.size(), size_t{4096});
    }

    // ---------------- 表格 ----------------
    {
        auto item = parseOne(R"({
            "kind":"table","header":true,
            "columns":[{"title":"File","w":"flex"},{"title":"Size","align":"right","w":8}],
            "rows":[["main.cpp","12.4 KB"],
                    ["gfx.cpp",{"text":"warn","color":"error","action":"open:gfx.cpp"}]]
        })");
        XX_TEST_EXPECT_EQ(item.columns.size(), size_t{2});
        XX_TEST_EXPECT_EQ(item.columns[0].flex, true);
        XX_TEST_EXPECT_EQ(item.columns[1].width, 8);
        XX_TEST_EXPECT_EQ(item.columns[1].align, std::string{"right"});
        XX_TEST_EXPECT_EQ(item.rows.size(), size_t{2});
        XX_TEST_EXPECT_EQ(item.rows[0][0].text, std::string{"main.cpp"});
        XX_TEST_EXPECT_EQ(item.rows[1][1].text, std::string{"warn"});
        XX_TEST_EXPECT_EQ(item.rows[1][1].action, std::string{"open:gfx.cpp"});
        XX_TEST_EXPECT_EQ(item.rows[1][1].color, std::string{"error"});
        XX_TEST_EXPECT_TRUE(item.interactive()); // 单元格带动作
    }
    {
        // 只有 rows 的简写: 按首行列数补列
        auto item = parseOne(R"({"kind":"table","rows":[["a","b"]]})");
        XX_TEST_EXPECT_EQ(item.columns.size(), size_t{2});
    }
    {
        // 单元格是可点结构 → interactive
        auto plain = parseOne(R"({"kind":"table","rows":[["a","b"]]})");
        XX_TEST_EXPECT_FALSE(plain.interactive());
    }

    // ---------------- 树 ----------------
    {
        auto item = parseOne(R"({"kind":"tree","nodes":[
            {"label":"src","children":[{"label":"main.cpp","action":"open:main.cpp"}]}
        ]})");
        XX_TEST_EXPECT_EQ(item.nodes.size(), size_t{1});
        XX_TEST_EXPECT_EQ(item.nodes[0].label, std::string{"src"});
        XX_TEST_EXPECT_EQ(item.nodes[0].children.size(), size_t{1});
        XX_TEST_EXPECT_EQ(item.nodes[0].children[0].action, std::string{"open:main.cpp"});
        XX_TEST_EXPECT_EQ(item.nodes[0].count(), size_t{2});
        XX_TEST_EXPECT_TRUE(item.interactive());
    }

    // ---------------- 键值对 ----------------
    {
        auto item = parseOne(R"({"kind":"kv","items":[{"k":"Model","v":"gpt-x"},
                                                       {"k":"Tokens","v":"12.3K","vColor":"accent"}]})");
        XX_TEST_EXPECT_EQ(item.pairs.size(), size_t{2});
        XX_TEST_EXPECT_EQ(item.pairs[0].key, std::string{"Model"});
        XX_TEST_EXPECT_EQ(item.pairs[0].value, std::string{"gpt-x"});
        XX_TEST_EXPECT_EQ(item.pairs[1].valueColor, std::string{"accent"});
    }
    {
        // 值为数值/布尔时转文本
        auto item = parseOne(R"({"kind":"kv","items":[{"k":"n","v":12},{"k":"b","v":true}]})");
        XX_TEST_EXPECT_EQ(item.pairs[0].value, std::string{"12"});
        XX_TEST_EXPECT_EQ(item.pairs[1].value, std::string{"true"});
    }

    // ---------------- 容器 (row / box / collapse) ----------------
    {
        auto items = parseMany(R"([
            {"kind":"row","gap":2,"align":"center","items":[
                {"kind":"text","text":"a","w":6},
                {"kind":"text","text":"b","w":"flex"}]},
            {"kind":"box","title":"Index","border":"round","pad":1,
             "items":[{"kind":"text","text":"inner"}]},
            {"kind":"collapse","id":"stack","title":"Stack","expanded":false,
             "items":[{"kind":"k v","text":"x"}]}
        ])");
        XX_TEST_EXPECT_EQ(items.size(), size_t{3});
        XX_TEST_EXPECT_EQ(items[0].items.size(), size_t{2});
        XX_TEST_EXPECT_EQ(items[0].gap, 2);
        XX_TEST_EXPECT_EQ(items[0].align, std::string{"center"});
        XX_TEST_EXPECT_EQ(items[0].items[0].columnWidth, 6);
        XX_TEST_EXPECT_TRUE(items[0].items[1].columnFlex);
        XX_TEST_EXPECT_EQ(items[1].title, std::string{"Index"});
        XX_TEST_EXPECT_EQ(items[1].border, std::string{"round"});
        XX_TEST_EXPECT_EQ(items[1].pad, 1);
        XX_TEST_EXPECT_EQ(items[2].id, std::string{"stack"});
        XX_TEST_EXPECT_FALSE(items[2].expanded);
        // 子项的未知 kind 不影响父项 (父项仍可用)
        XX_TEST_EXPECT_TRUE(items[2].known);
        XX_TEST_EXPECT_FALSE(items[2].items[0].known);
    }
    {
        // 嵌套深度上限: 超过深度上限的子树被丢弃
        std::string json = R"({"kind":"box","items":[)";
        const int   depth = 12;
        for (int i = 0; i < depth; ++i) {
            json += R"({"kind":"box","items":[)";
        }
        json += R"({"kind":"text","text":"deep"})";
        for (int i = 0; i < depth; ++i) {
            json += "]}";
        }
        json += "]}";
        auto items = agentxx::ui::parseItems(Json::array({Json::parse(json)}));
        XX_TEST_EXPECT_EQ(items.size(), size_t{1});
        // 逐层下钻: 到第 8 层后子项被丢弃 (不再有 items)
        int  levels = 0;
        const Item* cur = &items[0];
        while (!cur->items.empty() && levels < 40) {
            cur = &cur->items[0];
            ++levels;
        }
        XX_TEST_EXPECT_TRUE(levels <= 8);
    }
    {
        // 单层元素数上限 512
        std::string json = "[";
        for (int i = 0; i < 600; ++i) {
            if (i > 0) {
                json += ",";
            }
            json += R"({"kind":"gap"})";
        }
        json += "]";
        auto items = agentxx::ui::parseItems(Json::parse(json));
        XX_TEST_EXPECT_EQ(items.size(), size_t{512});
    }

    // ---------------- 控件与提交行 ----------------
    {
        auto item = parseOne(R"({"kind":"control","id":"mode","control":"buttons",
                                 "label":"模式","commitOnPick":true,
                                 "options":[{"value":"fast","label":"Fast"},
                                            {"value":"safe","label":"Safe"}],
                                 "default":"safe"})");
        XX_TEST_EXPECT_EQ(item.kind, std::string{"control"});
        XX_TEST_EXPECT_EQ(item.id, std::string{"mode"});
        XX_TEST_EXPECT_EQ(item.controlLabel, std::string{"模式"});
        XX_TEST_EXPECT_TRUE(item.commitOnPick);
        XX_TEST_EXPECT_EQ(item.options.size(), size_t{2});
        XX_TEST_EXPECT_EQ(item.options[0].label, std::string{"Fast"});
        XX_TEST_EXPECT_TRUE(item.interactive());
    }
    {
        // 候选项为纯字符串时, 值与标签都取该字符串
        auto item = parseOne(R"({"kind":"select","id":"s","options":["a","b"]})");
        XX_TEST_EXPECT_EQ(item.kind, std::string{"control"});
        XX_TEST_EXPECT_EQ(item.control, std::string{"select"});
        XX_TEST_EXPECT_EQ(item.options.size(), size_t{2});
        XX_TEST_EXPECT_EQ(item.options[1].label, std::string{"b"});
        XX_TEST_EXPECT_EQ(item.options[1].value.get<std::string>(), std::string{"b"});
    }
    {
        // 控件短写法归一化: 形态取自写法本身
        XX_TEST_EXPECT_EQ(
            parseOne(R"({"kind":"checkbox","id":"c"})").control,
            std::string{"checkbox"}
        );
        XX_TEST_EXPECT_EQ(
            parseOne(R"({"kind":"buttons","id":"b"})").control,
            std::string{"buttons"}
        );
        XX_TEST_EXPECT_EQ(
            parseOne(R"({"kind":"number","id":"n"})").control,
            std::string{"number"}
        );
        XX_TEST_EXPECT_EQ(
            parseOne(R"({"kind":"input","id":"i"})").control,
            std::string{"text"}
        );
    }
    {
        auto item = parseOne(R"({"kind":"control","id":"n","control":"number","default":3,
                                 "integer":true,"min":1,"max":9,"step":2})");
        XX_TEST_EXPECT_TRUE(item.integer);
        XX_TEST_EXPECT_TRUE(item.hasNumMin);
        XX_TEST_EXPECT_EQ(item.numMin, 1.0);
        XX_TEST_EXPECT_TRUE(item.hasNumMax);
        XX_TEST_EXPECT_EQ(item.numMax, 9.0);
        XX_TEST_EXPECT_EQ(item.step, 2.0);
    }
    {
        auto item = parseOne(R"({"kind":"submit","label":"应用","cancelLabel":"取消"})");
        XX_TEST_EXPECT_EQ(item.label, std::string{"应用"});
        XX_TEST_EXPECT_EQ(item.cancelLabel, std::string{"取消"});
    }

    // ---------------- custom / canvas ----------------
    {
        auto item = parseOne(R"({"kind":"custom","component":"components",
                                 "props":{"items":[{"kind":"text","text":"inside"}]}})");
        XX_TEST_EXPECT_EQ(item.kind, std::string{"custom"});
        XX_TEST_EXPECT_EQ(item.component, std::string{"components"});
        XX_TEST_EXPECT_EQ(item.items.size(), size_t{1});
        XX_TEST_EXPECT_EQ(item.items[0].text, std::string{"inside"});
    }
    {
        auto item = parseOne(R"({"kind":"custom","component":"table",
                                 "props":{"rows":[["a","b"]]}})");
        XX_TEST_EXPECT_EQ(item.component, std::string{"table"});
        XX_TEST_EXPECT_TRUE(item.props.is_object());
    }
    {
        auto item = parseOne(R"({"kind":"canvas","w":0,"h":6,"fallback":"[chart]",
                                 "rows":[[0,"abc"]]})");
        XX_TEST_EXPECT_TRUE(item.known);
        XX_TEST_EXPECT_EQ(item.kind, std::string{"canvas"});
        XX_TEST_EXPECT_EQ(item.fallback, std::string{"[chart]"});
        XX_TEST_EXPECT_TRUE(item.canvas.is_object()); // 原始描述原样保留
        XX_TEST_EXPECT_TRUE(item.canvas.contains("rows"));
    }

    // ---------------- 序列化往返 ----------------
    {
        auto items = parseMany(R"([
            {"kind":"table","header":true,
             "columns":[{"title":"A","w":"flex"},{"title":"B","align":"right","w":6}],
             "rows":[["1","2"]]},
            {"kind":"meter","value":30,"total":100,"width":10,
             "thresholds":[{"at":80,"color":"error"}]},
            {"kind":"canvas","h":4,"fallback":"x"}
        ])");
        auto dumped  = agentxx::ui::dumpItems(items);
        auto round   = agentxx::ui::parseItems(dumped);
        XX_TEST_EXPECT_EQ(round.size(), size_t{3});
        XX_TEST_EXPECT_EQ(round[0].columns.size(), size_t{2});
        XX_TEST_EXPECT_EQ(round[0].columns[1].width, 6);
        XX_TEST_EXPECT_EQ(round[0].columns[1].align, std::string{"right"});
        XX_TEST_EXPECT_EQ(round[0].rows.size(), size_t{1});
        XX_TEST_EXPECT_EQ(round[0].rows[0][0].text, std::string{"1"});
        XX_TEST_EXPECT_EQ(round[1].value, 30.0);
        XX_TEST_EXPECT_EQ(round[1].thresholds.size(), size_t{1});
        XX_TEST_EXPECT_EQ(round[1].thresholds[0].color, std::string{"error"});
        // canvas 往返不丢内容
        XX_TEST_EXPECT_EQ(round[2].kind, std::string{"canvas"});
        XX_TEST_EXPECT_EQ(round[2].fallback, std::string{"x"});
        XX_TEST_EXPECT_TRUE(round[2].canvas.contains("h"));
    }
    {
        // 文本长度上限: 超长文本按上限截断 (且不切断 UTF-8 码点)
        std::string longText(70000, 'x');
        std::string json = R"({"kind":"text","text":")" + longText + R"("})";
        auto        item = agentxx::ui::parseItem(Json::parse(json));
        XX_TEST_EXPECT_EQ(item.text.size(), size_t{64 * 1024});
    }

    // ---------------- plainText ----------------
    {
        auto items = parseMany(R"([
            {"kind":"text","text":"标题"},
            {"kind":"kv","items":[{"k":"Model","v":"gpt-x"}]},
            {"kind":"meter","value":50,"total":100,"width":10}
        ])");
        auto text = agentxx::ui::plainText(items);
        XX_TEST_EXPECT_TRUE(has(text, "标题"));
        XX_TEST_EXPECT_TRUE(has(text, "Model"));
        XX_TEST_EXPECT_TRUE(has(text, "gpt-x"));
        XX_TEST_EXPECT_TRUE(has(text, "50%"));
        XX_TEST_EXPECT_TRUE(has(text, "#####-----")); // 条宽 10, 填充 5
    }
    {
        // 表格: 表头 + 分隔线 + 右对齐数值列
        auto items = parseMany(R"([
            {"kind":"table","header":true,
             "columns":[{"title":"Name","w":6},{"title":"Size","align":"right","w":6}],
             "rows":[["a.txt","12"]]}
        ])");
        auto text = agentxx::ui::plainText(items);
        XX_TEST_EXPECT_TRUE(has(text, "Name"));
        // 分隔线按列宽逐列生成 (列宽 = max(表头, 各单元格))
        XX_TEST_EXPECT_TRUE(has(text, "-----  ----"));
        XX_TEST_EXPECT_TRUE(has(text, "a.txt"));
        XX_TEST_EXPECT_TRUE(has(text, "    12")); // 列间距 + 右对齐补白
    }
    {
        // 树: 连接线
        auto items = parseMany(R"([
            {"kind":"tree","nodes":[{"label":"src","children":[{"label":"main.cpp"}]}]}
        ])");
        auto text = agentxx::ui::plainText(items);
        XX_TEST_EXPECT_TRUE(has(text, "└─ src"));
        XX_TEST_EXPECT_TRUE(has(text, "└─ main.cpp"));
    }
    {
        // 折叠: 折叠时不输出内容; 展开时输出
        auto collapsed = parseMany(R"([
            {"kind":"collapse","title":"Stack","expanded":false,
             "items":[{"kind":"text","text":"hidden"}]}
        ])");
        auto ctext = agentxx::ui::plainText(collapsed);
        XX_TEST_EXPECT_TRUE(has(ctext, "Stack"));
        XX_TEST_EXPECT_FALSE(has(ctext, "hidden"));

        auto expanded = parseMany(R"([
            {"kind":"collapse","title":"Stack","expanded":true,
             "items":[{"kind":"text","text":"visible"}]}
        ])");
        XX_TEST_EXPECT_TRUE(has(agentxx::ui::plainText(expanded), "visible"));
    }
    {
        // 容器: box 标题 + 内容; row 以 " | " 连接
        auto items = parseMany(R"([
            {"kind":"box","title":"System","items":[{"kind":"text","text":"inner"}]},
            {"kind":"row","items":[{"kind":"text","text":"CPU"},{"kind":"text","text":"55%"}]}
        ])");
        auto text = agentxx::ui::plainText(items);
        XX_TEST_EXPECT_TRUE(has(text, "System"));
        XX_TEST_EXPECT_TRUE(has(text, "inner"));
        XX_TEST_EXPECT_TRUE(has(text, "CPU | 55%"));
    }
    {
        // 控件: 标签 + 候选项 + 形态说明; 提交行不输出
        auto items = parseMany(R"([
            {"kind":"control","id":"mode","control":"select","label":"模式",
             "options":[{"value":"fast","label":"Fast"},{"value":"safe","label":"Safe"}]},
            {"kind":"submit","label":"Go"}
        ])");
        auto text = agentxx::ui::plainText(items);
        XX_TEST_EXPECT_TRUE(has(text, "模式"));
        XX_TEST_EXPECT_TRUE(has(text, "Fast / Safe"));
        XX_TEST_EXPECT_TRUE(has(text, "(select)"));
        XX_TEST_EXPECT_FALSE(has(text, "Go"));
    }
    {
        // 未知 kind → fallback; canvas → fallback; custom 无内容 → 组件名占位
        auto items = parseMany(R"([
            {"kind":"unknown-x","fallback":"降级"},
            {"kind":"canvas","fallback":"[canvas fallback]"},
            {"kind":"custom","component":"my-widget"}
        ])");
        auto text = agentxx::ui::plainText(items);
        XX_TEST_EXPECT_TRUE(has(text, "降级"));
        XX_TEST_EXPECT_TRUE(has(text, "[canvas fallback]"));
        XX_TEST_EXPECT_TRUE(has(text, "my-widget"));
    }
    {
        // 折行宽度: 传入宽度后长文本被切成多行
        auto items = parseMany(R"([{"kind":"text","text":"0123456789abcdef"}])");
        auto text  = agentxx::ui::plainText(items, 8);
        XX_TEST_EXPECT_EQ(text, std::string{"01234567\n89abcdef"});
    }
    {
        // plainText(Json) 便捷入口: {"items":[...]} 与裸数组都接受
        auto a = agentxx::ui::plainText(Json::parse(R"({"items":[{"kind":"text","text":"x"}]})"));
        auto b = agentxx::ui::plainText(Json::parse(R"([{"kind":"text","text":"x"}])"));
        XX_TEST_EXPECT_EQ(a, std::string{"x"});
        XX_TEST_EXPECT_EQ(b, std::string{"x"});
    }

    // ---------------- 构建器 (build.h) ----------------
    {
        agentxx::ui::Items ui;
        ui.box("System",
               agentxx::ui::Items{}
                   .kv({{"Model", "gpt-x"}, {"Tokens", "12.3K"}})
                   .meter(55, 100, {.width = 20, .label = "CPU", .unit = "%"}),
               {.border = "round", .pad = 1})
            .table({.columns = {{"File", "left", 0, {}}, {"Size", "right", 8, {}}},
                    .rows    = {{Json("main.cpp"), Json("12.4 KB")}}})
            .checkbox("verbose", "Verbose", false)
            .submit("应用", "取消");

        auto items = agentxx::ui::parseItemList(ui.json());
        XX_TEST_EXPECT_EQ(items.size(), size_t{4});
        XX_TEST_EXPECT_EQ(items[0].kind, std::string{"box"});
        XX_TEST_EXPECT_EQ(items[0].title, std::string{"System"});
        XX_TEST_EXPECT_EQ(items[0].pad, 1);
        XX_TEST_EXPECT_EQ(items[0].items.size(), size_t{2});
        XX_TEST_EXPECT_EQ(items[0].items[0].kind, std::string{"kv"});
        XX_TEST_EXPECT_EQ(items[0].items[0].pairs.size(), size_t{2});
        XX_TEST_EXPECT_EQ(items[0].items[1].kind, std::string{"meter"});
        XX_TEST_EXPECT_EQ(items[0].items[1].width, 20);
        XX_TEST_EXPECT_EQ(items[1].kind, std::string{"table"});
        XX_TEST_EXPECT_EQ(items[1].columns.size(), size_t{2});
        XX_TEST_EXPECT_EQ(items[1].rows.size(), size_t{1});
        XX_TEST_EXPECT_EQ(items[2].kind, std::string{"control"});
        XX_TEST_EXPECT_EQ(items[2].control, std::string{"checkbox"});
        XX_TEST_EXPECT_EQ(items[3].kind, std::string{"submit"});
        XX_TEST_EXPECT_EQ(items[3].label, std::string{"应用"});
    }
    {
        // 构建器: 嵌套 Items 合并 (row 的多个列)
        agentxx::ui::Items row;
        row.row(std::vector<agentxx::ui::Items>{
            agentxx::ui::Items{}.text("CPU"),
            agentxx::ui::Items{}.sparkline({1, 2, 3}, {.color = "accent", .showLast = true}),
        });
        auto items = agentxx::ui::parseItemList(row.json());
        XX_TEST_EXPECT_EQ(items.size(), size_t{1});
        XX_TEST_EXPECT_EQ(items[0].items.size(), size_t{2});
        XX_TEST_EXPECT_EQ(items[0].items[1].kind, std::string{"sparkline"});
        XX_TEST_EXPECT_EQ(items[0].items[1].data.size(), size_t{3});
        XX_TEST_EXPECT_TRUE(items[0].items[1].showLast);
    }
    {
        // 构建器: 空树产出空 items 数组
        agentxx::ui::Items empty;
        XX_TEST_EXPECT_TRUE(empty.empty());
        auto parsed = agentxx::ui::parseItemList(empty.json());
        XX_TEST_EXPECT_EQ(parsed.size(), size_t{0});
    }

    // ---------------- 显示列宽 ----------------
    {
        XX_TEST_EXPECT_EQ(agentxx::ui::displayWidth("abc"), 3);
        XX_TEST_EXPECT_EQ(agentxx::ui::displayWidth("中文"), 4);
        XX_TEST_EXPECT_EQ(agentxx::ui::displayWidth("a中"), 3);
        XX_TEST_EXPECT_EQ(agentxx::ui::displayWidth("e\u0301"), 1); // 组合字符 0 列
        XX_TEST_EXPECT_EQ(agentxx::ui::displayWidth(""), 0);
    }
    {
        // 截断: 宽字符安全 + 省略号
        XX_TEST_EXPECT_EQ(agentxx::ui::truncateToWidth("abcdef", 4), std::string{"abc…"});
        XX_TEST_EXPECT_EQ(agentxx::ui::truncateToWidth("中文名", 4), std::string{"中…"});
        XX_TEST_EXPECT_EQ(agentxx::ui::truncateToWidth("abc", 5), std::string{"abc"});
        XX_TEST_EXPECT_EQ(agentxx::ui::truncateToWidth("abc", 0), std::string{});
        XX_TEST_EXPECT_EQ(
            agentxx::ui::truncateToWidth("abcdef", 3, ".."),
            std::string{"a.."}
        );
    }
    {
        XX_TEST_EXPECT_EQ(agentxx::ui::padRightToWidth("ab", 4), std::string{"ab  "});
        XX_TEST_EXPECT_EQ(agentxx::ui::padRightToWidth("abcd", 2), std::string{"abcd"});
        XX_TEST_EXPECT_EQ(agentxx::ui::padRightToWidth("中", 4), std::string{"中  "});
    }
    {
        int              used = 0;
        std::string_view head = agentxx::ui::prefixByWidth("中abc", 3, used);
        XX_TEST_EXPECT_EQ(used, 3);
        XX_TEST_EXPECT_EQ(std::string{head}, std::string{"中a"});
    }

    return TestResult{g_ui_items_passed, g_ui_items_failed};
}

} // namespace test
} // namespace agentxx
