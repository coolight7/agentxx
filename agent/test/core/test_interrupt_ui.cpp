#include "test_interrupt_ui.h"

#include "agentxx/middlewares/interrupt_presets.h"
#include "agentxx/middlewares/interrupt_ui.h"
#include "agentxx/middlewares/middleware.h"

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
using agentxx::util::Json;

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
    ui.blocks.push_back(agentxx::middleware::preset::textBlock("hello", "hint", 2, true, false, true));
    ui.blocks.push_back(agentxx::middleware::preset::markdownBlock("**md**", 1));
    ui.blocks.push_back(agentxx::middleware::preset::diffBlock("a.txt", "old", "new"));
    ui.blocks.push_back(agentxx::middleware::preset::separatorBlock(1));
    ui.blocks.push_back(agentxx::middleware::preset::gapBlock(3));
    auto number = agentxx::middleware::preset::numberControl("n", "N", "n.key", 2.0, true, 2.0);
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
    ui.blocks.push_back(agentxx::middleware::preset::submitBlock("Go", "go.key", "Back", "back.key"));

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
            {"kind": "custom", "component": "comp", "props": {"a": 1}, "fallback": "fb"}
        ]
    })");
    const auto ui = InterruptUi::fromJson(json);
    XX_TEST_EXPECT_EQ(ui.blocks.size(), size_t{2});
    XX_TEST_EXPECT_EQ(ui.blocks[0].kind, std::string("future_kind"));
    XX_TEST_EXPECT_EQ(ui.blocks[0].text, std::string("x"));
    XX_TEST_EXPECT_EQ(ui.blocks[1].kind, std::string("custom"));
    XX_TEST_EXPECT_EQ(ui.blocks[1].component, std::string("comp"));
    XX_TEST_EXPECT_EQ(ui.blocks[1].fallback, std::string("fb"));
    XX_TEST_EXPECT_TRUE(ui.blocks[1].props.is_object());
    XX_TEST_EXPECT_EQ(ui.blocks[1].props.value("a", 0), 1);

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
                           .label = "Mode", .depict = "choose", .type = "enum", .defaultValue = "b", .enumValues = {"a", "b"}
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
    size_t hintTexts = 0;
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
    const auto perm = preset::permissionCard("read_file", "filesystem_read", "/tmp/x");
    XX_TEST_EXPECT_EQ(perm.header.segments.size(), size_t{3});
    XX_TEST_EXPECT_EQ(countControls(perm, "checkbox"), size_t{2});
    const auto* remember = findControl(perm, "checkbox");
    XX_TEST_EXPECT_TRUE(remember != nullptr);
    if (remember) {
        XX_TEST_EXPECT_EQ(remember->id, std::string("remember"));
        XX_TEST_EXPECT_EQ(remember->labelKey, std::string("interrupt.remember"));
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
        XX_TEST_EXPECT_FALSE(fullAuth->defaultValue.is_boolean() && fullAuth->defaultValue.get<bool>());
    }
    const auto* decision = findControl(perm, "buttons");
    XX_TEST_EXPECT_TRUE(decision != nullptr);
    if (decision) {
        XX_TEST_EXPECT_EQ(decision->id, std::string("decision"));
        XX_TEST_EXPECT_TRUE(decision->commitOnPick);
        XX_TEST_EXPECT_EQ(decision->options.size(), size_t{2});
        XX_TEST_EXPECT_EQ(decision->options[0].value.get<std::string>(), std::string("true"));
        XX_TEST_EXPECT_EQ(decision->options[1].value.get<std::string>(), std::string("false"));
        // 默认选中"拒绝" (安全语义; 默认值与候选项同型)
        XX_TEST_EXPECT_EQ(decision->defaultValue.get<std::string>(), std::string("false"));
    }

    // 确认卡片: 标题/说明 + 是/否按钮 (控件 id 可定制) + 可选勾选项
    preset::ConfirmCardOptions opts;
    opts.title       = "[tool] Repeated call";
    opts.text        = "Allow it to run again?";
    opts.controlId   = "allow";
    opts.defaultValue = false;
    opts.remember    = true;
    const auto card  = preset::confirmCard(opts);
    XX_TEST_EXPECT_EQ(countControls(card, "checkbox"), size_t{1});
    const auto* allow = findControl(card, "buttons");
    XX_TEST_EXPECT_TRUE(allow != nullptr);
    if (allow) {
        XX_TEST_EXPECT_EQ(allow->id, std::string("allow"));
        XX_TEST_EXPECT_EQ(allow->defaultValue.get<std::string>(), std::string("false"));
        XX_TEST_EXPECT_TRUE(allow->commitOnPick);
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
        {"remember", false},
        {"count",   3      },
        {"ratio",   2.5    },
    });
    XX_TEST_EXPECT_TRUE(result.is_object());
    XX_TEST_EXPECT_TRUE(result["values"].is_object());

    // 整体结果对象 / 纯 values 对象两种口径均可读取
    XX_TEST_EXPECT_TRUE(interruptValueBool(result, "decision", false));
    XX_TEST_EXPECT_TRUE(interruptValueBool(result["values"], "decision", false));
    XX_TEST_EXPECT_FALSE(interruptValueBool(result, "remember", true));
    XX_TEST_EXPECT_EQ(interruptValueInt(result, "count", 0), int64_t{3});
    XX_TEST_EXPECT_EQ(interruptValueDouble(result["values"], "ratio", 0.0), 2.5);
    XX_TEST_EXPECT_EQ(interruptValueString(result, "decision", ""), std::string("true"));

    // 未命中 / 类型不符: 返回默认值
    XX_TEST_EXPECT_FALSE(interruptValueBool(result, "missing", false));
    XX_TEST_EXPECT_EQ(interruptValueString(result, "missing", "d"), std::string("d"));
    XX_TEST_EXPECT_EQ(interruptValueInt(Json{{"s", "abc"}}, "s", -1), int64_t{-1});

    // 布尔容错口径: 字符串 "true"/"yes"/"y"/"1" 与非 0 数值均为 true
    XX_TEST_EXPECT_TRUE(interruptValueBool(Json{{"a", "yes"}}, "a", false));
    XX_TEST_EXPECT_TRUE(interruptValueBool(Json{{"a", "1"}}, "a", false));
    XX_TEST_EXPECT_TRUE(interruptValueBool(Json{{"a", 2}}, "a", false));
    XX_TEST_EXPECT_FALSE(interruptValueBool(Json{{"a", "no"}}, "a", true));

    // 数值字符串可解析为数值
    XX_TEST_EXPECT_EQ(interruptValueInt(Json{{"n", "42"}}, "n", 0), int64_t{42});
    XX_TEST_EXPECT_EQ(interruptValueDouble(Json{{"n", " 1.5 "}}, "n", 0.0), 1.5);

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
    custom.component = "comp";
    custom.fallback = "fallback text";
    ui.blocks.push_back(custom);

    const auto text = interruptUiPlainText(ui, 0);
    XX_TEST_EXPECT_TRUE(text.find("first line") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("# Title") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("file: a.txt") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("-old") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("+new") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("---") != std::string::npos);
    // 控件: 标签 + 候选 + 形态
    XX_TEST_EXPECT_TRUE(text.find("Mode: fast / slow (select)") != std::string::npos);
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

// ---------------------------------------------------------------------------
// InterruptHandleArg 序列化 (ui 唯一描述来源; inputs[] 已删除)
// ---------------------------------------------------------------------------

void test_interrupt_handle_arg_serialization() {
    InterruptHandleArg arg;
    arg.name     = "permission";
    arg.resultId = "call_1";
    arg.arg      = Json{
        {"category", "filesystem_write"},
        {"target",   "/tmp/x"           },
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
    XX_TEST_EXPECT_FALSE(InterruptHandleArg::fromJson(Json{{"arg", 1}}).has_value());
    // 列表往返
    const auto list = InterruptHandleArg::listFromJson(Json::array({json, json}));
    XX_TEST_EXPECT_EQ(list.size(), size_t{2});
    XX_TEST_EXPECT_EQ(InterruptHandleArg::listToJson(list).size(), size_t{2});
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
    test_interrupt_handle_arg_serialization();

    return TestResult{g_interrupt_ui_passed, g_interrupt_ui_failed};
}

} // namespace test
} // namespace agentxx
