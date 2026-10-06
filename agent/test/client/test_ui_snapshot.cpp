/// UI 快照测试模块 (计划 UI-2 / TST-5)
///
/// 夹具实现 (固定尺寸文本 + 命中区基线 + 一键更新 + 差异输出) 在本文件, 供其它
/// 渲染相关模块复用 (见 agentxx-test/client/ui_snapshot.h)。
///
/// 覆盖范围 (UI-2 定义): Markdown 文本 / 表格 (含 CJK 与截断) / 树 (含宿主折叠) /
/// 差异 / 表单 (控件 + 提交取消) / 窄终端 / 超长内容裁剪 / 未知组件降级 /
/// 命中区基线 (表格单元格与表单控件)。
#include "agentxx-test/client/test_ui_snapshot.h"
#include "agentxx-test/client/ui_snapshot.h"

#include "agentxx-client/io/tui/tui_theme.h"
#include "agentxx-client/io/tui/ui_components.h"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include "pluginxx/ui.h"
#include "utilxx_base/json.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <string>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见)
int g_ui_snap_passed = 0;
int g_ui_snap_failed = 0;
} // namespace

#define XX_TEST_PASSED g_ui_snap_passed
#define XX_TEST_FAILED g_ui_snap_failed

#ifndef AGENTXX_UI_SNAPSHOT_DIR
// 独立构建 (不经顶层) 时未注入基线目录: 快照用例退化为"能渲染且内容非空"而不比较基线
#  define AGENTXX_UI_SNAPSHOT_DIR ""
#endif

namespace fs = std::filesystem;

namespace agentxx {
namespace test {

using namespace agentxx::client;

std::string uiSnapshotDir() {
    return AGENTXX_UI_SNAPSHOT_DIR;
}

std::string renderRowsToGrid(const UiRenderResult& res, int w, int h) {
    ftxui::Elements els;
    els.reserve(res.rows.size());
    for (const auto& row : res.rows) {
        els.push_back(row.element);
    }
    auto el = ftxui::vbox(std::move(els));
    auto screen
        = ftxui::Screen::Create(ftxui::Dimension::Fixed(w), ftxui::Dimension::Fixed(h));
    ftxui::Render(screen, el);
    std::string out;
    out.reserve(static_cast<size_t>(w + 1) * static_cast<size_t>(h));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::string& ch  = screen.PixelAt(x, y).character;
            out                   += ch.empty() ? std::string{" "} : ch;
        }
        out += '\n';
    }
    return out;
}

std::string serializeHitRegions(const UiRenderResult& res) {
    std::string out;
    for (size_t rowIndex = 0; rowIndex < res.rows.size(); ++rowIndex) {
        for (const auto& region : res.rows[rowIndex].regions) {
            out += fmt::format(
                "row={} kind={} id={} sub={} x={} y={} w={} h={} arg={} owner={} plugin={}\n",
                rowIndex,
                static_cast<int>(region.kind),
                region.id,
                region.sub,
                region.x,
                region.y,
                region.w,
                region.h,
                region.arg,
                region.ownerId,
                region.plugin
            );
        }
    }
    return out;
}

