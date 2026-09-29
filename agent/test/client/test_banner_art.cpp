// 启动 banner 艺术字 ("AGENT++") 中 "++" 的变形动画测试
//
// 覆盖场景:
// - 帧序列结构: 每帧 6 行、各帧每行等宽 (居中的艺术字在动画中不左右跳动)、
//   "AGENT" 前缀在所有帧中保持不变、末帧与原机器人图案逐字符一致、
//   与末帧的差异随帧递减 (动画一定收敛到末帧)
// - 播放组件 BannerArtComponent: 首次渲染开始播放、按累计时长推进、到末帧后停止
//   且不再续约、未渲染前不推进、动画等级不足时直接展示末帧
// - 消息列表空状态接入: 首帧展示大字 "+", 不发任何事件也随动画推进为机器人图案
//   (banner 元素播放期间不可跨帧缓存), 播放结束后仍是机器人图案
#include "agentxx-test/client/test_banner_art.h"

#include "agentxx-client/io/tui/components/banner_art.h"
#include "agentxx-client/io/tui/components/message_list.h"
#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx-client/io/tui/framework/tui_settings.h"
#include "agentxx-client/io/tui/framework/tui_state.h"
#include "agentxx-client/io/tui/tui_theme.h"
#include "asio/io_context.hpp"
#include "ftxui/component/animation.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_banner_art_passed = 0;
int g_banner_art_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_banner_art_passed
#define XX_TEST_FAILED g_banner_art_failed

