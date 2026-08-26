#include "test_mermaid_state.h"

#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx-client/io/tui/framework/tui_state.h"
#include "agentxx-client/io/tui/tui_theme.h"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include "ftxui/screen/terminal.hpp"
#include <markdown/state_diagram.hpp>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_mermaid_state_passed = 0;
int g_mermaid_state_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_mermaid_state_passed
#define XX_TEST_FAILED g_mermaid_state_failed
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace test {

namespace {

using markdown::MermaidStateDiagram;
using markdown::parseMermaidStateDiagram;
using markdown::renderMermaidStateDiagram;

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

/// 渲染到固定画布并取回文本
std::string renderToString(const MermaidStateDiagram& dg, int maxWidth = 0) {
    auto el     = renderMermaidStateDiagram(dg, maxWidth);
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(160), ftxui::Dimension::Fixed(160));
    ftxui::Render(screen, el);
    return screen.ToString();
}

size_t countOccurrences(std::string_view text, std::string_view needle) {
    size_t count = 0, pos = 0;
    while ((pos = text.find(needle, pos)) != std::string_view::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

// ---------------------------------------------------------------------------
// 解析测试
// ---------------------------------------------------------------------------

void test_parse_linear_chain() {
    auto dg = parseMermaidStateDiagram(R"(
stateDiagram-v2
    [*] --> 1_reproduce_bug
    1_reproduce_bug --> 1_in_progress: start
    1_in_progress --> 1_completed: reproduced
    1_completed --> [*]
)");
    XX_TEST_EXPECT_EQ(dg.nodes.size(), size_t{5}); // start/end 伪状态 + 3
    XX_TEST_EXPECT_EQ(dg.edges.size(), size_t{4});
    // 起始/结束伪状态
    int pseudoCount = 0;
    for (const auto& n : dg.nodes) {
        if (n.isPseudo) {
            ++pseudoCount;
        }
    }
    XX_TEST_EXPECT_EQ(pseudoCount, 2);
    // 边标签
    bool hasLabel = false;
    for (const auto& e : dg.edges) {
        if (e.label == "start") {
            hasLabel = true;
        }
    }
    XX_TEST_EXPECT_TRUE(hasLabel);
}

void test_parse_state_declarations() {
    auto dg = parseMermaidStateDiagram(R"(
stateDiagram
    state "Parse the roadmap" as phase_1_parse
    state idle
    state "standalone label"
    A --> B
)");
    // "Parse the roadmap" / idle / "standalone label" / A / B
    XX_TEST_EXPECT_EQ(dg.nodes.size(), size_t{5});
    bool hasLabel = false;
    for (const auto& n : dg.nodes) {
        if (n.id == "phase_1_parse" && n.label == "Parse the roadmap") {
            hasLabel = true;
        }
        if (n.id == "standalone label") {
            hasLabel = hasLabel && n.label == "standalone label";
        }
    }
    XX_TEST_EXPECT_TRUE(hasLabel);
}

void test_parse_direction() {
    auto dg = parseMermaidStateDiagram("direction LR\nA --> B\n");
    XX_TEST_EXPECT_TRUE(dg.directionLR);
    auto dg2 = parseMermaidStateDiagram("direction TB\nA --> B\n");
    XX_TEST_EXPECT_FALSE(dg2.directionLR);
    auto dg3 = parseMermaidStateDiagram("A --> B\n");
    XX_TEST_EXPECT_FALSE(dg3.directionLR);
}

void test_parse_tolerant() {
    // 未知语法/注释/空行不崩溃, 已知行正常解析
    auto dg = parseMermaidStateDiagram(R"(
%% comment
stateDiagram-v2
note right of A
  some note
end note
A --> B: with : colon in label
B --> C
unknown garbage line
state C {
    state inner
}
)");
    XX_TEST_EXPECT_EQ(dg.nodes.size(), size_t{3});
    XX_TEST_EXPECT_EQ(dg.edges.size(), size_t{2});
    bool hasColonLabel = false;
    for (const auto& e : dg.edges) {
        if (e.label == "with : colon in label") {
            hasColonLabel = true;
        }
    }
    XX_TEST_EXPECT_TRUE(hasColonLabel);
}

void test_parse_arrow_no_spaces() {
    auto dg = parseMermaidStateDiagram("A-->B\n");
    XX_TEST_EXPECT_EQ(dg.nodes.size(), size_t{2});
    XX_TEST_EXPECT_EQ(dg.edges.size(), size_t{1});
}

void test_parse_empty() {
    auto dg = parseMermaidStateDiagram("");
    XX_TEST_EXPECT_TRUE(dg.nodes.empty());
    XX_TEST_EXPECT_TRUE(dg.edges.empty());
}

// ---------------------------------------------------------------------------
// 渲染测试
// ---------------------------------------------------------------------------

void test_render_linear_chain() {
    auto dg  = parseMermaidStateDiagram(R"(
stateDiagram-v2
    [*] --> 1_reproduce_bug
    1_reproduce_bug --> 1_in_progress: start
    1_in_progress --> 1_completed: reproduced
    1_completed --> [*]
)");
    auto out = renderToString(dg);
    XX_TEST_EXPECT_TRUE(contains(out, "[*]"));
    XX_TEST_EXPECT_TRUE(contains(out, "1_reproduce_bug"));
    XX_TEST_EXPECT_TRUE(contains(out, "1_in_progress"));
    XX_TEST_EXPECT_TRUE(contains(out, "1_completed"));
    XX_TEST_EXPECT_TRUE(contains(out, "v"));     // 箭头
    XX_TEST_EXPECT_TRUE(contains(out, "start")); // 边标签
    XX_TEST_EXPECT_TRUE(contains(out, "reproduced"));
    // 框线
    XX_TEST_EXPECT_TRUE(contains(out, "┌"));
    XX_TEST_EXPECT_TRUE(contains(out, "└"));
}

void test_render_fork() {
    // 1 个宽源分叉到左右 2 个目标: 分叉 junction '┴'
    auto dg  = parseMermaidStateDiagram(R"(
stateDiagram-v2
    [*] --> A
    state "center node" as A
    A --> B
    A --> C
    state "right side node" as C
    B --> [*]
    C --> [*]
)");
    auto out = renderToString(dg);
    XX_TEST_EXPECT_TRUE(contains(out, "┴")); // 分叉
    XX_TEST_EXPECT_TRUE(contains(out, "center node"));
    XX_TEST_EXPECT_TRUE(contains(out, "B")); // 未带标签, 显示 id
    XX_TEST_EXPECT_TRUE(contains(out, "right side node"));
    // 至少 3 个箭头 (每带至少 1 个; 同一列汇入的箭头会合并显示)
    XX_TEST_EXPECT_TRUE(countOccurrences(out, "v") >= 3);
}

void test_render_merge() {
    // 2 个源 (左右各一) 汇合到居中的 1 个目标: 汇合 junction '┬'
    auto dg  = parseMermaidStateDiagram(R"(
stateDiagram-v2
    [*] --> A
    [*] --> B
    A --> C
    B --> C
    C --> [*]
    state "left source" as A
    state "right source" as B
    state "center target node" as C
)");
    auto out = renderToString(dg);
    XX_TEST_EXPECT_TRUE(contains(out, "┬")); // 汇合
    XX_TEST_EXPECT_TRUE(contains(out, "left source"));
    XX_TEST_EXPECT_TRUE(contains(out, "right source"));
    XX_TEST_EXPECT_TRUE(contains(out, "center target node"));
}

void test_render_lr() {
    auto dg  = parseMermaidStateDiagram(R"(
stateDiagram-v2
    direction LR
    [*] --> A
    A --> B: go
    B --> [*]
)");
    auto out = renderToString(dg);
    XX_TEST_EXPECT_TRUE(contains(out, ">"));  // 横向箭头
    XX_TEST_EXPECT_TRUE(contains(out, "go")); // 边标签
    XX_TEST_EXPECT_TRUE(contains(out, "A"));
    XX_TEST_EXPECT_TRUE(contains(out, "B"));
}

void test_render_skip_edge_legend() {
    // 跨层边 (1 --> 3) 进入图例文本
    auto dg  = parseMermaidStateDiagram(R"(
stateDiagram-v2
    [*] --> A
    A --> B
    B --> C
    A --> C
    C --> [*]
)");
    auto out = renderToString(dg);
    // A 与 C 不同层 (A 层1, C 层2): A --> C 是跨层边 → 图例
    XX_TEST_EXPECT_TRUE(contains(out, "A --> C"));
}

void test_render_max_width_truncation() {
    auto dg  = parseMermaidStateDiagram(R"(
stateDiagram-v2
    [*] --> phase_1_very_long_node_name
    phase_1_very_long_node_name --> [*]
)");
    auto out = renderToString(dg, 20);
    // 标签被截断 (带 "…")
    XX_TEST_EXPECT_TRUE(contains(out, "…"));
}

void test_render_empty_diagram() {
    auto dg  = parseMermaidStateDiagram("stateDiagram-v2\n%% only comments\n");
    auto out = renderToString(dg);
    // 空图: 不渲染任何节点/箭头
    XX_TEST_EXPECT_FALSE(contains(out, "┌"));
    XX_TEST_EXPECT_FALSE(contains(out, "[*]"));
    XX_TEST_EXPECT_FALSE(contains(out, "v"));
}

void test_render_multi_line_label() {
    auto dg  = parseMermaidStateDiagram(R"(
stateDiagram-v2
    state "line one\nline two" as A
    A --> B
)");
    auto out = renderToString(dg);
    XX_TEST_EXPECT_TRUE(contains(out, "line one"));
    XX_TEST_EXPECT_TRUE(contains(out, "line two"));
}

void test_render_lr_fallback_to_tb_when_too_wide() {
    // direction LR 但总宽超限: 自动退回 TB (按层截断), 不崩溃且内容可见
    auto dg  = parseMermaidStateDiagram(R"(
stateDiagram-v2
    direction LR
    [*] --> A
    A --> B
    B --> C
    C --> D
    D --> [*]
    state "node alpha with long label" as A
    state "node beta with long label" as B
    state "node gamma with long label" as C
    state "node delta with long label" as D
)");
    auto out = renderToString(dg, 30);
    XX_TEST_EXPECT_TRUE(contains(out, "node alpha"));
    XX_TEST_EXPECT_TRUE(contains(out, "node beta"));
    XX_TEST_EXPECT_TRUE(contains(out, "node gamma"));
    XX_TEST_EXPECT_TRUE(contains(out, "node delta"));
    XX_TEST_EXPECT_TRUE(contains(out, "[*]"));
}

void test_render_self_loop_and_backward_in_legend() {
    // 自环进入图例文本 (不参与分层边带)
    auto dg  = parseMermaidStateDiagram(R"(
stateDiagram-v2
    [*] --> A
    A --> A
)");
    auto out = renderToString(dg);
    XX_TEST_EXPECT_TRUE(contains(out, "A --> A"));
    XX_TEST_EXPECT_TRUE(contains(out, "A"));

    // 跨层边进入图例文本
    auto dg2  = parseMermaidStateDiagram(R"(
stateDiagram-v2
    [*] --> A
    A --> B
    B --> C
    A --> C
)");
    auto out2 = renderToString(dg2);
    XX_TEST_EXPECT_TRUE(contains(out2, "A --> C"));
    XX_TEST_EXPECT_TRUE(contains(out2, "A"));
    XX_TEST_EXPECT_TRUE(contains(out2, "B"));
    XX_TEST_EXPECT_TRUE(contains(out2, "C"));
}

void test_render_cjk_label_width() {
    // 中文标签按 2 列宽计算, 盒子对齐/箭头位置正确
    auto dg  = parseMermaidStateDiagram(R"(
stateDiagram-v2
    [*] --> A
    A --> B: 下一步
    B --> [*]
    state "开始阶段" as A
)");
    auto out = renderToString(dg);
    XX_TEST_EXPECT_TRUE(contains(out, "开始阶段"));
    XX_TEST_EXPECT_TRUE(contains(out, "下一步"));
    XX_TEST_EXPECT_TRUE(contains(out, "v"));
}

} // namespace

/// 固定 Terminal::Size() 返回值 (测试确定性):
/// ftxui 的 Terminal::Size() 在 Windows 下查询 stdout 控制台窗口尺寸, 测试进程
/// 在用户终端/CI 中运行时窗口大小不确定, 会使依赖弹窗尺寸的渲染断言
/// (弹窗高度) 随窗口变化而不稳定。
/// 这里将 stdout 句柄重定向到 NUL (非控制台 → 尺寸查询失败 → 走 fallback),
/// 再 SetFallbackSize 固定为 120x50, 使本模块渲染断言与运行终端无关。
/// 析构时恢复 stdout 与 fallback 默认值 (80x24)。
class TerminalSizeFix {
public:

    TerminalSizeFix() {
        ftxui::Terminal::SetFallbackSize({120, 50});
#if defined(_WIN32)
        oldOut_ = GetStdHandle(STD_OUTPUT_HANDLE);
        nul_    = CreateFileW(
            L"NUL",
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr
        );
        if (nul_ != INVALID_HANDLE_VALUE) {
            SetStdHandle(STD_OUTPUT_HANDLE, nul_);
        }
#else
        // POSIX: 重定向 STDOUT_FILENO 到 /dev/null, 使 ioctl(TIOCGWINSZ) 失败
        oldOutFd_         = dup(STDOUT_FILENO);
        const int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            close(devnull);
        }
#endif
    }

    ~TerminalSizeFix() {
#if defined(_WIN32)
        if (oldOut_ != INVALID_HANDLE_VALUE) {
            SetStdHandle(STD_OUTPUT_HANDLE, oldOut_);
        }
        if (nul_ != INVALID_HANDLE_VALUE) {
            CloseHandle(nul_);
        }
#else
        if (oldOutFd_ >= 0) {
            dup2(oldOutFd_, STDOUT_FILENO);
            close(oldOutFd_);
        }
#endif
        // 恢复 ftxui 默认 fallback (其他测试不依赖, 保持环境干净)
        ftxui::Terminal::SetFallbackSize({80, 24});
    }

    TerminalSizeFix(const TerminalSizeFix&)            = delete;
    TerminalSizeFix& operator=(const TerminalSizeFix&) = delete;

private:

#if defined(_WIN32)
    HANDLE oldOut_ = INVALID_HANDLE_VALUE;
    HANDLE nul_    = INVALID_HANDLE_VALUE;
#else
    int oldOutFd_ = -1;
#endif
};

TestResult testMermaidState() {
    g_mermaid_state_passed = 0;
    g_mermaid_state_failed = 0;

    // 固定终端尺寸: 使 PlanDiagramOverlay 弹窗尺寸断言与运行窗口大小无关
    TerminalSizeFix termFix;

    // 解析
    test_parse_linear_chain();
    test_parse_state_declarations();
    test_parse_direction();
    test_parse_tolerant();
    test_parse_arrow_no_spaces();
    test_parse_empty();
    // 渲染
    test_render_linear_chain();
    test_render_fork();
    test_render_merge();
    test_render_lr();
    test_render_skip_edge_legend();
    test_render_max_width_truncation();
    test_render_empty_diagram();
    test_render_multi_line_label();
    test_render_lr_fallback_to_tb_when_too_wide();
    test_render_self_loop_and_backward_in_legend();
    test_render_cjk_label_width();

    return TestResult{g_mermaid_state_passed, g_mermaid_state_failed};
}

} // namespace test
} // namespace agentxx