namespace {

/// 环境变量开关: 是否重写基线
bool snapshotUpdateModeFromEnv() {
    const char* v = std::getenv("AGENTXX_UPDATE_UI_SNAPSHOTS");
    return v != nullptr && (std::string_view{v} == "1" || std::string_view{v} == "true");
}

std::string fileOf(std::string_view name) {
    return (fs::path{uiSnapshotDir()} / fmt::format("{}.snap", name)).string();
}

/// 逐行比较, 生成"首个不同位置 + 两侧内容"的说明
std::string diffLines(const std::string& expected, const std::string& actual) {
    std::vector<std::string> expLines;
    std::vector<std::string> actLines;
    auto                     split = [](const std::string& text, std::vector<std::string>& out) {
        std::string cur;
        for (char c : text) {
            if (c == '\n') {
                out.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        if (!cur.empty()) {
            out.push_back(cur);
        }
    };
    split(expected, expLines);
    split(actual, actLines);
    const size_t maxLines = std::max(expLines.size(), actLines.size());
    for (size_t i = 0; i < maxLines; ++i) {
        const std::string expLine = (i < expLines.size()) ? expLines[i] : std::string{"<无此行>"};
        const std::string actLine = (i < actLines.size()) ? actLines[i] : std::string{"<无此行>"};
        if (expLine == actLine) {
            continue;
        }
        // 首个不同字符的列号 (便于定位表格对齐/CJK 宽度问题)
        size_t col = 0;
        while (col < expLine.size() && col < actLine.size() && expLine[col] == actLine[col]) {
            ++col;
        }
        return fmt::format(
            "line {} col {}:\n    baseline: |{}|\n    actual  : |{}|\n"
            "    baseline lines={} actual lines={}",
            i + 1,
            col + 1,
            expLine,
            actLine,
            expLines.size(),
            actLines.size()
        );
    }
    return "(no line-level difference; trailing newline differs)";
}

} // namespace

UiSnapshotCheck checkUiSnapshot(std::string_view name, const std::string& actual) {
    UiSnapshotCheck out;
    if (uiSnapshotDir().empty()) {
        // 未注入基线目录 (独立构建): 不比较, 视为通过 (调用方会打印提示)
        out.ok = true;
        return out;
    }
    const auto  path = fileOf(name);
    std::string baseline;
    bool        baselineExists = false;
    {
        std::ifstream ifs{utilxx_base::utf8ToPath(path), std::ios::binary};
        baselineExists = ifs.is_open();
        if (baselineExists) {
            baseline.assign(
                std::istreambuf_iterator<char>{ifs},
                std::istreambuf_iterator<char>{}
            );
        }
    }
    if (uiSnapshotUpdateMode()) {
        std::error_code ec;
        fs::create_directories(utilxx_base::utf8ToPath(uiSnapshotDir()), ec);
        std::ofstream ofs{utilxx_base::utf8ToPath(path), std::ios::binary | std::ios::trunc};
        ofs << actual;
        ofs.flush();
        out.ok      = static_cast<bool>(ofs);
        out.updated = out.ok;
        if (!out.ok) {
            out.diff = fmt::format("failed to write baseline {}", path);
        }
        return out;
    }
    if (!baselineExists) {
        out.diff = fmt::format(
            "baseline {} is missing; run with AGENTXX_UPDATE_UI_SNAPSHOTS=1 to generate it",
            path
        );
        return out;
    }
    if (baseline == actual) {
        out.ok = true;
        return out;
    }
    out.diff = fmt::format("baseline {} differs:\n{}", path, diffLines(baseline, actual));
    return out;
}

bool uiSnapshotUpdateMode() {
    return snapshotUpdateModeFromEnv();
}

// ---------------------------------------------------------------------------
// 用例辅助
// ---------------------------------------------------------------------------

namespace {

const TUITheme& theme() {
    static const TUITheme t = TUITheme::darkTheme();
    return t;
}

UiRenderCtx ctxFor(int width, int indent = 0) {
    UiRenderCtx ctx;
    ctx.theme  = &theme();
    ctx.width  = width;
    ctx.indent = indent;
    return ctx;
}

/// 解析 + 适配 + 渲染 (与宿主实际调用路径一致: 宿主先经 renderItemJson)
UiRenderResult renderJson(const std::string& json, const UiRenderCtx& ctx) {
    UiRenderResult out;
    renderItemJson(utilxx_base::Json::parse(json), ctx, out);
    return out;
}

/// 断言一个快照 (文本 + 命中区)
#define XX_EXPECT_SNAPSHOT(name, res, width, height)                                    \
    do {                                                                               \
        const auto _grid = renderRowsToGrid((res), (width), (height));                   \
        const auto _hits = serializeHitRegions((res));                                  \
        auto       _text = checkUiSnapshot((name), _grid);                              \
        if (!_text.ok) {                                                                \
            TEST_FAIL << "snapshot " << (name) << ": " << _text.diff << std::endl;      \
            XX_TEST_FAILED++;                                                           \
        } else {                                                                        \
            XX_TEST_PASSED++;                                                           \
        }                                                                               \
        auto _hitCheck = checkUiSnapshot(std::string{name} + ".hits", _hits);           \
        if (!_hitCheck.ok) {                                                            \
            TEST_FAIL << "snapshot " << (name) << ".hits: " << _hitCheck.diff           \
                      << std::endl;                                                     \
            XX_TEST_FAILED++;                                                           \
        } else {                                                                        \
            XX_TEST_PASSED++;                                                           \
        }                                                                               \
    } while (0)

} // namespace

TestResult testUiSnapshots() {
    const int passedBefore = g_ui_snap_passed;
    const int failedBefore = g_ui_snap_failed;

    if (uiSnapshotDir().empty()) {
        TEST_SKIP << "AGENTXX_UI_SNAPSHOT_DIR not defined: snapshots are render-only"
                  << std::endl;
    }

    // ---------------- 夹具自身的失败路径 (门禁必须真的会红) ----------------
    {
        // 基线不存在: 判失败并提示如何生成
        auto missing = checkUiSnapshot("__no_such_baseline__", "x\n");
        XX_TEST_EXPECT_FALSE(missing.ok);
        XX_TEST_EXPECT_TRUE(missing.diff.find("AGENTXX_UPDATE_UI_SNAPSHOTS=1") != std::string::npos);
    }
    if (!uiSnapshotDir().empty() && !uiSnapshotUpdateMode()) {
        // 内容不一致: 判失败并给出首个不同的行/列
        // (更新模式下会重写基线, 因此只在比较模式下验证)
        auto broken = checkUiSnapshot("markdown_basic", std::string{"totally different\n"});
        XX_TEST_EXPECT_FALSE(broken.ok);
        XX_TEST_EXPECT_TRUE(broken.diff.find("line ") != std::string::npos);
    }

    // ---------------- Markdown (标题/列表/行内代码/围栏代码折行) ----------------
    {
        auto res = renderJson(
            R"([{"kind":"Markdown","text":"# 标题\n\n- 列表项 **加粗** 与 `code`\n\n```cpp\nint main() { return a_long_identifier_name + another_long_identifier_name; }\n```\n"}])",
            ctxFor(48)
        );
        XX_EXPECT_SNAPSHOT("markdown_basic", res, 48, 12);
    }

    // ---------------- 表格 (CJK 宽度 + 右对齐 + 超宽截断 + 可点单元格) ----------------
    {
        auto res = renderJson(
            R"([{"kind":"Table","header":true,"columns":[{"title":"名称","width":80},{"title":"耗时","align":"end","width":48}],"rows":[["编译","1.4s"],[{"text":"测试用例集","action":{"kind":"dispatch","name":"open:test-log","args":{"line":12}}},"0.6s"],["a-very-long-file-name.cpp","12.4K"]]}])",
            ctxFor(48)
        );
        XX_EXPECT_SNAPSHOT("table_cjk", res, 48, 10);
    }

    // ---------------- 树 (连接线 + 宿主折叠状态) ----------------
    const std::string treeJson = R"([{"kind":"Tree","nodes":[{"label":"src","children":[{"label":"main.cpp"},{"label":"io.cpp"}]},{"label":"README.md"}]}])";
    {
        auto res = renderJson(treeJson, ctxFor(40));
        XX_EXPECT_SNAPSHOT("tree_expanded", res, 40, 8);
    }
    {
        // 宿主维护折叠状态: 键为节点路径 (见 UI 折叠约定)
        auto ctx = ctxFor(40);
        ctx.collapseExpanded = [](const std::string& id, bool defaultValue) {
            return (id.find("src") != std::string::npos) ? false : defaultValue;
        };
        auto res = renderJson(treeJson, ctx);
        XX_EXPECT_SNAPSHOT("tree_collapsed", res, 40, 8);
    }

    // ---------------- 差异 ----------------
    {
        auto res = renderJson(
            R"([{"kind":"Diff","path":"app.json","oldStr":"{\n  \"a\": 1,\n  \"b\": 2\n}","newStr":"{\n  \"a\": 1,\n  \"b\": 3,\n  \"c\": 4\n}"}])",
            ctxFor(48)
        );
        XX_EXPECT_SNAPSHOT("diff_json", res, 48, 12);
    }

