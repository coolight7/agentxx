#include "agentxx-test/core/test_interrupt_ui.h"

#include "agentxx/middlewares/interrupt_presets.h"
#include "agentxx/middlewares/interrupt_ui.h"
#include "agentxx/middlewares/middleware.h"
#include "utilxx_base/json.h"

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_interrupt_ui_passed = 0;
int g_interrupt_ui_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_interrupt_ui_passed
#define XX_TEST_FAILED g_interrupt_ui_failed

namespace agentxx {
namespace test {

using agentxx::middleware::InterruptHandleArg;
using agentxx::middleware::InterruptUi;
using agentxx::middleware::InterruptUiBlock;
using utilxx_base::Json;

namespace {

/// 统计描述中指定形态的控件块数量
size_t countControls(const InterruptUi& ui, std::string_view control) {
    size_t n = 0;
    for (const auto& b : ui.blocks) {
        if (b.kind == "control" && b.control == control) {
            ++n;
        }
    }
    return n;
}

/// 取首个指定形态的控件块 (不存在返回 nullptr)
const InterruptUiBlock* findControl(const InterruptUi& ui, std::string_view control) {
    for (const auto& b : ui.blocks) {
        if (b.kind == "control" && b.control == control) {
            return &b;
        }
    }
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// 描述 JSON 往返
// ---------------------------------------------------------------------------

void test_ui_json_roundtrip() {
    InterruptUi ui;
    ui.header.segments.push_back(agentxx::middleware::InterruptUiSegment{
        .text     = "! [Badge] ",
        .labelKey = "interrupt.badge",
        .color    = "error",
        .bold     = true,
    });
    ui.blocks.push_back(
        agentxx::middleware::preset::textBlock("hello", "hint", 2, true, false, true)
    );
    ui.blocks.push_back(agentxx::middleware::preset::markdownBlock("**md**", 1));
    ui.blocks.push_back(agentxx::middleware::preset::diffBlock("a.txt", "old", "new"));
    ui.blocks.push_back(agentxx::middleware::preset::separatorBlock(1));
    ui.blocks.push_back(agentxx::middleware::preset::gapBlock(3));
    auto number     = agentxx::middleware::preset::numberControl("n", "N", "n.key", 2.0, true, 2.0);
    number.hasMin   = true;
    number.minValue = -1.0;
    number.hasMax   = true;
    number.maxValue = 9.0;
    ui.blocks.push_back(number);
    ui.blocks.push_back(agentxx::middleware::preset::checkboxControl(
        "cb",
        "Check",
        "cb.key",
        true,
        "help",
        "help.key"
    ));
    ui.blocks.push_back(agentxx::middleware::preset::submitBlock("Go", "go.key", "Back", "back.key")
    );

    const auto dumped = ui.toJson();
    XX_TEST_EXPECT_TRUE(dumped.is_object());
    XX_TEST_EXPECT_EQ(dumped.value("version", 0), 1);

    const auto back = InterruptUi::fromJson(dumped);
    XX_TEST_EXPECT_EQ(back.header.segments.size(), size_t{1});
    XX_TEST_EXPECT_EQ(back.blocks.size(), ui.blocks.size());
    // 关键字段逐一还原
    XX_TEST_EXPECT_EQ(back.blocks[0].kind, std::string("text"));
    XX_TEST_EXPECT_EQ(back.blocks[0].text, std::string("hello"));
    XX_TEST_EXPECT_EQ(back.blocks[0].color, std::string("hint"));
    XX_TEST_EXPECT_EQ(back.blocks[0].indent, 2);
    XX_TEST_EXPECT_TRUE(back.blocks[0].wrap);
    XX_TEST_EXPECT_TRUE(back.blocks[0].dim);
    XX_TEST_EXPECT_EQ(back.blocks[1].kind, std::string("markdown"));
    XX_TEST_EXPECT_EQ(back.blocks[2].path, std::string("a.txt"));
    XX_TEST_EXPECT_EQ(back.blocks[4].lines, 3);
    const auto* num = findControl(back, "number");
    XX_TEST_EXPECT_TRUE(num != nullptr);
    if (num) {
        XX_TEST_EXPECT_EQ(num->id, std::string("n"));
        XX_TEST_EXPECT_EQ(num->label, std::string("N"));
        XX_TEST_EXPECT_EQ(num->labelKey, std::string("n.key"));
        XX_TEST_EXPECT_TRUE(num->integer);
        XX_TEST_EXPECT_EQ(num->step, 2.0);
        XX_TEST_EXPECT_TRUE(num->hasMin && num->hasMax);
        XX_TEST_EXPECT_EQ(num->minValue, -1.0);
        XX_TEST_EXPECT_EQ(num->maxValue, 9.0);
    }
    const auto* cb = findControl(back, "checkbox");
    XX_TEST_EXPECT_TRUE(cb != nullptr);
    if (cb) {
        XX_TEST_EXPECT_TRUE(cb->defaultValue.is_boolean() && cb->defaultValue.get<bool>());
        XX_TEST_EXPECT_EQ(cb->help, std::string("help"));
        XX_TEST_EXPECT_EQ(cb->helpKey, std::string("help.key"));
    }
    // 提交行标签
    const auto& submit = back.blocks.back();
    XX_TEST_EXPECT_EQ(submit.kind, std::string("submit"));
    XX_TEST_EXPECT_EQ(submit.label, std::string("Go"));
    XX_TEST_EXPECT_EQ(submit.cancelLabel, std::string("Back"));
    XX_TEST_EXPECT_EQ(submit.cancelLabelKey, std::string("back.key"));

    // 往返稳定 (第二次序列化 == 第一次)
    XX_TEST_EXPECT_EQ(back.toJson().dump(), dumped.dump());
}

void test_ui_unknown_and_custom_fields() {
    // 未知字段忽略、未知 kind 原样保留 (向前兼容; 客户端渲染时忽略/降级)
    const auto json = Json::parse(R"({
        "version": 1,
        "unknownTop": 123,
        "blocks": [
            {"kind": "future_kind", "text": "x", "extra": true},
            {"kind": "custom", "props": {"items": [{"kind": "Text", "text": "a"}]}, "fallback": "fb"}
        ]
    })");
    const auto ui   = InterruptUi::fromJson(json);
    XX_TEST_EXPECT_EQ(ui.blocks.size(), size_t{2});
    XX_TEST_EXPECT_EQ(ui.blocks[0].kind, std::string("future_kind"));
    XX_TEST_EXPECT_EQ(ui.blocks[0].text, std::string("x"));
    XX_TEST_EXPECT_EQ(ui.blocks[1].kind, std::string("custom"));
    XX_TEST_EXPECT_EQ(ui.blocks[1].fallback, std::string("fb"));
    XX_TEST_EXPECT_TRUE(ui.blocks[1].props.is_object());
    XX_TEST_EXPECT_TRUE(ui.blocks[1].props.contains("items"));

    // 空描述 / 非对象输入
    XX_TEST_EXPECT_TRUE(InterruptUi::fromJson(Json{}).empty());
    XX_TEST_EXPECT_TRUE(InterruptUi::fromJson(Json::array()).empty());
    XX_TEST_EXPECT_FALSE(InterruptUi::fromJson(json).empty());
}

// ---------------------------------------------------------------------------
// 预设模板
// ---------------------------------------------------------------------------

void test_preset_input_form() {
    using namespace agentxx::middleware;
    const auto ui = preset::inputForm({
        preset::InputSpec{
                          .label        = "Mode",
                          .depict       = "choose",
                          .type         = "enum",
                          .defaultValue = "b",
                          .enumValues   = {"a", "b"}
        },
        preset::InputSpec{.label = "Count", .type = "int", .defaultValue = "3"},
        preset::InputSpec{.label = "Ratio", .type = "double"},
        preset::InputSpec{.label = "Enable", .type = "bool", .defaultValue = "no"},
        preset::InputSpec{.label = "Note", .type = "string", .defaultValue = "text"},
    });

    // 每种类型映射到对应控件形态 (类型→控件的映射只存在于预设内)
    XX_TEST_EXPECT_EQ(countControls(ui, "select"), size_t{1});
    XX_TEST_EXPECT_EQ(countControls(ui, "number"), size_t{2});
    XX_TEST_EXPECT_EQ(countControls(ui, "buttons"), size_t{1});
    XX_TEST_EXPECT_EQ(countControls(ui, "text"), size_t{1});

    // 标签/说明文本块存在
    size_t hintTexts  = 0;
    size_t labelTexts = 0;
    for (const auto& b : ui.blocks) {
        if (b.kind != "text") {
            continue;
        }
        if (b.color == "hint") {
            ++hintTexts;
        } else if (b.color == "accent") {
            ++labelTexts;
        }
    }
    XX_TEST_EXPECT_EQ(labelTexts, size_t{5});
    XX_TEST_EXPECT_EQ(hintTexts, size_t{1});

    // 控件 id: 多输入项 = value1..value5; 末块为提交行
    const auto* select = findControl(ui, "select");
    XX_TEST_EXPECT_TRUE(select != nullptr);
    if (select) {
        XX_TEST_EXPECT_EQ(select->id, std::string("value1"));
        XX_TEST_EXPECT_EQ(select->options.size(), size_t{2});
        XX_TEST_EXPECT_EQ(select->defaultValue.get<std::string>(), std::string("b"));
    }
    const auto* buttons = findControl(ui, "buttons");
    XX_TEST_EXPECT_TRUE(buttons != nullptr);
    if (buttons) {
        XX_TEST_EXPECT_EQ(buttons->id, std::string("value4"));
        // bool 控件: 是/否一键按钮, 默认 "no" 选中"否"
        XX_TEST_EXPECT_TRUE(buttons->commitOnPick);
        XX_TEST_EXPECT_EQ(buttons->options.size(), size_t{2});
        XX_TEST_EXPECT_EQ(buttons->defaultValue.get<std::string>(), std::string("false"));
        // 固定文案只给 i18n 键 (字面文本由客户端词表提供)
        XX_TEST_EXPECT_EQ(buttons->options[0].labelKey, std::string("interrupt.yes"));
        XX_TEST_EXPECT_EQ(buttons->options[1].labelKey, std::string("interrupt.no"));
        XX_TEST_EXPECT_TRUE(buttons->options[0].label.empty());
        XX_TEST_EXPECT_TRUE(buttons->options[1].label.empty());
    }
    XX_TEST_EXPECT_EQ(ui.blocks.back().kind, std::string("submit"));

    // 单输入项: 控件 id = "value"
    const auto single = preset::inputForm({
        preset::InputSpec{.label = "Q", .type = "string"},
    });
    XX_TEST_EXPECT_EQ(countControls(single, "text"), size_t{1});
    XX_TEST_EXPECT_EQ(findControl(single, "text")->id, std::string("value"));

    // 空输入项: 仅提交行 (仅确认类询问)
    const auto empty = preset::inputForm({});
    XX_TEST_EXPECT_EQ(empty.blocks.size(), size_t{1});
    XX_TEST_EXPECT_EQ(empty.blocks[0].kind, std::string("submit"));
}

void test_preset_permission_and_confirm_card() {
    using namespace agentxx::middleware;

    // 权限卡片: 头行分段 + 目标描述 + 勾选项 + 允许/拒绝按钮
    // - 固定文案只给 i18n 键 (字面文本由客户端词表提供), 故校验键且文本为空
    const auto perm = preset::permissionCard("read_file", "filesystem_read", "/tmp/x");
    XX_TEST_EXPECT_EQ(perm.header.segments.size(), size_t{3});
    XX_TEST_EXPECT_EQ(perm.header.segments[0].labelKey, std::string("interrupt.permissionBadge"));
    XX_TEST_EXPECT_TRUE(perm.header.segments[0].text.empty());
    XX_TEST_EXPECT_EQ(countControls(perm, "checkbox"), size_t{2});
    const auto* remember = findControl(perm, "checkbox");
    XX_TEST_EXPECT_TRUE(remember != nullptr);
    if (remember) {
        XX_TEST_EXPECT_EQ(remember->id, std::string("remember"));
        XX_TEST_EXPECT_EQ(remember->labelKey, std::string("interrupt.remember"));
        XX_TEST_EXPECT_TRUE(remember->label.empty());
    }
    const InterruptUiBlock* fullAuth = nullptr;
    for (const auto& b : perm.blocks) {
        if (b.kind == "control" && b.control == "checkbox" && b.id == "fullAuth") {
            fullAuth = &b;
            break;
        }
    }
    XX_TEST_EXPECT_TRUE(fullAuth != nullptr);
    if (fullAuth) {
        XX_TEST_EXPECT_EQ(fullAuth->id, std::string("fullAuth"));
        XX_TEST_EXPECT_EQ(fullAuth->labelKey, std::string("interrupt.fullAuth"));
        XX_TEST_EXPECT_TRUE(fullAuth->label.empty());
        XX_TEST_EXPECT_FALSE(
            fullAuth->defaultValue.is_boolean() && fullAuth->defaultValue.get<bool>()
        );
    }
    const auto* decision = findControl(perm, "buttons");
    XX_TEST_EXPECT_TRUE(decision != nullptr);
    if (decision) {
        XX_TEST_EXPECT_EQ(decision->id, std::string("decision"));
        XX_TEST_EXPECT_TRUE(decision->commitOnPick);
        XX_TEST_EXPECT_EQ(decision->options.size(), size_t{2});
        XX_TEST_EXPECT_EQ(decision->options[0].value.get<std::string>(), std::string("true"));
        XX_TEST_EXPECT_EQ(decision->options[1].value.get<std::string>(), std::string("false"));
        XX_TEST_EXPECT_EQ(decision->options[0].labelKey, std::string("interrupt.allow"));
        XX_TEST_EXPECT_EQ(decision->options[1].labelKey, std::string("interrupt.deny"));
        XX_TEST_EXPECT_TRUE(decision->options[0].label.empty());
        XX_TEST_EXPECT_TRUE(decision->options[1].label.empty());
        // 默认选中"拒绝" (安全语义; 默认值与候选项同型)
        XX_TEST_EXPECT_EQ(decision->defaultValue.get<std::string>(), std::string("false"));
    }

    // 权限卡片 (目录目标): 紧随目标描述后附加生效范围提示行 (interrupt.rememberDir)
    // 且勾选项 remember/fullAuth 均无 help (提示归属路径本身)
    const auto permDir      = preset::permissionCard("list_dir", "filesystem_read", "/tmp/dir/");
    size_t     dirHintTexts = 0;
    bool       hasDirPrompt = false;
    for (const auto& b : permDir.blocks) {
        if (b.kind == "text" && b.color == "hint") {
            ++dirHintTexts;
            if (b.textKey == "interrupt.rememberDir") {
                hasDirPrompt = true;
                XX_TEST_EXPECT_TRUE(b.text.empty());
            }
        }
    }
    XX_TEST_EXPECT_EQ(dirHintTexts, size_t{2});
    XX_TEST_EXPECT_TRUE(hasDirPrompt);
    const auto* dirRemember = findControl(permDir, "checkbox");
    XX_TEST_EXPECT_TRUE(dirRemember != nullptr);
    if (dirRemember) {
        XX_TEST_EXPECT_TRUE(dirRemember->help.empty());
        XX_TEST_EXPECT_TRUE(dirRemember->helpKey.empty());
    }

    // 确认卡片: 标题/说明 + 是/否按钮 (控件 id 可定制) + 可选勾选项
    preset::ConfirmCardOptions opts;
    opts.title        = "[tool] Repeated call";
    opts.text         = "Allow it to run again?";
    opts.controlId    = "allow";
    opts.defaultValue = false;
    opts.remember     = true;
    const auto card   = preset::confirmCard(opts);
    XX_TEST_EXPECT_EQ(countControls(card, "checkbox"), size_t{1});
    const auto* cardRemember = findControl(card, "checkbox");
    XX_TEST_EXPECT_TRUE(cardRemember != nullptr);
    if (cardRemember) {
        XX_TEST_EXPECT_EQ(cardRemember->labelKey, std::string("interrupt.remember"));
        XX_TEST_EXPECT_TRUE(cardRemember->label.empty());
    }
    const auto* allow = findControl(card, "buttons");
    XX_TEST_EXPECT_TRUE(allow != nullptr);
    if (allow) {
        XX_TEST_EXPECT_EQ(allow->id, std::string("allow"));
        XX_TEST_EXPECT_EQ(allow->defaultValue.get<std::string>(), std::string("false"));
        XX_TEST_EXPECT_TRUE(allow->commitOnPick);
        // 未自定义标签时: 只给默认 i18n 键, 字面文本为空
        XX_TEST_EXPECT_EQ(allow->options[0].labelKey, std::string("interrupt.yes"));
        XX_TEST_EXPECT_EQ(allow->options[1].labelKey, std::string("interrupt.no"));
        XX_TEST_EXPECT_TRUE(allow->options[0].label.empty());
        XX_TEST_EXPECT_TRUE(allow->options[1].label.empty());
    }

    // 自定义字面文本 (无键): 不再补默认键, 按字面文本渲染
    preset::ConfirmCardOptions customOpts;
    customOpts.yesLabel     = "继续";
    customOpts.noLabel      = "停止";
    const auto  customCard  = preset::confirmCard(customOpts);
    const auto* customAllow = findControl(customCard, "buttons");
    XX_TEST_EXPECT_TRUE(customAllow != nullptr);
    if (customAllow) {
        XX_TEST_EXPECT_TRUE(customAllow->options[0].labelKey.empty());
        XX_TEST_EXPECT_TRUE(customAllow->options[1].labelKey.empty());
        XX_TEST_EXPECT_EQ(customAllow->options[0].label, std::string("继续"));
        XX_TEST_EXPECT_EQ(customAllow->options[1].label, std::string("停止"));
    }
    size_t markdowns = 0;
    for (const auto& b : card.blocks) {
        if (b.kind == "markdown") {
            ++markdowns;
        }
    }
    XX_TEST_EXPECT_EQ(markdowns, size_t{1});
}

// ---------------------------------------------------------------------------
// 结果契约与取值 helper
// ---------------------------------------------------------------------------

void test_result_contract_helpers() {
    using agentxx::middleware::interruptValueBool;
    using agentxx::middleware::interruptValueDouble;
    using agentxx::middleware::interruptValueInt;
    using agentxx::middleware::interruptValueString;
    using agentxx::middleware::makeInterruptResult;

    const auto result = makeInterruptResult(Json{
        {"decision", "true"},
        {"remember", false },
        {"count",    3     },
        {"ratio",    2.5   },
    });
    XX_TEST_EXPECT_TRUE(result.is_object());
    XX_TEST_EXPECT_TRUE(result["values"].is_object());

    // 整体结果对象 / 纯 values 对象两种形式均可读取
    XX_TEST_EXPECT_TRUE(interruptValueBool(result, "decision", false));
    XX_TEST_EXPECT_TRUE(interruptValueBool(result["values"], "decision", false));
    XX_TEST_EXPECT_FALSE(interruptValueBool(result, "remember", true));
    XX_TEST_EXPECT_EQ(interruptValueInt(result, "count", 0), int64_t{3});
    XX_TEST_EXPECT_EQ(interruptValueDouble(result["values"], "ratio", 0.0), 2.5);
    XX_TEST_EXPECT_EQ(interruptValueString(result, "decision", ""), std::string("true"));

    // 未命中 / 类型不符: 返回默认值
    XX_TEST_EXPECT_FALSE(interruptValueBool(result, "missing", false));
    XX_TEST_EXPECT_EQ(interruptValueString(result, "missing", "d"), std::string("d"));
    XX_TEST_EXPECT_EQ(
        interruptValueInt(
            Json{
                {"s", "abc"}
    },
            "s",
            -1
        ),
        int64_t{-1}
    );

    // 布尔值容错规则: 字符串 "true"/"yes"/"y"/"1" 与非 0 数值均为 true
    XX_TEST_EXPECT_TRUE(interruptValueBool(
        Json{
            {"a", "yes"}
    },
        "a",
        false
    ));
    XX_TEST_EXPECT_TRUE(interruptValueBool(
        Json{
            {"a", "1"}
    },
        "a",
        false
    ));
    XX_TEST_EXPECT_TRUE(interruptValueBool(
        Json{
            {"a", 2}
    },
        "a",
        false
    ));
    XX_TEST_EXPECT_FALSE(interruptValueBool(
        Json{
            {"a", "no"}
    },
        "a",
        true
    ));

    // 数值字符串可解析为数值
    XX_TEST_EXPECT_EQ(
        interruptValueInt(
            Json{
                {"n", "42"}
    },
            "n",
            0
        ),
        int64_t{42}
    );
    XX_TEST_EXPECT_EQ(
        interruptValueDouble(
            Json{
                {"n", " 1.5 "}
    },
            "n",
            0.0
        ),
        1.5
    );

    // 非对象 values 归一化: 空对象 (未应答语义)
    const auto empty = makeInterruptResult(Json::array());
    XX_TEST_EXPECT_TRUE(empty["values"].is_object());
    XX_TEST_EXPECT_TRUE(empty["values"].empty());
}

// ---------------------------------------------------------------------------
// 纯文本降级 (行式前端/日志)
// ---------------------------------------------------------------------------

void test_plain_text_degrade() {
    using namespace agentxx::middleware;
    InterruptUi ui;
    ui.blocks.push_back(preset::textBlock("first line"));
    ui.blocks.push_back(preset::markdownBlock("# Title\nbody"));
    ui.blocks.push_back(preset::diffBlock("a.txt", "old\n", "new\n"));
    ui.blocks.push_back(preset::separatorBlock());
    ui.blocks.push_back(preset::gapBlock(1));
    ui.blocks.push_back(preset::selectControl(
        "mode",
        {preset::option("fast", "fast"), preset::option("slow", "slow")},
        "Mode",
        {},
        Json("fast")
    ));
    ui.blocks.push_back(preset::submitBlock());
    InterruptUiBlock custom;
    custom.kind     = "custom";
    custom.fallback = "fallback text";
    ui.blocks.push_back(custom);

    const auto text = interruptUiPlainText(ui, 0);
    XX_TEST_EXPECT_TRUE(text.find("first line") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("# Title") != std::string::npos);
    // diff: 路径行 + "- "/"+ " 前缀的增删行 (格式由描述层的纯文本降级决定)
    XX_TEST_EXPECT_TRUE(text.find("a.txt") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("- old") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("+ new") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("---") != std::string::npos);
    // 控件: 标签 + 当前取值 (候选项清单不在纯文本里展开)
    XX_TEST_EXPECT_TRUE(text.find("Mode") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("fast") != std::string::npos);
    // 自定义块: 打印 fallback
    XX_TEST_EXPECT_TRUE(text.find("fallback text") != std::string::npos);
    // submit 行不输出
    XX_TEST_EXPECT_TRUE(text.find("submit") == std::string::npos);

    // 折行: 无空格长文本按宽度切开
    InterruptUi wrapUi;
    wrapUi.blocks.push_back(preset::textBlock("0123456789", {}, 0, true));
    const auto wrapped = interruptUiPlainText(wrapUi, 4);
    XX_TEST_EXPECT_TRUE(wrapped.find("0123") != std::string::npos);
    XX_TEST_EXPECT_TRUE(wrapped.find("4567") != std::string::npos);
    XX_TEST_EXPECT_TRUE(wrapped.find("89") != std::string::npos);
}

/// 扩展组件块 (表格/树/横排/键值/趋势图/计量条等) 的纯文本降级
///
/// 行式前端 (CLI/FFI/日志) 只走 interruptUiPlainText, 若这些块被忽略, 用户会
/// 完全看不到中断描述的内容 —— 本用例保护"按同一 schema 解析并输出"的行为。
void test_plain_text_extended_blocks() {
    using namespace agentxx::middleware;

    InterruptUi ui;
    // 扩展组件块 = 描述层的组件描述 (规范写法: 组件名 PascalCase、字段 camelCase)
    ui.blocks.push_back(InterruptUiBlock::fromJson(Json{
        {"kind", "Table"},
        {"header", true},
        {"columns", Json::array({Json{{"title", "Path"}}, Json{{"title", "Scope"}, {"align", "end"}}})},
        {"rows", Json::array({Json::array({"a.txt", "write"}), Json::array({"b.txt", "read"})})},
    }));
    ui.blocks.push_back(InterruptUiBlock::fromJson(Json{
        {"kind", "KV"},
        {"pairs", Json::array({Json{{"k", "Model"}, {"v", "gpt-x"}}})},
    }));
    ui.blocks.push_back(InterruptUiBlock::fromJson(Json{
        {"kind", "Tree"},
        {"nodes", Json::array({Json{{"label", "src"}, {"children", Json::array({Json{{"label", "main.cpp"}}})}}})},
    }));
    ui.blocks.push_back(InterruptUiBlock::fromJson(Json{
        {"kind", "Progress"},
        {"value", 72},
        {"total", 100},
        {"label", "CPU"},
    }));
    ui.blocks.push_back(InterruptUiBlock::fromJson(Json{
        {"kind", "Sparkline"},
        {"data", Json::array({1, 5, 3})},
    }));
    ui.blocks.push_back(InterruptUiBlock::fromJson(Json{
        {"kind", "Row"},
        {"children", Json::array({Json{{"kind", "Text"}, {"text", "L"}},
                                  Json{{"kind", "Text"}, {"text", "R"}}})},
    }));
    ui.blocks.push_back(InterruptUiBlock::fromJson(Json{
        {"kind", "future_widget"},
        {"fallback", "unsupported widget"},
    }));

    const auto text = interruptUiPlainText(ui, 80);
    XX_TEST_EXPECT_TRUE(text.find("Path") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("a.txt") != std::string::npos);
    // 键值: 键列按最长键补齐, 分隔符默认 " : " (格式由描述层的纯文本降级决定)
    XX_TEST_EXPECT_TRUE(text.find("Model") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("gpt-x") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("main.cpp") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("CPU") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("72%") != std::string::npos);
    // 趋势图用方块字符表示高低
    XX_TEST_EXPECT_TRUE(text.find("▁") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("L | R") != std::string::npos);
    // 未知 kind: 输出 fallback (不静默丢内容)
    XX_TEST_EXPECT_TRUE(text.find("unsupported widget") != std::string::npos);

    // 未知 kind 且无 fallback: 跳过 (向前兼容)
    InterruptUi unknown;
    unknown.blocks.push_back(InterruptUiBlock::fromJson(Json{{"kind", "future_widget"}}));
    XX_TEST_EXPECT_EQ(interruptUiPlainText(unknown, 0), std::string{});

    // 缩进与折行 (宽度按显示列宽算法, 与文本块一致)
    InterruptUi indented;
    indented.blocks.push_back(InterruptUiBlock::fromJson(Json{
        {"kind", "text"},
        {"text", "abcdefgh"},
        {"indent", 2},
    }));
    const auto wrappedText = interruptUiPlainText(indented, 6);
    XX_TEST_EXPECT_TRUE(wrappedText.find("  abcd") != std::string::npos);
    XX_TEST_EXPECT_TRUE(wrappedText.find("  efgh") != std::string::npos);
}

// ---------------------------------------------------------------------------
// InterruptHandleArg 序列化 (ui 唯一描述来源; inputs[] 已删除)
// ---------------------------------------------------------------------------

void test_interrupt_handle_arg_serialization() {
    InterruptHandleArg arg;
    arg.name     = "permission";
    arg.resultId = "call_1";
    arg.arg      = Json{
             {"category", "filesystem_write"},
             {"target",   "/tmp/x"          },
    };
    arg.ui = agentxx::middleware::preset::permissionCard("write", "filesystem_write", "/tmp/x");

    const auto json = arg.toJson();
    XX_TEST_EXPECT_TRUE(json.contains("ui"));
    XX_TEST_EXPECT_FALSE(json.contains("inputs")); // 参数类型声明已彻底删除
    XX_TEST_EXPECT_EQ(json.value("resultId", std::string{}), std::string("call_1"));

    const auto back = InterruptHandleArg::fromJson(json);
    XX_TEST_EXPECT_TRUE(back.has_value());
    if (back) {
        XX_TEST_EXPECT_EQ(back->name, std::string("permission"));
        XX_TEST_EXPECT_EQ(back->resultId, std::string("call_1"));
        XX_TEST_EXPECT_EQ(back->arg.value("target", std::string{}), std::string("/tmp/x"));
        XX_TEST_EXPECT_FALSE(back->ui.empty());
        XX_TEST_EXPECT_EQ(back->ui.header.segments.size(), size_t{3});
    }

    // 非法格式 (缺 name): 解析失败
    XX_TEST_EXPECT_FALSE(InterruptHandleArg::fromJson(Json{
                                                          {"arg", 1}
    }
    ).has_value());
    // 列表往返
    const auto list = InterruptHandleArg::listFromJson(Json::array({json, json}));
    XX_TEST_EXPECT_EQ(list.size(), size_t{2});
    XX_TEST_EXPECT_EQ(InterruptHandleArg::listToJson(list).size(), size_t{2});
}

/// 组件桥接: 构建器 → 中断块 → (TUI/纯文本共用) 组件项
///
/// 保护"中断描述可直接用表格/树/图表等新组件"这条能力: 也保护 itemOf/blockOf
/// 之间的往返不丢内容 (渲染层与纯文本降级都按 raw 走组件层)。
void test_component_bridge() {
    using namespace agentxx::middleware;
    using pluginxx::ui::Item;
    using pluginxx::ui::SizeValue;
    using pluginxx::ui::TableCell;
    using pluginxx::ui::TableColumn;
    using pluginxx::ui::TextValue;

    // 1) 组件项 → 中断块 (块只保留描述层的 JSON 原文, 内容不丢)
    Item table    = pluginxx::ui::build::node("Table");
    table.header  = true;
    table.columns.push_back(TableColumn{TextValue::of("Path"), "start", SizeValue::autoValue(), {}});
    table.columns.push_back(TableColumn{TextValue::of("Scope"), "end", SizeValue::of(6), {}});
    table.rows.push_back({TableCell{TextValue::of("a.txt"), {}, {}}});

    Item progress  = pluginxx::ui::build::progress(72, 100, "%");
    progress.label = TextValue::of("CPU");

    auto blocks = preset::blocksOf({pluginxx::ui::build::title("标题"), table, progress});
    XX_TEST_EXPECT_EQ(blocks.size(), size_t{3});
    // 域的块词汇: 文本块是 "text" (见 blockOf); 其余组件按原始 JSON 带走
    XX_TEST_EXPECT_EQ(blocks[0].kind, std::string{"text"});
    XX_TEST_EXPECT_EQ(blocks[0].text, std::string{"标题"});
    XX_TEST_EXPECT_EQ(blocks[1].kind, std::string{"Table"});
    XX_TEST_EXPECT_EQ(blocks[2].kind, std::string{"Progress"});

    // 2) 块 → 组件项 (唯一映射): 扩展组件按 raw 解析
    const auto tableItem = itemOf(blocks[1]);
    XX_TEST_EXPECT_TRUE(tableItem.has_value());
    if (tableItem) {
        XX_TEST_EXPECT_EQ(tableItem->kind, std::string{"Table"});
        XX_TEST_EXPECT_EQ(tableItem->columns.size(), size_t{2});
        XX_TEST_EXPECT_EQ(tableItem->columns[1].align, std::string{"end"});
        XX_TEST_EXPECT_EQ(tableItem->rows.size(), size_t{1});
        XX_TEST_EXPECT_EQ(tableItem->rows[0][0].text.fallback, std::string{"a.txt"});
    }

    // 3) 端到端: 含扩展组件的中断描述在行式前端 (CLI/日志) 上也有内容
    InterruptUi desc;
    desc.blocks = blocks;
    const auto text = interruptUiPlainText(desc, 80);
    XX_TEST_EXPECT_TRUE(text.find("标题") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("a.txt") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("CPU") != std::string::npos);

    // 4) 文本块缩进: 描述层没有"缩进"字段, 用 Padding 容器表达
    //    - blockOf 会把"纯左缩进"的 Padding 解包回域的 indent (域字段不丢)
    //    - itemOf 再把 indent 还原成 Padding
    const auto indented = preset::textBlock("缩进行", "accent", 2, false, true);
    XX_TEST_EXPECT_EQ(indented.kind, std::string{"text"});
    XX_TEST_EXPECT_EQ(indented.text, std::string{"缩进行"});
    XX_TEST_EXPECT_EQ(indented.color, std::string{"accent"});
    XX_TEST_EXPECT_EQ(indented.indent, 2);
    XX_TEST_EXPECT_TRUE(indented.bold);
    const auto indentedItem = itemOf(indented);
    XX_TEST_EXPECT_TRUE(indentedItem.has_value());
    if (indentedItem) {
        XX_TEST_EXPECT_EQ(indentedItem->kind, std::string{"Padding"});
        XX_TEST_EXPECT_EQ(indentedItem->children.size(), size_t{1});
        XX_TEST_EXPECT_EQ(indentedItem->children[0].kind, std::string{"Text"});
        XX_TEST_EXPECT_EQ(indentedItem->children[0].text.fallback, std::string{"缩进行"});
        XX_TEST_EXPECT_EQ(indentedItem->children[0].tone, std::string{"accent"});
        XX_TEST_EXPECT_TRUE(indentedItem->padding.left > 0.0);
    }

    // 5) 控件: 块 ↔ 组件项往返 (id/形态/标签/步进/整数/缺省值)
    const auto numberBlock = preset::numberControl("n", "N", {}, 3.0, true, 2.0);
    const auto numberItem  = itemOf(numberBlock);
    XX_TEST_EXPECT_TRUE(numberItem.has_value());
    if (numberItem) {
        XX_TEST_EXPECT_EQ(numberItem->kind, std::string{"Control"});
        XX_TEST_EXPECT_EQ(numberItem->control, std::string{"number"});
        XX_TEST_EXPECT_EQ(numberItem->label.fallback, std::string{"N"});
        XX_TEST_EXPECT_EQ(numberItem->step, 2.0);
        XX_TEST_EXPECT_TRUE(numberItem->integer);
        const auto back = blockOf(*numberItem);
        XX_TEST_EXPECT_EQ(back.kind, std::string{"control"});
        XX_TEST_EXPECT_EQ(back.control, std::string{"number"});
        XX_TEST_EXPECT_EQ(back.label, std::string{"N"});
        XX_TEST_EXPECT_EQ(back.step, 2.0);
        XX_TEST_EXPECT_TRUE(back.integer);
        XX_TEST_EXPECT_TRUE(back.hasMin == false && back.hasMax == false);
    }

    // 6) 提交行: 域内映射成"确认 / 取消"两个按钮 (描述层没有表单提交这一层)
    const auto submitRow = itemOf(preset::submitBlock("APPLY", {}, {}, {}));
    XX_TEST_EXPECT_TRUE(submitRow.has_value());
    if (submitRow) {
        XX_TEST_EXPECT_EQ(submitRow->kind, std::string{"Row"});
        XX_TEST_EXPECT_EQ(submitRow->children.size(), size_t{2});
        XX_TEST_EXPECT_EQ(submitRow->children[0].kind, std::string{"Button"});
        XX_TEST_EXPECT_EQ(submitRow->children[1].kind, std::string{"Button"});
        XX_TEST_EXPECT_TRUE(
            submitRow->children[0].action.kind == pluginxx::ui::Action::Kind::Dispatch
        );
        XX_TEST_EXPECT_EQ(submitRow->children[0].action.name, std::string{kInterruptSubmitActionId});
        XX_TEST_EXPECT_EQ(submitRow->children[1].action.name, std::string{kInterruptCancelActionId});
        XX_TEST_EXPECT_EQ(submitRow->children[0].label.fallback, std::string{"APPLY"});
    }

    // 7) 预设 helper: 树 / 表格块
    pluginxx::ui::TreeNode root;
    root.label = TextValue::of("src");
    root.children.push_back(
        pluginxx::ui::TreeNode{TextValue::of("main.cpp"), {}, pluginxx::ui::Action{}, {}}
    );
    const auto tree = preset::treeBlock({root});
    XX_TEST_EXPECT_EQ(tree.kind, std::string{"Tree"});
    const auto treeItem = itemOf(tree);
    XX_TEST_EXPECT_TRUE(treeItem.has_value());
    if (treeItem) {
        XX_TEST_EXPECT_EQ(treeItem->nodes.size(), size_t{1});
        XX_TEST_EXPECT_EQ(treeItem->nodes[0].children.size(), size_t{1});
        XX_TEST_EXPECT_EQ(treeItem->nodes[0].children[0].label.fallback, std::string{"main.cpp"});
    }

    const auto tableBlock = preset::tableBlock({{"File", "left", 0}}, {{"x.cpp"}});
    XX_TEST_EXPECT_EQ(tableBlock.kind, std::string{"Table"});
    const auto tableBlockItem = itemOf(tableBlock);
    XX_TEST_EXPECT_TRUE(tableBlockItem.has_value());
    if (tableBlockItem) {
        XX_TEST_EXPECT_EQ(tableBlockItem->columns[0].align, std::string{"start"});
        XX_TEST_EXPECT_EQ(tableBlockItem->rows[0][0].text.fallback, std::string{"x.cpp"});
    }
}
TestResult testInterruptUi() {
    g_interrupt_ui_passed = 0;
    g_interrupt_ui_failed = 0;

    test_ui_json_roundtrip();
    test_ui_unknown_and_custom_fields();
    test_preset_input_form();
    test_preset_permission_and_confirm_card();
    test_result_contract_helpers();
    test_plain_text_degrade();
    test_plain_text_extended_blocks();
    test_component_bridge();
    test_interrupt_handle_arg_serialization();

    return TestResult{g_interrupt_ui_passed, g_interrupt_ui_failed};
}

} // namespace test
} // namespace agentxx