namespace agentxx {
namespace test {

using namespace agentxx::client;

namespace {

/// 原机器人图案 (改动画前 banner 硬编码的 6 行艺术字; 动画末帧必须与它一致)
constexpr std::array<std::string_view, 6> kRobotArt = {
    " █████╗  ██████╗ ███████╗███╗   ██╗████████╗      ╔══╗     ╔══╗",
    "██╔══██╗██╔════╝ ██╔════╝████╗  ██║╚══██╔══╝   ╔══╬══╬═════╬══╬══╗",
    "███████║██║  ███╗█████╗  ██╔██╗ ██║   ██║    ╔═╬  ║++║     ║++║  ╬═╗",
    "██╔══██║██║   ██║██╔══╝  ██║╚██╗██║   ██║    ╚═╬       \\_/       ╬═╝",
    "██║  ██║╚██████╔╝███████╗██║ ╚████║   ██║      ╚═══════   ═══════╝",
    "╚═╝  ╚═╝ ╚═════╝ ╚══════╝╚═╝  ╚═══╝   ╚═╝         ╚══╝     ╚══╝",
};

/// 每帧展示时长 (与实现默认值一致; 推进一帧用的时间步长)
constexpr float kBannerArtStepSeconds = 0.07F;

/// "AGENT" 前缀列数 (动画不改动这部分)
constexpr size_t kPrefixColumns = 44;

/// 一行按 UTF-8 码点切分 (艺术字只用方块/制表符等单列宽字符, 一个码点占一列)
std::vector<std::string> splitChars(std::string_view line) {
    std::vector<std::string> chars;
    size_t                   i = 0;
    while (i < line.size()) {
        const unsigned char lead = static_cast<unsigned char>(line[i]);
        size_t              len  = 1;
        if ((lead & 0xE0U) == 0xC0U) {
            len = 2;
        } else if ((lead & 0xF0U) == 0xE0U) {
            len = 3;
        } else if ((lead & 0xF8U) == 0xF0U) {
            len = 4;
        }
        len = std::min(len, line.size() - i);
        chars.emplace_back(line.substr(i, len));
        i += len;
    }
    return chars;
}

/// 多行文本按行拆分
std::vector<std::string> splitLines(std::string_view text) {
    std::vector<std::string> lines;
    size_t                   pos = 0;
    while (pos <= text.size()) {
        const size_t nl  = text.find('\n', pos);
        const size_t end = (nl == std::string_view::npos) ? text.size() : nl;
        lines.emplace_back(text.substr(pos, end - pos));
        if (nl == std::string_view::npos) {
            break;
        }
        pos = nl + 1;
    }
    return lines;
}

/// 艺术字网格: 行 × 列 (每格一个字符)
using ArtGrid = std::vector<std::vector<std::string>>;

/// 一帧 / 一段文本解析为字符网格
ArtGrid gridOf(std::string_view text) {
    ArtGrid grid;
    for (const auto& line : splitLines(text)) {
        grid.push_back(splitChars(line));
    }
    return grid;
}

/// 与末帧不同的字符数 (动画收敛性判定: 该值必须随帧递减, 末帧为 0)
size_t diffToFinal(const ArtGrid& grid, const ArtGrid& finalGrid) {
    size_t diff = 0;
    for (size_t y = 0; y < grid.size(); ++y) {
        const size_t cols = std::max(grid[y].size(), finalGrid[y].size());
        for (size_t x = 0; x < cols; ++x) {
            const std::string cur   = (x < grid[y].size()) ? grid[y][x] : std::string{" "};
            const std::string final = (x < finalGrid[y].size()) ? finalGrid[y][x]
                                                                : std::string{" "};
            if (cur != final) {
                ++diff;
            }
        }
    }
    return diff;
}

/// 去掉行尾空白 (比较艺术字时忽略补齐用的空格)
std::string_view trimRight(std::string_view s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

/// 文本各行去行尾空白 (便于按行整体比较)
std::vector<std::string> trimmedLines(std::string_view text) {
    std::vector<std::string> out;
    for (const auto& line : splitLines(text)) {
        out.emplace_back(trimRight(line));
    }
    return out;
}

/// 渲染一帧并取各行可见文字 (逐格拼接: 艺术字字符均为单列宽, 一列一个字符)
std::vector<std::string> renderRows(const ftxui::Element& el, int w, int h) {
    auto screen = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(w),
        ftxui::Dimension::Fixed(h)
    );
    ftxui::Render(screen, el);
    std::vector<std::string> rows;
    rows.reserve(static_cast<size_t>(screen.dimy()));
    for (int y = 0; y < screen.dimy(); ++y) {
        std::string row;
        for (int x = 0; x < screen.dimx(); ++x) {
            row += screen.PixelAt(x, y).character;
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

/// 推进动画 n 帧
/// - 与运行期一致: FTXUI 每次动画回调只给出上一帧到当前的时间差, 故逐帧推进
///   (实现内部对单次回调的推进量有上限保护, 一次给一大段时间不会一次跳到末帧)
void advance(ftxui::ComponentBase& comp, int frames) {
    for (int i = 0; i < frames; ++i) {
        ftxui::animation::Params params{ftxui::animation::Duration{kBannerArtStepSeconds}};
        comp.OnAnimation(params);
    }
}

/// 渲染组件当前的 6 行艺术字 (行尾空白去掉)
std::vector<std::string> artRowsOf(ftxui::ComponentBase& comp) {
    const auto rows = renderRows(comp.Render(), 80, 8);
    std::vector<std::string> out;
    for (int i = 0; i < 6 && i < static_cast<int>(rows.size()); ++i) {
        out.emplace_back(trimRight(rows[static_cast<size_t>(i)]));
    }
    return out;
}

/// 测试夹具: 空消息列表 (100x20 视口), 用于渲染空状态 banner
struct BannerFixture {
    asio::io_context io;
    TUISharedState   sharedState;
    TUITheme         theme = TUITheme::darkTheme();

    TUICtx                                ctx;
    std::shared_ptr<MessageListComponent> comp;

    static constexpr int kWidth  = 100;
    static constexpr int kHeight = 20;

    BannerFixture() {
        ctx.state      = &sharedState;
        ctx.frameState = sharedState.readSnapshot();
        ctx.postRedraw = [] {};
        ctx.theme      = &theme;
        ctx.sessionId  = "s";
        ctx.remoteUrl  = "";
        comp           = std::make_shared<MessageListComponent>(ctx);
    }

    /// 渲染一帧 (模拟 UI 循环: 取快照 -> 渲染), 返回各行可见文字
    std::vector<std::string> render() {
        ctx.frameState = sharedState.readSnapshot();
        return renderRows(comp->Render(), kWidth, kHeight);
    }

    /// 屏幕各行中是否存在指定片段 (艺术字整行可见, 片段不会跨行)
    static bool hasRowContaining(const std::vector<std::string>& rows, std::string_view part) {
        for (const auto& row : rows) {
            if (row.find(part) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    /// 屏幕上是否完整出现某帧艺术字 (逐行比较: 每行都要在某一行屏幕中可见)
    /// - 视口 100 列宽于艺术字 (68 列), 整行不会被裁切
    static bool hasArtFrame(const std::vector<std::string>& rows, std::string_view frame) {
        for (const auto& line : trimmedLines(frame)) {
            if (!hasRowContaining(rows, line)) {
                return false;
            }
        }
        return true;
    }
};

// ---------------------------------------------------------------------------
// 帧序列结构: 每帧 6 行等宽, "AGENT" 前缀不变, 逐帧收敛到原机器人图案
// ---------------------------------------------------------------------------

void testFrameSequence() {
    const auto& frames = bannerArtFrames();

    // 至少要有保持帧 + 若干变形帧, 且首末帧不同 (确实存在动画)
    XX_TEST_EXPECT_GE(frames.size(), size_t{8});
    XX_TEST_EXPECT_TRUE(frames.front() != frames.back());

    // 每帧 6 行; 所有帧、所有行的列数一致: FTXUI 多行文本按最宽行定块宽,
    // 各帧等宽才能保证居中的艺术字在动画过程中不左右跳动
    size_t width = 0;
    for (const auto& frame : frames) {
        const auto grid = gridOf(frame);
        XX_TEST_EXPECT_EQ(grid.size(), size_t{6});
        for (const auto& row : grid) {
            if (width == 0) {
                width = row.size();
            }
            XX_TEST_EXPECT_EQ(row.size(), width);
        }
    }
    XX_TEST_EXPECT_GE(width, kPrefixColumns + 1);

    // 首帧: "AGENT" + 两个大字 "+"; 此时还没有机器人图案
    const auto   first  = gridOf(frames.front());
    const size_t pluses = [&] {
        size_t n = 0;
        for (const auto& row : first) {
            for (const auto& ch : row) {
                if (ch == "█") {
                    ++n;
                }
            }
        }
        return n;
    }();
    XX_TEST_EXPECT_GE(pluses, size_t{40}); // 两个加号字形共 40+ 个实心格
    for (const auto& row : first) {
        const std::string rowText = [&] {
            std::string s;
            for (const auto& ch : row) {
                s += ch;
            }
            return s;
        }();
        XX_TEST_EXPECT_FALSE(rowText.find("╔══╗") != std::string::npos); // 机器人触角
        XX_TEST_EXPECT_FALSE(rowText.find("║++║") != std::string::npos); // 机器人眼睛
    }

    // "AGENT" 前缀在所有帧中一致 (动画只改右侧图案)
    for (size_t i = 1; i < frames.size(); ++i) {
        const auto grid = gridOf(frames[i]);
        for (size_t y = 0; y < grid.size() && y < first.size(); ++y) {
            for (size_t x = 0; x < kPrefixColumns && x < first[y].size(); ++x) {
                XX_TEST_EXPECT_EQ(grid[y][x], first[y][x]);
            }
        }
    }

    // 末帧 = 原机器人图案 (逐行比较, 忽略行尾补齐用的空格)
    const auto finalLines = trimmedLines(bannerArtFinalFrame());
    const auto finalFrame = bannerArtFrames().back();
    XX_TEST_EXPECT_EQ(finalFrame, bannerArtFinalFrame());
    XX_TEST_EXPECT_EQ(finalLines.size(), kRobotArt.size());
    for (size_t y = 0; y < kRobotArt.size() && y < finalLines.size(); ++y) {
        XX_TEST_EXPECT_EQ(finalLines[y], std::string{kRobotArt[y]});
    }

    // 收敛性: 与末帧的差异逐帧递减, 末帧为 0 (加号收回后机器人图案逐格补齐)
    const auto finalGrid = gridOf(bannerArtFinalFrame());
    size_t     prevDiff  = diffToFinal(gridOf(frames.front()), finalGrid);
    for (size_t i = 1; i < frames.size(); ++i) {
        const size_t diff = diffToFinal(gridOf(frames[i]), finalGrid);
        XX_TEST_EXPECT_GE(prevDiff, diff);
        prevDiff = diff;
    }
    XX_TEST_EXPECT_EQ(diffToFinal(finalGrid, finalGrid), size_t{0});

    // 保持帧: 开头几帧完全相同 (先让 "++" 停留一下再开始变形)
    XX_TEST_EXPECT_EQ(frames.front(), frames[1]);
}

// ---------------------------------------------------------------------------
// 播放组件: 首次渲染开始播放, 按累计时长推进, 到末帧停止
// ---------------------------------------------------------------------------

void testPlayer() {
    auto&      settings   = TUISettings::instance();
    const auto savedLevel = settings.animationLevel();

    // 动画等级满足: 首次渲染开始播放, 推进到末帧后停止
    {
        settings.setAnimationLevel(AnimationLevel::High);
        BannerArtComponent comp;

        XX_TEST_EXPECT_TRUE(comp.animating()); // 等待渲染开始播放
        XX_TEST_EXPECT_EQ(comp.frameIndex(), size_t{0});

        // 未渲染 (banner 还没出现) 时不推进
        advance(comp, 4);
        XX_TEST_EXPECT_EQ(comp.frameIndex(), size_t{0});

        // 首次渲染: 展示两个大字 "+", 并请求下一动画帧
        const auto firstRows = artRowsOf(comp);
        XX_TEST_EXPECT_EQ(firstRows, trimmedLines(bannerArtFrames().front()));

        // 推进一帧
        advance(comp, 1);
        XX_TEST_EXPECT_EQ(comp.frameIndex(), size_t{1});
        XX_TEST_EXPECT_TRUE(comp.animating());
        const auto secondRows = artRowsOf(comp);
        XX_TEST_EXPECT_EQ(secondRows, trimmedLines(bannerArtFrames()[1]));

        // 推进到末帧: 停在机器人图案, 不再播放 (继续推进也不再变化)
        advance(comp, 64);
        XX_TEST_EXPECT_FALSE(comp.animating());
        XX_TEST_EXPECT_EQ(comp.frameIndex(), bannerArtFrames().size() - 1);
        XX_TEST_EXPECT_EQ(artRowsOf(comp), trimmedLines(bannerArtFinalFrame()));
        advance(comp, 4);
        XX_TEST_EXPECT_EQ(comp.frameIndex(), bannerArtFrames().size() - 1);
        XX_TEST_EXPECT_EQ(artRowsOf(comp), trimmedLines(bannerArtFinalFrame()));
    }

    // 动画等级不足: 不做动画, 直接展示末帧, 且不需要动画回调
    {
        settings.setAnimationLevel(AnimationLevel::Disabled);
        BannerArtComponent comp;
        XX_TEST_EXPECT_FALSE(comp.animating());
        XX_TEST_EXPECT_EQ(artRowsOf(comp), trimmedLines(bannerArtFinalFrame()));
        XX_TEST_EXPECT_EQ(comp.frameIndex(), bannerArtFrames().size() - 1);
        advance(comp, 4);
        XX_TEST_EXPECT_EQ(comp.frameIndex(), bannerArtFrames().size() - 1);
    }

    settings.setAnimationLevel(savedLevel);
}

// ---------------------------------------------------------------------------
// 消息列表空状态接入: 无事件也会从大字 "+" 推进到机器人图案
// ---------------------------------------------------------------------------

void testEmptyListBanner() {
    auto&      settings   = TUISettings::instance();
    const auto savedLevel = settings.animationLevel();
    settings.setAnimationLevel(AnimationLevel::High);

    BannerFixture f;

    // 首帧: 空列表 banner 展示 "AGENT" + 两个大字 "+"
    const auto rows0 = f.render();
    XX_TEST_EXPECT_TRUE(BannerFixture::hasArtFrame(rows0, bannerArtFrames().front()));
    XX_TEST_EXPECT_FALSE(BannerFixture::hasRowContaining(rows0, "║++║"));

    // 不发任何事件也会随动画推进 (banner 元素播放期间不可跨帧缓存)
    advance(*f.comp, 8);
    const auto rows1 = f.render();
    XX_TEST_EXPECT_TRUE(rows1 != rows0);

    // 播放结束: 静止在机器人图案 (眼睛/嘴可见)
    advance(*f.comp, 64);
    const auto rows2 = f.render();
    XX_TEST_EXPECT_TRUE(BannerFixture::hasArtFrame(rows2, bannerArtFinalFrame()));
    XX_TEST_EXPECT_TRUE(BannerFixture::hasRowContaining(rows2, "║++║"));
    XX_TEST_EXPECT_TRUE(BannerFixture::hasRowContaining(rows2, "\\_/"));
    XX_TEST_EXPECT_FALSE(BannerFixture::hasArtFrame(rows2, bannerArtFrames().front()));

    // 结束后的再次渲染仍是机器人图案 (缓存可复用, 内容不再变化)
    const auto rows3 = f.render();
    XX_TEST_EXPECT_EQ(rows2, rows3);

    // 动画等级不足: 空列表直接展示机器人图案 (不播放大字 "+")
    settings.setAnimationLevel(AnimationLevel::Disabled);
    BannerFixture g;
    const auto    rowsDisabled = g.render();
    XX_TEST_EXPECT_TRUE(BannerFixture::hasArtFrame(rowsDisabled, bannerArtFinalFrame()));
    XX_TEST_EXPECT_TRUE(BannerFixture::hasRowContaining(rowsDisabled, "║++║"));

    settings.setAnimationLevel(savedLevel);
}

} // namespace

TestResult testBannerArt() {
    XX_TEST_EXPECT_TRUE(true);

    testFrameSequence();

    testPlayer();

    testEmptyListBanner();

    return TestResult{g_banner_art_passed, g_banner_art_failed};
}

} // namespace test
} // namespace agentxx
