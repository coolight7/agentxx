/// 客户端界面能力段测试 (计划 UI-9)
///
/// 覆盖:
/// - 描述层能力字段 (apiVersion / kind / blocks / controls / cell / gap) 与
///   `tuiUiCapabilities()` 同源 (插件读到的能力 = 渲染前适配用的能力)
/// - 体验级别字段: 表单 (多字段提交 / 提交取消 / 点击即提交 / 校验)、
///   布局 (auto / fixed / scroll)、终端能力 (truecolor / mouse / wide_chars / 光标)
/// - 字段只增不改: 描述层解析不认识的键不受影响 (能力段仍是同一份 JSON 对象)
#include "agentxx-test/client/test_ui_capabilities.h"

#include "agentxx-client/io/tui/tui_plugin_adapter.h"
#include "agentxx-client/io/tui/ui_components.h"
#include "pluginxx/ui/capabilities.h"
#include "utilxx_base/json.h"
#include <string>
#include <vector>

namespace {
int g_uc_passed = 0;
int g_uc_failed = 0;
} // namespace

#define XX_TEST_PASSED g_uc_passed
#define XX_TEST_FAILED g_uc_failed

namespace agentxx {
namespace test {

using namespace utilxx_base;

TestResult testUiCapabilities() {
    const int passedBefore = g_uc_passed;
    const int failedBefore = g_uc_failed;

    // 空 weak_ptr: 本用例只读能力段, 不触发任何需要 TUI 实例的回调
    const agentxx::client::TuiPluginAdapter adapter{std::weak_ptr<agentxx::client::TUIClientAgentIO>{}};

    const std::string text = adapter.uiCapabilitiesJson();
    XX_TEST_EXPECT_FALSE(text.empty());

    auto json = Json::parse(text);
    XX_TEST_EXPECT_TRUE(json.is_object());

    // ---- 描述层字段与 tuiUiCapabilities() 同源 ----
    const auto& caps = agentxx::client::tuiUiCapabilities();
    XX_TEST_EXPECT_EQ(json.value("apiVersion", 0), caps.apiVersion);
    XX_TEST_EXPECT_EQ(json.value("kind", std::string{}), caps.kind);
    XX_TEST_EXPECT_EQ(json["blocks"].size(), caps.blocks.size());
    XX_TEST_EXPECT_EQ(json["controls"].size(), caps.controls.size());
    XX_TEST_EXPECT_TRUE(json.contains("cell") && json.contains("gap"));
    // 终端换算口径一致: 固定 8u 换算出的列数与能力段自身算法一致
    XX_TEST_EXPECT_EQ(caps.colsOf(8.0, true), static_cast<int>(8.0 / caps.cell.width + 0.5));

    // ---- 体验级别字段 (UI-9) ----
    XX_TEST_EXPECT_TRUE(json.contains("form"));
    XX_TEST_EXPECT_TRUE(json["form"].value("multi_field", false));
    XX_TEST_EXPECT_TRUE(json["form"].value("submit_cancel", false));
    XX_TEST_EXPECT_TRUE(json["form"].value("commit_on_pick", false));
    XX_TEST_EXPECT_TRUE(json["form"].value("validation", false));

    XX_TEST_EXPECT_TRUE(json.contains("layout"));
    XX_TEST_EXPECT_TRUE(json["layout"].value("auto", false));
    XX_TEST_EXPECT_TRUE(json["layout"].value("fixed", false));
    XX_TEST_EXPECT_TRUE(json["layout"].value("scroll", false));

    XX_TEST_EXPECT_TRUE(json.contains("terminal"));
    XX_TEST_EXPECT_TRUE(json["terminal"].value("truecolor", false));
    XX_TEST_EXPECT_TRUE(json["terminal"].value("mouse", false));
    XX_TEST_EXPECT_TRUE(json["terminal"].value("wide_chars", false));
    // 硬件光标尚未实现 (UI-5 待实施): 如实上报 false, 不虚报能力
    // 输入栏硬件光标 (计划 UI-5): 取值来自 tuiHardwareCursorSupported() —— 终端
    // 支持时请求系统光标定位 (输入法候选框跟随), 不支持时回退到自绘光标格。
    // 这里断言能力段与该唯一来源一致 (不允许各写一份常量)。
    XX_TEST_EXPECT_EQ(
        json["terminal"].value("hardware_cursor", false),
        agentxx::client::tuiHardwareCursorSupported()
    );

    // ---- 字段只增不改: 能力段仍能被描述层解析为同一份能力 ----
    const auto parsed = pluginxx::ui::capabilitiesFromJson(json);
    XX_TEST_EXPECT_EQ(parsed.apiVersion, caps.apiVersion);
    XX_TEST_EXPECT_EQ(parsed.blocks.size(), caps.blocks.size());
    XX_TEST_EXPECT_TRUE(parsed.supportsBlock("Text"));
    XX_TEST_EXPECT_EQ(parsed.cell.width, caps.cell.width);

    return TestResult{g_uc_passed - passedBefore, g_uc_failed - failedBefore};
}

} // namespace test
} // namespace agentxx