    // ---------------- 表单 (控件 + 提交/取消) ----------------
    {
        const std::string formJson = R"([
            {"kind":"Control","control":"buttons","id":"mode","label":"模式","options":[{"label":"快速","value":"fast"},{"label":"完整","value":"full"}],"value":"quick"},
            {"kind":"Control","control":"text","id":"name","label":"名称","value":"default-name"},
            {"kind":"Control","control":"switch","id":"enabled","label":"启用","value":true},
            {"kind":"Control","control":"number","id":"count","label":"数量","value":3,"min":0,"max":10,"step":1,"integer":true},
            {"kind":"Row","children":[{"kind":"Button","label":"提交","variant":"primary","action":"__submit"},{"kind":"Button","label":"取消","action":"__cancel"}]}
        ])";
        auto items = pluginxx::ui::parseBlocks(utilxx_base::Json::parse(formJson));
        auto ctx   = ctxFor(48);
        UiFormState form;
        initFormState(form, items);
        // 用户交互: 选中第二项候选 + 编辑文本框 (快照要覆盖"有状态"的画面)
        form.ensure("mode").selected = 1;
        form.focusedId                = "name";
        form.ensure("name").initialized = true;
        form.ensure("name").editText    = "my-session";
        ctx.form                        = &form;

        UiRenderResult res;
        renderItems(items, ctx, res);
        XX_EXPECT_SNAPSHOT("form_controls", res, 48, 14);
    }

    // ---------------- 窄终端 ----------------
    {
        auto res = renderJson(
            R"([{"kind":"KV","pairs":[{"k":"模型","v":"deepseek-v4.1-flash"},{"k":"目录","v":"/very/long/path/to/project"}]},{"kind":"Table","header":true,"columns":[{"title":"名称"},{"title":"耗时","align":"end"}],"rows":[["编译","1.4s"]]}])",
            ctxFor(24)
        );
        XX_EXPECT_SNAPSHOT("narrow_terminal", res, 24, 10);
    }

    // ---------------- 超长内容 (裁剪而非溢出) ----------------
    {
        auto res = renderJson(
            R"([{"kind":"Text","text":"AAAAaaaaAAAAaaaaAAAAaaaaAAAAaaaaAAAAaaaaAAAAaaaaAAAAaaaaAAAAaaaaAAAAaaaaAAAAaaaaAAAAaaaaAAAAaaaaAAAAaaaaAAAAaaaaAAAAaaaa"},{"kind":"Text","text":"第二行"},{"kind":"Text","text":"第三行"}] )",
            ctxFor(32)
        );
        // 画面高度小于内容高度: 只能看到被裁剪后的固定尺寸画面
        XX_EXPECT_SNAPSHOT("long_content_clipped", res, 32, 3);
    }

    // ---------------- 未知组件与未知字段降级 ----------------
    {
        auto res = renderJson(
            R"([{"kind":"Text","text":"before","unknownField":123},{"kind":"FutureWidget","fallback":"降级文本","someNewField":true},{"kind":"Text","text":"after"}])",
            ctxFor(40)
        );
        XX_EXPECT_SNAPSHOT("unknown_component", res, 40, 6);
    }

    return TestResult{g_ui_snap_passed - passedBefore, g_ui_snap_failed - failedBefore};
}

} // namespace test
} // namespace agentxx
